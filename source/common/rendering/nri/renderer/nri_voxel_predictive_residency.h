#pragma once

#include <cstdint>
#include <vector>

enum class NRIVoxelPredictionTier : uint8_t
{
	Required = 0,
	LocalPlayerReflection,
	NearbyCurrentActor,
	CurrentActor,
	MapAuthored,
	InitialAnimation,
	CommonAuthored,
	RareOptional,
	ExcludedDynamic,
};

struct NRIVoxelPredictiveResidencySettings
{
	bool enabled = true;
	bool strict = false;
	float nearbyDistance = 8192.0f;
	uint32_t maxBindings = 64;
	uint32_t maxUniqueMeshes = 16;
	uint32_t maxMaterialsPerMesh = 4;
	uint64_t maxUniqueGeometryBytes = 128ull * 1024ull * 1024ull;
};

struct NRIVoxelPredictiveCandidate
{
	uint64_t meshKey = 0;
	uint64_t materialKey = 0;
	uint64_t stableKey = 0;
	uint64_t estimatedGeometryBytes = 0;
	int32_t priority = 0;
	int32_t admissionRank = 0;
	float actorDistanceSquared = 0.0f;
	bool required = false;
	bool localPlayer = false;
	bool actorOwned = false;
	bool currentActor = false;
	bool initialAnimation = false;
	bool mapAuthored = false;
	bool commonAuthored = false;
	bool dynamicMaterial = false;
	bool sourceReady = false;
	bool materialReady = false;
};

struct NRIVoxelPredictiveSelection
{
	std::vector<uint8_t> tiers;
	std::vector<uint8_t> geometrySelected;
	uint64_t digest = 1469598103934665603ull;
	uint64_t selectedGeometryBytes = 0;
	uint32_t candidates = 0;
	uint32_t selectedBindings = 0;
	uint32_t selectedUniqueMeshes = 0;
	uint32_t selectedRequired = 0;
	uint32_t selectedPredicted = 0;
	uint32_t excludedDynamic = 0;
	uint32_t capSkippedBindings = 0;
	uint32_t capSkippedMeshes = 0;
	uint32_t capSkippedMaterials = 0;
	uint32_t capSkippedBytes = 0;
};

NRIVoxelPredictionTier ClassifyNRIVoxelPredictiveCandidate(
	const NRIVoxelPredictiveCandidate& candidate,
	const NRIVoxelPredictiveResidencySettings& settings);

NRIVoxelPredictiveSelection SelectNRIVoxelPredictiveResidency(
	const std::vector<NRIVoxelPredictiveCandidate>& candidates,
	const NRIVoxelPredictiveResidencySettings& settings);

const char* NRIVoxelPredictionTierName(NRIVoxelPredictionTier tier);
