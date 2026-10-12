// Sphere against the arena's collision faces (tools/collision.py has the
// format). The game sweeps a sphere along the move and stops at the first hit
// (0x8005d08c, faces at 0x8005e270); this resolves overlap after each small
// step instead, which gives the same contacts as long as a step is shorter
// than the radius -- the caller sub-steps fast moves.
#include "collide.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLOOR_NY 0.6f   // a push this close to straight up counts as standing

bool collisionLoad(Collision* c, const char* path)
{
	memset(c, 0, sizeof *c);
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	char magic[4];
	u32 count;
	if (fread(magic, 4, 1, f) != 1 || memcmp(magic, "COL1", 4) || fread(&count, 4, 1, f) != 1) {
		fclose(f);
		return false;
	}
	c->blocks = calloc(count, sizeof(ColBlock));
	c->blockCount = count;
	for (int a = 0; a < 3; a++) { c->lo[a] = 1e9f; c->hi[a] = -1e9f; }
	c->top = -1e9f;
	bool ok = true;
	for (u32 i = 0; i < count && ok; i++) {
		ColBlock* b = &c->blocks[i];
		float hdr[4];
		u32 ints[3];
		ok = fread(hdr, 4, 4, f) == 4 && fread(ints, 4, 3, f) == 3 && ints[2] >= 3 && ints[2] <= 4;
		if (!ok)
			break;
		memcpy(b->centre, hdr, 12);
		b->radius = hdr[3];
		b->flags = ints[0];
		b->faceCount = ints[1];
		b->nverts = ints[2];
		b->faces = calloc(b->faceCount, sizeof(ColFace));
		for (u32 k = 0; k < b->faceCount && ok; k++) {
			ColFace* fc = &b->faces[k];
			float buf[4 * 3 + 4];
			ok = fread(buf, 4, b->nverts * 3 + 4, f) == b->nverts * 3 + 4 && fread(&fc->kind, 4, 1, f) == 1;
			memcpy(fc->v, buf, b->nverts * 12);
			memcpy(fc->n, buf + b->nverts * 3, 12);
			fc->d = buf[b->nverts * 3 + 3];
			if (fc->n[1] > FLOOR_NY && !(b->flags & 0x80))
				for (u32 j = 0; j < b->nverts; j++)
					if (fc->v[j][1] > c->top) c->top = fc->v[j][1];
			if (i == 0 && fc->n[1] > FLOOR_NY)
				for (u32 j = 0; j < b->nverts; j++)
					for (int a = 0; a < 3; a++) {
						if (fc->v[j][a] < c->lo[a]) c->lo[a] = fc->v[j][a];
						if (fc->v[j][a] > c->hi[a]) c->hi[a] = fc->v[j][a];
					}
		}
	}
	fclose(f);
	if (!ok)
		collisionFree(c);
	return ok;
}

void collisionFree(Collision* c)
{
	for (u32 i = 0; i < c->blockCount; i++)
		free(c->blocks[i].faces);
	free(c->blocks);
	memset(c, 0, sizeof *c);
}

static float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void closestOnSegment(const float* a, const float* b, const float* p, float* out)
{
	float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	float ap[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
	float len2 = dot3(ab, ab);
	float t = len2 > 1e-9f ? dot3(ap, ab) / len2 : 0.0f;
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	for (int k = 0; k < 3; k++)
		out[k] = a[k] + ab[k] * t;
}

// Closest point of a convex face to p; the inside test is winding-agnostic
// (every edge must put the point on the same side).
static void closestOnFace(const ColFace* f, u32 n, const float* p, float* out)
{
	float s = dot3(f->n, p) + f->d;
	float q[3] = { p[0] - f->n[0] * s, p[1] - f->n[1] * s, p[2] - f->n[2] * s };
	int pos = 0, neg = 0;
	for (u32 i = 0; i < n; i++) {
		const float* a = f->v[i];
		const float* b = f->v[(i + 1) % n];
		float e[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
		float w[3] = { q[0] - a[0], q[1] - a[1], q[2] - a[2] };
		float c[3] = { e[1] * w[2] - e[2] * w[1], e[2] * w[0] - e[0] * w[2], e[0] * w[1] - e[1] * w[0] };
		float side = dot3(c, f->n);
		if (side > 1e-6f) pos++;
		else if (side < -1e-6f) neg++;
	}
	memcpy(out, q, sizeof q);
	if (!pos || !neg)
		return;
	float best = 1e30f;
	for (u32 i = 0; i < n; i++) {
		float c[3];
		closestOnSegment(f->v[i], f->v[(i + 1) % n], p, c);
		float d[3] = { p[0] - c[0], p[1] - c[1], p[2] - c[2] };
		float d2 = dot3(d, d);
		if (d2 < best) { best = d2; memcpy(out, c, sizeof c); }
	}
}

bool collideSphere(const Collision* c, float p[3], float radius, bool* ground, float* groundNormal)
{
	bool hit = false;
	*ground = false;
	for (int pass = 0; pass < 3; pass++) {
		bool moved = false;
		for (u32 i = 0; i < c->blockCount; i++) {
			const ColBlock* b = &c->blocks[i];
			if (b->flags & 0x80)
				continue;
			float to[3] = { p[0] - b->centre[0], p[1] - b->centre[1], p[2] - b->centre[2] };
			float reach = b->radius + radius;
			if (dot3(to, to) > reach * reach)
				continue;
			for (u32 k = 0; k < b->faceCount; k++) {
				const ColFace* f = &b->faces[k];
				float s = dot3(f->n, p) + f->d;
				// One-sided: only what is in front of the face, or has just
				// slipped behind it this step, is pushed back out.
				if (s >= radius || s < -radius)
					continue;
				float q[3];
				closestOnFace(f, b->nverts, p, q);
				float d[3] = { p[0] - q[0], p[1] - q[1], p[2] - q[2] };
				float dist = sqrtf(dot3(d, d));
				float push[3];
				float depth;
				if (dist < 1e-5f || s < 0.0f) {
					if (s < 0.0f && dist > fabsf(s) + 1e-3f)
						continue;  // behind the plane and off the face: not ours
					memcpy(push, f->n, sizeof push);
					depth = radius - s;
				} else {
					if (dist >= radius)
						continue;
					for (int a = 0; a < 3; a++) push[a] = d[a] / dist;
					depth = radius - dist;
				}
				for (int a = 0; a < 3; a++)
					p[a] += push[a] * depth;
				if (push[1] > FLOOR_NY) {
					*ground = true;
					if (groundNormal) memcpy(groundNormal, push, sizeof push);
				}
				hit = moved = true;
			}
		}
		if (!moved)
			break;
	}
	return hit;
}

// The face's plane is crossed somewhere inside the face (winding-agnostic, as
// closestOnFace).
static bool insideFace(const ColFace* f, u32 n, const float* q)
{
	int pos = 0, neg = 0;
	for (u32 i = 0; i < n; i++) {
		const float* a = f->v[i];
		const float* b = f->v[(i + 1) % n];
		float e[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
		float w[3] = { q[0] - a[0], q[1] - a[1], q[2] - a[2] };
		float c[3] = { e[1] * w[2] - e[2] * w[1], e[2] * w[0] - e[0] * w[2], e[0] * w[1] - e[1] * w[0] };
		float side = dot3(c, f->n);
		if (side > 1e-6f) pos++;
		else if (side < -1e-6f) neg++;
	}
	return !pos || !neg;
}

bool collideSegment(const Collision* c, const float a[3], const float b[3], float hit[3], float normal[3])
{
	float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	float mid[3] = { (a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f, (a[2] + b[2]) * 0.5f };
	float half = 0.5f * sqrtf(dot3(d, d));
	float best = 2.0f;
	for (u32 i = 0; i < c->blockCount; i++) {
		const ColBlock* bl = &c->blocks[i];
		if (bl->flags & 0xc0)
			continue;
		float to[3] = { mid[0] - bl->centre[0], mid[1] - bl->centre[1], mid[2] - bl->centre[2] };
		float reach = bl->radius + half;
		if (dot3(to, to) > reach * reach)
			continue;
		for (u32 k = 0; k < bl->faceCount; k++) {
			const ColFace* f = &bl->faces[k];
			float sa = dot3(f->n, a) + f->d, sb = dot3(f->n, b) + f->d;
			if (sa < 0.0f || sb >= 0.0f)
				continue;   // one-sided: only crossing from the front counts
			float t = sa / (sa - sb);
			if (t >= best)
				continue;
			float q[3] = { a[0] + d[0] * t, a[1] + d[1] * t, a[2] + d[2] * t };
			if (!insideFace(f, bl->nverts, q))
				continue;
			best = t;
			memcpy(hit, q, sizeof q);
			memcpy(normal, f->n, 3 * sizeof(float));
		}
	}
	return best <= 1.0f;
}

float floorBelow(const Collision* c, float x, float y, float z)
{
	return floorKindBelow(c, x, y, z, NULL);
}

float floorKindBelow(const Collision* c, float x, float y, float z, u32* kind)
{
	float best = -1e9f;
	if (kind)
		*kind = 0;
	for (u32 i = 0; i < c->blockCount; i++) {
		const ColBlock* bl = &c->blocks[i];
		if (bl->flags & 0x80)
			continue;
		float dx = x - bl->centre[0], dz = z - bl->centre[2];
		if (dx * dx + dz * dz > bl->radius * bl->radius)
			continue;
		for (u32 k = 0; k < bl->faceCount; k++) {
			const ColFace* f = &bl->faces[k];
			if (f->n[1] < FLOOR_NY)
				continue;
			// Height of the face's plane under (x, z).
			float h = -(f->n[0] * x + f->n[2] * z + f->d) / f->n[1];
			float q[3] = { x, h, z };
			if (h <= y && h > best && insideFace(f, bl->nverts, q)) {
				best = h;
				if (kind)
					*kind = f->kind;
			}
		}
	}
	return best;
}
