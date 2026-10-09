#pragma once
#include <3ds.h>
#include <citro3d.h>

// Batch flags, matching tools/cook.py
enum { MODEL_LIT = 1, MODEL_BLEND = 2, MODEL_NO_ZWRITE = 4, MODEL_ALPHA_TEST = 8 };

typedef struct {
	float pos[3];
	float uv[2];
	float nrm[3];
	u8 rgba[4];
} ModelVertex;

typedef struct {
	int texture;    // -1: untextured
	u32 flags;
	u32 first;      // first vertex in the shared VBO
	u32 count;
} ModelBatch;

typedef struct {
	C3D_Tex* textures;
	u32 textureCount;
	ModelBatch* batches;
	u32 batchCount;
	ModelVertex* vbo;     // linear memory
	u32 vertexCount;
	float centre[3];
	float radius;
} Model;

// Loads a cooked .crm file. Returns false (with a message in err) on failure.
bool modelLoad(Model* m, const char* path, char* err, size_t errSize);
void modelFree(Model* m);
