#include "nri_smoke_prompt_fallback.h"

#include "nri_smoke_admission.h"
#include "nri_smoke_pulses.h"

#include <algorithm>

NRISmokePromptPrepareResult NRISmokePromptFallback::Prepare(
	std::vector<NRISmokeInjectionCommandGpu>& commands, uint64_t rendererFrame,
	float gridCellSize, std::vector<NRISmokePromptRangeIdentity>& retained,
	uint32_t maximumFallbackCarrierQuantity)
{
	NRISmokePromptPrepareResult result = {};
	const uint32_t fallbackQuantity = std::min(maximumFallbackCarrierQuantity,
		FixedFallbackCarrierQuantity);
	mPlan.clear();
	mNewlyClaimedSlots.clear();
	retained.clear();
	std::vector<NRISmokeInjectionCommandGpu> scheduled;
	scheduled.reserve(commands.size());
	std::vector<bool> handled(commands.size(), false);
	auto identityOf = [](const NRISmokeInjectionCommandGpu& command)
	{
		return NRISmokePromptRangeIdentity { command.pulseIdLow, command.pulseIdHigh,
			command.rangeBegin, command.rangeCount };
	};
	auto matches = [](const NRISmokePromptRangeIdentity& left,
		const NRISmokePromptRangeIdentity& right)
	{
		return left.rangeCount != 0u && left.pulseIdLow == right.pulseIdLow &&
			left.pulseIdHigh == right.pulseIdHigh && left.rangeBegin == right.rangeBegin &&
			left.rangeCount == right.rangeCount;
	};
	auto defer = [&](const NRISmokeInjectionCommandGpu& command)
	{
		result.deferredRanges++;
		result.deferredMass += command.rangeCount;
		result.deferredBrickWork += NRIEstimateSmokeCommandBrickWork(command, gridCellSize);
	};
	auto schedule = [&](NRISmokeInjectionCommandGpu command, uint32_t slot)
	{
		command.sourceMetadata |= NRI_SMOKE_SOURCE_METADATA_PROMPT_ELIGIBLE;
		command.sourceMetadata &= ~NRI_SMOKE_SOURCE_METADATA_PROMPT_SLOT_MASK;
		command.sourceMetadata |= slot << NRI_SMOKE_SOURCE_METADATA_PROMPT_SLOT_SHIFT;
		mPlan.push_back(identityOf(command));
		scheduled.push_back(command);
	};

	for (uint32_t index = 0u; index < commands.size(); ++index)
	{
		if (!NRIIsInteractiveSmokeSource(commands[index].sourceMetadata))
		{
			scheduled.push_back(commands[index]);
			handled[index] = true;
		}
	}
	// Preserve already-published sticky identities before claiming capacity for
	// newcomers. A profile reduction limits scheduled quantity, not valid slot
	// identity; an existing range may live in any of the eight ABI slots.
	for (uint32_t index = 0u; index < commands.size(); ++index)
	{
		if (handled[index]) continue;
		const NRISmokePromptRangeIdentity identity = identityOf(commands[index]);
		uint32_t slot = FixedFallbackCarrierQuantity;
		for (uint32_t i = 0u; i < FixedFallbackCarrierQuantity; ++i)
			if (matches(mActiveSlots[i], identity)) { slot = i; break; }
		if (slot == FixedFallbackCarrierQuantity) continue;
		if (mPlan.size() < fallbackQuantity)
			schedule(commands[index], slot);
		else
			defer(commands[index]);
		handled[index] = true;
	}
	for (uint32_t index = 0u; index < commands.size(); ++index)
	{
		if (handled[index]) continue;
		auto command = commands[index];
		if (mPlan.size() >= fallbackQuantity)
		{
			defer(command);
			continue;
		}
		uint32_t slot = FixedFallbackCarrierQuantity;
		for (uint32_t i = 0u; i < FixedFallbackCarrierQuantity; ++i)
			if (mActiveSlots[i].rangeCount == 0u) { slot = i; break; }
		if (slot == FixedFallbackCarrierQuantity)
		{
			defer(command);
			continue;
		}
		mActiveSlots[slot] = identityOf(command);
		mNewlyClaimedSlots.push_back(slot);
		schedule(command, slot);
	}
	commands.swap(scheduled);
	retained = mPlan;
	mSnapshot.scheduledFallbackQuantity = (uint32_t)mPlan.size();
	mSnapshot.maximumFallbackCarrierQuantity = fallbackQuantity;
	mSnapshot.scheduledFallbackRanges += mPlan.size();
	mSnapshot.authoredRendererFrame = rendererFrame;
	mSnapshot.authoredPendingMass = 0;
	for (const auto& range : mPlan)
		mSnapshot.authoredPendingMass += range.rangeCount;
	mSnapshot.promptDeferredRanges += result.deferredRanges;
	mSnapshot.promptDeferredMass += result.deferredMass;
	mSnapshot.promptDeferredBrickWork += result.deferredBrickWork;
	return result;
}

void NRISmokePromptFallback::CommitGridHandoffs(NRISmokePulseOwner& pulses,
	const std::vector<NRISmokePromptOutcomeGpu>& outcomes)
{
	for (const auto& outcome : outcomes)
	{
		if (outcome.outcome == (uint32_t)NRISmokePromptOutcome::Fallback)
		{
			mSnapshot.executedFallbackRanges++;
			mSnapshot.fallbackCarrierMass += outcome.rangeCount;
			continue;
		}
		if (outcome.outcome == (uint32_t)NRISmokePromptOutcome::InternalError)
		{
			mSnapshot.internalErrors++;
			continue;
		}
		if (outcome.outcome != (uint32_t)NRISmokePromptOutcome::GridCommitted)
			continue;
		if (pulses.Acknowledge(outcome.pulseIdLow, outcome.pulseIdHigh,
			outcome.rangeBegin, outcome.rangeCount))
		{
			mSnapshot.gridHandoffs++;
			mSnapshot.committedDepositedMass += outcome.rangeCount;
			for (auto& active : mActiveSlots)
				if (active.pulseIdLow == outcome.pulseIdLow && active.pulseIdHigh == outcome.pulseIdHigh &&
					active.rangeBegin == outcome.rangeBegin && active.rangeCount == outcome.rangeCount)
					active = {};
		}
	}
}

void NRISmokePromptFallback::Commit(uint64_t rendererFrame)
{
	(void)rendererFrame;
	mPlan.clear();
	mNewlyClaimedSlots.clear();
}

void NRISmokePromptFallback::Rollback()
{
	if (!mPlan.empty())
		mSnapshot.rollbackCount++;
	for (uint32_t slot : mNewlyClaimedSlots)
		if (slot < mActiveSlots.size()) mActiveSlots[slot] = {};
	mPlan.clear();
	mNewlyClaimedSlots.clear();
	mSnapshot.scheduledFallbackQuantity = 0;
}

void NRISmokePromptFallback::Reset()
{
	mPlan.clear();
	mNewlyClaimedSlots.clear();
	mActiveSlots = {};
	mSnapshot = {};
}
