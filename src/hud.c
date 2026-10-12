#include "hud.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "model.h"
#include "render.h"

// Sprite order, as tools/hud.py packs them.
enum { SPR_SHADOW, SPR_PLATE, SPR_BAR, SPR_DIGITS, SPR_TAGS, SPR_DOWN, SPR_REBIRTH, SPR_COUNT };

// The art is drawn for 640x480; the top screen is 400x240.
#define HUD_SCALE 0.7f
#define PLATE_W 102
#define PLATE_H 32
#define MAX_QUADS 64

typedef struct { u16 x, y, w, h; } HudRect;

static C3D_Tex tex;
static HudRect rects[SPR_COUNT];
static u16 atlasW, atlasH;
static ModelVertex* verts;
static int used;
static bool loaded;

bool hudLoad(char* err, size_t errSize)
{
	const char* path = "romfs:/hud/battle.bin";
	FILE* f = fopen(path, "rb");
	if (!f) {
		snprintf(err, errSize, "%s: missing (tools/hud.py)", path);
		return false;
	}
	char magic[4];
	u32 count = 0;
	bool ok = fread(magic, 4, 1, f) == 1 && memcmp(magic, "HUD1", 4) == 0 &&
		fread(&atlasW, 2, 1, f) == 1 && fread(&atlasH, 2, 1, f) == 1 &&
		fread(&count, 4, 1, f) == 1 && count == SPR_COUNT &&
		fread(rects, sizeof(HudRect), SPR_COUNT, f) == SPR_COUNT &&
		C3D_TexInit(&tex, atlasW, atlasH, GPU_RGBA8);
	if (ok) {
		ok = fread(tex.data, 1, tex.size, f) == tex.size;
		if (!ok)
			C3D_TexDelete(&tex);
	}
	fclose(f);
	if (!ok) {
		snprintf(err, errSize, "%s: bad file", path);
		return false;
	}
	C3D_TexFlush(&tex);
	C3D_TexSetFilter(&tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(&tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	verts = linearAlloc(MAX_QUADS * 6 * sizeof(ModelVertex));
	loaded = verts != NULL;
	if (!loaded) {
		C3D_TexDelete(&tex);
		snprintf(err, errSize, "HUD: no linear memory");
	}
	return loaded;
}

void hudFree(void)
{
	if (!loaded)
		return;
	C3D_TexDelete(&tex);
	linearFree(verts);
	loaded = false;
}

void hudBegin(void)
{
	used = 0;
}

// Part of sprite s (sx, sy, w, h in its own pixels) at screen (x, y), scaled.
static void quad(int s, float sx, float sy, float w, float h, float x, float y, u32 rgba)
{
	if (!loaded || used + 6 > MAX_QUADS * 6)
		return;
	const HudRect* r = &rects[s];
	// Stored top row first and sampled with t = 1 at the top.
	float u0 = (r->x + sx) / atlasW, u1 = (r->x + sx + w) / atlasW;
	float t0 = 1.0f - (r->y + sy) / atlasH, t1 = 1.0f - (r->y + sy + h) / atlasH;
	float x1 = x + w * HUD_SCALE, y1 = y + h * HUD_SCALE;
	const float c[4][4] = { { x, y, u0, t0 }, { x1, y, u1, t0 }, { x, y1, u0, t1 }, { x1, y1, u1, t1 } };
	static const int order[6] = { 0, 2, 1, 1, 2, 3 };
	for (int i = 0; i < 6; i++) {
		ModelVertex* v = &verts[used++];
		const float* p = c[order[i]];
		v->pos[0] = p[0]; v->pos[1] = p[1]; v->pos[2] = 0;
		v->uv[0] = p[2]; v->uv[1] = p[3];
		v->nrm[0] = v->nrm[1] = 0; v->nrm[2] = 1;
		memcpy(v->rgba, &rgba, 4);
	}
}

#define WHITE 0xffffffffu
#define RGBA(r, g, b, a) ((u32)(r) | (u32)(g) << 8 | (u32)(b) << 16 | (u32)(a) << 24)

// Slots on the plate, in its own pixels, read from the art (tools/hud.py).
#define TAG_X 2
#define TAG_Y 8
#define HP_RIGHT 95   // the slanted box's right edge at mid height
#define HP_Y 8
#define DIGIT_ADVANCE 11
#define BAR_X 17
#define BAR_Y 24

void hudPlate(float x, float y, int tag, u32 tint, float hp, float gauge, int word)
{
	// Kept on screen, with room above for the DOWN / REBIRTH word.
	float halfW = PLATE_W * HUD_SCALE / 2, top = (PLATE_H + 15) * HUD_SCALE;
	x = x < halfW ? halfW : x > 400 - halfW ? 400 - halfW : x;
	y = y < top ? top : y > 240 ? 240 : y;
	float ox = x - halfW, oy = y - PLATE_H * HUD_SCALE;
	const HudRect* r = &rects[SPR_PLATE];
	quad(SPR_SHADOW, 0, 0, r->w, r->h, ox, oy, RGBA(255, 255, 255, 200));
	quad(SPR_PLATE, 0, 0, r->w, r->h, ox, oy, WHITE);
	quad(SPR_TAGS, tag * 27, 0, 27, 14, ox + TAG_X * HUD_SCALE, oy + TAG_Y * HUD_SCALE, tint);

	// HP, right-aligned, rounded up so a robot with any HP left never shows 0.
	int n = (int)ceilf(hp);
	if (n < 0) n = 0;
	float dx = HP_RIGHT - 13;
	do {
		quad(SPR_DIGITS, (n % 10) * 13, 0, 13, 14, ox + dx * HUD_SCALE, oy + HP_Y * HUD_SCALE, WHITE);
		dx -= DIGIT_ADVANCE;
		n /= 10;
	} while (n > 0);

	// GUESS: the down gauge's colours; the game's HUD code is not traced.
	if (gauge > 1) gauge = 1;
	if (gauge > 0) {
		u32 c = gauge > 0.66f ? RGBA(255, 80, 40, 255) : RGBA(255, 220, 40, 255);
		quad(SPR_BAR, 0, 0, rects[SPR_BAR].w * gauge, rects[SPR_BAR].h, ox + BAR_X * HUD_SCALE,
			oy + BAR_Y * HUD_SCALE, c);
	}

	if (word != HUD_WORD_NONE) {
		int s = word == HUD_WORD_DOWN ? SPR_DOWN : SPR_REBIRTH;
		const HudRect* w = &rects[s];
		quad(s, 0, 0, w->w, w->h, x - w->w * HUD_SCALE / 2, oy - w->h * HUD_SCALE, WHITE);
	}
}

void hudEnd(void)
{
	if (!loaded || !used)
		return;
	GSPGPU_FlushDataCache(verts, used * sizeof(ModelVertex));
	drawScreenQuads(&tex, verts, used);
}

bool hudProject(const C3D_Mtx* view, float fovy, const float* world, float* x, float* y)
{
	// The same projection as the battle's Mtx_PerspTilt, untilted, so the
	// result is in the screen's own 400x240 orientation.
	C3D_Mtx proj, pv;
	Mtx_Persp(&proj, fovy, C3D_AspectRatioTop, 0.1f, 1500.0f, false);
	Mtx_Multiply(&pv, &proj, view);
	C3D_FVec c = Mtx_MultiplyFVec4(&pv, FVec4_New(world[0], world[1], world[2], 1.0f));
	if (c.w <= 0.01f)
		return false;
	*x = (c.x / c.w + 1.0f) * 200.0f;
	*y = (1.0f - c.y / c.w) * 120.0f;
	return true;
}
