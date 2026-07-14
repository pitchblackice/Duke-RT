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

