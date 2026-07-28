#include "nri_smoke_spatial_interest.h"

#include <algorithm>
#include <cmath>

namespace
{
	uint32_t Age(uint32_t current, uint32_t prior)
	{
		return current >= prior ? current - prior : 0u;
	}

	float DistanceSquared(const float left[3], const float right[3])
	{
		const float dx = left[0] - right[0];
		const float dy = left[1] - right[1];
		const float dz = left[2] - right[2];
		return dx * dx + dy * dy + dz * dz;
	}

	bool Finite3(const float value[3])
	{
		return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
	}

	bool ValidConfig(const NRISmokeSpatialInterestConfig& config)
	{
		return config.hotEnterDistance >= 0.0f && config.hotLeaveDistance >= config.hotEnterDistance &&
			config.warmEnterDistance >= config.hotEnterDistance &&
			config.warmLeaveDistance >= config.warmEnterDistance &&
			config.maximumPrefetchDistance >= 0.0f && config.cameraCutDistance >= 0.0f;
	}

	bool Intersects(const float brickMin[3], const float brickMax[3],
		const NRISmokeSpatialInterestRegion& region)
	{
		if (!Finite3(region.boundsMin) || !Finite3(region.boundsMax))
			return false;
		for (uint32_t axis = 0u; axis < 3u; ++axis)
		{
			if (region.boundsMin[axis] > region.boundsMax[axis] ||
				brickMax[axis] < region.boundsMin[axis] || brickMin[axis] > region.boundsMax[axis])
				return false;
		}
		return true;
	}

	bool DemotionOrder(const NRISmokeSpatialCandidate& left, const NRISmokeSpatialCandidate& right)
	{
		if (left.tierAgeFrames != right.tierAgeFrames) return left.tierAgeFrames > right.tierAgeFrames;
		if (left.opticalMass != right.opticalMass) return left.opticalMass < right.opticalMass;
		if (left.coordinate == right.coordinate) return left.generation < right.generation;
		return left.coordinate < right.coordinate;
	}

	bool PromotionOrder(const NRISmokeSpatialCandidate& left, const NRISmokeSpatialCandidate& right)
	{
		if (left.tier != right.tier) return left.tier == NRISmokeInterestTier::Hot;
		const uint32_t positiveMask = NRISmokeSpatialReason_MainView |
			NRISmokeSpatialReason_Portal | NRISmokeSpatialReason_Mirror;
		const bool leftPositive = (left.reasons & positiveMask) != 0u;
		const bool rightPositive = (right.reasons & positiveMask) != 0u;
		if (leftPositive != rightPositive) return leftPositive;
		if (left.simulationAgeFrames != right.simulationAgeFrames)
			return left.simulationAgeFrames > right.simulationAgeFrames;
		if (left.coordinate == right.coordinate) return left.generation < right.generation;
		return left.coordinate < right.coordinate;
	}
}

void NRISmokeSpatialInterestOwner::Reset(uint32_t epoch)
{
	mStates.clear();
	mSnapshot = {};
	mEpoch = epoch;
	mLastFrame = 0u;
	mCameraCutOrigin[0] = mCameraCutOrigin[1] = mCameraCutOrigin[2] = 0.0f;
	mCameraCutGraceUntil = 0u;
}

const NRISmokeSpatialInterestSnapshot& NRISmokeSpatialInterestOwner::Update(
	const NRISmokeSpatialInterestFrameInput& input,
	const NRISmokeSpatialInterestConfig& config)
{
	if (input.epoch == 0u || input.brickWorldSize <= 0.0f ||
		!std::isfinite(input.brickWorldSize) || !Finite3(input.cameraPosition) || !ValidConfig(config))
	{
		mSnapshot = {};
		mSnapshot.epoch = input.epoch;
		mSnapshot.rendererFrame = input.rendererFrame;
		mSnapshot.invalidObservations = static_cast<uint32_t>(input.bricks.size());
		return mSnapshot;
	}
	if (mEpoch != input.epoch || input.rendererFrame < mLastFrame)
		Reset(input.epoch);
	mLastFrame = input.rendererFrame;
	mSnapshot = {};
	mSnapshot.epoch = input.epoch;
	mSnapshot.rendererFrame = input.rendererFrame;
	mSnapshot.demotionEvidenceValid = input.conservativeInterestComplete && !input.runtimePortalUncertain;
	for (auto& entry : mStates)
		entry.second.observed = false;

	float predictedCamera[3] = { input.cameraPosition[0], input.cameraPosition[1], input.cameraPosition[2] };
	if (input.hasPreviousCamera && Finite3(input.previousCameraPosition))
	{
		const float cameraDeltaSquared = DistanceSquared(input.cameraPosition, input.previousCameraPosition);
		mSnapshot.cameraCut = cameraDeltaSquared >= config.cameraCutDistance * config.cameraCutDistance;
		if (mSnapshot.cameraCut)
		{
			std::copy(input.previousCameraPosition, input.previousCameraPosition + 3, mCameraCutOrigin);
			mCameraCutGraceUntil = input.rendererFrame + config.cameraCutGraceFrames;
		}
		else if (cameraDeltaSquared > 0.0f)
		{
			const float distance = std::sqrt(cameraDeltaSquared);
			const float scale = std::min(config.maximumPrefetchDistance / distance, 8.0f);
			for (uint32_t axis = 0u; axis < 3u; ++axis)
				predictedCamera[axis] += (input.cameraPosition[axis] - input.previousCameraPosition[axis]) * scale;
		}
	}

	std::vector<NRISmokeSpatialBrickObservation> observations = input.bricks;
	std::sort(observations.begin(), observations.end(), [](const auto& left, const auto& right)
	{
		if (left.coordinate == right.coordinate) return left.generation > right.generation;
		return left.coordinate < right.coordinate;
	});
	std::vector<NRISmokeSpatialCandidate> eligibleDemotions;
	std::vector<NRISmokeSpatialCandidate> eligiblePromotions;
	for (size_t index = 0u; index < observations.size(); ++index)
	{
		const auto& brick = observations[index];
		if (index != 0u && brick.coordinate == observations[index - 1u].coordinate)
		{
			mSnapshot.duplicateObservations++;
			continue;
		}
		if (brick.generation == 0u || !std::isfinite(brick.opticalMass) || brick.opticalMass < 0.0f)
		{
			mSnapshot.invalidObservations++;
			continue;
		}
		mSnapshot.observed++;
		BrickState& state = mStates[brick.coordinate];
		const bool authorityTransition = state.generation != 0u &&
			state.authority != brick.authority;
		const bool firstObservation = state.generation == 0u ||
			(state.generation != brick.generation && !authorityTransition);
		if (firstObservation)
		{
			state = {};
			state.generation = brick.generation;
			state.authority = brick.authority;
			state.tier = NRISmokeInterestTier::Warm;
			state.firstObservedFrame = input.rendererFrame;
			state.tierSinceFrame = input.rendererFrame;
		}
		else if (authorityTransition)
		{
			// Publish-before-release and rehydrate-before-retire change physical
			// generations without changing the logical smoke at this coordinate.
			// Preserve hysteresis so a new archive is not immediately promoted by
			// discovery grace (or a rehydrated brick immediately re-demoted).
			state.generation = brick.generation;
			state.authority = brick.authority;
		}
		state.observed = true;
		state.lastObservedFrame = input.rendererFrame;

		float brickMin[3];
		float brickMax[3];
		float center[3];
		const int32_t coordinate[3] = { brick.coordinate.x, brick.coordinate.y, brick.coordinate.z };
		for (uint32_t axis = 0u; axis < 3u; ++axis)
		{
			brickMin[axis] = static_cast<float>(coordinate[axis]) * input.brickWorldSize;
			brickMax[axis] = brickMin[axis] + input.brickWorldSize;
			center[axis] = (brickMin[axis] + brickMax[axis]) * 0.5f;
		}
		uint32_t reasons = NRISmokeSpatialReason_None;
		for (const auto& region : input.positiveRegions)
		{
			if (Intersects(brickMin, brickMax, region))
				reasons |= region.reasons & (NRISmokeSpatialReason_MainView |
					NRISmokeSpatialReason_Portal | NRISmokeSpatialReason_Mirror);
		}
		const bool positive = reasons != NRISmokeSpatialReason_None;
		if (positive)
			state.lastPositiveFrame = input.rendererFrame;
		const bool recentPositive = state.lastPositiveFrame != UINT32_MAX &&
			Age(input.rendererFrame, state.lastPositiveFrame) <= config.recentPositiveFrames;
		const bool discoveryGrace = Age(input.rendererFrame, state.firstObservedFrame) <= config.discoveryGraceFrames;
		const bool wasHot = state.tier == NRISmokeInterestTier::Hot;
		const bool wasDormant = state.tier == NRISmokeInterestTier::Dormant;
		const float hotDistance = wasHot ? config.hotLeaveDistance : config.hotEnterDistance;
		const float warmDistance = !wasDormant ? config.warmLeaveDistance : config.warmEnterDistance;
		const bool nearHot = DistanceSquared(center, input.cameraPosition) <= hotDistance * hotDistance;
		const bool nearWarm = DistanceSquared(center, input.cameraPosition) <= warmDistance * warmDistance;
		const bool prefetch = DistanceSquared(center, predictedCamera) <= config.hotEnterDistance * config.hotEnterDistance;
		const bool cameraCutGrace = input.rendererFrame <= mCameraCutGraceUntil &&
			DistanceSquared(center, mCameraCutOrigin) <= config.warmLeaveDistance * config.warmLeaveDistance;
		if (nearHot) reasons |= NRISmokeSpatialReason_HotDistance;
		if (nearWarm) reasons |= NRISmokeSpatialReason_WarmDistance;
		if (prefetch) reasons |= NRISmokeSpatialReason_MovementPrefetch;
		if (recentPositive) reasons |= NRISmokeSpatialReason_RecentPositive;
		if (cameraCutGrace) reasons |= NRISmokeSpatialReason_CameraCutGrace;
		if (discoveryGrace) reasons |= NRISmokeSpatialReason_DiscoveryGrace;
		if (input.runtimePortalUncertain) reasons |= NRISmokeSpatialReason_RuntimePortalUncertain;
		if (!input.conservativeInterestComplete) reasons |= NRISmokeSpatialReason_IncompleteEvidence;
		if ((wasHot && nearHot) || (!wasDormant && nearWarm)) reasons |= NRISmokeSpatialReason_LeaveHysteresis;
		if ((!wasHot && nearHot) || (wasDormant && nearWarm)) reasons |= NRISmokeSpatialReason_EnterHysteresis;

		NRISmokeInterestTier tier;
		if (positive || nearHot)
			tier = NRISmokeInterestTier::Hot;
		else if (prefetch || nearWarm || recentPositive || cameraCutGrace || discoveryGrace ||
			input.runtimePortalUncertain || !input.conservativeInterestComplete)
			tier = NRISmokeInterestTier::Warm;
		else
			tier = NRISmokeInterestTier::Dormant;
		if (tier != state.tier)
		{
			state.tier = tier;
			state.tierSinceFrame = input.rendererFrame;
		}
		if (tier == NRISmokeInterestTier::Hot) mSnapshot.hot++;
		else if (tier == NRISmokeInterestTier::Warm) mSnapshot.warm++;
		else mSnapshot.dormant++;

		NRISmokeSpatialCandidate candidate = {};
		candidate.coordinate = brick.coordinate;
		candidate.generation = brick.generation;
		candidate.authority = brick.authority;
		candidate.tier = tier;
		candidate.reasons = reasons;
		candidate.tierAgeFrames = Age(input.rendererFrame, state.tierSinceFrame);
		candidate.simulationAgeFrames = Age(input.rendererFrame, brick.lastSimulationFrame);
		candidate.opticalMass = brick.opticalMass;
		if (brick.authority == NRISmokeSpatialAuthority::Fine && brick.occupied &&
			tier == NRISmokeInterestTier::Dormant && mSnapshot.demotionEvidenceValid &&
			candidate.tierAgeFrames >= config.minimumDormantFrames)
		{
			candidate.projectedRecoveredBricks = 1u;
			eligibleDemotions.push_back(candidate);
		}
		else if (brick.authority == NRISmokeSpatialAuthority::Coarse && brick.occupied &&
			tier != NRISmokeInterestTier::Dormant)
		{
			eligiblePromotions.push_back(candidate);
		}
	}

	for (auto it = mStates.begin(); it != mStates.end(); )
	{
		if (!it->second.observed && Age(input.rendererFrame, it->second.lastObservedFrame) > config.stateRetentionFrames)
			it = mStates.erase(it);
		else
			++it;
	}
	std::sort(eligibleDemotions.begin(), eligibleDemotions.end(), DemotionOrder);
	std::sort(eligiblePromotions.begin(), eligiblePromotions.end(), PromotionOrder);
	mSnapshot.eligibleDemotions = static_cast<uint32_t>(eligibleDemotions.size());
	mSnapshot.eligiblePromotions = static_cast<uint32_t>(eligiblePromotions.size());
	mSnapshot.projectedRecoveredBricks = mSnapshot.eligibleDemotions;
	const size_t demotionCount = std::min<size_t>(input.demotionQuantity, eligibleDemotions.size());
	const size_t promotionCount = std::min<size_t>(input.promotionQuantity, eligiblePromotions.size());
	mSnapshot.demotions.assign(eligibleDemotions.begin(), eligibleDemotions.begin() + demotionCount);
	mSnapshot.promotions.assign(eligiblePromotions.begin(), eligiblePromotions.begin() + promotionCount);
	mSnapshot.selectedRecoveredBricks = static_cast<uint32_t>(mSnapshot.demotions.size());
	return mSnapshot;
}
