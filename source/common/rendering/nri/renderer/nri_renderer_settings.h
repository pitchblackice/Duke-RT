#pragma once

#include "nri_nrd.h"

#include <array>
#include <cstdint>

struct NRITraceSettings
{
	uint32_t lightBounceCount = 4;
	uint32_t mirrorBounceCount = 8;
	uint32_t portalDepth = 8;
	uint32_t emissiveSampleCount = 4;
};

struct NRIDenoiserSettings
{
	NRINrdDenoiserMode denoiserMode = NRINrdDenoiserMode::Reblur;
	uint32_t maxAccumulatedFrameNum = 0;
	uint32_t maxFastAccumulatedFrameNum = 0;
	uint32_t maxStabilizedFrameNum = 0;
	uint32_t sigmaMaxStabilizedFrameNum = 0;
	uint32_t hitDistanceReconstructionMode = 0;
	uint32_t inputSplitMode = 0;
	float fastHistoryClampingSigmaScale = 1.0f;
	float diffusePrepassBlurRadius = 0.0f;
	float specularPrepassBlurRadius = 0.0f;
	float minBlurRadius = 0.0f;
	float maxBlurRadius = 0.0f;
	float sigmaPlaneDistanceSensitivity = 0.001f;
	bool enableAntiFirefly = true;
	bool enableValidation = false;
};

struct NRIPersistentVoxelSettings
{
	uint32_t buildActors = 0;
	uint32_t buildPrimitives = 0;
	uint64_t buildBytes = 0;
	uint32_t texturePrewarms = 0;
	uint64_t textureBytes = 0;
	uint32_t runtimeBudgetMode = 0;
	uint32_t admissionLoadVariants = 0;
	uint64_t admissionLoadBytes = 0;
	uint32_t admissionRuntimeVariants = 0;
	uint64_t admissionRuntimeBytes = 0;
	uint32_t admissionGraceFrames = 0;
	uint32_t admissionGraceVariants = 0;
	uint32_t preloadReadyGraceFrames = 0;
	uint64_t admitMaxBytesLoading = 0;
	uint64_t admitMaxBytesRuntime = 0;
	uint32_t admitMaxMsLoading = 0;
	uint32_t admitMaxMsRuntime = 0;
	uint32_t admitMaxBlasLoading = 0;
	uint32_t admitMaxBlasRuntime = 0;
	uint32_t admitMaxBlasPrimitives = 0;
	int32_t admitIsolateBlasPrimitives = 0;
	uint32_t computeMaxJobs = 0;
	uint64_t residentMaxBytes = 0;
	uint64_t residentMinHeadroomBytes = 0;
	uint32_t residentMaxColdMaps = 0;
	bool trimColdOnLoading = false;
	bool sharedBlasBuildEnabled = false;
	uint32_t sharedBlasBuildsPerFrame = 0;
	bool sharedBlasLoadingWarmupEnabled = false;
	bool sharedBlasRouteEnabled = false;
	bool transformKeyed = false;
	bool diagnosticsEnabled = false;
	std::array<int32_t, 3> excludeIndices = { -1, -1, -1 };
	uint32_t excludeMinPrimitives = 0;
};

struct NRIRuntimeMutationSettings
{
	bool worklistEnabled = true;
	uint32_t worklistSweepBudget = 32;
	bool deferFarMaterialRefreshes = true;
	bool deferNearInvisibleMaterialRefreshes = true;
	uint32_t nearInvisibleMaterialBudget = 4;
	bool deferFarStructuralRebuilds = true;
	uint32_t farStructuralBudget = 2;
	bool deferNearInvisibleStructuralRebuilds = true;
	uint32_t nearInvisibleStructuralBudget = 2;
	float nearDistance = 1024.0f;
};

struct NRISmokeSettings
{
	bool enabled = false;
	bool readback = false;
	uint32_t quality = 1;
	uint32_t particleCapacity = 8192;
	uint32_t froxelPixelSize = 16;
	uint32_t froxelDepth = 48;
	uint32_t columnCapacity = 64;
	uint32_t simulationRate = 60;
	uint32_t maxSubsteps = 4;
	bool pointLights = true;
	bool directionalLight = true;
	bool emissiveLights = true;
	uint32_t emissiveReuseMode = 2;
	bool emissiveReference = false;
	uint32_t emissiveBackend = 0;
	bool emissiveWorldFilter = false;
	uint32_t emissiveWorldDebug = 0;
	bool emissiveLegacyGatherDisabled = false;
	bool emissiveQuarterKey = false;
	float emissiveSourceClamp = 32.0f;
	uint32_t directReuseMode = 2;
	uint32_t directReferenceMode = 0;
	bool volumeHistory = true;
	uint32_t dlrrMode = 1;
	bool indirect = true;
	uint32_t indirectCacheMode = 3;
	uint32_t lightMode = 1;
	uint32_t lightSamples = 1;
	uint32_t maxLightCandidates = 8;
	bool filteredVisibility = true;
	uint32_t debugMode = 0;
	uint32_t traceMode = 0;
	float froxelMaxDistance = 4096.0f;
	float timeScale = 1.0f;
	float wind[3] = {};
	float densityScale = 1.0f;
	float radianceScale = 1.0f;
	float indirectScale = 1.0f;
	uint32_t representation = 0;
	uint32_t gridBrickCapacity = 512;
	float gridCellSize = 8.0f;
	float gridBuoyancy = 1.0f;
	float gridVelocityDamping = 0.15f;
	float gridWindCoupling = 0.5f;
	float gridDensityHalfLifeScale = 1.0f;
	float gridCoolingScale = 1.0f;
	float gridMaxVelocity = 128.0f;
	float gridMaxBacktrace = 32.0f;
	float gridActiveThreshold = 0.0001f;
	uint32_t gridReclaimGrace = 120;
};

NRITraceSettings BuildNRITraceSettingsFromCVars();
NRIDenoiserSettings BuildNRIDenoiserSettingsFromCVars();
NRIPersistentVoxelSettings BuildNRIPersistentVoxelSettingsFromCVars();
NRIRuntimeMutationSettings BuildNRIRuntimeMutationSettingsFromCVars();
NRISmokeSettings BuildNRISmokeSettingsFromCVars();
