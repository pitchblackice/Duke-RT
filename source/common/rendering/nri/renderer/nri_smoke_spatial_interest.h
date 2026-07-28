#pragma once

#include "nri_smoke_interest.h"

#include <cstdint>
#include <map>
#include <vector>

struct NRISmokeSpatialCoordinate
{
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;

	bool operator==(const NRISmokeSpatialCoordinate& other) const
	{
		return x == other.x && y == other.y && z == other.z;
	}
	bool operator<(const NRISmokeSpatialCoordinate& other) const
	{
		if (x != other.x) return x < other.x;
		if (y != other.y) return y < other.y;
		return z < other.z;
	}
};

enum class NRISmokeSpatialAuthority : uint8_t
{
	Fine,
	Coarse,
};

enum NRISmokeSpatialInterestReason : uint32_t
{
	NRISmokeSpatialReason_None = 0u,
	NRISmokeSpatialReason_MainView = 1u << 0u,
	NRISmokeSpatialReason_Portal = 1u << 1u,
	NRISmokeSpatialReason_Mirror = 1u << 2u,
	NRISmokeSpatialReason_HotDistance = 1u << 3u,
	NRISmokeSpatialReason_WarmDistance = 1u << 4u,
	NRISmokeSpatialReason_MovementPrefetch = 1u << 5u,
	NRISmokeSpatialReason_RecentPositive = 1u << 6u,
	NRISmokeSpatialReason_CameraCutGrace = 1u << 7u,
	NRISmokeSpatialReason_RuntimePortalUncertain = 1u << 8u,
	NRISmokeSpatialReason_IncompleteEvidence = 1u << 9u,
	NRISmokeSpatialReason_DiscoveryGrace = 1u << 10u,
	NRISmokeSpatialReason_EnterHysteresis = 1u << 11u,
	NRISmokeSpatialReason_LeaveHysteresis = 1u << 12u,
};

struct NRISmokeSpatialInterestRegion
{
	float boundsMin[3] = {};
	float boundsMax[3] = {};
	// One or more of MainView, Portal, and Mirror. These regions must come
	// from conservative sector/portal/mirror interest, not bare frustum tests.
	uint32_t reasons = NRISmokeSpatialReason_MainView;
};

struct NRISmokeSpatialBrickObservation
{
	NRISmokeSpatialCoordinate coordinate = {};
	uint32_t generation = 0u;
	NRISmokeSpatialAuthority authority = NRISmokeSpatialAuthority::Fine;
	bool occupied = false;
	float opticalMass = 0.0f;
	uint32_t lastSimulationFrame = 0u;
};

struct NRISmokeSpatialInterestConfig
{
	float hotEnterDistance = NRISmokeInterestTracker::HotEnterDistance;
	float hotLeaveDistance = NRISmokeInterestTracker::HotLeaveDistance;
	float warmEnterDistance = NRISmokeInterestTracker::WarmEnterDistance;
	float warmLeaveDistance = NRISmokeInterestTracker::WarmLeaveDistance;
	float maximumPrefetchDistance = NRISmokeInterestTracker::MaximumPrefetchDistance;
	float cameraCutDistance = NRISmokeInterestTracker::CameraJumpDistance;
	uint32_t recentPositiveFrames = NRISmokeInterestTracker::RecentVisibilityFrames;
	uint32_t cameraCutGraceFrames = NRISmokeInterestTracker::CameraJumpGraceFrames;
	uint32_t discoveryGraceFrames = 120u;
	uint32_t minimumDormantFrames = 240u;
	uint32_t stateRetentionFrames = 7200u;
};

struct NRISmokeSpatialInterestFrameInput
{
	uint32_t epoch = 0u;
	uint32_t rendererFrame = 0u;
	float brickWorldSize = 64.0f;
	float cameraPosition[3] = {};
	float previousCameraPosition[3] = {};
	bool hasPreviousCamera = false;
	// False when sector/portal/mirror interest could not be established. In
	// that state the owner may promote, but never publishes demotion work.
	bool conservativeInterestComplete = false;
	bool runtimePortalUncertain = false;
	uint32_t demotionQuantity = 0u;
	uint32_t promotionQuantity = 0u;
	std::vector<NRISmokeSpatialInterestRegion> positiveRegions;
	std::vector<NRISmokeSpatialBrickObservation> bricks;
};

struct NRISmokeSpatialCandidate
{
	NRISmokeSpatialCoordinate coordinate = {};
	uint32_t generation = 0u;
	NRISmokeSpatialAuthority authority = NRISmokeSpatialAuthority::Fine;
	NRISmokeInterestTier tier = NRISmokeInterestTier::Warm;
	uint32_t reasons = NRISmokeSpatialReason_None;
	uint32_t tierAgeFrames = 0u;
	uint32_t simulationAgeFrames = 0u;
	float opticalMass = 0.0f;
	uint32_t projectedRecoveredBricks = 0u;
};

struct NRISmokeSpatialInterestSnapshot
{
	uint32_t epoch = 0u;
	uint32_t rendererFrame = 0u;
	bool cameraCut = false;
	bool demotionEvidenceValid = false;
	uint32_t observed = 0u;
	uint32_t invalidObservations = 0u;
	uint32_t duplicateObservations = 0u;
	uint32_t hot = 0u;
	uint32_t warm = 0u;
	uint32_t dormant = 0u;
	uint32_t eligibleDemotions = 0u;
	uint32_t eligiblePromotions = 0u;
	uint32_t projectedRecoveredBricks = 0u;
	uint32_t selectedRecoveredBricks = 0u;
	std::vector<NRISmokeSpatialCandidate> demotions;
	std::vector<NRISmokeSpatialCandidate> promotions;
};

// Read-only classification owner. It publishes candidates and capacity
// projections only; it cannot release fine authority or claim coarse storage.
class NRISmokeSpatialInterestOwner
{
public:
	const NRISmokeSpatialInterestSnapshot& Update(
		const NRISmokeSpatialInterestFrameInput& input,
		const NRISmokeSpatialInterestConfig& config = {});
	void Reset(uint32_t epoch = 0u);

	const NRISmokeSpatialInterestSnapshot& GetSnapshot() const { return mSnapshot; }

private:
	struct BrickState
	{
		uint32_t generation = 0u;
		NRISmokeSpatialAuthority authority = NRISmokeSpatialAuthority::Fine;
		NRISmokeInterestTier tier = NRISmokeInterestTier::Warm;
		uint32_t firstObservedFrame = 0u;
		uint32_t lastObservedFrame = 0u;
		uint32_t lastPositiveFrame = UINT32_MAX;
		uint32_t tierSinceFrame = 0u;
		bool observed = false;
	};

	std::map<NRISmokeSpatialCoordinate, BrickState> mStates;
	NRISmokeSpatialInterestSnapshot mSnapshot = {};
	uint32_t mEpoch = 0u;
	uint32_t mLastFrame = 0u;
	float mCameraCutOrigin[3] = {};
	uint32_t mCameraCutGraceUntil = 0u;
};
