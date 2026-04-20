#pragma once

#include "nri_scene_bridge.h"

#include <vector>

namespace nri_scene
{
struct SceneVertex
{
	float position[3] = {};
	float prevPosition[3] = {};
	float uv[2] = {};
};

struct PrimitiveData
{
	uint32_t indices[3] = {};
	uint32_t materialIndex = 0;
	float uv0[2] = {};
	float uv1[2] = {};
	float uv2[2] = {};
	float normal[3] = {};
	uint32_t flags = 0;
	uint32_t portalIndex = UINT32_MAX;
	uint32_t reserved0 = UINT32_MAX;
};

struct GeometryData
{
	std::vector<SceneVertex> vertices;
	std::vector<uint32_t> indices;
	std::vector<PrimitiveData> primitives;
	std::vector<SurfaceProvenance> primitiveProvenance;
};

void BuildGeometry(const SceneView& sceneView, GeometryData& outGeometry);
}
