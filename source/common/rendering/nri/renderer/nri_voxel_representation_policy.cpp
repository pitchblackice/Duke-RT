#include "nri_voxel_representation_policy.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
	constexpr float ProjectionNearDepth = 0.001f;
	constexpr uint32_t HysteresisObservationFrameCount = 3u;

	bool IsFinite(float value)
	{
		return std::isfinite(value);
	}

	bool IsFinite3(const std::array<float, 3>& value)
	{
		return IsFinite(value[0]) && IsFinite(value[1]) && IsFinite(value[2]);
	}

	bool IsFiniteTransform(const std::array<float, 12>& transform)
	{
		for (float value : transform)
		{
			if (!IsFinite(value))
			{
				return false;
			}
		}
		return true;
	}

	float Dot3(const std::array<float, 3>& left, const std::array<float, 3>& right)
	{
		return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
	}

	std::array<float, 3> TransformPoint(
		const std::array<float, 12>& transform,
		const std::array<float, 3>& point)
	{
		return
		{
			transform[0] * point[0] + transform[1] * point[1] + transform[2] * point[2] + transform[3],
			transform[4] * point[0] + transform[5] * point[1] + transform[6] * point[2] + transform[7],
			transform[8] * point[0] + transform[9] * point[1] + transform[10] * point[2] + transform[11],
		};
	}

	std::array<float, 3> Subtract3(
		const std::array<float, 3>& left,
		const std::array<float, 3>& right)
	{
		return { left[0] - right[0], left[1] - right[1], left[2] - right[2] };
	}

	uint64_t BuildDecisionIdentity(const NRIVoxelRepresentationFacts& facts)
	{
		// Actor indices survive ordinary actor motion and are restored by save/load.
		// The policy state is separately reset by map generation and by a changed
		// source identity, so index reuse cannot carry hysteresis across incarnations.
		return facts.actorIndex >= 0 ? (uint64_t)(uint32_t)facts.actorIndex + 1ull : facts.sourceIdentityKey;
	}

	NRIVoxelProjectedBounds ProjectBounds(
		const NRIVoxelRepresentationFrameInput& frame,
		const NRIVoxelRepresentationFacts& facts,
		NRIVoxelRepresentationReason& outFailureReason)
	{
		NRIVoxelProjectedBounds projected = {};
		if (!facts.boundsValid ||
			!IsFinite3(facts.boundsMin) ||
			!IsFinite3(facts.boundsMax) ||
			facts.boundsMin[0] > facts.boundsMax[0] ||
			facts.boundsMin[1] > facts.boundsMax[1] ||
			facts.boundsMin[2] > facts.boundsMax[2])
		{
			outFailureReason = NRIVoxelRepresentationReason::MissingBounds;
			return projected;
		}
		if (!IsFiniteTransform(facts.transform))
		{
			outFailureReason = NRIVoxelRepresentationReason::InvalidTransform;
			return projected;
		}
		if (frame.renderWidth == 0u || frame.renderHeight == 0u ||
			!IsFinite3(frame.cameraPosition) ||
			!IsFinite3(frame.cameraForward) ||
			!IsFinite3(frame.cameraRight) ||
			!IsFinite3(frame.cameraUp) ||
			!IsFinite(frame.tanHalfFovX) || !IsFinite(frame.tanHalfFovY) ||
			frame.tanHalfFovX <= 0.0f || frame.tanHalfFovY <= 0.0f)
		{
			outFailureReason = NRIVoxelRepresentationReason::ProjectionUnavailable;
			return projected;
		}

		float minX = std::numeric_limits<float>::max();
		float minY = std::numeric_limits<float>::max();
		float maxX = std::numeric_limits<float>::lowest();
		float maxY = std::numeric_limits<float>::lowest();
		float nearestDepth = std::numeric_limits<float>::max();
		uint32_t projectedCornerCount = 0u;
		for (uint32_t corner = 0u; corner < 8u; ++corner)
		{
			const std::array<float, 3> local =
			{
				(corner & 1u) != 0u ? facts.boundsMax[0] : facts.boundsMin[0],
				(corner & 2u) != 0u ? facts.boundsMax[1] : facts.boundsMin[1],
				(corner & 4u) != 0u ? facts.boundsMax[2] : facts.boundsMin[2],
			};
			const std::array<float, 3> world = TransformPoint(facts.transform, local);
			const std::array<float, 3> relative = Subtract3(world, frame.cameraPosition);
			const float depth = Dot3(relative, frame.cameraForward);
			if (!IsFinite(depth) || depth <= ProjectionNearDepth)
			{
				projected.clippedByNearPlane = true;
				continue;
			}

			const float ndcX = Dot3(relative, frame.cameraRight) / (depth * frame.tanHalfFovX);
			const float ndcY = Dot3(relative, frame.cameraUp) / (depth * frame.tanHalfFovY);
			const float pixelX = (ndcX * 0.5f + 0.5f) * (float)frame.renderWidth;
			const float pixelY = (0.5f - ndcY * 0.5f) * (float)frame.renderHeight;
			if (!IsFinite(pixelX) || !IsFinite(pixelY))
			{
				continue;
			}
			minX = std::min(minX, pixelX);
			minY = std::min(minY, pixelY);
			maxX = std::max(maxX, pixelX);
			maxY = std::max(maxY, pixelY);
			nearestDepth = std::min(nearestDepth, depth);
			projectedCornerCount++;
		}

		if (projectedCornerCount == 0u)
		{
			outFailureReason = NRIVoxelRepresentationReason::BehindCamera;
			return projected;
		}

		projected.valid = true;
		if (projected.clippedByNearPlane)
		{
			// A box crossing the near plane can project beyond every screen edge.
			// Keep the diagnostic conservative so a later representation policy
			// cannot mistake a camera-intersecting occurrence for a small one.
			minX = 0.0f;
			minY = 0.0f;
			maxX = (float)frame.renderWidth;
			maxY = (float)frame.renderHeight;
		}
		projected.minX = minX;
		projected.minY = minY;
		projected.maxX = maxX;
		projected.maxY = maxY;
		projected.nearestDepth = nearestDepth;
		const float clippedMinX = std::max(minX, 0.0f);
		const float clippedMinY = std::max(minY, 0.0f);
		const float clippedMaxX = std::min(maxX, (float)frame.renderWidth);
		const float clippedMaxY = std::min(maxY, (float)frame.renderHeight);
		projected.widthPixels = std::max(clippedMaxX - clippedMinX, 0.0f);
		projected.heightPixels = std::max(clippedMaxY - clippedMinY, 0.0f);
		projected.areaPixels = projected.widthPixels * projected.heightPixels;
		projected.maxExtentPixels = std::max(projected.widthPixels, projected.heightPixels);
		projected.intersectsViewport = projected.widthPixels > 0.0f && projected.heightPixels > 0.0f;
		outFailureReason = NRIVoxelRepresentationReason::ExactOnly;
		return projected;
	}

	void AccumulateMaskCounts(
		const NRIVoxelRepresentationDecision& decision,
		NRIVoxelRepresentationSnapshot& snapshot)
	{
		snapshot.primaryOccurrenceCount += decision.primaryWorkloadMask != 0u ? 1u : 0u;
		snapshot.shadowOccurrenceCount += decision.shadowWorkloadMask != 0u ? 1u : 0u;
		snapshot.reflectionOccurrenceCount += decision.reflectionWorkloadMask != 0u ? 1u : 0u;
		snapshot.giOccurrenceCount += decision.giWorkloadMask != 0u ? 1u : 0u;
		snapshot.emissiveOccurrenceCount += decision.emissiveWorkloadMask != 0u ? 1u : 0u;
		snapshot.debugOccurrenceCount += decision.debugWorkloadMask != 0u ? 1u : 0u;
	}
}

void NRIVoxelRepresentationPolicy::BeginFrame(const NRIVoxelRepresentationFrameInput& input)
{
	const bool mapChanged = mHasFrame && mFrame.mapBuildSerial != input.mapBuildSerial;
	if (mapChanged)
	{
		mHysteresis.clear();
	}
	if (mHasFrame && !mapChanged && mFrame.frameIndex == input.frameIndex)
	{
		return;
	}

	if (!mapChanged && mHasFrame)
	{
		for (auto stateIt = mHysteresis.begin(); stateIt != mHysteresis.end();)
		{
			const uint32_t frameAge = input.frameIndex - stateIt->second.lastFrameIndex;
			if (frameAge > 1u)
			{
				stateIt = mHysteresis.erase(stateIt);
			}
			else
			{
				++stateIt;
			}
		}
	}

	mFrame = input;
	mHasFrame = true;
	std::vector<NRIVoxelRepresentationDecision> decisionStorage;
	decisionStorage.swap(mSnapshot.decisions);
	mSnapshot = {};
	mSnapshot.decisions.swap(decisionStorage);
	mSnapshot.decisions.clear();
	mSnapshot.mapBuildSerial = input.mapBuildSerial;
	mSnapshot.frameIndex = input.frameIndex;
}

NRIVoxelRepresentationDecision NRIVoxelRepresentationPolicy::EvaluateExact(
	const NRIVoxelRepresentationFacts& facts)
{
	NRIVoxelRepresentationDecision decision = {};
	decision.decisionIdentity = BuildDecisionIdentity(facts);
	decision.sourceIdentityKey = facts.sourceIdentityKey;
	decision.meshResourceKey = facts.meshResourceKey;
	decision.materialKeyHash = facts.materialKeyHash;
	decision.actorIndex = facts.actorIndex;
	decision.resolvedVoxelIndex = facts.resolvedVoxelIndex;
	decision.primitiveCount = facts.primitiveCount;
	decision.retainedFrameAge = facts.retainedFrameAge;
	decision.representation = NRIVoxelRepresentationKind::Exact;
	decision.requestedWorkloadMask = facts.workloadMask;
	decision.exactWorkloadMask = facts.workloadMask;
	decision.proxyWorkloadMask = 0u;
	decision.primaryWorkloadMask = (uint8_t)(facts.workloadMask & (uint8_t)NRI_TLAS_MASK_MAIN);
	decision.shadowWorkloadMask = (uint8_t)(facts.workloadMask & (uint8_t)NRI_TLAS_MASK_SHADOW);
	decision.reflectionWorkloadMask = (uint8_t)(facts.workloadMask & (uint8_t)NRI_TLAS_MASK_REFLECTION);
	decision.giWorkloadMask = (uint8_t)(facts.workloadMask & (uint8_t)NRI_TLAS_MASK_GI);
	decision.emissiveWorkloadMask = (uint8_t)(facts.workloadMask & (uint8_t)NRI_TLAS_MASK_EMISSIVE);
	decision.debugWorkloadMask = (uint8_t)(facts.workloadMask & (uint8_t)NRI_TLAS_MASK_DEBUG);
	decision.capturedThisFrame = facts.capturedThisFrame;
	decision.routedThroughSharedBlas = facts.routedThroughSharedBlas;
	decision.reason = facts.workloadMask == 0u ? NRIVoxelRepresentationReason::EmptyWorkload : NRIVoxelRepresentationReason::ExactOnly;
	if (facts.workloadMask != 0u)
	{
		decision.projectedBounds = ProjectBounds(mFrame, facts, decision.reason);
	}

	HysteresisState& state = mHysteresis[decision.decisionIdentity];
	const bool sameSource = state.valid && state.sourceIdentityKey == facts.sourceIdentityKey;
	const bool consecutiveFrame = sameSource && state.lastFrameIndex + 1u == mFrame.frameIndex;
	const bool sameFrame = sameSource && state.lastFrameIndex == mFrame.frameIndex;
	if (!sameFrame)
	{
		state.framesInExactState = consecutiveFrame ? state.framesInExactState + 1u : 1u;
		state.consecutiveProjectedFrames =
			consecutiveFrame && decision.projectedBounds.valid ?
				state.consecutiveProjectedFrames + 1u :
				(decision.projectedBounds.valid ? 1u : 0u);
		state.lastFrameIndex = mFrame.frameIndex;
		state.sourceIdentityKey = facts.sourceIdentityKey;
		state.valid = true;
	}
	decision.framesInExactState = state.framesInExactState;
	decision.consecutiveProjectedFrames = state.consecutiveProjectedFrames;
	decision.transitionCount = state.transitionCount;
	decision.hysteresisObservationReady =
		decision.consecutiveProjectedFrames >= HysteresisObservationFrameCount;
	// Slice 4.4 is deliberately observational. A valid projection and mature
	// hysteresis sample never authorize a representation transition.
	decision.proxyEligible = false;
	decision.proxyReady = false;
	decision.transitionReady = false;

	mSnapshot.decisionCount++;
	mSnapshot.exactDecisionCount++;
	mSnapshot.exactPrimitiveCount += facts.primitiveCount;
	mSnapshot.projectedDecisionCount += decision.projectedBounds.valid ? 1u : 0u;
	mSnapshot.viewportIntersectionCount += decision.projectedBounds.intersectsViewport ? 1u : 0u;
	mSnapshot.hysteresisObservationReadyCount += decision.hysteresisObservationReady ? 1u : 0u;
	mSnapshot.proxyReadyCount += decision.proxyReady ? 1u : 0u;
	AccumulateMaskCounts(decision, mSnapshot);
	mSnapshot.decisions.push_back(decision);
	return decision;
}

void NRIVoxelRepresentationPolicy::Reset()
{
	mFrame = {};
	mSnapshot = {};
	mHysteresis.clear();
	mHasFrame = false;
}

const char* GetNRIVoxelRepresentationKindName(NRIVoxelRepresentationKind kind)
{
	switch (kind)
	{
	case NRIVoxelRepresentationKind::Exact: return "exact";
	default: return "unknown";
	}
}

const char* GetNRIVoxelRepresentationReasonName(NRIVoxelRepresentationReason reason)
{
	switch (reason)
	{
	case NRIVoxelRepresentationReason::ExactOnly: return "exact-only";
	case NRIVoxelRepresentationReason::EmptyWorkload: return "empty-workload";
	case NRIVoxelRepresentationReason::MissingBounds: return "missing-bounds";
	case NRIVoxelRepresentationReason::InvalidTransform: return "invalid-transform";
	case NRIVoxelRepresentationReason::ProjectionUnavailable: return "projection-unavailable";
	case NRIVoxelRepresentationReason::BehindCamera: return "behind-camera";
	default: return "unknown";
	}
}
