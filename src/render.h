#pragma once
#include <3ds.h>
#include <citro3d.h>
#include "model.h"
#include "anim.h"

// Where each part root hangs off the body, from the robot draw code at
// 0x80039fac in main.dol: it writes the solved matrix out[k] straight into the
// part root's world matrix with JOBJ_USER_DEF_MTX | MTX_INDEPEND_PARENT |
// MTX_INDEPEND_SRT, so the root's own stored SRT is ignored, and scales the
// basis by PART_SCALE. Rig slot r is driven by out[r+1], so slot = k - 1.
#define PART_SCALE 0.1f
enum { PART_GUN, PART_BOMB, PART_POD, PART_LEG, PART_COUNT };
extern const char* const partFolder[PART_COUNT];
typedef struct { u8 part, root, slot; } PartAttach;
extern const PartAttach attach[];
extern const int attachCount;

void renderInit(void);
void renderFini(void);

// world: per-joint matrices for skinned (CRM2) models, NULL otherwise.
// fixed: one matrix for the whole model, used for an attached weapon part.
// onlyRoot: draw just that CRM3 root, or -1 for every batch.
void drawModelEx(const Model* m, const C3D_Mtx* projection, const C3D_Mtx* view,
	const Mat34* world, const C3D_Mtx* fixed, int onlyRoot);
void drawModel(const Model* m, const C3D_Mtx* projection, const C3D_Mtx* view, const Mat34* world);
// Only the batches bound to joints [jointLo, jointHi): one model of a CRM4 effect.
void drawModelJoints(const Model* m, const C3D_Mtx* projection, const C3D_Mtx* view,
	const Mat34* world, int jointLo, int jointHi);

// A part sits at its rig slot's world matrix with the basis scaled by PART_SCALE.
void partMatrix(const Mat34* slot, C3D_Mtx* out);

// A posed body plus whichever parts are loaded. view already includes the
// robot's placement in the world.
void drawRobot(const Model* body, const Mat34* world, const Model* parts, const bool* havePart,
	const C3D_Mtx* projection, const C3D_Mtx* view);
