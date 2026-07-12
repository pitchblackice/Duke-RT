#include "Include/SmokeResources.hlsli"

void SmokeAccumulateCandidate(
	uint particleIndex,
	uint particleCount,
	uint styleCount,
	float3 ray,
	float inverseRayLengthSquared,
	float sliceNearDepth,
	float sliceFarDepth,
	float sliceLength,
	inout float extinction,
	inout float3 scatteringCoefficient,
	inout float weightedAnisotropy,
	inout float anisotropyWeight,
	inout uint contributingCandidates)
{
	if (particleIndex >= min(gSmokeConstants.ParticleCapacity, particleCount))
		return;
	const SmokeParticle particle = gSmokeParticles[particleIndex];
	if (particle.Active == 0u || particle.Epoch != gSmokeConstants.SimulationEpoch || particle.StyleIndex >= min(gSmokeConstants.StyleCount, styleCount))
		return;
	const SmokeStyle style = gSmokeStyles[particle.StyleIndex];
	const float closestDepth = clamp(dot(particle.Position - gSmokeConstants.CameraPosition, ray) * inverseRayLengthSquared, sliceNearDepth, sliceFarDepth);
	const float3 particleSamplePosition = gSmokeConstants.CameraPosition + ray * closestDepth;
	const float normalizedDistance = length(particleSamplePosition - particle.Position) / max(particle.Radius, 0.001);
	const float axialCoverage = saturate((2.0 * particle.Radius) / sliceLength);
	const float weight = saturate(1.0 - normalizedDistance * normalizedDistance) * axialCoverage;
	const float localExtinction = weight * particle.Density * style.Extinction * gSmokeConstants.DensityScale;
	if (localExtinction <= 0.0)
		return;
	extinction += localExtinction;
	const float3 localScattering = localExtinction * saturate(style.Albedo);
	scatteringCoefficient += localScattering;
	const float localScatteringWeight = dot(localScattering, float3(0.2126, 0.7152, 0.0722));
	weightedAnisotropy += clamp(style.Anisotropy, -0.95, 0.95) * localScatteringWeight;
	anisotropyWeight += localScatteringWeight;
	contributingCandidates++;
}

void SmokeAccumulatePackedCandidate(
	uint packedCandidate,
	uint particleCount,
	uint styleCount,
	float3 ray,
	float inverseRayLengthSquared,
	float sliceNearDepth,
	float sliceFarDepth,
	float sliceLength,
	bool diagnostics,
	inout float extinction,
	inout float3 scatteringCoefficient,
	inout float weightedAnisotropy,
	inout float anisotropyWeight,
	inout uint contributingCandidates)
{
	if (packedCandidate == 0u)
		return;
	if (diagnostics)
		InterlockedAdd(gSmokeControl[0].MediumCandidateTests, 1u);
	SmokeAccumulateCandidate(SmokeUnpackCandidateIndex(packedCandidate), particleCount, styleCount, ray, inverseRayLengthSquared,
		sliceNearDepth, sliceFarDepth, sliceLength, extinction, scatteringCoefficient, weightedAnisotropy, anisotropyWeight, contributingCandidates);
}

[numthreads(4, 4, 4)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (gSmokeConstants.FroxelWidth == 0u || gSmokeConstants.FroxelHeight == 0u || gSmokeConstants.FroxelDepth == 0u)
		return;
	if (dispatchThreadId.x >= gSmokeConstants.FroxelWidth || dispatchThreadId.y >= gSmokeConstants.FroxelHeight || dispatchThreadId.z >= gSmokeConstants.FroxelDepth)
		return;

	uint fineCellCount, fineCellIndexCount, wideCellCount, wideCellIndexCount, globalDepthCount, globalDepthIndexCount;
	uint particleCount, styleCount, mediumCount, phaseCount, sourceCount, occupiedCapacity, controlCount, ignoredStride;
	gSmokeFineCellCounts.GetDimensions(fineCellCount, ignoredStride);
	gSmokeFineCellIndices.GetDimensions(fineCellIndexCount, ignoredStride);
	gSmokeWideCellCounts.GetDimensions(wideCellCount, ignoredStride);
	gSmokeWideCellIndices.GetDimensions(wideCellIndexCount, ignoredStride);
	gSmokeGlobalDepthCounts.GetDimensions(globalDepthCount, ignoredStride);
	gSmokeGlobalDepthIndices.GetDimensions(globalDepthIndexCount, ignoredStride);
	gSmokeParticles.GetDimensions(particleCount, ignoredStride);
	gSmokeStyles.GetDimensions(styleCount, ignoredStride);
	gSmokeFroxelMedium.GetDimensions(mediumCount, ignoredStride);
	gSmokeFroxelPhase.GetDimensions(phaseCount, ignoredStride);
	gSmokeFroxelSource.GetDimensions(sourceCount, ignoredStride);
	gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, ignoredStride);
	gSmokeControl.GetDimensions(controlCount, ignoredStride);

	const uint froxelIndex = SmokeFroxelIndex(dispatchThreadId.x, dispatchThreadId.y, dispatchThreadId.z);
	if (froxelIndex >= fineCellCount || froxelIndex >= mediumCount || froxelIndex >= phaseCount || froxelIndex >= sourceCount)
		return;
	const float sliceNearDepth = SmokeSliceNearDepth(dispatchThreadId.z);
	const float sliceFarDepth = SmokeSliceFarDepth(dispatchThreadId.z);
	const float sliceLength = max(sliceFarDepth - sliceNearDepth, 0.001);
	const float3 ray = SmokeFroxelRay(dispatchThreadId.xy);
	const float inverseRayLengthSquared = rcp(max(dot(ray, ray), 0.000001));
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u && controlCount > 0u;

	float extinction = 0.0;
	float3 scatteringCoefficient = 0.0;
	float weightedAnisotropy = 0.0;
	float anisotropyWeight = 0.0;
	uint contributingCandidates = 0u;
	const uint fineCandidateCount = gSmokeFineCellCounts[froxelIndex] > 0u ? NRI_SMOKE_FINE_CELL_CAPACITY : 0u;
	[unroll]
	for (uint i = 0u; i < fineCandidateCount; ++i)
	{
		const uint candidateIndex = froxelIndex * NRI_SMOKE_FINE_CELL_CAPACITY + i;
		if (candidateIndex >= fineCellIndexCount)
			break;
		SmokeAccumulatePackedCandidate(gSmokeFineCellIndices[candidateIndex], particleCount, styleCount, ray, inverseRayLengthSquared,
			sliceNearDepth, sliceFarDepth, sliceLength, diagnostics, extinction, scatteringCoefficient, weightedAnisotropy, anisotropyWeight, contributingCandidates);
	}

	const uint2 wideCellPosition = min(uint2(
		dispatchThreadId.x * NRI_SMOKE_WIDE_GRID_X / gSmokeConstants.FroxelWidth,
		dispatchThreadId.y * NRI_SMOKE_WIDE_GRID_Y / gSmokeConstants.FroxelHeight),
		uint2(NRI_SMOKE_WIDE_GRID_X - 1u, NRI_SMOKE_WIDE_GRID_Y - 1u));
	const uint wideCellIndex = SmokeWideCellIndex(wideCellPosition.x, wideCellPosition.y, dispatchThreadId.z);
	const uint wideCandidateCount = wideCellIndex < wideCellCount && gSmokeWideCellCounts[wideCellIndex] > 0u ? NRI_SMOKE_WIDE_CELL_CAPACITY : 0u;
	[unroll]
	for (uint i = 0u; i < wideCandidateCount; ++i)
	{
		const uint candidateIndex = wideCellIndex * NRI_SMOKE_WIDE_CELL_CAPACITY + i;
		if (candidateIndex >= wideCellIndexCount)
			break;
		SmokeAccumulatePackedCandidate(gSmokeWideCellIndices[candidateIndex], particleCount, styleCount, ray, inverseRayLengthSquared,
			sliceNearDepth, sliceFarDepth, sliceLength, diagnostics, extinction, scatteringCoefficient, weightedAnisotropy, anisotropyWeight, contributingCandidates);
	}

	const uint globalCandidateCount = dispatchThreadId.z < globalDepthCount && gSmokeGlobalDepthCounts[dispatchThreadId.z] > 0u
		? NRI_SMOKE_GLOBAL_DEPTH_CAPACITY : 0u;
	[unroll]
	for (uint i = 0u; i < globalCandidateCount; ++i)
	{
		const uint candidateIndex = dispatchThreadId.z * NRI_SMOKE_GLOBAL_DEPTH_CAPACITY + i;
		if (candidateIndex >= globalDepthIndexCount)
			break;
		SmokeAccumulatePackedCandidate(gSmokeGlobalDepthIndices[candidateIndex], particleCount, styleCount, ray, inverseRayLengthSquared,
			sliceNearDepth, sliceFarDepth, sliceLength, diagnostics, extinction, scatteringCoefficient, weightedAnisotropy, anisotropyWeight, contributingCandidates);
	}

	extinction = max(extinction, 0.0);
	const bool occupied = extinction > 0.0;
	const float anisotropy = anisotropyWeight > 1e-6 ? weightedAnisotropy / anisotropyWeight : 0.0;
	gSmokeFroxelMedium[froxelIndex] = float4(scatteringCoefficient, extinction);
	gSmokeFroxelPhase[froxelIndex] = float4(anisotropy, anisotropyWeight, (float)contributingCandidates, occupied ? 1.0 : 0.0);
	gSmokeFroxelSource[froxelIndex] = 0.0;

	if (occupied && controlCount > 0u)
	{
		uint occupiedSlot = 0u;
		InterlockedAdd(gSmokeControl[0].OccupiedCount, 1u, occupiedSlot);
		if (occupiedSlot < occupiedCapacity)
			gSmokeOccupiedFroxelIndices[occupiedSlot] = froxelIndex;
		else
			InterlockedAdd(gSmokeControl[0].OccupiedOverflow, 1u);
	}
}
