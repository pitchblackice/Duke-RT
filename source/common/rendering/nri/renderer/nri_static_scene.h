#pragma once

#include "nri_frame_resources.h"
#include "nri_resources.h"
#include "../scene/nri_map_builder.h"
#include "../scene/nri_scene_bridge.h"

#include <cstdint>
#include <vector>

struct StaticMapChunkAtlas
{
	struct FreeRange
	{
		uint32_t offset = 0;
		uint32_t count = 0;
	};

	struct ChunkEntry
	{
		uint32_t chunkIndex = UINT32_MAX;
		uint32_t staticSceneChunkListIndex = UINT32_MAX;
		uint32_t vertexOffset = 0;
		uint32_t vertexCount = 0;
		uint32_t indexOffset = 0;
		uint32_t indexCount = 0;
		uint32_t primitiveOffset = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialOffset = 0;
		uint32_t materialCount = 0;
		bool valid = false;
	};

	bool valid = false;
	uint64_t buildSerial = 0;
	uint32_t chunkCount = 0;
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
	uint32_t primitiveCount = 0;
	uint32_t materialCount = 0;
	uint32_t vertexCapacity = 0;
	uint32_t indexCapacity = 0;
	uint32_t primitiveCapacity = 0;
	uint32_t materialCapacity = 0;
	std::vector<ChunkEntry> chunks;
	std::vector<FreeRange> freeVertexRanges;
	std::vector<FreeRange> freeIndexRanges;
	std::vector<FreeRange> freePrimitiveRanges;
	std::vector<FreeRange> freeMaterialRanges;
};

struct ResidentMapChunkRegistry
{
	struct Entry
	{
		uint32_t chunkIndex = UINT32_MAX;
		uint32_t staticSceneChunkListIndex = UINT32_MAX;
		uint32_t vertexOffset = 0;
		uint32_t vertexCount = 0;
		uint32_t indexOffset = 0;
		uint32_t indexCount = 0;
		uint32_t primitiveOffset = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialOffset = 0;
		uint32_t materialCount = 0;
		uint64_t geometryTopologySignature = 0;
		uint64_t baselineSignature = 0;
		uint64_t liveSignature = 0;
		uint64_t exactGeometrySignature = 0;
		uint64_t animatedMaterialSignature = 0;
		uint64_t materialPayloadHash = 0;
		uint64_t geometryPayloadHash = 0;
		uint64_t animatedGeometrySignature = 0;
		bool valid = false;
		bool active = false;
		bool mappedInStaticScene = false;
		bool accelerationResident = false;
		bool hasAnimatedTextureCandidates = false;
		bool animatedRefreshSuppressed = false;
		bool wasVisibleLastFrame = false;
		bool visibleValidationTraceEmitted = false;
		uint8_t visibleValidationFramesRemaining = 0;
		uint32_t animatedSuppressionEmitCount = 0;
		uint32_t runtimeAnimatedAttemptCount = 0;
		uint32_t runtimeAnimatedResidentApplyCount = 0;
		uint32_t runtimeAnimatedSyncSkipCount = 0;
		nri_scene::PTMapChunkMutationBaseline appliedBaseline;
	};

	bool valid = false;
	uint64_t buildSerial = 0;
	uint32_t chunkCount = 0;
	uint32_t activeChunkCount = 0;
	uint32_t mappedChunkCount = 0;
	uint32_t accelerationResidentChunkCount = 0;
	uint32_t animatedCandidateChunkCount = 0;
	uint32_t animatedRefreshSuppressedChunkCount = 0;
	std::vector<Entry> entries;
};

class NRIStaticSceneResidency
{
public:
	ResidentMapChunkRegistry& Registry() { return mResidentMapChunkRegistry; }
	const ResidentMapChunkRegistry& Registry() const { return mResidentMapChunkRegistry; }
	void ResetResidentMapChunkRegistry() { mResidentMapChunkRegistry = {}; }

private:
	ResidentMapChunkRegistry mResidentMapChunkRegistry;
};

struct StaticMapSceneResources
{
	NRIBufferResource vertexBuffer;
	NRIBufferResource indexBuffer;
	NRIBufferResource primitiveBuffer;
	NRIBufferResource materialBuffer;
	StaticMapChunkAtlas chunkAtlas;
	NRIBufferResource tlasInstanceBuffer;
	NRIBufferResource scratchBuffer;
	NRIBufferResource topLevelScratchBuffer;
	NRIAccelerationStructureResource topLevelAS;
	uint64_t accelerationBuildSerial = 0;
	uint32_t tlasInstanceCount = 0;
	std::vector<SceneInstanceData> sceneInstances;
};
