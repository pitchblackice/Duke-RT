#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"

void SmokeAccumulateCandidate(
	uint particleIndex,
	uint particleCount,
	uint styleCount,
	uint directionalVisibilityCount,
	float3 ray,
	float sliceNearDepth,
	float sliceFarDepth,
	inout float extinction,
	inout float3 scatteringCoefficient,
	inout float weightedAnisotropy,
	inout float anisotropyWeight,
	inout float3 visibleDirectionalScattering,
	inout uint contributingCandidates)
{
	if (particleIndex >= min(gSmokeConstants.ParticleCapacity, particleCount))
		return;
	const SmokeParticle particle = gSmokeParticles[particleIndex];
	if (particle.Active == 0u || particle.Epoch != gSmokeConstants.SimulationEpoch || particle.StyleIndex >= min(gSmokeConstants.StyleCount, styleCount))
		return;
	const SmokeStyle style = gSmokeStyles[particle.StyleIndex];
	const float weight = SmokeSphereSegmentKernelAverage(particle.Position, particle.Radius, ray, sliceNearDepth, sliceFarDepth);
	const float localExtinction = weight * particle.Density * style.Extinction * gSmokeConstants.DensityScale;
	if (localExtinction <= 0.0)
		return;
	extinction += localExtinction;
	const float3 localScattering = localExtinction * saturate(style.Albedo);
	scatteringCoefficient += localScattering;
	const float localScatteringWeight = dot(localScattering, float3(0.2126, 0.7152, 0.0722));
	weightedAnisotropy += clamp(style.Anisotropy, -0.95, 0.95) * localScatteringWeight;
	anisotropyWeight += localScatteringWeight;
	const float directionalVisibility = particleIndex < directionalVisibilityCount
		? saturate(gSmokeParticleDirectionalVisibility[particleIndex])
		: 0.0;
	visibleDirectionalScattering += localScattering * directionalVisibility;
	contributingCandidates++;
}

void SmokeAccumulateReferenceList(
	SmokeCellHeader cell,
	uint particleCount,
	uint styleCount,
	uint directionalVisibilityCount,
	float3 ray,
	float sliceNearDepth,
	float sliceFarDepth,
	inout uint remainingTraversal,
	inout uint testedReferences,
	inout bool invalidLink,
	inout bool traversalLimitExit,
	inout float extinction,
	inout float3 scatteringCoefficient,
	inout float weightedAnisotropy,
	inout float anisotropyWeight,
	inout float3 visibleDirectionalScattering,
	inout uint contributingCandidates)
{
	if (cell.Head == NRI_SMOKE_REFERENCE_END || cell.Count == 0u || remainingTraversal == 0u)
	{
		if ((cell.Head == NRI_SMOKE_REFERENCE_END) != (cell.Count == 0u))
			invalidLink = true;
		if (cell.Head != NRI_SMOKE_REFERENCE_END && remainingTraversal == 0u)
			traversalLimitExit = true;
		return;
	}

	uint nodeCount, ignoredStride;
	gSmokeReferenceNext.GetDimensions(nodeCount, ignoredStride);
	uint nodeIndex = cell.Head;
	uint traversed = 0u;
	const uint traversalCount = min(cell.Count, remainingTraversal);
	[loop]
	while (traversed < traversalCount && nodeIndex != NRI_SMOKE_REFERENCE_END)
	{
		if (nodeIndex >= nodeCount)
		{
			invalidLink = true;
			break;
		}
		const uint particleIndex = nodeIndex / NRI_SMOKE_MAX_TIER_REFERENCES;
		SmokeAccumulateCandidate(particleIndex, particleCount, styleCount, directionalVisibilityCount, ray, sliceNearDepth, sliceFarDepth,
			extinction, scatteringCoefficient, weightedAnisotropy, anisotropyWeight, visibleDirectionalScattering, contributingCandidates);
		nodeIndex = gSmokeReferenceNext[nodeIndex];
		traversed++;
		testedReferences++;
		remainingTraversal--;
	}

	if (nodeIndex == NRI_SMOKE_REFERENCE_END)
	{
		if (traversed != cell.Count)
			invalidLink = true;
	}
	else if (remainingTraversal == 0u)
	{
		traversalLimitExit = true;
	}
	else if (traversed >= cell.Count)
	{
		invalidLink = true;
	}
}

[numthreads(4, 4, 4)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (gSmokeConstants.FroxelWidth == 0u || gSmokeConstants.FroxelHeight == 0u || gSmokeConstants.FroxelDepth == 0u)
		return;
	if (dispatchThreadId.x >= gSmokeConstants.FroxelWidth || dispatchThreadId.y >= gSmokeConstants.FroxelHeight || dispatchThreadId.z >= gSmokeConstants.FroxelDepth)
		return;

	uint fineCellCount, wideCellCount, globalDepthCount;
	uint particleCount, styleCount, mediumCount, phaseCount, sourceCount, occupiedCapacity, controlCount, scratchCount;
	uint directionalVisibilityCount, ignoredStride;
	gSmokeFineCells.GetDimensions(fineCellCount, ignoredStride);
	gSmokeWideCells.GetDimensions(wideCellCount, ignoredStride);
	gSmokeGlobalDepthCells.GetDimensions(globalDepthCount, ignoredStride);
	gSmokeParticles.GetDimensions(particleCount, ignoredStride);
	gSmokeStyles.GetDimensions(styleCount, ignoredStride);
	gSmokeFroxelMedium.GetDimensions(mediumCount, ignoredStride);
	gSmokeFroxelPhase.GetDimensions(phaseCount, ignoredStride);
	gSmokeFroxelSource.GetDimensions(sourceCount, ignoredStride);
	gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, ignoredStride);
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeIndirectScratch.GetDimensions(scratchCount, ignoredStride);
	gSmokeParticleDirectionalVisibility.GetDimensions(directionalVisibilityCount, ignoredStride);

	const uint froxelIndex = SmokeFroxelIndex(dispatchThreadId.x, dispatchThreadId.y, dispatchThreadId.z);
	if (froxelIndex >= fineCellCount || froxelIndex >= mediumCount || froxelIndex >= phaseCount || froxelIndex >= sourceCount)
		return;
	const float sliceNearDepth = SmokeSliceNearDepth(dispatchThreadId.z);
	const float sliceFarDepth = SmokeSliceFarDepth(dispatchThreadId.z);
	const float3 ray = SmokeFroxelRay(dispatchThreadId.xy);
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u && controlCount > 0u;

	float extinction = 0.0;
	float3 scatteringCoefficient = 0.0;
	float weightedAnisotropy = 0.0;
	float anisotropyWeight = 0.0;
	float3 visibleDirectionalScattering = 0.0;
	uint contributingCandidates = 0u;
	uint testedReferences = 0u;
	uint remainingTraversal = min(gSmokeConstants.ParticleCapacity, particleCount);
	bool invalidLink = false;
	bool traversalLimitExit = false;

	SmokeAccumulateReferenceList(gSmokeFineCells[froxelIndex], particleCount, styleCount, directionalVisibilityCount, ray, sliceNearDepth, sliceFarDepth,
		remainingTraversal, testedReferences, invalidLink, traversalLimitExit, extinction, scatteringCoefficient,
		weightedAnisotropy, anisotropyWeight, visibleDirectionalScattering, contributingCandidates);

	const uint2 wideCellPosition = min(uint2(
		dispatchThreadId.x * NRI_SMOKE_WIDE_GRID_X / gSmokeConstants.FroxelWidth,
		dispatchThreadId.y * NRI_SMOKE_WIDE_GRID_Y / gSmokeConstants.FroxelHeight),
		uint2(NRI_SMOKE_WIDE_GRID_X - 1u, NRI_SMOKE_WIDE_GRID_Y - 1u));
	const uint wideCellIndex = SmokeWideCellIndex(wideCellPosition.x, wideCellPosition.y, dispatchThreadId.z);
	if (wideCellIndex < wideCellCount)
	{
		SmokeAccumulateReferenceList(gSmokeWideCells[wideCellIndex], particleCount, styleCount, directionalVisibilityCount, ray, sliceNearDepth, sliceFarDepth,
			remainingTraversal, testedReferences, invalidLink, traversalLimitExit, extinction, scatteringCoefficient,
			weightedAnisotropy, anisotropyWeight, visibleDirectionalScattering, contributingCandidates);
	}

	if (dispatchThreadId.z < globalDepthCount)
	{
		SmokeAccumulateReferenceList(gSmokeGlobalDepthCells[dispatchThreadId.z], particleCount, styleCount, directionalVisibilityCount, ray, sliceNearDepth, sliceFarDepth,
			remainingTraversal, testedReferences, invalidLink, traversalLimitExit, extinction, scatteringCoefficient,
			weightedAnisotropy, anisotropyWeight, visibleDirectionalScattering, contributingCandidates);
	}

	if (diagnostics)
	{
		if (testedReferences > 0u) InterlockedAdd(gSmokeControl[0].MediumCandidateTests, testedReferences);
		InterlockedMax(gSmokeControl[0].MaximumCandidatesPerFroxel, testedReferences);
		if (invalidLink) InterlockedAdd(gSmokeControl[0].ReferenceInvalidLinks, 1u);
		if (traversalLimitExit) InterlockedAdd(gSmokeControl[0].ReferenceTraversalLimitExits, 1u);
	}

	extinction = max(extinction, 0.0);
	const bool occupied = extinction > 0.0;
	const float anisotropy = anisotropyWeight > 1e-6 ? weightedAnisotropy / anisotropyWeight : 0.0;
	gSmokeFroxelMedium[froxelIndex] = float4(scatteringCoefficient, extinction);
	gSmokeFroxelPhase[froxelIndex] = float4(anisotropy, anisotropyWeight, (float)contributingCandidates, occupied ? 1.0 : 0.0);
	gSmokeFroxelSource[froxelIndex] = 0.0;
	if (froxelIndex < scratchCount)
	{
		SmokeIndirectCacheRecord currentRecord = (SmokeIndirectCacheRecord)0;
		currentRecord.Radiance = max(visibleDirectionalScattering, 0.0);
		gSmokeIndirectScratch[froxelIndex] = currentRecord;
	}

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
