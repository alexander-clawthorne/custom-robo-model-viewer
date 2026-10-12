#pragma once
#include <3ds.h>
#include <citro3d.h>

// The nameplate over each robot in battle, drawn from the game's own art
// (sprite/us_battle.tpl, cooked by tools/hud.py into romfs/hud/battle.bin).
enum { HUD_TAG_1P, HUD_TAG_2P, HUD_TAG_3P, HUD_TAG_4P, HUD_TAG_CPU };
enum { HUD_WORD_NONE, HUD_WORD_DOWN, HUD_WORD_REBIRTH };

bool hudLoad(char* err, size_t errSize);
void hudFree(void);

// Between hudBegin and hudEnd: one plate per call, its bottom centre at
// screen pixel (x, y) (400x240, y down). gauge is the down gauge, 0..1.
void hudBegin(void);
void hudPlate(float x, float y, int tag, u32 tint, float hp, float gauge, int word);
void hudEnd(void);

// Top-screen pixel of a world point under the battle camera; false when it is
// behind the camera. fovy in radians, as given to Mtx_PerspTilt.
bool hudProject(const C3D_Mtx* view, float fovy, const float* world, float* x, float* y);
