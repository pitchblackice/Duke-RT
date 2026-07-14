#include "nri_world_tlas_policy.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace
{
	bool TransformBitsEqual(const nri::TopLevelInstance& previous, const nri::TopLevelInstance& current)
	{
		return std::memcmp(previous.transform, current.transform, sizeof(previous.transform)) == 0;
	}

	void AppendDirtyTransformIndex(std::vector<NRIWorldTlasDirtyInstanceRange>& ranges, uint32_t instanceIndex)
	{
		if (!ranges.empty())
		{
			NRIWorldTlasDirtyInstanceRange& tail = ranges.back();
			if (tail.firstInstance + tail.instanceCount == instanceIndex)
			{
				tail.instanceCount++;
				return;
			}
		}

		ranges.push_back({ instanceIndex, 1 });
	}
}

NRIWorldTlasDecision ClassifyNRIWorldTlasInstances(
	const std::vector<nri::TopLevelInstance>& previousInstances,
	const std::vector<nri::TopLevelInstance>& currentInstances,
	bool priorSlotValid,
	uint32_t priorCapacity)
{
	NRIWorldTlasDecision decision = {};
	if (!priorSlotValid)
	{
		decision.reasonMask |= NRIWorldTlasChangeReason_Cold;
	}
	if (currentInstances.size() > priorCapacity)
	{
		decision.reasonMask |= NRIWorldTlasChangeReason_Capacity;
	}
	if (previousInstances.size() != currentInstances.size())
	{
		decision.reasonMask |= NRIWorldTlasChangeReason_Count;
	}

	std::vector<NRIWorldTlasDirtyInstanceRange> dirtyTransformRanges;
	const bool canProduceDirtyRanges = priorSlotValid &&
		previousInstances.size() == currentInstances.size() &&
		currentInstances.size() <= priorCapacity;
	const size_t sharedCount = std::min(previousInstances.size(), currentInstances.size());
	for (size_t index = 0; index < sharedCount; ++index)
	{
		const nri::TopLevelInstance& previous = previousInstances[index];
		const nri::TopLevelInstance& current = currentInstances[index];
		if (previous.instanceId != current.instanceId)
		{
			decision.reasonMask |= NRIWorldTlasChangeReason_InstanceId;
		}
		if (previous.mask != current.mask)
		{
			decision.reasonMask |= NRIWorldTlasChangeReason_Mask;
		}
		if (previous.shaderBindingTableLocalOffset != current.shaderBindingTableLocalOffset)
		{
			decision.reasonMask |= NRIWorldTlasChangeReason_SbtOffset;
		}
		if (previous.flags != current.flags)
		{
			decision.reasonMask |= NRIWorldTlasChangeReason_Flags;
		}
		if (previous.accelerationStructureHandle != current.accelerationStructureHandle)
		{
			decision.reasonMask |= NRIWorldTlasChangeReason_BlasHandle;
		}
		if (!TransformBitsEqual(previous, current))
		{
			decision.reasonMask |= NRIWorldTlasChangeReason_Transform;
			if (canProduceDirtyRanges)
			{
				AppendDirtyTransformIndex(dirtyTransformRanges, (uint32_t)index);
			}
		}
	}

	const uint32_t fullBuildReasons =
		NRIWorldTlasChangeReason_Cold |
		NRIWorldTlasChangeReason_Capacity |
		NRIWorldTlasChangeReason_Count |
		NRIWorldTlasChangeReason_InstanceId |
		NRIWorldTlasChangeReason_Mask |
		NRIWorldTlasChangeReason_SbtOffset |
		NRIWorldTlasChangeReason_Flags |
		NRIWorldTlasChangeReason_BlasHandle;
	if ((decision.reasonMask & fullBuildReasons) != 0)
	{
		decision.kind = NRIWorldTlasDecisionKind::FullBuild;
		return decision;
	}
	if ((decision.reasonMask & NRIWorldTlasChangeReason_Transform) != 0)
	{
		decision.kind = NRIWorldTlasDecisionKind::Update;
		decision.dirtyTransformRanges = std::move(dirtyTransformRanges);
		return decision;
	}

	decision.kind = NRIWorldTlasDecisionKind::ExactReuse;
	return decision;
}
