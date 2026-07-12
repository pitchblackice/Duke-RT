#include "Include/SmokeResources.hlsli"

SmokeCellHeader SmokeEmptyCell()
{
	SmokeCellHeader cell = (SmokeCellHeader)0;
	cell.Head = NRI_SMOKE_REFERENCE_END;
	return cell;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint index = dispatchThreadId.x;
	const uint froxelCount = gSmokeConstants.FroxelWidth * gSmokeConstants.FroxelHeight * gSmokeConstants.FroxelDepth;
	const uint expectedWideCellCount = NRI_SMOKE_WIDE_CELL_COUNT * gSmokeConstants.FroxelDepth;
	uint controlCount, particleCount, fineCellCount, wideCellCount;
	uint globalDepthCount, mediumFroxelCount, integratedFroxelCount, phaseFroxelCount, sourceFroxelCount, ignoredStride;
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeParticles.GetDimensions(particleCount, ignoredStride);
	gSmokeFineCells.GetDimensions(fineCellCount, ignoredStride);
	gSmokeWideCells.GetDimensions(wideCellCount, ignoredStride);
	gSmokeGlobalDepthCells.GetDimensions(globalDepthCount, ignoredStride);
	gSmokeFroxelMedium.GetDimensions(mediumFroxelCount, ignoredStride);
	gSmokeFroxelIntegrated.GetDimensions(integratedFroxelCount, ignoredStride);
	gSmokeFroxelPhase.GetDimensions(phaseFroxelCount, ignoredStride);
	gSmokeFroxelSource.GetDimensions(sourceFroxelCount, ignoredStride);

	const bool clearWorld = (gSmokeConstants.Flags & 1u) != 0u;
	if (index == 0u && controlCount != 0u)
	{
		if (clearWorld)
		{
			SmokeControl control = (SmokeControl)0;
			control.Epoch = gSmokeConstants.SimulationEpoch;
			control.MaximumCandidatesPerFroxel = 0u;
			gSmokeControl[0] = control;
		}
		else
		{
			gSmokeControl[0].WideParticlesProjected = 0u;
			gSmokeControl[0].WideGlobalDrops = 0u;
			gSmokeControl[0].FineColumnReferences = 0u;
			gSmokeControl[0].WideCellReferences = 0u;
			gSmokeControl[0].GlobalDepthReferences = 0u;
			gSmokeControl[0].ReferenceInvalidLinks = 0u;
			gSmokeControl[0].ReferenceTraversalLimitExits = 0u;
			gSmokeControl[0].FineTierParticles = 0u;
			gSmokeControl[0].WideTierParticles = 0u;
			gSmokeControl[0].GlobalTierParticles = 0u;
			gSmokeControl[0].FineOccupiedCells = 0u;
			gSmokeControl[0].WideOccupiedCells = 0u;
			gSmokeControl[0].GlobalOccupiedSlices = 0u;
			gSmokeControl[0].FineMaximumCellReferences = 0u;
			gSmokeControl[0].WideMaximumCellReferences = 0u;
			gSmokeControl[0].GlobalMaximumCellReferences = 0u;
			gSmokeControl[0].MaximumDepthSpan = 0u;
			gSmokeControl[0].DepthSpanOne = 0u;
			gSmokeControl[0].DepthSpanTwoToFour = 0u;
			gSmokeControl[0].DepthSpanFiveToSixteen = 0u;
			gSmokeControl[0].DepthSpanOverSixteen = 0u;
			gSmokeControl[0].MaximumCandidatesPerFroxel = 0u;
			gSmokeControl[0].OccupiedCount = 0u;
			gSmokeControl[0].OccupiedOverflow = 0u;
			gSmokeControl[0].MediumCandidateTests = 0u;
			gSmokeControl[0].PointFroxelsProcessed = 0u;
			gSmokeControl[0].DirectionalFroxelsProcessed = 0u;
			gSmokeControl[0].DirectionalSamples = 0u;
			gSmokeControl[0].DirectionalShadowRays = 0u;
			gSmokeControl[0].DirectionalShadowVisible = 0u;
			gSmokeControl[0].DirectionalShadowOccluded = 0u;
			gSmokeControl[0].DirectionalRadianceClamps = 0u;
			gSmokeControl[0].EmissiveFroxelsProcessed = 0u;
			gSmokeControl[0].EmissiveSamples = 0u;
			gSmokeControl[0].EmissiveCandidateMisses = 0u;
			gSmokeControl[0].EmissiveDistanceRejected = 0u;
			gSmokeControl[0].EmissiveFacingRejected = 0u;
			gSmokeControl[0].EmissiveShadowRays = 0u;
			gSmokeControl[0].EmissiveShadowVisible = 0u;
			gSmokeControl[0].EmissiveShadowOccluded = 0u;
			gSmokeControl[0].EmissiveContributed = 0u;
			gSmokeControl[0].EmissiveRadianceClamps = 0u;
			gSmokeControl[0].LightCandidatesTested = 0u;
			gSmokeControl[0].LightDistanceRejected = 0u;
			gSmokeControl[0].LightShadowRays = 0u;
			gSmokeControl[0].LightShadowVisible = 0u;
			gSmokeControl[0].LightShadowOccluded = 0u;
			gSmokeControl[0].LightSoftSamples = 0u;
			gSmokeControl[0].LightRadianceClamps = 0u;
			gSmokeControl[0].FilterCandidateHits = 0u;
			gSmokeControl[0].FilterAlphaRejects = 0u;
			gSmokeControl[0].FilterNoShadowRejects = 0u;
			gSmokeControl[0].FilterOneWayRejects = 0u;
			gSmokeControl[0].FilterReflectionRejects = 0u;
			gSmokeControl[0].FilterPortalContinuations = 0u;
			gSmokeControl[0].FilterAcceptedBlockers = 0u;
			gSmokeControl[0].FilterMisses = 0u;
			gSmokeControl[0].FilterSkipLimitExits = 0u;
			gSmokeControl[0].FilterContinuationLimitExits = 0u;
			gSmokeControl[0].FilterResourceDowngrades = 0u;
			if ((gSmokeConstants.FilteredVisibilityEnabled & 1u) != 0u && (gSmokeConstants.FilteredVisibilityEnabled & 2u) == 0u && gSmokeConstants.LightMode > 0u)
				gSmokeControl[0].FilterResourceDowngrades = 1u;
		}
	}
	if (clearWorld && index < min(gSmokeConstants.ParticleCapacity, particleCount))
	{
		SmokeParticle particle = (SmokeParticle)0;
		particle.Epoch = gSmokeConstants.SimulationEpoch;
		gSmokeParticles[index] = particle;
	}
	if (index < min(froxelCount, fineCellCount))
		gSmokeFineCells[index] = SmokeEmptyCell();
	if (index < min(expectedWideCellCount, wideCellCount))
		gSmokeWideCells[index] = SmokeEmptyCell();
	if (index < min(gSmokeConstants.FroxelDepth, globalDepthCount))
		gSmokeGlobalDepthCells[index] = SmokeEmptyCell();
	if (index < min(froxelCount, min(mediumFroxelCount, min(integratedFroxelCount, min(phaseFroxelCount, sourceFroxelCount)))))
	{
		gSmokeFroxelMedium[index] = 0.0;
		gSmokeFroxelIntegrated[index] = float4(0.0, 0.0, 0.0, 1.0);
		gSmokeFroxelPhase[index] = 0.0;
		gSmokeFroxelSource[index] = 0.0;
	}
}
