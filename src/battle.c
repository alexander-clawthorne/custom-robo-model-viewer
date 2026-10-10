// Battle test: run, jump, air dash and land in a real arena, against its own
// collision, with a camera that keeps both robots in view.
//
// The numbers in MoveParams are the game's where they have been read out of
// main.dol and placeholders where not; each says which.
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

typedef struct {
	float runSpeed;     // ground speed, units per frame (60 Hz)
	float runAccel;     // speed gained per frame towards the stick
	float runDecel;     // speed lost per frame with the stick released
	float turnRate;     // radians per frame
	float gravity;      // units per frame per frame
	float maxFall;
	float jumpSpeed;    // upward speed at take-off
	float airSpeed;     // horizontal speed reachable in the air
	float airAccel;
	int   airDashes;    // per jump
	float dashSpeed;
	int   dashFrames;
	float dashLift;     // upward speed at the start of a dash
	int   landFrames;   // recovery after landing
	float radius;       // collision sphere, centred this high above the feet
} MoveParams;

// PLACEHOLDERS until the stats and movement code are read out of the DOL.
// runSpeed is the stride of sf_normal_run: its root channel advances 3.4
// units over the 50-frame cycle.
static const MoveParams defaultParams = {
	.runSpeed = 0.068f, .runAccel = 0.012f, .runDecel = 0.02f, .turnRate = 0.25f,
	.gravity = 0.010f, .maxFall = 0.35f, .jumpSpeed = 0.22f,
	.airSpeed = 0.05f, .airAccel = 0.004f,
	.airDashes = 1, .dashSpeed = 0.20f, .dashFrames = 18, .dashLift = 0.05f,
	.landFrames = 8, .radius = 0.45f,
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
	float yaw;
	int state, timer, dashesLeft;
	bool ground;
	int motion;          // MO_*
	float frame;
	AnimState anim;
	MoveParams p;
} Robot;

typedef struct {
	const MotionSet* set;
	int id[MO_COUNT];    // index into set, -1 if missing
} Motions;

static float wrapAngle(float a)
{
	while (a > PI) a -= 2 * PI;
	while (a < -PI) a += 2 * PI;
	return a;
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

static bool robotLoad(Robot* r, int body, char* err, size_t errSize)
{
	char path[64];
	snprintf(path, sizeof path, "romfs:/robo/%03d.crm", body);
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
	r->state = ST_AIR;
	r->timer = 0;
	r->dashesLeft = 0;
	r->ground = false;
	r->motion = -1;
	r->anim.hasLast = false;
	setMotion(r, MO_JUMP);
	r->frame = 1e6f;  // falling in: hold the end of the jump
}

// One frame of movement. move is the wanted direction on the ground plane
// (length 0-1); jump is the button's press this frame.
static void robotStep(Robot* r, const Collision* col, const float move[2], bool jump)
{
	const MoveParams* p = &r->p;
	float mag = sqrtf(move[0] * move[0] + move[1] * move[1]);
	float hv[2] = { r->vel[0], r->vel[2] };

	switch (r->state) {
	case ST_GROUND:
	case ST_LAND: {
		bool canAct = r->state == ST_GROUND;
		if (r->state == ST_LAND && --r->timer <= 0)
			r->state = ST_GROUND;
		float want[2] = { 0, 0 };
		if (canAct && mag > 0.01f) {
			want[0] = move[0] * p->runSpeed;
			want[1] = move[1] * p->runSpeed;
			float target = atan2f(move[0], move[1]);
			float d = wrapAngle(target - r->yaw);
			r->yaw = wrapAngle(r->yaw + (fabsf(d) < p->turnRate ? d : copysignf(p->turnRate, d)));
		}
		float acc = mag > 0.01f && canAct ? p->runAccel : p->runDecel;
		for (int a = 0; a < 2; a++) {
			float dv = want[a] - hv[a];
			hv[a] += fabsf(dv) < acc ? dv : copysignf(acc, dv);
		}
		if (canAct && jump) {
			r->state = ST_AIR;
			r->vel[1] = p->jumpSpeed;
			r->dashesLeft = p->airDashes;
			setMotion(r, MO_JUMP);
		}
		break;
	}
	case ST_AIR:
		if (mag > 0.01f) {
			for (int a = 0; a < 2; a++) {
				float dv = move[a] * p->airSpeed - hv[a];
				hv[a] += fabsf(dv) < p->airAccel ? dv : copysignf(p->airAccel, dv);
			}
		}
		if (jump && r->dashesLeft > 0) {
			// Air dash: the stick's direction, or straight ahead without it.
			float dir[2] = { sinf(r->yaw), cosf(r->yaw) };
			if (mag > 0.01f) {
				dir[0] = move[0] / mag;
				dir[1] = move[1] / mag;
				r->yaw = atan2f(dir[0], dir[1]);
			}
			r->dashesLeft--;
			r->state = ST_DASH;
			r->timer = p->dashFrames;
			hv[0] = dir[0] * p->dashSpeed;
			hv[1] = dir[1] * p->dashSpeed;
			r->vel[1] = p->dashLift;
			setMotion(r, MO_DASH);
		}
		break;
	case ST_DASH:
		if (--r->timer <= 0) {
			r->state = ST_AIR;
			hv[0] *= 0.35f;
			hv[1] *= 0.35f;
			setMotion(r, MO_JUMP);
			r->frame = 1e6f;
		}
		break;
	}
	r->vel[0] = hv[0];
	r->vel[2] = hv[1];
	if (r->state == ST_DASH)
		r->vel[1] *= 0.9f;          // the dash holds its height
	else
		r->vel[1] -= p->gravity;
	if (r->vel[1] < -p->maxFall)
		r->vel[1] = -p->maxFall;

	// Sub-step so no step moves more than half the radius.
	float speed = sqrtf(r->vel[0] * r->vel[0] + r->vel[1] * r->vel[1] + r->vel[2] * r->vel[2]);
	int steps = 1 + (int)(speed / (p->radius * 0.5f));
	bool ground = false;
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
	if (ground && wasAir && r->vel[1] <= 0.0f) {
		r->state = ST_LAND;
		r->timer = p->landFrames;
		r->vel[1] = 0;
		setMotion(r, MO_LAND);
	} else if (!ground && !wasAir) {
		// Walked off a ledge.
		r->state = ST_AIR;
		r->dashesLeft = p->airDashes;
		setMotion(r, MO_JUMP);
		r->frame = 1e6f;
	}
	if (ground && r->vel[1] < 0)
		r->vel[1] = 0;
	r->ground = ground;

	if (r->state == ST_GROUND) {
		float hs = sqrtf(r->vel[0] * r->vel[0] + r->vel[2] * r->vel[2]);
		setMotion(r, hs > p->runSpeed * 0.25f ? MO_RUN : MO_STAND);
	} else if (r->state == ST_LAND && r->timer <= 0) {
		setMotion(r, MO_STAND);
	}
}

static void robotAnimate(Robot* r, const Motions* mo)
{
	int id = r->motion >= 0 ? mo->id[r->motion] : -1;
	const Motion* m = id >= 0 ? &mo->set->motions[id] : NULL;
	if (m) {
		bool loop = r->motion == MO_STAND || r->motion == MO_RUN;
		float rate = 1.0f;
		if (r->motion == MO_RUN) {
			float hs = sqrtf(r->vel[0] * r->vel[0] + r->vel[2] * r->vel[2]);
			rate = hs / r->p.runSpeed;
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
// (x right, y forward).
typedef struct { int from, to; float x, y; u32 keys; } Cue;
static const Cue script[] = {
	{  60, 150,  0.0f,  1.0f, 0 },        // run forward
	{ 150, 151,  0.0f,  1.0f, KEY_B },    // jump
	{ 175, 176,  1.0f,  0.0f, KEY_B },    // air dash right
	{ 240, 360, -1.0f,  0.0f, 0 },        // run left, into whatever is there
	{ 360, 361, -1.0f,  0.0f, KEY_B },    // jump
	{ 385, 386,  0.0f, -1.0f, KEY_B },    // air dash back
	{ 450, 560,  0.7f, -0.7f, 0 },        // run diagonally to a wall
};
#define SCRIPT_FRAMES 600
static void scripted(int t, float* x, float* y, u32* down)
{
	*x = *y = 0;
	*down = 0;
	for (u32 i = 0; i < sizeof script / sizeof script[0]; i++)
		if (t >= script[i].from && t < script[i].to) {
			*x = script[i].x;
			*y = script[i].y;
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
		u32 down = hidKeysDown();
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
			body = (body + 7) % roster->bodies;
			for (int k = 0; k < PART_COUNT; k++) partIndex[k] += 3;
			reloadStage = reloadRobots = true;
		}
		scripted(autoFrame++, &sx, &sy, &down);
#endif
		if (down & KEY_START)
			break;
		if (down & KEY_DRIGHT) { arena = (arena + 1) % roster->arenas; reloadStage = true; }
		if (down & KEY_DLEFT) { arena = (arena + roster->arenas - 1) % roster->arenas; reloadStage = true; }
		if (down & KEY_DUP) { body = (body + 1) % roster->bodies; reloadRobots = true; }
		if (down & KEY_DDOWN) { body = (body + roster->bodies - 1) % roster->bodies; reloadRobots = true; }
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
					robotLoad(&robots[k], (body + k * 5) % roster->bodies, err, sizeof err);
					robots[k].p = defaultParams;
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
			robotStep(me, &stage.col, move, down & KEY_B);
			float none[2] = { 0, 0 };
			// The other robot stands its ground facing this one.
			foe->yaw = atan2f(me->pos[0] - foe->pos[0], me->pos[2] - foe->pos[2]);
			robotStep(foe, &stage.col, none, false);
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
			printf("Arena %d / %d   body %03d\n", arena + 1, roster->arenas, body);
			printf("parts g%d b%d p%d l%d\n", partIndex[0], partIndex[1], partIndex[2], partIndex[3]);
			if (err[0]) printf("%s\n", err);
			if (ready) {
				printf("\nstate  %-10s dashes %d\n", stateName[me->state], me->dashesLeft);
				printf("pos    %6.2f %6.2f %6.2f\n", me->pos[0], me->pos[1], me->pos[2]);
				printf("vel    %6.3f %6.3f %6.3f\n", me->vel[0], me->vel[1], me->vel[2]);
				printf("motion %s\n", me->motion >= 0 ? motionName[me->motion] : "-");
			}
			printf("\n%4.1f fps   game speed %3.0f%%\n", fps, speed * 100.0f);
			printf("CPU %5.2f ms   GPU %5.2f ms\n", C3D_GetProcessingTime(), C3D_GetDrawingTime());
			printf("\nCircle pad   run\n");
			printf("B            jump / air dash\n");
			printf("D-pad < >    arena\n");
			printf("D-pad ^ v    robot body\n");
			printf("SELECT       next parts\n");
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
	consoleClear();
}
