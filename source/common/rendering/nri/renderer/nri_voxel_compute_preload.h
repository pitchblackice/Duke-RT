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
	uint32_t rawVariants = 0;
	uint32_t rawRequired = 0;
	uint32_t rawOptional = 0;
	uint32_t rawSelected = 0;
	uint32_t rawSelectedRequired = 0;
	uint32_t rawSelectedOptional = 0;
	uint32_t rawUniqueMeshes = 0;
	uint32_t rawUniqueMaterials = 0;
	uint32_t rawSourceResident = 0;
	uint32_t rawSourceMissing = 0;
	uint32_t rawMaterialContextReady = 0;
	uint32_t rawMaterialContextMissing = 0;
	uint32_t rawCpuSurfaceReady = 0;
	uint32_t rawLegacyGpuCandidate = 0;
	uint32_t rawLegacyGpuSourceSkipped = 0;
	uint32_t rawSkippedDisabled = 0;
	uint32_t rawSkippedRequiredOff = 0;
	uint32_t rawSkippedOptionalOff = 0;
	uint32_t rawSkippedSourceMissing = 0;
	uint32_t rawSkippedMaterialMissing = 0;
	uint32_t rawSkippedByteBudget = 0;
	uint32_t rawSkippedJobBudget = 0;
	uint64_t rawEstimatedGeometryBytes = 0;
	uint64_t rawSelectedGeometryBytes = 0;
	uint32_t manifestSources = 0;
	uint32_t manifestLines = 0;
	uint32_t manifestRequests = 0;
	uint32_t manifestSkippedInactive = 0;
	uint32_t manifestSkippedSyntax = 0;
	uint32_t manifestSkippedActor = 0;
	uint32_t manifestSkippedUnsupported = 0;
	uint32_t manifestDiscovered = 0;
	uint32_t manifestUniqueRequests = 0;
	uint32_t manifestSkippedInvalid = 0;
	uint32_t manifestSkippedDuplicate = 0;
};

NRIVoxelComputePreloadSettings BuildNRIVoxelComputePreloadSettingsFromCVars();

void BuildNRIVoxelComputePreloadDirectVariants(
	const std::vector<nri_scene::PrecachedVoxelRawManifestView>& rawVariants,
	const NRIVoxelComputePreloadSettings& settings,
	std::vector<nri_scene::PrecachedVoxelVariantView>& outVariants);

NRIVoxelComputePreloadStats PlanNRIVoxelComputePreload(
	const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
	const std::vector<nri_scene::PrecachedVoxelRawManifestView>& rawVariants,
	const nri_scene::PrecachedVoxelRawManifestStats& rawManifestStats,
	const NRIPersistentVoxelResidency& residency,
	const NRIVoxelComputePreloadSettings& settings,
	const char* levelName,
	uint64_t buildSerial,
	uint32_t frameIndex,
	const char* timelineStage);
