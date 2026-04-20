#ifndef __GL_CLOCK_H
#define __GL_CLOCK_H

#include "stats.h"
#include "m_fixed.h"

extern glcycle_t RenderWall,SetupWall,ClipWall;
extern glcycle_t RenderFlat,SetupFlat;
extern glcycle_t RenderSprite,SetupSprite;
extern glcycle_t All, Finish, PortalAll, Bsp;
extern glcycle_t ProcessAll, PostProcess;
extern glcycle_t RenderAll;
extern glcycle_t Dirty;
extern glcycle_t drawcalls, twoD, Flush3D;
extern glcycle_t MTWait, WTTotal;
extern glcycle_t NriPTAll, NriPTInitialize, NriPTFrameResources, NriPTUpdateState;
extern glcycle_t NriPTSceneCapture, NriPTGeometryBuild, NriPTMaterialBuild;
extern glcycle_t NriPTPaletteUpload, NriPTSceneTextures, NriPTSceneBuffers, NriPTAcceleration;
extern glcycle_t NriPTFrameWait, NriPTWaitPresent, NriPTAcquireSwap, NriPTQueueSubmit, NriPTQueuePresent;
extern glcycle_t NriPTBootstrapDispatch, NriPTFrameGraph, NriPTTraceOpaque, NriPTDenoiser;
extern glcycle_t NriPTComposition, NriPTRawPresent, NriPTFinalPresent, NriPTUpscale, NriPTFinal, NriPTCopyFinal;

extern int iter_dlightf, iter_dlight, draw_dlight, draw_dlightf;
extern int rendered_lines,rendered_flats,rendered_sprites,rendered_decals,render_vertexsplit,render_texsplit;
extern int rendered_portals;

extern int vertexcount, flatvertices, flatprimitives;

struct PerfRenderTraceStats
{
	double allMs = 0.0;
	double finishMs = 0.0;
	double renderAllMs = 0.0;
	double processAllMs = 0.0;
	double portalAllMs = 0.0;
	double postProcessMs = 0.0;
	double drawCallsMs = 0.0;
	double renderWallMs = 0.0;
	double setupWallMs = 0.0;
	double clipWallMs = 0.0;
	double bspMs = 0.0;
	double renderFlatMs = 0.0;
	double setupFlatMs = 0.0;
	double renderSpriteMs = 0.0;
	double setupSpriteMs = 0.0;
	double twoDMs = 0.0;
	double finish3DMs = 0.0;
	double mtWaitMs = 0.0;
	double wtTotalMs = 0.0;
	int renderedWalls = 0;
	int renderedFlats = 0;
	int renderedSprites = 0;
	int renderedDecals = 0;
	int renderedPortals = 0;
	int renderedVertices = 0;
	int flatVertexCount = 0;
	int flatPrimitiveCount = 0;
	bool nriActive = false;
	double nriAllMs = 0.0;
	double nriInitializeMs = 0.0;
	double nriFrameResourcesMs = 0.0;
	double nriUpdateStateMs = 0.0;
	double nriSceneCaptureMs = 0.0;
	double nriGeometryBuildMs = 0.0;
	double nriMaterialBuildMs = 0.0;
	double nriPaletteUploadMs = 0.0;
	double nriSceneTexturesMs = 0.0;
	double nriSceneBuffersMs = 0.0;
	double nriAccelerationMs = 0.0;
	double nriBootstrapDispatchMs = 0.0;
	double nriFrameGraphMs = 0.0;
	double nriCopyFinalMs = 0.0;
	double nriFrameWaitMs = 0.0;
	double nriWaitPresentMs = 0.0;
	double nriAcquireSwapMs = 0.0;
	double nriQueueSubmitMs = 0.0;
	double nriQueuePresentMs = 0.0;
	double nriTraceOpaqueMs = 0.0;
	double nriDenoiserMs = 0.0;
	double nriCompositionMs = 0.0;
	double nriUpscaleMs = 0.0;
	double nriFinalMs = 0.0;
	double nriRawPresentMs = 0.0;
	double nriFinalPresentMs = 0.0;
};

PerfRenderTraceStats GetPerfRenderTraceStats();

void ResetProfilingData();
void CheckBench();
void  checkBenchActive();


#endif
