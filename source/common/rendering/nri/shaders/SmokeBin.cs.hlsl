#include "Include/SmokeResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint particleIndex = dispatchThreadId.x;
	if (particleIndex >= gSmokeConstants.ParticleCapacity)
		return;

	const SmokeParticle particle = gSmokeParticles[particleIndex];
	if (particle.Active == 0u || particle.Epoch != gSmokeConstants.SimulationEpoch)
		return;

	const float3 relativePosition = particle.Position - gSmokeConstants.CameraPosition;
	const float viewDepth = dot(relativePosition, gSmokeConstants.CameraForward);
	if (viewDepth + particle.Radius <= 0.0 || viewDepth - particle.Radius >= gSmokeConstants.FroxelMaxDistance)
		return;

	const float2 ndcCenter = float2(
		dot(relativePosition, gSmokeConstants.CameraRight) / max(viewDepth * gSmokeConstants.TanHalfFovX, 0.001),
		-dot(relativePosition, gSmokeConstants.CameraUp) / max(viewDepth * gSmokeConstants.TanHalfFovY, 0.001));
	const float2 uvCenter = ndcCenter * 0.5 + 0.5;
	const float2 uvRadius = float2(
		particle.Radius / max(viewDepth * gSmokeConstants.TanHalfFovX, 0.001),
		particle.Radius / max(viewDepth * gSmokeConstants.TanHalfFovY, 0.001)) * 0.5;
	const int2 minimumColumn = max(int2(floor((uvCenter - uvRadius) * float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight))), int2(0, 0));
	const int2 maximumColumn = min(int2(floor((uvCenter + uvRadius) * float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight))), int2(gSmokeConstants.FroxelWidth - 1u, gSmokeConstants.FroxelHeight - 1u));

	for (int y = minimumColumn.y; y <= maximumColumn.y; ++y)
	{
		for (int x = minimumColumn.x; x <= maximumColumn.x; ++x)
		{
			const uint columnIndex = (uint)y * gSmokeConstants.FroxelWidth + (uint)x;
			uint listIndex = 0u;
			InterlockedAdd(gSmokeColumnCounts[columnIndex], 1u, listIndex);
			if (listIndex < gSmokeConstants.ColumnCapacity)
				gSmokeColumnIndices[columnIndex * gSmokeConstants.ColumnCapacity + listIndex] = particleIndex;
			else
				InterlockedAdd(gSmokeControl[0].ColumnOverflow, 1u);
		}
	}
}
