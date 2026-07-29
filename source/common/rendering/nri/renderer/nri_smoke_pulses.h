#pragma once

#include "nri_smoke_contracts.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

enum class NRISmokePulseQueuePolicy : uint8_t
{
	Retry,
	Latest,
};

// CPU-only authoring data. This deliberately stays beside the GPU command
// instead of consuming command ABI fields used by grid injection.
struct NRISmokePulseEnqueueInfo
{
	double authoredSimulationSeconds = -1.0;
	double authoredGameplaySeconds = -1.0;
	float maximumLatencySeconds = 0.0f;
	NRISmokePulseQueuePolicy queuePolicy = NRISmokePulseQueuePolicy::Retry;
	bool transitory = false;
	uint64_t analyticBridgeSourceKey = 0u;
	uint64_t analyticBridgeSegmentRevision = 0u;
};

struct NRISmokePulseBridgeIdentity
{
	uint64_t sourceKey = 0u;
	uint64_t segmentRevision = 0u;
	uint32_t epoch = 0u;
	bool Valid() const { return sourceKey != 0u && segmentRevision != 0u; }
};

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
	uint64_t supersededPulses = 0;
	uint64_t supersededMass = 0;
	uint64_t staleDroppedPulses = 0;
	uint64_t staleDroppedMass = 0;
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
	void Enqueue(const std::vector<NRISmokeInjectionCommandGpu>& commands,
		const std::vector<NRISmokePulseEnqueueInfo>& enqueueInfo,
		double fallbackSimulationSeconds, double fallbackGameplaySeconds);
	uint32_t ExpireStale(double gameplaySeconds);
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
	bool MarkVisible(uint32_t pulseIdLow, uint32_t pulseIdHigh,
		uint32_t rangeBegin, uint32_t rangeCount);

	const std::vector<NRISmokeInjectionCommandGpu>& PendingCommands() const { return mPending; }
	const NRISmokePulseSnapshot& GetSnapshot() const { return mSnapshot; }
	double AuthoredSimulationSeconds(const NRISmokeInjectionCommandGpu& command,
		double fallbackSeconds) const;
	NRISmokePulseBridgeIdentity BridgeIdentity(
		const NRISmokeInjectionCommandGpu& command) const;

private:
	struct PulseState
	{
		double authoredSimulationSeconds = 0.0;
		double authoredGameplaySeconds = 0.0;
		float maximumLatencySeconds = 0.0f;
		NRISmokePulseQueuePolicy queuePolicy = NRISmokePulseQueuePolicy::Retry;
		bool transitory = false;
		bool visible = false;
		uint64_t analyticBridgeSourceKey = 0u;
		uint64_t analyticBridgeSegmentRevision = 0u;
	};

	static uint64_t PulseId(const NRISmokeInjectionCommandGpu& command);
	static uint64_t SourceKey(const NRISmokeInjectionCommandGpu& command);
	static void SetPulseId(NRISmokeInjectionCommandGpu& command, uint64_t pulseId);
	static uint64_t RangeEnd(const NRISmokeInjectionCommandGpu& command);
	enum class RetirementKind { GridCommitted, FallbackCompleted, DeferredExpired };
	bool RetireRange(uint32_t pulseIdLow, uint32_t pulseIdHigh,
		uint32_t rangeBegin, uint32_t rangeCount, RetirementKind kind);
	void ReleaseAuthoredTimeIfComplete(uint64_t pulseId);
	bool IsStale(const PulseState& state, double gameplaySeconds) const;
	void RetireUnpublishedPulse(uint64_t pulseId, bool superseded);
	void RefreshPendingSnapshot();
	void ClearPlan();

	std::vector<NRISmokeInjectionCommandGpu> mPending;
	std::vector<NRISmokeInjectionCommandGpu> mPlan;
	std::unordered_map<uint64_t, PulseState> mPulseStates;
	NRISmokePulseSnapshot mSnapshot = {};
	uint64_t mNextPulseId = 1;
	uint64_t mNextPlanToken = 1;
	uint64_t mActivePlanToken = 0;
};
