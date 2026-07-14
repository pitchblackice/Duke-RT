#pragma once

#include "runtime_map_mover.h"

#include <cstdint>
#include <map>
#include <vector>

enum NRIMapMoverChangeBits : uint32_t
{
	NRIMapMoverChange_None = 0,
	NRIMapMoverChange_Added = 1u << 0,
	NRIMapMoverChange_Removed = 1u << 1,
	NRIMapMoverChange_Topology = 1u << 2,
	NRIMapMoverChange_Geometry = 1u << 3,
	NRIMapMoverChange_Material = 1u << 4,
	NRIMapMoverChange_Transform = 1u << 5,
	NRIMapMoverChange_Visibility = 1u << 6,
	NRIMapMoverChange_Light = 1u << 7,
	NRIMapMoverChange_ValidationMismatch = 1u << 8,
	NRIMapMoverChange_Lifecycle = 1u << 9,
};

struct NRIMapMoverDomainCounters
{
	uint32_t topology = 0;
	uint32_t geometry = 0;
	uint32_t material = 0;
	uint32_t transform = 0;
	uint32_t visibility = 0;
	uint32_t light = 0;

	uint32_t Total() const;
};

struct NRIMapMoverFrameStats
{
	uint64_t buildSerial = 0;
	uint64_t mapEpoch = 0;
	uint64_t authorityRevision = 0;
	double presentationFraction = 0.0;
	uint32_t authorityAvailable = 0;
	uint32_t authoritySampled = 0;
	uint32_t capturedSnapshots = 0;
	uint32_t groupCount = 0;
	uint32_t memberCount = 0;
	uint32_t addedGroups = 0;
	uint32_t updatedGroups = 0;
	uint32_t removedGroups = 0;
	uint32_t changedLifecycles = 0;
	uint32_t queuedGroups = 0;
	uint32_t coalescedChanges = 0;
	uint32_t resetCount = 0;
	uint32_t invalidGroupIds = 0;
	uint32_t invalidMapEpochs = 0;
	uint32_t duplicateGroupIds = 0;
	uint32_t mixedMapEpochs = 0;
	NRIMapMoverDomainCounters changed;
	NRIMapMoverDomainCounters missedGenerations;
	NRIMapMoverDomainCounters redundantGenerations;
	NRIMapMoverDomainCounters regressedGenerations;
};

struct NRIMapMoverChangedGroup
{
	uint64_t stableGroupId = 0;
	uint32_t changeMask = NRIMapMoverChange_None;
	bool removed = false;
	RuntimeMapMoverSnapshot snapshot;
};

class NRIMapMoverPerfSink
{
public:
	virtual ~NRIMapMoverPerfSink() = default;
	virtual void EmitLine(const char* line) = 0;
};

class NRIMapMoverSystem
{
public:
	template<class GameInterfaceType>
	void CaptureFrame(
		GameInterfaceType* gameInterface,
		double presentationFraction,
		uint64_t buildSerial)
	{
		RuntimeMapMoverAuthorityState authority = {};
		if (gameInterface != nullptr)
		{
			authority = gameInterface->GetRuntimeMapMoverAuthorityState();
		}
		const bool needsSample = authority.available &&
			(!m_initialized || buildSerial != m_buildSerial || authority.mapEpoch != m_mapEpoch ||
				authority.revision != m_authorityRevision);
		if (needsSample)
		{
			TArray<RuntimeMapMoverSnapshot> snapshots;
			gameInterface->CaptureRuntimeMapMovers(snapshots);
			IngestFrame(snapshots, authority, presentationFraction, buildSerial);
		}
		else
		{
			AdvancePresentationFrame(authority, presentationFraction, buildSerial);
		}
	}

	void IngestFrame(
		const TArray<RuntimeMapMoverSnapshot>& snapshots,
		const RuntimeMapMoverAuthorityState& authority,
		double presentationFraction,
		uint64_t buildSerial);
	void Reset();

	const RuntimeMapMoverSnapshot* FindGroup(uint64_t stableGroupId) const;
	const std::vector<uint64_t>* FindGroupsForSector(int32_t sectorIndex) const;
	bool PopChangedGroup(NRIMapMoverChangedGroup& changedGroup);
	uint32_t GetGroupCount() const { return (uint32_t)m_groups.size(); }
	uint32_t GetPendingChangedGroupCount() const { return (uint32_t)m_changedGroups.size(); }
	uint64_t GetBuildSerial() const { return m_buildSerial; }
	uint64_t GetMapEpoch() const { return m_mapEpoch; }
	const NRIMapMoverFrameStats& GetFrameStats() const { return m_frameStats; }

	void EmitPerfTrace(uint64_t frameIndex, NRIMapMoverPerfSink& sink, bool includeMembers = false) const;

private:
	using GroupMap = std::map<uint64_t, RuntimeMapMoverSnapshot>;

	void ResetRegistry(uint64_t buildSerial, uint64_t mapEpoch);
	void AdvancePresentationFrame(
		const RuntimeMapMoverAuthorityState& authority,
		double presentationFraction,
		uint64_t buildSerial);
	void RefreshPresentationPoses(double presentationFraction);
	void RebuildSectorLookup();
	void QueueChangedGroup(const RuntimeMapMoverSnapshot& snapshot, uint32_t changeMask, bool removed);

	GroupMap m_groups;
	std::map<int32_t, std::vector<uint64_t>> m_sectorGroups;
	std::map<uint64_t, NRIMapMoverChangedGroup> m_changedGroups;
	NRIMapMoverFrameStats m_frameStats;
	uint64_t m_buildSerial = 0;
	uint64_t m_mapEpoch = 0;
	uint64_t m_authorityRevision = 0;
	bool m_initialized = false;
};
