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

// One ModelSet of an effect archive: its joints in the CRM4 list.
typedef struct { u16 first, count; float duration; } EfxPart;

// Animation for one effect model.
typedef struct {
	float    duration;
	u32      jointTrackCount;
	u32      matTrackCount;
	EfxTrack* tracks;   // [0..jointTrackCount-1] joint, [jointTrackCount..] mat
	u32      partCount;
	EfxPart* parts;     // one per ModelSet (bullet, trail, flash...)
} EfxAnim;

// All animations for one category (e.g. gunfx/anim.efx).
typedef struct {
	u32      count;
	EfxAnim* anims;
	u8*      _buf;
} EfxSet;

// Live effect instance.
#define EFX_POOL 96
typedef struct {
	bool   active;
	bool   loop;         // wrap at the end instead of expiring
	u16    gen;          // bumped on every spawn, so stale handles miss
	s32    modelIndex;   // index into romfs/<cat>/<N>.crm
	s32    part;         // ModelSet to play, -1 = all of them
	EfxSet* set;
	float  frame;
	float  rate;         // animation frames per game frame (1 at spawn)
	Mat34  base;         // effect space -> world (position, facing, scale)
	Model* model;        // borrowed pointer, not owned
	Mat34* world;        // per-joint world matrices (jointCount entries)
} EfxInstance;

bool efxSetLoad(EfxSet* s, const char* path);
void efxSetFree(EfxSet* s);

// Spawn an effect instance. model must already be loaded (CRM4); part is
// the ModelSet to play (-1 = all); base places it in the world.
// Returns a handle for efxMove / efxKill, or -1 if the pool is full.
int efxSpawn(EfxSet* set, int modelIndex, Model* model, int part, const Mat34* base, bool loop);

// Re-place a live instance (a bullet's effect follows the bullet).
// false if the handle has ended.
bool efxMove(int handle, const Mat34* base);

// Animation frames advanced per game frame.
void efxSetRate(int handle, float rate);

// End one instance; a handle that already ended is ignored.
void efxKill(int handle);

// End every active instance (before freeing a model or set they use).
void efxClearAll(void);

// Advance all active instances by one frame.
void efxStepAll(void);

// Draw all active instances.
void efxDrawAll(const C3D_Mtx* proj, const C3D_Mtx* view);
