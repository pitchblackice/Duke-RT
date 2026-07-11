#include "Include/SmokeResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint index = dispatchThreadId.x;
	const uint columnCount = gSmokeConstants.FroxelWidth * gSmokeConstants.FroxelHeight;
	const uint froxelCount = columnCount * gSmokeConstants.FroxelDepth;

	const bool clearWorld = (gSmokeConstants.Flags & 1u) != 0u;
	if (clearWorld && index == 0u)
	{
		SmokeControl control = (SmokeControl)0;
		control.Epoch = gSmokeConstants.SimulationEpoch;
		gSmokeControl[0] = control;
	}
	if (clearWorld && index < gSmokeConstants.ParticleCapacity)
	{
		SmokeParticle particle = (SmokeParticle)0;
		particle.Epoch = gSmokeConstants.SimulationEpoch;
		gSmokeParticles[index] = particle;
	}
	if (index < columnCount)
	{
		gSmokeColumnCounts[index] = 0u;
	}
	if (index < froxelCount)
	{
		gSmokeFroxelLocal[index] = 0.0;
		gSmokeFroxelIntegrated[index] = float4(0.0, 0.0, 0.0, 1.0);
	}
}
