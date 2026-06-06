#include "Include/ExposureConstants.hlsli"

groupshared uint gLocalHistogram[NRI_EXPOSURE_MAX_HISTOGRAM_BINS];

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID, uint groupThreadIndex : SV_GroupIndex)
{
	if (groupThreadIndex < NRI_EXPOSURE_MAX_HISTOGRAM_BINS)
	{
		gLocalHistogram[groupThreadIndex] = 0u;
	}
	GroupMemoryBarrierWithGroupSync();

	const uint sampleStep = max(gExposureConstants.SampleStep, 1u);
	const uint2 pixelPos = dispatchThreadId.xy * sampleStep;
	if (pixelPos.x < gExposureConstants.RenderWidth && pixelPos.y < gExposureConstants.RenderHeight)
	{
		const float3 color = SanitizeExposureColor(gExposureSource.Load(int3(pixelPos, 0)).rgb);
		const float luminance = max(ExposureLuminance(color), 1.0e-6);
		const float logLuminance = log2(luminance);
		const uint bin = ExposureLogLuminanceToBin(logLuminance);
		InterlockedAdd(gLocalHistogram[bin], 1u);
		InterlockedAdd(gExposureDebug[2], 1u);
	}

	GroupMemoryBarrierWithGroupSync();
	if (groupThreadIndex < gExposureConstants.HistogramBinCount)
	{
		const uint count = gLocalHistogram[groupThreadIndex];
		if (count != 0u)
		{
			InterlockedAdd(gExposureHistogram[groupThreadIndex], count);
		}
	}
}
