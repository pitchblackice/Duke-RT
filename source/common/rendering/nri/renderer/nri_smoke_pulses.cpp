#include "nri_smoke_pulses.h"

#include <algorithm>
#include <utility>

uint64_t NRISmokePulseOwner::PulseId(const NRISmokeInjectionCommandGpu& command)
{
	return (uint64_t(command.pulseIdHigh) << 32u) | command.pulseIdLow;
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
	mSnapshot.planActive = mActivePlanToken != 0;
}

void NRISmokePulseOwner::ClearPlan()
{
	mPlan.clear();
	mActivePlanToken = 0;
	RefreshPendingSnapshot();
}

void NRISmokePulseOwner::Enqueue(const std::vector<NRISmokeInjectionCommandGpu>& commands)
{
	for (const auto& authored : commands)
	{
		if (authored.count == 0u)
			continue;
		NRISmokeInjectionCommandGpu command = authored;
		command.rangeBegin = 0u;
		command.rangeCount = command.count;
		uint64_t pulseId = mNextPulseId++;
		if (pulseId == 0u)
			pulseId = mNextPulseId++;
		SetPulseId(command, pulseId);
		mPending.push_back(command);
		mSnapshot.enqueuedPulses++;
		mSnapshot.enqueuedMass += command.rangeCount;
	}
	RefreshPendingSnapshot();
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
	ClearPlan();
	return true;
}

bool NRISmokePulseOwner::Acknowledge(uint32_t pulseIdLow, uint32_t pulseIdHigh,
	uint32_t rangeBegin, uint32_t rangeCount)
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
	mSnapshot.committedRanges++;
	mSnapshot.committedMass += rangeCount;
	RefreshPendingSnapshot();
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

uint32_t NRISmokePulseOwner::Reset()
{
	const uint32_t discarded = (uint32_t)mPending.size();
	mSnapshot.resetPulses += discarded;
	mSnapshot.resetMass += mSnapshot.pendingMass;
	mPending.clear();
	ClearPlan();
	return discarded;
}
