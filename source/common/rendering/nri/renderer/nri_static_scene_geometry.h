#pragma once

#include "nri_static_scene.h"

#include <cstdint>

namespace nri_static_scene_geometry
{
int32_t FindMapChunkIndexForSector(const nri_scene::PTMapWorld& mapWorld, int32_t sectorIndex);
int32_t ResolveVisibilityChunkIndexForProvenance(const nri_scene::PTMapWorld& mapWorld, const nri_scene::SurfaceProvenance& provenance);
uint32_t NormalizeResidentAtlasIndex(uint32_t value, uint32_t base);

uint64_t HashResidentGeometryPayload(
	const nri_scene::PTMapWorld& mapWorld,
	const nri_scene::GeometryData& geometry,
	uint32_t vertexOffset,
	uint32_t vertexCount,
	uint32_t indexOffset,
	uint32_t indexCount,
	uint32_t primitiveOffset,
	uint32_t primitiveCount,
	uint32_t materialOffset,
	uint32_t materialCount);

uint64_t HashResidentGeometryPayloadOrderIndependent(
	const nri_scene::PTMapWorld& mapWorld,
	const nri_scene::GeometryData& geometry,
	uint32_t vertexOffset,
	uint32_t vertexCount,
	uint32_t primitiveOffset,
	uint32_t primitiveCount,
	uint32_t materialOffset,
	uint32_t materialCount);

uint64_t ComputeGeometryTopologySignature(const nri_scene::GeometryData& geometry);
uint64_t ComputePrimitiveLayoutSignature(const nri_scene::GeometryData& geometry);

void ResetStaticMapChunkAtlas(StaticMapChunkAtlas& atlas);
uint32_t GetChunkAtlasCapacity(uint32_t usedCount);
uint32_t AllocateChunkAtlasSlice(uint32_t count, uint32_t alignment, uint32_t& cursor);
uint32_t AllocateChunkAtlasRange(uint32_t count, uint32_t capacity, std::vector<StaticMapChunkAtlas::FreeRange>& freeRanges, uint32_t& cursor);
void ReleaseChunkAtlasRange(std::vector<StaticMapChunkAtlas::FreeRange>& freeRanges, uint32_t offset, uint32_t count);
bool BuildStaticMapChunkAtlasLayout(const StaticMapSceneCache& staticScene, StaticMapChunkAtlas& outAtlas);

void CopyChunkVertexDataToAtlas(
	const nri_scene::GeometryData& sourceGeometry,
	const StaticMapSceneCache::ChunkCache& sourceChunk,
	const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
	std::vector<nri_scene::SceneVertex>& outVertices);
void CopyChunkIndexDataToAtlas(
	const nri_scene::GeometryData& sourceGeometry,
	const StaticMapSceneCache::ChunkCache& sourceChunk,
	const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
	std::vector<uint32_t>& outIndices);
void CopyChunkVertexAndIndexDataToAtlas(
	const nri_scene::GeometryData& sourceGeometry,
	const StaticMapSceneCache::ChunkCache& sourceChunk,
	const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
	std::vector<nri_scene::SceneVertex>& outVertices,
	std::vector<uint32_t>& outIndices);
void CopyChunkPrimitiveDataToAtlas(
	const nri_scene::PTMapWorld& mapWorld,
	const nri_scene::GeometryData& sourceGeometry,
	const StaticMapSceneCache::ChunkCache& sourceChunk,
	const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
	std::vector<nri_scene::PrimitiveData>& outPrimitives);
void CopyChunkGeometryToAtlas(
	const nri_scene::PTMapWorld& mapWorld,
	const nri_scene::GeometryData& sourceGeometry,
	const StaticMapSceneCache::ChunkCache& sourceChunk,
	const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
	std::vector<nri_scene::SceneVertex>& outVertices,
	std::vector<uint32_t>& outIndices,
	std::vector<nri_scene::PrimitiveData>& outPrimitives);
void CopyChunkMaterialsToAtlas(
	const std::vector<nri_scene::MaterialData>& sourceMaterials,
	const StaticMapSceneCache::ChunkCache& sourceChunk,
	const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
	std::vector<nri_scene::MaterialData>& outMaterials);
bool RebuildResidentStaticCpuAtlasMirror(
	const nri_scene::PTMapWorld& mapWorld,
	StaticMapSceneCache& staticScene,
	const StaticMapChunkAtlas& atlas);
}
