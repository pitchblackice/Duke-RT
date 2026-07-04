#pragma once

#include <cstdint>

namespace nri_scene
{
struct SurfaceRef;

struct VoxelGeometryContentHashes
{
	uint64_t geometryContentHash = 0;
	uint64_t renderPrimitiveHash = 0;
};

uint64_t BuildVoxelGeometryContentHash(const SurfaceRef& surface);
uint64_t BuildVoxelRenderPrimitiveHash(const SurfaceRef& surface);
VoxelGeometryContentHashes BuildVoxelGeometryContentHashes(const SurfaceRef& surface);
}
