#include "nri_smoke_analytic_carriers.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
bool Finite3(const float value[3])
{
	return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

bool Valid(const NRISmokeAnalyticCarrierRequest& request)
{
	return Finite3(request.position) && Finite3(request.velocity) &&
		Finite3(request.halfAxisU) && Finite3(request.halfAxisV) &&
		std::isfinite(request.initialRadius) && request.initialRadius > 0.0f &&
		std::isfinite(request.initialDensity) && request.initialDensity >= 0.0f &&
		request.rangeCount > 0u && request.shape <= 1u &&
		std::isfinite(request.expansionVelocity) &&
		std::isfinite(request.densityHalfLife) && request.densityHalfLife > 0.0f &&
		std::isfinite(request.lifetimeSeconds) && request.lifetimeSeconds > 0.0f &&
		std::isfinite(request.maximumLatencySeconds) && request.maximumLatencySeconds >= 0.0f &&
		std::isfinite(request.authoredGameplaySeconds);
}

float SupportVolume(const NRISmokeAnalyticCarrierRequest& request, float radius)
{
	constexpr float Pi = 3.14159265359f;
	const float clampedRadius = std::max(radius, 0.001f);
	if (request.shape == 0u)
		return (4.0f * Pi / 3.0f) * clampedRadius * clampedRadius * clampedRadius;
	const float u = std::sqrt(request.halfAxisU[0] * request.halfAxisU[0] +
		request.halfAxisU[1] * request.halfAxisU[1] +
		request.halfAxisU[2] * request.halfAxisU[2]);
	const float v = std::sqrt(request.halfAxisV[0] * request.halfAxisV[0] +
		request.halfAxisV[1] * request.halfAxisV[1] +
		request.halfAxisV[2] * request.halfAxisV[2]);
	const float area = 4.0f * u * v;
	const float perimeter = 4.0f * (u + v);
	return 2.0f * area * clampedRadius + (Pi * 0.5f) * perimeter *
		clampedRadius * clampedRadius + (4.0f * Pi / 3.0f) *
		clampedRadius * clampedRadius * clampedRadius;
}
}

void NRISmokeAnalyticCarriers::BeginFrame(double gameplayTimeSeconds,
	uint32_t maximumActiveQuantity)
{
	mGameplayTimeSeconds = gameplayTimeSeconds;
	mSnapshot.maximumActiveQuantity = std::min(maximumActiveQuantity, FixedCarrierCapacity);
	mPrepared = std::isfinite(gameplayTimeSeconds);
	Refresh();
}

NRISmokeAnalyticCarrierAdmission NRISmokeAnalyticCarriers::Admit(
	const NRISmokeAnalyticCarrierRequest& request)
{
	mSnapshot.requested++;
	if (!mPrepared) return Drop(NRISmokeAnalyticCarrierDropReason::NotPrepared);
	if (mSnapshot.maximumActiveQuantity == 0u)
		return Drop(NRISmokeAnalyticCarrierDropReason::Disabled);
	if (!Valid(request)) return Drop(NRISmokeAnalyticCarrierDropReason::InvalidRequest);
	if (request.epoch != mSnapshot.epoch)
		return Drop(NRISmokeAnalyticCarrierDropReason::StaleEpoch);
	if (mGameplayTimeSeconds - request.authoredGameplaySeconds >= request.lifetimeSeconds)
		return Drop(NRISmokeAnalyticCarrierDropReason::ExpiredOnArrival);
	if (request.maximumLatencySeconds > 0.0f &&
		mGameplayTimeSeconds - request.authoredGameplaySeconds > request.maximumLatencySeconds)
		return Drop(NRISmokeAnalyticCarrierDropReason::StaleOnArrival);
	if (mSnapshot.activeQuantity >= mSnapshot.maximumActiveQuantity)
		return Drop(NRISmokeAnalyticCarrierDropReason::Capacity);

	for (uint32_t index = 0u; index < mSlots.size(); ++index)
	{
		Slot& slot = mSlots[index];
		if (slot.active || slot.generation == std::numeric_limits<uint32_t>::max()) continue;
		slot.generation++;
		slot.request = request;
		slot.active = true;
		mSnapshot.admitted++;
		Refresh();
		return { { index, slot.generation, mSnapshot.epoch },
			NRISmokeAnalyticCarrierDropReason::None };
	}

	return Drop(NRISmokeAnalyticCarrierDropReason::Capacity);
}

uint32_t NRISmokeAnalyticCarriers::AdmitBatch(
	const NRISmokeAnalyticCarrierRequest* requests, uint32_t count)
{
	if (requests == nullptr || count == 0u) return 0u;
	uint32_t availableSlots = 0u;
	for (const Slot& slot : mSlots)
		availableSlots += !slot.active && slot.generation !=
			std::numeric_limits<uint32_t>::max() ? 1u : 0u;
	if (count > availableSlots || mSnapshot.activeQuantity + count >
		mSnapshot.maximumActiveQuantity)
	{
		mSnapshot.requested += count;
		for (uint32_t index = 0u; index < count; ++index)
			Drop(NRISmokeAnalyticCarrierDropReason::Capacity);
		return 0u;
	}
	uint32_t admitted = 0u;
	for (uint32_t index = 0u; index < count; ++index)
		admitted += Admit(requests[index]).Accepted() ? 1u : 0u;
	return admitted;
}

bool NRISmokeAnalyticCarriers::IsLive(const NRISmokeAnalyticCarrierHandle& handle) const
{
	if (handle.slot >= mSlots.size() || handle.epoch != mSnapshot.epoch) return false;
	const Slot& slot = mSlots[handle.slot];
	return slot.active && slot.generation == handle.generation;
}

void NRISmokeAnalyticCarriers::Reset(uint32_t epoch)
{
	for (Slot& slot : mSlots)
	{
		if (slot.generation != std::numeric_limits<uint32_t>::max()) slot.generation++;
		slot.request = {};
		slot.active = false;
	}
	mGpuCarriers.clear();
	mSnapshot = {};
	mSnapshot.epoch = epoch;
	mGameplayTimeSeconds = 0.0;
	mPrepared = false;
}

void NRISmokeAnalyticCarriers::Refresh()
{
	mGpuCarriers.clear();
	mSnapshot.activeQuantity = 0u;
	double oldestAgeSeconds = 0.0;
	for (uint32_t slotIndex = 0u; slotIndex < mSlots.size(); ++slotIndex)
	{
		Slot& slot = mSlots[slotIndex];
		if (!slot.active) continue;
		const double ageSeconds = std::max(0.0,
			mGameplayTimeSeconds - slot.request.authoredGameplaySeconds);
		if (mPrepared && ageSeconds >= slot.request.lifetimeSeconds)
		{
			slot.active = false;
			mSnapshot.expired++;
			continue;
		}

		mSnapshot.activeQuantity++;
		oldestAgeSeconds = std::max(oldestAgeSeconds, ageSeconds);
		NRISmokeAnalyticCarrierGpu gpu = {};
		for (uint32_t axis = 0u; axis < 3u; ++axis)
		{
			gpu.position[axis] = slot.request.position[axis] +
				slot.request.velocity[axis] * (float)ageSeconds;
			gpu.halfAxisU[axis] = slot.request.halfAxisU[axis];
			gpu.halfAxisV[axis] = slot.request.halfAxisV[axis];
		}
		gpu.radius = std::max(slot.request.initialRadius +
			slot.request.expansionVelocity * (float)ageSeconds, 0.001f);
		const float initialVolume = SupportVolume(slot.request, slot.request.initialRadius);
		const float currentVolume = SupportVolume(slot.request, gpu.radius);
		const float expansionDilution = std::min(1.0f,
			initialVolume / std::max(currentVolume, 1e-20f));
		gpu.densityScale = slot.request.initialDensity * expansionDilution *
			std::exp2(-(float)ageSeconds / slot.request.densityHalfLife);
		gpu.shape = slot.request.shape;
		gpu.styleIndex = slot.request.styleIndex;
		gpu.rangeCount = slot.request.rangeCount;
		gpu.epoch = slot.request.epoch;
		gpu.flags = 1u | (slotIndex << 1u) | (slot.generation << 8u);
		mGpuCarriers.push_back(gpu);
	}
	mSnapshot.highWaterQuantity = std::max(mSnapshot.highWaterQuantity,
		mSnapshot.activeQuantity);
	mSnapshot.oldestActiveAgeMilliseconds = (uint32_t)std::min(
		oldestAgeSeconds * 1000.0, (double)UINT32_MAX);
}

NRISmokeAnalyticCarrierAdmission NRISmokeAnalyticCarriers::Drop(
	NRISmokeAnalyticCarrierDropReason reason)
{
	switch (reason)
	{
	case NRISmokeAnalyticCarrierDropReason::NotPrepared: mSnapshot.droppedNotPrepared++; break;
	case NRISmokeAnalyticCarrierDropReason::Disabled: mSnapshot.droppedDisabled++; break;
	case NRISmokeAnalyticCarrierDropReason::InvalidRequest: mSnapshot.droppedInvalidRequest++; break;
	case NRISmokeAnalyticCarrierDropReason::StaleEpoch: mSnapshot.droppedStaleEpoch++; break;
	case NRISmokeAnalyticCarrierDropReason::ExpiredOnArrival:
		mSnapshot.droppedExpiredOnArrival++; break;
	case NRISmokeAnalyticCarrierDropReason::StaleOnArrival:
		mSnapshot.droppedStaleOnArrival++; break;
	case NRISmokeAnalyticCarrierDropReason::Capacity: mSnapshot.droppedCapacity++; break;
	case NRISmokeAnalyticCarrierDropReason::None: break;
	}
	return { {}, reason };
}
