#pragma once

#include "nri_map_movers.h"
#include "nri_map_mover_shadow_state.h"

#include <cstdint>
#include <map>

namespace nri_scene
{
	struct PTMapWorld;
}

class NRIMapMoverShadow
{
public:
	void CaptureChangedGroups(
		NRIMapMoverSystem& movers,
		const nri_scene::PTMapWorld& mapWorld,
		uint64_t frameIndex,
		int traceMode,
		uint32_t groupBudget = 16);
	void ObserveLiveChunk(
		const NRIMapMoverSystem& movers,
		const nri_scene::PTMapWorld& liveWorld,
		uint64_t frameIndex,
		int traceMode);
	void EndFrame(
		const NRIMapMoverSystem& movers,
		const nri_scene::PTMapWorld& mapWorld,
		uint64_t frameIndex,
		int traceMode);
	void Reset()
	{
		m_state.Reset();
		m_pendingPairs.clear();
		m_observedPairs.clear();
		m_frameStats = {};
		m_frameIndex = UINT64_MAX;
		m_buildSerial = 0;
		m_mapEpoch = 0;
		m_cumulativeWrapperFailures = 0;
	}

	const NRIMapMoverShadowState& GetState() const { return m_state; }

private:
	struct PendingPair
	{
		RuntimeMapMoverMember member;
		uint32_t changeMask = 0;
		uint64_t firstQueuedFrame = 0;
		uint64_t lastAttemptFrame = 0;
		uint32_t attemptCount = 0;
	};

	struct FrameStats
	{
		uint32_t queuedBefore = 0;
		uint32_t queuedAfter = 0;
		uint32_t groupsCaptured = 0;
		uint32_t groupsRemoved = 0;
		uint32_t groupsWithoutGeometry = 0;
		uint32_t pairsQueued = 0;
		uint32_t pairsCoalesced = 0;
		uint32_t memberObservations = 0;
		uint32_t canonicalCreates = 0;
		uint32_t validComparisons = 0;
		uint32_t failures = 0;
		uint32_t quarantined = 0;
		uint32_t rigid = 0;
		uint32_t deformer = 0;
		uint32_t topology = 0;
		uint32_t material = 0;
		uint32_t unknown = 0;
	};

	NRIMapMoverShadowState m_state;
	std::map<NRIMapMoverShadowRecordKey, PendingPair> m_pendingPairs;
	std::map<NRIMapMoverShadowRecordKey, RuntimeMapMoverMember> m_observedPairs;
	FrameStats m_frameStats;
	uint64_t m_frameIndex = UINT64_MAX;
	uint64_t m_buildSerial = 0;
	uint64_t m_mapEpoch = 0;
	uint64_t m_cumulativeWrapperFailures = 0;
};

const char* GetNRIMapMoverShadowQuarantineName(uint32_t quarantineBit);
