#include "Include/SmokeGridLightingResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint capacity, ignoredStride;
	gSmokeGridLightSupportStamps.GetDimensions(capacity, ignoredStride);
	if (dispatchThreadId.x < capacity)
		gSmokeGridLightSupportStamps[dispatchThreadId.x] = (SmokeGridLightSupportStamp)0;
}
