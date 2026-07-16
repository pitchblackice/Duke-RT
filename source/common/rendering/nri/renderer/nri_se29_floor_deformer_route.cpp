#include "nri_se29_floor_deformer_route.h"

#include "nri_map_movers.h"
#include "nri_static_scene.h"
#include "../scene/nri_map_world.h"

#include <algorithm>

namespace
{
	constexpr uint64_t ScheduledCandidateMaxWaitFrames = 32;
	// The semantic fast path and topology fallback both compare primitive
	// candidates quadratically. Keep the live lane on the small deformer chunks
	// it can update cheaply; larger mixed chunks stay on the exact path until a
	// retained semantic-index owner removes that scan.
	constexpr uint32_t LiveCanonicalMappingMaxPrimitives = 64;

	constexpr uint64_t HashOffset = 1469598103934665603ull;
	constexpr uint64_t HashPrime = 1099511628211ull;

	void HashBytes(uint64_t& hash, const void* data, size_t size)
	{
		const auto* bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < size; ++i)
		{
			hash = (hash ^ bytes[i]) * HashPrime;
		}
	}

	template<typename T>
	void HashValue(uint64_t& hash, const T& value)
	{
		HashBytes(hash, &value, sizeof(value));
	}

	uint64_t FinishHash(uint64_t hash)
	{
		return hash != 0 ? hash : 1;
	}

	void HashProvenance(uint64_t& hash, const nri_scene::SurfaceProvenance& provenance)
	{
		HashValue(hash, provenance.sourceType);
		HashValue(hash, provenance.sectorIndex);
		HashValue(hash, provenance.wallIndex);
		HashValue(hash, provenance.sectionIndex);
		HashValue(hash, provenance.mapChunkIndex);
		HashValue(hash, provenance.nextSectorIndex);
		HashValue(hash, provenance.actorIndex);
		HashValue(hash, provenance.drawListType);
		HashValue(hash, provenance.cstat);
		HashValue(hash, provenance.materialFlags);
		HashValue(hash, provenance.actorOverlayRuleCount);
		for (uint32_t ruleId : provenance.actorOverlayRuleIds)
		{
			HashValue(hash, ruleId);
		}
	}

	bool AppendUnique(std::vector<int32_t>& values, int32_t value)
	{
		if (value < 0 || std::find(values.begin(), values.end(), value) != values.end())
		{
			return false;
		}
		values.push_back(value);
		return true;
	}

	bool AppendUnique(std::vector<uint64_t>& values, uint64_t value)
	{
		if (value == 0 || std::find(values.begin(), values.end(), value) != values.end())
		{
			return false;
		}
		values.push_back(value);
		return true;
	}

	struct DependencyFacts
	{
		std::vector<uint64_t> groupIds;
		std::vector<uint64_t> se29GroupIds;
		uint64_t membership = HashOffset;
		NRISE29FloorDeformerDependencyStamp stamp;
	};

	bool BuildDependencyFacts(
		const NRIMapMoverSystem& movers,
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& mapChunk,
		DependencyFacts& outFacts)
	{
		std::vector<int32_t> sectors;
		AppendUnique(sectors, mapChunk.sectorIndex);
		const uint32_t surfaceEnd = std::min<uint32_t>(
			mapChunk.firstSurface + mapChunk.surfaceCount,
			(uint32_t)mapWorld.surfaces.size());
		for (uint32_t surfaceIndex = mapChunk.firstSurface; surfaceIndex < surfaceEnd; ++surfaceIndex)
		{
			const auto& provenance = mapWorld.surfaces[surfaceIndex].surface.provenance;
			AppendUnique(sectors, provenance.sectorIndex);
			AppendUnique(sectors, provenance.nextSectorIndex);
		}

		for (int32_t sectorIndex : sectors)
		{
			const std::vector<uint64_t>* groups = movers.FindGroupsForSector(sectorIndex);
			if (groups == nullptr)
			{
				continue;
			}
			for (uint64_t groupId : *groups)
			{
				AppendUnique(outFacts.groupIds, groupId);
			}
		}
		std::sort(outFacts.groupIds.begin(), outFacts.groupIds.end());
		if (outFacts.groupIds.empty())
		{
			return false;
		}

		uint64_t authority = HashOffset;
		uint64_t dependency = HashOffset;
		uint64_t topology = HashOffset;
		uint64_t membership = HashOffset;
		uint64_t material = HashOffset;
		for (uint64_t groupId : outFacts.groupIds)
		{
			const RuntimeMapMoverSnapshot* snapshot = movers.FindGroup(groupId);
			if (snapshot == nullptr)
			{
				return false;
			}
			HashValue(dependency, groupId);
			HashValue(dependency, snapshot->topologyGeneration);
			HashValue(dependency, snapshot->geometryGeneration);
			HashValue(dependency, snapshot->materialGeneration);
			HashValue(dependency, snapshot->transformGeneration);
			HashValue(dependency, snapshot->visibilityGeneration);
			HashValue(dependency, snapshot->lightGeneration);
			HashValue(topology, groupId);
			HashValue(topology, snapshot->topologyGeneration);
			HashValue(topology, snapshot->topologySignature);
			HashValue(membership, groupId);
			HashValue(membership, snapshot->capability);
			for (const RuntimeMapMoverMember& member : snapshot->members)
			{
				HashValue(membership, member.sectorIndex);
				HashValue(membership, member.canonicalWallOffset);
				HashValue(membership, member.wallCount);
				HashValue(membership, member.flags);
			}
			HashValue(material, groupId);
			HashValue(material, snapshot->materialGeneration);
			HashValue(material, snapshot->visibilityGeneration);
			if (snapshot->capability == RuntimeMapMoverCapability::StableTopologyDeformer &&
				snapshot->effectorLotag == (int32_t)NRI_SE29_FLOOR_DEFORMER_LOTAG &&
				snapshot->deformer.kind == RuntimeMapMoverDeformerKind::SectorFloorPlane &&
				std::find(sectors.begin(), sectors.end(), snapshot->deformer.sectorIndex) != sectors.end())
			{
				outFacts.se29GroupIds.push_back(groupId);
				HashValue(authority, groupId);
				HashValue(authority, snapshot->geometryGeneration);
			}
		}
		if (outFacts.se29GroupIds.empty())
		{
			return false;
		}

		outFacts.membership = FinishHash(membership);
		outFacts.stamp.mapEpoch = movers.GetMapEpoch();
		outFacts.stamp.authorityGeneration = FinishHash(authority);
		outFacts.stamp.dependencyGeneration = FinishHash(dependency);
		outFacts.stamp.topologyGeneration = FinishHash(topology);
		outFacts.stamp.membershipGeneration = outFacts.membership;
		outFacts.stamp.geometryGeneration = outFacts.stamp.authorityGeneration;
		outFacts.stamp.materialSlotGeneration = FinishHash(material);
		return outFacts.stamp.mapEpoch != 0;
	}

	NRISE29FloorDeformerLayoutFingerprint BuildLayoutFingerprint(
		const NRISE29FloorDeformerIdentity& identity,
		uint32_t memberCount,
		uint64_t membershipFingerprint,
		const nri_scene::GeometryData& geometry,
		const nri_scene::MapDeformerGeometrySlice& slice)
	{
		NRISE29FloorDeformerLayoutFingerprint result;
		result.identity = identity;
		result.memberCount = memberCount;
		result.vertexCount = slice.vertexCount;
		result.indexCount = slice.indexCount;
		result.primitiveCount = slice.primitiveCount;
		result.membershipFingerprint = membershipFingerprint;

		uint64_t topology = HashOffset;
		uint64_t vertexOrder = HashOffset;
		uint64_t indexOrder = HashOffset;
		uint64_t primitiveOrder = HashOffset;
		uint64_t provenance = HashOffset;
		uint64_t materialSlots = HashOffset;
		HashValue(topology, slice.vertexCount);
		HashValue(topology, slice.indexCount);
		HashValue(topology, slice.primitiveCount);
		for (uint32_t i = 0; i < slice.vertexCount; ++i)
		{
			// The canonical mapper has already restored resident vertex order.
			// Position is mutable for a fixed-layout deformer and for rigid
			// dependencies that share its storage chunk, so it must not define
			// layout identity.
			HashValue(vertexOrder, i);
		}
		for (uint32_t i = 0; i < slice.indexCount; ++i)
		{
			const uint32_t normalizedIndex = geometry.indices[slice.indexOffset + i] - slice.vertexOffset;
			HashValue(topology, normalizedIndex);
			HashValue(indexOrder, normalizedIndex);
		}
		for (uint32_t i = 0; i < slice.primitiveCount; ++i)
		{
			const auto& primitive = geometry.primitives[slice.primitiveOffset + i];
			const auto& primitiveProvenance = geometry.primitiveProvenance[slice.primitiveOffset + i];
			HashProvenance(provenance, primitiveProvenance);
			HashProvenance(primitiveOrder, primitiveProvenance);
			for (uint32_t corner = 0; corner < 3; ++corner)
			{
				const uint32_t normalizedIndex = primitive.indices[corner] - slice.vertexOffset;
				HashValue(primitiveOrder, normalizedIndex);
			}
			const uint32_t materialSlot = primitive.materialIndex - slice.materialOffset;
			HashValue(materialSlots, materialSlot);
		}
		result.topologyFingerprint = FinishHash(topology);
		result.vertexOrderFingerprint = FinishHash(vertexOrder);
		result.indexOrderFingerprint = FinishHash(indexOrder);
		result.primitiveOrderFingerprint = FinishHash(primitiveOrder);
		result.primitiveProvenanceFingerprint = FinishHash(provenance);
		result.materialSlotLayoutFingerprint = FinishHash(materialSlots);
		return result;
	}

	uint64_t BuildStableKey(const DependencyFacts& facts, uint32_t chunkIndex)
	{
		uint64_t hash = HashOffset;
		const uint32_t kind = 0x53453239u;
		HashValue(hash, kind);
		HashValue(hash, chunkIndex);
		for (uint64_t groupId : facts.se29GroupIds)
		{
			HashValue(hash, groupId);
		}
		return FinishHash(hash);
	}
}

void NRISE29FloorDeformerRoute::BeginFrame(uint64_t frameIndex, uint64_t buildSerial, uint64_t mapEpoch)
{
	const bool reset =
		buildSerial == 0 || mapEpoch == 0 ||
		buildSerial != m_buildSerial || mapEpoch != m_mapEpoch;
	if (reset)
	{
		m_pending.Clear();
		m_scheduled.clear();
		m_pendingAges.clear();
		m_scheduledSinceFrame = 0;
		m_pendingHighWater = 0;
		m_buildSerial = buildSerial;
		m_mapEpoch = mapEpoch;
	}
	else
	{
		if (!m_scheduled.empty() &&
			frameIndex - m_scheduledSinceFrame > ScheduledCandidateMaxWaitFrames)
		{
			m_scheduled.clear();
			m_scheduledSinceFrame = 0;
		}
		m_pendingAges.erase(
			std::remove_if(
				m_pendingAges.begin(),
				m_pendingAges.end(),
				[frameIndex](const PendingAge& age)
				{
					return age.lastSeenFrame + 1u < frameIndex;
				}),
			m_pendingAges.end());
		std::vector<NRISE29FloorDeformerIdentity> stalePending;
		for (const NRISE29FloorDeformerPendingWork& pending : m_pending.Items())
		{
			const auto age = std::find_if(
				m_pendingAges.begin(),
				m_pendingAges.end(),
				[&pending](const PendingAge& candidate)
				{
					return candidate.identity == pending.residentLayout.identity;
				});
			if (age == m_pendingAges.end())
			{
				stalePending.push_back(pending.residentLayout.identity);
			}
		}
		for (const NRISE29FloorDeformerIdentity& identity : stalePending)
		{
			m_pending.Remove(identity);
		}

		// Arbitrate every compatible candidate observed since the previous
		// admission. A scheduled identity remains reserved for a bounded window
		// because exact structural rebuilds can visit a deformer intermittently.
		// Every non-selected observation still renders through the exact path.
		if (m_scheduled.empty())
		{
			const NRISE29FloorDeformerBatchPlan plan =
				SelectNRISE29FloorDeformerWork(m_pending.Items());
			std::vector<NRISE29FloorDeformerIdentity> removePending;
			for (const NRISE29FloorDeformerDecision& decision : plan.decisions)
			{
				if (decision.action == NRISE29FloorDeformerAction::Refit)
				{
					m_scheduled.push_back(decision.identity);
					removePending.push_back(decision.identity);
				}
				else if ((decision.rejectMask &
					(NRISE29FloorDeformerReject_ChunkBudget |
						NRISE29FloorDeformerReject_PrimitiveBudget |
						NRISE29FloorDeformerReject_UploadBudget)) == 0)
				{
					removePending.push_back(decision.identity);
				}
			}
			for (const NRISE29FloorDeformerIdentity& identity : removePending)
			{
				m_pending.Remove(identity);
			}
			if (!m_scheduled.empty())
			{
				m_scheduledSinceFrame = frameIndex;
			}
		}
	}
	m_remainingBudget = {};
	m_frameStats = {};
	m_frameStats.frameIndex = frameIndex;
	m_frameStats.scheduled = (uint32_t)m_scheduled.size();
	m_frameStats.pending = m_pending.Size();
	m_frameStats.pendingHighWater = m_pendingHighWater;
}

NRISE29FloorDeformerRouteResult NRISE29FloorDeformerRoute::TryCanonicalize(
	const NRISE29FloorDeformerRouteInput& input)
{
	NRISE29FloorDeformerRouteResult result;
	if (!input.enabled || input.movers == nullptr || input.mapWorld == nullptr ||
		input.staticScene == nullptr || input.atlas == nullptr || input.registry == nullptr ||
		input.mapChunk == nullptr || input.exactCurrentGeometry == nullptr)
	{
		return result;
	}

	DependencyFacts dependencies;
	if (!BuildDependencyFacts(*input.movers, *input.mapWorld, *input.mapChunk, dependencies))
	{
		return result;
	}
	result.candidate = true;
	m_frameStats.candidates++;
	m_frameStats.dependencyGroups += (uint32_t)dependencies.groupIds.size();
	result.stableKey = BuildStableKey(dependencies, input.mapChunk->chunkIndex);

	const uint32_t chunkIndex = input.mapChunk->chunkIndex;
	NRISE29FloorDeformerIdentity identity;
	identity.stableKey = result.stableKey;
	identity.chunkIndex = chunkIndex;
	auto releaseCurrentReservation = [this, &identity]()
	{
		m_scheduled.erase(
			std::remove(m_scheduled.begin(), m_scheduled.end(), identity),
			m_scheduled.end());
		m_pending.Remove(identity);
		m_scheduledSinceFrame = m_scheduled.empty() ? 0 : m_scheduledSinceFrame;
		m_frameStats.scheduled = (uint32_t)m_scheduled.size();
		m_frameStats.pending = m_pending.Size();
	};
	if (!input.staticScene->valid || !input.staticScene->buffersResident ||
		!input.staticScene->accelerationResident || !input.atlas->valid ||
		input.atlas->buildSerial != input.staticScene->buildSerial ||
		chunkIndex >= input.registry->entries.size())
	{
		releaseCurrentReservation();
		m_frameStats.residentRejects++;
		m_frameStats.exactFallbacks++;
		return result;
	}
	const auto& registryEntry = input.registry->entries[chunkIndex];
	const uint32_t chunkListIndex = registryEntry.staticSceneChunkListIndex;
	if (!registryEntry.valid || !registryEntry.active || !registryEntry.mappedInStaticScene ||
		!registryEntry.accelerationResident || chunkListIndex >= input.staticScene->chunks.size() ||
		chunkListIndex >= input.atlas->chunks.size())
	{
		releaseCurrentReservation();
		m_frameStats.residentRejects++;
		m_frameStats.exactFallbacks++;
		return result;
	}
	const auto& residentChunk = input.staticScene->chunks[chunkListIndex];
	const auto& atlasChunk = input.atlas->chunks[chunkListIndex];
	if (!residentChunk.active || residentChunk.chunkIndex != chunkIndex || !atlasChunk.valid ||
		atlasChunk.chunkIndex != chunkIndex || residentChunk.accelerationStructure.accelerationStructure == nullptr ||
		!residentChunk.accelerationStructure.buildTypeValid ||
		residentChunk.accelerationStructure.buildType != nri::AccelerationStructureType::BOTTOM_LEVEL ||
		((uint32_t)residentChunk.accelerationStructure.buildFlags &
			(uint32_t)nri::AccelerationStructureBits::ALLOW_UPDATE) == 0 ||
		residentChunk.accelerationStructure.compacted ||
		residentChunk.residentBlasVertexBuffer == nullptr || residentChunk.residentBlasIndexBuffer == nullptr ||
		residentChunk.residentBlasVertexNum != input.atlas->vertexCount ||
		residentChunk.residentBlasIndexOffset != (uint64_t)atlasChunk.indexOffset * sizeof(uint32_t) ||
		residentChunk.residentBlasIndexNum != atlasChunk.indexCount ||
		atlasChunk.vertexCount != input.exactCurrentGeometry->vertices.size() ||
		atlasChunk.indexCount != input.exactCurrentGeometry->indices.size() ||
		atlasChunk.primitiveCount != input.exactCurrentGeometry->primitives.size())
	{
		releaseCurrentReservation();
		m_frameStats.residentRejects++;
		m_frameStats.exactFallbacks++;
		return result;
	}

	nri_scene::MapDeformerGeometrySlice retainedSlice;
	retainedSlice.vertexOffset = atlasChunk.vertexOffset;
	retainedSlice.vertexCount = atlasChunk.vertexCount;
	retainedSlice.indexOffset = atlasChunk.indexOffset;
	retainedSlice.indexCount = atlasChunk.indexCount;
	retainedSlice.primitiveOffset = atlasChunk.primitiveOffset;
	retainedSlice.primitiveCount = atlasChunk.primitiveCount;
	retainedSlice.materialOffset = atlasChunk.materialOffset;
	retainedSlice.materialCount = atlasChunk.materialCount;
	if (retainedSlice.primitiveCount > LiveCanonicalMappingMaxPrimitives)
	{
		releaseCurrentReservation();
		result.layoutRejectMask = nri_scene::MapDeformerLayoutReject_WorkBudget;
		m_frameStats.layoutRejectMaskOr |= result.layoutRejectMask;
		m_frameStats.exactFallbacks++;
		return result;
	}
	nri_scene::MapDeformerLayoutMapping mapping =
		nri_scene::MapCurrentGeometryToRetainedDeformerLayout(
			input.staticScene->geometry,
			retainedSlice,
			*input.exactCurrentGeometry);
	if (!mapping.compatible)
	{
		releaseCurrentReservation();
		result.layoutRejectMask = mapping.rejectMask;
		m_frameStats.layoutRejectMaskOr |= mapping.rejectMask;
		m_frameStats.exactFallbacks++;
		return result;
	}

	auto age = std::find_if(
		m_pendingAges.begin(),
		m_pendingAges.end(),
		[&identity](const PendingAge& candidate) { return candidate.identity == identity; });
	if (age == m_pendingAges.end())
	{
		m_pendingAges.push_back({ identity, input.frameIndex, input.frameIndex });
		age = m_pendingAges.end() - 1;
	}
	else
	{
		age->lastSeenFrame = input.frameIndex;
	}
	nri_scene::MapDeformerGeometrySlice currentSlice;
	currentSlice.vertexCount = (uint32_t)mapping.canonicalCurrent.vertices.size();
	currentSlice.indexCount = (uint32_t)mapping.canonicalCurrent.indices.size();
	currentSlice.primitiveCount = (uint32_t)mapping.canonicalCurrent.primitives.size();
	currentSlice.materialCount = retainedSlice.materialCount;
	NRISE29FloorDeformerPendingWork work;
	work.residentLayout = BuildLayoutFingerprint(
		identity,
		(uint32_t)dependencies.groupIds.size(),
		dependencies.membership,
		input.staticScene->geometry,
		retainedSlice);
	work.currentLayout = BuildLayoutFingerprint(
		identity,
		(uint32_t)dependencies.groupIds.size(),
		dependencies.membership,
		mapping.canonicalCurrent,
		currentSlice);
	work.capturedStamp = dependencies.stamp;
	work.currentStamp = dependencies.stamp;
	work.observationFrame = input.frameIndex;
	work.pendingSinceFrame = age->firstUnservedFrame;
	work.effectorLotag = NRI_SE29_FLOOR_DEFORMER_LOTAG;
	work.floorPlaneOnly = true;
	work.rayVisible = input.rayVisible;
	work.required = input.required;
	work.usesSmoothNormals = false;
	work.vertexMutableFieldMask = mapping.changedVertexSpans.empty() ?
		NRISE29FloorDeformerVertexMutable_None :
		(NRISE29FloorDeformerVertexMutable_Position |
			NRISE29FloorDeformerVertexMutable_PreviousPosition |
			NRISE29FloorDeformerVertexMutable_TextureCoordinates);
	work.primitiveMutableFieldMask = mapping.changedPrimitiveSpans.empty() ?
		NRISE29FloorDeformerPrimitiveMutable_None :
		(NRISE29FloorDeformerPrimitiveMutable_TextureCoordinates |
			NRISE29FloorDeformerPrimitiveMutable_GeometricNormal);
	work.vertexStrideBytes = sizeof(nri_scene::SceneVertex);
	work.primitiveStrideBytes = sizeof(nri_scene::PrimitiveData);
	for (const nri_scene::MapDeformerChangedSpan& span : mapping.changedVertexSpans)
	{
		work.vertexDirtySpans.push_back({ span.sourceElementOffset, span.elementCount });
	}
	for (const nri_scene::MapDeformerChangedSpan& span : mapping.changedPrimitiveSpans)
	{
		work.primitiveDirtySpans.push_back({ span.sourceElementOffset, span.elementCount });
	}

	m_pending.QueueLatest(work);
	m_pendingHighWater = std::max(m_pendingHighWater, m_pending.Size());
	m_frameStats.pending = m_pending.Size();
	m_frameStats.pendingHighWater = m_pendingHighWater;
	for (const auto& pending : m_pending.Items())
	{
		if (input.frameIndex >= pending.pendingSinceFrame)
		{
			m_frameStats.maxPendingAge = std::max(
				m_frameStats.maxPendingAge,
				input.frameIndex - pending.pendingSinceFrame);
		}
	}

	const bool scheduled =
		std::find(m_scheduled.begin(), m_scheduled.end(), identity) != m_scheduled.end();
	const NRISE29FloorDeformerBatchPlan currentPlan =
		SelectNRISE29FloorDeformerWork({ work }, m_remainingBudget);
	const NRISE29FloorDeformerDecision* decision =
		currentPlan.decisions.empty() ? nullptr : &currentPlan.decisions.front();
	if (!scheduled || decision == nullptr ||
		decision->action != NRISE29FloorDeformerAction::Refit)
	{
		if (!scheduled)
		{
			result.policyRejectMask = NRISE29FloorDeformerReject_ChunkBudget;
			m_frameStats.budgetDeferred++;
		}
		else if (decision != nullptr)
		{
			result.policyRejectMask = decision->rejectMask;
			m_scheduled.erase(
				std::remove(m_scheduled.begin(), m_scheduled.end(), identity),
				m_scheduled.end());
			m_scheduledSinceFrame = m_scheduled.empty() ? 0 : m_scheduledSinceFrame;
			m_frameStats.scheduled = (uint32_t)m_scheduled.size();
		}
		m_frameStats.policyRejectMaskOr |= result.policyRejectMask;
		m_frameStats.exactFallbacks++;
		return result;
	}

	m_remainingBudget.maxChunks -= 1;
	m_remainingBudget.maxPrimitives -= decision->primitiveCost;
	m_remainingBudget.maxUploadBytes -= decision->uploadBytes;
	age->firstUnservedFrame = input.frameIndex;
	m_pending.Remove(identity);
	m_scheduled.erase(
		std::remove(m_scheduled.begin(), m_scheduled.end(), identity),
		m_scheduled.end());
	m_scheduledSinceFrame = m_scheduled.empty() ? 0 : m_scheduledSinceFrame;
	m_frameStats.scheduled = (uint32_t)m_scheduled.size();
	m_frameStats.pending = m_pending.Size();
	m_frameStats.admitted++;
	m_frameStats.plannedRefitChunks++;
	m_frameStats.vertexSpans += (uint32_t)mapping.changedVertexSpans.size();
	m_frameStats.primitiveSpans += (uint32_t)mapping.changedPrimitiveSpans.size();
	m_frameStats.vertexBytes += mapping.changedVertexBytes;
	m_frameStats.primitiveBytes += mapping.changedPrimitiveBytes;
	result.admitted = true;
	result.canonicalGeometry = std::move(mapping.canonicalCurrent);
	for (const nri_scene::MapDeformerChangedSpan& span : mapping.changedVertexSpans)
	{
		result.vertexSpans.push_back({ span.sourceElementOffset, span.elementCount });
	}
	for (const nri_scene::MapDeformerChangedSpan& span : mapping.changedPrimitiveSpans)
	{
		result.primitiveSpans.push_back({ span.sourceElementOffset, span.elementCount });
	}
	return result;
}

void NRISE29FloorDeformerRoute::NotePartialUpload(
	uint64_t stableKey,
	uint32_t vertexSpanCount,
	uint32_t primitiveSpanCount,
	uint64_t vertexBytes,
	uint64_t primitiveBytes)
{
	if (stableKey == 0 || (vertexSpanCount == 0 && primitiveSpanCount == 0))
	{
		return;
	}
	m_frameStats.partialUploadChunks++;
	m_frameStats.partialUploadVertexSpans += vertexSpanCount;
	m_frameStats.partialUploadPrimitiveSpans += primitiveSpanCount;
	m_frameStats.partialUploadVertexBytes += vertexBytes;
	m_frameStats.partialUploadPrimitiveBytes += primitiveBytes;
}

void NRISE29FloorDeformerRoute::NoteApplyFailure(uint64_t stableKey)
{
	if (stableKey != 0)
	{
		m_frameStats.applyFailures++;
	}
}

void NRISE29FloorDeformerRoute::NoteBlasUpdated(uint64_t stableKey)
{
	if (stableKey != 0)
	{
		m_frameStats.blasUpdated++;
	}
}

void NRISE29FloorDeformerRoute::NoteBlasRecreated(uint64_t stableKey)
{
	if (stableKey != 0)
	{
		m_frameStats.blasRecreated++;
	}
}

void NRISE29FloorDeformerRoute::Reset()
{
	m_pending.Clear();
	m_remainingBudget = {};
	m_scheduled.clear();
	m_pendingAges.clear();
	m_frameStats = {};
	m_buildSerial = 0;
	m_mapEpoch = 0;
	m_scheduledSinceFrame = 0;
	m_pendingHighWater = 0;
}
