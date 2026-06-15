#include "nri_renderer_settings.h"
#include "nri_cvars.h"

#include "c_cvars.h"

#include <algorithm>



namespace
{
	uint32_t ClampTraceBounceCount(int value, uint32_t maxValue)
	{
		return (uint32_t)std::max(0, std::min(value, (int)maxValue));
	}

	uint32_t ClampNrdHistoryFrameCount(int value)
	{
		return (uint32_t)std::clamp(value, 0, (int)nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
	}

	uint32_t ClampNrdFastFrameCount(int value, uint32_t maxAccumulatedFrameNum)
	{
		return (uint32_t)std::clamp(value, 0, (int)maxAccumulatedFrameNum);
	}

	uint32_t ClampNrdStabilizationFrameCount(int value, uint32_t maxAccumulatedFrameNum)
	{
		return (uint32_t)std::clamp(value, 0, (int)maxAccumulatedFrameNum);
	}

	uint32_t ClampSigmaStabilizationFrameCount(int value)
	{
		return (uint32_t)std::clamp(value, 0, (int)nrd::SIGMA_MAX_HISTORY_FRAME_NUM);
	}

	float ClampNrdFastHistorySigmaScale(float value)
	{
		return std::clamp(value, 1.0f, 3.0f);
	}

	float ClampNrdPrepassBlurRadius(float value)
	{
		return std::clamp(value, 0.0f, 75.0f);
	}

	float ClampNrdBlurRadius(float value)
	{
		return std::clamp(value, 0.0f, 60.0f);
	}

	float ClampSigmaPlaneDistanceSensitivity(float value)
	{
		return std::clamp(value, 0.001f, 0.1f);
	}
}

NRITraceSettings BuildNRITraceSettingsFromCVars()
{
	NRITraceSettings settings = {};
	settings.lightBounceCount = ClampTraceBounceCount((int)nri_ptlightbounces, 4u);
	settings.mirrorBounceCount = ClampTraceBounceCount((int)nri_ptmirrorbounces, 8u);
	settings.portalDepth = ClampTraceBounceCount((int)nri_ptportaldepth, 8u);
	settings.emissiveSampleCount = std::max<uint32_t>(ClampTraceBounceCount((int)nri_ptemissivesamples, 4u), 1u);
	return settings;
}

NRIDenoiserSettings BuildNRIDenoiserSettingsFromCVars()
{
	NRIDenoiserSettings settings = {};
	settings.denoiserMode = (NRINrdDenoiserMode)std::clamp((int)nri_nrddenoiser, 0, 1);
	settings.maxAccumulatedFrameNum = ClampNrdHistoryFrameCount((int)nri_nrdmaxframes);
	settings.maxFastAccumulatedFrameNum = ClampNrdFastFrameCount((int)nri_nrdfastframes, settings.maxAccumulatedFrameNum);
	settings.maxStabilizedFrameNum = ClampNrdStabilizationFrameCount((int)nri_nrdstabilizationframes, settings.maxAccumulatedFrameNum);
	settings.sigmaMaxStabilizedFrameNum = ClampSigmaStabilizationFrameCount((int)nri_nrdsigmastabilization);
	settings.hitDistanceReconstructionMode = (uint32_t)std::clamp((int)nri_nrdhitdistrecon, 0, 2);
	settings.inputSplitMode = (uint32_t)std::clamp((int)nri_nrdsplit, 0, 2);
	settings.fastHistoryClampingSigmaScale = ClampNrdFastHistorySigmaScale((float)nri_nrdfasthistorysigma);
	settings.diffusePrepassBlurRadius = ClampNrdPrepassBlurRadius((float)nri_nrdprepassdiffuse);
	settings.specularPrepassBlurRadius = ClampNrdPrepassBlurRadius((float)nri_nrdprepassspecular);
	settings.minBlurRadius = ClampNrdBlurRadius((float)nri_nrdblurmin);
	settings.maxBlurRadius = std::max(settings.minBlurRadius, ClampNrdBlurRadius((float)nri_nrdblurmax));
	settings.sigmaPlaneDistanceSensitivity = ClampSigmaPlaneDistanceSensitivity((float)nri_nrdsigmaplanedistance);
	settings.enableAntiFirefly = nri_nrdantifirefly;
	settings.enableValidation = nri_validation;
	return settings;
}

NRIPersistentVoxelSettings BuildNRIPersistentVoxelSettingsFromCVars()
{
	NRIPersistentVoxelSettings settings = {};
	settings.buildActors = (uint32_t)std::max(1, (int)nri_ptpersistentvoxelbuildactors);
	settings.buildPrimitives = (int)nri_ptpersistentvoxelbuildprims <= 0 ? 0u : (uint32_t)(int)nri_ptpersistentvoxelbuildprims;
	settings.buildBytes = (int)nri_ptpersistentvoxelbuildbytes <= 0 ? 0ull : (uint64_t)(int)nri_ptpersistentvoxelbuildbytes;
	settings.texturePrewarms = (int)nri_ptpersistentvoxeltextureprewarms <= 0 ? 0u : (uint32_t)(int)nri_ptpersistentvoxeltextureprewarms;
	settings.textureBytes = (int)nri_ptpersistentvoxeltexturebytes <= 0 ? 0ull : (uint64_t)(int)nri_ptpersistentvoxeltexturebytes;
	settings.runtimeBudgetMode = (uint32_t)std::clamp((int)nri_ptvoxelruntimebudget, 0, 4);
	settings.admissionLoadVariants = (int)nri_ptvoxeladmissionloadvariants <= 0 ? UINT32_MAX : (uint32_t)(int)nri_ptvoxeladmissionloadvariants;
	settings.admissionLoadBytes = (int)nri_ptvoxeladmissionloadbytes <= 0 ? 0ull : (uint64_t)(int)nri_ptvoxeladmissionloadbytes;
	settings.admissionRuntimeVariants = (int)nri_ptvoxeladmissionruntimevariants <= 0 ? UINT32_MAX : (uint32_t)(int)nri_ptvoxeladmissionruntimevariants;
	settings.admissionRuntimeBytes = (int)nri_ptvoxeladmissionruntimebytes <= 0 ? 0ull : (uint64_t)(int)nri_ptvoxeladmissionruntimebytes;
	settings.admitMaxBytesLoading = (int)nri_ptvoxeladmitmaxbytesloading <= 0 ? 0ull : (uint64_t)(int)nri_ptvoxeladmitmaxbytesloading;
	settings.admitMaxBytesRuntime = (int)nri_ptvoxeladmitmaxbytesruntime <= 0 ? 0ull : (uint64_t)(int)nri_ptvoxeladmitmaxbytesruntime;
	settings.admitMaxMsLoading = (uint32_t)std::max(0, (int)nri_ptvoxeladmitmaxmsloading);
	settings.admitMaxMsRuntime = (uint32_t)std::max(0, (int)nri_ptvoxeladmitmaxmsruntime);
	settings.admitMaxBlasLoading = (int)nri_ptvoxeladmitmaxblasloading <= 0 ? UINT32_MAX : (uint32_t)(int)nri_ptvoxeladmitmaxblasloading;
	settings.admitMaxBlasRuntime = (int)nri_ptvoxeladmitmaxblasruntime <= 0 ? UINT32_MAX : (uint32_t)(int)nri_ptvoxeladmitmaxblasruntime;
	settings.admitMaxBlasPrimitives = (int)nri_ptvoxeladmitmaxblasprims <= 0 ? UINT32_MAX : (uint32_t)(int)nri_ptvoxeladmitmaxblasprims;
	settings.admitIsolateBlasPrimitives = (int)nri_ptvoxeladmitisolateblasprims;
	settings.residentMaxBytes = (int)nri_ptvoxelresidentmaxbytes <= 0 ? 0ull : (uint64_t)(int)nri_ptvoxelresidentmaxbytes;
	settings.residentMinHeadroomBytes = (int)nri_ptvoxelresidentminheadroombytes <= 0 ? 0ull : (uint64_t)(int)nri_ptvoxelresidentminheadroombytes;
	settings.residentMaxColdMaps = (int)nri_ptvoxelresidentmaxcoldmaps < 0 ? UINT32_MAX : (uint32_t)(int)nri_ptvoxelresidentmaxcoldmaps;
	settings.transformKeyed = (bool)nri_ptvoxeltransformkeyed;
	settings.excludeIndices = {
		(int32_t)(int)nri_ptvoxelexcludeindex,
		(int32_t)(int)nri_ptvoxelexcludeindex2,
		(int32_t)(int)nri_ptvoxelexcludeindex3
	};
	settings.excludeMinPrimitives = (uint32_t)std::max(0, (int)nri_ptvoxelexcludeminprims);
	return settings;
}

NRIRuntimeMutationSettings BuildNRIRuntimeMutationSettingsFromCVars()
{
	NRIRuntimeMutationSettings settings = {};
	settings.worklistEnabled = (bool)nri_ptruntimeworklist;
	settings.worklistSweepBudget = (uint32_t)std::max(0, (int)nri_ptruntimeworklistsweepbudget);
	settings.deferFarMaterialRefreshes = (bool)nri_ptruntimedeferfarmaterial;
	settings.deferNearInvisibleMaterialRefreshes = (bool)nri_ptruntimedefernearinvisiblematerial;
	settings.nearInvisibleMaterialBudget = (uint32_t)std::max(0, (int)nri_ptruntimenearinvisiblematerialbudget);
	settings.deferFarStructuralRebuilds = (bool)nri_ptruntimedeferfarstructural;
	settings.farStructuralBudget = (uint32_t)std::max(0, (int)nri_ptruntimefarstructuralbudget);
	settings.deferNearInvisibleStructuralRebuilds = (bool)nri_ptruntimedefernearinvisiblestructural;
	settings.nearInvisibleStructuralBudget = (uint32_t)std::max(0, (int)nri_ptruntimenearinvisiblestructuralbudget);
	settings.nearDistance = std::max(0.0f, (float)nri_ptruntimemutationneardistance);
	return settings;
}