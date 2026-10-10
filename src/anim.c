// Robot motion playback, ported from tools/pose.py, which follows the game's
// own code in main.dol: keyframes at 0x8001eb48, pose solve at 0x8005aee8.
#include "anim.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEG (3.14159265358979f / 180.0f)

// ------------------------------------------------------------------ matrices
static void identity(Mat34* a)
{
	memset(a, 0, sizeof *a);
	a->m[0][0] = a->m[1][1] = a->m[2][2] = 1.0f;
}

static Mat34 mul(const Mat34* a, const Mat34* b)
{
	Mat34 r;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 4; j++) {
			float v = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j];
			r.m[i][j] = j == 3 ? v + a->m[i][3] : v;
		}
	}
	return r;
}

static void mulIn(Mat34* a, const Mat34* b) { *a = mul(a, b); }

static Mat34 trans(float x, float y, float z)
{
	Mat34 a;
	identity(&a);
	a.m[0][3] = x; a.m[1][3] = y; a.m[2][3] = z;
	return a;
}

static Mat34 rotZ(float deg)
{
	Mat34 a; identity(&a);
	float c = cosf(deg * DEG), s = sinf(deg * DEG);
	a.m[0][0] = c; a.m[0][1] = -s; a.m[1][0] = s; a.m[1][1] = c;
	return a;
}

static Mat34 rotY(float deg)
{
	Mat34 a; identity(&a);
	float c = cosf(deg * DEG), s = sinf(deg * DEG);
	a.m[0][0] = c; a.m[0][2] = s; a.m[2][0] = -s; a.m[2][2] = c;
	return a;
}

static Mat34 rotX(float deg)
{
	Mat34 a; identity(&a);
	float c = cosf(deg * DEG), s = sinf(deg * DEG);
	a.m[1][1] = c; a.m[1][2] = -s; a.m[2][1] = s; a.m[2][2] = c;
	return a;
}

// Node rotation in degrees: Rz * Ry * Rx (0x80053da8).
static Mat34 nodeRot(const float* r)
{
	Mat34 a = rotZ(r[2]), b = rotY(r[1]), c = rotX(r[0]);
	mulIn(&a, &b);
	mulIn(&a, &c);
	return a;
}

// Turn the local X axis toward v (0x8005b2ac).
static Mat34 aim(const float* v)
{
	Mat34 a = rotZ(atan2f(v[1], v[0]) / DEG);
	Mat34 b = rotY(atan2f(-v[2], hypotf(v[0], v[1])) / DEG);
	mulIn(&a, &b);
	return a;
}

// p in the frame of G: R^T (p - t).
static void localOf(const Mat34* g, const float* p, float* out)
{
	float d[3] = { p[0] - g->m[0][3], p[1] - g->m[1][3], p[2] - g->m[2][3] };
	for (int i = 0; i < 3; i++)
		out[i] = g->m[0][i] * d[0] + g->m[1][i] * d[1] + g->m[2][i] * d[2];
}

static float len3(const float* v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static float clamp1(float x) { return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x); }

// Law-of-cosines angles (degrees) at the root joint and the middle joint.
static void bend(float a, float b, float d, float* angA, float* angB)
{
	if (d < 1e-6f) d = 1e-6f;
	*angA = acosf(clamp1((a * a + d * d - b * b) / (2 * a * d))) / DEG;
	*angB = acosf(clamp1((a * a + b * b - d * d) / (2 * a * b))) / DEG;
}

void mat34ToC3D(const Mat34* a, C3D_Mtx* out)
{
	for (int i = 0; i < 3; i++) {
		out->r[i].x = a->m[i][0]; out->r[i].y = a->m[i][1];
		out->r[i].z = a->m[i][2]; out->r[i].w = a->m[i][3];
	}
	out->r[3].x = out->r[3].y = out->r[3].z = 0.0f;
	out->r[3].w = 1.0f;
}

// ------------------------------------------------------------------ motions
bool motionSetLoad(MotionSet* set, const char* path)
{
	memset(set, 0, sizeof *set);
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* file = malloc(n);
	bool ok = file && fread(file, 1, n, f) == (size_t)n && n >= 92 && memcmp(file, "MOT1", 4) == 0;
	fclose(f);
	if (!ok) {
		free(file);
		return false;
	}
	u32 count;
	memcpy(&count, file + 4, 4);
	memcpy(set->defaults, file + 8, sizeof set->defaults);
	// First pass: count keys so they can live in one aligned pool.
	const u8* at = file + 92;
	size_t keys = 0;
	for (u32 i = 0; i < count; i++) {
		at += 36;
		for (int c = 0; c < 78; c++) {
			u16 k;
			memcpy(&k, at, 2);
			at += 2 + (size_t)k * 16;
			keys += k;
		}
	}
	set->motions = calloc(count, sizeof(Motion));
	set->keyPool = malloc(keys * sizeof(MotionKey) + 1);
	if (!set->motions || !set->keyPool) {
		free(file);
		motionSetFree(set);
		return false;
	}
	at = file + 92;
	MotionKey* pool = set->keyPool;
	for (u32 i = 0; i < count; i++) {
		Motion* m = &set->motions[i];
		memcpy(m->name, at, 32);
		m->name[31] = 0;
		u16 length;
		memcpy(&length, at + 32, 2);
		m->length = length;
		at += 36;
		for (int c = 0; c < 78; c++) {
			u16 k;
			memcpy(&k, at, 2);
			at += 2;
			m->tracks[c].count = k;
			m->tracks[c].keys = pool;
			memcpy(pool, at, (size_t)k * 16);  // unaligned source, aligned copy
			pool += k;
			at += (size_t)k * 16;
		}
	}
	set->count = count;
	free(file);
	return true;
}

void motionSetFree(MotionSet* set)
{
	free(set->motions);
	free(set->keyPool);
	memset(set, 0, sizeof *set);
}

int motionFind(const MotionSet* set, const char* name)
{
	for (int i = 0; i < set->count; i++)
		if (strcmp(set->motions[i].name, name) == 0)
			return i;
	return -1;
}

// Cubic Hermite between keys; tangents are per 30 frames (0x8001ee58).
static float evalTrack(const MotionTrack* t, float frame)
{
	if (!t->count)
		return 0.0f;
	const MotionKey* k = t->keys;
	int i = 0;
	while (i < t->count && frame > k[i].time)
		i++;
	if (i == t->count)
		return k[t->count - 1].value;
	if (i == 0 || k[i].time == frame)
		return k[i].value;
	float t0 = k[i - 1].time, t1 = k[i].time;
	float dt = (t1 - t0) / 30.0f, s = (frame - t0) / (t1 - t0), u = 1.0f - s;
	return k[i - 1].value * u * u * (1 + 2 * s) + k[i].value * s * s * (3 - 2 * s)
		+ dt * (k[i - 1].tanOut * s * u * u - k[i].tanIn * s * s * u);
}

// ------------------------------------------------------------------ solve
static const int ROT_NODES[16] = { 1, 2, 4, 6, 7, 9, 10, 11, 13, 14, 15, 17, 18, 19, 21, 22 };

// out[0] root, out[r + 1] rig slot r (19 slots).
static void solve(const float* ch, const float* D, const float* ds, Mat34* out)
{
	const float* node[26];
	for (int n = 0; n < 26; n++)
		node[n] = ch + 3 * n;
	Mat34 Mk[16];
	for (int k = 0; k < 16; k++)
		Mk[k] = nodeRot(node[ROT_NODES[k]]);
	const float* rootPos = node[0];
	float v[3];

	Mat34 rootT = trans(rootPos[0], rootPos[1], rootPos[2]);
	out[0] = mul(&rootT, &Mk[0]);
	Mat34 spine = mul(&out[0], &Mk[1]);
	localOf(&spine, node[3], v);
	Mat34 a = aim(v);
	mulIn(&spine, &a);
	out[1] = spine;

	// Pass 1 on the default skeleton: head direction, arm directions and bends.
	const float dsSpine = ds[0], dsNeck = ds[1];
	Mat34 h = spine, t;
	t = trans(dsSpine + dsNeck, 0, 0); mulIn(&h, &t);
	mulIn(&h, &Mk[2]);
	float headDir[3];
	localOf(&h, node[5], headDir);

	static const int armRot[2][3] = { { 3, 4, 5 }, { 6, 7, 8 } };
	static const int armTarget[2] = { 8, 12 };
	float armV[2][3], armA[2], armB[2];
	for (int side = 0; side < 2; side++) {
		const float* d = ds + 2 + 5 * side;  // base x, J1 x, J1 z, upper, fore
		Mat34 g = spine;
		t = trans(dsSpine + d[0], 0, 0); mulIn(&g, &t);
		mulIn(&g, &Mk[armRot[side][0]]);
		t = trans(d[1], 0, d[2]); mulIn(&g, &t);
		mulIn(&g, &Mk[armRot[side][1]]);
		localOf(&g, node[armTarget[side]], armV[side]);
		bend(d[3], d[4], len3(armV[side]), &armA[side], &armB[side]);
	}

	// Pass 2 on the robot's own dimensions.
	Mat34 chest = spine;
	t = trans(D[0], 0, 0); mulIn(&chest, &t);
	Mat34 head = chest;
	t = trans(D[1], 0, 0); mulIn(&head, &t);
	mulIn(&head, &Mk[2]);
	a = aim(headDir); mulIn(&head, &a);
	out[2] = head;
	for (int side = 0; side < 2; side++) {
		const float* d = D + 2 + 6 * side;
		Mat34 g = chest;
		t = trans(d[0], 0, 0); mulIn(&g, &t);
		mulIn(&g, &Mk[armRot[side][0]]);
		out[3 + 4 * side] = g;
		t = trans(d[1], 0, d[2]); mulIn(&g, &t);
		mulIn(&g, &Mk[armRot[side][1]]);
		a = aim(armV[side]); mulIn(&g, &a);
		t = rotZ(-armA[side]); mulIn(&g, &t);
		out[4 + 4 * side] = g;
		t = trans(d[3], 0, 0); mulIn(&g, &t);
		t = rotZ(180.0f - armB[side]); mulIn(&g, &t);
		out[5 + 4 * side] = g;
		t = trans(d[4], 0, d[5]); mulIn(&g, &t);
		mulIn(&g, &Mk[armRot[side][2]]);
		out[6 + 4 * side] = g;
	}

	Mat34 hips = mul(&rootT, &Mk[0]);
	mulIn(&hips, &Mk[9]);
	t = rotZ(-90.0f); mulIn(&hips, &t);
	out[11] = hips;
	static const int legRot[2][3] = { { 10, 11, 12 }, { 13, 14, 15 } };
	static const int legTarget[2] = { 16, 20 };
	for (int side = 0; side < 2; side++) {
		const float* dl = ds + 13 + 4 * side;  // J1 x, J1 z, thigh, shin
		Mat34 g = hips;
		t = trans(ds[12] + dl[0], 0, dl[1]); mulIn(&g, &t);
		mulIn(&g, &Mk[legRot[side][0]]);
		localOf(&g, node[legTarget[side]], v);
		float A, B;
		bend(dl[2], dl[3], len3(v), &A, &B);

		const float* d = D + 15 + 6 * side;
		g = hips;
		t = trans(D[14] + d[0], 0, d[1]); mulIn(&g, &t);
		mulIn(&g, &Mk[legRot[side][0]]);
		a = aim(v); mulIn(&g, &a);
		t = rotZ(A); mulIn(&g, &t);
		out[12 + 4 * side] = g;
		t = trans(d[2], 0, 0); mulIn(&g, &t);
		t = rotZ(B - 180.0f); mulIn(&g, &t);
		out[13 + 4 * side] = g;
		t = trans(d[3], 0, 0); mulIn(&g, &t);
		// Foot orientation is absolute, not chained down the leg (0x80059cc0).
		const Mat34* foot = &Mk[legRot[side][1]];
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				g.m[i][j] = foot->m[i][j];
		out[14 + 4 * side] = g;
		t = trans(0, d[4], d[5]); mulIn(&g, &t);
		mulIn(&g, &Mk[legRot[side][2]]);
		out[15 + 4 * side] = g;
	}
}

// ------------------------------------------------------------------ pose
static Mat34 jointLocal(const ModelJoint* j)
{
	Mat34 m = trans(j->trans[0], j->trans[1], j->trans[2]);
	Mat34 r = rotZ(j->rot[2] / DEG), ry = rotY(j->rot[1] / DEG), rx = rotX(j->rot[0] / DEG);
	mulIn(&r, &ry);
	mulIn(&r, &rx);
	mulIn(&m, &r);
	for (int i = 0; i < 3; i++)
		for (int k = 0; k < 3; k++)
			m.m[i][k] *= j->scale[k];
	return m;
}

void animPose(const Model* model, const MotionSet* set, const Motion* m, float frame, Mat34* world)
{
	Mat34 out[20];
	bool solved = m && model->hasRig && set;
	if (solved) {
		float ch[78];
		for (int c = 0; c < 78; c++)
			ch[c] = evalTrack(&m->tracks[c], frame);
		// Play in place: remove the root's horizontal travel from every position node.
		static const int posNodes[7] = { 0, 3, 5, 8, 12, 16, 20 };
		float rx = ch[0], rz = ch[2];
		for (int i = 0; i < 7; i++) {
			ch[3 * posNodes[i]] -= rx;
			ch[3 * posNodes[i] + 2] -= rz;
		}
		solve(ch, model->dims, set->defaults, out);
	}
	for (u32 i = 0; i < model->jointCount; i++) {  // parents come first
		const ModelJoint* j = &model->joints[i];
		int slot = -1;
		if (solved)
			for (int r = 0; r < 19; r++)
				if (model->slots[r] == (s16)i)
					slot = r;
		if (slot >= 0) {
			world[i] = out[slot + 1];
			continue;
		}
		Mat34 local = jointLocal(j);
		if (j->parent >= 0)
			world[i] = mul(&world[j->parent], &local);
		else
			world[i] = local;
		if (j->ik == 3 && j->parent >= 0 && model->joints[j->parent].boneLength > 0.0f) {
			// Effector: bone length along the parent's X axis (HSD jobj.c).
			const Mat34* p = &world[j->parent];
			float x[3] = { p->m[0][0], p->m[1][0], p->m[2][0] };
			float l = len3(x);
			if (l > 0.0f)
				for (int k = 0; k < 3; k++)
					world[i].m[k][3] = p->m[k][3] + x[k] / l * model->joints[j->parent].boneLength;
		}
	}
}

void animSkin(Model* model, const Mat34* world)
{
	// Skin matrices: world * inverse bind, per joint.
	static Mat34 skin[96];
	u32 n = model->jointCount < 96 ? model->jointCount : 96;
	for (u32 i = 0; i < n; i++) {
		Mat34 inv;
		memcpy(inv.m, model->joints[i].invBind, sizeof inv.m);
		skin[i] = mul(&world[i], &inv);
	}
	for (u32 b = 0; b < model->batchCount; b++) {
		ModelBatch* batch = &model->batches[b];
		if (batch->joint >= 0 || !batch->envSource)
			continue;
		for (u32 v = 0; v < batch->count; v++) {
			const ModelVertex* src = &batch->envSource[v];
			ModelVertex* dst = &model->vbo[batch->first + v];
			float p[3] = { 0 }, nrm[3] = { 0 };
			for (int k = 0; k < 4; k++) {
				float w = batch->envWeight[v][k];
				if (w == 0.0f)
					continue;
				const Mat34* s = &skin[batch->envJoint[v][k] < n ? batch->envJoint[v][k] : 0];
				for (int r = 0; r < 3; r++) {
					p[r] += w * (s->m[r][0] * src->pos[0] + s->m[r][1] * src->pos[1] + s->m[r][2] * src->pos[2] + s->m[r][3]);
					nrm[r] += w * (s->m[r][0] * src->nrm[0] + s->m[r][1] * src->nrm[1] + s->m[r][2] * src->nrm[2]);
				}
			}
			memcpy(dst->pos, p, sizeof p);
			memcpy(dst->nrm, nrm, sizeof nrm);
		}
		GSPGPU_FlushDataCache(&model->vbo[batch->first], batch->count * sizeof(ModelVertex));
	}
}
