#include "nri_map_movers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace
{
	RuntimeMapMoverPose InterpolatePose(
		const RuntimeMapMoverPose& previous,
		const RuntimeMapMoverPose& current,
		double fraction)
	{
		fraction = std::clamp(fraction, 0.0, 1.0);
		RuntimeMapMoverPose result = {};
		result.translation = previous.translation + (current.translation - previous.translation) * fraction;
		result.rotation = previous.rotation + deltaangle(previous.rotation, current.rotation) * fraction;
		return result;
	}

	bool SamePose(const RuntimeMapMoverPose& a, const RuntimeMapMoverPose& b)
	{
		return a.translation == b.translation && a.rotation == b.rotation;
	}

	bool SameMember(const RuntimeMapMoverMember& a, const RuntimeMapMoverMember& b)
	{
		return
			a.sectorIndex == b.sectorIndex &&
			a.canonicalWallOffset == b.canonicalWallOffset &&
			a.wallCount == b.wallCount &&
			a.flags == b.flags;
	}

	bool SameMembers(const TArray<RuntimeMapMoverMember>& a, const TArray<RuntimeMapMoverMember>& b)
	{
		if (a.Size() != b.Size())
		{
			return false;
		}
		for (unsigned index = 0; index < a.Size(); ++index)
		{
			if (!SameMember(a[index], b[index]))
			{
				return false;
			}
		}
		return true;
	}

	bool SameTopologyFacts(const RuntimeMapMoverSnapshot& a, const RuntimeMapMoverSnapshot& b)
	{
		return
			a.topologySignature == b.topologySignature &&
			a.capability == b.capability &&
			a.ownerSectorIndex == b.ownerSectorIndex &&
			a.effectorLotag == b.effectorLotag &&
			SameMembers(a.members, b.members);
	}

	const char* GetCapabilityName(RuntimeMapMoverCapability capability)
	{
		switch (capability)
		{
		case RuntimeMapMoverCapability::RigidTranslation: return "rigid-translation";
		case RuntimeMapMoverCapability::RigidTransform: return "rigid-transform";
		case RuntimeMapMoverCapability::StableTopologyDeformer: return "stable-topology-deformer";
		case RuntimeMapMoverCapability::MaterialOrLightOnly: return "material-or-light-only";
		default: return "unknown";
		}
	}

	void ClassifyGeneration(
		uint64_t previousGeneration,
		uint64_t currentGeneration,
		bool factsChanged,
		uint32_t changeBit,
		uint32_t& changeMask,
		uint32_t& changed,
		uint32_t& missed,
		uint32_t& redundant,
		uint32_t& regressed,
		bool validateRedundant)
	{
		const bool generationChanged = previousGeneration != currentGeneration;
		if (generationChanged || factsChanged)
		{
			changeMask |= changeBit;
			changed++;
		}
		if (factsChanged && !generationChanged)
		{
			changeMask |= NRIMapMoverChange_ValidationMismatch;
			missed++;
		}
		if (currentGeneration < previousGeneration)
		{
			changeMask |= NRIMapMoverChange_ValidationMismatch;
			regressed++;
		}
		else if (validateRedundant && !factsChanged && currentGeneration > previousGeneration)
		{
			redundant++;
		}
	}

	void UpdateSnapshotWithoutTopologyCopy(
		RuntimeMapMoverSnapshot& destination,
		const RuntimeMapMoverSnapshot& source)
	{
		destination.topologyGeneration = source.topologyGeneration;
		destination.geometryGeneration = source.geometryGeneration;
		destination.materialGeneration = source.materialGeneration;
		destination.transformGeneration = source.transformGeneration;
		destination.visibilityGeneration = source.visibilityGeneration;
		destination.lightGeneration = source.lightGeneration;
		destination.topologySignature = source.topologySignature;
		destination.geometrySignature = source.geometrySignature;
		destination.materialSignature = source.materialSignature;
		destination.visibilitySignature = source.visibilitySignature;
		destination.lightSignature = source.lightSignature;
		destination.ownerActorIndex = source.ownerActorIndex;
		destination.effectorHitag = source.effectorHitag;
		destination.simulationPreviousPose = source.simulationPreviousPose;
		destination.simulationCurrentPose = source.simulationCurrentPose;
	}
}

uint32_t NRIMapMoverDomainCounters::Total() const
{
	return topology + geometry + material + transform + visibility + light;
}

void NRIMapMoverSystem::IngestFrame(
	const TArray<RuntimeMapMoverSnapshot>& snapshots,
	const RuntimeMapMoverAuthorityState& authority,
	double presentationFraction,
	uint64_t buildSerial)
{
	m_frameStats = {};
	m_frameStats.buildSerial = buildSerial;
	m_frameStats.mapEpoch = authority.mapEpoch;
	m_frameStats.authorityRevision = authority.revision;
	m_frameStats.presentationFraction = presentationFraction;
	m_frameStats.authorityAvailable = authority.available ? 1u : 0u;
	m_frameStats.authoritySampled = 1;
	m_frameStats.capturedSnapshots = snapshots.Size();

	std::vector<const RuntimeMapMoverSnapshot*> orderedSnapshots;
	orderedSnapshots.reserve(snapshots.Size());
	bool invalidFrame = !authority.available;
	if (authority.mapEpoch == 0 || authority.revision == 0)
	{
		m_frameStats.invalidMapEpochs++;
		invalidFrame = true;
	}
	for (const auto& snapshot : snapshots)
	{
		if (snapshot.stableGroupId == 0)
		{
			m_frameStats.invalidGroupIds++;
			invalidFrame = true;
			continue;
		}
		if (snapshot.mapEpoch == 0)
		{
			m_frameStats.invalidMapEpochs++;
			invalidFrame = true;
			continue;
		}
		if (snapshot.mapEpoch != authority.mapEpoch)
		{
			m_frameStats.mixedMapEpochs++;
			invalidFrame = true;
			continue;
		}
		orderedSnapshots.push_back(&snapshot);
	}
	std::sort(orderedSnapshots.begin(), orderedSnapshots.end(),
		[](const RuntimeMapMoverSnapshot* a, const RuntimeMapMoverSnapshot* b)
		{
			return a->stableGroupId < b->stableGroupId;
		});

	uint64_t previousInputGroupId = 0;
	bool hasPreviousInputGroup = false;
	for (const RuntimeMapMoverSnapshot* snapshot : orderedSnapshots)
	{
		if (hasPreviousInputGroup && snapshot->stableGroupId == previousInputGroupId)
		{
			m_frameStats.duplicateGroupIds++;
			invalidFrame = true;
		}
		previousInputGroupId = snapshot->stableGroupId;
		hasPreviousInputGroup = true;
	}

	if (invalidFrame)
	{
		RefreshPresentationPoses(presentationFraction);
		m_frameStats.groupCount = (uint32_t)m_groups.size();
		for (const auto& group : m_groups) m_frameStats.memberCount += group.second.members.Size();
		m_frameStats.queuedGroups = (uint32_t)m_changedGroups.size();
		return;
	}

	const bool epochChanged = m_initialized && authority.mapEpoch != m_mapEpoch;
	const bool buildChanged = m_initialized && buildSerial != m_buildSerial;
	const bool adjacentAuthoritySample = m_initialized && !epochChanged && !buildChanged &&
		authority.revision == m_authorityRevision + 1;
	if (!m_initialized || epochChanged || buildChanged)
	{
		const bool countedReset = m_initialized;
		ResetRegistry(buildSerial, authority.mapEpoch);
		m_frameStats.resetCount = countedReset ? 1u : 0u;
	}
	m_authorityRevision = authority.revision;
	m_frameStats.buildSerial = m_buildSerial;
	m_frameStats.mapEpoch = m_mapEpoch;
	std::set<uint64_t> seenGroups;
	bool sectorLookupDirty = false;
	for (const RuntimeMapMoverSnapshot* snapshotPointer : orderedSnapshots)
	{
		const RuntimeMapMoverSnapshot& snapshot = *snapshotPointer;
		seenGroups.insert(snapshot.stableGroupId);

		auto existing = m_groups.find(snapshot.stableGroupId);
		if (existing == m_groups.end())
		{
			auto inserted = m_groups.emplace(snapshot.stableGroupId, snapshot).first;
			inserted->second.presentationCurrentPose = InterpolatePose(
				snapshot.simulationPreviousPose, snapshot.simulationCurrentPose, presentationFraction);
			inserted->second.presentationPreviousPose = inserted->second.presentationCurrentPose;
			m_frameStats.addedGroups++;
			QueueChangedGroup(inserted->second, NRIMapMoverChange_Added, false);
			sectorLookupDirty = true;
			continue;
		}

		const RuntimeMapMoverSnapshot& previous = existing->second;
		const RuntimeMapMoverPose previousPresentation = previous.presentationCurrentPose;
		const bool topologyFactsChanged = !SameTopologyFacts(previous, snapshot);
		uint32_t changeMask = NRIMapMoverChange_None;
		ClassifyGeneration(previous.topologyGeneration, snapshot.topologyGeneration,
			topologyFactsChanged, NRIMapMoverChange_Topology, changeMask,
			m_frameStats.changed.topology, m_frameStats.missedGenerations.topology,
			m_frameStats.redundantGenerations.topology, m_frameStats.regressedGenerations.topology,
			adjacentAuthoritySample);
		ClassifyGeneration(previous.geometryGeneration, snapshot.geometryGeneration,
			previous.geometrySignature != snapshot.geometrySignature, NRIMapMoverChange_Geometry, changeMask,
			m_frameStats.changed.geometry, m_frameStats.missedGenerations.geometry,
			m_frameStats.redundantGenerations.geometry, m_frameStats.regressedGenerations.geometry,
			adjacentAuthoritySample);
		ClassifyGeneration(previous.materialGeneration, snapshot.materialGeneration,
			previous.materialSignature != snapshot.materialSignature, NRIMapMoverChange_Material, changeMask,
			m_frameStats.changed.material, m_frameStats.missedGenerations.material,
			m_frameStats.redundantGenerations.material, m_frameStats.regressedGenerations.material,
			adjacentAuthoritySample);
		ClassifyGeneration(previous.transformGeneration, snapshot.transformGeneration,
			!SamePose(previous.simulationCurrentPose, snapshot.simulationCurrentPose), NRIMapMoverChange_Transform, changeMask,
			m_frameStats.changed.transform, m_frameStats.missedGenerations.transform,
			m_frameStats.redundantGenerations.transform, m_frameStats.regressedGenerations.transform,
			adjacentAuthoritySample);
		ClassifyGeneration(previous.visibilityGeneration, snapshot.visibilityGeneration,
			previous.visibilitySignature != snapshot.visibilitySignature, NRIMapMoverChange_Visibility, changeMask,
			m_frameStats.changed.visibility, m_frameStats.missedGenerations.visibility,
			m_frameStats.redundantGenerations.visibility, m_frameStats.regressedGenerations.visibility,
			adjacentAuthoritySample);
		ClassifyGeneration(previous.lightGeneration, snapshot.lightGeneration,
			previous.lightSignature != snapshot.lightSignature, NRIMapMoverChange_Light, changeMask,
			m_frameStats.changed.light, m_frameStats.missedGenerations.light,
			m_frameStats.redundantGenerations.light, m_frameStats.regressedGenerations.light,
			adjacentAuthoritySample);

		if (topologyFactsChanged)
		{
			existing->second = snapshot;
			sectorLookupDirty = true;
		}
		else
		{
			UpdateSnapshotWithoutTopologyCopy(existing->second, snapshot);
		}
		existing->second.presentationPreviousPose = previousPresentation;
		existing->second.presentationCurrentPose = InterpolatePose(
			existing->second.simulationPreviousPose,
			existing->second.simulationCurrentPose,
			presentationFraction);
		if (changeMask != NRIMapMoverChange_None)
		{
			m_frameStats.updatedGroups++;
			QueueChangedGroup(existing->second, changeMask, false);
		}
	}

	for (auto group = m_groups.begin(); group != m_groups.end();)
	{
		if (seenGroups.find(group->first) != seenGroups.end())
		{
			++group;
			continue;
		}
		QueueChangedGroup(group->second, NRIMapMoverChange_Removed, true);
		m_frameStats.removedGroups++;
		group = m_groups.erase(group);
		sectorLookupDirty = true;
	}

	if (sectorLookupDirty) RebuildSectorLookup();
	m_frameStats.groupCount = (uint32_t)m_groups.size();
	m_frameStats.memberCount = 0;
	for (const auto& group : m_groups)
	{
		m_frameStats.memberCount += group.second.members.Size();
	}
	m_frameStats.queuedGroups = (uint32_t)m_changedGroups.size();
}

void NRIMapMoverSystem::AdvancePresentationFrame(
	const RuntimeMapMoverAuthorityState& authority,
	double presentationFraction,
	uint64_t buildSerial)
{
	m_frameStats = {};
	m_frameStats.buildSerial = buildSerial;
	m_frameStats.mapEpoch = authority.mapEpoch;
	m_frameStats.authorityRevision = authority.revision;
	m_frameStats.presentationFraction = presentationFraction;
	m_frameStats.authorityAvailable = authority.available ? 1u : 0u;
	if (!authority.available)
	{
		if (m_initialized) Reset();
		return;
	}
	RefreshPresentationPoses(presentationFraction);
	m_frameStats.buildSerial = m_buildSerial;
	m_frameStats.mapEpoch = m_mapEpoch;
	m_frameStats.groupCount = (uint32_t)m_groups.size();
	for (const auto& group : m_groups) m_frameStats.memberCount += group.second.members.Size();
	m_frameStats.queuedGroups = (uint32_t)m_changedGroups.size();
}

void NRIMapMoverSystem::RefreshPresentationPoses(double presentationFraction)
{
	for (auto& groupPair : m_groups)
	{
		auto& snapshot = groupPair.second;
		snapshot.presentationPreviousPose = snapshot.presentationCurrentPose;
		snapshot.presentationCurrentPose = InterpolatePose(
			snapshot.simulationPreviousPose,
			snapshot.simulationCurrentPose,
			presentationFraction);
	}
}

void NRIMapMoverSystem::Reset()
{
	m_groups.clear();
	m_sectorGroups.clear();
	m_changedGroups.clear();
	m_frameStats = {};
	m_buildSerial = 0;
	m_mapEpoch = 0;
	m_authorityRevision = 0;
	m_initialized = false;
}

const RuntimeMapMoverSnapshot* NRIMapMoverSystem::FindGroup(uint64_t stableGroupId) const
{
	const auto found = m_groups.find(stableGroupId);
	return found != m_groups.end() ? &found->second : nullptr;
}

const std::vector<uint64_t>* NRIMapMoverSystem::FindGroupsForSector(int32_t sectorIndex) const
{
	const auto found = m_sectorGroups.find(sectorIndex);
	return found != m_sectorGroups.end() ? &found->second : nullptr;
}

bool NRIMapMoverSystem::PopChangedGroup(NRIMapMoverChangedGroup& changedGroup)
{
	if (m_changedGroups.empty())
	{
		return false;
	}
	changedGroup = std::move(m_changedGroups.begin()->second);
	m_changedGroups.erase(m_changedGroups.begin());
	if (!changedGroup.removed)
	{
		const auto current = m_groups.find(changedGroup.stableGroupId);
		if (current != m_groups.end()) changedGroup.snapshot = current->second;
	}
	return true;
}

void NRIMapMoverSystem::EmitPerfTrace(uint64_t frameIndex, NRIMapMoverPerfSink& sink, bool includeMembers) const
{
	char line[2048];
	std::snprintf(line, sizeof(line),
		"PERF pt map mover summary NRI: frame=%llu build_serial=%llu map_epoch=%llu authority_revision=%llu authority_available=%u authority_sampled=%u presentation_fraction=%.6f captured=%u groups=%u members=%u added=%u updated=%u removed=%u queued=%u coalesced=%u resets=%u invalid_group=%u invalid_epoch=%u duplicate_group=%u mixed_epoch=%u changed_topology=%u changed_geometry=%u changed_material=%u changed_transform=%u changed_visibility=%u changed_light=%u missed_topology=%u missed_geometry=%u missed_material=%u missed_transform=%u missed_visibility=%u missed_light=%u redundant_topology=%u redundant_geometry=%u redundant_material=%u redundant_transform=%u redundant_visibility=%u redundant_light=%u regressed_topology=%u regressed_geometry=%u regressed_material=%u regressed_transform=%u regressed_visibility=%u regressed_light=%u\n",
		(unsigned long long)frameIndex,
		(unsigned long long)m_frameStats.buildSerial,
		(unsigned long long)m_frameStats.mapEpoch,
		(unsigned long long)m_frameStats.authorityRevision,
		m_frameStats.authorityAvailable,
		m_frameStats.authoritySampled,
		m_frameStats.presentationFraction,
		m_frameStats.capturedSnapshots,
		m_frameStats.groupCount,
		m_frameStats.memberCount,
		m_frameStats.addedGroups,
		m_frameStats.updatedGroups,
		m_frameStats.removedGroups,
		m_frameStats.queuedGroups,
		m_frameStats.coalescedChanges,
		m_frameStats.resetCount,
		m_frameStats.invalidGroupIds,
		m_frameStats.invalidMapEpochs,
		m_frameStats.duplicateGroupIds,
		m_frameStats.mixedMapEpochs,
		m_frameStats.changed.topology,
		m_frameStats.changed.geometry,
		m_frameStats.changed.material,
		m_frameStats.changed.transform,
		m_frameStats.changed.visibility,
		m_frameStats.changed.light,
		m_frameStats.missedGenerations.topology,
		m_frameStats.missedGenerations.geometry,
		m_frameStats.missedGenerations.material,
		m_frameStats.missedGenerations.transform,
		m_frameStats.missedGenerations.visibility,
		m_frameStats.missedGenerations.light,
		m_frameStats.redundantGenerations.topology,
		m_frameStats.redundantGenerations.geometry,
		m_frameStats.redundantGenerations.material,
		m_frameStats.redundantGenerations.transform,
		m_frameStats.redundantGenerations.visibility,
		m_frameStats.redundantGenerations.light,
		m_frameStats.regressedGenerations.topology,
		m_frameStats.regressedGenerations.geometry,
		m_frameStats.regressedGenerations.material,
		m_frameStats.regressedGenerations.transform,
		m_frameStats.regressedGenerations.visibility,
		m_frameStats.regressedGenerations.light);
	sink.EmitLine(line);
	if (!includeMembers)
	{
		return;
	}

	for (const auto& groupPair : m_groups)
	{
		const RuntimeMapMoverSnapshot& group = groupPair.second;
		for (const auto& member : group.members)
		{
			std::snprintf(line, sizeof(line),
				"PERF pt map mover member NRI: frame=%llu group=0x%016llx map_epoch=%llu capability=%s owner_actor=%d owner_sector=%d lotag=%d hitag=%d sector=%d wall_offset=%d wall_count=%d flags=0x%x topology_generation=%llu geometry_generation=%llu material_generation=%llu transform_generation=%llu visibility_generation=%llu light_generation=%llu\n",
				(unsigned long long)frameIndex,
				(unsigned long long)group.stableGroupId,
				(unsigned long long)group.mapEpoch,
				GetCapabilityName(group.capability),
				group.ownerActorIndex,
				group.ownerSectorIndex,
				group.effectorLotag,
				group.effectorHitag,
				member.sectorIndex,
				member.canonicalWallOffset,
				member.wallCount,
				member.flags,
				(unsigned long long)group.topologyGeneration,
				(unsigned long long)group.geometryGeneration,
				(unsigned long long)group.materialGeneration,
				(unsigned long long)group.transformGeneration,
				(unsigned long long)group.visibilityGeneration,
				(unsigned long long)group.lightGeneration);
			sink.EmitLine(line);
		}
	}
}

void NRIMapMoverSystem::ResetRegistry(uint64_t buildSerial, uint64_t mapEpoch)
{
	m_groups.clear();
	m_sectorGroups.clear();
	m_changedGroups.clear();
	m_buildSerial = buildSerial;
	m_mapEpoch = mapEpoch;
	m_authorityRevision = 0;
	m_initialized = true;
}

void NRIMapMoverSystem::RebuildSectorLookup()
{
	m_sectorGroups.clear();
	for (const auto& groupPair : m_groups)
	{
		for (const auto& member : groupPair.second.members)
		{
			auto& groups = m_sectorGroups[member.sectorIndex];
			if (groups.empty() || groups.back() != groupPair.first)
			{
				groups.push_back(groupPair.first);
			}
		}
	}
}

void NRIMapMoverSystem::QueueChangedGroup(
	const RuntimeMapMoverSnapshot& snapshot,
	uint32_t changeMask,
	bool removed)
{
	auto found = m_changedGroups.find(snapshot.stableGroupId);
	if (found != m_changedGroups.end())
	{
		auto& changed = found->second;
		if (!removed && (changeMask & NRIMapMoverChange_Added) != 0)
		{
			changed.changeMask &= ~NRIMapMoverChange_Removed;
		}
		changed.changeMask |= changeMask;
		changed.removed = removed;
		changed.snapshot = snapshot;
		m_frameStats.coalescedChanges++;
		return;
	}

	NRIMapMoverChangedGroup changed = {};
	changed.stableGroupId = snapshot.stableGroupId;
	changed.changeMask = changeMask;
	changed.removed = removed;
	changed.snapshot = snapshot;
	m_changedGroups.emplace(changed.stableGroupId, std::move(changed));
}
