#include "nri_renderer_settings.h"

#include "c_cvars.h"

#include <algorithm>

EXTERN_CVAR(Int, nri_nrddenoiser)
EXTERN_CVAR(Int, nri_nrdmaxframes)
EXTERN_CVAR(Int, nri_nrdfastframes)
EXTERN_CVAR(Int, nri_nrdstabilizationframes)
EXTERN_CVAR(Bool, nri_nrdantifirefly)
EXTERN_CVAR(Int, nri_nrdhitdistrecon)
EXTERN_CVAR(Int, nri_nrdsplit)
EXTERN_CVAR(Float, nri_nrdfasthistorysigma)
EXTERN_CVAR(Float, nri_nrdprepassdiffuse)
EXTERN_CVAR(Float, nri_nrdprepassspecular)
EXTERN_CVAR(Float, nri_nrdblurmin)
EXTERN_CVAR(Float, nri_nrdblurmax)
EXTERN_CVAR(Int, nri_nrdsigmastabilization)
EXTERN_CVAR(Float, nri_nrdsigmaplanedistance)
EXTERN_CVAR(Bool, nri_validation)
EXTERN_CVAR(Int, nri_ptlightbounces)
EXTERN_CVAR(Int, nri_ptmirrorbounces)
EXTERN_CVAR(Int, nri_ptportaldepth)
EXTERN_CVAR(Int, nri_ptemissivesamples)

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
