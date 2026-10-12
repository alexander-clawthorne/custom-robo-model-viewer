// Guns, bombs and pods, on the game's own records (tools/stats.py cooks them).
//
// The rules come from main.dol, per part kind:
//   gun   volleys 0x80042ab8, bullets and their hits 0x80041358
//   bomb  aim and throw 0x8003f248, flight and landing 0x8003fe98
//   pod   launch 0x8003d7e0, flight 0x8003dd38
//   blasts (bombs and pods) 0x80044ea4 / 0x80044a90, hits 0x800443c0
// A hit is not applied here: it is written into the victim's Hit record, and
// the victim's own update reads it the next frame, as 0x8004b0a0 does.
// What is still a guess says so.
#include "weapon.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "anim.h"
#include "effects.h"
#include "model.h"
#include "render.h"

#define PI 3.14159265f
#define DEG (PI / 180.0f)
#define ANGLE16 (2.0f * PI / 65536.0f)

static float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static float norm3(float* v)
{
	float l = sqrtf(dot3(v, v));
	if (l > 1e-8f)
		for (int k = 0; k < 3; k++) v[k] /= l;
	return l;
}

// Turn a vector about the vertical by a (radians), +x toward +z... in the
// robot's terms, yaw grows from +z toward +x as in battle.c's facing.
static void rotY(float* v, float a)
{
	float c = cosf(a), s = sinf(a);
	float x = v[0] * c + v[2] * s, z = -v[0] * s + v[2] * c;
	v[0] = x;
	v[2] = z;
}

static float wrap(float a)
{
	while (a > PI) a -= 2 * PI;
	while (a < -PI) a += 2 * PI;
	return a;
}

static float frand(Arsenal* a)   // deterministic, so autotest runs repeat
{
	a->rng = a->rng * 1664525u + 1013904223u;
	return (a->rng >> 8) / 16777216.0f;
}

static bool outside(const ArenaBounds* b, const float* p)
{
	float dx = p[0] - b->centre[0], dz = p[2] - b->centre[1];
	return dx * dx + dz * dz > b->radius2;
}

// H+0x14 = (u16)(damage * 0.01 * power + H+0x14), H+0x16 += down, the rest
// copied: 0x80041358 for bullets, 0x800443c0 for blasts.
static void addHit(Arsenal* a, Hit* h, int kind, const HitParams* p, int power, const float* dir)
{
	h->any = true;
	h->kind = kind;
	h->damage = (int)(p->damage * 0.01f * power + h->damage);
	h->down += p->down;
	h->last = *p;
	float d[3] = { dir[0], 0, dir[2] };
	if (norm3(d) < 1e-6f) { d[0] = 0; d[2] = 1; }
	memcpy(h->dir, d, sizeof d);
	a->hits[kind]++;
}

void arsenalReset(Arsenal* a, const GunStats* gun, const BombStats* bomb, const PodStats* pod)
{
	u32 rng = a->rng ? a->rng : 12345;
	for (int i = 0; i < MAX_BULLETS; i++)
		if (a->bullets[i].fx) efxKill(a->bullets[i].fx - 1);
	if (a->flashFx) efxKill(a->flashFx - 1);
	memset(a, 0, sizeof *a);
	a->gun = gun;
	a->bomb = bomb;
	a->pod = pod;
	a->rng = rng;
}

// ------------------------------------------------------------------ gun
void gunStart(Arsenal* a, bool air)
{
	if (!a->gun || a->gunState)
		return;
	a->gunState = air ? 2 : 1;
	a->volleys = 0;
	a->gunTimer = 0;
	a->sinceVolley = 0x100;   // the first volley fires at once
}

bool gunBusy(const Arsenal* a) { return a->gunState != 0; }

void gunInterrupt(Arsenal* a, bool deleteShots)
{
	if (a->gunState) {
		const GunPattern* P = &a->gun->pattern[a->gunState - 1];
		if (a->volleys <= P->volleys) {
			a->gunTimer = P->recovery;
			a->volleys = P->volleys + 1;
		}
	}
	if (deleteShots)
		for (int i = 0; i < MAX_BULLETS; i++) a->bullets[i].state = 0;
}

static EfxSet* sGunFxSet;
static Model*  sGunFxModel;
static int     sGunFxIndex = -1;

void weaponSetGunFx(EfxSet* set, Model* model, int index)
{
	sGunFxSet = set;
	sGunFxModel = model;
	sGunFxIndex = index;
}

// A basis with `axis` (normalised) as column `col`, the other two square to
// it and to world up (world x if axis is vertical), every column times scale.
static void axisMatrix(const float* axis, int col, float scale, const float* pos, Mat34* m)
{
	float a[3] = { axis[0], axis[1], axis[2] };
	if (norm3(a) < 1e-6f) { a[0] = 0; a[1] = 0; a[2] = 1; }
	float s[3] = { -a[2], 0, a[0] };   // a x up
	if (norm3(s) < 1e-6f) { s[0] = 1; s[1] = 0; s[2] = 0; }
	float t[3] = { s[1] * a[2] - s[2] * a[1], s[2] * a[0] - s[0] * a[2], s[0] * a[1] - s[1] * a[0] };
	// columns (col, col+1, col+2) = (a, t, s) keeps the basis right-handed
	const float* c[3] = { a, t, s };
	for (int r = 0; r < 3; r++) {
		for (int k = 0; k < 3; k++)
			m->m[r][(col + k) % 3] = c[k][r] * scale;
		m->m[r][3] = pos[r];
	}
}

// Per flying bullet, model 0 of the gun's effect (0x800381d0 builds its
// matrix): +Y along the flight (0x8001d03c writes column 1), scaled by the
// phase's radius, at the bullet. Its animation runs 30 frames per phase
// (rate 150 / frames / 5). Any other state ends it.
static void bulletFxSync(Arsenal* a, const Fighter* me)
{
	for (int i = 0; i < MAX_BULLETS; i++) {
		Bullet* b = &a->bullets[i];
		if (b->state != 1 || !sGunFxModel) {
			if (b->fx) efxKill(b->fx - 1);
			b->fx = 0;
			continue;
		}
		const GunPhase* E = &b->pat->phase[b->phase];
		Mat34 m;
		axisMatrix(b->dir, 1, E->radius, b->pos, &m);
		if (!b->fx || !efxMove(b->fx - 1, &m)) {
			int h = efxSpawn(sGunFxSet, sGunFxIndex, sGunFxModel, 0, &m, true);
			b->fx = h >= 0 ? h + 1 : 0;
		}
		if (b->fx && E->frames)
			efxSetRate(b->fx - 1, 30.0f / E->frames);
	}
	// Model 1 is the muzzle flash: 10 frames from each shot (0x80042fa4),
	// drawn at the muzzle joint's world matrix (0x80042a68). Only the
	// barrel's +X is known here, so the roll about it is a guess.
	if (a->flashTimer > 0 && sGunFxModel) {
		Mat34 m;
		axisMatrix(me->gunDir, 0, PART_SCALE, me->gunAt, &m);
		if (!a->flashFx || !efxMove(a->flashFx - 1, &m)) {
			int h = efxSpawn(sGunFxSet, sGunFxIndex, sGunFxModel, 1, &m, false);
			a->flashFx = h >= 0 ? h + 1 : 0;
		}
		a->flashTimer--;
	} else if (a->flashFx) {
		efxKill(a->flashFx - 1);
		a->flashFx = 0;
	}
}

static void fireVolley(Arsenal* a, const GunPattern* P, const Fighter* me, const Fighter* foe, const Collision* col)
{
	int slots = a->gun->slots < MAX_BULLETS ? a->gun->slots : MAX_BULLETS;
	// Along the barrel: the muzzle joint's +X, which the gun arm's aim at
	// the opponent (battle.c) points their way. The Giant Gun aims instead.
	float dir[3];
	memcpy(dir, me->gunDir, sizeof dir);
	if (a->gun->id == GUN_GIANT)
		for (int k = 0; k < 3; k++) dir[k] = foe->chest[k] - me->gunAt[k];
	if (norm3(dir) < 1e-6f) memcpy(dir, me->facing, sizeof dir);
	for (int n = 0; n < P->perVolley; n++) {
		int i = 0;
		while (i < slots && a->bullets[i].state) i++;
		if (i == slots)
			return;   // the rest of the volley is dropped
		Bullet* b = &a->bullets[i];
		memset(b, 0, sizeof *b);
		b->state = 1;
		b->pat = P;
		b->index = n;
		memcpy(b->pos, me->gunAt, sizeof b->pos);
		memcpy(b->dir, dir, sizeof b->dir);
		memcpy(b->trail[0], b->pos, sizeof b->pos);
		a->fired++;
		a->flashTimer = 10;
		// A muzzle inside a wall: the bullet ends where the wall is.
		float hit[3], nrm[3];
		if (collideSegment(col, me->hips, b->pos, hit, nrm)) {
			memcpy(b->pos, hit, sizeof hit);
			b->state = 2;
			b->linger = 1;
		}
	}
}

// Yaw and pitch of a direction.
static void angles(const float* d, float* yaw, float* pitch)
{
	*yaw = atan2f(d[0], d[2]);
	*pitch = atan2f(d[1], sqrtf(d[0] * d[0] + d[2] * d[2]));
}

static void fromAngles(float yaw, float pitch, float* d)
{
	d[0] = cosf(pitch) * sinf(yaw);
	d[1] = sinf(pitch);
	d[2] = cosf(pitch) * cosf(yaw);
}

static float clampf(float v, float lim) { return v > lim ? lim : v < -lim ? -lim : v; }

// Distance from p to the segment a-b.
static float segDist(const float* a, const float* b, const float* p)
{
	float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	float ap[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
	float l2 = dot3(ab, ab);
	float t = l2 > 1e-12f ? dot3(ap, ab) / l2 : 0.0f;
	if (t < 0) t = 0;
	if (t > 1) t = 1;
	float d[3] = { ap[0] - ab[0] * t, ap[1] - ab[1] * t, ap[2] - ab[2] * t };
	return sqrtf(dot3(d, d));
}

// 0x80041358: the victim is a sphere; the bullet a capsule along the last
// trail+1 frames of its path, with the phase's radius.
static bool bulletTouches(const Bullet* b, const GunPhase* E, const Fighter* foe)
{
	float reach = foe->radius + E->radius;
	int segs = E->trail + 1;
	if (segs > b->age) segs = b->age;
	if (segs > BULLET_TRAIL - 1) segs = BULLET_TRAIL - 1;
	for (int s = 0; s < segs; s++) {
		const float* p0 = b->trail[(b->age - s) % BULLET_TRAIL];
		const float* p1 = b->trail[(b->age - s - 1 + BULLET_TRAIL) % BULLET_TRAIL];
		if (segDist(p0, p1, foe->centre) < reach)
			return true;
	}
	float d[3] = { foe->centre[0] - b->pos[0], foe->centre[1] - b->pos[1], foe->centre[2] - b->pos[2] };
	return dot3(d, d) < reach * reach;
}

// 0x80041810: the Giant and Knuckle Guns only hit with a clear line from
// the victim's centre to the bullet.
static bool bulletHits(const Arsenal* a, const Bullet* b, const GunPhase* E, const Fighter* foe,
	const Collision* col)
{
	if (!bulletTouches(b, E, foe))
		return false;
	if (a->gun->id != GUN_GIANT && a->gun->id != GUN_KNUCKLE)
		return true;
	float hit[3], nrm[3];
	return !collideSegment(col, foe->centre, b->pos, hit, nrm);
}

static void bulletStep(Arsenal* a, Bullet* b, const Fighter* me, const Fighter* foe, Hit* foeHit,
	const Collision* col, const ArenaBounds* bounds)
{
	const GunPattern* P = b->pat;
	const GunPhase* E = &P->phase[b->phase];
	if (b->state == 1) {
		// The fan and the random cone, once, at the start of one phase.
		if (b->phase == P->fanPhase && b->frame == 0) {
			float yaw, pitch;
			angles(b->dir, &yaw, &pitch);
			// 0x80042ab8 turns the bullet about its left axis (pitch fan) or
			// its down axis (yaw fan) with a right-handed rotation: a positive
			// angle aims it down, or to the shooter's right. The Left Arc's
			// ground fan is negative and goes left; the four arc and loop guns
			// flip the sign in their air patterns -- that is in the data.
			float fan = (P->fanStart - b->index * P->fanStep) * ANGLE16;
			if (P->fanAxis) yaw -= fan; else pitch -= fan;
			if (P->cone) {
				yaw += (frand(a) * 2 - 1) * P->cone * ANGLE16;
				pitch += (frand(a) * 2 - 1) * P->cone * ANGLE16;
			}
			fromAngles(yaw, pitch, b->dir);
		}
		// Homing toward out[1], each angle by at most the phase's limit.
		float to[3] = { foe->aim[0] - b->pos[0], foe->aim[1] - b->pos[1], foe->aim[2] - b->pos[2] };
		if ((E->homeYaw > 0 || E->homePitch > 0) && (!a->gun->homeAhead || dot3(to, b->dir) > 0)) {
			float yaw, pitch, tyaw, tpitch;
			angles(b->dir, &yaw, &pitch);
			angles(to, &tyaw, &tpitch);
			yaw += clampf(wrap(tyaw - yaw), E->homeYaw);
			pitch += clampf(tpitch - pitch, E->homePitch);
			fromAngles(yaw, pitch, b->dir);
		}
		float old[3];
		memcpy(old, b->pos, sizeof old);
		for (int k = 0; k < 3; k++) b->pos[k] += b->dir[k] * E->speed;
		float hit[3], nrm[3];
		if (collideSegment(col, old, b->pos, hit, nrm) || outside(bounds, b->pos)) {
			if (!outside(bounds, b->pos)) memcpy(b->pos, hit, sizeof hit);
			b->state = 2;
			b->linger = E->trail + 1;
		}
		b->age++;
		memcpy(b->trail[b->age % BULLET_TRAIL], b->pos, sizeof b->pos);
		if (b->state == 1 && ++b->frame >= E->frames) {
			b->frame = 0;
			do b->phase++; while (b->phase < 5 && P->phase[b->phase].frames == 0);
			if (b->phase >= 5) {
				b->state = 0;   // expires without a mark
				return;
			}
		}
	} else if (--b->linger <= 0) {
		b->state = 0;
		return;
	}
	// The hit, once per bullet: a flying bullet stops there, a lingering
	// trail can still catch the robot.
	if (!b->hit && foe->hittable && bulletHits(a, b, E, foe, col)) {
		b->hit = true;
		addHit(a, foeHit, HIT_GUN, &E->hit, me->power[0], b->dir);
		if (b->state == 1) {
			b->state = 2;
			b->linger = E->trail + 1;
		}
	}
}

static void gunStep(Arsenal* a, const Fighter* me, const Fighter* foe, const Collision* col)
{
	if (!a->gunState)
		return;
	const GunPattern* P = &a->gun->pattern[a->gunState - 1];
	if (a->volleys < P->volleys) {
		if (a->sinceVolley < P->interval) {
			a->sinceVolley++;
		} else {
			fireVolley(a, P, me, foe, col);
			a->sinceVolley = 0;
			if (++a->volleys == P->volleys)
				a->gunTimer = a->gunState == 1 ? a->gun->cooldown : 0;   // the air pattern has no cooldown
		}
	} else if (a->volleys == P->volleys) {
		if (a->gunTimer > 0) {
			a->gunTimer--;
		} else {
			a->volleys++;
			a->gunTimer = P->recovery * me->gunRecovery / 100;
		}
	} else if (a->gunTimer > 0) {
		a->gunTimer--;
	} else {
		a->gunState = 0;
	}
}

// ------------------------------------------------------------------ blasts
static void explode(Explosion* x, const Blast* e, const float* pos, const float* vel, bool typeR, const float* owner)
{
	memset(x, 0, sizeof *x);
	x->active = true;
	x->e = e;
	x->typeR = typeR;
	memcpy(x->pos, pos, sizeof x->pos);
	memcpy(x->vel, vel, sizeof x->vel);
	memcpy(x->owner, owner, sizeof x->owner);
}

static float burstRadius(const Blast* e, int age)
{
	if (age <= e->grow)
		return e->grow ? age * e->r1 / e->grow : e->r1;
	return e->r1 + (age - e->grow) * (e->r2 - e->r1) / (e->tail ? e->tail : 1);
}

// 0x80044a90 and the hit test 0x800443c0. The bursts are laid along the
// explosion's drift; each is a growing vertical cylinder.
static void explosionStep(Arsenal* a, Explosion* x, int kind, const Fighter* me, const Fighter* foe, Hit* foeHit)
{
	const Blast* e = x->e;
	int gap = e->gap ? e->gap : 1;
	int n = e->bursts > 4 ? 4 : e->bursts < 1 ? 1 : e->bursts;
	if (x->t > e->grow + (n - 1) * gap + e->tail) {
		x->active = false;
		return;
	}
	if (x->t <= (n - 1) * gap && x->t % gap == 0) {
		Burst* b = &x->burst[x->t / gap];
		memset(b, 0, sizeof *b);
		b->active = true;
		memcpy(b->pos, x->pos, sizeof b->pos);
		b->pos[1] += 0.03f;
		a->blasts++;
	}
	for (int k = 0; k < 3; k++) x->pos[k] += x->vel[k];
	x->t++;
	for (int i = 0; i < 4; i++) {
		Burst* b = &x->burst[i];
		if (!b->active)
			continue;
		b->radius = burstRadius(e, b->age);
		if (++b->age > e->grow + e->tail) {
			b->active = false;
			continue;
		}
		if (b->hit || !foe->hittable)
			continue;
		float v[3] = { foe->centre[0] - b->pos[0], foe->centre[1] - b->pos[1], foe->centre[2] - b->pos[2] };
		// As decompiled, R is read from the robot being updated -- the
		// blast's owner -- not from the victim.
		float R = me->radius, r = b->radius;
		if (v[0] * v[0] + v[2] * v[2] < (R + r) * (R + r) && fabsf(v[1]) < r * e->height + R) {
			b->hit = true;
			float dir[3];
			if (x->typeR) {   // "Type R": away from the owner, not the blast
				for (int k = 0; k < 3; k++) dir[k] = foe->pos[k] - x->owner[k];
			} else {
				memcpy(dir, v, sizeof dir);
			}
			addHit(a, foeHit, kind, &e->hit, me->power[kind - 1], dir);
		}
	}
}

// ------------------------------------------------------------------ bomb
bool bombReady(const Arsenal* a) { return a->bomb && a->bombReload < 1; }

static void floorCursor(float* c, const Collision* col)
{
	float f = floorBelow(col, c[0], 50.0f, c[2]);
	if (f > -1e8f)
		c[1] = f;
}

void bombAim(Arsenal* a, const Fighter* me, const Fighter* foe, const Collision* col)
{
	if (!a->bomb)
		return;
	const BombMode* m = &a->bomb->mode[me->airborne];
	float t = m->aim * 0.01f;
	for (int k = 0; k < 3; k++)
		a->cursor[k] = me->pos[k] * (1 - t) + foe->pos[k] * t;
	floorCursor(a->cursor, col);
	// Not through a wall: a sweep from the cursor back to the robot.
	float from[3] = { a->cursor[0], a->cursor[1] + 0.5f, a->cursor[2] };
	float to[3] = { me->pos[0], me->pos[1] + 0.5f, me->pos[2] };
	float hit[3], nrm[3];
	if (collideSegment(col, from, to, hit, nrm) && nrm[1] < 0.3f) {
		a->cursor[0] = hit[0];
		a->cursor[2] = hit[2];
		floorCursor(a->cursor, col);
	}
	a->aiming = true;
	a->bombReload = 4;   // the s16 at 0x801c6c10
}

void bombCancel(Arsenal* a) { a->aiming = false; }

void bombThrow(Arsenal* a, const Fighter* me)
{
	if (!a->bomb || !a->aiming)
		return;
	const BombMode* m = &a->bomb->mode[me->airborne];
	// Forward is the launcher joint's +X, flattened (0x8003f248).
	float f[3] = { me->bombDir[0], 0, me->bombDir[2] };
	if (norm3(f) < 1e-6f) { f[0] = a->cursor[0] - me->bombAt[0]; f[1] = 0; f[2] = a->cursor[2] - me->bombAt[2]; }
	if (norm3(f) < 1e-6f) memcpy(f, me->facing, sizeof f);
	float s[3] = { f[2], 0, -f[0] };
	bool used = false;
	for (int k = 0; k < a->bomb->count && k < 4; k++) {
		int i = 0;
		while (i < 8 && a->bombs[i].state) i++;
		if (i == 8)
			break;
		Bomb* b = &a->bombs[i];
		memset(b, 0, sizeof *b);
		b->state = 1;
		b->mode = m;
		b->shot = k;
		memcpy(b->origin, me->bombAt, sizeof b->origin);
		memcpy(b->pos, me->bombAt, sizeof b->pos);
		for (int c = 0; c < 3; c++)
			b->target[c] = a->cursor[c] + 0.01f * m->shot[k][0] * s[c] + 0.01f * m->shot[k][1] * f[c];
		b->target[1] += 0.0001f;
		float d[3] = { b->target[0] - b->origin[0], b->target[1] - b->origin[1], b->target[2] - b->origin[2] };
		float dist = norm3(d);
		memcpy(b->vel, d, sizeof d);
		b->flight = (int)(1000.0f * dist / (m->speed ? m->speed : 1));
		if (b->flight < 1) b->flight = 1;
		used = true;
		a->thrown++;
	}
	a->aiming = false;
	if (used)
		a->bombReload = m->reload;
}

static bool nearFoe(const Fighter* foe, const float* p, float r)
{
	float d[3] = { foe->pos[0] - p[0], foe->pos[1] - p[1], foe->pos[2] - p[2] };
	return dot3(d, d) < r * r;
}

static bool bombTriggered(const Arsenal* a, const Bomb* b, const Fighter* foe)
{
	float r = 0.01f * a->bomb->trigger;
	if (nearFoe(foe, b->pos, r))
		return true;
	float below[3] = { b->pos[0], b->pos[1] - 1.0f, b->pos[2] };
	return (a->bomb->flags & BOMB_TOMAHAWK) && nearFoe(foe, below, r);
}

static void bombExplode(Arsenal* a, Bomb* b, const Fighter* me)
{
	const BombMode* m = b->mode;
	float v[3] = { b->vel[0], 0, b->vel[2] };
	if (norm3(v) < 1e-6f) { v[0] = 0; v[2] = 1; }
	for (int k = 0; k < 3; k++) v[k] *= 0.001f * m->speed;
	float at[3];
	memcpy(at, b->pos, sizeof at);
	if (a->bomb->flags & BOMB_SMASH) {   // a blast right in front of the owner
		float d[3] = { b->target[0] - b->origin[0], 0, b->target[2] - b->origin[2] };
		norm3(d);
		for (int k = 0; k < 3; k++) at[k] = me->hips[k] + 2.05f * d[k];
		at[1] += 2.45f;
	}
	explode(&b->x, &m->blast, at, v, a->bomb->flags & BOMB_TYPE_R, me->pos);
	b->state = 3;
}

static void bombStep(Arsenal* a, Bomb* b, const Fighter* me, const Fighter* foe, Hit* foeHit,
	const Collision* col, const ArenaBounds* bounds)
{
	const BombMode* m = b->mode;
	if (b->state == 1) {
		float old[3];
		memcpy(old, b->pos, sizeof old);
		if (b->hop > 0) {
			// Bounced off a wall early: straight up, then the arc again.
			b->pos[1] += 0.0005f * m->speed;
			b->vel[0] = b->vel[2] = 0;
			b->vel[1] = 1;
			if (--b->hop == 0)
				memcpy(b->origin, b->pos, sizeof b->origin);
		} else if (b->t < b->flight) {
			// A parametric arc from the origin to the target, not gravity.
			b->t++;
			float d[3] = { b->target[0] - b->origin[0], b->target[1] - b->origin[1], b->target[2] - b->origin[2] };
			float dist = norm3(d);
			float side[3] = { d[2], 0, -d[0] };
			if (norm3(side) < 1e-6f) { side[0] = 1; side[2] = 0; }
			float up[3] = { d[1] * side[2] - d[2] * side[1], d[2] * side[0] - d[0] * side[2], d[0] * side[1] - d[1] * side[0] };
			float u = (float)b->t / b->flight;
			float lift = 0.003f * dist * sinf(PI * u);
			for (int k = 0; k < 3; k++)
				b->pos[k] = b->origin[k] + dist * u * d[k] + lift * (m->shot[b->shot][2] * up[k] + m->shot[b->shot][3] * side[k]);
			float v[3] = { b->pos[0] - old[0], b->pos[1] - old[1], b->pos[2] - old[2] };
			if (norm3(v) > 1e-6f) memcpy(b->vel, v, sizeof v);
		} else if (a->bomb->flags & BOMB_DOUBLE_MINE) {
			// GUESS: the Double Mine stops at its target; it is let drop.
			b->vel[0] = b->vel[2] = 0;
			b->vel[1] = -1;
			b->pos[1] -= 0.001f * m->speed;
		} else {
			for (int k = 0; k < 3; k++) b->pos[k] += b->vel[k] * 0.001f * m->speed;   // past the target: straight on
		}
		float hit[3], n[3];
		if (collideSegment(col, old, b->pos, hit, n)) {
			memcpy(b->pos, hit, sizeof hit);
			if (b->t > 5 || fabsf(n[1]) > 0.5f || b->hopped) {
				b->state = 2;
				b->age = 0;
			} else {
				for (int k = 0; k < 3; k++) b->pos[k] += 0.1f * n[k];
				b->hop = 3000 / (m->speed ? m->speed : 1);
				b->flight *= 2;
				b->t = 0;
				b->hopped = true;
			}
		}
		if (b->pos[1] >= 30.0f && b->state == 1) {
			b->state = 2;
			b->age = 0;
		}
		if (outside(bounds, b->pos)) {
			b->state = 0;   // gone without a blast
			return;
		}
		if (bombTriggered(a, b, foe))
			bombExplode(a, b, me);
	} else if (b->state == 2) {
		if (b->age++ >= m->fuse || bombTriggered(a, b, foe))
			bombExplode(a, b, me);
	}
	if (b->state == 3) {
		explosionStep(a, &b->x, HIT_BOMB, me, foe, foeHit);
		if (!b->x.active)
			b->state = 0;
	}
}

// ------------------------------------------------------------------ pod
bool podReady(const Arsenal* a) { return a->pod && a->podsOut < a->pod->max; }

void podLaunch(Arsenal* a, const Fighter* me)
{
	if (!podReady(a))
		return;
	const PodStats* p = a->pod;
	int air = me->airborne, n = p->per;
	for (int i = 0; i < n; i++) {
		int s = 0;
		while (s < 3 && a->pods[s].state) s++;
		if (s == 3)
			break;
		Pod* q = &a->pods[s];
		memset(q, 0, sizeof *q);
		q->state = 1;
		q->life = p->life;
		q->launch = p->launchFrames[air];
		memcpy(q->pos, me->chest, sizeof q->pos);
		// The fan of launch directions, +s/2 down to -s/2, and the homing
		// heading each starts on, from the record's two yaw words.
		float spread = n > 1 ? p->spread * (0.5f - (float)i / (n - 1)) : 0.0f;
		float yaw = n > 1 ? p->yaw[air][1] + (p->yaw[air][0] - p->yaw[air][1]) * (float)i / (n - 1) : p->yaw[air][0];
		float h[3];
		memcpy(h, me->facing, sizeof h);
		rotY(h, spread * DEG);
		float e = p->elev[air] * DEG, sp = 0.001f * p->launchSpeed[air];
		q->vel[0] = h[0] * cosf(e) * sp;
		q->vel[1] = sinf(e) * sp;
		q->vel[2] = h[2] * cosf(e) * sp;
		memcpy(q->heading, me->facing, sizeof q->heading);
		// rotY turns toward the left: a positive spread fans left, a
		// positive yaw starts the homing to the right (0x8003d7e0; the
		// Double Wave Pod's first pod goes left on both).
		rotY(q->heading, -yaw * DEG);
		a->podsOut++;
		a->launched++;
	}
}

static bool podTriggered(const PodStats* p, const Pod* q, const Fighter* foe)
{
	float dx = foe->pos[0] - q->pos[0], dz = foe->pos[2] - q->pos[2];
	float rh = 0.01f * p->rangeH, rv = 0.01f * p->rangeV;
	if (dx * dx + dz * dz >= rh * rh)
		return false;
	float dy = foe->pos[1] - q->pos[1];
	return (p->flags & POD_LOW_TRIGGER) ? dy < rv && dy > -0.65f : fabsf(dy) < rv;
}

static void podExplode(Arsenal* a, Pod* q, const Fighter* me)
{
	explode(&q->x, &a->pod->blast, q->pos, q->vel, a->pod->flags & POD_TYPE_R, me->pos);
	q->state = 2;
}

static void podStep(Arsenal* a, Pod* q, const Fighter* me, const Fighter* foe, Hit* foeHit,
	const Collision* col, const ArenaBounds* bounds)
{
	const PodStats* p = a->pod;
	if (q->state == 1) {
		bool launching = q->launch > 0;
		if (launching) {
			for (int k = 0; k < 3; k++) q->pos[k] += q->vel[k];
			q->launch--;
		} else {
			float d[3] = { foe->pos[0] - q->pos[0], 0, foe->pos[2] - q->pos[2] };
			float dist = sqrtf(dot3(d, d));
			float err = atan2f(q->heading[2] * d[0] - q->heading[0] * d[2], dot3(q->heading, d));
			bool was = q->locked;
			q->locked = p->lockOn && dist < 0.01f * p->lockRange && (p->cone > 180 || fabsf(err) < p->cone * DEG);
			if (q->locked && !was)
				q->pause = p->pause;
			rotY(q->heading, err * 0.001f * p->gain[was]);
			if (q->pause > 0) {
				q->pause--;
				q->vel[0] = q->vel[1] = q->vel[2] = 0;
			} else {
				float sp = 0.001f * p->speed[was];   // the lock as the frame began, as for the gain
				q->vel[0] = q->heading[0] * sp;
				q->vel[2] = q->heading[2] * sp;
			}
			q->vel[1] -= 0.003f;
			for (int k = 0; k < 3; k++) q->pos[k] += q->vel[k];
			q->life--;
		}
		// Walls: slide along (or bounce off, for some pods); never destroyed.
		float before[3];
		memcpy(before, q->pos, sizeof before);
		bool ground;
		if (collideSphere(col, q->pos, 0.3f, &ground, NULL)) {
			float n[3] = { q->pos[0] - before[0], q->pos[1] - before[1], q->pos[2] - before[2] };
			if (norm3(n) > 1e-6f && fabsf(n[1]) < 0.6f) {
				float c = p->reflect ? 2.0f : 1.0f, vn = dot3(q->vel, n), hn = dot3(q->heading, n);
				if (vn < 0)
					for (int k = 0; k < 3; k++) q->vel[k] -= c * vn * n[k];
				if (hn < 0) {
					for (int k = 0; k < 3; k++) q->heading[k] -= c * hn * n[k];
					q->heading[1] = 0;
					norm3(q->heading);
				}
			}
		}
		float floor = floorBelow(col, q->pos[0], q->pos[1] + 0.5f, q->pos[2]);
		if (floor > -1e8f) {
			if (q->pos[1] < floor) q->pos[1] = floor;
			if (!launching) {   // a spring toward the hover height
				q->vel[1] += 0.01f * (0.01f * p->hover[q->locked] + 0.6f + floor - q->pos[1]);
				q->vel[1] *= 0.9f;
			}
		}
		if ((!launching || !(p->flags & POD_NO_EARLY)) && podTriggered(p, q, foe))
			podExplode(a, q, me);
		else if ((!launching && q->life <= 0) || outside(bounds, q->pos))
			podExplode(a, q, me);
	}
	if (q->state == 2) {
		explosionStep(a, &q->x, HIT_POD, me, foe, foeHit);
		if (!q->x.active) {
			q->state = 0;   // a pod counts as out until its blast is over
			a->podsOut--;
		}
	}
}

// ------------------------------------------------------------------ frame
void arsenalStep(Arsenal* a, const Fighter* me, const Fighter* foe, Hit* foeHit, const Collision* col,
	const ArenaBounds* bounds)
{
	if (a->gun) {
		gunStep(a, me, foe, col);
		for (int i = 0; i < MAX_BULLETS; i++)
			if (a->bullets[i].state)
				bulletStep(a, &a->bullets[i], me, foe, foeHit, col, bounds);
	}
	bulletFxSync(a, me);
	if (a->bomb) {
		if (a->aiming && a->bombReload <= 0) {
			// The stick moves the cursor, 0.1 * 175 hundredths a frame.
			float old[3];
			memcpy(old, a->cursor, sizeof old);
			a->cursor[0] += 0.175f * me->stick[0];
			a->cursor[2] += 0.175f * me->stick[1];
			float from[3] = { old[0], old[1] + 0.5f, old[2] }, to[3] = { a->cursor[0], old[1] + 0.5f, a->cursor[2] };
			float hit[3], nrm[3];
			if (collideSegment(col, from, to, hit, nrm) && nrm[1] < 0.3f)
				memcpy(a->cursor, old, sizeof old);
			floorCursor(a->cursor, col);
		}
		if (a->aiming) a->bombReload--;   // aiming counts down twice as fast
		for (int i = 0; i < 8; i++)
			if (a->bombs[i].state)
				bombStep(a, &a->bombs[i], me, foe, foeHit, col, bounds);
		if (a->bombReload > -1000) a->bombReload--;
	}
	if (a->pod)
		for (int i = 0; i < 3; i++)
			if (a->pods[i].state)
				podStep(a, &a->pods[i], me, foe, foeHit, col, bounds);
}

int arsenalLive(const Arsenal* a)
{
	int n = 0;
	for (int i = 0; i < MAX_BULLETS; i++) n += a->bullets[i].state != 0;
	for (int i = 0; i < 8; i++) n += a->bombs[i].state != 0;
	for (int i = 0; i < 3; i++) n += a->pods[i].state != 0;
	return n;
}

// ------------------------------------------------------------------ drawing
// Until the effects play (step 13) every shot is a plain sphere: bullets
// yellow, bombs orange, pods cyan, blasts a see-through shell the size and
// height of what they hit.
enum { BALL_BULLET, BALL_BOMB, BALL_POD, BALL_BLAST, BALL_CURSOR, BALL_COUNT };
static Model balls[BALL_COUNT];

static void makeBall(Model* m, const u8* rgba, bool blend)
{
	// An octahedron split twice: 128 triangles.
	static const float oct[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	static const u8 face[8][3] = { { 0, 2, 4 }, { 4, 2, 1 }, { 1, 2, 5 }, { 5, 2, 0 }, { 4, 3, 0 }, { 1, 3, 4 }, { 5, 3, 1 }, { 0, 3, 5 } };
	memset(m, 0, sizeof *m);
	m->vertexCount = 8 * 16 * 3;
	m->vbo = linearAlloc(m->vertexCount * sizeof(ModelVertex));
	m->batches = calloc(1, sizeof(ModelBatch));
	m->batchCount = 1;
	if (!m->vbo || !m->batches)
		return;
	ModelBatch* b = &m->batches[0];
	b->texture = -1;
	b->flags = blend ? MODEL_BLEND | MODEL_NO_ZWRITE : MODEL_LIT;
	b->count = m->vertexCount;
	b->joint = b->root = -1;
	ModelVertex* v = m->vbo;
	for (int f = 0; f < 8; f++) {
		const float* A = oct[face[f][0]], * B = oct[face[f][1]], * C = oct[face[f][2]];
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 4 - i; j++)
				for (int up = 0; up < (j < 3 - i ? 2 : 1); up++) {
					// Corners of one small triangle in barycentric steps of 1/4.
					int tri[3][2] = { { i, j }, { i + 1, j }, { i, j + 1 } };
					if (up) { tri[0][0] = i + 1; tri[0][1] = j; tri[1][0] = i + 1; tri[1][1] = j + 1; tri[2][0] = i; tri[2][1] = j + 1; }
					for (int c = 0; c < 3; c++) {
						float u = tri[c][0] / 4.0f, w = tri[c][1] / 4.0f, s = 1 - u - w;
						float p[3];
						for (int k = 0; k < 3; k++) p[k] = A[k] * s + B[k] * u + C[k] * w;
						norm3(p);
						memcpy(v->pos, p, sizeof p);
						memcpy(v->nrm, p, sizeof p);
						v->uv[0] = v->uv[1] = 0;
						memcpy(v->rgba, rgba, 4);
						v++;
					}
				}
	}
	GSPGPU_FlushDataCache(m->vbo, m->vertexCount * sizeof(ModelVertex));
}

void weaponDrawInit(void)
{
	static const u8 colour[BALL_COUNT][4] = {
		{ 255, 230, 80, 255 }, { 255, 140, 40, 255 }, { 80, 220, 255, 255 }, { 255, 150, 60, 110 }, { 255, 60, 60, 120 },
	};
	for (int k = 0; k < BALL_COUNT; k++)
		makeBall(&balls[k], colour[k], k >= BALL_BLAST);
}

void weaponDrawFini(void)
{
	for (int k = 0; k < BALL_COUNT; k++) {
		if (balls[k].vbo) linearFree(balls[k].vbo);
		free(balls[k].batches);
		memset(&balls[k], 0, sizeof balls[k]);
	}
}

static void drawBall(int kind, const float* at, float rx, float ry, const C3D_Mtx* projection, const C3D_Mtx* view)
{
	if (!balls[kind].vbo)
		return;
	C3D_Mtx mv = *view;
	Mtx_Translate(&mv, at[0], at[1], at[2], true);
	Mtx_Scale(&mv, rx, ry, rx);
	drawModelEx(&balls[kind], projection, view, NULL, &mv, -1);
}

static void drawExplosion(const Explosion* x, const C3D_Mtx* projection, const C3D_Mtx* view)
{
	for (int i = 0; i < 4; i++)
		if (x->burst[i].active && x->burst[i].radius > 0.01f)
			drawBall(BALL_BLAST, x->burst[i].pos, x->burst[i].radius, x->burst[i].radius * x->e->height, projection, view);
}

void arsenalDraw(const Arsenal* a, const C3D_Mtx* projection, const C3D_Mtx* view)
{
	for (int i = 0; i < MAX_BULLETS; i++) {
		const Bullet* b = &a->bullets[i];
		if (!b->state)
			continue;
		float r = b->pat->phase[b->phase].radius * 0.6f;
		if (r < 0.06f) r = 0.06f;
		if (!b->fx)   // the gun's own effect stands in for the ball
			drawBall(BALL_BULLET, b->pos, r, r, projection, view);
		// A long trail (the lasers) is drawn as a string of beads.
		int trail = b->pat->phase[b->phase].trail;
		for (int s = 4; s <= trail && s < b->age && s < BULLET_TRAIL; s += 4)
			drawBall(BALL_BULLET, b->trail[(b->age - s) % BULLET_TRAIL], r * 0.7f, r * 0.7f, projection, view);
	}
	for (int i = 0; i < 8; i++) {
		const Bomb* b = &a->bombs[i];
		if (b->state == 1 || b->state == 2) drawBall(BALL_BOMB, b->pos, 0.2f, 0.2f, projection, view);
		if (b->state == 3) drawExplosion(&b->x, projection, view);
	}
	for (int i = 0; i < 3; i++) {
		const Pod* q = &a->pods[i];
		if (q->state == 1) drawBall(BALL_POD, q->pos, 0.22f, 0.22f, projection, view);
		if (q->state == 2) drawExplosion(&q->x, projection, view);
	}
	if (a->aiming)
		drawBall(BALL_CURSOR, a->cursor, 0.5f, 0.08f, projection, view);
}
