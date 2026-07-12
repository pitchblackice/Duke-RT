#include "Include/SmokeResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint index = dispatchThreadId.x;
	const uint columnCount = gSmokeConstants.FroxelWidth * gSmokeConstants.FroxelHeight;
	const uint froxelCount = columnCount * gSmokeConstants.FroxelDepth;
	uint controlCount, particleCount, actualColumnCount, localFroxelCount, integratedFroxelCount, ignoredStride;
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeParticles.GetDimensions(particleCount, ignoredStride);
	gSmokeColumnCounts.GetDimensions(actualColumnCount, ignoredStride);
	gSmokeFroxelLocal.GetDimensions(localFroxelCount, ignoredStride);
	gSmokeFroxelIntegrated.GetDimensions(integratedFroxelCount, ignoredStride);

	const bool clearWorld = (gSmokeConstants.Flags & 1u) != 0u;
	if (index == 0u && controlCount != 0u)
	{
		gSmokeControl[0].Reserved = 0u;
		gSmokeControl[0].LightCandidatesTested = 0u;
		gSmokeControl[0].LightDistanceRejected = 0u;
		gSmokeControl[0].LightShadowRays = 0u;
		gSmokeControl[0].LightShadowVisible = 0u;
		gSmokeControl[0].LightShadowOccluded = 0u;
		gSmokeControl[0].LightSoftSamples = 0u;
		gSmokeControl[0].LightRadianceClamps = 0u;
	}
	if (clearWorld && index == 0u && controlCount != 0u)
	{
		SmokeControl control = (SmokeControl)0;
		control.Epoch = gSmokeConstants.SimulationEpoch;
		gSmokeControl[0] = control;
	}
	if (clearWorld && index < min(gSmokeConstants.ParticleCapacity, particleCount))
	{
		SmokeParticle particle = (SmokeParticle)0;
		particle.Epoch = gSmokeConstants.SimulationEpoch;
		gSmokeParticles[index] = particle;
	}
	if (index < min(columnCount, actualColumnCount))
	{
		gSmokeColumnCounts[index] = 0u;
	}
	if (index < min(froxelCount, min(localFroxelCount, integratedFroxelCount)))
	{
		gSmokeFroxelLocal[index] = 0.0;
		gSmokeFroxelIntegrated[index] = float4(0.0, 0.0, 0.0, 1.0);
	}
}
