#include "nri_map_world.h"

#include <algorithm>

namespace nri_scene
{
namespace
{
	PTSkySourceType GetSkySourceType(const SurfaceProvenance& provenance)
	{
		switch (provenance.sourceType)
		{
		case SurfaceSourceType::MapPortalSurface:
			return PTSkySourceType::Portal;
		case SurfaceSourceType::MapFloorSection:
		case SurfaceSourceType::MapCeilingSection:
			return PTSkySourceType::Flat;
		default:
			return PTSkySourceType::Wall;
		}
	}

	void AppendSurfaceToSceneView(const PTMapSurface& surface, SceneView& outView)
	{
		SurfaceRef copy = surface.surface;
		if ((copy.material.flags & MaterialFlag_Sky) != 0 && copy.material.texture != nullptr)
		{
			UpdateSceneSky(outView, copy.material.texture, 0, GetSkySourceType(copy.provenance));
		}

		switch (surface.kind)
		{
		case PTMapSurfaceKind::Floor:
		case PTMapSurfaceKind::Ceiling:
			outView.opaqueFlats.push_back(std::move(copy));
			break;
		default:
			outView.opaqueWalls.push_back(std::move(copy));
			break;
		}
	}
}

void PTMapWorld::Reset()
{
	level = nullptr;
	buildSerial = 0;
	valid = false;
	chunks.clear();
	surfaces.clear();
	stats = {};
}

SceneDebugStats CollectMapWorldDebugStats(const PTMapWorld& mapWorld)
{
	SceneDebugStats stats = {};
	stats.wallDrawItems = mapWorld.stats.wallSurfaceCount;
	stats.flatDrawItems = mapWorld.stats.flatSurfaceCount;
	stats.spriteDrawItems = 0;
	stats.translucentDrawItems = 0;
	stats.triangleEstimate = mapWorld.stats.triangleCount;
	stats.materialRefs = mapWorld.stats.surfaceCount;
	stats.skySurfaces = mapWorld.stats.skySurfaceCount;
	stats.portalViews = 0;
	stats.portalCapturesSkipped = 0;

	for (const PTMapSurface& surface : mapWorld.surfaces)
	{
		if ((surface.surface.material.flags & MaterialFlag_Mirror) != 0)
		{
			stats.mirrorSurfaces++;
		}
	}

	return stats;
}

void BuildMapSceneView(const PTMapWorld& mapWorld, SceneView& outView)
{
	outView = {};
	outView.stats = CollectMapWorldDebugStats(mapWorld);
	outView.opaqueWalls.reserve(mapWorld.stats.wallSurfaceCount);
	outView.opaqueFlats.reserve(mapWorld.stats.flatSurfaceCount);

	for (const PTMapSurface& surface : mapWorld.surfaces)
	{
		AppendSurfaceToSceneView(surface, outView);
	}
}

void BuildMapChunkSceneView(const PTMapWorld& mapWorld, const PTMapChunk& chunk, SceneView& outView)
{
	outView = {};
	outView.drawInfo = nullptr;
	outView.stats = CollectMapWorldDebugStats(mapWorld);
	outView.opaqueWalls.reserve(chunk.surfaceCount);
	outView.opaqueFlats.reserve(chunk.surfaceCount);

	const uint32_t endSurface = std::min<uint32_t>(chunk.firstSurface + chunk.surfaceCount, (uint32_t)mapWorld.surfaces.size());
	for (uint32_t surfaceIndex = chunk.firstSurface; surfaceIndex < endSurface; ++surfaceIndex)
	{
		AppendSurfaceToSceneView(mapWorld.surfaces[surfaceIndex], outView);
	}
}
}
