#include "Include/ExposureConstants.hlsli"

[numthreads(256, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint index = dispatchThreadId.x;
	if (index < gExposureConstants.HistogramBinCount)
	{
		gExposureHistogram[index] = 0u;
	}

	if (index < NRI_EXPOSURE_DEBUG_WORD_COUNT)
	{
		gExposureDebug[index] = 0u;
	}
}
