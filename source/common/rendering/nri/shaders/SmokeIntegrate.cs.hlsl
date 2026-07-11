#include "Include/SmokeResources.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gSmokeConstants.FroxelWidth || dispatchThreadId.y >= gSmokeConstants.FroxelHeight)
		return;

	float transmittance = 1.0;
	float3 radiance = 0.0;
	float previousDepth = 0.0;
	for (uint z = 0u; z < gSmokeConstants.FroxelDepth; ++z)
	{
		const uint froxelIndex = SmokeFroxelIndex(dispatchThreadId.x, dispatchThreadId.y, z);
		const float4 localMedium = gSmokeFroxelLocal[froxelIndex];
		const float farDepth = SmokeSliceFarDepth(z);
		const float stepLength = max(farDepth - previousDepth, 0.0);
		const float stepTransmittance = exp(-localMedium.a * stepLength);
		const float scatterIntegral = localMedium.a > 0.000001 ? (1.0 - stepTransmittance) / localMedium.a : stepLength;
		radiance += transmittance * localMedium.rgb * scatterIntegral;
		transmittance *= stepTransmittance;
		gSmokeFroxelIntegrated[froxelIndex] = float4(radiance, transmittance);
		previousDepth = farDepth;
	}
}
