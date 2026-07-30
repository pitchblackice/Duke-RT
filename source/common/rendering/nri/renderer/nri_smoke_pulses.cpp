#include "nri_smoke_pulses.h"

#include "nri_smoke_admission.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

uint64_t NRISmokePulseOwner::PulseId(const NRISmokeInjectionCommandGpu& command)
{
	return (uint64_t(command.pulseIdHigh) << 32u) | command.pulseIdLow;
}

uint64_t NRISmokePulseOwner::SourceKey(const NRISmokeInjectionCommandGpu& command)
{
	constexpr uint32_t presentationMask = NRI_SMOKE_SOURCE_METADATA_PROMPT_SLOT_MASK |
		NRI_SMOKE_SOURCE_METADATA_PROMPT_ELIGIBLE;
	return (uint64_t(command.sourceMetadata & ~presentationMask) << 32u) | command.sourceId;
}

void NRISmokePulseOwner::SetPulseId(NRISmokeInjectionCommandGpu& command, uint64_t pulseId)
{
	command.pulseIdLow = (uint32_t)pulseId;
	command.pulseIdHigh = (uint32_t)(pulseId >> 32u);
}

uint64_t NRISmokePulseOwner::RangeEnd(const NRISmokeInjectionCommandGpu& command)
{
	return uint64_t(command.rangeBegin) + command.rangeCount;
}

void NRISmokePulseOwner::RefreshPendingSnapshot()
{
	mSnapshot.pendingRanges = (uint32_t)mPending.size();
	mSnapshot.pendingMass = 0;
	for (const auto& command : mPending)
		mSnapshot.pendingMass += command.rangeCount;
	mSnapshot.authoredClockCount = (uint32_t)mPulseStates.size();
	mSnapshot.planActive = mActivePlanToken != 0;
}

void NRISmokePulseOwner::ClearPlan()
{
	mPlan.clear();
	mActivePlanToken = 0;
	RefreshPendingSnapshot();
}

void NRISmokePulseOwner::Enqueue(const std::vector<NRISmokeInjectionCommandGpu>& commands,
	double authoredSimulationSeconds)
{
	Enqueue(commands, {}, authoredSimulationSeconds, authoredSimulationSeconds);
}

void NRISmokePulseOwner::Enqueue(const std::vector<NRISmokeInjectionCommandGpu>& commands,
	const std::vector<NRISmokePulseEnqueueInfo>& enqueueInfo,
	double fallbackSimulationSeconds, double fallbackGameplaySeconds)
{
	std::vector<PulseState> states(commands.size());
	std::unordered_set<uint64_t> supersedingSources;
	for (size_t index = 0; index < commands.size(); ++index)
	{
		const NRISmokePulseEnqueueInfo info = index < enqueueInfo.size() ?
			enqueueInfo[index] : NRISmokePulseEnqueueInfo {};
		PulseState& state = states[index];
		state.authoredSimulationSeconds = info.authoredSimulationSeconds >= 0.0 ?
			info.authoredSimulationSeconds : fallbackSimulationSeconds;
		state.authoredGameplaySeconds = info.authoredGameplaySeconds >= 0.0 ?
			info.authoredGameplaySeconds : fallbackGameplaySeconds;
		state.maximumLatencySeconds = std::max(info.maximumLatencySeconds, 0.0f);
		state.queuePolicy = info.queuePolicy;
		state.transitory = info.transitory;
		state.analyticBridgeSourceKey = info.analyticBridgeSourceKey;
		state.analyticBridgeSegmentRevision = info.analyticBridgeSegmentRevision;
		if (commands[index].count != 0u && state.transitory &&
			state.queuePolicy == NRISmokePulseQueuePolicy::Latest &&
			!IsStale(state, fallbackGameplaySeconds))
			supersedingSources.insert(SourceKey(commands[index]));
	}

	// Treat one Gather result as the newest coherent batch. Commands within the
	// batch (for example, the points making one RPG trail segment) do not evict
	// each other; they only replace older, never-published batches.
	if (mActivePlanToken == 0u && !supersedingSources.empty())
	{
		std::vector<uint64_t> superseded;
		for (const auto& command : mPending)
		{
			const uint64_t pulseId = PulseId(command);
			const auto found = mPulseStates.find(pulseId);
			if (found == mPulseStates.end() || found->second.visible || !found->second.transitory ||
				found->second.queuePolicy != NRISmokePulseQueuePolicy::Latest ||
				supersedingSources.count(SourceKey(command)) == 0u)
				continue;
			if (std::find(superseded.begin(), superseded.end(), pulseId) == superseded.end())
				superseded.push_back(pulseId);
		}
		for (const uint64_t pulseId : superseded)
			RetireUnpublishedPulse(pulseId, true);
	}

	for (size_t index = 0; index < commands.size(); ++index)
	{
		const auto& authored = commands[index];
		if (authored.count == 0u)
			continue;
		const PulseState& state = states[index];
		if (state.transitory && state.queuePolicy == NRISmokePulseQueuePolicy::Latest &&
			IsStale(state, fallbackGameplaySeconds))
		{
			mSnapshot.staleDroppedPulses++;
			mSnapshot.staleDroppedMass += authored.count;
			continue;
		}
		NRISmokeInjectionCommandGpu command = authored;
		command.rangeBegin = 0u;
		command.rangeCount = command.count;
		uint64_t pulseId = mNextPulseId++;
		if (pulseId == 0u)
			pulseId = mNextPulseId++;
		SetPulseId(command, pulseId);
		mPending.push_back(command);
		if (NRIIsInteractiveSmokeSource(command.sourceMetadata) || state.transitory ||
			state.queuePolicy != NRISmokePulseQueuePolicy::Retry)
			mPulseStates[pulseId] = state;
		mSnapshot.enqueuedPulses++;
		mSnapshot.enqueuedMass += command.rangeCount;
	}
	RefreshPendingSnapshot();
}

NRISmokePulseBridgeIdentity NRISmokePulseOwner::BridgeIdentity(
	const NRISmokeInjectionCommandGpu& command) const
{
	const auto found = mPulseStates.find(PulseId(command));
	if (found == mPulseStates.end()) return {};
	return { found->second.analyticBridgeSourceKey,
		found->second.analyticBridgeSegmentRevision, command.epoch };
}

bool NRISmokePulseOwner::IsStale(const PulseState& state, double gameplaySeconds) const
{
	return state.maximumLatencySeconds > 0.0f &&
		gameplaySeconds - state.authoredGameplaySeconds > state.maximumLatencySeconds;
}

void NRISmokePulseOwner::RetireUnpublishedPulse(uint64_t pulseId, bool superseded)
{
	uint64_t mass = 0u;
	mPending.erase(std::remove_if(mPending.begin(), mPending.end(), [&](const auto& command)
	{
		if (PulseId(command) != pulseId) return false;
		mass += command.rangeCount;
		return true;
	}), mPending.end());
	if (mass == 0u) return;
	if (superseded)
	{
		mSnapshot.supersededPulses++;
		mSnapshot.supersededMass += mass;
	}
	else
	{
		mSnapshot.staleDroppedPulses++;
		mSnapshot.staleDroppedMass += mass;
	}
	mPulseStates.erase(pulseId);
}

uint32_t NRISmokePulseOwner::ExpireStale(double gameplaySeconds)
{
	if (mActivePlanToken != 0u) return 0u;
	std::vector<uint64_t> stale;
	for (const auto& [pulseId, state] : mPulseStates)
	{
		if (!state.visible && state.transitory &&
			state.queuePolicy == NRISmokePulseQueuePolicy::Latest && IsStale(state, gameplaySeconds))
			stale.push_back(pulseId);
	}
	for (const uint64_t pulseId : stale)
		RetireUnpublishedPulse(pulseId, false);
	RefreshPendingSnapshot();
	return (uint32_t)stale.size();
}

double NRISmokePulseOwner::AuthoredSimulationSeconds(const NRISmokeInjectionCommandGpu& command,
	double fallbackSeconds) const
{
	const auto found = mPulseStates.find(PulseId(command));
	return found != mPulseStates.end() ? found->second.authoredSimulationSeconds : fallbackSeconds;
}

bool NRISmokePulseOwner::Plan(const std::vector<NRISmokeInjectionCommandGpu>& selected,
	std::vector<NRISmokeInjectionCommandGpu>& planned, uint64_t& token)
{
	planned.clear();
	token = 0u;
	if (mActivePlanToken != 0u)
		return false;
	std::vector<NRISmokeInjectionCommandGpu> candidatePlan;

	for (const auto& selection : selected)
	{
		if (selection.rangeCount == 0u || RangeEnd(selection) > selection.count)
			return false;
		const uint64_t pulseId = PulseId(selection);
		const auto pending = std::find_if(mPending.begin(), mPending.end(), [&](const auto& candidate)
		{
			return PulseId(candidate) == pulseId && candidate.epoch == selection.epoch &&
				selection.rangeBegin >= candidate.rangeBegin && RangeEnd(selection) <= RangeEnd(candidate);
		});
		if (pending == mPending.end())
			return false;
		const bool overlapsPlan = std::any_of(candidatePlan.begin(), candidatePlan.end(), [&](const auto& candidate)
		{
			return PulseId(candidate) == pulseId &&
				selection.rangeBegin < RangeEnd(candidate) && candidate.rangeBegin < RangeEnd(selection);
		});
		if (overlapsPlan)
			return false;
		candidatePlan.push_back(selection);
	}

	if (candidatePlan.empty())
	{
		RefreshPendingSnapshot();
		return true;
	}
	mActivePlanToken = mNextPlanToken++;
	if (mActivePlanToken == 0u)
		mActivePlanToken = mNextPlanToken++;
	mPlan = std::move(candidatePlan);
	planned = mPlan;
	token = mActivePlanToken;
	mSnapshot.plannedRanges += mPlan.size();
	for (const auto& command : mPlan)
		mSnapshot.plannedMass += command.rangeCount;
	RefreshPendingSnapshot();
	return true;
}

bool NRISmokePulseOwner::Commit(uint64_t token)
{
	return CommitRetaining(token, {});
}

bool NRISmokePulseOwner::CommitRetaining(uint64_t token,
	const std::vector<NRISmokeInjectionCommandGpu>& retained)
{
	if (token == 0u || token != mActivePlanToken)
		return false;
	// Validate the complete mutation set first. A late stale range must never
	// leave an earlier range committed from the same immutable plan.
	for (const auto& committed : mPlan)
	{
		const bool keepPending = std::any_of(retained.begin(), retained.end(), [&](const auto& candidate)
		{
			return PulseId(candidate) == PulseId(committed) && candidate.rangeBegin == committed.rangeBegin &&
				candidate.rangeCount == committed.rangeCount;
		});
		if (keepPending)
			continue;
		const uint64_t pulseId = PulseId(committed);
		if (std::none_of(mPending.begin(), mPending.end(), [&](const auto& candidate)
		{
			return PulseId(candidate) == pulseId && candidate.epoch == committed.epoch &&
				committed.rangeBegin >= candidate.rangeBegin && RangeEnd(committed) <= RangeEnd(candidate);
		}))
			return false;
	}
	for (const auto& committed : mPlan)
	{
		const bool keepPending = std::any_of(retained.begin(), retained.end(), [&](const auto& candidate)
		{
			return PulseId(candidate) == PulseId(committed) && candidate.rangeBegin == committed.rangeBegin &&
				candidate.rangeCount == committed.rangeCount;
		});
		if (keepPending)
			continue;
		const uint64_t pulseId = PulseId(committed);
		const auto pending = std::find_if(mPending.begin(), mPending.end(), [&](const auto& candidate)
		{
			return PulseId(candidate) == pulseId && candidate.epoch == committed.epoch &&
				committed.rangeBegin >= candidate.rangeBegin && RangeEnd(committed) <= RangeEnd(candidate);
		});
		if (pending == mPending.end())
			return false;

		const size_t index = (size_t)std::distance(mPending.begin(), pending);
		const NRISmokeInjectionCommandGpu original = *pending;
		const uint64_t originalEnd = RangeEnd(original);
		mPending.erase(mPending.begin() + index);
		size_t insertion = index;
		if (original.rangeBegin < committed.rangeBegin)
		{
			auto left = original;
			left.rangeCount = committed.rangeBegin - original.rangeBegin;
			mPending.insert(mPending.begin() + insertion++, left);
		}
		if (RangeEnd(committed) < originalEnd)
		{
			auto right = original;
			right.rangeBegin = (uint32_t)RangeEnd(committed);
			right.rangeCount = (uint32_t)(originalEnd - RangeEnd(committed));
			mPending.insert(mPending.begin() + insertion, right);
		}
		mSnapshot.committedRanges++;
		mSnapshot.committedMass += committed.rangeCount;
	}
	for (const auto& committed : mPlan)
		ReleaseAuthoredTimeIfComplete(PulseId(committed));
	ClearPlan();
	return true;
}

bool NRISmokePulseOwner::Acknowledge(uint32_t pulseIdLow, uint32_t pulseIdHigh,
	uint32_t rangeBegin, uint32_t rangeCount)
{
	return RetireRange(pulseIdLow, pulseIdHigh, rangeBegin, rangeCount,
		RetirementKind::GridCommitted);
}

bool NRISmokePulseOwner::RetireFallback(uint32_t pulseIdLow, uint32_t pulseIdHigh,
	uint32_t rangeBegin, uint32_t rangeCount)
{
	return RetireRange(pulseIdLow, pulseIdHigh, rangeBegin, rangeCount,
		RetirementKind::FallbackCompleted);
}

bool NRISmokePulseOwner::ExpireDeferred(uint32_t pulseIdLow, uint32_t pulseIdHigh,
	uint32_t rangeBegin, uint32_t rangeCount)
{
	return RetireRange(pulseIdLow, pulseIdHigh, rangeBegin, rangeCount,
		RetirementKind::DeferredExpired);
}

bool NRISmokePulseOwner::RetireRange(uint32_t pulseIdLow, uint32_t pulseIdHigh,
	uint32_t rangeBegin, uint32_t rangeCount, RetirementKind kind)
{
	if (mActivePlanToken != 0u || rangeCount == 0u)
		return false;
	NRISmokeInjectionCommandGpu acknowledged = {};
	acknowledged.pulseIdLow = pulseIdLow;
	acknowledged.pulseIdHigh = pulseIdHigh;
	acknowledged.rangeBegin = rangeBegin;
	acknowledged.rangeCount = rangeCount;
	const uint64_t pulseId = PulseId(acknowledged);
	const auto pending = std::find_if(mPending.begin(), mPending.end(), [&](const auto& candidate)
	{
		return PulseId(candidate) == pulseId && rangeBegin >= candidate.rangeBegin &&
			RangeEnd(acknowledged) <= RangeEnd(candidate);
	});
	if (pending == mPending.end())
		return false;
	const size_t index = (size_t)std::distance(mPending.begin(), pending);
	const auto original = *pending;
	const uint64_t originalEnd = RangeEnd(original);
	mPending.erase(mPending.begin() + index);
	size_t insertion = index;
	if (original.rangeBegin < rangeBegin)
	{
		auto left = original;
		left.rangeCount = rangeBegin - original.rangeBegin;
		mPending.insert(mPending.begin() + insertion++, left);
	}
	if (RangeEnd(acknowledged) < originalEnd)
	{
		auto right = original;
		right.rangeBegin = (uint32_t)RangeEnd(acknowledged);
		right.rangeCount = (uint32_t)(originalEnd - RangeEnd(acknowledged));
		mPending.insert(mPending.begin() + insertion, right);
	}
	if (kind == RetirementKind::GridCommitted)
	{
		mSnapshot.committedRanges++;
		mSnapshot.committedMass += rangeCount;
	}
	else if (kind == RetirementKind::FallbackCompleted)
	{
		mSnapshot.fallbackRetiredRanges++;
		mSnapshot.fallbackRetiredMass += rangeCount;
	}
	else
	{
		mSnapshot.deferredExpiredRanges++;
		mSnapshot.deferredExpiredMass += rangeCount;
	}
	ReleaseAuthoredTimeIfComplete(pulseId);
	RefreshPendingSnapshot();
	return true;
}

void NRISmokePulseOwner::ReleaseAuthoredTimeIfComplete(uint64_t pulseId)
{
	if (std::none_of(mPending.begin(), mPending.end(), [&](const auto& command)
	{
		return PulseId(command) == pulseId;
	}))
		mPulseStates.erase(pulseId);
}

bool NRISmokePulseOwner::MarkVisible(uint32_t pulseIdLow, uint32_t pulseIdHigh,
	uint32_t rangeBegin, uint32_t rangeCount)
{
	if (rangeCount == 0u) return false;
	NRISmokeInjectionCommandGpu identity = {};
	identity.pulseIdLow = pulseIdLow;
	identity.pulseIdHigh = pulseIdHigh;
	identity.rangeBegin = rangeBegin;
	identity.rangeCount = rangeCount;
	const uint64_t pulseId = PulseId(identity);
	const bool pending = std::any_of(mPending.begin(), mPending.end(), [&](const auto& command)
	{
		return PulseId(command) == pulseId && rangeBegin >= command.rangeBegin &&
			RangeEnd(identity) <= RangeEnd(command);
	});
	const auto state = mPulseStates.find(pulseId);
	if (!pending || state == mPulseStates.end()) return false;
	state->second.visible = true;
	return true;
}

bool NRISmokePulseOwner::Rollback(uint64_t token)
{
	if (token == 0u || token != mActivePlanToken)
		return false;
	mSnapshot.rollbackCount++;
	ClearPlan();
	return true;
}

void NRISmokePulseOwner::RebaseEpoch(uint32_t epoch)
{
	for (auto& command : mPending)
		command.epoch = epoch;
	for (auto& command : mPlan)
		command.epoch = epoch;
}

void NRISmokePulseOwner::RebaseSimulationClock(double oldSimulationSeconds,
	double newSimulationSeconds)
{
	const double offset = newSimulationSeconds - oldSimulationSeconds;
	for (auto& [pulseId, state] : mPulseStates)
	{
		(void)pulseId;
		state.authoredSimulationSeconds += offset;
	}
	RefreshPendingSnapshot();
}

uint32_t NRISmokePulseOwner::Reset()
{
	const uint32_t discarded = (uint32_t)mPending.size();
	mSnapshot.resetPulses += discarded;
	mSnapshot.resetMass += mSnapshot.pendingMass;
	mPending.clear();
	mPulseStates.clear();
	ClearPlan();
	return discarded;
}
