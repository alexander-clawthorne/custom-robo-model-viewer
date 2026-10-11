#pragma once
#include <3ds.h>
#include <citro3d.h>
#include "collide.h"

// Gun, bomb and pod records as tools/stats.py cooks them into
// romfs/stats/parts.bin (little-endian, field for field). Distances are world
// units, speeds world units per frame, angles degrees unless noted.

// What a hit carries: the record the victim reads next frame (S+0x18a0).
typedef struct {
	u16 damage;
	s16 down;             // added to the down gauge; < 0 always knocks down
	s16 push, pushAngle, flinch, pushGravity;    // stagger: speed x100, frames, gravity %
	s16 launch, launchAngle, launchGravity;      // knockdown
	s16 pad;
} HitParams;

typedef struct {
	u16 frames;
	u8 trail, pad;        // trail: frames of path that still hit (and linger after impact)
	float speed, homeYaw, homePitch, radius;     // homing in radians per frame
	HitParams hit;
} GunPhase;

typedef struct {
	u8 windup, volleys, perVolley, fanAxis, interval, recovery, fanPhase, pad;
	s16 cone, fanStart, fanStep, pad2;           // 65536 = 360 degrees
	GunPhase phase[5];
} GunPattern;

typedef struct {
	char name[24];
	u8 cooldown, homeAhead, slots, pad;
	GunPattern pattern[2];                       // ground, air
} GunStats;

typedef struct {
	u8 bursts, gap;
	u16 grow, tail, pad;
	float r1, r2, height;
	HitParams hit;
} Blast;

typedef struct {
	u8 aim, reload, fuse, pad;
	u16 speed, pad2;                             // x1000
	s16 shot[4][4];                              // side, forward (x100), arc height, swing
	Blast blast;
} BombMode;

enum { BOMB_TOMAHAWK = 1, BOMB_DOUBLE_MINE = 2, BOMB_SMASH = 4, BOMB_TYPE_R = 8 };
typedef struct {
	char name[24];
	u8 count, flags, pad[2];
	u16 trigger, pad2;                           // x100
	BombMode mode[2];                            // ground, air
} BombStats;

enum { POD_NO_EARLY = 1, POD_LOW_TRIGGER = 2, POD_TYPE_R = 4 };
typedef struct {
	char name[24];
	u8 max, per, lockOn, reflect, gain[2], flags, pad;
	s16 spread;
	u16 rangeH, rangeV, life;                    // x100, frames
	s16 yaw[2][2];                               // [ground, air][last, first]
	u16 speed[2], hover[2], cone, lockRange;     // [free, locked] x1000, x100
	s16 pause, elev[2], launchSpeed[2], launchFrames[2], pad2;
	Blast blast;
} PodStats;

enum { HIT_GUN = 1, HIT_BOMB = 2, HIT_POD = 3 };

// The victim's hit record, filled by the other robot's weapons and read
// (then cleared) by the victim's next frame, as 0x8004b0a0 reads S+0x18a0.
typedef struct {
	bool any;
	int kind;
	int damage, down;     // summed over the frame's hits
	HitParams last;       // the reaction comes from the last hit
	float dir[3];         // horizontal push direction
} Hit;

// What the weapons need to know about a robot each frame, in world space.
typedef struct {
	float pos[3];         // feet
	float centre[3];      // hit sphere centre: midway between out[2] and out[11]
	float radius;         // body +0xac / 100
	float aim[3];         // out[1]: what homing bullets steer at
	float chest[3];       // out[2]: where pods start
	float hips[3];        // out[11]: bullets are checked for walls from here
	float gunAt[3], bombAt[3];   // the gun and bomb mounts
	float facing[3];      // unit, horizontal
	float stick[2];       // world x, z, length 0-1: steers the bomb cursor
	bool airborne;        // S+0x139 == 2
	int power[3], gunRecovery;   // body 0x4c-0x4f, percent
	bool hittable;        // false while invulnerable
} Fighter;

#define MAX_BULLETS 32
#define BULLET_TRAIL 62

typedef struct {
	u8 state, phase;      // 0 free, 1 flying, 2 ended (lingering trail)
	u16 frame;
	int age, linger, index;
	bool hit;
	const GunPattern* pat;
	float pos[3], dir[3];
	float trail[BULLET_TRAIL][3];   // ring of recent positions, newest at age
} Bullet;

typedef struct {
	bool active, hit;
	int age;
	float radius, pos[3];
} Burst;

typedef struct {
	bool active, typeR;
	int t;
	const Blast* e;
	float pos[3], vel[3], owner[3];
	Burst burst[4];
} Explosion;

typedef struct {
	u8 state;             // 0 free, 1 flying, 2 landed, 3 exploding
	int shot, t, flight, hop, age;
	bool hopped;
	const BombMode* mode;
	float pos[3], vel[3], origin[3], target[3];
	Explosion x;
} Bomb;

typedef struct {
	u8 state;             // 0 free, 1 flying, 2 exploding
	bool locked;
	int pause, launch, life;
	float pos[3], vel[3], heading[3];
	Explosion x;
} Pod;

typedef struct {
	const GunStats* gun;
	const BombStats* bomb;
	const PodStats* pod;
	// Gun (S+0x20c4..0x20ca): pattern state 0 idle / 1 ground / 2 air.
	int gunState, volleys, sinceVolley, gunTimer;
	Bullet bullets[MAX_BULLETS];
	// Bomb: the aiming cursor and the reload (L+0x18e).
	bool aiming;
	float cursor[3];
	int bombReload, throwMode;
	Bomb bombs[8];
	int podsOut;
	Pod pods[3];
	u32 rng;
	// Counters for the autotest and the bottom screen.
	int fired, thrown, launched, hits[4], blasts;
} Arsenal;

typedef struct {
	float centre[2], radius2;   // the circle outside which shots are dropped
} ArenaBounds;

void arsenalReset(Arsenal* a, const GunStats* gun, const BombStats* bomb, const PodStats* pod);

// Gun: pattern 0 ground, 1 air. Starts the volleys (action 3 at S+0xe2 == 1).
void gunStart(Arsenal* a, bool air);
bool gunBusy(const Arsenal* a);
// The shooter was hit: skip to the recovery; deleteShots also clears the
// bullets in flight (a knockdown by a gun does).
void gunInterrupt(Arsenal* a, bool deleteShots);

bool bombReady(const Arsenal* a);
void bombAim(Arsenal* a, const Fighter* me, const Fighter* foe, const Collision* col);
void bombThrow(Arsenal* a, const Fighter* me);
void bombCancel(Arsenal* a);

bool podReady(const Arsenal* a);
void podLaunch(Arsenal* a, const Fighter* me);

// One frame: move every shot, then test it against the other robot and
// write what hit into foeHit.
void arsenalStep(Arsenal* a, const Fighter* me, const Fighter* foe, Hit* foeHit, const Collision* col,
	const ArenaBounds* bounds);

int arsenalLive(const Arsenal* a);   // shots, bombs, pods and blasts in the air

void weaponDrawInit(void);
void weaponDrawFini(void);
void arsenalDraw(const Arsenal* a, const C3D_Mtx* projection, const C3D_Mtx* view);
