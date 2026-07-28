#pragma once

#include "nri_smoke_contracts.h"
#include "nri_smoke_grid_contracts.h"

#include <cstdint>
#include <array>
#include <vector>

class NRISmokePulseOwner;

struct NRISmokePromptRangeIdentity
{
	uint32_t pulseIdLow = 0;
	uint32_t pulseIdHigh = 0;
	uint32_t rangeBegin = 0;
	uint32_t rangeCount = 0;
};

struct NRISmokePromptFallbackSnapshot
{
	uint64_t authoredPendingMass = 0;
	uint64_t committedDepositedMass = 0;
	uint64_t fallbackCarrierMass = 0;
	uint64_t scheduledFallbackRanges = 0;
	uint64_t executedFallbackRanges = 0;
	uint64_t gridHandoffs = 0;
	uint64_t expiredFallbackRanges = 0;
	uint64_t expiredFallbackMass = 0;
	uint64_t expiryAcknowledgeFailures = 0;
	uint64_t rollbackCount = 0;
	uint64_t internalErrors = 0;
	uint64_t promptDeferredRanges = 0;
	uint64_t promptDeferredMass = 0;
	uint64_t promptDeferredBrickWork = 0;
	uint32_t scheduledFallbackQuantity = 0;
	uint32_t maximumFallbackCarrierQuantity = 8;
	uint32_t activeFallbackSlots = 0;
	uint32_t oldestActiveAgeMilliseconds = 0;
	uint64_t authoredRendererFrame = 0;
	uint64_t publishedVisibleRendererFrame = 0;
};

struct NRISmokePromptPrepareResult
{
	uint32_t deferredRanges = 0;
	uint64_t deferredMass = 0;
	uint64_t deferredBrickWork = 0;
};

// Owns the fixed first-use transaction. A planned interactive range retains
// CPU pulse authority until the grid reports a committed deposit. Until that
// handoff, the GPU publishes the same stable range as a coarse source kernel.
class NRISmokePromptFallback
{
public:
	static constexpr uint32_t FixedFallbackCarrierQuantity = 8u;

	NRISmokePromptPrepareResult Prepare(std::vector<NRISmokeInjectionCommandGpu>& commands,
		uint64_t rendererFrame, double simulationTimeSeconds, const std::vector<NRISmokeStyleGpu>& styles,
		float gridCellSize, std::vector<NRISmokePromptRangeIdentity>& retained,
		uint32_t maximumFallbackCarrierQuantity = FixedFallbackCarrierQuantity);
	void RetireExpired(NRISmokePulseOwner& pulses, double simulationTimeSeconds);
	void CommitGridHandoffs(NRISmokePulseOwner& pulses,
		const std::vector<NRISmokePromptOutcomeGpu>& outcomes);
	void Commit(uint64_t rendererFrame);
	void Rollback();
	void Reset();

	const NRISmokePromptFallbackSnapshot& GetSnapshot() const { return mSnapshot; }

private:
	struct ActiveSlot
	{
		NRISmokePromptRangeIdentity identity = {};
		double authoredSimulationSeconds = 0.0;
		float lifetimeSeconds = 0.0f;
	};
	void RefreshActiveSnapshot(double simulationTimeSeconds);

	NRISmokePromptFallbackSnapshot mSnapshot = {};
	std::vector<NRISmokePromptRangeIdentity> mPlan;
	std::vector<uint32_t> mNewlyClaimedSlots;
	std::array<ActiveSlot, FixedFallbackCarrierQuantity> mActiveSlots = {};
};
