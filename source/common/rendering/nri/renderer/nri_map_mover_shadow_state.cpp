#include "nri_map_mover_shadow_state.h"

#include <algorithm>
#include <cmath>

namespace
{
	using namespace nri_scene;

	struct Bounds
	{
		bool valid = false;
		double minimum[3] = {};
		double maximum[3] = {};
	};

	void Include(Bounds& bounds, const WorldMapMoverPosition& position)
	{
		if (!bounds.valid)
		{
			for (int component = 0; component < 3; ++component)
			{
				bounds.minimum[component] = position.value[component];
				bounds.maximum[component] = position.value[component];
			}
			bounds.valid = true;
			return;
		}
		for (int component = 0; component < 3; ++component)
		{
			bounds.minimum[component] = std::min(bounds.minimum[component], position.value[component]);
			bounds.maximum[component] = std::max(bounds.maximum[component], position.value[component]);
		}
	}

	Bounds ComputeWorldBounds(const WorldMapMoverGeometry& geometry)
	{
		Bounds bounds;
		for (const auto& surface : geometry.surfaces)
		{
			for (const auto& vertex : surface.vertices) Include(bounds, vertex.position);
		}
		return bounds;
	}

	Bounds ComputeReconstructedBounds(
		const CanonicalLocalMapMoverGeometry& geometry,
		const MapMoverLocalToWorldTransform& transform)
	{
		Bounds bounds;
		for (const auto& surface : geometry.surfaces)
		{
			for (const auto& vertex : surface.vertices)
			{
				Include(bounds, TransformMapMoverPosition(transform, vertex.position));
			}
		}
		return bounds;
	}

	double ComputeBoundsError(const Bounds& expected, const Bounds& actual)
	{
		if (!expected.valid || !actual.valid) return INFINITY;
		double error = 0.0;
		for (int component = 0; component < 3; ++component)
		{
			error = std::max(error, std::abs(expected.minimum[component] - actual.minimum[component]));
			error = std::max(error, std::abs(expected.maximum[component] - actual.maximum[component]));
		}
		return error;
	}

	void CountGeometry(
		const WorldMapMoverGeometry& geometry,
		uint32_t& outSurfaceCount,
		uint32_t& outVertexCount,
		uint32_t& outTriangleCount)
	{
		outSurfaceCount = (uint32_t)geometry.surfaces.size();
		outVertexCount = 0;
		outTriangleCount = 0;
		for (const auto& surface : geometry.surfaces)
		{
			outVertexCount += (uint32_t)surface.vertices.size();
			outTriangleCount += (uint32_t)surface.triangles.size();
		}
	}

	bool IsRigid(MapMoverGeometryClassification classification)
	{
		return classification == MapMoverGeometryClassification::RigidTranslation ||
			classification == MapMoverGeometryClassification::RigidTransform;
	}

	bool RequiresCanonicalReplacement(const MapMoverGeometryComparison& comparison)
	{
		return comparison.membershipChanged || comparison.topologyChanged || comparison.materialSlotChanged;
	}

	bool RememberResourceKey(NRIMapMoverShadowRecord& record, uint64_t resourceKey)
	{
		for (uint32_t i = 0; i < record.rememberedResourceKeyCount; ++i)
		{
			if (record.rememberedResourceKeys[i] == resourceKey) return true;
		}
		if (record.rememberedResourceKeyCount < record.rememberedResourceKeys.size())
		{
			record.rememberedResourceKeys[record.rememberedResourceKeyCount++] = resourceKey;
		}
		else
		{
			record.resourceKeyOverflowCount++;
		}
		return false;
	}

	uint64_t EstimateCanonicalBytes(const CanonicalLocalMapMoverGeometry& geometry)
	{
		uint64_t bytes = sizeof(geometry);
		bytes += geometry.surfaces.capacity() * sizeof(CanonicalLocalMapMoverSurface);
		for (const auto& surface : geometry.surfaces)
		{
			bytes += surface.vertices.capacity() * sizeof(CanonicalLocalMapMoverVertex);
			bytes += surface.triangles.capacity() * sizeof(CanonicalLocalMapMoverTriangle);
		}
		return bytes;
	}

	bool GenerationsRegressed(
		const NRIMapMoverShadowGenerations& current,
		const NRIMapMoverShadowGenerations& previous)
	{
		return current.topology < previous.topology ||
			current.geometry < previous.geometry ||
			current.material < previous.material ||
			current.transform < previous.transform ||
			current.visibility < previous.visibility ||
			current.light < previous.light;
	}

	void AccumulateGenerationMaximum(
		NRIMapMoverShadowGenerations& destination,
		const NRIMapMoverShadowGenerations& source)
	{
		destination.topology = std::max(destination.topology, source.topology);
		destination.geometry = std::max(destination.geometry, source.geometry);
		destination.material = std::max(destination.material, source.material);
		destination.transform = std::max(destination.transform, source.transform);
		destination.visibility = std::max(destination.visibility, source.visibility);
		destination.light = std::max(destination.light, source.light);
	}

	bool AcknowledgeMaterialState(
		CanonicalLocalMapMoverGeometry& canonical,
		const CanonicalLocalMapMoverGeometry& current)
	{
		if (canonical.topologyKey != current.topologyKey ||
			canonical.materialLayoutKey != current.materialLayoutKey ||
			canonical.surfaces.size() != current.surfaces.size())
		{
			return false;
		}
		for (size_t surfaceIndex = 0; surfaceIndex < canonical.surfaces.size(); ++surfaceIndex)
		{
			auto& destination = canonical.surfaces[surfaceIndex];
			const auto& source = current.surfaces[surfaceIndex];
			if (!(destination.provenance == source.provenance) ||
				destination.materialSlotKey != source.materialSlotKey)
			{
				return false;
			}
		}
		for (size_t surfaceIndex = 0; surfaceIndex < canonical.surfaces.size(); ++surfaceIndex)
		{
			canonical.surfaces[surfaceIndex].materialStateKey = current.surfaces[surfaceIndex].materialStateKey;
		}
		canonical.materialStateKey = current.materialStateKey;
		return true;
	}
}

void NRIMapMoverShadowState::Synchronize(uint64_t buildSerial, uint64_t mapEpoch)
{
	if (m_initialized && m_stats.buildSerial == buildSerial && m_stats.mapEpoch == mapEpoch) return;
	const uint64_t resetCount = m_stats.resetCount + 1;
	m_records.clear();
	m_stats = {};
	m_stats.buildSerial = buildSerial;
	m_stats.mapEpoch = mapEpoch;
	m_stats.resetCount = resetCount;
	m_initialized = true;
}

void NRIMapMoverShadowState::Reset()
{
	m_records.clear();
	m_stats = {};
	m_initialized = false;
}

void NRIMapMoverShadowState::RemoveGroup(uint64_t stableGroupId)
{
	for (auto record = m_records.begin(); record != m_records.end();)
	{
		if (record->first.stableGroupId == stableGroupId) record = m_records.erase(record);
		else ++record;
	}
}

void NRIMapMoverShadowState::ReconcileGroup(
	uint64_t stableGroupId,
	const std::vector<uint32_t>& retainedChunkIndices)
{
	for (auto record = m_records.begin(); record != m_records.end();)
	{
		const bool retained = record->first.stableGroupId != stableGroupId ||
			std::find(retainedChunkIndices.begin(), retainedChunkIndices.end(), record->first.chunkIndex) !=
				retainedChunkIndices.end();
		if (!retained) record = m_records.erase(record);
		else ++record;
	}
}

void NRIMapMoverShadowState::NoteFailure(const NRIMapMoverShadowObservation& observation)
{
	m_stats.observations++;
	m_stats.observationFailures++;
	if (!m_initialized || observation.key.stableGroupId == 0 ||
		observation.key.chunkIndex == UINT32_MAX)
	{
		return;
	}

	auto inserted = m_records.emplace(observation.key, NRIMapMoverShadowRecord{});
	NRIMapMoverShadowRecord& record = inserted.first->second;
	if (inserted.second)
	{
		record.key = observation.key;
		record.sectorIndex = observation.sectorIndex;
		record.memberFlags = observation.memberFlags;
		record.generations = observation.generations;
		record.visibilitySignature = observation.visibilitySignature;
		record.previousTransform = observation.previousTransform;
		record.currentTransform = observation.currentTransform;
	}
	else if (!record.canonical.valid)
	{
		AccumulateGenerationMaximum(record.generations, observation.generations);
		record.sectorIndex = observation.sectorIndex;
		record.memberFlags = observation.memberFlags;
		record.visibilitySignature = observation.visibilitySignature;
		record.previousTransform = observation.previousTransform;
		record.currentTransform = observation.currentTransform;
	}
	record.quarantineMask |= observation.quarantineMask;
	record.observationCount++;
}

bool NRIMapMoverShadowState::Observe(
	const NRIMapMoverShadowObservation& observation,
	NRIMapMoverShadowObservationResult& outResult)
{
	outResult = {};
	m_stats.observations++;
	if (!m_initialized || observation.key.stableGroupId == 0 ||
		observation.key.chunkIndex == UINT32_MAX || observation.worldGeometry == nullptr)
	{
		outResult.validation.failure = nri_scene::MapMoverGeometryFailure::EmptyGeometry;
		m_stats.observationFailures++;
		return false;
	}

	CountGeometry(*observation.worldGeometry, outResult.surfaceCount, outResult.vertexCount, outResult.triangleCount);
	auto found = m_records.find(observation.key);
	if (found != m_records.end() && GenerationsRegressed(observation.generations, found->second.generations))
	{
		outResult.generationRegression = true;
		outResult.authorityGenerationMismatch = true;
		outResult.previousResourceKey = found->second.canonical.resourceKey;
		outResult.currentResourceKey = found->second.canonical.resourceKey;
		outResult.resourceKeyStable = true;
		found->second.quarantineMask |= observation.quarantineMask |
			NRIMapMoverShadowQuarantine_AuthorityGenerationMismatch;
		found->second.observationCount++;
		m_stats.observationFailures++;
		return false;
	}

	if (found == m_records.end() || !found->second.canonical.valid)
	{
		NRIMapMoverShadowRecord record = found == m_records.end()
			? NRIMapMoverShadowRecord{}
			: found->second;
		record.key = observation.key;
		if (!nri_scene::BuildCanonicalLocalMapMoverGeometry(
			*observation.worldGeometry,
			observation.currentTransform,
			record.canonical,
			outResult.validation))
		{
			m_stats.observationFailures++;
			return false;
		}
		record.sectorIndex = observation.sectorIndex;
		record.memberFlags = observation.memberFlags;
		record.quarantineMask = observation.quarantineMask;
		record.generations = observation.generations;
		record.visibilitySignature = observation.visibilitySignature;
		record.previousTransform = observation.previousTransform;
		record.currentTransform = observation.currentTransform;
		record.observationCount++;
		RememberResourceKey(record, record.canonical.resourceKey);
		outResult.createdCanonical = true;
		outResult.currentResourceKey = record.canonical.resourceKey;
		outResult.resourceKeyStable = true;
		const Bounds worldBounds = ComputeWorldBounds(*observation.worldGeometry);
		const Bounds reconstructedBounds = ComputeReconstructedBounds(record.canonical, observation.currentTransform);
		outResult.reconstructedBoundsMaxError = ComputeBoundsError(worldBounds, reconstructedBounds);
		if (found == m_records.end()) m_records.emplace(observation.key, std::move(record));
		else found->second = std::move(record);
		m_stats.canonicalCreates++;
		outResult.valid = true;
		return true;
	}

	NRIMapMoverShadowRecord& record = found->second;
	const NRIMapMoverShadowGenerations previousGenerations = record.generations;
	outResult.previousResourceKey = record.canonical.resourceKey;
	outResult.comparison = nri_scene::ClassifyMapMoverGeometryChange(
		record.canonical,
		*observation.worldGeometry,
		observation.currentTransform);
	outResult.validation = outResult.comparison.validation;
	if (!outResult.comparison.validation.valid)
	{
		record.lastComparison = outResult.comparison;
		record.quarantineMask |= observation.quarantineMask;
		m_stats.observationFailures++;
		m_stats.unknownObservations++;
		return false;
	}

	outResult.authorityGenerationMismatch =
		((outResult.comparison.membershipChanged || outResult.comparison.topologyChanged) &&
			observation.generations.topology <= previousGenerations.topology) ||
		(outResult.comparison.materialSlotChanged &&
			observation.generations.material <= previousGenerations.material) ||
		((!outResult.comparison.membershipChanged && !outResult.comparison.topologyChanged &&
			(!outResult.comparison.rigidFitWithinTolerance || outResult.comparison.vertexAttributeChanged)) &&
			observation.generations.geometry <= previousGenerations.geometry) ||
		(outResult.comparison.materialStateChanged &&
			observation.generations.material <= previousGenerations.material &&
			observation.generations.light <= previousGenerations.light);

	if (RequiresCanonicalReplacement(outResult.comparison) && !outResult.authorityGenerationMismatch)
	{
		nri_scene::CanonicalLocalMapMoverGeometry replacement;
		nri_scene::MapMoverGeometryValidation replacementValidation;
		if (!nri_scene::BuildCanonicalLocalMapMoverGeometry(
			*observation.worldGeometry,
			observation.currentTransform,
			replacement,
			replacementValidation))
		{
			outResult.validation = replacementValidation;
			record.lastComparison = outResult.comparison;
			record.quarantineMask |= observation.quarantineMask;
			m_stats.observationFailures++;
			return false;
		}
		const bool seenBefore = RememberResourceKey(record, replacement.resourceKey);
		outResult.abaResourceHit = seenBefore && replacement.resourceKey != record.canonical.resourceKey;
		if (outResult.abaResourceHit)
		{
			record.abaResourceHitCount++;
			m_stats.abaResourceHits++;
		}
		record.canonical = std::move(replacement);
		record.canonicalReplacementCount++;
		outResult.replacedCanonical = true;
		m_stats.canonicalReplacements++;
	}
	else if (outResult.comparison.materialStateChanged && !outResult.authorityGenerationMismatch)
	{
		nri_scene::CanonicalLocalMapMoverGeometry current;
		nri_scene::MapMoverGeometryValidation currentValidation;
		if (!nri_scene::BuildCanonicalLocalMapMoverGeometry(
			*observation.worldGeometry,
			observation.currentTransform,
			current,
			currentValidation) ||
			!AcknowledgeMaterialState(record.canonical, current))
		{
			outResult.validation = currentValidation;
			record.lastComparison = outResult.comparison;
			record.quarantineMask |= observation.quarantineMask;
			m_stats.observationFailures++;
			return false;
		}
		outResult.materialStateAcknowledged = true;
	}

	record.sectorIndex = observation.sectorIndex;
	record.memberFlags = observation.memberFlags;
	uint32_t currentQuarantine = observation.quarantineMask;
	if (outResult.authorityGenerationMismatch)
	{
		currentQuarantine |= NRIMapMoverShadowQuarantine_AuthorityGenerationMismatch;
	}
	else
	{
		record.generations = observation.generations;
		record.visibilitySignature = observation.visibilitySignature;
		record.previousTransform = observation.previousTransform;
		record.currentTransform = observation.currentTransform;
	}
	record.observationCount++;
	record.lastComparison = outResult.comparison;
	if (IsRigid(outResult.comparison.classification))
	{
		record.consecutiveRigidCount++;
		m_stats.rigidObservations++;
	}
	else
	{
		record.consecutiveRigidCount = 0;
		if (outResult.comparison.classification == nri_scene::MapMoverGeometryClassification::StableLayoutDeformer)
			m_stats.deformerObservations++;
		else if (outResult.comparison.topologyChanged || outResult.comparison.membershipChanged)
			m_stats.topologyObservations++;
		else if (outResult.comparison.materialSlotChanged || outResult.comparison.materialStateChanged)
			m_stats.materialObservations++;
		else
			m_stats.unknownObservations++;
	}

	outResult.currentResourceKey = record.canonical.resourceKey;
	outResult.resourceKeyStable = outResult.previousResourceKey == outResult.currentResourceKey;
	const Bounds worldBounds = ComputeWorldBounds(*observation.worldGeometry);
	const Bounds reconstructedBounds = ComputeReconstructedBounds(record.canonical, observation.currentTransform);
	outResult.reconstructedBoundsMaxError = ComputeBoundsError(worldBounds, reconstructedBounds);
	outResult.rigidContractMismatch = observation.declaredRigid &&
		(!outResult.comparison.rigidFitWithinTolerance ||
			outResult.comparison.membershipChanged ||
			outResult.comparison.topologyChanged ||
			outResult.comparison.vertexAttributeChanged);
	if (outResult.rigidContractMismatch)
	{
		currentQuarantine |= NRIMapMoverShadowQuarantine_RigidContractMismatch;
	}
	if (observation.declaredRigid && outResult.reconstructedBoundsMaxError > 1.0e-4)
	{
		currentQuarantine |= NRIMapMoverShadowQuarantine_ReconstructedBoundsMismatch;
	}
	record.quarantineMask = currentQuarantine;
	outResult.valid = true;
	return true;
}

const NRIMapMoverShadowRecord* NRIMapMoverShadowState::Find(uint64_t stableGroupId, uint32_t chunkIndex) const
{
	const auto found = m_records.find({ stableGroupId, chunkIndex });
	return found == m_records.end() ? nullptr : &found->second;
}

uint64_t NRIMapMoverShadowState::EstimateRetainedCpuBytes() const
{
	uint64_t bytes = sizeof(*this) + m_records.size() * sizeof(RecordMap::value_type);
	for (const auto& pair : m_records) bytes += EstimateCanonicalBytes(pair.second.canonical);
	return bytes;
}
