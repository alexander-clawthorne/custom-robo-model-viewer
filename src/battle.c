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
	int dashFrames;       // s12
	float dashSpeed;      // 0.1 * s13
	float dashSteer;      // 0.1 * s14 degrees per frame
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
	10, 0.3f, 2.0f * DEG, 0.0f, 2, 0.45f,
};

enum { ST_GROUND, ST_AIR, ST_DASH, ST_LAND };
static const char* const stateName[] = { "ground", "air", "air dash", "landing" };

enum { MO_STAND, MO_RUN, MO_JUMP, MO_DASH, MO_LAND, MO_COUNT };
static const char* const motionName[MO_COUNT] = {
	"sf_normal_ta_kamae", "sf_normal_run", "sf_normal_jump", "sf_jump_dash", "sf_landing",
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
	int pendingLag;       // after-dash lag waiting for the landing (S+0xe4)
	bool ground;
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
	r->timer = r->hold = r->dashesLeft = r->pendingLag = 0;
	r->ground = false;
	r->motion = -1;
	r->anim.hasLast = false;
	setMotion(r, MO_JUMP);
	r->frame = 1e6f;  // falling in: hold the end of the jump
}

// One frame of movement. move is the wanted direction on the ground plane
// (length 0-1); press and held are the jump button.
static void robotStep(Robot* r, const Collision* col, const float move[2], bool press, bool held)
{
	const MoveParams* p = &r->p;
	float mag = sqrtf(move[0] * move[0] + move[1] * move[1]);
	if (mag > 1.0f) mag = 1.0f;

	switch (r->state) {
	case ST_LAND:
		// Landing lag: no control. GUESS: the robot just slows to a stop.
		r->speed -= r->speed < p->runAccel ? r->speed : p->runAccel;
		if (--r->timer <= 0)
			r->state = ST_GROUND;
		r->vel[0] = sinf(r->yaw) * r->speed;
		r->vel[2] = cosf(r->yaw) * r->speed;
		break;
	case ST_GROUND: {
		// Turn the facing toward the stick, then run along the facing at a
		// speed that eases toward the stick's share of the top speed.
		if (mag > 0.01f)
			r->yaw = turnToward(r->yaw, atan2f(move[0], move[1]), p->turn);
		float target = p->runSpeed * mag;
		float d = target - r->speed;
		r->speed += fabsf(d) < p->runAccel ? d : copysignf(p->runAccel, d);
		r->vel[0] = sinf(r->yaw) * r->speed;
		r->vel[2] = cosf(r->yaw) * r->speed;
		if (press) {
			// The run carries into the jump; the lift comes from holding.
			r->state = ST_AIR;
			r->hold = p->jumpHold;
			r->dashesLeft = p->dashes;
			r->speed = 0;
			setMotion(r, MO_JUMP);
		}
		break;
	}
	case ST_AIR:
		if (r->hold > 0) {
			if (held) {
				r->vel[1] += p->jumpLift;
				r->hold--;
			} else {
				r->hold = 0;
			}
		}
		if (press && r->dashesLeft > 0) {
			// Air dash: the stick's direction, or the facing without it.
			if (mag > 0.01f)
				r->yaw = atan2f(move[0], move[1]);
			r->dashesLeft--;
			r->state = ST_DASH;
			r->timer = p->dashFrames;
			r->hold = 0;
			setMotion(r, MO_DASH);
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
		// Flat at the dash speed, pitched up by the body's dash angle, the
		// stick turning it a little each frame.
		if (mag > 0.01f)
			r->yaw = turnToward(r->yaw, atan2f(move[0], move[1]), p->dashSteer);
		r->vel[0] = sinf(r->yaw) * cosf(p->dashPitch) * p->dashSpeed;
		r->vel[1] = sinf(p->dashPitch) * p->dashSpeed;
		r->vel[2] = cosf(r->yaw) * cosf(p->dashPitch) * p->dashSpeed;
		if (--r->timer <= 0) {
			r->state = ST_AIR;
			r->pendingLag = p->afterDash;
			setMotion(r, MO_JUMP);
			r->frame = 1e6f;
		}
	} else {
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
	if (ground && wasAir && fall > 0.0f) {
		// Landing lag, 0x8004b0a0: s7 frames scaled by how hard the landing
		// was against a 6-unit fall, plus whatever an air dash left owing.
		float impact = fall / p->impactSpeed;
		if (impact > 1.0f) impact = 1.0f;
		int lag = (int)(p->landLag * impact + r->pendingLag);
		r->pendingLag = 0;
		r->hold = 0;
		r->vel[1] = 0;
		r->speed = 0;
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

// ------------------------------------------------------------------ scripted input
#ifdef AUTOTEST
#ifndef AUTOTEST_STEP
#define AUTOTEST_STEP 10
#endif
// A fixed run for the emulator, one arena every 10 s: drop in, run, jump,
// air dash, run into an obstacle and a wall. Moves are in camera space
// (x right, y forward); keys are pressed on the first frame of a cue and
// held to its end.
typedef struct { int from, to; float x, y; u32 keys; } Cue;
static const Cue script[] = {
	{  60, 150,  0.0f,  1.0f, 0 },        // run forward
	{ 150, 162,  0.0f,  1.0f, KEY_B },    // full jump: held past the hold window
	{ 175, 176,  1.0f,  0.0f, KEY_B },    // air dash right
	{ 240, 360, -1.0f,  0.0f, 0 },        // run left, into whatever is there
	{ 360, 361, -1.0f,  0.0f, KEY_B },    // tapped jump: a hop
	{ 385, 386,  0.0f, -1.0f, KEY_B },    // air dash back
	{ 450, 560,  0.7f, -0.7f, 0 },        // run diagonally to a wall
};
#define SCRIPT_FRAMES 600
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
	// Start on opposite sides of the floor, dropping in from above it.
	const Collision* c = &s->col;
	bool floor = c->lo[0] < c->hi[0];
	float cx = floor ? (c->lo[0] + c->hi[0]) * 0.5f : c->blocks[0].centre[0];
	float cz = floor ? (c->lo[2] + c->hi[2]) * 0.5f : c->blocks[0].centre[2];
	float ext = floor ? (c->hi[2] - c->lo[2]) * 0.3f : 4.0f;
	float top = floor ? c->hi[1] + 4.0f : c->blocks[0].centre[1];
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
	bool camSnap = true;
	int tick = 0;
	// Speed meter: frames drawn and frames simulated per real second. The
	// game logic is written per 60 Hz frame, so speed = simulated / 60.
	u64 meterStart = osGetTime();
	int meterFrames = 0, meterSteps = 0;
	float fps = 0.0f, speed = 0.0f;
#ifdef AUTOTEST
	int autoFrame = 0;
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
			if (arena + AUTOTEST_STEP >= roster->arenas)
				break;
			arena += AUTOTEST_STEP;
			body = (body + 7) % bodies;
			for (int k = 0; k < PART_COUNT; k++) partIndex[k] += 3;
			reloadStage = reloadRobots = true;
		}
		scripted(autoFrame++, &sx, &sy, &down, &held);
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
			camSnap = info = true;
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

			// Camera: behind this robot on the line from the other one, high
			// enough to see the floor, looking at a point between the two.
			// PLACEHOLDER layout until the game's camera is read.
			float ax = me->pos[0] - foe->pos[0], az = me->pos[2] - foe->pos[2];
			float sep = sqrtf(ax * ax + az * az);
			if (sep < 1e-3f) { ax = 0; az = 1; sep = 1; }
			ax /= sep; az /= sep;
			float back = 5.0f + sep * 0.35f;
			float eye[3] = { me->pos[0] + ax * back, me->pos[1] + 3.2f + sep * 0.15f, me->pos[2] + az * back };
			float at[3] = { me->pos[0] - ax * sep * 0.35f, (me->pos[1] + foe->pos[1]) * 0.5f + 0.9f,
				me->pos[2] - az * sep * 0.35f };
			float k = camSnap ? 1.0f : 0.08f;
			for (int a = 0; a < 3; a++) {
				camEye[a] += (eye[a] - camEye[a]) * k;
				camAt[a] += (at[a] - camAt[a]) * k;
			}
			camSnap = false;
			for (int k2 = 0; k2 < 2; k2++)
				robotAnimate(&robots[k2], &mo);
			meterSteps++;
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
				printf("\nstate  %-10s dashes %d\n", stateName[me->state], me->dashesLeft);
				printf("pos    %6.2f %6.2f %6.2f\n", me->pos[0], me->pos[1], me->pos[2]);
				printf("vel    %6.3f %6.3f %6.3f\n", me->vel[0], me->vel[1], me->vel[2]);
				printf("motion %s\n", me->motion >= 0 ? motionName[me->motion] : "-");
				const int* st = me->p.s;
				printf("run %d/%d turn %d  jump %d/%d  g %d\n", st[0], st[1], st[2], st[3], st[4], st[8]);
				printf("air %d/%d  dash %dx %d@%d  lag %d\n", st[5], st[6], me->p.dashes, st[12], st[13], st[7]);
			}
			printf("\n%4.1f fps   game speed %3.0f%%\n", fps, speed * 100.0f);
			printf("CPU %5.2f ms   GPU %5.2f ms\n", C3D_GetProcessingTime(), C3D_GetDrawingTime());
			printf("\nCircle pad   run\n");
			printf("B            jump (hold: higher)\n");
			printf("B in the air air dash\n");
			printf("D-pad < >    arena\n");
			printf("D-pad ^ v    robot body\n");
			printf("SELECT       next parts and leg\n");
			printf("START        model viewer\n");
		}

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		if (ready)
			for (int k = 0; k < 2; k++)
				animSkin(&robots[k].body, robots[k].world);
		C3D_RenderTargetClear(top, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
		C3D_FrameDrawOn(top);
		if (ready) {
			C3D_Mtx projection, view;
			Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(50.0f), C3D_AspectRatioTop, 0.1f, 1500.0f, false);
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
	for (int k = 0; k < 2; k++)
		robotFree(&robots[k]);
	for (int k = 0; k < PART_COUNT; k++)
		if (havePart[k]) modelFree(&parts[k]);
	stageFree(&stage);
	statsFree(&stats);
	consoleClear();
}
