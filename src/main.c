// Custom Robo 3DS model viewer: robots and arenas cooked from the user's disc.
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model.h"
#include "vshader_shbin.h"

#define CLEAR_COLOR 0x202830FF
#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct {
	const char* folder;
	const char* label;
	char** names;
	int count;
} Catalog;

static Catalog catalogs[2] = { { "robo", "Robot", NULL, 0 }, { "arena", "Arena", NULL, 0 } };

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

static void drawModel(const Model* m, const C3D_Mtx* projection, const C3D_Mtx* modelView)
{
	C3D_BufInfo* buf = C3D_GetBufInfo();
	BufInfo_Init(buf);
	BufInfo_Add(buf, m->vbo, sizeof(ModelVertex), 4, 0x3210);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uProjection, projection);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, modelView);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLightVec, 0.35f, 0.6f, 0.72f, 0.0f);

	for (u32 i = 0; i < m->batchCount; i++) {
		const ModelBatch* b = &m->batches[i];
		bool lit = b->flags & MODEL_LIT;
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

int main(void)
{
	gfxInitDefault();
	romfsInit();
	consoleInit(GFX_BOTTOM, NULL);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C3D_RenderTarget* top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
	sceneInit();
	loadCatalog(&catalogs[0]);
	loadCatalog(&catalogs[1]);

	int mode = catalogs[0].count ? 0 : 1;
	int index[2] = { 0, 0 };
	Model model;
	memset(&model, 0, sizeof model);
	bool reload = true, loaded = false, spin = true;
	float yaw = 0.6f, pitch = 0.25f, zoom = 1.0f;
	char err[96] = "";

#ifdef AUTOTEST
	// Test build (make AUTOTEST=1): step through every 10th robot, then every
	// arena, about 2 s each, so an emulator run can be checked without input.
	int autoFrames = 0;
	spin = false;
#endif
	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld();
#ifdef AUTOTEST
		if (++autoFrames == 120) {
			autoFrames = 0;
			if (mode == 0 && index[0] + 10 < catalogs[0].count) index[0] += 10;
			else if (mode == 0) mode = 1;
			else if (index[1] + 1 < catalogs[1].count) index[1]++;
			else break;
			reload = true;
		}
#endif
		if (down & KEY_START)
			break;
		Catalog* cat = &catalogs[mode];
		if (cat->count) {
			if (down & KEY_R) { index[mode] = (index[mode] + 1) % cat->count; reload = true; }
			if (down & KEY_L) { index[mode] = (index[mode] + cat->count - 1) % cat->count; reload = true; }
			if (down & KEY_DRIGHT) { index[mode] = (index[mode] + 10) % cat->count; reload = true; }
			if (down & KEY_DLEFT) { index[mode] = (index[mode] + cat->count - 10 % cat->count) % cat->count; reload = true; }
		}
		if ((down & KEY_Y) && catalogs[!mode].count) { mode = !mode; reload = true; }
		if (down & KEY_B) spin = !spin;
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
			cat = &catalogs[mode];
			loaded = false;
			err[0] = 0;
			if (cat->count) {
				char path[96];
				snprintf(path, sizeof path, "romfs:/%s/%s", cat->folder, cat->names[index[mode]]);
				loaded = modelLoad(&model, path, err, sizeof err);
			} else {
				snprintf(err, sizeof err, "no cooked models in romfs");
			}
			consoleClear();
			printf("\x1b[1;1HCustom Robo Viewer\n\n");
			printf("%s %d / %d  (%s)\n\n", cat->label, index[mode] + 1, cat->count, cat->count ? cat->names[index[mode]] : "-");
			if (loaded)
				printf("%lu triangles, %lu textures\n%lu batches, radius %.2f\n", model.vertexCount / 3,
					model.textureCount, model.batchCount, model.radius);
			else
				printf("Load failed: %s\n", err);
			printf("\n\nL / R        previous / next\n");
			printf("D-pad < >    jump 10\n");
			printf("Y            robots / arenas\n");
			printf("Circle pad   rotate\n");
			printf("D-pad ^ v    zoom\n");
			printf("B            auto-spin   A  reset\n");
			printf("START        exit\n");
		}

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C3D_RenderTargetClear(top, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
		C3D_FrameDrawOn(top);
		if (loaded) {
			float r = model.radius > 0.001f ? model.radius : 1.0f;
			float dist = r * 2.6f * zoom;
			C3D_Mtx projection, view;
			// Far plane covers arena skyboxes (about 500 units out).
			Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(50.0f), C3D_AspectRatioTop,
				r * 0.02f, dist + 1200.0f, false);
			Mtx_Identity(&view);
			Mtx_Translate(&view, 0.0f, 0.0f, -dist, true);
			Mtx_RotateX(&view, pitch, true);
			Mtx_RotateY(&view, yaw, true);
			Mtx_Translate(&view, -model.centre[0], -model.centre[1], -model.centre[2], true);
			drawModel(&model, &projection, &view);
		}
		C3D_FrameEnd(0);
	}

	if (loaded) modelFree(&model);
	shaderProgramFree(&program);
	DVLB_Free(vshader);
	C3D_Fini();
	romfsExit();
	gfxExit();
	return 0;
}
