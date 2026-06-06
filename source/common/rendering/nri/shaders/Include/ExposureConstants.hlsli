#ifndef RAZE_NRI_PT_EXPOSURE_CONSTANTS_HLSLI
#define RAZE_NRI_PT_EXPOSURE_CONSTANTS_HLSLI

#include "NRI.hlsl"

#define SET_EXPOSURE_INPUTS 0
#define SET_EXPOSURE_OUTPUTS 1
#define SET_EXPOSURE_ROOT 2

#define NRI_EXPOSURE_FLAG_FREEZE 0x1u
#define NRI_EXPOSURE_DEBUG_MAGIC 0x45585033u
#define NRI_EXPOSURE_MAX_HISTOGRAM_BINS 256u
#define NRI_EXPOSURE_DEBUG_WORD_COUNT 16u

struct NRIExposureConstants
{
	uint RenderWidth;
	uint RenderHeight;
	uint FrameIndex;
	uint Flags;
	uint HistogramBinCount;
	uint SampleStep;
	uint Reserved0;
	uint Reserved1;
	float LogLuminanceMin;
	float LogLuminanceMax;
	float InvLogLuminanceRange;
	float TargetLuminance;
	float MinExposure;
	float MaxExposure;
	float ExposureBias;
	float LowPercentile;
	float HighPercentile;
	float FallbackManualExposure;
	float AdaptUpSpeed;
	float AdaptDownSpeed;
};

NRI_ROOT_CONSTANTS(NRIExposureConstants, gExposureConstants, 0, SET_EXPOSURE_ROOT);

Texture2D<float4> gExposureSource : register(t0, space0);
Texture2D<float4> gPreviousExposureState : register(t1, space0);

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gCurrentExposureState, u, 0, SET_EXPOSURE_OUTPUTS);
RWStructuredBuffer<uint> gExposureHistogram : register(u1, space1);
RWStructuredBuffer<uint> gExposureDebug : register(u2, space1);

float SanitizeExposureFloat(float value, float fallbackValue)
{
	return (isnan(value) || isinf(value)) ? fallbackValue : value;
}

float3 SanitizeExposureColor(float3 value)
{
	if (any(isnan(value)) || any(isinf(value)))
	{
		return 0.0;
	}

	return max(value, 0.0);
}

float ExposureBinToLogLuminance(uint bin)
{
	const float binCount = max((float)gExposureConstants.HistogramBinCount, 1.0);
	const float normalized = ((float)bin + 0.5) / binCount;
	return lerp(gExposureConstants.LogLuminanceMin, gExposureConstants.LogLuminanceMax, normalized);
}

uint ExposureLogLuminanceToBin(float logLuminance)
{
	const float normalized = saturate((logLuminance - gExposureConstants.LogLuminanceMin) * gExposureConstants.InvLogLuminanceRange);
	const uint binCount = max(gExposureConstants.HistogramBinCount, 1u);
	return min((uint)(normalized * (float)binCount), binCount - 1u);
}

float ExposureLuminance(float3 color)
{
	return dot(color, float3(0.2126, 0.7152, 0.0722));
}

#endif
