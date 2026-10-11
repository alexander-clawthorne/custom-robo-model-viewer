#pragma once
#include <3ds.h>
#include "model.h"

// Affine 3x4 matrix: m[row][col], columns 0-2 are the local axes, column 3 the origin.
typedef struct { float m[3][4]; } Mat34;

typedef struct { float time, value, tanIn, tanOut; } MotionKey;
typedef struct { u16 count; MotionKey* keys; } MotionTrack;
typedef struct {
	char name[32];
	float length;
	MotionTrack tracks[78];   // 26 nodes x (x, y, z)
} Motion;

typedef struct {
	Motion* motions;
	int count;
	float defaults[21];       // the game's default skeleton (tools/cook_motion.py)
	MotionKey* keyPool;
} MotionSet;

bool motionSetLoad(MotionSet* set, const char* path);
void motionSetFree(MotionSet* set);
int motionFind(const MotionSet* set, const char* name);

// Solver input for one frame: the seven position targets (root, chest, head,
// two hands, two feet), the 16 node rotations and the root-node rotation.
typedef struct {
	float pos[7][3];
	Mat34 rot[16];
	Mat34 root;
} PoseInput;

// Blending between motions: keeps the last input and mixes the next ones
// into it over blendLen frames.
typedef struct {
	PoseInput last;
	bool hasLast;
	float blend, blendLen;
	// Arm aim, as the battle's aim stage 0x8003c4f0 does before re-solving
	// (0x80058ad0): while aiming, hand aimHand (0 = arm 1, the bomb's; 1 =
	// arm 2, the gun's) reaches for aimAt, given in the model's own space.
	bool aiming;
	int aimHand;
	float aimAt[3];
} AnimState;

void animBlendStart(AnimState* state, float frames);

// World matrix of every joint for one frame of motion m (NULL = bind pose).
// mirror plays it left-right swapped. state (may be NULL) carries blending
// from one call to the next. world must hold model->jointCount matrices.
void animPose(const Model* model, const MotionSet* set, const Motion* m, float frame, bool mirror,
	AnimState* state, Mat34* world);

// Re-skin the model's envelope batches into its VBO from the given joint matrices.
void animSkin(Model* model, const Mat34* world);

void mat34ToC3D(const Mat34* a, C3D_Mtx* out);
