#include "Include/SmokeGridLightingResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint activeCapacity, ignoredStride;
	gSmokeGridLightActive.GetDimensions(activeCapacity, ignoredStride);
	if (dispatchThreadId.x >= min(gSmokeGridLightControl[0].ActiveCount, activeCapacity))
		return;
	const uint cellIndex = gSmokeGridLightActive[dispatchThreadId.x];
	const uint brickIndex = cellIndex / NRI_SMOKE_GRID_CELLS_PER_BRICK;
	const uint localIndex = cellIndex % NRI_SMOKE_GRID_CELLS_PER_BRICK;
	const uint3 local = uint3(localIndex & 7u, (localIndex >> 3u) & 7u, (localIndex >> 6u) & 7u);
	const SmokeGridBrick brick = gSmokeRenderGridBricks[brickIndex];
	const int3 cell = SmokeGridCellCoordinate(brick.Coordinate, local);
	SmokeGridLightRecord center;
	if ((gSmokeConstants.Flags & NRI_SMOKE_GRID_LIGHT_FIELD_PING) != 0u)
		center = gSmokeGridLightHistory[cellIndex];
	else
		center = gSmokeGridLightCurrent[cellIndex];
	if (!SmokeGridLightRecordValid(center, brick.Generation, gSmokeConstants.SimulationEpoch))
	{
		gSmokeGridLightFiltered[cellIndex] = center;
		return;
	}
	SmokeGridLightRecord output = center;
	[unroll]
	for (uint lobe = 0u; lobe < 6u; ++lobe)
	{
		float3 sum = SmokeGridLightMean(center, lobe);
		float3 second = SmokeGridLightSecondMoment(center, lobe);
		float weight = 1.0;
		[unroll]
		for (uint face = 0u; face < 6u; ++face)
		{
			if (!SmokeGridLightDirectedFaceOpen(cell, face))
				continue;
			uint neighborIndex, neighborGeneration;
			if (!SmokeGridLightCellAddress(cell + NRI_SMOKE_GRID_LIGHT_LOBE_AXES[face], neighborIndex, neighborGeneration))
				continue;
			SmokeGridLightRecord neighbor;
			if ((gSmokeConstants.Flags & NRI_SMOKE_GRID_LIGHT_FIELD_PING) != 0u)
				neighbor = gSmokeGridLightHistory[neighborIndex];
			else
				neighbor = gSmokeGridLightCurrent[neighborIndex];
			if (!SmokeGridLightRecordValid(neighbor, neighborGeneration, gSmokeConstants.SimulationEpoch))
				continue;
			const float3 neighborMean = SmokeGridLightMean(neighbor, lobe);
			const float3 centerSigma = sqrt(max(SmokeGridLightSecondMoment(center, lobe) - SmokeGridLightMean(center, lobe) * SmokeGridLightMean(center, lobe), 0.0));
			const float tolerance = 0.025 + 3.0 * max(length(centerSigma), length(sqrt(max(SmokeGridLightSecondMoment(neighbor, lobe) - neighborMean * neighborMean, 0.0))));
			if (length(neighborMean - SmokeGridLightMean(center, lobe)) > tolerance + 0.75 * max(length(neighborMean), length(SmokeGridLightMean(center, lobe))))
			{
				InterlockedAdd(gSmokeGridLightControl[0].FilterRejected, 1u);
				continue;
			}
			const float tapWeight = 0.125 * SmokeGridLightConfidence(neighbor);
			sum += neighborMean * tapWeight;
			second += SmokeGridLightSecondMoment(neighbor, lobe) * tapWeight;
			weight += tapWeight;
			InterlockedAdd(gSmokeGridLightControl[0].FilterAccepted, 1u);
		}
		const float3 filteredMean = sum / weight;
		SmokeGridLightStoreLobe(output, lobe, filteredMean, max(second / weight, filteredMean * filteredMean));
	}
	gSmokeGridLightFiltered[cellIndex] = output;
}
