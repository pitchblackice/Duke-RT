#include "nri_smoke_prompt_fallback.h"

#include "nri_smoke_admission.h"
#include "nri_smoke_pulses.h"

#include <algorithm>
#include <cmath>

NRISmokePromptPrepareResult NRISmokePromptFallback::Prepare(
	std::vector<NRISmokeInjectionCommandGpu>& commands, const NRISmokePulseOwner& pulses,
	uint64_t rendererFrame,
	double simulationTimeSeconds, const std::vector<NRISmokeStyleGpu>& styles, float gridCellSize,
	std::vector<NRISmokePromptRangeIdentity>& retained,
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
		const ActiveSlot& active = mActiveSlots[slot];
		const float age = (float)std::max(0.0, simulationTimeSeconds - active.authoredSimulationSeconds);
		if (command.styleIndex < styles.size())
		{
			const NRISmokeStyleGpu& style = styles[command.styleIndex];
			// The normalized source kernel treats DensityScale as integrated mass.
			// Expanding support supplies geometric dilution; only half-life decay
			// should reduce the mass handed to either fallback or a late grid deposit.
			const float initialRadius = std::max(std::max(command.spawnRadius,
				style.radius * command.radiusScale), std::max(gridCellSize, 0.001f));
			command.spawnRadius = std::max(initialRadius + style.expansionVelocity * age, 0.001f);
			command.densityScale *= std::exp2(-age / std::max(style.densityHalfLife, 0.001f));
		}
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
			if (matches(mActiveSlots[i].identity, identity)) { slot = i; break; }
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
			if (mActiveSlots[i].identity.rangeCount == 0u) { slot = i; break; }
		if (slot == FixedFallbackCarrierQuantity)
		{
			defer(command);
			continue;
		}
		mActiveSlots[slot].identity = identityOf(command);
		mActiveSlots[slot].authoredSimulationSeconds =
			pulses.AuthoredSimulationSeconds(command, simulationTimeSeconds);
		mActiveSlots[slot].lifetimeSeconds = command.styleIndex < styles.size() ?
			std::max(styles[command.styleIndex].lifetime, 0.001f) : 0.001f;
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
	RefreshActiveSnapshot(simulationTimeSeconds);
	return result;
}

void NRISmokePromptFallback::RefreshActiveSnapshot(double simulationTimeSeconds)
{
	mSnapshot.activeFallbackSlots = 0u;
	double oldestAgeSeconds = 0.0;
	for (const ActiveSlot& active : mActiveSlots)
	{
		if (active.identity.rangeCount == 0u) continue;
		mSnapshot.activeFallbackSlots++;
		oldestAgeSeconds = std::max(oldestAgeSeconds,
			std::max(0.0, simulationTimeSeconds - active.authoredSimulationSeconds));
	}
	mSnapshot.oldestActiveAgeMilliseconds = (uint32_t)std::min(
		oldestAgeSeconds * 1000.0, (double)UINT32_MAX);
}

void NRISmokePromptFallback::RetireExpired(NRISmokePulseOwner& pulses,
	double simulationTimeSeconds, const std::vector<NRISmokeStyleGpu>& styles)
{
	for (ActiveSlot& active : mActiveSlots)
	{
		if (active.identity.rangeCount == 0u ||
			simulationTimeSeconds - active.authoredSimulationSeconds < active.lifetimeSeconds)
			continue;
		const NRISmokePromptRangeIdentity identity = active.identity;
		if (!pulses.RetireFallback(identity.pulseIdLow, identity.pulseIdHigh,
			identity.rangeBegin, identity.rangeCount))
		{
			mSnapshot.expiryAcknowledgeFailures++;
			continue;
		}
		mSnapshot.expiredFallbackRanges++;
		mSnapshot.expiredFallbackMass += identity.rangeCount;
		active = {};
	}
	const std::vector<NRISmokeInjectionCommandGpu> pending = pulses.PendingCommands();
	for (const NRISmokeInjectionCommandGpu& command : pending)
	{
		if (!NRIIsInteractiveSmokeSource(command.sourceMetadata) || command.styleIndex >= styles.size())
			continue;
		const NRISmokePromptRangeIdentity identity = { command.pulseIdLow, command.pulseIdHigh,
			command.rangeBegin, command.rangeCount };
		const bool active = std::any_of(mActiveSlots.begin(), mActiveSlots.end(), [&](const ActiveSlot& slot)
		{
			return slot.identity.rangeCount != 0u && slot.identity.pulseIdLow == identity.pulseIdLow &&
				slot.identity.pulseIdHigh == identity.pulseIdHigh &&
				slot.identity.rangeBegin == identity.rangeBegin && slot.identity.rangeCount == identity.rangeCount;
		});
		if (active) continue;
		const double authored = pulses.AuthoredSimulationSeconds(command, simulationTimeSeconds);
		if (simulationTimeSeconds - authored < std::max(styles[command.styleIndex].lifetime, 0.001f))
			continue;
		if (pulses.ExpireDeferred(identity.pulseIdLow, identity.pulseIdHigh,
			identity.rangeBegin, identity.rangeCount))
		{
			mSnapshot.expiredDeferredRanges++;
			mSnapshot.expiredDeferredMass += identity.rangeCount;
		}
	}
	RefreshActiveSnapshot(simulationTimeSeconds);
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
				if (active.identity.pulseIdLow == outcome.pulseIdLow &&
					active.identity.pulseIdHigh == outcome.pulseIdHigh &&
					active.identity.rangeBegin == outcome.rangeBegin &&
					active.identity.rangeCount == outcome.rangeCount)
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
