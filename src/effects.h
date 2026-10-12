#pragma once
#include <3ds.h>
#include "model.h"
#include "anim.h"

// One keyframe in an EFX1 track (same layout as MotionKey).
typedef struct { float frame, value, tanIn, tanOut; } EfxKey;

// One animated channel within an effect (joint transform or material alpha).
typedef struct {
	s16   index;    // joint index or mat-obj index
	u8    channel;  // JOBJ_TRACKS key (1-10, 40) or MAT_TRACKS key (10)
	u8    _pad;
	u16   count;
	u16   _pad2;
	EfxKey* keys;
} EfxTrack;

// Animation for one effect model.
typedef struct {
	float    duration;
	u32      jointTrackCount;
	u32      matTrackCount;
	EfxTrack* tracks;   // [0..jointTrackCount-1] joint, [jointTrackCount..] mat
} EfxAnim;

// All animations for one category (e.g. gunfx/anim.efx).
typedef struct {
	u32      count;
	EfxAnim* anims;
	u8*      _buf;
} EfxSet;

// Live effect instance.
#define EFX_POOL 32
typedef struct {
	bool   active;
	s32    modelIndex;   // index into romfs/<cat>/<N>.crm
	EfxSet* set;
	float  frame;
	float  pos[3];
	Model* model;        // borrowed pointer, not owned
	Mat34* world;        // per-joint world matrices (jointCount entries)
} EfxInstance;

bool efxSetLoad(EfxSet* s, const char* path);
void efxSetFree(EfxSet* s);

// Spawn an effect instance. model must already be loaded (CRM4).
// Returns a slot index, or -1 if the pool is full.
int efxSpawn(EfxSet* set, int modelIndex, Model* model, const float pos[3]);

// End every active instance (before freeing a model or set they use).
void efxClearAll(void);

// Advance all active instances by one frame.
void efxStepAll(void);

// Draw all active instances.
void efxDrawAll(const C3D_Mtx* proj, const C3D_Mtx* view);
