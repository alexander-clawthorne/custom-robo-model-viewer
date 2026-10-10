// Custom Robo 3DS model viewer: robots and arenas cooked from the user's disc.
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model.h"
#include "anim.h"
#include "render.h"
#include "battle.h"

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


#define MAX_CATALOGS 16
static Catalog catalogs[MAX_CATALOGS];
static int catalogCount;


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

int main(void)
{
	gfxInitDefault();
	romfsInit();
	consoleInit(GFX_BOTTOM, NULL);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C3D_RenderTarget* top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
	renderInit();
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
	// The app opens on the battle test; START there drops to the model viewer.
	BattleRoster roster = { 0 };
	{
		Catalog* c;
		if ((c = findCatalog("robo"))) roster.bodies = c->count;
		if ((c = findCatalog("gun"))) roster.guns = c->count;
		if ((c = findCatalog("bomb"))) roster.bombs = c->count;
		if ((c = findCatalog("pod"))) roster.pods = c->count;
		if ((c = findCatalog("leg"))) roster.legs = c->count;
		if ((c = findCatalog("arena"))) roster.arenas = c->count;
	}
	bool battle = roster.bodies && roster.arenas && haveSet[0];
#if defined(AUTOTEST) && defined(AUTOTEST_CATEGORY)
	battle = battle && !strcmp(AUTOTEST_CATEGORY, "battle");
#elif defined(AUTOTEST)
	battle = false;  // the full sweep is the model viewer's
#endif
	if (battle) {
		battleRun(top, &sets[0], &roster);
#if defined(AUTOTEST)
		goto done;
#endif
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
				for (int k = 0; k < attachCount; k++) {
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
			drawRobot(&model, world, parts, havePart, &projection, &view);
		}
		C3D_FrameEnd(0);
	}

	if (loaded) modelFree(&model);
	for (int k = 0; k < PART_COUNT; k++)
		if (havePart[k])
			modelFree(&parts[k]);
	free(world);
#if defined(AUTOTEST)
done:
#endif
	for (int k = 0; k < 2; k++)
		if (haveSet[k]) motionSetFree(&sets[k]);
	renderFini();
	C3D_Fini();
	romfsExit();
	gfxExit();
	return 0;
}
