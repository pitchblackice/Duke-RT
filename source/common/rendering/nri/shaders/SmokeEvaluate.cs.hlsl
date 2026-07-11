#include "Include/SmokeResources.hlsli"

[numthreads(4, 4, 4)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (gSmokeConstants.FroxelWidth == 0u || gSmokeConstants.FroxelHeight == 0u || gSmokeConstants.FroxelDepth == 0u || gSmokeConstants.ColumnCapacity == 0u)
		return;

	if (dispatchThreadId.x >= gSmokeConstants.FroxelWidth || dispatchThreadId.y >= gSmokeConstants.FroxelHeight || dispatchThreadId.z >= gSmokeConstants.FroxelDepth)
		return;
	uint actualColumnCount, columnIndexCount, particleCount, styleCount, localFroxelCount, ignoredStride;
	gSmokeColumnCounts.GetDimensions(actualColumnCount, ignoredStride);
	gSmokeColumnIndices.GetDimensions(columnIndexCount, ignoredStride);
	gSmokeParticles.GetDimensions(particleCount, ignoredStride);
	gSmokeStyles.GetDimensions(styleCount, ignoredStride);
	gSmokeFroxelLocal.GetDimensions(localFroxelCount, ignoredStride);

	const uint columnIndex = dispatchThreadId.y * gSmokeConstants.FroxelWidth + dispatchThreadId.x;
	const uint froxelIndex = SmokeFroxelIndex(dispatchThreadId.x, dispatchThreadId.y, dispatchThreadId.z);
	if (columnIndex >= actualColumnCount || froxelIndex >= localFroxelCount)
		return;
	const uint candidateCount = min(min(gSmokeColumnCounts[columnIndex], gSmokeConstants.ColumnCapacity), NRI_SMOKE_MAX_EVALUATED_PARTICLES_PER_COLUMN);
	const float sliceNearDepth = dispatchThreadId.z == 0u ? 0.0 : SmokeSliceFarDepth(dispatchThreadId.z - 1u);
	const float sliceFarDepth = SmokeSliceFarDepth(dispatchThreadId.z);
	const float2 uv = (float2(dispatchThreadId.xy) + 0.5) / float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight);
	const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	const float3 ray = gSmokeConstants.CameraForward +
		gSmokeConstants.CameraRight * (ndc.x * gSmokeConstants.TanHalfFovX) +
		gSmokeConstants.CameraUp * (ndc.y * gSmokeConstants.TanHalfFovY);
	const float inverseRayLengthSquared = rcp(max(dot(ray, ray), 0.000001));
	const float sliceLength = max(sliceFarDepth - sliceNearDepth, 0.001);

	float extinction = 0.0;
	float3 scattering = 0.0;
	for (uint i = 0u; i < candidateCount; ++i)
	{
		const uint candidateIndex = columnIndex * gSmokeConstants.ColumnCapacity + i;
		if (candidateIndex >= columnIndexCount)
			break;
		const uint particleIndex = gSmokeColumnIndices[candidateIndex];
		if (particleIndex >= min(gSmokeConstants.ParticleCapacity, particleCount))
			continue;
		const SmokeParticle particle = gSmokeParticles[particleIndex];
		if (particle.Active == 0u || particle.Epoch != gSmokeConstants.SimulationEpoch || particle.StyleIndex >= min(gSmokeConstants.StyleCount, styleCount))
			continue;
		const SmokeStyle style = gSmokeStyles[particle.StyleIndex];
		const float closestDepth = clamp(dot(particle.Position - gSmokeConstants.CameraPosition, ray) * inverseRayLengthSquared, sliceNearDepth, sliceFarDepth);
		const float3 worldPosition = gSmokeConstants.CameraPosition + ray * closestDepth;
		const float normalizedDistance = length(worldPosition - particle.Position) / max(particle.Radius, 0.001);
		const float axialCoverage = saturate((2.0 * particle.Radius) / sliceLength);
		const float weight = saturate(1.0 - normalizedDistance * normalizedDistance) * axialCoverage;
		const float localExtinction = weight * particle.Density * style.Extinction * gSmokeConstants.DensityScale;
		extinction += localExtinction;
		// Deterministic ambient single scattering is the Phase 2 baseline.
		scattering += localExtinction * saturate(style.Albedo) * 0.18 * gSmokeConstants.RadianceScale;
	}

	gSmokeFroxelLocal[froxelIndex] = float4(scattering, max(extinction, 0.0));
}
