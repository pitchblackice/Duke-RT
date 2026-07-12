#include "Include/SmokeResources.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (gSmokeConstants.FroxelWidth == 0u || gSmokeConstants.FroxelHeight == 0u || gSmokeConstants.FroxelDepth == 0u)
		return;

	if (dispatchThreadId.x >= gSmokeConstants.FroxelWidth || dispatchThreadId.y >= gSmokeConstants.FroxelHeight)
		return;
	uint mediumFroxelCount, sourceFroxelCount, integratedFroxelCount, ignoredStride;
	gSmokeFroxelMedium.GetDimensions(mediumFroxelCount, ignoredStride);
	gSmokeFroxelSource.GetDimensions(sourceFroxelCount, ignoredStride);
	gSmokeFroxelIntegrated.GetDimensions(integratedFroxelCount, ignoredStride);

	float transmittance = 1.0;
	float3 radiance = 0.0;
	float previousDepth = 0.0;
	for (uint z = 0u; z < gSmokeConstants.FroxelDepth; ++z)
	{
		const uint froxelIndex = SmokeFroxelIndex(dispatchThreadId.x, dispatchThreadId.y, z);
		if (froxelIndex >= min(mediumFroxelCount, min(sourceFroxelCount, integratedFroxelCount)))
			break;
		const float4 medium = gSmokeFroxelMedium[froxelIndex];
		const float3 source = gSmokeFroxelSource[froxelIndex].rgb;
		const float farDepth = SmokeSliceFarDepth(z);
		const float stepLength = max(farDepth - previousDepth, 0.0);
		const float stepTransmittance = exp(-medium.a * stepLength);
		const float scatterIntegral = medium.a > 0.000001 ? (1.0 - stepTransmittance) / medium.a : stepLength;
		radiance += transmittance * source * scatterIntegral;
		transmittance *= stepTransmittance;
		gSmokeFroxelIntegrated[froxelIndex] = float4(radiance, transmittance);
		previousDepth = farDepth;
	}
}
