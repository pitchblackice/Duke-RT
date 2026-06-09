#pragma once

#include "nri_nrd.h"

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

NRITraceSettings BuildNRITraceSettingsFromCVars();
NRIDenoiserSettings BuildNRIDenoiserSettingsFromCVars();
