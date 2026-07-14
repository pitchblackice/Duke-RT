#ifndef NRI_SMOKE_GRID_TRANSMITTANCE_HLSLI
#define NRI_SMOKE_GRID_TRANSMITTANCE_HLSLI

#include "SmokeResources.hlsli"

#define NRI_SMOKE_GRID_TRANSMITTANCE_MAX_STEPS 64u

uint SmokeGridTransmittanceStepBudget()
{
	const uint quality = min((gSmokeConstants.Flags >> 5u) & 3u, 2u);
	return 16u << quality;
}

bool SmokeGridTransmittanceLookupBrick(int3 coordinate, out uint brickIndex)
{
	brickIndex = 0xffffffffu;
	uint hashCount, ignoredStride;
	gSmokeRenderGridHash.GetDimensions(hashCount, ignoredStride);
	if (hashCount == 0u || (hashCount & (hashCount - 1u)) != 0u)
		return false;
	const uint mask = hashCount - 1u;
	const uint base = SmokeGridHashCoordinate(coordinate) & mask;
	[loop]
	for (uint probe = 0u; probe < NRI_SMOKE_GRID_HASH_PROBES; ++probe)
	{
		const SmokeGridHashEntry entry = gSmokeRenderGridHash[(base + probe) & mask];
		if (entry.State == NRI_SMOKE_GRID_EMPTY)
			return false;
		if (entry.State != NRI_SMOKE_GRID_RESIDENT || !all(entry.Coordinate == coordinate))
			continue;
		uint brickCount;
		gSmokeRenderGridBricks.GetDimensions(brickCount, ignoredStride);
		if (entry.BrickIndex >= brickCount)
			return false;
		const SmokeGridBrick brick = gSmokeRenderGridBricks[entry.BrickIndex];
		if (brick.State != NRI_SMOKE_GRID_RESIDENT || brick.Generation != entry.Generation ||
			!all(brick.Coordinate == coordinate))
			return false;
		brickIndex = entry.BrickIndex;
		return true;
	}
	return false;
}

float SmokeGridTransmittanceCellSigmaT(int3 cell)
{
	const int3 brickCoordinate = SmokeGridBrickCoordinate(cell);
	uint brickIndex;
	if (!SmokeGridTransmittanceLookupBrick(brickCoordinate, brickIndex))
		return 0.0;
	const uint cellIndex = brickIndex * NRI_SMOKE_GRID_CELLS_PER_BRICK +
		SmokeGridLocalIndex(SmokeGridLocalCoordinate(cell, brickCoordinate));
	uint scalarCapacity, ignoredStride;
	gSmokeRenderGridScalarA.GetDimensions(scalarCapacity, ignoredStride);
	if (cellIndex >= scalarCapacity)
		return 0.0;
	const bool fieldB = gSmokeRenderGridControl[0].FieldPing != 0u;
	const float4 scalar = fieldB ? gSmokeRenderGridScalarB[cellIndex] : gSmokeRenderGridScalarA[cellIndex];
	return isfinite(scalar.z) ? max(scalar.z * gSmokeConstants.DensityScale, 0.0) : 0.0;
}

float SmokeGridTransmittanceSampleSigmaT(float3 worldPosition, float cellSize)
{
	const float3 gridPosition = worldPosition / cellSize - 0.5;
	const int3 lower = (int3)floor(gridPosition);
	const float3 blend = saturate(gridPosition - (float3)lower);
	float sigma[8];
	[unroll]
	for (uint corner = 0u; corner < 8u; ++corner)
	{
		const int3 offset = int3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
		sigma[corner] = SmokeGridTransmittanceCellSigmaT(lower + offset);
	}
	const float s00 = lerp(sigma[0], sigma[1], blend.x);
	const float s10 = lerp(sigma[2], sigma[3], blend.x);
	const float s01 = lerp(sigma[4], sigma[5], blend.x);
	const float s11 = lerp(sigma[6], sigma[7], blend.x);
	return max(lerp(lerp(s00, s10, blend.y), lerp(s01, s11, blend.y), blend.z), 0.0);
}

float SmokeGridMediumTransmittance(float3 receiverPosition, float3 lightDirection,
	float lightDistance, out uint traversedSteps, out bool truncated)
{
	traversedSteps = 0u;
	truncated = false;
	if (!SmokeSelfShadowEnabled(gSmokeConstants.DebugMode) || lightDistance <= 1e-4)
		return 1.0;
	uint controlCount, ignoredStride;
	gSmokeRenderGridControl.GetDimensions(controlCount, ignoredStride);
	if (controlCount == 0u)
		return 1.0;
	const float cellSize = asfloat(gSmokeRenderGridControl[0].CellSizeBits);
	if (!isfinite(cellSize) || cellSize <= 1e-4)
		return 1.0;
	const float directionLength = length(lightDirection);
	if (directionLength <= 1e-6)
		return 1.0;
	const float3 direction = lightDirection / directionLength;
	const float distance = max(lightDistance, 0.0);
	const uint stepBudget = SmokeGridTransmittanceStepBudget();
	const uint idealSteps = max((uint)ceil(distance / cellSize), 1u);
	const uint sampleCount = min(idealSteps, stepBudget);
	const float segmentLength = distance / (float)sampleCount;
	truncated = idealSteps > stepBudget;
	float opticalDepth = 0.0;
	[loop]
	for (uint sampleIndex = 0u; sampleIndex < NRI_SMOKE_GRID_TRANSMITTANCE_MAX_STEPS && sampleIndex < sampleCount; ++sampleIndex)
	{
		const float sampleT = ((float)sampleIndex + 0.5) * segmentLength;
		const float3 position = receiverPosition + direction * sampleT;
		opticalDepth += SmokeGridTransmittanceSampleSigmaT(position, cellSize) * segmentLength;
		traversedSteps++;
		if (opticalDepth >= 20.0)
			return 0.0;
	}
	return exp(-min(opticalDepth, 20.0));
}

float SmokeGridDirectionalTransmittance(float3 receiverPosition, float3 lightDirection,
	out uint traversedSteps, out bool truncated)
{
	uint controlCount, ignoredStride;
	gSmokeRenderGridControl.GetDimensions(controlCount, ignoredStride);
	if (controlCount == 0u)
	{
		traversedSteps = 0u;
		truncated = false;
		return 1.0;
	}
	const float cellSize = max(asfloat(gSmokeRenderGridControl[0].CellSizeBits), 0.0001);
	const float boundedDistance = min(gSmokeConstants.FroxelMaxDistance,
		cellSize * (float)SmokeGridTransmittanceStepBudget());
	return SmokeGridMediumTransmittance(receiverPosition, lightDirection, boundedDistance, traversedSteps, truncated);
}

#endif
