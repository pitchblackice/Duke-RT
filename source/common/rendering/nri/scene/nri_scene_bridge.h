#pragma once

#include "flatvertices.h"
#include "hw_drawinfo.h"
#include "hw_drawstructs.h"

#include <vector>

class FGameTexture;

namespace nri_scene
{
bool TryGetAverageTextureColor(FGameTexture* texture, float* outColor);

enum MaterialFlags : uint32_t
{
	MaterialFlag_None = 0,
	MaterialFlag_Indexed = 1u << 0,
	MaterialFlag_Fullbright = 1u << 1,
	MaterialFlag_Flat = 1u << 2,
	MaterialFlag_Sprite = 1u << 3,
	MaterialFlag_Mirror = 1u << 4,
	MaterialFlag_Sky = 1u << 5,
	MaterialFlag_Portal = 1u << 6,
};

struct SceneDebugStats
{
	unsigned int totalDrawItems = 0;
	unsigned int wallDrawItems = 0;
	unsigned int flatDrawItems = 0;
	unsigned int spriteDrawItems = 0;
	unsigned int translucentDrawItems = 0;
	unsigned int triangleEstimate = 0;
	unsigned int materialRefs = 0;
	unsigned int mirrorSurfaces = 0;
	unsigned int skySurfaces = 0;
	unsigned int portalViews = 0;
	unsigned int portalCapturesSkipped = 0;
	unsigned int modelDrawItems = 0;
	unsigned int voxelProxyDrawItems = 0;
	unsigned int unsupportedModelDrawItems = 0;
};

struct MaterialRef
{
	FGameTexture* texture = nullptr;
	int palette = 0;
	int shade = 0;
	float alpha = 1.0f;
	uint32_t flags = MaterialFlag_None;
};

struct CapturedVertex
{
	float position[3] = {};
	float prevPosition[3] = {};
	float uv[2] = {};
};

struct SurfaceRef
{
	std::vector<CapturedVertex> vertices;
	MaterialRef material;
};

struct SceneView
{
	HWDrawInfo* drawInfo = nullptr;
	std::vector<SurfaceRef> opaqueWalls;
	std::vector<SurfaceRef> opaqueFlats;
	std::vector<SurfaceRef> opaqueSprites;
	SceneDebugStats stats;
	float skyColor[3] = { 0.38f, 0.48f, 0.65f };
	float groundColor[3] = { 0.08f, 0.08f, 0.08f };
};

SceneDebugStats CollectDebugStats(HWDrawInfo& di);
bool CaptureScene(HWDrawInfo& di, SceneView& outView);
}
