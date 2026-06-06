#include "Include/ExposureConstants.hlsli"

groupshared uint gLocalHistogram[NRI_EXPOSURE_MAX_HISTOGRAM_BINS];

uint ResolveMeteringWeight(uint2 pixelPos)
{
	if (gExposureConstants.MeteringMode != NRI_EXPOSURE_METERING_CENTER_WEIGHTED)
	{
		return 1u;
	}

	const uint2 center = uint2(gExposureConstants.RenderWidth, gExposureConstants.RenderHeight) / 2u;
	const uint2 delta = uint2(
		pixelPos.x > center.x ? pixelPos.x - center.x : center.x - pixelPos.x,
		pixelPos.y > center.y ? pixelPos.y - center.y : center.y - pixelPos.y);
	const uint2 quarter = uint2(max(gExposureConstants.RenderWidth / 4u, 1u), max(gExposureConstants.RenderHeight / 4u, 1u));
	const uint2 threeEighths = uint2(max((gExposureConstants.RenderWidth * 3u) / 8u, 1u), max((gExposureConstants.RenderHeight * 3u) / 8u, 1u));
	if (delta.x <= quarter.x && delta.y <= quarter.y)
	{
		return 4u;
	}
	if (delta.x <= threeEighths.x && delta.y <= threeEighths.y)
	{
		return 2u;
	}
	return 1u;
}

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
		const uint weight = ResolveMeteringWeight(pixelPos);
		InterlockedAdd(gLocalHistogram[bin], weight);
		InterlockedAdd(gExposureDebug[2], weight);
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
