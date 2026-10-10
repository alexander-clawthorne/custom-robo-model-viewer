// Custom Robo 3DS model viewer: robots and arenas cooked from the user's disc.
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model.h"
#include "anim.h"
#include "vshader_shbin.h"

#define CLEAR_COLOR 0x202830FF
#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct {
	char* folder;
	char* label;
	char** names;
	int count;
	bool assembled;   // draw the body with a weapon part on each mount joint
} Catalog;

// Where each part root hangs off the body, from the robot draw code at
// 0x80039fac in main.dol: it writes the solved matrix out[k] straight into the
// part root's world matrix with JOBJ_USER_DEF_MTX | MTX_INDEPEND_PARENT |
// MTX_INDEPEND_SRT, so the root's own stored SRT is ignored, and scales the
// basis by PART_SCALE. Rig slot r is driven by out[r+1], so slot = k - 1.
#define PART_SCALE 0.1f
enum { PART_GUN, PART_BOMB, PART_POD, PART_LEG, PART_COUNT };
static const char* partFolder[PART_COUNT] = { "gun", "bomb", "pod", "leg" };
static const struct { u8 part, root, slot; } attach[] = {
	{ PART_GUN,  0, 8 },   // out[9]  arm2 elbow: the gun is the forearm
	{ PART_BOMB, 1, 4 },   // out[5]  arm1 elbow
	{ PART_POD,  0, 0 },   // out[1]  spine, one pod each side
	{ PART_POD,  1, 0 },
	{ PART_LEG,  5, 12 },  // out[13] leg1 knee
	{ PART_LEG,  3, 13 },  // out[14] leg1 foot
	{ PART_LEG,  4, 16 },  // out[17] leg2 knee
	{ PART_LEG,  1, 17 },  // out[18] leg2 foot
};
#define ATTACH_COUNT ((int)(sizeof attach / sizeof attach[0]))

#define MAX_CATALOGS 16
static Catalog catalogs[MAX_CATALOGS];
static int catalogCount;

static DVLB_s* vshader;
static shaderProgram_s program;
static int uProjection, uModelView, uLightVec, uLighting;

static void loadCatalog(Catalog* c)
{
	char path[64];
	snprintf(path, sizeof path, "romfs:/%s/index.txt", c->folder);
	FILE* f = fopen(path, "r");
	if (!f)
		return;
	char line[64];
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0])
			continue;
		c->names = realloc(c->names, (c->count + 1) * sizeof(char*));
		c->names[c->count++] = strdup(line);
	}
	fclose(f);
}

// romfs:/categories.txt: one "folder|label" line per category (tools/cook.py).
static void loadCategories(void)
{
	FILE* f = fopen("romfs:/categories.txt", "r");
	if (!f)
		return;
	char line[96];
	while (catalogCount < MAX_CATALOGS && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		char* bar = strchr(line, '|');
		if (!bar)
			continue;
		*bar = 0;
		char* mode = strchr(bar + 1, '|');
		if (mode)
			*mode++ = 0;
		Catalog* c = &catalogs[catalogCount];
		c->folder = strdup(line);
		c->label = strdup(bar + 1);
		c->assembled = mode && !strcmp(mode, "assembled");
		loadCatalog(c);
		if (c->count)
			catalogCount++;
	}
	fclose(f);
}

static Catalog* findCatalog(const char* folder)
{
	for (int k = 0; k < catalogCount; k++)
		if (!strcmp(catalogs[k].folder, folder))
			return &catalogs[k];
	return NULL;
}

static void sceneInit(void)
{
	vshader = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &vshader->DVLE[0]);
	C3D_BindProgram(&program);
	uProjection = shaderInstanceGetUniformLocation(program.vertexShader, "projection");
	uModelView  = shaderInstanceGetUniformLocation(program.vertexShader, "modelView");
	uLightVec   = shaderInstanceGetUniformLocation(program.vertexShader, "lightVec");
	uLighting   = shaderInstanceGetUniformLocation(program.vertexShader, "lighting");

	C3D_AttrInfo* attr = C3D_GetAttrInfo();
	AttrInfo_Init(attr);
	AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);         // position
	AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);         // texcoord
	AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 3);         // normal
	AttrInfo_AddLoader(attr, 3, GPU_UNSIGNED_BYTE, 4); // colour (0-255)
	C3D_CullFace(GPU_CULL_NONE);
}

// world: per-joint matrices for skinned (CRM2) models, NULL otherwise.
// world: per-joint matrices for skinned (CRM2) models, NULL otherwise.
// fixed: one matrix for the whole model, used for an attached weapon part.
// onlyRoot: draw just that CRM3 root, or -1 for every batch.
static void drawModelEx(const Model* m, const C3D_Mtx* projection, const C3D_Mtx* view,
	const Mat34* world, const C3D_Mtx* fixed, int onlyRoot)
{
	const C3D_Mtx* modelView = view;
	C3D_BufInfo* buf = C3D_GetBufInfo();
	BufInfo_Init(buf);
	BufInfo_Add(buf, m->vbo, sizeof(ModelVertex), 4, 0x3210);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uProjection, projection);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, modelView);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLightVec, 0.35f, 0.6f, 0.72f, 0.0f);

	for (u32 i = 0; i < m->batchCount; i++) {
		const ModelBatch* b = &m->batches[i];
		bool lit = b->flags & MODEL_LIT;
		if (onlyRoot >= 0 && b->root != onlyRoot)
			continue;
		if (fixed) {
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, fixed);
		} else if (world && b->joint >= 0 && (u32)b->joint < m->jointCount) {
			C3D_Mtx joint, mv;
			mat34ToC3D(&world[b->joint], &joint);
			Mtx_Multiply(&mv, view, &joint);
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, &mv);
		} else {
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, modelView);
		}
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uLighting, lit ? 1.0f : 0.0f, 0.45f, 0.65f, 0.0f);

		C3D_TexEnv* env = C3D_GetTexEnv(0);
		C3D_TexEnvInit(env);
		if (b->texture >= 0) {
			C3D_TexBind(0, &m->textures[b->texture]);
			C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
			C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
		} else {
			C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
			C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
		}

		if (b->flags & MODEL_BLEND)
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
				GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
		else
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
		C3D_AlphaTest(b->flags & MODEL_ALPHA_TEST, GPU_GREATER, 0x60);
		C3D_DepthTest(true, GPU_GREATER, (b->flags & MODEL_NO_ZWRITE) ? GPU_WRITE_COLOR : GPU_WRITE_ALL);
		C3D_DrawArrays(GPU_TRIANGLES, b->first, b->count);
	}
}

static void drawModel(const Model* m, const C3D_Mtx* projection, const C3D_Mtx* view, const Mat34* world)
{
	drawModelEx(m, projection, view, world, NULL, -1);
}

// A part sits at its rig slot's world matrix with the basis scaled by
// PART_SCALE -- weapon parts are modelled ten times robot size.
static void partMatrix(const Mat34* slot, C3D_Mtx* out)
{
	// Scale the basis, keep the origin. Goes through mat34ToC3D because a
	// C3D_FVec stores its components as w, z, y, x -- indexing .c[] by column
	// writes the matrix scrambled.
	Mat34 s = *slot;
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			s.m[r][c] *= PART_SCALE;
	mat34ToC3D(&s, out);
}

int main(void)
{
	gfxInitDefault();
	romfsInit();
	consoleInit(GFX_BOTTOM, NULL);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C3D_RenderTarget* top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
	sceneInit();
	loadCategories();
	// Robots play robot motions; story characters play the story set.
	static const char* setFile[2] = { "romfs:/motion/robot.mot", "romfs:/motion/story.mot" };
	static const char* setStart[2] = { "sf_normal_walk", "m_walk" };
	MotionSet sets[2];
	bool haveSet[2];
	int setMotion[2];
	for (int k = 0; k < 2; k++) {
		haveSet[k] = motionSetLoad(&sets[k], setFile[k]);
		setMotion[k] = haveSet[k] ? motionFind(&sets[k], setStart[k]) : -1;
		if (haveSet[k] && setMotion[k] < 0) setMotion[k] = 0;
	}
	int active = 0;
	AnimState animState;
	memset(&animState, 0, sizeof animState);
	bool mirror = false;
	float frame = 0.0f;
	Mat34* world = NULL;
	bool info = false, reframe = false;
	float camCentre[3] = { 0 }, camRadius = 1.0f;

	int mode = 0;
	int index[MAX_CATALOGS] = { 0 };
#ifdef AUTOTEST_CATEGORY
	// make AUTOTEST=1 AUTOCAT=collide: run only that category's models.
	// Two categories share the romfs/robo folder, so AUTOCAT=assembled selects
	// the composite one by its mode rather than by folder name.
	for (int k = 0; k < catalogCount; k++) {
		bool hit = !strcmp(AUTOTEST_CATEGORY, "assembled") ? catalogs[k].assembled
			: !strcmp(catalogs[k].folder, AUTOTEST_CATEGORY);
		if (hit) { mode = k; break; }
	}
#endif
	Model model;
	memset(&model, 0, sizeof model);
	Model parts[PART_COUNT];
	bool havePart[PART_COUNT] = { false };
	int partIndex[PART_COUNT] = { 0 };
	memset(parts, 0, sizeof parts);
	bool reload = true, loaded = false, spin = true;
	float yaw = 0.6f, pitch = 0.25f, zoom = 1.0f;
	char err[96] = "";

#ifdef AUTOTEST
	// Test build (make AUTOTEST=1): step through every 10th model of every
	// category, about 2 s each, so an emulator run can be checked without
	// input. AUTOCAT=<folder> runs one category only and AUTOSTEP=<n> changes
	// the stride, so checking one change does not cost a full sweep.
#ifndef AUTOTEST_STEP
#define AUTOTEST_STEP 10
#endif
	int autoFrames = 0;
	spin = false;
#endif
	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld();
#ifdef AUTOTEST
		if (++autoFrames == 120) {
			autoFrames = 0;
			for (int k = 0; k < 2; k++)  // a different motion on each model
				if (haveSet[k] && sets[k].count)
					setMotion[k] = (setMotion[k] + 23) % sets[k].count;
			frame = 0;
			if (index[mode] + AUTOTEST_STEP < catalogs[mode].count) index[mode] += AUTOTEST_STEP;
#ifndef AUTOTEST_CATEGORY
			else if (mode + 1 < catalogCount) mode++;
#endif
			else break;
			reload = true;
		}
#endif
		if (down & KEY_START)
			break;
		Catalog* cat = &catalogs[mode];
		if (catalogCount && cat->count) {
			if (down & KEY_R) { index[mode] = (index[mode] + 1) % cat->count; reload = true; }
			if (down & KEY_L) { index[mode] = (index[mode] + cat->count - 1) % cat->count; reload = true; }
			if (down & KEY_DRIGHT) { index[mode] = (index[mode] + 10) % cat->count; reload = true; }
			if (down & KEY_DLEFT) { index[mode] = (index[mode] + cat->count - 10 % cat->count) % cat->count; reload = true; }
		}
		if ((down & KEY_Y) && catalogCount > 1) { mode = (mode + 1) % catalogCount; reload = true; }
		if (down & KEY_B) spin = !spin;
		bool haveMotions = haveSet[active];
		MotionSet* motions = &sets[active];
		int* motionIdx = &setMotion[active];
		if (haveMotions && loaded && model.hasRig) {
			// X / SELECT: next / previous motion, blended in over 10 frames; one step
			// past the end is the bind pose. Touch: play mirrored.
			int before = *motionIdx;
			if (down & KEY_X) *motionIdx = *motionIdx + 1 >= motions->count ? -1 : *motionIdx + 1;
			if (down & KEY_SELECT) *motionIdx = *motionIdx < 0 ? motions->count - 1 : *motionIdx - 1;
			if (down & KEY_TOUCH) { mirror = !mirror; info = true; }
			if (*motionIdx != before) {
				frame = 0;
				info = true;
				animBlendStart(&animState, 10.0f);
			}
		}
		if (down & KEY_A) { yaw = 0.6f; pitch = 0.25f; zoom = 1.0f; }
		if (held & KEY_DUP) zoom *= 0.97f;
		if (held & KEY_DDOWN) zoom *= 1.03f;
		if (zoom < 0.05f) zoom = 0.05f;
		if (zoom > 8.0f) zoom = 8.0f;
		circlePosition pad;
		hidCircleRead(&pad);
		if (abs(pad.dx) > 15) { yaw += pad.dx * 0.0006f; spin = false; }
		if (abs(pad.dy) > 15) pitch -= pad.dy * 0.0004f;
		if (pitch > 1.5f) pitch = 1.5f;
		if (pitch < -1.5f) pitch = -1.5f;
		if (spin) yaw += 0.01f;

		if (reload) {
			reload = false;
			C3D_FrameSync(); // the GPU may still be reading the old buffers
			if (loaded) modelFree(&model);
			free(world);
			world = NULL;
			cat = &catalogs[mode];
			loaded = false;
			err[0] = 0;
			for (int k = 0; k < PART_COUNT; k++) {
				if (havePart[k])
					modelFree(&parts[k]);
				havePart[k] = false;
			}
			if (catalogCount && cat->count) {
				char path[96];
				snprintf(path, sizeof path, "romfs:/%s/%s", cat->folder, cat->names[index[mode]]);
				loaded = modelLoad(&model, path, err, sizeof err);
				if (loaded && model.skinned)
					world = malloc(model.jointCount * sizeof(Mat34));
				// Assembled: one part from each weapon folder, stepping with the
				// body so a sweep shows different combinations.
				if (loaded && cat->assembled) {
					for (int k = 0; k < PART_COUNT; k++) {
						Catalog* pc = findCatalog(partFolder[k]);
						if (!pc || !pc->count)
							continue;
						partIndex[k] = index[mode] % pc->count;
						snprintf(path, sizeof path, "romfs:/%s/%s", pc->folder, pc->names[partIndex[k]]);
						char perr[96];
						havePart[k] = modelLoad(&parts[k], path, perr, sizeof perr);
					}
				}
				active = strcmp(cat->folder, "snrchara") == 0 ? 1 : 0;
				animState.hasLast = false;
				frame = 0;
			} else {
				snprintf(err, sizeof err, "no cooked models in romfs");
			}
			info = true;
		}
		if (info) {
			info = false;
			reframe = true;
			cat = &catalogs[mode];
			consoleClear();
			printf("\x1b[1;1HCustom Robo Viewer\n\n");
			printf("%s %d / %d  (%s)\n\n", catalogCount ? cat->label : "-", index[mode] + 1, cat->count, cat->count ? cat->names[index[mode]] : "-");
			if (loaded)
				printf("%lu triangles, %lu textures\n%lu batches, radius %.2f\n", model.vertexCount / 3,
					model.textureCount, model.batchCount, model.radius);
			else
				printf("Load failed: %s\n", err);
			if (loaded && model.hasRig && haveSet[active])
				printf("\nMotion %d / %d%s\n%s\n", setMotion[active] + 1, sets[active].count,
					mirror ? "  (mirrored)" : "",
					setMotion[active] >= 0 ? sets[active].motions[setMotion[active]].name : "(bind pose)");
			printf("\nL / R        previous / next\n");
			printf("D-pad < >    jump 10\n");
			printf("Y            next category\n");
			printf("Circle pad   rotate\n");
			printf("D-pad ^ v    zoom\n");
			printf("B            auto-spin   A  reset\n");
			if (loaded && model.hasRig && haveSet[active]) {
				printf("X / SELECT   next / previous motion\n");
				printf("Touch        mirror motion\n");
			}
			printf("START        exit\n");
		}

		const Motion* playing = NULL;
		haveMotions = haveSet[active];
		motions = &sets[active];
		motionIdx = &setMotion[active];
		if (loaded && world) {
			if (haveMotions && model.hasRig && *motionIdx >= 0) {
				playing = &motions->motions[*motionIdx];
				frame += 1.0f;
				if (frame > playing->length)
					frame = 0.0f;
			}
			if (reframe && model.jointCount) {
				// Frame the whole motion, not the T-pose: motions put the hips about
				// a unit above the bind pose. Sample 8 poses and fit their joints.
				float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
				for (int k = 0; k < 8; k++) {
					float f = playing ? playing->length * k / 8.0f : 0.0f;
					animPose(&model, haveMotions ? motions : NULL, playing, f, mirror, NULL, world);
					for (u32 j = 0; j < model.jointCount; j++)
						for (int a = 0; a < 3; a++) {
							float v = world[j].m[a][3];
							if (v < lo[a]) lo[a] = v;
							if (v > hi[a]) hi[a] = v;
						}
				}
				float d2 = 0.0f;
				for (int a = 0; a < 3; a++) {
					camCentre[a] = (lo[a] + hi[a]) * 0.5f;
					d2 += (hi[a] - lo[a]) * (hi[a] - lo[a]);
				}
				// Joints sit inside the meshes; pad for heads, shoulders and weapons.
				camRadius = sqrtf(d2) * 0.5f * 1.05f;
				if (camRadius < model.radius) camRadius = model.radius;
				// Weapon parts stick out past the body: fit them too.
				for (int k = 0; k < ATTACH_COUNT; k++) {
					if (!havePart[attach[k].part] || !model.hasRig)
						continue;
					s16 joint = model.slots[attach[k].slot];
					if (joint < 0 || (u32)joint >= model.jointCount)
						continue;
					float d = 0.0f;
					for (int a = 0; a < 3; a++) {
						float v = world[joint].m[a][3] - camCentre[a];
						d += v * v;
					}
					float reach = sqrtf(d) + parts[attach[k].part].radius * PART_SCALE;
					if (reach > camRadius) camRadius = reach;
				}
			}
			animPose(&model, haveMotions ? motions : NULL, playing, frame, mirror, &animState, world);
		}
		if (reframe && loaded && !world) {
			memcpy(camCentre, model.centre, sizeof camCentre);
			camRadius = model.radius;
		}
		reframe = false;

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		if (loaded && world)
			animSkin(&model, world);  // after the frame sync: the GPU is done with the VBO
		C3D_RenderTargetClear(top, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
		C3D_FrameDrawOn(top);
		if (loaded) {
			float r = camRadius > 0.001f ? camRadius : 1.0f;
			float dist = r * 2.6f * zoom;
			C3D_Mtx projection, view;
			// Far plane covers arena skyboxes (about 500 units out).
			Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(50.0f), C3D_AspectRatioTop,
				r * 0.02f, dist + 1200.0f, false);
			Mtx_Identity(&view);
			Mtx_Translate(&view, 0.0f, 0.0f, -dist, true);
			Mtx_RotateX(&view, pitch, true);
			Mtx_RotateY(&view, yaw, true);
			Mtx_Translate(&view, -camCentre[0], -camCentre[1], -camCentre[2], true);
			drawModel(&model, &projection, &view, world);
			if (world && model.hasRig) {
				for (int k = 0; k < ATTACH_COUNT; k++) {
					const Model* pm = &parts[attach[k].part];
					if (!havePart[attach[k].part])
						continue;  // a root with no batches simply draws nothing
					s16 joint = model.slots[attach[k].slot];
					if (joint < 0 || (u32)joint >= model.jointCount)
						continue;
					C3D_Mtx part, mv;
					partMatrix(&world[joint], &part);
					Mtx_Multiply(&mv, &view, &part);
					drawModelEx(pm, &projection, &view, NULL, &mv, attach[k].root);
				}
			}
		}
		C3D_FrameEnd(0);
	}

	if (loaded) modelFree(&model);
	for (int k = 0; k < PART_COUNT; k++)
		if (havePart[k])
			modelFree(&parts[k]);
	free(world);
	for (int k = 0; k < 2; k++)
		if (haveSet[k]) motionSetFree(&sets[k]);
	shaderProgramFree(&program);
	DVLB_Free(vshader);
	C3D_Fini();
	romfsExit();
	gfxExit();
	return 0;
}
