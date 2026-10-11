// Battle test: run, jump, air dash and land in a real arena, against its own
// collision, with a camera that keeps both robots in view.
//
// Movement follows main.dol: the body and leg stat tables (tools/stats.py,
// romfs/stats/parts.bin) combined as 0x80006658 does, and the rules of the
// movement update 0x80049924, the air dash 0x80046f58 and the gravity and
// landing code in 0x8004b0a0. What is still a guess says so.
#include "battle.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "collide.h"
#include "model.h"
#include "render.h"

#define CLEAR_COLOR 0x202830FF
#define PI 3.14159265f
#define DEG (PI / 180.0f)

// The game keeps velocity in hundredths of a world unit per frame: its
// position update is pos += 0.01 * vel. Everything here is in world units.
#define VEL 0.01f

// romfs/stats/parts.bin (tools/stats.py --cook): per body a (min, normal,
// max) triple for 14 stats, per leg the percentage that picks along each.
typedef struct {
	char name[24];
	u16 hp;
	s16 tri[14][3];
	s16 steer;            // stat 14, the body's own
	u8 dashType, dashes;
	s8 dashPitch;         // degrees above horizontal
	u8 pad;
} BodyStats;
typedef struct {
	char name[24];
	u8 pct[14];
	u8 pad[2];
} LegStats;
typedef struct {
	BodyStats* body;
	LegStats* leg;
	u32 bodies, legs;
} PartStats;

static bool statsLoad(PartStats* s, const char* path)
{
	memset(s, 0, sizeof *s);
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	char magic[4];
	u32 n[2];
	bool ok = fread(magic, 4, 1, f) == 1 && !memcmp(magic, "STA1", 4) && fread(n, 4, 2, f) == 2;
	if (ok) {
		s->bodies = n[0];
		s->legs = n[1];
		s->body = malloc(n[0] * sizeof(BodyStats));
		s->leg = malloc(n[1] * sizeof(LegStats));
		ok = fread(s->body, sizeof(BodyStats), n[0], f) == n[0] && fread(s->leg, sizeof(LegStats), n[1], f) == n[1];
	}
	fclose(f);
	if (!ok) {
		free(s->body);
		free(s->leg);
		memset(s, 0, sizeof *s);
	}
	return ok;
}

static void statsFree(PartStats* s)
{
	free(s->body);
	free(s->leg);
	memset(s, 0, sizeof *s);
}

// 0x80006658: leg percentage p picks along the body's triple, 100 = normal.
static void statsCombine(const BodyStats* b, const LegStats* l, int out[15])
{
	for (int k = 0; k < 14; k++) {
		int lo = b->tri[k][0], mid = b->tri[k][1], hi = b->tri[k][2], p = l->pct[k];
		float v = p > 100 ? mid + (p - 100) * (hi - mid) / 100.0f : lo + p * (mid - lo) / 100.0f;
		out[k] = (int)v;
	}
	out[14] = b->steer;
}

enum { DASH_AIR, DASH_JUMP, DASH_SKY };
static const char* const dashTypeName[] = { "air dash", "air jump", "sky dash" };

typedef struct {
	int s[15];            // the combined stats, for the bottom screen
	float runSpeed;       // 0.1 * s0
	float runAccel;       // 0.001 * s1 per frame
	float turn;           // 0.1 * s2 degrees per frame
	float jumpLift;       // added each frame the button is held, up to s4 frames
	int jumpHold;
	float airSpeed;       // 0.1 * s5, the cap
	float airAccel;       // 0.003 * s6 per frame
	float gravity;        // 0.006 * s8
	int landLag;          // s7: frames of landing lag after a hard landing
	float impactSpeed;    // fall speed that counts as a hard landing
	int afterDash;        // s10: extra landing lag after an air dash
	// The body's dash type (byte 0x56) picks the action the press starts,
	// from the table at 0x801f8eb8 indexed by S+0x528: 7 air dash, 6 air
	// jump, 8 sky dash. All three first hang still for s9 frames.
	int dashType;         // DASH_*
	int windup;           // s9
	float airJump;        // type 1: sqrt(2 g s11), straight up
	int dashFrames;       // s12
	float dashSpeed;      // 0.1 * s13
	float dashSteer;      // 0.1 * s14 degrees per frame, sky dash only
	float dashPitch;
	int dashes;
	float radius;         // collision sphere, centred this high above the feet
} MoveParams;

static void moveParams(const BodyStats* b, const LegStats* l, MoveParams* p)
{
	int* s = p->s;
	statsCombine(b, l, s);
	p->runSpeed = 0.1f * s[0] * VEL;
	p->runAccel = 0.001f * s[1] * VEL;
	p->turn = 0.1f * s[2] * DEG;
	// Gravity, 0x8004b0a0: 0.01 * (0.3 * 2.0) * s8, the 2.0 being the float
	// at 0x801c6c04.
	float g = 0.006f * s[8];
	// Held for all s4 frames the jump peaks at s3 (both in velocity units):
	// the lift per frame solves height = a t^2/2 + (a t)^2/(2g) for a + g.
	float t = s[4] > 0 ? s[4] : 1;
	p->jumpLift = (g * t + sqrtf(g * g * t * t + 8.0f * g * s[3])) / (2.0f * t) * VEL;
	p->jumpHold = s[4];
	p->airSpeed = 0.1f * s[5] * VEL;
	p->airAccel = 0.003f * s[6] * VEL;
	p->gravity = g * VEL;
	p->landLag = s[7];
	// Landing impact is measured against the speed of a fall of 6 units (the
	// float at 0x801c6c08).
	p->impactSpeed = sqrtf(2.0f * p->gravity * 6.0f);
	p->afterDash = s[10];
	p->dashType = b->dashType <= DASH_SKY ? b->dashType : DASH_AIR;
	p->windup = s[9];
	p->airJump = sqrtf(2.0f * g * s[11]) * VEL;   // 0x80048b10
	p->dashFrames = s[12];
	p->dashSpeed = 0.1f * s[13] * VEL;
	p->dashSteer = 0.1f * s[14] * DEG;
	p->dashPitch = b->dashPitch * DEG;
	p->dashes = b->dashes;
	p->radius = 0.45f;    // GUESS: the sweep radius is not read yet
}

// Without parts.bin: Ray 01 on the standard leg.
static const MoveParams fallbackParams = {
	{ 80, 150, 200, 250, 8, 100, 100, 20, 100, 6, 15, 160, 10, 300, 20 },
	0.08f, 0.0015f, 20.0f * DEG, 0.02486f, 8, 0.14f, 0.003f, 0.006f, 20, 0.268f, 15,
	DASH_AIR, 6, 0.0139f, 10, 0.3f, 2.0f * DEG, 0.0f, 2, 0.45f,
};

enum { ST_GROUND, ST_AIR, ST_DASH, ST_LAND };
static const char* const stateName[] = { "ground", "air", "air dash", "landing" };

enum { MO_STAND, MO_RUN, MO_JUMP, MO_DASH, MO_LAND, MO_DOUBLE, MO_COUNT };
static const char* const motionName[MO_COUNT] = {
	"sf_normal_ta_kamae", "sf_normal_run", "sf_normal_jump", "sf_jump_dash", "sf_landing",
	"sf_double_jump",
};

// The bodies face -Z in their own model space: sf_normal_run leans the chest
// and head towards -Z. Facing +Z made every robot run backwards (seen on
// hardware), so turn the model half a circle onto the robot's heading.
#define MODEL_YAW PI

typedef struct {
	Model body;
	bool loaded;
	Mat34* world;
	float pos[3], vel[3];
	float yaw;            // facing: (sin yaw, 0, cos yaw)
	float speed;          // ground speed along the facing (S+0x128)
	int state, timer;
	int hold;             // frames the jump may still rise (S+0x13a)
	int dashesLeft;       // S+0x13b
	int windup;           // dash wind-up frames still to go (S+0xe2)
	bool sliding;         // skidding on the ground (S+0x139 == 1)
	int pendingLag;       // after-dash lag waiting for the landing (S+0xe4)
	bool ground;
	// What the last moves measured, for the bottom screen and the autotest.
	bool jumping;
	float takeoffY, peakY, lastJump, topSpeed;
	float wantYaw;        // where the stick last pointed, in the world
	int heldFrames, lastHeld, dashesThisJump, lastDashes, lastLag, lastJumpLag;
	float lastLandSpeed;  // horizontal speed at the last landing
	int motion;           // MO_*
	float frame;
	AnimState anim;
	MoveParams p;
} Robot;

typedef struct {
	const MotionSet* set;
	int id[MO_COUNT];     // index into set, -1 if missing
} Motions;

static float wrapAngle(float a)
{
	while (a > PI) a -= 2 * PI;
	while (a < -PI) a += 2 * PI;
	return a;
}

static float turnToward(float from, float to, float step)
{
	float d = wrapAngle(to - from);
	return wrapAngle(from + (fabsf(d) < step ? d : copysignf(step, d)));
}

static void setMotion(Robot* r, int motion)
{
	if (r->motion == motion)
		return;
	r->motion = motion;
	r->frame = 0;
	animBlendStart(&r->anim, motion == MO_DASH ? 3.0f : 6.0f);
}

static void robotFree(Robot* r)
{
	if (r->loaded)
		modelFree(&r->body);
	free(r->world);
	r->world = NULL;
	r->loaded = false;
}

// chara.BIN holds the 34 bodies in stat-table order, five colour variants
// each (the file sizes repeat in fives), so body b is model 5b.
static bool robotLoad(Robot* r, int body, char* err, size_t errSize)
{
	char path[64];
	snprintf(path, sizeof path, "romfs:/robo/%03d.crm", body * 5);
	r->loaded = modelLoad(&r->body, path, err, errSize);
	if (r->loaded && r->body.skinned)
		r->world = malloc(r->body.jointCount * sizeof(Mat34));
	return r->loaded && r->world;
}

static void robotSpawn(Robot* r, const float* at, float yaw)
{
	memcpy(r->pos, at, sizeof r->pos);
	memset(r->vel, 0, sizeof r->vel);
	r->yaw = yaw;
	r->speed = 0;
	r->state = ST_AIR;
	r->timer = r->hold = r->dashesLeft = r->pendingLag = r->windup = 0;
	r->ground = r->jumping = false;
	r->lastJump = r->topSpeed = 0;
	r->lastHeld = r->lastDashes = r->lastLag = r->lastJumpLag = 0;
	r->motion = -1;
	r->anim.hasLast = false;
	setMotion(r, MO_JUMP);
	r->frame = 1e6f;  // falling in: hold the end of the jump
}

// Ground velocity, 0x8004b0a0: the robot wants its facing times its run
// speed, times the floor's grip. Within a threshold of its current velocity
// it simply takes it; beyond, it slides, changing by a fixed step a frame --
// which is the skid after landing from a moving jump, and the ice of some
// floors. Values per surface kind from 0x801c6c1c: (threshold 0.02 x, step
// 0.002 x, grip 0.01 x); every ordinary floor is (100, 100, 100).
// GUESS: every face is treated as kind 0, the surface kind is not read.
#define SLIP_THRESHOLD (2.0f * VEL)
#define SLIDE_STEP (0.2f * VEL)

static void groundVelocity(Robot* r)
{
	float want[2] = { sinf(r->yaw) * r->speed, cosf(r->yaw) * r->speed };
	float d[2] = { want[0] - r->vel[0], want[1] - r->vel[2] };
	float len = sqrtf(d[0] * d[0] + d[1] * d[1]);
	r->sliding = len > SLIP_THRESHOLD;
	if (r->sliding) {
		r->vel[0] += d[0] / len * SLIDE_STEP;
		r->vel[2] += d[1] / len * SLIDE_STEP;
	} else {
		r->vel[0] = want[0];
		r->vel[2] = want[1];
	}
}

// One frame of movement. move is the wanted direction on the ground plane
// (length 0-1); press and held are the jump button.
static void robotStep(Robot* r, const Collision* col, const float move[2], bool press, bool held)
{
	const MoveParams* p = &r->p;
	float mag = sqrtf(move[0] * move[0] + move[1] * move[1]);
	if (mag > 1.0f) mag = 1.0f;
	// The frame the jump is pressed only switches the state: the game's
	// ground branch sets it and the air branch lifts from the next frame on.
	// Gravity and the landing test must wait too, or the robot -- still on
	// the floor -- lands again in the same frame and never leaves it.
	bool takeoff = false;
	if (mag > 0.01f)
		r->wantYaw = atan2f(move[0], move[1]);

	switch (r->state) {
	case ST_LAND:
		// Landing lag: no control -- the movement code's ground block is
		// skipped, so the run speed stays where it was (0 after a jump) and
		// the robot skids to it.
		if (--r->timer <= 0)
			r->state = ST_GROUND;
		groundVelocity(r);
		break;
	case ST_GROUND: {
		// Turn the facing toward the stick, then run along the facing at a
		// speed that eases toward the stick's share of the top speed.
		if (mag > 0.01f)
			r->yaw = turnToward(r->yaw, atan2f(move[0], move[1]), p->turn);
		float target = p->runSpeed * mag;
		float d = target - r->speed;
		r->speed += fabsf(d) < p->runAccel ? d : copysignf(p->runAccel, d);
		groundVelocity(r);
		if (r->speed > r->topSpeed)
			r->topSpeed = r->speed;
		if (press) {
			// The run carries into the jump; the lift comes from holding.
			// The input code at 0x80005928 keeps held buttons at S+0x23e and
			// new presses (new & ~old) at S+0x242: the jump starts on the
			// press and rises only while the button stays down.
			r->state = ST_AIR;
			r->hold = p->jumpHold;
			r->dashesLeft = p->dashes;
			r->speed = 0;
			r->vel[1] = 0;
			takeoff = r->jumping = true;
			r->takeoffY = r->peakY = r->pos[1];
			r->heldFrames = r->dashesThisJump = 0;
			setMotion(r, MO_JUMP);
		}
		break;
	}
	case ST_AIR:
		if (r->hold > 0) {
			if (held) {
				r->vel[1] += p->jumpLift;
				r->hold--;
				r->heldFrames++;
			} else {
				r->hold = 0;
			}
		}
		if (press && r->dashesLeft > 0) {
			// 0x80049924: the direction is the stick's, or the facing without
			// it, flattened; the velocity is zeroed and the wind-up starts.
			if (mag > 0.01f)
				r->yaw = atan2f(move[0], move[1]);
			r->dashesLeft--;
			r->dashesThisJump++;
			r->state = ST_DASH;
			r->windup = p->windup;
			r->timer = p->dashFrames;
			r->hold = 0;
			r->vel[0] = r->vel[1] = r->vel[2] = 0;
			setMotion(r, p->dashType == DASH_JUMP ? MO_DOUBLE : MO_DASH);
			break;
		}
		// Steer in the air, capped at the air speed.
		r->vel[0] += move[0] * p->airAccel;
		r->vel[2] += move[1] * p->airAccel;
		{
			float h = sqrtf(r->vel[0] * r->vel[0] + r->vel[2] * r->vel[2]);
			if (h > p->airSpeed) {
				r->vel[0] *= p->airSpeed / h;
				r->vel[2] *= p->airSpeed / h;
			}
		}
		break;
	case ST_DASH:
		break;
	}
	if (r->state == ST_DASH) {
		// No gravity in any of the three: only the movement action (0) applies
		// it, and the position update runs for every action.
		if (r->windup > 0) {
			r->windup--;                       // hang still
			r->vel[0] = r->vel[1] = r->vel[2] = 0;
		} else if (p->dashType == DASH_JUMP) {
			// 0x80048b10: straight up, fast enough to rise s11, no drift.
			r->vel[1] = p->airJump;
			r->state = ST_AIR;
			r->pendingLag = p->afterDash;
			setMotion(r, MO_JUMP);
		} else if (r->timer > 0) {
			// 0x80048708 (air dash) keeps its line; 0x80048050 (sky dash)
			// turns toward the stick by up to s14 / 10 degrees a frame. Both
			// fly at s13 / 10, pitched up by the body's dash angle.
			if (p->dashType == DASH_SKY && mag > 0.01f)
				r->yaw = turnToward(r->yaw, atan2f(move[0], move[1]), p->dashSteer);
			r->vel[0] = sinf(r->yaw) * cosf(p->dashPitch) * p->dashSpeed;
			r->vel[1] = sinf(p->dashPitch) * p->dashSpeed;
			r->vel[2] = cosf(r->yaw) * cosf(p->dashPitch) * p->dashSpeed;
			r->timer--;
		} else {
			// The dash ends: back to the movement action with the speed
			// dropped and the after-dash lag owed on landing.
			r->state = ST_AIR;
			r->pendingLag = p->afterDash;
			setMotion(r, MO_JUMP);
			r->frame = 1e6f;
		}
	} else if (!takeoff) {
		r->vel[1] -= p->gravity;   // on the ground too: it keeps the floor contact
	}

	// Sub-step so no step moves more than half the radius.
	float speed = sqrtf(r->vel[0] * r->vel[0] + r->vel[1] * r->vel[1] + r->vel[2] * r->vel[2]);
	int steps = 1 + (int)(speed / (p->radius * 0.5f));
	bool ground = false;
	float fall = -r->vel[1];
	for (int s = 0; s < steps; s++) {
		float c[3] = { r->pos[0] + r->vel[0] / steps, r->pos[1] + p->radius + r->vel[1] / steps,
			r->pos[2] + r->vel[2] / steps };
		bool g = false;
		float before[3] = { c[0], c[1], c[2] };
		if (collideSphere(col, c, p->radius, &g, NULL)) {
			// Take out the part of the velocity that went into the surface.
			float push[3] = { c[0] - before[0], c[1] - before[1], c[2] - before[2] };
			float len = sqrtf(push[0] * push[0] + push[1] * push[1] + push[2] * push[2]);
			if (len > 1e-6f) {
				float n[3] = { push[0] / len, push[1] / len, push[2] / len };
				float into = r->vel[0] * n[0] + r->vel[1] * n[1] + r->vel[2] * n[2];
				if (into < 0)
					for (int a = 0; a < 3; a++) r->vel[a] -= into * n[a];
			}
		}
		ground |= g;
		r->pos[0] = c[0];
		r->pos[1] = c[1] - p->radius;
		r->pos[2] = c[2];
	}
	bool wasAir = r->state == ST_AIR || r->state == ST_DASH;
	if (wasAir && r->pos[1] > r->peakY)
		r->peakY = r->pos[1];
	if (ground && wasAir && !takeoff && fall > 0.0f) {
		// Landing lag, 0x8004b0a0: s7 frames scaled by how hard the landing
		// was against a 6-unit fall, plus whatever an air dash left owing.
		float impact = fall / p->impactSpeed;
		if (impact > 1.0f) impact = 1.0f;
		int lag = (int)(p->landLag * impact + r->pendingLag);
		if (r->jumping) {
			r->lastJump = r->peakY - r->takeoffY;
			r->lastHeld = r->heldFrames;
			r->lastDashes = r->dashesThisJump;
			r->lastJumpLag = lag;   // the jump's own landing, not a later step off a ledge
			r->lastLandSpeed = sqrtf(r->vel[0] * r->vel[0] + r->vel[2] * r->vel[2]);
		}
		r->jumping = false;
		r->lastLag = lag;
		r->pendingLag = 0;
		r->hold = 0;
		r->vel[1] = 0;
		// The run speed is left alone: it is 0 after a jump or a dash (both
		// clear it) and still the run's after walking off a ledge. The
		// horizontal velocity carries over and skids (groundVelocity).
		if (lag > 0) {
			r->state = ST_LAND;
			r->timer = lag;
			setMotion(r, MO_LAND);
		} else {
			r->state = ST_GROUND;
		}
	} else if (!ground && !wasAir) {
		// Walked off a ledge: the dash count is whatever the last jump left.
		r->state = ST_AIR;
		r->hold = 0;
		setMotion(r, MO_JUMP);
		r->frame = 1e6f;
	}
	if (ground && r->vel[1] < 0)
		r->vel[1] = 0;
	r->ground = ground;

	if (r->state == ST_GROUND)
		setMotion(r, r->speed > p->runSpeed * 0.1f ? MO_RUN : MO_STAND);
}

static void robotAnimate(Robot* r, const Motions* mo)
{
	int id = r->motion >= 0 ? mo->id[r->motion] : -1;
	const Motion* m = id >= 0 ? &mo->set->motions[id] : NULL;
	if (m) {
		bool loop = r->motion == MO_STAND || r->motion == MO_RUN;
		float rate = 1.0f;
		if (r->motion == MO_RUN) {
			rate = r->speed / r->p.runSpeed;
			if (rate < 0.4f) rate = 0.4f;
		}
		r->frame += rate;
		if (r->frame > m->length)
			r->frame = loop ? fmodf(r->frame, m->length) : m->length;
	}
	animPose(&r->body, mo->set, m, r->frame, false, &r->anim, r->world);
}

static void robotDraw(Robot* r, const Model* parts, const bool* havePart,
	const C3D_Mtx* projection, const C3D_Mtx* view)
{
	C3D_Mtx place = *view;
	Mtx_Translate(&place, r->pos[0], r->pos[1], r->pos[2], true);
	Mtx_RotateY(&place, r->yaw + MODEL_YAW, true);
	drawRobot(&r->body, r->world, parts, havePart, projection, &place);
}

// ------------------------------------------------------------------ camera
// The battle camera, CAMMODE_MIDDLE: the camera-mode table at 0x801f7ff0
// holds an (init, update) pair per mode, and mode 2's update 0x80035bcc
// calls the fit at 0x8002b3c4 and the blend at 0x8002e04c. The fit keeps a
// fixed direction -- yaw 0, looking down -Z, pitched 30 degrees (r13-0x7f98)
// -- and moves the camera until every robot sits inside four frustum planes:
// 25.2 degrees either side (30 x 1.68 / 2), and from 20 below to 10 above
// the view axis. Those two vertical planes are a 30-degree view aimed 5
// degrees lower, hence CAM_PITCH 35. Each robot is pushed out by a margin
// first, 0.85 sideways and 0.546 vertically, both x 1.9 with two robots
// (0x801bcac8); its low point is 0.25 above the feet and its high point
// 1.15 x height / 1.31. The camera stays 5.7 (r13-0x7f90) behind the
// nearest robot.
#define CAM_FOVY 30.0f
#define CAM_PITCH (35.0f * DEG)
#define CAM_NEAREST 5.7f

static void battleCamera(const Robot* robots, int count, float eye[3], float at[3])
{
	// Horizontal half-angle from the 3DS screen, which is 1.667 wide, not
	// the game's 1.68, so nothing it fits is cropped.
	const float b = CAM_FOVY * 0.5f * DEG;
	const float a = atanf(tanf(b) * 400.0f / 240.0f);
	const float mh = 0.85f * 1.9f, mv = 0.546f * 1.9f;
	const float f[3] = { 0, -sinf(CAM_PITCH), -cosf(CAM_PITCH) };
	const float r[3] = { 1, 0, 0 };
	const float u[3] = { 0, cosf(CAM_PITCH), -sinf(CAM_PITCH) };
	float nR[3], nL[3], nT[3], nB[3];   // inward normals
	for (int k = 0; k < 3; k++) {
		nR[k] = -cosf(a) * r[k] + sinf(a) * f[k];
		nL[k] = cosf(a) * r[k] + sinf(a) * f[k];
		nT[k] = -cosf(b) * u[k] + sinf(b) * f[k];
		nB[k] = cosf(b) * u[k] + sinf(b) * f[k];
	}
	// Each plane's tightest robot: the eye's offset along the plane normal
	// can be at most p.n - margin.
	float kR = 1e9f, kL = 1e9f, kT = 1e9f, kB = 1e9f, nearest = 1e9f;
	for (int i = 0; i < count; i++) {
		const float* q = robots[i].pos;
		float low[3] = { q[0], q[1] + 0.25f, q[2] };
		float high[3] = { q[0], q[1] + 1.15f, q[2] };   // GUESS: height 1.31, S+0x17d4 not read
		float dR = low[0] * nR[0] + low[1] * nR[1] + low[2] * nR[2] - mh;
		float dL = low[0] * nL[0] + low[1] * nL[1] + low[2] * nL[2] - mh;
		float dB = low[0] * nB[0] + low[1] * nB[1] + low[2] * nB[2] - mv;
		float dT = high[0] * nT[0] + high[1] * nT[1] + high[2] * nT[2] - mv;
		float depth = low[0] * f[0] + low[1] * f[1] + low[2] * f[2];
		if (dR < kR) kR = dR;
		if (dL < kL) kL = dL;
		if (dB < kB) kB = dB;
		if (dT < kT) kT = dT;
		if (depth < nearest) nearest = depth;
	}
	// Each pair of planes fixes the eye across and along the view; the
	// further back of the two distances wins.
	float er = (kL - kR) / (2.0f * cosf(a));
	float eu = (kB - kT) / (2.0f * cosf(b));
	float ef = fminf((kR + kL) / (2.0f * sinf(a)), (kT + kB) / (2.0f * sinf(b)));
	if (nearest - ef < CAM_NEAREST)
		ef = nearest - CAM_NEAREST;
	for (int k = 0; k < 3; k++) {
		eye[k] = er * r[k] + eu * u[k] + ef * f[k];
		at[k] = eye[k] + f[k] * 10.0f;
	}
}

// ------------------------------------------------------------------ scripted input
#ifdef AUTOTEST
#ifndef AUTOTEST_STEP
#define AUTOTEST_STEP 10
#endif
// A fixed run for the emulator that does every movement once per arena, then
// checks what it measured against the stats (shown on the bottom screen):
// run to top speed, a full held jump, a short hop, a jump spending every air
// dash, a 180-degree turn, and a run into a wall. Moves are in camera space
// (x right, y forward); keys are pressed on the first frame of a cue and
// held to its end.
typedef struct { int from, to; float x, y; u32 keys; } Cue;
static const Cue script[] = {
	// The run is long: the slowest bodies need 53 frames to top speed, and a
	// robot that spawns on an obstacle loses some to the landing off it.
	{  60, 220,  0.0f,  1.0f, 0 },        // run forward to top speed
	{ 220, 255,  0.0f,  0.0f, KEY_B },    // full jump, held past any hold window
	{ 340, 342,  0.0f,  0.0f, KEY_B },    // hop: held 2 frames
	{ 390, 440,  0.0f,  1.0f, 0 },        // run forward...
	{ 440, 480,  0.0f, -1.0f, 0 },        // ...then turn round and run back
	{ 490, 560,  0.7f,  0.7f, 0 },        // run diagonally into a wall
	// Dashes last: a sky dash flies for up to 240 frames and cannot be cut.
	{ 580, 610,  0.0f,  0.0f, KEY_B },    // jump for the air dashes (pressed below)
	{ 610, 700,  1.0f,  0.0f, 0 },        // ...dashing right
};
#define SCRIPT_FRAMES 960
// B every 4 frames while dashing, so each dash starts as soon as the last ends.
#define DASH_FROM 612
#define DASH_TO 700
static void scripted(int t, float* x, float* y, u32* down, u32* held)
{
	*x = *y = 0;
	*down = *held = 0;
	for (u32 i = 0; i < sizeof script / sizeof script[0]; i++)
		if (t >= script[i].from && t < script[i].to) {
			*x = script[i].x;
			*y = script[i].y;
			*held |= script[i].keys;
			if (t == script[i].from) *down |= script[i].keys;
		}
	if (t >= DASH_FROM && t < DASH_TO && (t - DASH_FROM) % 4 == 0) {
		*down |= KEY_B;
		*held |= KEY_B;
	}
}

// Readings taken at fixed points of the script, then judged.
typedef struct {
	float topSpeed, full, hop, turn, aim, landSpeed;
	int fullHeld, lag, dashes, skid;
	bool done, pass;
	char line[7][40];
} AutoCheck;

static void autoSample(AutoCheck* c, const Robot* r, int t, float* yawMark)
{
	if (t == 220) c->topSpeed = r->topSpeed;
	if (t == 335) {
		c->full = r->lastJump; c->fullHeld = r->lastHeld; c->lag = r->lastJumpLag;
		c->landSpeed = r->lastLandSpeed;
	}
	// The full jump keeps the run's momentum (no stick in the air), so its
	// landing skids: count the frames.
	if (t > 220 && t < 335 && r->sliding && (r->state == ST_GROUND || r->state == ST_LAND)) c->skid++;
	if (t == 375) c->hop = r->lastJump;
	// Most dashes in one jump: once they are spent the script's further
	// presses land as new hops, which start the count again.
	if (t >= 580 && r->dashesThisJump > c->dashes) c->dashes = r->dashesThisJump;
	if (t == 439) *yawMark = r->yaw;
	// The stick is read relative to a camera that moves, so "reverse" is not
	// an exact half turn in the world: judge that the robot turned a long way
	// and ended up facing where the stick points.
	if (t == 470) {
		c->turn = fabsf(wrapAngle(r->yaw - *yawMark)) / DEG;
		c->aim = fabsf(wrapAngle(r->yaw - r->wantYaw)) / DEG;
	}
	if (t != SCRIPT_FRAMES - 1)
		return;
	const MoveParams* p = &r->p;
	float want = p->s[3] * VEL;     // the held jump's design height
	bool run = c->topSpeed >= p->runSpeed * 0.95f;
	bool full = fabsf(c->full - want) <= want * 0.15f;
	bool hop = c->hop > 0.0f && c->hop < c->full * 0.8f;
	bool dash = c->dashes == p->dashes;
	bool turn = c->turn > 90.0f && c->aim < 10.0f;
	bool land = c->lag > 0;
	// Each skid frame closes 0.002 of the gap; it ends inside 0.02 of the
	// target (0 during the lag, the stick is neutral after it).
	int skidWant = c->landSpeed > SLIP_THRESHOLD ? (int)ceilf((c->landSpeed - SLIP_THRESHOLD) / SLIDE_STEP) : 0;
	bool skid = abs(c->skid - skidWant) <= 1;
	snprintf(c->line[0], 40, "run  %s top %.3f/%.3f", run ? "ok  " : "FAIL", c->topSpeed, p->runSpeed);
	snprintf(c->line[1], 40, "jump %s %.2f of %.2f, %df", full ? "ok  " : "FAIL", c->full, want, c->fullHeld);
	snprintf(c->line[2], 40, "hop  %s %.2f", hop ? "ok  " : "FAIL", c->hop);
	snprintf(c->line[3], 40, "dash %s %d of %d", dash ? "ok  " : "FAIL", c->dashes, p->dashes);
	snprintf(c->line[4], 40, "turn %s %.0f deg, %.0f off stick", turn ? "ok  " : "FAIL", c->turn, c->aim);
	snprintf(c->line[5], 40, "land %s lag %d", land ? "ok  " : "FAIL", c->lag);
	snprintf(c->line[6], 40, "skid %s %d of %d frames", skid ? "ok  " : "FAIL", c->skid, skidWant);
	c->pass = run && full && hop && dash && turn && land && skid;
	c->done = true;
}
#endif

// ------------------------------------------------------------------ main loop
typedef struct {
	Model arena;
	bool arenaLoaded;
	Collision col;
	bool colLoaded;
	float spawn[2][3];
} Stage;

static void stageFree(Stage* s)
{
	if (s->arenaLoaded) modelFree(&s->arena);
	if (s->colLoaded) collisionFree(&s->col);
	s->arenaLoaded = s->colLoaded = false;
}

static bool stageLoad(Stage* s, int index, char* err, size_t errSize)
{
	char path[64];
	snprintf(path, sizeof path, "romfs:/arena/%03d.crm", index);
	s->arenaLoaded = modelLoad(&s->arena, path, err, errSize);
	snprintf(path, sizeof path, "romfs:/collide/%03d.col", index);
	s->colLoaded = collisionLoad(&s->col, path);
	if (!s->colLoaded) {
		snprintf(err, errSize, "no collision %03d", index);
		return false;
	}
	// Start on opposite sides of the floor, dropping in from above the highest
	// surface anyone can stand on: the shell's own floor can be a pit below
	// the stage (arena 24's is 10 units down), which spawned robots under it.
	const Collision* c = &s->col;
	bool floor = c->lo[0] < c->hi[0];
	float cx = floor ? (c->lo[0] + c->hi[0]) * 0.5f : c->blocks[0].centre[0];
	float cz = floor ? (c->lo[2] + c->hi[2]) * 0.5f : c->blocks[0].centre[2];
	float ext = floor ? (c->hi[2] - c->lo[2]) * 0.3f : 4.0f;
	float top = c->top > -1e8f ? c->top + 4.0f : c->blocks[0].centre[1];
	for (int k = 0; k < 2; k++) {
		s->spawn[k][0] = cx;
		s->spawn[k][1] = top;
		s->spawn[k][2] = cz + (k ? -ext : ext);
	}
	return true;
}

static int loadParts(Model* parts, bool* havePart, const BattleRoster* roster, const int* index)
{
	const int count[PART_COUNT] = { roster->guns, roster->bombs, roster->pods, roster->legs };
	int ok = 0;
	for (int k = 0; k < PART_COUNT; k++) {
		if (havePart[k]) modelFree(&parts[k]);
		havePart[k] = false;
		if (!count[k])
			continue;
		char path[64], err[96];
		snprintf(path, sizeof path, "romfs:/%s/%03d.crm", partFolder[k], index[k] % count[k]);
		havePart[k] = modelLoad(&parts[k], path, err, sizeof err);
		ok += havePart[k];
	}
	return ok;
}

void battleRun(C3D_RenderTarget* top, const MotionSet* set, const BattleRoster* roster)
{
	Motions mo = { set, { 0 } };
	for (int k = 0; k < MO_COUNT; k++)
		mo.id[k] = set ? motionFind(set, motionName[k]) : -1;

	Robot robots[2];
	memset(robots, 0, sizeof robots);
	Stage stage;
	memset(&stage, 0, sizeof stage);
	Model parts[PART_COUNT];
	bool havePart[PART_COUNT] = { false };
	memset(parts, 0, sizeof parts);
	int partIndex[PART_COUNT] = { 0, 0, 0, 0 };

	PartStats stats;
	bool haveStats = statsLoad(&stats, "romfs:/stats/parts.bin");
	int bodies = roster->bodies / 5;
	if (haveStats && (int)stats.bodies < bodies) bodies = stats.bodies;
	if (bodies < 1) bodies = 1;
	int arena = 0, body = 0;
	bool reloadStage = true, reloadRobots = true, info = true;
	char err[96] = "";
	float camEye[3] = { 0, 6, 10 }, camAt[3] = { 0, 1, 0 };
	int tick = 0;
	// Speed meter: frames drawn and frames simulated per real second. The
	// game logic is written per 60 Hz frame, so speed = simulated / 60.
	u64 meterStart = osGetTime();
	int meterFrames = 0, meterSteps = 0;
	float fps = 0.0f, speed = 0.0f;
#ifdef AUTOTEST
	int autoFrame = 0, autoRuns = 0, autoPasses = 0;
	AutoCheck check, lastCheck;
	memset(&check, 0, sizeof check);
	memset(&lastCheck, 0, sizeof lastCheck);
	float yawMark = 0;
	FILE* log = fopen("sdmc:/cr_autotest.log", "w");
	// make AUTOBODY= / AUTOLEG= / AUTOARENA=: one robot, or one arena only.
#ifdef AUTOTEST_BODY
	body = AUTOTEST_BODY % bodies;
#endif
#ifdef AUTOTEST_LEG
	partIndex[PART_LEG] = AUTOTEST_LEG;
#endif
#ifdef AUTOTEST_ARENA
	arena = AUTOTEST_ARENA % roster->arenas;
#endif
#endif

	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld();
		circlePosition pad;
		hidCircleRead(&pad);
		float sx = abs(pad.dx) > 15 ? pad.dx / 150.0f : 0.0f;
		float sy = abs(pad.dy) > 15 ? pad.dy / 150.0f : 0.0f;
#ifdef AUTOTEST
		if (autoFrame == SCRIPT_FRAMES) {
			autoFrame = 0;
			autoRuns++;
			autoPasses += check.pass;
			lastCheck = check;
			memset(&check, 0, sizeof check);
#ifdef AUTOTEST_ARENA
			bool last = true;
#else
			bool last = arena + AUTOTEST_STEP >= roster->arenas;
#endif
			if (last) {
				consoleClear();
				printf("Last arena (%d):\n", arena + 1);
				for (int k = 0; k < 7; k++) puts(lastCheck.line[k]);
				printf("AUTOTEST DONE: %d of %d arenas passed%s", autoPasses, autoRuns, "\n");
				if (log) {
					// The test script closes Citra once it sees this line.
					fprintf(log, "# DONE %d of %d arenas passed\n", autoPasses, autoRuns);
					fflush(log);
				}
				for (int k = 0; k < 120 && aptMainLoop(); k++)
					gspWaitForVBlank();
				break;
			}
			arena += AUTOTEST_STEP;
#ifndef AUTOTEST_BODY
			body = (body + 7) % bodies;
#endif
			for (int k = 0; k < PART_COUNT; k++) partIndex[k] += 3;
#ifdef AUTOTEST_LEG
			partIndex[PART_LEG] = AUTOTEST_LEG;
#endif
			reloadStage = reloadRobots = true;
		}
		int scriptT = autoFrame++;
		scripted(scriptT, &sx, &sy, &down, &held);
#endif
		if (down & KEY_START)
			break;
		if (down & KEY_DRIGHT) { arena = (arena + 1) % roster->arenas; reloadStage = true; }
		if (down & KEY_DLEFT) { arena = (arena + roster->arenas - 1) % roster->arenas; reloadStage = true; }
		if (down & KEY_DUP) { body = (body + 1) % bodies; reloadRobots = true; }
		if (down & KEY_DDOWN) { body = (body + bodies - 1) % bodies; reloadRobots = true; }
		if (down & KEY_SELECT) {
			for (int k = 0; k < PART_COUNT; k++) partIndex[k]++;
			reloadRobots = true;
		}
		float mag = sqrtf(sx * sx + sy * sy);
		if (mag > 1.0f) { sx /= mag; sy /= mag; }

		if (reloadStage || reloadRobots) {
			C3D_FrameSync();
			if (reloadStage) {
				stageFree(&stage);
				err[0] = 0;
				stageLoad(&stage, arena, err, sizeof err);
			}
			if (reloadRobots) {
				for (int k = 0; k < 2; k++) {
					robotFree(&robots[k]);
					// The two robots need their own copy of the body: envelope
					// batches are skinned into the model's own vertex buffer.
					int b = (body + k * 5) % bodies;
					robotLoad(&robots[k], b, err, sizeof err);
					// The leg models and the leg stat table share an order.
					if (haveStats)
						moveParams(&stats.body[b], &stats.leg[partIndex[PART_LEG] % stats.legs], &robots[k].p);
					else
						robots[k].p = fallbackParams;
				}
				loadParts(parts, havePart, roster, partIndex);
			}
			if (stage.colLoaded)
				for (int k = 0; k < 2; k++)
					robotSpawn(&robots[k], stage.spawn[k], k ? 0.0f : PI);
			reloadStage = reloadRobots = false;
			info = true;
		}

		bool ready = stage.colLoaded && robots[0].loaded && robots[1].loaded;
		Robot* me = &robots[0];
		Robot* foe = &robots[1];
		if (ready) {
			// The stick moves the robot relative to the camera.
			float fx = camAt[0] - camEye[0], fz = camAt[2] - camEye[2];
			float fl = sqrtf(fx * fx + fz * fz);
			if (fl < 1e-4f) { fx = 0; fz = -1; fl = 1; }
			fx /= fl; fz /= fl;
			float move[2] = { fx * sy - fz * sx, fz * sy + fx * sx };
			robotStep(me, &stage.col, move, down & KEY_B, held & KEY_B);
			float none[2] = { 0, 0 };
			// The other robot stands its ground facing this one.
			foe->yaw = atan2f(me->pos[0] - foe->pos[0], me->pos[2] - foe->pos[2]);
			robotStep(foe, &stage.col, none, false, false);
			for (int k = 0; k < 2; k++)
				if (robots[k].pos[1] < stage.col.lo[1] - 15.0f)
					robotSpawn(&robots[k], stage.spawn[k], robots[k].yaw);

			// The game's blend toward the fitted camera (0x8002e04c) uses a
			// factor of 1.0 (r2-0x7cd8): it snaps every frame.
			battleCamera(robots, 2, camEye, camAt);
			for (int k2 = 0; k2 < 2; k2++)
				robotAnimate(&robots[k2], &mo);
			meterSteps++;
#ifdef AUTOTEST
			autoSample(&check, me, scriptT, &yawMark);
			if (log && scriptT == SCRIPT_FRAMES - 1) {
				fprintf(log, "# arena %d body %d leg %d %s\n", arena, body, partIndex[PART_LEG],
					check.pass ? "PASS" : "FAIL");
				for (int k = 0; k < 7; k++)
					fprintf(log, "#   %s\n", check.line[k]);
			}
			if (log)
				fprintf(log, "%d %d %d %.4f %.3f %.3f %.3f %.4f %.4f %.4f %d %d %d\n", arena, scriptT, me->state, me->speed,
					me->pos[0], me->pos[1], me->pos[2], me->vel[0], me->vel[1], me->vel[2], me->dashesLeft, me->timer, me->sliding);
#endif
		}

		u64 now = osGetTime();
		if (now - meterStart >= 1000) {
			float secs = (now - meterStart) / 1000.0f;
			fps = meterFrames / secs;
			speed = meterSteps / secs / 60.0f;
			meterStart = now;
			meterFrames = meterSteps = 0;
		}
		if (info || (++tick % 15) == 0) {
			info = false;
			consoleClear();
			printf("\x1b[1;1HCustom Robo - battle test\n\n");
			printf("Arena %d / %d\n", arena + 1, roster->arenas);
			printf("Body %d %s   leg %d\n", body, haveStats ? stats.body[body].name : "",
				partIndex[PART_LEG] % (roster->legs ? roster->legs : 1));
			printf("parts g%d b%d p%d%s\n", partIndex[0], partIndex[1], partIndex[2], haveStats ? "" : "  (no stats)");
			if (err[0]) printf("%s\n", err);
			if (ready) {
				bool skid = me->sliding && (me->state == ST_GROUND || me->state == ST_LAND);
				printf("\nstate  %-10s dashes %d%s\n", stateName[me->state], me->dashesLeft, skid ? "  skid" : "");
				printf("pos    %6.2f %6.2f %6.2f\n", me->pos[0], me->pos[1], me->pos[2]);
				printf("vel    %6.3f %6.3f %6.3f\n", me->vel[0], me->vel[1], me->vel[2]);
				printf("motion %s\n", me->motion >= 0 ? motionName[me->motion] : "-");
				const int* st = me->p.s;
				printf("run %d/%d turn %d  jump %d/%d  g %d\n", st[0], st[1], st[2], st[3], st[4], st[8]);
				printf("air %d/%d  %s %dx  lag %d\n", st[5], st[6], dashTypeName[me->p.dashType], me->p.dashes, st[7]);
				if (me->p.dashType == DASH_JUMP)
					printf("  wind-up %d  height %d\n", st[9], st[11]);
				else
					printf("  wind-up %d  %d frames at %d\n", st[9], st[12], st[13]);
				printf("last jump %.2f held %df  dashes %d  lag %d\n", me->lastJump, me->lastHeld, me->lastDashes, me->lastLag);
			}
#ifdef AUTOTEST
			{
				const AutoCheck* c = check.done ? &check : &lastCheck;
				printf("\nAUTOTEST %s  (%d of %d arenas passed)\n",
					!c->done ? "running" : c->pass ? "PASS" : "FAIL",
					autoPasses + (check.done && check.pass), autoRuns + check.done);
				if (c->done)
					for (int k = 0; k < 7; k++) printf("%s\n", c->line[k]);
			}
#endif
			printf("\n%4.1f fps   game speed %3.0f%%\n", fps, speed * 100.0f);
			printf("CPU %5.2f ms   GPU %5.2f ms\n", C3D_GetProcessingTime(), C3D_GetDrawingTime());
#ifndef AUTOTEST  // the bottom screen holds 30 rows: the results need the room
			printf("\nCircle pad   run\n");
			printf("B            jump (hold: higher)\n");
			printf("B in the air air dash\n");
			printf("D-pad < >    arena\n");
			printf("D-pad ^ v    robot body\n");
			printf("SELECT       next parts and leg\n");
			printf("START        model viewer\n");
#endif
		}

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		if (ready)
			for (int k = 0; k < 2; k++)
				animSkin(&robots[k].body, robots[k].world);
		C3D_RenderTargetClear(top, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
		C3D_FrameDrawOn(top);
		if (ready) {
			C3D_Mtx projection, view;
			Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(CAM_FOVY), C3D_AspectRatioTop, 0.1f, 1500.0f, false);
			Mtx_LookAt(&view, FVec3_New(camEye[0], camEye[1], camEye[2]),
				FVec3_New(camAt[0], camAt[1], camAt[2]), FVec3_New(0, 1, 0), false);
			if (stage.arenaLoaded)
				drawModel(&stage.arena, &projection, &view, NULL);
			for (int k = 0; k < 2; k++)
				robotDraw(&robots[k], parts, havePart, &projection, &view);
		}
		C3D_FrameEnd(0);
		meterFrames++;
	}

	C3D_FrameSync();
#ifdef AUTOTEST
	if (log)
		fclose(log);
#endif
	for (int k = 0; k < 2; k++)
		robotFree(&robots[k]);
	for (int k = 0; k < PART_COUNT; k++)
		if (havePart[k]) modelFree(&parts[k]);
	stageFree(&stage);
	statsFree(&stats);
	consoleClear();
}
