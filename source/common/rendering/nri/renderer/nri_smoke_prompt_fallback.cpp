#include "nri_smoke_prompt_fallback.h"

#include "nri_smoke_admission.h"
#include "nri_smoke_pulses.h"

NRISmokePromptPrepareResult NRISmokePromptFallback::Prepare(
	std::vector<NRISmokeInjectionCommandGpu>& commands, uint64_t rendererFrame,
	float gridCellSize, std::vector<NRISmokePromptRangeIdentity>& retained)
{
	NRISmokePromptPrepareResult result = {};
	mPlan.clear();
	mNewlyClaimedSlots.clear();
	retained.clear();
	std::vector<NRISmokeInjectionCommandGpu> scheduled;
	scheduled.reserve(commands.size());
	for (auto command : commands)
	{
		if (!NRIIsInteractiveSmokeSource(command.sourceMetadata))
		{
			scheduled.push_back(command);
			continue;
		}
		NRISmokePromptRangeIdentity identity = { command.pulseIdLow, command.pulseIdHigh,
			command.rangeBegin, command.rangeCount };
		auto matches = [&](const NRISmokePromptRangeIdentity& candidate)
		{
			return candidate.rangeCount != 0u && candidate.pulseIdLow == identity.pulseIdLow &&
				candidate.pulseIdHigh == identity.pulseIdHigh && candidate.rangeBegin == identity.rangeBegin &&
				candidate.rangeCount == identity.rangeCount;
		};
		uint32_t slot = FixedFallbackCarrierQuantity;
		for (uint32_t i = 0u; i < FixedFallbackCarrierQuantity; ++i)
			if (matches(mActiveSlots[i])) { slot = i; break; }
		if (slot == FixedFallbackCarrierQuantity)
			for (uint32_t i = 0u; i < FixedFallbackCarrierQuantity; ++i)
				if (mActiveSlots[i].rangeCount == 0u)
				{
					slot = i;
					mActiveSlots[i] = identity;
					mNewlyClaimedSlots.push_back(i);
					break;
				}
		if (slot == FixedFallbackCarrierQuantity)
		{
			result.deferredRanges++;
			result.deferredMass += command.rangeCount;
			result.deferredBrickWork += NRIEstimateSmokeCommandBrickWork(command, gridCellSize);
			continue;
		}
		command.sourceMetadata |= NRI_SMOKE_SOURCE_METADATA_PROMPT_ELIGIBLE;
		command.sourceMetadata &= ~NRI_SMOKE_SOURCE_METADATA_PROMPT_SLOT_MASK;
		command.sourceMetadata |= slot << NRI_SMOKE_SOURCE_METADATA_PROMPT_SLOT_SHIFT;
		mPlan.push_back(identity);
		scheduled.push_back(command);
	}
	commands.swap(scheduled);
	retained = mPlan;
	mSnapshot.scheduledFallbackQuantity = (uint32_t)mPlan.size();
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
