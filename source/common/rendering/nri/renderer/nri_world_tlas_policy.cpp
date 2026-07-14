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

NRIWorldTlasExactReuseDecision EvaluateNRIWorldTlasExactReuse(const NRIWorldTlasExactReuseInput& input)
{
	NRIWorldTlasExactReuseDecision decision = {};
	decision.sameRecordingFence =
		input.publishedRecordingFence != 0 &&
		input.publishedRecordingFence == input.currentRecordingFence;
	if (!input.publicationValid)
	{
		decision.rejectReasonMask |= NRIWorldTlasExactReuseRejectReason_Unpublished;
	}
	if (!input.hasAccelerationStructure)
	{
		decision.rejectReasonMask |= NRIWorldTlasExactReuseRejectReason_AccelerationStructure;
	}
	if (!input.hasDescriptor)
	{
		decision.rejectReasonMask |= NRIWorldTlasExactReuseRejectReason_Descriptor;
	}
	if (!input.hasInstanceBuffer)
	{
		decision.rejectReasonMask |= NRIWorldTlasExactReuseRejectReason_InstanceBuffer;
	}
	if (input.publishedInstanceCapacity < input.requiredInstanceCount ||
		input.instanceBufferCapacityBytes < input.requiredInstanceBytes)
	{
		decision.rejectReasonMask |= NRIWorldTlasExactReuseRejectReason_Capacity;
	}
	if (input.currentMapEpoch == 0 || input.publishedMapEpoch != input.currentMapEpoch)
	{
		decision.rejectReasonMask |= NRIWorldTlasExactReuseRejectReason_MapEpoch;
	}
	if (input.currentBuildEpoch == 0 || input.publishedBuildEpoch != input.currentBuildEpoch)
	{
		decision.rejectReasonMask |= NRIWorldTlasExactReuseRejectReason_BuildEpoch;
	}
	if (input.publishedRecordingFence == 0 ||
		input.currentRecordingFence == 0 ||
		(!decision.sameRecordingFence && !input.publishedFenceComplete))
	{
		decision.rejectReasonMask |= NRIWorldTlasExactReuseRejectReason_Fence;
	}
	if (input.currentBlasGeneration == 0 || input.publishedBlasGeneration != input.currentBlasGeneration)
	{
		decision.rejectReasonMask |= NRIWorldTlasExactReuseRejectReason_BlasGeneration;
	}
	if (!input.instanceBytesEqual)
	{
		decision.rejectReasonMask |= NRIWorldTlasExactReuseRejectReason_InstanceBytes;
	}

	decision.reuse = decision.rejectReasonMask == NRIWorldTlasExactReuseRejectReason_None;
	return decision;
}

NRIWorldTlasUpdateDecision EvaluateNRIWorldTlasUpdate(
	const NRIWorldTlasDecision& instanceDecision,
	const NRIWorldTlasExactReuseInput& state,
	bool updateEnabled)
{
	NRIWorldTlasUpdateDecision decision = {};
	if (!updateEnabled)
	{
		decision.rejectReasonMask |= NRIWorldTlasUpdateRejectReason_Disabled;
	}

	const NRIWorldTlasExactReuseDecision exactGate = EvaluateNRIWorldTlasExactReuse(state);
	decision.gateRejectReasonMask = exactGate.rejectReasonMask &
		~(NRIWorldTlasExactReuseRejectReason_BlasGeneration |
			NRIWorldTlasExactReuseRejectReason_InstanceBytes);
	if (decision.gateRejectReasonMask != NRIWorldTlasExactReuseRejectReason_None)
	{
		decision.rejectReasonMask |= NRIWorldTlasUpdateRejectReason_Gate;
	}
	if (exactGate.sameRecordingFence || !state.publishedFenceComplete)
	{
		decision.rejectReasonMask |= NRIWorldTlasUpdateRejectReason_Fence;
	}

	const bool transformOnly =
		instanceDecision.kind == NRIWorldTlasDecisionKind::Update &&
		instanceDecision.reasonMask == NRIWorldTlasChangeReason_Transform;
	const bool blasGenerationOverride =
		instanceDecision.kind == NRIWorldTlasDecisionKind::ExactReuse &&
		state.publishedBlasGeneration != 0 &&
		state.currentBlasGeneration != 0 &&
		state.instanceBytesEqual &&
		state.publishedBlasGeneration != state.currentBlasGeneration;
	if (transformOnly)
	{
		decision.reasonMask |= NRIWorldTlasUpdateReason_Transform;
	}
	if (blasGenerationOverride)
	{
		decision.reasonMask |= NRIWorldTlasUpdateReason_BlasGenerationOverride;
	}
	if (decision.reasonMask == NRIWorldTlasUpdateReason_None)
	{
		decision.rejectReasonMask |= NRIWorldTlasUpdateRejectReason_UnsupportedChange;
	}

	decision.update = decision.rejectReasonMask == NRIWorldTlasUpdateRejectReason_None;
	return decision;
}
