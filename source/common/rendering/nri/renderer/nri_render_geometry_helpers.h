#pragma once

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_map_world.h"
#include "../scene/nri_scene_bridge.h"

#include <cstdint>

struct MapCeilingNudgeStats
{
	uint32_t surfaceCount = 0;
	uint32_t vertexCount = 0;
	uint32_t skippedNormalCount = 0;
};

uint32_t CountOrphanLocalSpaces(const nri_scene::PTMapWorld& mapWorld);
MapCeilingNudgeStats NudgeMapCeilingSections(nri_scene::SceneView& sceneView, float depthNudge);
void TranslateGeometry(nri_scene::GeometryData& geometry, float dx, float dy, float dz, float prevDx, float prevDy, float prevDz);
void AssignGeometryPortalIndices(const nri_scene::PTMapWorld& mapWorld, nri_scene::GeometryData& geometry);
void AppendGeometry(const nri_scene::GeometryData& source, uint32_t materialIndexOffset, nri_scene::GeometryData& destination);
