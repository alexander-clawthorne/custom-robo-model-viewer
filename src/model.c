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
	if (memcmp(at, "CRM1", 4) != 0) {
		snprintf(err, errSize, "not a CRM1 file");
		goto fail;
	}
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

	// First pass: count vertices; second: copy into one linear VBO.
	const u8* batchStart = at;
	u32 total = 0;
	for (u32 i = 0; i < m->batchCount; i++) {
		NEED(12);
		u32 count;
		memcpy(&count, at + 8, 4);
		at += 12;
		NEED((size_t)count * sizeof(ModelVertex));
		at += (size_t)count * sizeof(ModelVertex);
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
		memcpy(&b->count, at + 8, 4);
		at += 12;
		b->first = first;
		memcpy(m->vbo + first, at, b->count * sizeof(ModelVertex));
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
	free(m->batches);
	if (m->vbo)
		linearFree(m->vbo);
	memset(m, 0, sizeof(*m));
}
