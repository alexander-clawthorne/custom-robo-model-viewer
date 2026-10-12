// Drawing shared by the model viewer and the battle mode.
#include "render.h"
#include "vshader_shbin.h"

const char* const partFolder[PART_COUNT] = { "gun", "bomb", "pod", "leg" };
const PartAttach attach[] = {
	{ PART_GUN,  0, 8 },   // out[9]  arm2 elbow: the gun is the forearm
	{ PART_BOMB, 1, 4 },   // out[5]  arm1 elbow
	{ PART_POD,  0, 0 },   // out[1]  spine, one pod each side
	{ PART_POD,  1, 0 },
	{ PART_LEG,  5, 12 },  // out[13] leg1 knee
	{ PART_LEG,  3, 13 },  // out[14] leg1 foot
	{ PART_LEG,  4, 16 },  // out[17] leg2 knee
	{ PART_LEG,  1, 17 },  // out[18] leg2 foot
};
const int attachCount = sizeof attach / sizeof attach[0];

static DVLB_s* vshader;
static shaderProgram_s program;
static int uProjection, uModelView, uLightVec, uLighting;

void renderInit(void)
{
	vshader = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &vshader->DVLE[0]);
	C3D_BindProgram(&program);
	uProjection = shaderInstanceGetUniformLocation(program.vertexShader, "projection");
	uModelView  = shaderInstanceGetUniformLocation(program.vertexShader, "modelView");
	uLightVec   = shaderInstanceGetUniformLocation(program.vertexShader, "lightVec");
	uLighting   = shaderInstanceGetUniformLocation(program.vertexShader, "lighting");

	C3D_AttrInfo* attr = C3D_GetAttrInfo();
	AttrInfo_Init(attr);
	AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);         // position
	AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);         // texcoord
	AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 3);         // normal
	AttrInfo_AddLoader(attr, 3, GPU_UNSIGNED_BYTE, 4); // colour (0-255)
	C3D_CullFace(GPU_CULL_NONE);
}

void renderFini(void)
{
	shaderProgramFree(&program);
	DVLB_Free(vshader);
}

static void drawBatches(const Model* m, const C3D_Mtx* projection, const C3D_Mtx* view,
	const Mat34* world, const C3D_Mtx* fixed, int onlyRoot, int jointLo, int jointHi)
{
	const C3D_Mtx* modelView = view;
	C3D_BufInfo* buf = C3D_GetBufInfo();
	BufInfo_Init(buf);
	BufInfo_Add(buf, m->vbo, sizeof(ModelVertex), 4, 0x3210);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uProjection, projection);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, modelView);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLightVec, 0.35f, 0.6f, 0.72f, 0.0f);

	for (u32 i = 0; i < m->batchCount; i++) {
		const ModelBatch* b = &m->batches[i];
		bool lit = b->flags & MODEL_LIT;
		if (onlyRoot >= 0 && b->root != onlyRoot)
			continue;
		if (jointHi > jointLo && (b->joint < jointLo || b->joint >= jointHi))
			continue;
		if (fixed) {
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, fixed);
		} else if (world && b->joint >= 0 && (u32)b->joint < m->jointCount) {
			C3D_Mtx joint, mv;
			mat34ToC3D(&world[b->joint], &joint);
			Mtx_Multiply(&mv, view, &joint);
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, &mv);
		} else {
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, modelView);
		}
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uLighting, lit ? 1.0f : 0.0f, 0.45f, 0.65f, 0.0f);

		C3D_TexEnv* env = C3D_GetTexEnv(0);
		C3D_TexEnvInit(env);
		if (b->texture >= 0) {
			C3D_TexBind(0, &m->textures[b->texture]);
			C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
			C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
		} else {
			C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
			C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
		}

		if (b->flags & MODEL_BLEND)
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
				GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
		else
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
		C3D_AlphaTest(b->flags & MODEL_ALPHA_TEST, GPU_GREATER, 0x60);
		C3D_DepthTest(true, GPU_GREATER, (b->flags & MODEL_NO_ZWRITE) ? GPU_WRITE_COLOR : GPU_WRITE_ALL);
		C3D_DrawArrays(GPU_TRIANGLES, b->first, b->count);
	}
}

void drawModelEx(const Model* m, const C3D_Mtx* projection, const C3D_Mtx* view,
	const Mat34* world, const C3D_Mtx* fixed, int onlyRoot)
{
	drawBatches(m, projection, view, world, fixed, onlyRoot, 0, 0);
}

void drawModelJoints(const Model* m, const C3D_Mtx* projection, const C3D_Mtx* view,
	const Mat34* world, int jointLo, int jointHi)
{
	drawBatches(m, projection, view, world, NULL, -1, jointLo, jointHi);
}

void drawModel(const Model* m, const C3D_Mtx* projection, const C3D_Mtx* view, const Mat34* world)
{
	drawModelEx(m, projection, view, world, NULL, -1);
}

void partMatrix(const Mat34* slot, C3D_Mtx* out)
{
	// Scale the basis, keep the origin. Goes through mat34ToC3D because a
	// C3D_FVec stores its components as w, z, y, x -- indexing .c[] by column
	// writes the matrix scrambled.
	Mat34 s = *slot;
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			s.m[r][c] *= PART_SCALE;
	mat34ToC3D(&s, out);
}

void drawRobot(const Model* body, const Mat34* world, const Model* parts, const bool* havePart,
	const C3D_Mtx* projection, const C3D_Mtx* view)
{
	drawModel(body, projection, view, world);
	if (!world || !body->hasRig)
		return;
	for (int k = 0; k < attachCount; k++) {
		if (!havePart[attach[k].part])
			continue;  // a root with no batches simply draws nothing
		s16 joint = body->slots[attach[k].slot];
		if (joint < 0 || (u32)joint >= body->jointCount)
			continue;
		C3D_Mtx part, mv;
		partMatrix(&world[joint], &part);
		Mtx_Multiply(&mv, view, &part);
		drawModelEx(&parts[attach[k].part], projection, view, NULL, &mv, attach[k].root);
	}
}
