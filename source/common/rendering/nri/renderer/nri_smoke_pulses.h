#pragma once

#include "nri_smoke_contracts.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

struct NRISmokePulseSnapshot
{
	uint64_t enqueuedPulses = 0;
	uint64_t enqueuedMass = 0;
	uint64_t plannedRanges = 0;
	uint64_t plannedMass = 0;
	uint64_t committedRanges = 0;
	uint64_t committedMass = 0;
	uint64_t fallbackRetiredRanges = 0;
	uint64_t fallbackRetiredMass = 0;
	uint64_t deferredExpiredRanges = 0;
	uint64_t deferredExpiredMass = 0;
	uint64_t rollbackCount = 0;
	uint64_t resetPulses = 0;
	uint64_t resetMass = 0;
	uint32_t pendingRanges = 0;
	uint64_t pendingMass = 0;
	uint32_t authoredClockCount = 0;
	bool planActive = false;
};

// Owns authored smoke pulses until an immutable command-range plan has been
// recorded successfully. Planning never advances progress; Commit is the only
// operation that removes nominal mass, while Rollback leaves it available for
// a later plan.
class NRISmokePulseOwner
{
public:
	void Enqueue(const std::vector<NRISmokeInjectionCommandGpu>& commands,
		double authoredSimulationSeconds = 0.0);
	bool Plan(const std::vector<NRISmokeInjectionCommandGpu>& selected,
		std::vector<NRISmokeInjectionCommandGpu>& planned, uint64_t& token);
	bool Commit(uint64_t token);
	bool CommitRetaining(uint64_t token, const std::vector<NRISmokeInjectionCommandGpu>& retained);
	bool Acknowledge(uint32_t pulseIdLow, uint32_t pulseIdHigh, uint32_t rangeBegin, uint32_t rangeCount);
	bool RetireFallback(uint32_t pulseIdLow, uint32_t pulseIdHigh,
		uint32_t rangeBegin, uint32_t rangeCount);
	bool ExpireDeferred(uint32_t pulseIdLow, uint32_t pulseIdHigh,
		uint32_t rangeBegin, uint32_t rangeCount);
	bool Rollback(uint64_t token);
	void RebaseEpoch(uint32_t epoch);
	void RebaseSimulationClock(double oldSimulationSeconds, double newSimulationSeconds);
	uint32_t Reset();

	const std::vector<NRISmokeInjectionCommandGpu>& PendingCommands() const { return mPending; }
	const NRISmokePulseSnapshot& GetSnapshot() const { return mSnapshot; }
	double AuthoredSimulationSeconds(const NRISmokeInjectionCommandGpu& command,
		double fallbackSeconds) const;

private:
	static uint64_t PulseId(const NRISmokeInjectionCommandGpu& command);
	static void SetPulseId(NRISmokeInjectionCommandGpu& command, uint64_t pulseId);
	static uint64_t RangeEnd(const NRISmokeInjectionCommandGpu& command);
	enum class RetirementKind { GridCommitted, FallbackCompleted, DeferredExpired };
	bool RetireRange(uint32_t pulseIdLow, uint32_t pulseIdHigh,
		uint32_t rangeBegin, uint32_t rangeCount, RetirementKind kind);
	void ReleaseAuthoredTimeIfComplete(uint64_t pulseId);
	void RefreshPendingSnapshot();
	void ClearPlan();

	std::vector<NRISmokeInjectionCommandGpu> mPending;
	std::vector<NRISmokeInjectionCommandGpu> mPlan;
	std::unordered_map<uint64_t, double> mAuthoredSimulationSeconds;
	NRISmokePulseSnapshot mSnapshot = {};
	uint64_t mNextPulseId = 1;
	uint64_t mNextPlanToken = 1;
	uint64_t mActivePlanToken = 0;
};
