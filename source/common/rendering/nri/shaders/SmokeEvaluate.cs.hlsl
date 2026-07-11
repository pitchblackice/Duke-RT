#include "Include/SmokeResources.hlsli"

[numthreads(4, 4, 4)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gSmokeConstants.FroxelWidth || dispatchThreadId.y >= gSmokeConstants.FroxelHeight || dispatchThreadId.z >= gSmokeConstants.FroxelDepth)
		return;

	const uint columnIndex = dispatchThreadId.y * gSmokeConstants.FroxelWidth + dispatchThreadId.x;
	const uint candidateCount = min(gSmokeColumnCounts[columnIndex], gSmokeConstants.ColumnCapacity);
	const float viewDepth = 0.5 * (SmokeSliceFarDepth(dispatchThreadId.z) + (dispatchThreadId.z == 0u ? 0.0 : SmokeSliceFarDepth(dispatchThreadId.z - 1u)));
	const float2 uv = (float2(dispatchThreadId.xy) + 0.5) / float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight);
	const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	const float3 worldPosition = gSmokeConstants.CameraPosition +
		gSmokeConstants.CameraForward * viewDepth +
		gSmokeConstants.CameraRight * (ndc.x * viewDepth * gSmokeConstants.TanHalfFovX) +
		gSmokeConstants.CameraUp * (ndc.y * viewDepth * gSmokeConstants.TanHalfFovY);

	float extinction = 0.0;
	float3 scattering = 0.0;
	for (uint i = 0u; i < candidateCount; ++i)
	{
		const uint particleIndex = gSmokeColumnIndices[columnIndex * gSmokeConstants.ColumnCapacity + i];
		const SmokeParticle particle = gSmokeParticles[particleIndex];
		if (particle.Active == 0u || particle.StyleIndex >= gSmokeConstants.StyleCount)
			continue;
		const SmokeStyle style = gSmokeStyles[particle.StyleIndex];
		const float normalizedDistance = length(worldPosition - particle.Position) / max(particle.Radius, 0.001);
		const float weight = saturate(1.0 - normalizedDistance * normalizedDistance);
		const float localExtinction = weight * particle.Density * style.Extinction * gSmokeConstants.DensityScale;
		extinction += localExtinction;
		// Deterministic ambient single scattering is the Phase 2 baseline.
		scattering += localExtinction * saturate(style.Albedo) * 0.18 * gSmokeConstants.RadianceScale;
	}

	gSmokeFroxelLocal[SmokeFroxelIndex(dispatchThreadId.x, dispatchThreadId.y, dispatchThreadId.z)] = float4(scattering, max(extinction, 0.0));
}
