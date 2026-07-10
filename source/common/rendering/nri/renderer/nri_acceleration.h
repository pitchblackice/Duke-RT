#pragma once

#include "nri_resources.h"
#include "../scene/nri_geometry_bridge.h"

#include <cstdint>
#include <vector>

class NRIRenderer;

class NRIAccelerationStructureManager
{
public:
	static bool BuildDynamic(NRIRenderer& renderer, const nri_scene::GeometryData& geometry);
	static bool BuildDynamic(
		NRIRenderer& renderer,
		const nri_scene::GeometryData& geometry,
		uint32_t indexOffset,
		uint32_t indexCount,
		uint32_t primitiveCount,
		NRIAccelerationStructureResource& outAccelerationStructure,
		bool updateDynamicPerfStats);
	static bool BuildBottomLevel(
		NRIRenderer& renderer,
		const NRIBufferResource& vertexBuffer,
		const NRIBufferResource& indexBuffer,
		uint32_t vertexOffset,
		uint32_t vertexCount,
		uint32_t indexOffset,
		uint32_t indexCount,
		uint32_t primitiveCount,
		NRIAccelerationStructureResource& outAccelerationStructure,
		bool updateDynamicPerfStats,
		NRIBufferResource* buildScratchBuffer = nullptr,
		nri::AccelerationStructureBits buildFlags = nri::AccelerationStructureBits::PREFER_FAST_BUILD);
	static bool BuildEmissiveTopLevel(NRIRenderer& renderer);
	static bool BuildTopLevel(NRIRenderer& renderer, const std::vector<nri::TopLevelInstance>& instances, uint32_t sceneBufferMask);
	static bool BuildTopLevel(
		NRIRenderer& renderer,
		const std::vector<nri::TopLevelInstance>& instances,
		uint32_t sceneBufferMask,
		NRIAccelerationStructureResource& topLevelAS,
		NRIBufferResource& tlasInstanceBuffer,
		NRIBufferResource& topLevelScratchBuffer,
		const NRIBufferResource* staticVertexBuffer,
		const NRIBufferResource* staticIndexBuffer,
		uint32_t* outTlasInstanceCount,
		bool updateLiveState,
		bool tlasInstanceWritesQuiesced);
	static bool EnsureTopLevelCapacity(NRIRenderer& renderer, uint32_t instanceCount);
};
