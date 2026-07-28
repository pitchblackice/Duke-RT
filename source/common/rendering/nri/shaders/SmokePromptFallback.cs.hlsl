#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"

static const float kPromptFallbackOpticalScale = 0.5;

[numthreads(4, 4, 4)]
void main(uint3 froxel : SV_DispatchThreadID)
{
	if (froxel.x >= gSmokeConstants.FroxelWidth || froxel.y >= gSmokeConstants.FroxelHeight ||
		froxel.z >= gSmokeConstants.FroxelDepth)
		return;
	const float nearDepth = SmokeSliceNearDepth(froxel.z);
	const float farDepth = SmokeSliceFarDepth(froxel.z);
	const float3 worldPosition = SmokeWorldPosition(
		((float2)froxel.xy + 0.5) / float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight),
		0.5 * (nearDepth + farDepth));
	float extinction = 0.0;
	float3 scattering = 0.0;
	float weightedAnisotropy = 0.0;
	float anisotropyWeight = 0.0;
	uint commandCapacity, commandStride;
	gSmokeCommands.GetDimensions(commandCapacity, commandStride);
	[unroll]
	for (uint slot = 0u; slot < NRI_SMOKE_PROMPT_FALLBACK_QUANTITY; ++slot)
	{
		const SmokePromptOutcome outcome = gSmokePromptOutcomes[slot];
		if (outcome.Outcome != NRI_SMOKE_PROMPT_OUTCOME_FALLBACK || outcome.CommandIndex >= commandCapacity)
			continue;
		const SmokeInjectionCommand command = gSmokeCommands[outcome.CommandIndex];
		if (command.PulseIdLow != outcome.PulseIdLow || command.PulseIdHigh != outcome.PulseIdHigh ||
			command.RangeBegin != outcome.RangeBegin || command.RangeCount != outcome.RangeCount ||
			command.Epoch != gSmokeConstants.SimulationEpoch || command.StyleIndex >= gSmokeConstants.StyleCount)
			continue;
		const SmokeStyle style = gSmokeStyles[command.StyleIndex];
		float3 halfAxisU, halfAxisV;
		SmokeInjectionRectangleHalfAxes(command, halfAxisU, halfAxisV);
		const float3 closest = SmokeInjectionClosestRectanglePoint(worldPosition, command.Position,
			halfAxisU, halfAxisV);
		const float radius = max(max(command.SpawnRadius, style.Radius * command.RadiusScale), 0.001);
		const float normalized = saturate(1.0 - distance(worldPosition, closest) / radius);
		const float kernel = normalized * normalized * (3.0 - 2.0 * normalized);
		const float cellSize = max(asfloat(gSmokeRenderGridControl[0].CellSizeBits), 0.0001);
		const float radiusCells = radius / cellSize;
		const float halfUCells = length(halfAxisU) / cellSize;
		const float halfVCells = length(halfAxisV) / cellSize;
		const float rectangleAreaCells = 4.0 * halfUCells * halfVCells;
		const float rectanglePerimeterCells = 4.0 * (halfUCells + halfVCells);
		const float kernelNormalization = max(1.0,
			rectangleAreaCells * radiusCells +
			(3.0 * 3.14159265359 / 20.0) * rectanglePerimeterCells * radiusCells * radiusCells +
			(4.0 * 3.14159265359 / 15.0) * radiusCells * radiusCells * radiusCells);
		const float density = max(style.Density * command.DensityScale, 0.0) *
			(float)min(command.RangeCount, 256u) * kernel / kernelNormalization;
		const float sigmaT = density * max(style.Extinction, 0.0) *
			gSmokeConstants.DensityScale * kPromptFallbackOpticalScale;
		const float3 sigmaS = sigmaT * saturate(style.Albedo);
		extinction += sigmaT;
		scattering += sigmaS;
		const float weight = dot(sigmaS, float3(0.2126, 0.7152, 0.0722));
		weightedAnisotropy += weight * clamp(style.Anisotropy, -0.95, 0.95);
		anisotropyWeight += weight;
	}
	if (extinction <= 1e-6)
		return;
	const uint index = SmokeFroxelIndex(froxel.x, froxel.y, froxel.z);
	const float4 previousMedium = gSmokeFroxelMedium[index];
	const float4 previousPhase = gSmokeFroxelPhase[index];
	const float4 previousSource = gSmokeFroxelSource[index];
	const bool wasOccupied = previousMedium.w > 1e-6;
	gSmokeFroxelMedium[index] += float4(scattering, extinction);
	const float previousWeight = max(previousPhase.y, 0.0);
	const float combinedWeight = previousWeight + anisotropyWeight;
	const float anisotropy = combinedWeight > 1e-6 ?
		(previousPhase.x * previousWeight + weightedAnisotropy) / combinedWeight : 0.0;
	gSmokeFroxelPhase[index] = float4(anisotropy, combinedWeight, 1.0, 2.0);
	// Safe first-use seed: carrier visibility never depends on stale cached
	// radiance. Later analytic/world passes refine this conservative ambient term.
	const float3 environmentSeed = scattering * 0.02;
	const uint previousMetadata = SmokeFroxelMetadata(previousSource.w);
	if (wasOccupied && SmokeFroxelCarrierValid(previousMetadata) && SmokeFroxelRadianceValid(previousMetadata))
		gSmokeFroxelSource[index] = previousSource;
	else
	{
		uint metadata = SmokeFroxelCarrierMetadata(gSmokeConstants.SimulationEpoch);
		metadata = SmokeFroxelResolveRadiance(metadata, gSmokeConstants.SimulationEpoch,
			NRI_SMOKE_FALLBACK_ENVIRONMENT, 0u);
		gSmokeFroxelSource[index] = float4(environmentSeed,
			SmokeFroxelMetadataValue(metadata));
	}
	if (!wasOccupied)
	{
		uint capacity, ignoredStride, occupiedSlot;
		gSmokeOccupiedFroxelIndices.GetDimensions(capacity, ignoredStride);
		InterlockedAdd(gSmokeControl[0].OccupiedCount, 1u, occupiedSlot);
		if (occupiedSlot < capacity)
			gSmokeOccupiedFroxelIndices[occupiedSlot] = index;
		else
			InterlockedAdd(gSmokeControl[0].OccupiedOverflow, 1u);
	}
}
