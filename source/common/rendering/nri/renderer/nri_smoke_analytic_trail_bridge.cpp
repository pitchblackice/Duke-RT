#include "nri_smoke_analytic_trail_bridge.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr float Pi = 3.14159265359f;

bool Finite3(const float value[3])
{
	return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

float Distance(const float a[3], const float b[3])
{
	const float x = a[0] - b[0];
	const float y = a[1] - b[1];
	const float z = a[2] - b[2];
	return std::sqrt(x * x + y * y + z * z);
}

float SphereVolume(float radius)
{
	return (4.0f * Pi / 3.0f) * radius * radius * radius;
}

float CapsuleVolume(float halfLength, float radius)
{
	return 2.0f * Pi * halfLength * radius * radius + SphereVolume(radius);
}
}

void NRISmokeAnalyticTrailBridge::Reset(uint32_t epoch)
{
	mStates.clear();
	mPresentations.clear();
	mSnapshot = {};
	mSnapshot.epoch = epoch;
	mGameplayTimeSeconds = 0.0;
	mNextHandoffSerial = 1u;
	mNextLightingGroupGeneration = 1u;
}

void NRISmokeAnalyticTrailBridge::BeginFrame(double gameplayTimeSeconds, uint32_t epoch)
{
	if (epoch != mSnapshot.epoch)
		Reset(epoch);
	mGameplayTimeSeconds = gameplayTimeSeconds;
	if (!std::isfinite(gameplayTimeSeconds))
	{
		Refresh();
		return;
	}
	for (auto it = mStates.begin(); it != mStates.end(); )
	{
		const NRISmokeAnalyticCarrierRequest& carrier = it->second.presentation.carrier;
		if (gameplayTimeSeconds - carrier.authoredGameplaySeconds >= carrier.lifetimeSeconds)
		{
			if (it->second.pendingHandoff.Valid()) mSnapshot.handoffsCancelled++;
			mSnapshot.expired++;
			it = mStates.erase(it);
		}
		else
			++it;
	}
	Refresh();
}

NRISmokeAnalyticTrailObserveResult NRISmokeAnalyticTrailBridge::Observe(
	const NRISmokeAnalyticTrailObservation& observation,
	const NRISmokeAnalyticTrailPoint* points, size_t pointCount)
{
	mSnapshot.observations++;
	if (observation.persistentSource)
		return Reject(NRISmokeAnalyticTrailObserveResult::PersistentSource);
	if (observation.epoch != mSnapshot.epoch)
		return Reject(NRISmokeAnalyticTrailObserveResult::StaleEpoch);
	if (observation.stableSourceKey == 0u || observation.updateOrdinal == 0u ||
		points == nullptr || pointCount == 0u || !Finite3(observation.velocity) ||
		!std::isfinite(observation.authoredGameplaySeconds) ||
		!std::isfinite(observation.radius) || observation.radius <= 0.0f ||
		!std::isfinite(observation.densityScale) || observation.densityScale < 0.0f ||
		!std::isfinite(observation.expansionVelocity) ||
		!std::isfinite(observation.densityHalfLife) || observation.densityHalfLife <= 0.0f ||
		!std::isfinite(observation.presentationLifetimeSeconds) ||
		observation.presentationLifetimeSeconds <= 0.0f ||
		!std::isfinite(observation.maximumLatencySeconds) ||
		observation.maximumLatencySeconds < 0.0f ||
		!std::isfinite(observation.maximumSegmentLength) ||
		observation.maximumSegmentLength <= 0.0f)
		return Reject(NRISmokeAnalyticTrailObserveResult::Invalid);
	for (size_t index = 0u; index < pointCount; ++index)
		if (!Finite3(points[index].position) || points[index].rangeCount == 0u ||
			points[index].rangeCount > MaximumRangeCountPerPoint)
			return Reject(NRISmokeAnalyticTrailObserveResult::Invalid);

	const float lifetime = std::min(observation.presentationLifetimeSeconds,
		MaximumPresentationLifetimeSeconds);
	if (std::isfinite(mGameplayTimeSeconds) &&
		mGameplayTimeSeconds - observation.authoredGameplaySeconds >= lifetime)
		return Reject(NRISmokeAnalyticTrailObserveResult::ExpiredOnArrival);

	auto existing = mStates.find(observation.stableSourceKey);
	if (existing != mStates.end() && observation.updateOrdinal <= existing->second.updateOrdinal)
		return Reject(NRISmokeAnalyticTrailObserveResult::StaleUpdate);
	if (existing == mStates.end() && mStates.size() >= MaximumSources)
		return Reject(NRISmokeAnalyticTrailObserveResult::Capacity);

	const size_t newest = pointCount - 1u;
	const size_t oldestAllowed = pointCount > MaximumPointsPerUpdate ?
		pointCount - MaximumPointsPerUpdate : 0u;
	size_t oldest = newest;
	float pathLength = 0.0f;
	while (oldest > oldestAllowed)
	{
		const float step = Distance(points[oldest].position, points[oldest - 1u].position);
		if (pathLength + step > observation.maximumSegmentLength) break;
		pathLength += step;
		oldest--;
	}
	mSnapshot.discardedOldPoints += oldest;

	uint32_t rangeCount = 0u;
	for (size_t index = oldest; index <= newest; ++index)
		rangeCount += points[index].rangeCount;

	State state = existing != mStates.end() ? existing->second : State{};
	if (existing != mStates.end())
	{
		mSnapshot.replacements++;
		if (state.pendingHandoff.Valid())
		{
			mSnapshot.handoffsCancelled++;
			state.pendingHandoff = {};
		}
	}
	else
	{
		state.presentation.lightingGroupKey = observation.stableSourceKey;
		state.presentation.lightingGroupGeneration = mNextLightingGroupGeneration++;
		if (mNextLightingGroupGeneration == 0u) mNextLightingGroupGeneration = 1u;
	}

	NRISmokeAnalyticTrailPresentation& presentation = state.presentation;
	presentation.stableSourceKey = observation.stableSourceKey;
	presentation.segmentRevision = observation.updateOrdinal;
	state.nextSegmentRevision = std::max(state.nextSegmentRevision,
		observation.updateOrdinal + 1u);
	presentation.coalescedPointCount = static_cast<uint32_t>(newest - oldest + 1u);
	NRISmokeAnalyticCarrierRequest& carrier = presentation.carrier;
	carrier = {};
	for (uint32_t axis = 0u; axis < 3u; ++axis)
	{
		carrier.position[axis] = (points[oldest].position[axis] +
			points[newest].position[axis]) * 0.5f;
		carrier.halfAxisU[axis] = (points[newest].position[axis] -
			points[oldest].position[axis]) * 0.5f;
		carrier.velocity[axis] = observation.velocity[axis];
	}
	const float halfLength = Distance(points[oldest].position,
		points[newest].position) * 0.5f;
	carrier.shape = halfLength > 0.0001f ? 1u : 0u;
	carrier.initialRadius = observation.radius;
	const float sourceVolume = SphereVolume(observation.radius);
	const float targetVolume = carrier.shape != 0u ?
		CapsuleVolume(halfLength, observation.radius) : sourceVolume;
	carrier.initialDensity = observation.densityScale * sourceVolume /
		std::max(targetVolume, std::numeric_limits<float>::min());
	carrier.rangeCount = rangeCount;
	carrier.expansionVelocity = observation.expansionVelocity;
	carrier.densityHalfLife = observation.densityHalfLife;
	carrier.lifetimeSeconds = lifetime;
	carrier.styleIndex = observation.styleIndex;
	carrier.sourceId = observation.sourceId;
	carrier.epoch = observation.epoch;
	carrier.authoredGameplaySeconds = observation.authoredGameplaySeconds;
	carrier.maximumLatencySeconds = observation.maximumLatencySeconds;
	carrier.sourceEventSerial = observation.stableSourceKey;
	carrier.replacementKey = observation.stableSourceKey;
	carrier.batchIndex = 0u;
	carrier.batchCount = 1u;

	state.updateOrdinal = observation.updateOrdinal;
	state.depositedRangeCount = rangeCount;
	state.committedRangeCount = 0u;
	mStates[observation.stableSourceKey] = state;
	mSnapshot.accepted++;
	Refresh();
	return NRISmokeAnalyticTrailObserveResult::Accepted;
}

bool NRISmokeAnalyticTrailBridge::PublishFallback(uint64_t stableSourceKey,
	uint64_t segmentRevision, uint32_t epoch)
{
	if (epoch != mSnapshot.epoch) return false;
	auto it = mStates.find(stableSourceKey);
	if (it == mStates.end() ||
		it->second.presentation.segmentRevision != segmentRevision) return false;
	if (!it->second.published) mSnapshot.fallbackPublications++;
	it->second.published = true;
	Refresh();
	return true;
}

bool NRISmokeAnalyticTrailBridge::CommitExactGridRange(uint64_t stableSourceKey,
	uint64_t segmentRevision, uint32_t epoch, uint32_t depositedRangeCount)
{
	if (epoch != mSnapshot.epoch || depositedRangeCount == 0u) return false;
	auto it = mStates.find(stableSourceKey);
	if (it == mStates.end() ||
		it->second.presentation.segmentRevision != segmentRevision) return false;
	it->second.committedRangeCount = std::min(it->second.depositedRangeCount,
		it->second.committedRangeCount + depositedRangeCount);
	if (it->second.committedRangeCount < it->second.depositedRangeCount) return false;
	mStates.erase(it);
	mSnapshot.handoffsCommitted++;
	mSnapshot.retired++;
	Refresh();
	return true;
}

bool NRISmokeAnalyticTrailBridge::RetireSource(uint64_t stableSourceKey, uint32_t epoch)
{
	if (epoch != mSnapshot.epoch) return false;
	auto it = mStates.find(stableSourceKey);
	if (it == mStates.end()) return false;
	if (it->second.pendingHandoff.Valid()) mSnapshot.handoffsCancelled++;
	mStates.erase(it);
	mSnapshot.retired++;
	Refresh();
	return true;
}

NRISmokeAnalyticTrailHandoffToken NRISmokeAnalyticTrailBridge::BeginExactGridHandoff(
	uint64_t stableSourceKey, uint64_t segmentRevision)
{
	auto it = mStates.find(stableSourceKey);
	if (it == mStates.end() || it->second.presentation.segmentRevision != segmentRevision ||
		it->second.pendingHandoff.Valid())
	{
		mSnapshot.handoffsRejected++;
		return {};
	}
	NRISmokeAnalyticTrailHandoffToken token = {};
	token.stableSourceKey = stableSourceKey;
	token.segmentRevision = segmentRevision;
	token.serial = mNextHandoffSerial++;
	if (mNextHandoffSerial == 0u) mNextHandoffSerial = 1u;
	token.epoch = mSnapshot.epoch;
	it->second.pendingHandoff = token;
	mSnapshot.handoffsBegun++;
	return token;
}

bool NRISmokeAnalyticTrailBridge::CommitExactGridHandoff(
	const NRISmokeAnalyticTrailHandoffToken& token,
	const NRISmokeAnalyticTrailGridReceipt& receipt)
{
	auto it = mStates.find(token.stableSourceKey);
	if (it == mStates.end() || !TokenMatches(it->second, token) ||
		receipt.stableSourceKey != token.stableSourceKey ||
		receipt.segmentRevision != token.segmentRevision ||
		receipt.epoch != token.epoch || receipt.gridAuthorityGeneration == 0u ||
		receipt.depositedRangeCount != it->second.depositedRangeCount)
	{
		mSnapshot.handoffsRejected++;
		return false;
	}
	mStates.erase(it);
	mSnapshot.handoffsCommitted++;
	mSnapshot.retired++;
	Refresh();
	return true;
}

bool NRISmokeAnalyticTrailBridge::CancelExactGridHandoff(
	const NRISmokeAnalyticTrailHandoffToken& token)
{
	auto it = mStates.find(token.stableSourceKey);
	if (it == mStates.end() || !TokenMatches(it->second, token))
	{
		mSnapshot.handoffsRejected++;
		return false;
	}
	it->second.pendingHandoff = {};
	mSnapshot.handoffsCancelled++;
	return true;
}

void NRISmokeAnalyticTrailBridge::Refresh()
{
	mPresentations.clear();
	for (const auto& entry : mStates)
		if (entry.second.published)
			mPresentations.push_back(entry.second.presentation);
	mSnapshot.activeSources = static_cast<uint32_t>(mStates.size());
	mSnapshot.highWaterSources = std::max(mSnapshot.highWaterSources,
		mSnapshot.activeSources);
}

NRISmokeAnalyticTrailObserveResult NRISmokeAnalyticTrailBridge::Reject(
	NRISmokeAnalyticTrailObserveResult result)
{
	switch (result)
	{
	case NRISmokeAnalyticTrailObserveResult::Invalid: mSnapshot.rejectedInvalid++; break;
	case NRISmokeAnalyticTrailObserveResult::PersistentSource:
		mSnapshot.rejectedPersistent++; break;
	case NRISmokeAnalyticTrailObserveResult::StaleEpoch:
		mSnapshot.rejectedStaleEpoch++; break;
	case NRISmokeAnalyticTrailObserveResult::StaleUpdate:
		mSnapshot.rejectedStaleUpdate++; break;
	case NRISmokeAnalyticTrailObserveResult::ExpiredOnArrival:
		mSnapshot.rejectedExpired++; break;
	case NRISmokeAnalyticTrailObserveResult::Capacity: mSnapshot.rejectedCapacity++; break;
	case NRISmokeAnalyticTrailObserveResult::Accepted: break;
	}
	return result;
}

bool NRISmokeAnalyticTrailBridge::TokenMatches(const State& state,
	const NRISmokeAnalyticTrailHandoffToken& token) const
{
	return token.Valid() && token.epoch == mSnapshot.epoch &&
		state.pendingHandoff.stableSourceKey == token.stableSourceKey &&
		state.pendingHandoff.segmentRevision == token.segmentRevision &&
		state.pendingHandoff.serial == token.serial &&
		state.pendingHandoff.epoch == token.epoch;
}
