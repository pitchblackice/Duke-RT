#pragma once

#include "nri_smoke_analytic_carriers.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

struct NRISmokeAnalyticTrailPoint
{
	float position[3] = {};
	uint32_t rangeCount = 1u;
};

struct NRISmokeAnalyticTrailObservation
{
	uint64_t stableSourceKey = 0u;
	uint64_t updateOrdinal = 0u;
	uint32_t epoch = 0u;
	uint32_t sourceId = 0u;
	uint32_t styleIndex = 0u;
	double authoredGameplaySeconds = 0.0;
	float velocity[3] = {};
	float radius = 0.0f;
	float densityScale = 0.0f;
	float expansionVelocity = 0.0f;
	float densityHalfLife = 0.0f;
	float presentationLifetimeSeconds = 0.0f;
	float maximumLatencySeconds = 0.0f;
	float maximumSegmentLength = 0.0f;
	bool persistentSource = false;
};

struct NRISmokeAnalyticTrailObservationBatch
{
	NRISmokeAnalyticTrailObservation observation = {};
	std::vector<NRISmokeAnalyticTrailPoint> points;
};

enum class NRISmokeAnalyticTrailObserveResult : uint8_t
{
	Accepted = 0u,
	Invalid,
	PersistentSource,
	StaleEpoch,
	StaleUpdate,
	ExpiredOnArrival,
	Capacity,
};

struct NRISmokeAnalyticTrailPresentation
{
	NRISmokeAnalyticCarrierRequest carrier = {};
	uint64_t stableSourceKey = 0u;
	uint64_t segmentRevision = 0u;
	uint64_t lightingGroupKey = 0u;
	uint32_t lightingGroupGeneration = 0u;
	uint32_t coalescedPointCount = 0u;
};

struct NRISmokeAnalyticTrailHandoffToken
{
	uint64_t stableSourceKey = 0u;
	uint64_t segmentRevision = 0u;
	uint64_t serial = 0u;
	uint32_t epoch = 0u;

	bool Valid() const { return stableSourceKey != 0u && serial != 0u; }
};

struct NRISmokeAnalyticTrailGridReceipt
{
	uint64_t stableSourceKey = 0u;
	uint64_t segmentRevision = 0u;
	uint32_t epoch = 0u;
	uint32_t depositedRangeCount = 0u;
	uint32_t gridAuthorityGeneration = 0u;
};

struct NRISmokeAnalyticTrailBridgeSnapshot
{
	uint64_t observations = 0u;
	uint64_t accepted = 0u;
	uint64_t replacements = 0u;
	uint64_t rejectedInvalid = 0u;
	uint64_t rejectedPersistent = 0u;
	uint64_t rejectedStaleEpoch = 0u;
	uint64_t rejectedStaleUpdate = 0u;
	uint64_t rejectedExpired = 0u;
	uint64_t rejectedCapacity = 0u;
	uint64_t expired = 0u;
	uint64_t retired = 0u;
	uint64_t fallbackPublications = 0u;
	uint64_t handoffsBegun = 0u;
	uint64_t handoffsCommitted = 0u;
	uint64_t handoffsCancelled = 0u;
	uint64_t handoffsRejected = 0u;
	uint64_t discardedOldPoints = 0u;
	uint32_t epoch = 0u;
	uint32_t activeSources = 0u;
	uint32_t highWaterSources = 0u;
};

// Owns a bounded, newest-only analytic presentation for transitory projectile
// trails. Each source owns at most one rounded segment and one stable lighting
// identity. Persistent smoke is deliberately outside this owner.
class NRISmokeAnalyticTrailBridge
{
public:
	static constexpr uint32_t MaximumSources = 64u;
	static constexpr uint32_t MaximumPointsPerUpdate = 16u;
	static constexpr uint32_t MaximumRangeCountPerPoint = 16u;
	static constexpr float MaximumPresentationLifetimeSeconds = 0.5f;

	void Reset(uint32_t epoch);
	void BeginFrame(double gameplayTimeSeconds, uint32_t epoch);
	NRISmokeAnalyticTrailObserveResult Observe(
		const NRISmokeAnalyticTrailObservation& observation,
		const NRISmokeAnalyticTrailPoint* points, size_t pointCount);
	bool RetireSource(uint64_t stableSourceKey, uint32_t epoch);
	bool PublishFallback(uint64_t stableSourceKey, uint64_t segmentRevision,
		uint32_t epoch);
	bool CommitExactGridRange(uint64_t stableSourceKey, uint64_t segmentRevision,
		uint32_t epoch, uint32_t depositedRangeCount);

	NRISmokeAnalyticTrailHandoffToken BeginExactGridHandoff(
		uint64_t stableSourceKey, uint64_t segmentRevision);
	bool CommitExactGridHandoff(const NRISmokeAnalyticTrailHandoffToken& token,
		const NRISmokeAnalyticTrailGridReceipt& receipt);
	bool CancelExactGridHandoff(const NRISmokeAnalyticTrailHandoffToken& token);

	const std::vector<NRISmokeAnalyticTrailPresentation>& GetPresentations() const
	{
		return mPresentations;
	}
	const NRISmokeAnalyticTrailBridgeSnapshot& GetSnapshot() const { return mSnapshot; }

private:
	struct State
	{
		NRISmokeAnalyticTrailPresentation presentation = {};
		uint64_t updateOrdinal = 0u;
		uint64_t nextSegmentRevision = 1u;
		uint32_t depositedRangeCount = 0u;
		uint32_t committedRangeCount = 0u;
		bool published = false;
		NRISmokeAnalyticTrailHandoffToken pendingHandoff = {};
	};

	void Refresh();
	NRISmokeAnalyticTrailObserveResult Reject(NRISmokeAnalyticTrailObserveResult result);
	bool TokenMatches(const State& state, const NRISmokeAnalyticTrailHandoffToken& token) const;

	std::map<uint64_t, State> mStates;
	std::vector<NRISmokeAnalyticTrailPresentation> mPresentations;
	NRISmokeAnalyticTrailBridgeSnapshot mSnapshot = {};
	double mGameplayTimeSeconds = 0.0;
	uint64_t mNextHandoffSerial = 1u;
	uint32_t mNextLightingGroupGeneration = 1u;
};
