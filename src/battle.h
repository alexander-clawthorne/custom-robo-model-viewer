#pragma once
#include <3ds.h>
#include <citro3d.h>
#include "anim.h"

// How many cooked files each folder holds (romfs/<folder>/NNN.crm).
typedef struct {
	int bodies, guns, bombs, pods, legs, arenas;
} BattleRoster;

// Battle test: one robot on the pad, a second standing in the arena, the
// arena's own collision and a following camera. Returns when START is
// pressed (or, in an AUTOTEST build, when the scripted run ends).
void battleRun(C3D_RenderTarget* top, const MotionSet* motions, const BattleRoster* roster);
