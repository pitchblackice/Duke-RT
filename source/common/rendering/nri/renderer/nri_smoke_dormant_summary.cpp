#include "nri_smoke_dormant_summary.h"

#include <algorithm>
#include <cmath>

namespace
{
	bool Finite3(const float value[3])
	{
		return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
	}

	bool ValidSummary(const NRISmokeDormantSummaryGpu& summary)
	{
		return summary.epoch != 0u && summary.brickGeneration != 0u &&
			Finite3(summary.boundsMin) && Finite3(summary.boundsMax) &&
			Finite3(summary.centroid) && Finite3(summary.velocity) &&
			std::isfinite(summary.opticalMass) && summary.opticalMass >= 0.0f &&
			std::isfinite(summary.thermalMass) && summary.thermalMass >= 0.0f &&
			std::isfinite(summary.densityMass) && summary.densityMass >= 0.0f &&
			std::isfinite(summary.velocityWeight) && summary.velocityWeight >= 0.0f &&
			summary.boundsMin[0] <= summary.boundsMax[0] &&
			summary.boundsMin[1] <= summary.boundsMax[1] &&
			summary.boundsMin[2] <= summary.boundsMax[2];
	}
}

NRISmokeDormantSummaryOwner::NRISmokeDormantSummaryOwner(uint32_t capacity)
	: mSlots(capacity)
{
	RefreshStatus();
}

void NRISmokeDormantSummaryOwner::Reset(uint32_t epoch)
{
	mEpoch = epoch;
	mRehydrateCursor = 0u;
	for (auto& slot : mSlots)
		slot = {};
	RefreshStatus();
}

NRISmokeDormantSummaryToken NRISmokeDormantSummaryOwner::Claim(uint32_t epoch)
{
	if (epoch == 0u || epoch != mEpoch)
	{
		mStatus.staleRejects++;
		return {};
	}
	for (uint32_t index = 0u; index < mSlots.size(); ++index)
	{
		Slot& slot = mSlots[index];
		if (slot.state != SlotState::Free)
			continue;
		uint32_t generation = mNextGeneration++;
		if (generation == 0u)
			generation = mNextGeneration++;
		slot.state = SlotState::Claimed;
		slot.generation = generation;
		slot.epoch = epoch;
		slot.summary = {};
		RefreshStatus();
		return { index, generation, epoch };
	}
	mStatus.claimFailures++;
	return {};
}

NRISmokeDormantSummaryOwner::Slot* NRISmokeDormantSummaryOwner::Resolve(
	const NRISmokeDormantSummaryToken& token, SlotState expected)
{
	if (!token.IsValid() || token.slot >= mSlots.size())
		return nullptr;
	Slot& slot = mSlots[token.slot];
	return slot.state == expected && slot.generation == token.generation &&
		slot.epoch == token.epoch && token.epoch == mEpoch ? &slot : nullptr;
}

bool NRISmokeDormantSummaryOwner::Publish(const NRISmokeDormantSummaryToken& token,
	const NRISmokeDormantSummaryGpu& summary)
{
	Slot* slot = Resolve(token, SlotState::Claimed);
	if (slot == nullptr)
	{
		mStatus.staleRejects++;
		return false;
	}
	if (!ValidSummary(summary) || summary.epoch != token.epoch)
	{
		mStatus.publishRejects++;
		return false;
	}
	slot->summary = summary;
	slot->summary.summaryGeneration = token.generation;
	slot->state = SlotState::Archived;
	RefreshStatus();
	return true;
}

bool NRISmokeDormantSummaryOwner::CancelClaim(const NRISmokeDormantSummaryToken& token)
{
	Slot* slot = Resolve(token, SlotState::Claimed);
	if (slot == nullptr)
	{
		mStatus.staleRejects++;
		return false;
	}
	*slot = {};
	RefreshStatus();
	return true;
}

float NRISmokeDormantSummaryOwner::ExactDecay(
	uint32_t elapsedTicks, float secondsPerTick, float halfLifeSeconds)
{
	if (elapsedTicks == 0u)
		return 1.0f;
	if (!(secondsPerTick > 0.0f) || !(halfLifeSeconds > 0.0f) ||
		!std::isfinite(secondsPerTick) || !std::isfinite(halfLifeSeconds))
		return 0.0f;
	return std::exp2(-(static_cast<float>(elapsedTicks) * secondsPerTick) / halfLifeSeconds);
}

std::vector<NRISmokeDormantRehydrateWork> NRISmokeDormantSummaryOwner::BeginRehydrate(
	uint32_t epoch, uint32_t currentTick, uint32_t fixedQuantity,
	float secondsPerTick, float densityHalfLifeSeconds, float thermalHalfLifeSeconds)
{
	std::vector<NRISmokeDormantRehydrateWork> work;
	if (epoch == 0u || epoch != mEpoch || fixedQuantity == 0u || mSlots.empty())
		return work;
	work.reserve(std::min<size_t>(fixedQuantity, mSlots.size()));
	const uint32_t startingCursor = mRehydrateCursor;
	for (uint32_t visited = 0u; visited < mSlots.size() && work.size() < fixedQuantity; ++visited)
	{
		const uint32_t index = (startingCursor + visited) % static_cast<uint32_t>(mSlots.size());
		Slot& slot = mSlots[index];
		if (slot.state != SlotState::Archived || slot.epoch != epoch ||
			slot.summary.epoch != epoch || slot.summary.summaryGeneration != slot.generation ||
			currentTick < slot.summary.lastSimulationTick)
			continue;
		NRISmokeDormantSummaryGpu decayed = slot.summary;
		const uint32_t elapsed = currentTick - decayed.lastSimulationTick;
		const float densityDecay = ExactDecay(elapsed, secondsPerTick, densityHalfLifeSeconds);
		const float thermalDecay = ExactDecay(elapsed, secondsPerTick, thermalHalfLifeSeconds);
		decayed.opticalMass *= densityDecay;
		decayed.densityMass *= densityDecay;
		decayed.velocityWeight *= densityDecay;
		decayed.thermalMass *= thermalDecay;
		decayed.lastSimulationTick = currentTick;
		slot.state = SlotState::Rehydrating;
		work.push_back({ { index, slot.generation, slot.epoch }, decayed });
		mStatus.rehydrateAttempts++;
		mRehydrateCursor = (index + 1u) % static_cast<uint32_t>(mSlots.size());
	}
	RefreshStatus();
	return work;
}

bool NRISmokeDormantSummaryOwner::CommitRehydrate(
	const NRISmokeDormantSummaryToken& token, bool finePublished)
{
	Slot* slot = Resolve(token, SlotState::Rehydrating);
	if (slot == nullptr)
	{
		mStatus.staleRejects++;
		return false;
	}
	if (finePublished)
	{
		*slot = {};
		mStatus.rehydrateCommits++;
	}
	else
	{
		slot->state = SlotState::Archived;
		mStatus.rehydrateRetained++;
	}
	RefreshStatus();
	return true;
}

bool NRISmokeDormantSummaryOwner::CancelRehydrate(const NRISmokeDormantSummaryToken& token)
{
	return CommitRehydrate(token, false);
}

void NRISmokeDormantSummaryOwner::RefreshStatus()
{
	mStatus.capacity = static_cast<uint32_t>(mSlots.size());
	mStatus.free = mStatus.claimed = mStatus.archived = mStatus.rehydrating = 0u;
	for (const Slot& slot : mSlots)
	{
		if (slot.state == SlotState::Free) mStatus.free++;
		else if (slot.state == SlotState::Claimed) mStatus.claimed++;
		else if (slot.state == SlotState::Archived) mStatus.archived++;
		else mStatus.rehydrating++;
	}
}
