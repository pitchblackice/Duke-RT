#include "nri_map_world.h"

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
}
