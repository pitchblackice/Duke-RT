#ifndef NRI_SMOKE_GRID_LIGHTING_RESOURCES_HLSLI
#define NRI_SMOKE_GRID_LIGHTING_RESOURCES_HLSLI

#include "SmokeResources.hlsli"

#ifndef NRI_SMOKE_GRID_LIGHT_WORLD_ENABLED
#define NRI_SMOKE_GRID_LIGHT_WORLD_ENABLED 0x200000u
#endif
#define NRI_SMOKE_GRID_LIGHT_WORLD_COMPARE 0x400000u
#define NRI_SMOKE_GRID_LIGHT_FILTER_ENABLED 0x800000u
#ifndef NRI_SMOKE_EMISSIVE_LEGACY_GATHER_DISABLED
#define NRI_SMOKE_EMISSIVE_LEGACY_GATHER_DISABLED 0x1000000u
#endif
#ifndef NRI_SMOKE_EMISSIVE_QUARTER_KEY
#define NRI_SMOKE_EMISSIVE_QUARTER_KEY 0x2000000u
#endif
#define NRI_SMOKE_GRID_LIGHT_FIELD_PING 0x4000000u
#define NRI_SMOKE_GRID_LIGHT_DEBUG_SHIFT 27u

static const int3 NRI_SMOKE_GRID_LIGHT_LOBE_AXES[6] = {
	int3(1, 0, 0), int3(-1, 0, 0), int3(0, 1, 0),
	int3(0, -1, 0), int3(0, 0, 1), int3(0, 0, -1)
};

bool SmokeGridLightLookupBrick(int3 coordinate, out uint brickIndex, out uint generation)
{
	brickIndex = 0xffffffffu;
	generation = 0u;
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
		if (brick.State != NRI_SMOKE_GRID_RESIDENT || brick.Generation != entry.Generation || !all(brick.Coordinate == coordinate))
			return false;
		brickIndex = entry.BrickIndex;
		generation = entry.Generation;
		return true;
	}
	return false;
}

bool SmokeGridLightCellAddress(int3 cell, out uint cellIndex, out uint generation)
{
	const int3 brick = SmokeGridBrickCoordinate(cell);
	uint brickIndex;
	if (!SmokeGridLightLookupBrick(brick, brickIndex, generation))
	{
		cellIndex = 0xffffffffu;
		return false;
	}
	cellIndex = brickIndex * NRI_SMOKE_GRID_CELLS_PER_BRICK +
		SmokeGridLocalIndex(SmokeGridLocalCoordinate(cell, brick));
	uint capacity, ignoredStride;
	gSmokeGridLightCurrent.GetDimensions(capacity, ignoredStride);
	return cellIndex < capacity;
}

float3 SmokeGridLightCellCenter(int3 cell, float cellSize)
{
	return ((float3)cell + 0.5) * cellSize;
}

void SmokeGridLightWorldCoordinates(float3 worldPosition, float cellSize, out int3 lower, out float3 blend)
{
	const float3 gridPosition = worldPosition / max(cellSize, 0.0001) - 0.5;
	lower = (int3)floor(gridPosition);
	blend = saturate(gridPosition - (float3)lower);
}

SmokeGridLightRecord SmokeGridLightLoadShadingRecord(uint cellIndex)
{
	if ((gSmokeConstants.Flags & NRI_SMOKE_GRID_LIGHT_FILTER_ENABLED) != 0u)
		return gSmokeGridLightFiltered[cellIndex];
	if ((gSmokeConstants.Flags & NRI_SMOKE_GRID_LIGHT_FIELD_PING) != 0u)
		return gSmokeGridLightHistory[cellIndex];
	return gSmokeGridLightCurrent[cellIndex];
}

bool SmokeGridLightFaceOpen(int3 cell, uint axis)
{
	uint cellIndex, generation;
	if (!SmokeGridLightCellAddress(cell, cellIndex, generation))
		return false;
	const uint4 link = gSmokeGridLightLinks[cellIndex];
	return link.y == generation && link.z == gSmokeConstants.SimulationEpoch && (link.x & (1u << (axis * 2u))) != 0u;
}

bool SmokeGridLightPathOpen(int3 start, uint3 order, uint axisCount, uint3 targetOffset)
{
	int3 cursor = start;
	[unroll]
	for (uint step = 0u; step < 3u; ++step)
	{
		if (step >= axisCount)
			break;
		const uint axis = order[step];
		if (targetOffset[axis] == 0u || !SmokeGridLightFaceOpen(cursor, axis))
			return false;
		cursor[axis] += 1;
	}
	return true;
}

bool SmokeGridLightCornerReachable(int3 lower, uint3 offset)
{
	const uint axisCount = offset.x + offset.y + offset.z;
	if (axisCount == 0u)
		return true;
	static const uint3 orders[6] = {
		uint3(0,1,2), uint3(0,2,1), uint3(1,0,2),
		uint3(1,2,0), uint3(2,0,1), uint3(2,1,0)
	};
	uint validOrders = 0u;
	[unroll]
	for (uint orderIndex = 0u; orderIndex < 6u; ++orderIndex)
	{
		uint3 compact = 0u;
		uint count = 0u;
		[unroll]
		for (uint position = 0u; position < 3u; ++position)
		{
			const uint axis = orders[orderIndex][position];
			if (offset[axis] != 0u)
				compact[count++] = axis;
		}
		bool duplicate = false;
		[unroll]
		for (uint prior = 0u; prior < orderIndex; ++prior)
		{
			uint3 previous = 0u;
			uint previousCount = 0u;
			[unroll]
			for (uint position2 = 0u; position2 < 3u; ++position2)
			{
				const uint axis2 = orders[prior][position2];
				if (offset[axis2] != 0u) previous[previousCount++] = axis2;
			}
			duplicate = duplicate || (previousCount == count && all(previous == compact));
		}
		if (duplicate)
			continue;
		validOrders++;
		if (!SmokeGridLightPathOpen(lower, compact, count, offset))
			return false;
	}
	return validOrders > 0u;
}

bool SmokeGridLightSample(float3 worldPosition, float cellSize, out float3 lobes[6], out float confidence)
{
	[unroll] for (uint lobe = 0u; lobe < 6u; ++lobe) lobes[lobe] = 0.0;
	confidence = 0.0;
	int3 lower;
	float3 blend;
	SmokeGridLightWorldCoordinates(worldPosition, cellSize, lower, blend);
	float weightSum = 0.0;
	[unroll]
	for (uint corner = 0u; corner < 8u; ++corner)
	{
		const uint3 offset = uint3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
		const int3 cell = lower + (int3)offset;
		uint cellIndex, generation;
		if (!SmokeGridLightCellAddress(cell, cellIndex, generation) || !SmokeGridLightCornerReachable(lower, offset))
			continue;
		const SmokeGridLightRecord record = SmokeGridLightLoadShadingRecord(cellIndex);
		if (!SmokeGridLightRecordValid(record, generation, gSmokeConstants.SimulationEpoch))
			continue;
		const float3 cornerWeight = lerp(1.0 - blend, blend, (float3)offset);
		const float weight = cornerWeight.x * cornerWeight.y * cornerWeight.z;
		[unroll] for (uint lobe = 0u; lobe < 6u; ++lobe) lobes[lobe] += SmokeGridLightMean(record, lobe) * weight;
		confidence += SmokeGridLightConfidence(record) * weight;
		weightSum += weight;
	}
	if (weightSum <= 0.0)
		return false;
	[unroll] for (uint lobe = 0u; lobe < 6u; ++lobe) lobes[lobe] /= weightSum;
	confidence /= weightSum;
	return true;
}

#endif
