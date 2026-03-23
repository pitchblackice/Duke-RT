#pragma once

#include "nri_scene_bridge.h"

#include <cstdint>
#include <vector>

struct MapRecord;

namespace nri_scene
{
enum class PTMapChunkKind : uint32_t
{
	Sector = 0,
};

enum class PTMapSurfaceKind : uint32_t
{
	Floor = 0,
	Ceiling,
	WallOneSided,
	WallUpper,
	WallMiddle,
	WallLower,
	Portal,
};

struct PTMapSurfaceKey
{
	uint32_t primary = UINT32_MAX;
	uint32_t secondary = UINT32_MAX;
};

struct PTMapSurface
{
	SurfaceRef surface;
	PTMapSurfaceKind kind = PTMapSurfaceKind::Floor;
	PTMapSurfaceKey key = {};
	uint32_t chunkIndex = UINT32_MAX;
};

struct PTMapChunk
{
	PTMapChunkKind kind = PTMapChunkKind::Sector;
	uint32_t chunkIndex = UINT32_MAX;
	int32_t sectorIndex = -1;
	uint32_t firstSurface = 0;
	uint32_t surfaceCount = 0;
	uint32_t triangleCount = 0;
};

struct PTMapWorldStats
{
	uint32_t sectorCount = 0;
	uint32_t sectionCount = 0;
	uint32_t chunkCount = 0;
	uint32_t surfaceCount = 0;
	uint32_t wallSurfaceCount = 0;
	uint32_t flatSurfaceCount = 0;
	uint32_t portalSurfaceCount = 0;
	uint32_t skySurfaceCount = 0;
	uint32_t triangleCount = 0;
};

struct PTMapWorld
{
	MapRecord* level = nullptr;
	uint64_t buildSerial = 0;
	bool valid = false;
	std::vector<PTMapChunk> chunks;
	std::vector<PTMapSurface> surfaces;
	PTMapWorldStats stats;

	void Reset();
};

SceneDebugStats CollectMapWorldDebugStats(const PTMapWorld& mapWorld);
void BuildMapSceneView(const PTMapWorld& mapWorld, SceneView& outView);
}
