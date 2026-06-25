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
	uint64_t admitMaxBytesLoading = 0;
	uint64_t admitMaxBytesRuntime = 0;
	uint32_t admitMaxMsLoading = 0;
	uint32_t admitMaxMsRuntime = 0;
	uint32_t admitMaxBlasLoading = 0;
	uint32_t admitMaxBlasRuntime = 0;
	uint32_t admitMaxBlasPrimitives = 0;
	int32_t admitIsolateBlasPrimitives = 0;
	uint64_t residentMaxBytes = 0;
	uint64_t residentMinHeadroomBytes = 0;
	uint32_t residentMaxColdMaps = 0;
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

NRITraceSettings BuildNRITraceSettingsFromCVars();
NRIDenoiserSettings BuildNRIDenoiserSettingsFromCVars();
NRIPersistentVoxelSettings BuildNRIPersistentVoxelSettingsFromCVars();
NRIRuntimeMutationSettings BuildNRIRuntimeMutationSettingsFromCVars();
