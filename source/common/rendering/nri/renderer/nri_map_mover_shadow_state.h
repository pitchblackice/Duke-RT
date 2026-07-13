#pragma once

#include "../scene/nri_map_mover_geometry.h"

#include <array>
#include <cstdint>
#include <map>
#include <vector>

enum NRIMapMoverShadowQuarantineBits : uint32_t
{
	NRIMapMoverShadowQuarantine_None = 0,
	NRIMapMoverShadowQuarantine_UnknownCapability = 1u << 0,
	NRIMapMoverShadowQuarantine_AdjacencyUnproven = 1u << 1,
	NRIMapMoverShadowQuarantine_OverlappingGeometryOwner = 1u << 2,
	NRIMapMoverShadowQuarantine_PortalSurface = 1u << 3,
	NRIMapMoverShadowQuarantine_AuthoredTopology = 1u << 4,
	NRIMapMoverShadowQuarantine_SurfaceAdapter = 1u << 5,
	NRIMapMoverShadowQuarantine_GeometryValidation = 1u << 6,
	NRIMapMoverShadowQuarantine_TransformValidation = 1u << 7,
	NRIMapMoverShadowQuarantine_AuthorityGenerationMismatch = 1u << 8,
	NRIMapMoverShadowQuarantine_RigidContractMismatch = 1u << 9,
	NRIMapMoverShadowQuarantine_ReconstructedBoundsMismatch = 1u << 10,
};

struct NRIMapMoverShadowRecordKey
{
	uint64_t stableGroupId = 0;
	uint32_t chunkIndex = UINT32_MAX;

	bool operator<(const NRIMapMoverShadowRecordKey& other) const
	{
		return stableGroupId < other.stableGroupId ||
			(stableGroupId == other.stableGroupId && chunkIndex < other.chunkIndex);
	}
};

struct NRIMapMoverShadowGenerations
{
	uint64_t topology = 0;
	uint64_t geometry = 0;
	uint64_t material = 0;
	uint64_t transform = 0;
	uint64_t visibility = 0;
	uint64_t light = 0;
};

struct NRIMapMoverShadowObservation
{
	NRIMapMoverShadowRecordKey key;
	int32_t sectorIndex = -1;
	uint32_t memberFlags = 0;
	uint32_t quarantineMask = 0;
	bool declaredRigid = false;
	NRIMapMoverShadowGenerations generations;
	uint64_t visibilitySignature = 0;
	const nri_scene::WorldMapMoverGeometry* worldGeometry = nullptr;
	nri_scene::MapMoverLocalToWorldTransform previousTransform;
	nri_scene::MapMoverLocalToWorldTransform currentTransform;
};

struct NRIMapMoverShadowObservationResult
{
	bool valid = false;
	bool createdCanonical = false;
	bool replacedCanonical = false;
	bool resourceKeyStable = false;
	bool abaResourceHit = false;
	bool authorityGenerationMismatch = false;
	bool generationRegression = false;
	bool rigidContractMismatch = false;
	bool materialStateAcknowledged = false;
	uint64_t previousResourceKey = 0;
	uint64_t currentResourceKey = 0;
	uint32_t surfaceCount = 0;
	uint32_t vertexCount = 0;
	uint32_t triangleCount = 0;
	double reconstructedBoundsMaxError = 0.0;
	nri_scene::MapMoverGeometryValidation validation;
	nri_scene::MapMoverGeometryComparison comparison;
};

struct NRIMapMoverShadowRecord
{
	static constexpr size_t MaxRememberedResourceKeys = 8;

	NRIMapMoverShadowRecordKey key;
	int32_t sectorIndex = -1;
	uint32_t memberFlags = 0;
	uint32_t quarantineMask = 0;
	NRIMapMoverShadowGenerations generations;
	uint64_t visibilitySignature = 0;
	uint64_t observationCount = 0;
	uint64_t consecutiveRigidCount = 0;
	uint64_t canonicalReplacementCount = 0;
	uint64_t abaResourceHitCount = 0;
	uint32_t rememberedResourceKeyCount = 0;
	uint32_t resourceKeyOverflowCount = 0;
	std::array<uint64_t, MaxRememberedResourceKeys> rememberedResourceKeys = {};
	nri_scene::MapMoverLocalToWorldTransform previousTransform;
	nri_scene::MapMoverLocalToWorldTransform currentTransform;
	nri_scene::CanonicalLocalMapMoverGeometry canonical;
	nri_scene::MapMoverGeometryComparison lastComparison;
};

struct NRIMapMoverShadowStateStats
{
	uint64_t buildSerial = 0;
	uint64_t mapEpoch = 0;
	uint64_t resetCount = 0;
	uint64_t observations = 0;
	uint64_t observationFailures = 0;
	uint64_t canonicalCreates = 0;
	uint64_t canonicalReplacements = 0;
	uint64_t rigidObservations = 0;
	uint64_t deformerObservations = 0;
	uint64_t topologyObservations = 0;
	uint64_t materialObservations = 0;
	uint64_t unknownObservations = 0;
	uint64_t abaResourceHits = 0;
};

class NRIMapMoverShadowState
{
public:
	void Synchronize(uint64_t buildSerial, uint64_t mapEpoch);
	void Reset();
	void RemoveGroup(uint64_t stableGroupId);
	void ReconcileGroup(uint64_t stableGroupId, const std::vector<uint32_t>& retainedChunkIndices);
	void NoteFailure(const NRIMapMoverShadowObservation& observation);
	bool Observe(
		const NRIMapMoverShadowObservation& observation,
		NRIMapMoverShadowObservationResult& outResult);

	const NRIMapMoverShadowRecord* Find(uint64_t stableGroupId, uint32_t chunkIndex) const;
	uint32_t GetRecordCount() const { return (uint32_t)m_records.size(); }
	uint64_t EstimateRetainedCpuBytes() const;
	const NRIMapMoverShadowStateStats& GetStats() const { return m_stats; }

private:
	using RecordMap = std::map<NRIMapMoverShadowRecordKey, NRIMapMoverShadowRecord>;

	RecordMap m_records;
	NRIMapMoverShadowStateStats m_stats;
	bool m_initialized = false;
};
