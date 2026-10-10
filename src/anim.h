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

// World matrix of every joint for one frame of motion m (NULL = bind pose).
// world must hold model->jointCount matrices.
void animPose(const Model* model, const MotionSet* set, const Motion* m, float frame, Mat34* world);

// Re-skin the model's envelope batches into its VBO from the given joint matrices.
void animSkin(Model* model, const Mat34* world);

void mat34ToC3D(const Mat34* a, C3D_Mtx* out);
