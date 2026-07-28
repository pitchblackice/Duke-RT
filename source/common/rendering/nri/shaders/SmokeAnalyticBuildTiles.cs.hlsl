#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint carrierCapacity, carrierStride;
	gSmokeAnalyticCarriers.GetDimensions(carrierCapacity, carrierStride);
	const uint carrierIndex = dispatchThreadId.x;
	if (carrierIndex >= min(min(carrierCapacity, NRI_SMOKE_ANALYTIC_MAX_CARRIERS),
		gSmokeConstants.ParticleCapacity))
		return;
	const SmokeAnalyticCarrier carrier = gSmokeAnalyticCarriers[carrierIndex];
	if ((carrier.Flags & NRI_SMOKE_ANALYTIC_CARRIER_ACTIVE) == 0u ||
		carrier.Epoch != gSmokeConstants.SimulationEpoch ||
		!isfinite(carrier.Radius) || carrier.Radius <= 0.0 ||
		!all(isfinite(carrier.Position)))
		return;

	float projectionRadius = carrier.Radius;
	if (carrier.Shape == NRI_SMOKE_INJECTION_SHAPE_RECTANGLE)
	{
		if (!all(isfinite(carrier.HalfAxisU)) || !all(isfinite(carrier.HalfAxisV)))
			return;
		projectionRadius += length(carrier.HalfAxisU) + length(carrier.HalfAxisV);
	}
	int2 minimumColumn, maximumColumn;
	if (!SmokeProjectSphereToFroxelBounds(carrier.Position, projectionRadius,
		minimumColumn, maximumColumn))
		return;

	const uint2 tileCount = SmokeAnalyticTileCount(
		gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight);
	if (any(tileCount == 0u))
		return;
	const uint2 minimumTile = (uint2)minimumColumn / NRI_SMOKE_ANALYTIC_TILE_SIZE;
	const uint2 maximumTile = min((uint2)maximumColumn / NRI_SMOKE_ANALYTIC_TILE_SIZE,
		tileCount - 1u);
	uint headerCapacity, headerStride;
	uint indexCapacity, indexStride;
	gSmokeAnalyticTileHeaders.GetDimensions(headerCapacity, headerStride);
	gSmokeAnalyticTileIndices.GetDimensions(indexCapacity, indexStride);
	[loop]
	for (uint y = minimumTile.y; y <= maximumTile.y; ++y)
	{
		[loop]
		for (uint x = minimumTile.x; x <= maximumTile.x; ++x)
		{
			const uint tileIndex = SmokeAnalyticTileIndex(uint2(x, y), tileCount);
			if (tileIndex >= headerCapacity)
				continue;
			uint slot;
			InterlockedAdd(gSmokeAnalyticTileHeaders[tileIndex].Count, 1u, slot);
			if (slot < NRI_SMOKE_ANALYTIC_MAX_CARRIERS_PER_TILE)
			{
				const uint outputIndex = tileIndex * NRI_SMOKE_ANALYTIC_MAX_CARRIERS_PER_TILE + slot;
				if (outputIndex < indexCapacity)
					gSmokeAnalyticTileIndices[outputIndex] = carrierIndex;
				else
					InterlockedAdd(gSmokeAnalyticTileHeaders[tileIndex].Overflow, 1u);
			}
			else
			{
				InterlockedAdd(gSmokeAnalyticTileHeaders[tileIndex].Overflow, 1u);
			}
		}
	}
}
