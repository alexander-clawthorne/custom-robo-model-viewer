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
	int joint;      // CRM2: vertices are local to this joint; -1 = envelope (CPU skinned)
	ModelVertex* envSource;  // envelope batches: model-space source vertices
	u8 (*envJoint)[4];
	float (*envWeight)[4];
} ModelBatch;

typedef struct {
	s16 parent;
	u8 ik;          // 1 JOINT1, 2 JOINT2, 3 EFFECTOR
	float rot[3];   // radians
	float scale[3];
	float trans[3];
	float boneLength;
	float invBind[3][4];
} ModelJoint;

typedef struct {
	C3D_Tex* textures;
	u32 textureCount;
	ModelBatch* batches;
	u32 batchCount;
	ModelVertex* vbo;     // linear memory
	u32 vertexCount;
	float centre[3];
	float radius;
	// CRM2 (skinned robots)
	bool skinned;
	ModelJoint* joints;
	u32 jointCount;
	bool hasRig;
	s16 slots[19];        // joint index per rig slot
	float dims[32];       // the robot's limb dimensions
} Model;

// Loads a cooked .crm file. Returns false (with a message in err) on failure.
bool modelLoad(Model* m, const char* path, char* err, size_t errSize);
void modelFree(Model* m);
