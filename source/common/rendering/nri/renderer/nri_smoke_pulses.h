#pragma once

#include "nri_smoke_contracts.h"

#include <cstdint>
#include <vector>

struct NRISmokePulseSnapshot
{
	uint64_t enqueuedPulses = 0;
	uint64_t enqueuedMass = 0;
	uint64_t plannedRanges = 0;
	uint64_t plannedMass = 0;
	uint64_t committedRanges = 0;
	uint64_t committedMass = 0;
	uint64_t rollbackCount = 0;
	uint64_t resetPulses = 0;
	uint64_t resetMass = 0;
	uint32_t pendingRanges = 0;
	uint64_t pendingMass = 0;
	bool planActive = false;
};

// Owns authored smoke pulses until an immutable command-range plan has been
// recorded successfully. Planning never advances progress; Commit is the only
// operation that removes nominal mass, while Rollback leaves it available for
// a later plan.
class NRISmokePulseOwner
{
public:
	void Enqueue(const std::vector<NRISmokeInjectionCommandGpu>& commands);
	bool Plan(const std::vector<NRISmokeInjectionCommandGpu>& selected,
		std::vector<NRISmokeInjectionCommandGpu>& planned, uint64_t& token);
	bool Commit(uint64_t token);
	bool Rollback(uint64_t token);
	void RebaseEpoch(uint32_t epoch);
	uint32_t Reset();

	const std::vector<NRISmokeInjectionCommandGpu>& PendingCommands() const { return mPending; }
	const NRISmokePulseSnapshot& GetSnapshot() const { return mSnapshot; }

private:
	static uint64_t PulseId(const NRISmokeInjectionCommandGpu& command);
	static void SetPulseId(NRISmokeInjectionCommandGpu& command, uint64_t pulseId);
	static uint64_t RangeEnd(const NRISmokeInjectionCommandGpu& command);
	void RefreshPendingSnapshot();
	void ClearPlan();

	std::vector<NRISmokeInjectionCommandGpu> mPending;
	std::vector<NRISmokeInjectionCommandGpu> mPlan;
	NRISmokePulseSnapshot mSnapshot = {};
	uint64_t mNextPulseId = 1;
	uint64_t mNextPlanToken = 1;
	uint64_t mActivePlanToken = 0;
};
