#pragma once
#include <3ds.h>

// Arena collision cooked by tools/cook.py (romfs/collide/NNN.col): blocks of
// one-sided convex faces, each with its plane, normal pointing into the arena.
typedef struct {
	float v[4][3];
	float n[3], d;
	u32 kind;
} ColFace;

typedef struct {
	float centre[3], radius;
	u32 flags;            // 0x80: skipped by the game's sweeps
	u32 faceCount, nverts;
	ColFace* faces;
} ColBlock;

typedef struct {
	ColBlock* blocks;
	u32 blockCount;
	float lo[3], hi[3];   // bounds of the floor faces of block 0, the shell
} Collision;

bool collisionLoad(Collision* c, const char* path);
void collisionFree(Collision* c);

// Push a sphere at p out of every face it overlaps. Returns true if it
// touched anything; *ground is set if one of the pushes came from a face
// facing up (a floor or the top of an obstacle).
bool collideSphere(const Collision* c, float p[3], float radius, bool* ground, float* groundNormal);
