#include "Include/SmokeResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint index = dispatchThreadId.x;
	uint headerCount, headerStride;
	uint mediumCount, mediumStride;
	gSmokeAnalyticTileHeaders.GetDimensions(headerCount, headerStride);
	gSmokeAnalyticFroxelMedium.GetDimensions(mediumCount, mediumStride);
	if (index < headerCount)
		gSmokeAnalyticTileHeaders[index] = (SmokeAnalyticTileHeader)0;
	if (index < mediumCount)
		gSmokeAnalyticFroxelMedium[index] = 0.0;
}
