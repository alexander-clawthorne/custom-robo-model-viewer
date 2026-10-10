// Loader for the cooked .crm models written by tools/cook.py.
#include "model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8* readFile(const char* path, size_t* size)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* buf = n > 0 ? malloc(n) : NULL;
	if (buf && fread(buf, 1, n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	*size = buf ? (size_t)n : 0;
	return buf;
}

#define NEED(n) do { if (at + (n) > end) { snprintf(err, errSize, "truncated file"); goto fail; } } while (0)

bool modelLoad(Model* m, const char* path, char* err, size_t errSize)
{
	memset(m, 0, sizeof(*m));
	size_t size;
	u8* file = readFile(path, &size);
	if (!file) {
		snprintf(err, errSize, "cannot read %s", path);
		return false;
	}
	const u8* at = file;
	const u8* end = file + size;
	u32 header[2];
	NEED(28);
	if (memcmp(at, "CRM1", 4) != 0 && memcmp(at, "CRM2", 4) != 0) {
		snprintf(err, errSize, "not a CRM file");
		goto fail;
	}
	m->skinned = at[3] == '2';
	memcpy(header, at + 4, 8);
	memcpy(m->centre, at + 12, 12);
	memcpy(&m->radius, at + 24, 4);
	at += 28;
	m->textureCount = header[0];
	m->batchCount = header[1];
	m->textures = calloc(m->textureCount ? m->textureCount : 1, sizeof(C3D_Tex));
	m->batches = calloc(m->batchCount ? m->batchCount : 1, sizeof(ModelBatch));

	for (u32 i = 0; i < m->textureCount; i++) {
		NEED(12);
		u16 w, h;
		u32 bytes;
		memcpy(&w, at, 2);
		memcpy(&h, at + 2, 2);
		u8 fmt = at[4], wrapS = at[5], wrapT = at[6];
		memcpy(&bytes, at + 8, 4);
		at += 12;
		NEED(bytes);
		C3D_Tex* t = &m->textures[i];
		if (!C3D_TexInit(t, w, h, (GPU_TEXCOLOR)fmt)) {
			snprintf(err, errSize, "texture %lu: no memory (%ux%u)", i, w, h);
			goto fail;
		}
		memcpy(t->data, at, bytes < t->size ? bytes : t->size);
		C3D_TexFlush(t);
		C3D_TexSetFilter(t, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(t, (GPU_TEXTURE_WRAP_PARAM)wrapS, (GPU_TEXTURE_WRAP_PARAM)wrapT);
		at += bytes;
	}

	if (m->skinned) {
		// Skeleton: joints, rig slots, dimensions (tools/cook.py skeleton_block).
		NEED(4);
		memcpy(&m->jointCount, at, 4);
		at += 4;
		m->joints = calloc(m->jointCount ? m->jointCount : 1, sizeof(ModelJoint));
		for (u32 i = 0; i < m->jointCount; i++) {
			NEED(92);
			ModelJoint* j = &m->joints[i];
			memcpy(&j->parent, at, 2);
			j->ik = at[2];
			memcpy(j->rot, at + 4, 12);
			memcpy(j->scale, at + 16, 12);
			memcpy(j->trans, at + 28, 12);
			memcpy(&j->boneLength, at + 40, 4);
			memcpy(j->invBind, at + 44, 48);
			at += 92;
		}
		NEED(40 + 128);
		memcpy(m->slots, at, sizeof m->slots);
		memcpy(m->dims, at + 40, sizeof m->dims);
		at += 40 + 128;
		m->hasRig = true;
		for (int r = 0; r < 19; r++)
			if (m->slots[r] < 0 || (u32)m->slots[r] >= m->jointCount)
				m->hasRig = false;
	}
	const size_t head = m->skinned ? 16 : 12;

	// First pass: count vertices; second: copy into one linear VBO.
	const u8* batchStart = at;
	u32 total = 0;
	for (u32 i = 0; i < m->batchCount; i++) {
		NEED(head);
		u32 count;
		int joint = 0;
		memcpy(&count, at + head - 4, 4);
		if (m->skinned)
			memcpy(&joint, at + 8, 4);
		at += head;
		NEED((size_t)count * sizeof(ModelVertex));
		at += (size_t)count * sizeof(ModelVertex);
		if (m->skinned && joint < 0) {
			NEED((size_t)count * 20);
			at += (size_t)count * 20;
		}
		total += count;
	}
	m->vbo = linearAlloc(total ? total * sizeof(ModelVertex) : sizeof(ModelVertex));
	if (!m->vbo) {
		snprintf(err, errSize, "no linear memory for %lu vertices", total);
		goto fail;
	}
	at = batchStart;
	u32 first = 0;
	for (u32 i = 0; i < m->batchCount; i++) {
		ModelBatch* b = &m->batches[i];
		memcpy(&b->texture, at, 4);
		memcpy(&b->flags, at + 4, 4);
		memcpy(&b->count, at + head - 4, 4);
		b->joint = -1;
		if (m->skinned)
			memcpy(&b->joint, at + 8, 4);
		at += head;
		b->first = first;
		memcpy(m->vbo + first, at, b->count * sizeof(ModelVertex));
		if (m->skinned && b->joint < 0) {
			// Envelope batch: keep the model-space source and the weights for CPU skinning.
			b->envSource = malloc(b->count * sizeof(ModelVertex));
			b->envJoint = malloc(b->count * sizeof *b->envJoint);
			b->envWeight = malloc(b->count * sizeof *b->envWeight);
			if (!b->envSource || !b->envJoint || !b->envWeight) {
				snprintf(err, errSize, "no memory for skinning");
				goto fail;
			}
			memcpy(b->envSource, at, b->count * sizeof(ModelVertex));
			const u8* w = at + b->count * sizeof(ModelVertex);
			for (u32 v = 0; v < b->count; v++, w += 20) {
				memcpy(b->envJoint[v], w, 4);
				memcpy(b->envWeight[v], w + 4, 16);
			}
			at += b->count * 20;
		}
		if (!m->skinned)
			b->joint = -1;
		at += b->count * sizeof(ModelVertex);
		first += b->count;
		if (b->texture >= (int)m->textureCount)
			b->texture = -1;
	}
	m->vertexCount = total;
	GSPGPU_FlushDataCache(m->vbo, total * sizeof(ModelVertex));
	free(file);
	return true;

fail:
	free(file);
	modelFree(m);
	return false;
}

void modelFree(Model* m)
{
	if (m->textures) {
		for (u32 i = 0; i < m->textureCount; i++)
			if (m->textures[i].data)
				C3D_TexDelete(&m->textures[i]);
		free(m->textures);
	}
	if (m->batches) {
		for (u32 i = 0; i < m->batchCount; i++) {
			free(m->batches[i].envSource);
			free(m->batches[i].envJoint);
			free(m->batches[i].envWeight);
		}
	}
	free(m->batches);
	free(m->joints);
	if (m->vbo)
		linearFree(m->vbo);
	memset(m, 0, sizeof(*m));
}
