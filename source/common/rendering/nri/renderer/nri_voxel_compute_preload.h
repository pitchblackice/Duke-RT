#pragma once

#include "../scene/nri_scene_bridge.h"

#include <cstdint>
#include <vector>

class NRIPersistentVoxelResidency;

struct NRIVoxelComputePreloadSettings
{
	bool enabled = false;
	bool dryRun = true;
	int traceLevel = 0;
	bool includeRequired = true;
	bool includeOptional = false;
	bool preloadMaterials = false;
	uint32_t maxMilliseconds = 0;
	uint32_t maxJobs = 0;
	uint32_t maxBlasBuilds = 0;
	uint64_t maxBytes = 0;
	uint32_t maxMaterialRows = 0;
};

struct NRIVoxelComputePreloadStats
{
	bool emitted = false;
	bool enabled = false;
	bool dryRun = true;
	bool actionReady = false;
	uint32_t variants = 0;
	uint32_t required = 0;
	uint32_t optional = 0;
	uint32_t selected = 0;
	uint32_t selectedRequired = 0;
	uint32_t selectedOptional = 0;
	uint32_t skippedDisabled = 0;
	uint32_t skippedRequiredOff = 0;
	uint32_t skippedOptionalOff = 0;
	uint32_t skippedByteBudget = 0;
	uint32_t skippedJobBudget = 0;
	uint32_t skippedMaterialBudget = 0;
	uint32_t uniqueMeshes = 0;
	uint32_t uniqueMaterials = 0;
	uint32_t directOnly = 0;
	uint32_t surfaceReady = 0;
	uint32_t sourceReady = 0;
	uint32_t materialContextReady = 0;
	uint32_t meshResident = 0;
	uint32_t materialResident = 0;
	uint32_t blasReady = 0;
	uint32_t ready = 0;
	uint32_t notReady = 0;
	uint32_t materialRowsPlanned = 0;
	uint64_t estimatedGeometryBytes = 0;
	uint64_t selectedGeometryBytes = 0;
	uint64_t residentSceneBytes = 0;
	uint64_t residentAsBytes = 0;
	double planMs = 0.0;
};

NRIVoxelComputePreloadSettings BuildNRIVoxelComputePreloadSettingsFromCVars();

NRIVoxelComputePreloadStats PlanNRIVoxelComputePreload(
	const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
	const NRIPersistentVoxelResidency& residency,
	const NRIVoxelComputePreloadSettings& settings,
	const char* levelName,
	uint64_t buildSerial,
	uint32_t frameIndex);
