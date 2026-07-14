#pragma once

#include "../system/nri_local.h"

#include <cstdint>
#include <vector>

enum class NRIWorldTlasDecisionKind : uint8_t
{
	ExactReuse,
	Update,
	FullBuild,
};

enum NRIWorldTlasChangeReason : uint32_t
{
	NRIWorldTlasChangeReason_None = 0,
	NRIWorldTlasChangeReason_Cold = 1u << 0,
	NRIWorldTlasChangeReason_Capacity = 1u << 1,
	NRIWorldTlasChangeReason_Count = 1u << 2,
	NRIWorldTlasChangeReason_InstanceId = 1u << 3,
	NRIWorldTlasChangeReason_Mask = 1u << 4,
	NRIWorldTlasChangeReason_SbtOffset = 1u << 5,
	NRIWorldTlasChangeReason_Flags = 1u << 6,
	NRIWorldTlasChangeReason_BlasHandle = 1u << 7,
	NRIWorldTlasChangeReason_Transform = 1u << 8,
};

struct NRIWorldTlasDirtyInstanceRange
{
	uint32_t firstInstance = 0;
	uint32_t instanceCount = 0;
};

struct NRIWorldTlasDecision
{
	NRIWorldTlasDecisionKind kind = NRIWorldTlasDecisionKind::FullBuild;
	uint32_t reasonMask = NRIWorldTlasChangeReason_None;
	std::vector<NRIWorldTlasDirtyInstanceRange> dirtyTransformRanges;
};

NRIWorldTlasDecision ClassifyNRIWorldTlasInstances(
	const std::vector<nri::TopLevelInstance>& previousInstances,
	const std::vector<nri::TopLevelInstance>& currentInstances,
	bool priorSlotValid,
	uint32_t priorCapacity);

enum NRIWorldTlasExactReuseRejectReason : uint32_t
{
	NRIWorldTlasExactReuseRejectReason_None = 0,
	NRIWorldTlasExactReuseRejectReason_Unpublished = 1u << 0,
	NRIWorldTlasExactReuseRejectReason_AccelerationStructure = 1u << 1,
	NRIWorldTlasExactReuseRejectReason_Descriptor = 1u << 2,
	NRIWorldTlasExactReuseRejectReason_InstanceBuffer = 1u << 3,
	NRIWorldTlasExactReuseRejectReason_Capacity = 1u << 4,
	NRIWorldTlasExactReuseRejectReason_MapEpoch = 1u << 5,
	NRIWorldTlasExactReuseRejectReason_BuildEpoch = 1u << 6,
	NRIWorldTlasExactReuseRejectReason_Fence = 1u << 7,
	NRIWorldTlasExactReuseRejectReason_BlasGeneration = 1u << 8,
	NRIWorldTlasExactReuseRejectReason_InstanceBytes = 1u << 9,
	NRIWorldTlasExactReuseRejectReason_BuildInstanceCount = 1u << 10,
};

struct NRIWorldTlasExactReuseInput
{
	bool publicationValid = false;
	bool hasAccelerationStructure = false;
	bool hasDescriptor = false;
	bool hasInstanceBuffer = false;
	uint32_t allocatedInstanceCapacity = 0;
	uint32_t publishedBuildInstanceCount = 0;
	uint32_t requiredInstanceCount = 0;
	uint64_t instanceBufferCapacityBytes = 0;
	uint64_t requiredInstanceBytes = 0;
	uint64_t publishedMapEpoch = 0;
	uint64_t currentMapEpoch = 0;
	uint64_t publishedBuildEpoch = 0;
	uint64_t currentBuildEpoch = 0;
	uint64_t publishedRecordingFence = 0;
	uint64_t currentRecordingFence = 0;
	bool publishedFenceComplete = false;
	uint64_t publishedBlasGeneration = 0;
	uint64_t currentBlasGeneration = 0;
	bool instanceBytesEqual = false;
};

struct NRIWorldTlasExactReuseDecision
{
	bool reuse = false;
	bool sameRecordingFence = false;
	uint32_t rejectReasonMask = NRIWorldTlasExactReuseRejectReason_None;
};

NRIWorldTlasExactReuseDecision EvaluateNRIWorldTlasExactReuse(const NRIWorldTlasExactReuseInput& input);

enum NRIWorldTlasUpdateReason : uint32_t
{
	NRIWorldTlasUpdateReason_None = 0,
	NRIWorldTlasUpdateReason_Transform = 1u << 0,
	NRIWorldTlasUpdateReason_BlasGenerationOverride = 1u << 1,
};

enum NRIWorldTlasUpdateRejectReason : uint32_t
{
	NRIWorldTlasUpdateRejectReason_None = 0,
	NRIWorldTlasUpdateRejectReason_Disabled = 1u << 0,
	NRIWorldTlasUpdateRejectReason_Gate = 1u << 1,
	NRIWorldTlasUpdateRejectReason_Fence = 1u << 2,
	NRIWorldTlasUpdateRejectReason_UnsupportedChange = 1u << 3,
	NRIWorldTlasUpdateRejectReason_Runtime = 1u << 4,
};

struct NRIWorldTlasUpdateDecision
{
	bool update = false;
	uint32_t reasonMask = NRIWorldTlasUpdateReason_None;
	uint32_t rejectReasonMask = NRIWorldTlasUpdateRejectReason_None;
	uint32_t gateRejectReasonMask = NRIWorldTlasExactReuseRejectReason_None;
};

NRIWorldTlasUpdateDecision EvaluateNRIWorldTlasUpdate(
	const NRIWorldTlasDecision& instanceDecision,
	const NRIWorldTlasExactReuseInput& state,
	bool updateEnabled);

enum NRIWorldTlasFullBuildReuseRejectReason : uint32_t
{
	NRIWorldTlasFullBuildReuseRejectReason_None = 0,
	NRIWorldTlasFullBuildReuseRejectReason_Unpublished = 1u << 0,
	NRIWorldTlasFullBuildReuseRejectReason_AccelerationStructure = 1u << 1,
	NRIWorldTlasFullBuildReuseRejectReason_Descriptor = 1u << 2,
	NRIWorldTlasFullBuildReuseRejectReason_InstanceBuffer = 1u << 3,
	NRIWorldTlasFullBuildReuseRejectReason_ScratchBuffer = 1u << 4,
	NRIWorldTlasFullBuildReuseRejectReason_InstanceCapacity = 1u << 5,
	NRIWorldTlasFullBuildReuseRejectReason_InstanceBufferCapacity = 1u << 6,
	NRIWorldTlasFullBuildReuseRejectReason_ScratchCapacity = 1u << 7,
	NRIWorldTlasFullBuildReuseRejectReason_Flags = 1u << 8,
	NRIWorldTlasFullBuildReuseRejectReason_ResourceState = 1u << 9,
	NRIWorldTlasFullBuildReuseRejectReason_Fence = 1u << 10,
	NRIWorldTlasFullBuildReuseRejectReason_Epoch = 1u << 11,
	NRIWorldTlasFullBuildReuseRejectReason_Compacted = 1u << 12,
	NRIWorldTlasFullBuildReuseRejectReason_Type = 1u << 13,
	NRIWorldTlasFullBuildReuseRejectReason_Runtime = 1u << 14,
};

enum NRIWorldTlasFullBuildGrowthReason : uint32_t
{
	NRIWorldTlasFullBuildGrowthReason_None = 0,
	NRIWorldTlasFullBuildGrowthReason_AccelerationStructure = 1u << 0,
	NRIWorldTlasFullBuildGrowthReason_InstanceBuffer = 1u << 1,
	NRIWorldTlasFullBuildGrowthReason_ScratchBuffer = 1u << 2,
};

struct NRIWorldTlasFullBuildReuseInput
{
	bool publicationValid = false;
	bool hasAccelerationStructure = false;
	bool hasDescriptor = false;
	bool hasInstanceBuffer = false;
	bool hasScratchBuffer = false;
	bool updateEnabled = false;
	bool buildFlagsCompatible = false;
	bool buildTypeCompatible = false;
	bool compacted = false;
	bool destinationInComputeReadState = false;
	uint32_t allocatedInstanceCapacity = 0;
	uint32_t publishedBuildInstanceCount = 0;
	uint32_t requiredInstanceCount = 0;
	uint64_t instanceBufferCapacityBytes = 0;
	uint64_t requiredInstanceBytes = 0;
	uint64_t scratchBufferCapacityBytes = 0;
	uint64_t cachedBuildScratchBytes = 0;
	uint64_t requiredScratchBytes = 0;
	uint64_t publishedMapEpoch = 0;
	uint64_t currentMapEpoch = 0;
	uint64_t publishedBuildEpoch = 0;
	uint64_t currentBuildEpoch = 0;
	uint64_t publishedRecordingFence = 0;
	uint64_t currentRecordingFence = 0;
	bool publishedFenceComplete = false;
};

struct NRIWorldTlasFullBuildReuseDecision
{
	bool reuseDestination = false;
	bool replacementRequired = true;
	bool sameRecordingFence = false;
	uint32_t rejectReasonMask = NRIWorldTlasFullBuildReuseRejectReason_None;
	uint32_t growthReasonMask = NRIWorldTlasFullBuildGrowthReason_None;
};

NRIWorldTlasFullBuildReuseDecision EvaluateNRIWorldTlasFullBuildReuse(
	const NRIWorldTlasFullBuildReuseInput& input);
