#pragma once

#include "nri_smoke_dormant_summary_contracts.h"

#include <cstdint>
#include <vector>

struct NRISmokeDormantSummaryToken
{
	uint32_t slot = UINT32_MAX;
	uint32_t generation = 0;
	uint32_t epoch = 0;

	bool IsValid() const { return slot != UINT32_MAX && generation != 0u && epoch != 0u; }
};

struct NRISmokeDormantSummaryStatus
{
	bool productionEnabled = false;
	uint32_t capacity = 0;
	uint32_t free = 0;
	uint32_t claimed = 0;
	uint32_t archived = 0;
	uint32_t rehydrating = 0;
	uint64_t claimFailures = 0;
	uint64_t publishRejects = 0;
	uint64_t staleRejects = 0;
	uint64_t rehydrateAttempts = 0;
	uint64_t rehydrateCommits = 0;
	uint64_t rehydrateRetained = 0;
};

struct NRISmokeDormantRehydrateWork
{
	NRISmokeDormantSummaryToken token = {};
	NRISmokeDormantSummaryGpu summary = {};
};

// Fixed-capacity transactional owner for the dormant-summary experiment.
// It never decides that fine smoke may be released. The future archive pass
// must first Claim and Publish, then release the matching fine generation.
// Rehydration similarly frees a row only after Commit(..., true).
class NRISmokeDormantSummaryOwner
{
public:
	explicit NRISmokeDormantSummaryOwner(uint32_t capacity = 0u);

	void Reset(uint32_t epoch);
	NRISmokeDormantSummaryToken Claim(uint32_t epoch);
	bool Publish(const NRISmokeDormantSummaryToken& token, const NRISmokeDormantSummaryGpu& summary);
	bool CancelClaim(const NRISmokeDormantSummaryToken& token);

	std::vector<NRISmokeDormantRehydrateWork> BeginRehydrate(
		uint32_t epoch, uint32_t currentTick, uint32_t fixedQuantity,
		float secondsPerTick, float densityHalfLifeSeconds, float thermalHalfLifeSeconds);
	bool CommitRehydrate(const NRISmokeDormantSummaryToken& token, bool finePublished);
	bool CancelRehydrate(const NRISmokeDormantSummaryToken& token);

	const NRISmokeDormantSummaryStatus& GetStatus() const { return mStatus; }
	static float ExactDecay(uint32_t elapsedTicks, float secondsPerTick, float halfLifeSeconds);

private:
	enum class SlotState : uint8_t { Free, Claimed, Archived, Rehydrating };
	struct Slot
	{
		SlotState state = SlotState::Free;
		uint32_t generation = 0;
		uint32_t epoch = 0;
		NRISmokeDormantSummaryGpu summary = {};
	};

	Slot* Resolve(const NRISmokeDormantSummaryToken& token, SlotState expected);
	void RefreshStatus();

	std::vector<Slot> mSlots;
	NRISmokeDormantSummaryStatus mStatus = {};
	uint32_t mEpoch = 0;
	uint32_t mNextGeneration = 1u;
	uint32_t mRehydrateCursor = 0u;
};
