// Effect animation playback for CRM4 models driven by EFX1 tracks.
#include "effects.h"
#include "render.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static EfxInstance pool[EFX_POOL];

// ------------------------------------------------------------------ math

static Mat34 efxMul(const Mat34* a, const Mat34* b)
{
	Mat34 r;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 4; j++) {
			float v = a->m[i][0]*b->m[0][j] + a->m[i][1]*b->m[1][j] + a->m[i][2]*b->m[2][j];
			r.m[i][j] = j == 3 ? v + a->m[i][3] : v;
		}
	}
	return r;
}

// Rotation matrix from radians: Rz * Ry * Rx (same convention as HSD JObj).
static Mat34 efxRot(float rx, float ry, float rz)
{
	float cx = cosf(rx), sx = sinf(rx);
	float cy = cosf(ry), sy = sinf(ry);
	float cz = cosf(rz), sz = sinf(rz);
	Mat34 a;
	memset(&a, 0, sizeof a);
	// Rz * Ry * Rx
	a.m[0][0] =  cy*cz; a.m[0][1] = -cy*sz; a.m[0][2] =  sy;
	a.m[1][0] =  sx*sy*cz + cx*sz; a.m[1][1] = -sx*sy*sz + cx*cz; a.m[1][2] = -sx*cy;
	a.m[2][0] = -cx*sy*cz + sx*sz; a.m[2][1] =  cx*sy*sz + sx*cz; a.m[2][2] =  cx*cy;
	return a;
}

// ------------------------------------------------------------------ track eval

// Cubic Hermite without /30 scaling: tangents are per-frame (raw HSD FObj).
static float efxEvalTrack(const EfxKey* keys, u16 count, float frame)
{
	if (!count)
		return 0.0f;
	if (frame <= keys[0].frame)
		return keys[0].value;
	for (int i = 0; i < (int)count - 1; i++) {
		float t0 = keys[i].frame, t1 = keys[i+1].frame;
		if (frame >= t0 && frame < t1) {
			float span = t1 - t0;
			if (span <= 0.0f)
				return keys[i].value;
			float u = frame - t0;
			float s = u / span;
			float u2 = s * s, u3 = u2 * s;
			float h00 = 2*u3 - 3*u2 + 1, h10 = u3 - 2*u2 + s;
			float h01 = -2*u3 + 3*u2,    h11 = u3 - u2;
			return (h00 * keys[i].value + h01 * keys[i+1].value
				+ h10 * keys[i].tanOut * span
				+ h11 * keys[i+1].tanIn * span);
		}
	}
	return keys[count - 1].value;
}

// ------------------------------------------------------------------ world matrices

static void efxWorldMats(const Model* m, const EfxAnim* anim, float frame, const float* pos, Mat34* world)
{
	u32 n = m->jointCount;

	// Build local SRT for each joint, overridden by animation tracks.
	float rot[3], scale[3], tra[3];
	for (u32 i = 0; i < n; i++) {
		const ModelJoint* j = &m->joints[i];
		memcpy(rot,   j->rot,   12);
		memcpy(scale, j->scale, 12);
		memcpy(tra,   j->trans, 12);

		// Apply animation tracks for this joint.
		if (anim) {
			for (u32 t = 0; t < anim->jointTrackCount; t++) {
				const EfxTrack* tr = &anim->tracks[t];
				if (tr->index != (s16)i)
					continue;
				float v = efxEvalTrack(tr->keys, tr->count, frame);
				switch (tr->channel) {
					case 1: rot[0]   = v; break;
					case 2: rot[1]   = v; break;
					case 3: rot[2]   = v; break;
					case 5: tra[0]   = v; break;
					case 6: tra[1]   = v; break;
					case 7: tra[2]   = v; break;
					case 8: scale[0] = v; break;
					case 9: scale[1] = v; break;
					case 10: scale[2] = v; break;
				}
			}
		}

		// local = T * R * S
		Mat34 R = efxRot(rot[0], rot[1], rot[2]);
		// Apply scale to R columns
		for (int r = 0; r < 3; r++) {
			R.m[r][0] *= scale[0];
			R.m[r][1] *= scale[1];
			R.m[r][2] *= scale[2];
			R.m[r][3]  = tra[r];
		}
		// world[i] = parent * local; root also shifted by spawn position
		if (j->parent < 0) {
			R.m[0][3] += pos[0];
			R.m[1][3] += pos[1];
			R.m[2][3] += pos[2];
			world[i] = R;
		} else {
			world[i] = efxMul(&world[j->parent], &R);
		}
	}
}

// ------------------------------------------------------------------ load / free

bool efxSetLoad(EfxSet* s, const char* path)
{
	memset(s, 0, sizeof *s);
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* buf = malloc(sz);
	if (!buf || (long)fread(buf, 1, sz, f) != sz) { free(buf); fclose(f); return false; }
	fclose(f);
	if (sz < 8 || memcmp(buf, "EFX1", 4) != 0) { free(buf); return false; }
	u32 count;
	memcpy(&count, buf + 4, 4);
	if ((long)(8 + count * 4) > sz) { free(buf); return false; }

	EfxAnim* anims = calloc(count, sizeof(EfxAnim));
	if (!anims) { free(buf); return false; }

	for (u32 i = 0; i < count; i++) {
		u32 off;
		memcpy(&off, buf + 8 + i * 4, 4);
		if ((long)(off + 12) > sz) continue;
		EfxAnim* a = &anims[i];
		memcpy(&a->duration,        buf + off,     4);
		memcpy(&a->jointTrackCount, buf + off + 4, 4);
		memcpy(&a->matTrackCount,   buf + off + 8, 4);
		u32 total = a->jointTrackCount + a->matTrackCount;
		if (!total) continue;
		a->tracks = malloc(total * sizeof(EfxTrack));
		if (!a->tracks) continue;
		u32 at = off + 12;
		for (u32 t = 0; t < total; t++) {
			if ((long)(at + 8) > sz) break;
			EfxTrack* tr = &a->tracks[t];
			memcpy(&tr->index,   buf + at,     2);
			tr->channel = buf[at + 2];
			tr->_pad    = 0;
			memcpy(&tr->count,   buf + at + 4, 2);
			at += 8;
			if (!tr->count) { tr->keys = NULL; continue; }
			long need = (long)tr->count * 16;
			if (at + need > sz) { tr->count = 0; tr->keys = NULL; break; }
			tr->keys = malloc(need);
			if (!tr->keys) { tr->count = 0; break; }
			memcpy(tr->keys, buf + at, need);
			at += need;
		}
	}

	s->count = count;
	s->anims = anims;
	s->_buf  = buf;
	return true;
}

void efxSetFree(EfxSet* s)
{
	if (!s->anims) return;
	for (u32 i = 0; i < s->count; i++) {
		EfxAnim* a = &s->anims[i];
		if (a->tracks) {
			u32 total = a->jointTrackCount + a->matTrackCount;
			for (u32 t = 0; t < total; t++)
				free(a->tracks[t].keys);
			free(a->tracks);
		}
	}
	free(s->anims);
	free(s->_buf);
	memset(s, 0, sizeof *s);
}

// ------------------------------------------------------------------ spawn / step / draw

int efxSpawn(EfxSet* set, int modelIndex, Model* model, const float pos[3])
{
	for (int i = 0; i < EFX_POOL; i++) {
		if (pool[i].active) continue;
		EfxInstance* e = &pool[i];
		e->active     = true;
		e->modelIndex = modelIndex;
		e->set        = set;
		e->frame      = 0.0f;
		e->model      = model;
		memcpy(e->pos, pos, 12);
		if (model->jointCount > 0) {
			e->world = malloc(model->jointCount * sizeof(Mat34));
			if (!e->world) { e->active = false; return -1; }
		} else {
			e->world = NULL;
		}
		return i;
	}
	return -1;
}

// Ends every live effect; call before freeing a model or set they use.
void efxClearAll(void)
{
	for (int i = 0; i < EFX_POOL; i++) {
		free(pool[i].world);
		pool[i].world  = NULL;
		pool[i].active = false;
	}
}

void efxStepAll(void)
{
	for (int i = 0; i < EFX_POOL; i++) {
		EfxInstance* e = &pool[i];
		if (!e->active) continue;
		EfxAnim* anim = (e->set && e->modelIndex >= 0 &&
		                 (u32)e->modelIndex < e->set->count)
		                ? &e->set->anims[e->modelIndex] : NULL;
		e->frame += 1.0f;
		if (anim && anim->duration > 0.0f && e->frame > anim->duration) {
			e->active = false;
			free(e->world);
			e->world = NULL;
		}
	}
}

void efxDrawAll(const C3D_Mtx* proj, const C3D_Mtx* view)
{
	for (int i = 0; i < EFX_POOL; i++) {
		EfxInstance* e = &pool[i];
		if (!e->active || !e->model) continue;
		EfxAnim* anim = (e->set && e->modelIndex >= 0 &&
		                 (u32)e->modelIndex < e->set->count)
		                ? &e->set->anims[e->modelIndex] : NULL;

		// Compute per-joint world matrices in effect-local space.
		if (e->world && e->model->jointCount > 0)
			efxWorldMats(e->model, anim, e->frame, e->pos, e->world);

		drawModel(e->model, proj, view, e->world);
	}
}
