#ifndef NRI_SMOKE_GRID_RESOURCES_HLSLI
#define NRI_SMOKE_GRID_RESOURCES_HLSLI

#include "NRI.hlsl"
#include "SmokeData.hlsli"
#include "SmokeGridData.hlsli"

StructuredBuffer<SmokeStyle> gSmokeGridStyles : register(t0, space0);
StructuredBuffer<SmokeInjectionCommand> gSmokeGridCommands : register(t1, space0);

RWStructuredBuffer<SmokeGridControl> gSmokeGridControl : register(u0, space1);
RWStructuredBuffer<SmokeGridHashEntry> gSmokeGridHash : register(u1, space1);
RWStructuredBuffer<SmokeGridBrick> gSmokeGridBricks : register(u2, space1);
RWStructuredBuffer<uint> gSmokeGridFreeList : register(u3, space1);
RWStructuredBuffer<uint> gSmokeGridActiveA : register(u4, space1);
RWStructuredBuffer<uint> gSmokeGridActiveB : register(u5, space1);
RWStructuredBuffer<uint> gSmokeGridDispatch : register(u6, space1);
RWStructuredBuffer<float4> gSmokeGridScalarA : register(u7, space1);
RWStructuredBuffer<float4> gSmokeGridScalarB : register(u8, space1);
RWStructuredBuffer<float4> gSmokeGridVelocityA : register(u9, space1);
RWStructuredBuffer<float4> gSmokeGridVelocityB : register(u10, space1);
RWStructuredBuffer<float4> gSmokeGridOpticalA : register(u11, space1);
RWStructuredBuffer<float4> gSmokeGridOpticalB : register(u12, space1);
RWStructuredBuffer<float4> gSmokeGridDynamicsA : register(u13, space1);
RWStructuredBuffer<float4> gSmokeGridDynamicsB : register(u14, space1);
RWStructuredBuffer<int4> gSmokeGridDeposit0 : register(u15, space1);
RWStructuredBuffer<int4> gSmokeGridDeposit1 : register(u16, space1);
RWStructuredBuffer<int4> gSmokeGridDeposit2 : register(u17, space1);
RWStructuredBuffer<int4> gSmokeGridDeposit3 : register(u18, space1);

NRI_ROOT_CONSTANTS(SmokeGridConstants, gSmokeGridConstants, 0, 2);

uint SmokeGridActiveCount()
{
	return gSmokeGridConstants.ActivePing == 0u ? gSmokeGridControl[0].ActiveCountA : gSmokeGridControl[0].ActiveCountB;
}

uint SmokeGridActiveBrick(uint index)
{
	if (gSmokeGridConstants.ActivePing == 0u)
		return gSmokeGridActiveA[index];
	return gSmokeGridActiveB[index];
}

uint SmokeGridActiveCountForPing(uint ping)
{
	return ping == 0u ? gSmokeGridControl[0].ActiveCountA : gSmokeGridControl[0].ActiveCountB;
}

bool SmokeGridTryAppendActive(uint ping, uint brickIndex)
{
	uint destination = 0u;
	if (ping == 0u)
		InterlockedAdd(gSmokeGridControl[0].ActiveCountA, 1u, destination);
	else
		InterlockedAdd(gSmokeGridControl[0].ActiveCountB, 1u, destination);
	if (destination >= gSmokeGridConstants.BrickCapacity)
	{
		uint ignored = 0u;
		if (ping == 0u)
			InterlockedMin(gSmokeGridControl[0].ActiveCountA, gSmokeGridConstants.BrickCapacity, ignored);
		else
			InterlockedMin(gSmokeGridControl[0].ActiveCountB, gSmokeGridConstants.BrickCapacity, ignored);
		return false;
	}
	if (ping == 0u)
		gSmokeGridActiveA[destination] = brickIndex;
	else
		gSmokeGridActiveB[destination] = brickIndex;
	return true;
}

bool SmokeGridAppendActive(uint brickIndex)
{
	return SmokeGridTryAppendActive(gSmokeGridConstants.ActivePing, brickIndex);
}

bool SmokeGridAppendNextActive(uint brickIndex)
{
	return SmokeGridTryAppendActive(1u - min(gSmokeGridConstants.ActivePing, 1u), brickIndex);
}

bool SmokeGridValidateEntry(SmokeGridHashEntry entry, int3 coordinate, bool acceptNew, out uint brickIndex)
{
	brickIndex = 0xffffffffu;
	if (!all(entry.Coordinate == coordinate) || entry.BrickIndex >= gSmokeGridConstants.BrickCapacity)
		return false;
	const SmokeGridBrick brick = gSmokeGridBricks[entry.BrickIndex];
	const bool stateValid = brick.State == NRI_SMOKE_GRID_RESIDENT || (acceptNew && brick.State == NRI_SMOKE_GRID_NEW);
	if (!stateValid || brick.Generation != entry.Generation || !all(brick.Coordinate == coordinate))
		return false;
	brickIndex = entry.BrickIndex;
	return true;
}

// Sampling readers accept only fully-cleared, published resident bricks. NEW,
// CLAIMED, and TOMBSTONE entries keep probing but never expose field data.
bool SmokeGridLookupBrick(int3 coordinate, out uint brickIndex)
{
	brickIndex = 0xffffffffu;
	if (gSmokeGridConstants.HashCapacity == 0u)
		return false;
	const uint mask = gSmokeGridConstants.HashCapacity - 1u;
	const uint base = SmokeGridHashCoordinate(coordinate) & mask;
	const uint probeLimit = min(NRI_SMOKE_GRID_HASH_PROBES, gSmokeGridConstants.HashCapacity);
	[loop]
	for (uint probe = 0u; probe < probeLimit; ++probe)
	{
		const uint slot = (base + probe) & mask;
		const SmokeGridHashEntry entry = gSmokeGridHash[slot];
		if (entry.State == NRI_SMOKE_GRID_EMPTY)
			return false;
		if (entry.State == NRI_SMOKE_GRID_RESIDENT && SmokeGridValidateEntry(entry, coordinate, false, brickIndex))
			return true;
	}
	return false;
}

bool SmokeGridPopFreeSerial(out uint brickIndex)
{
	brickIndex = 0xffffffffu;
	const uint freeCount = min(gSmokeGridControl[0].FreeCount, gSmokeGridConstants.BrickCapacity);
	if (freeCount == 0u)
		return false;
	gSmokeGridControl[0].FreeCount = freeCount - 1u;
	brickIndex = gSmokeGridFreeList[freeCount - 1u];
	return brickIndex < gSmokeGridConstants.BrickCapacity;
}

bool SmokeGridPushFree(uint brickIndex)
{
	uint destination = 0u;
	InterlockedAdd(gSmokeGridControl[0].FreeCount, 1u, destination);
	if (destination >= gSmokeGridConstants.BrickCapacity)
	{
		uint ignored = 0u;
		InterlockedMin(gSmokeGridControl[0].FreeCount, gSmokeGridConstants.BrickCapacity, ignored);
		return false;
	}
	gSmokeGridFreeList[destination] = brickIndex;
	return true;
}

bool SmokeGridAllocateAtSlotSerial(int3 coordinate, uint flags, uint slot, uint probe, out uint brickIndex)
{
	brickIndex = 0xffffffffu;
	if (!SmokeGridPopFreeSerial(brickIndex))
	{
		InterlockedAdd(gSmokeGridControl[0].AllocationFailures, 1u);
		return false;
	}

	uint generation = gSmokeGridBricks[brickIndex].Generation + 1u;
	if (generation == 0u)
		generation = 1u;
	gSmokeGridHash[slot].State = NRI_SMOKE_GRID_CLAIMED;
	SmokeGridBrick brick = (SmokeGridBrick)0;
	brick.Coordinate = coordinate;
	brick.HashSlot = slot;
	brick.Generation = generation;
	brick.State = NRI_SMOKE_GRID_NEW;
	brick.Flags = flags;
	gSmokeGridBricks[brickIndex] = brick;
	gSmokeGridHash[slot].Coordinate = coordinate;
	gSmokeGridHash[slot].BrickIndex = brickIndex;
	gSmokeGridHash[slot].Generation = generation;
	DeviceMemoryBarrier();
	gSmokeGridHash[slot].State = NRI_SMOKE_GRID_NEW;
	if (!SmokeGridAppendActive(brickIndex))
	{
		gSmokeGridHash[slot].State = NRI_SMOKE_GRID_TOMBSTONE;
		gSmokeGridBricks[brickIndex].State = NRI_SMOKE_GRID_EMPTY;
		SmokeGridPushFree(brickIndex);
		InterlockedAdd(gSmokeGridControl[0].AllocationFailures, 1u);
		brickIndex = 0xffffffffu;
		return false;
	}
	InterlockedAdd(gSmokeGridControl[0].ResidentCount, 1u);
	InterlockedAdd(gSmokeGridControl[0].Allocated, 1u);
	InterlockedMax(gSmokeGridControl[0].MaximumProbe, probe + 1u);
	return true;
}

// Allocation passes are deliberately single-threaded. That makes NEW-key
// publication deterministic and avoids cross-threadgroup spin or duplicate
// insertion while retaining parallel preparation/deposition/simulation.
bool SmokeGridFindOrAllocateBrickSerial(int3 coordinate, uint flags, out uint brickIndex, out bool newlyAllocated)
{
	brickIndex = 0xffffffffu;
	newlyAllocated = false;
	if (gSmokeGridConstants.HashCapacity == 0u)
		return false;
	const uint mask = gSmokeGridConstants.HashCapacity - 1u;
	const uint base = SmokeGridHashCoordinate(coordinate) & mask;
	const uint probeLimit = min(NRI_SMOKE_GRID_HASH_PROBES, gSmokeGridConstants.HashCapacity);
	uint firstTombstone = 0xffffffffu;
	uint firstTombstoneProbe = 0u;
	[loop]
	for (uint probe = 0u; probe < probeLimit; ++probe)
	{
		const uint slot = (base + probe) & mask;
		const SmokeGridHashEntry entry = gSmokeGridHash[slot];
		if ((entry.State == NRI_SMOKE_GRID_RESIDENT || entry.State == NRI_SMOKE_GRID_NEW) && all(entry.Coordinate == coordinate))
		{
			if (SmokeGridValidateEntry(entry, coordinate, true, brickIndex))
			{
				gSmokeGridBricks[brickIndex].Flags |= flags;
				gSmokeGridBricks[brickIndex].IdleFrames = 0u;
				InterlockedMax(gSmokeGridControl[0].MaximumProbe, probe + 1u);
				return true;
			}
			gSmokeGridHash[slot].State = NRI_SMOKE_GRID_TOMBSTONE;
			if (firstTombstone == 0xffffffffu)
			{
				firstTombstone = slot;
				firstTombstoneProbe = probe;
			}
		}
		else if (entry.State == NRI_SMOKE_GRID_TOMBSTONE && firstTombstone == 0xffffffffu)
		{
			firstTombstone = slot;
			firstTombstoneProbe = probe;
		}
		if (entry.State == NRI_SMOKE_GRID_EMPTY)
		{
			const uint destination = firstTombstone != 0xffffffffu ? firstTombstone : slot;
			const uint destinationProbe = firstTombstone != 0xffffffffu ? firstTombstoneProbe : probe;
			newlyAllocated = SmokeGridAllocateAtSlotSerial(coordinate, flags, destination, destinationProbe, brickIndex);
			return newlyAllocated;
		}
	}
	if (firstTombstone != 0xffffffffu)
	{
		newlyAllocated = SmokeGridAllocateAtSlotSerial(coordinate, flags, firstTombstone, firstTombstoneProbe, brickIndex);
		return newlyAllocated;
	}
	InterlockedAdd(gSmokeGridControl[0].ProbeFailures, 1u);
	return false;
}

uint SmokeGridCellIndex(uint brickIndex, uint3 local)
{
	return brickIndex * NRI_SMOKE_GRID_CELLS_PER_BRICK + SmokeGridLocalIndex(local);
}

float4 SmokeGridLoadScalar(uint ping, uint index)
{
	if (ping == 0u) return gSmokeGridScalarA[index];
	return gSmokeGridScalarB[index];
}

float4 SmokeGridLoadVelocity(uint ping, uint index)
{
	if (ping == 0u) return gSmokeGridVelocityA[index];
	return gSmokeGridVelocityB[index];
}

float4 SmokeGridLoadOptical(uint ping, uint index)
{
	if (ping == 0u) return gSmokeGridOpticalA[index];
	return gSmokeGridOpticalB[index];
}

float4 SmokeGridLoadDynamics(uint ping, uint index)
{
	if (ping == 0u) return gSmokeGridDynamicsA[index];
	return gSmokeGridDynamicsB[index];
}

void SmokeGridStoreScalar(uint ping, uint index, float4 value)
{
	if (ping == 0u) gSmokeGridScalarA[index] = value;
	else gSmokeGridScalarB[index] = value;
}

void SmokeGridStoreVelocity(uint ping, uint index, float4 value)
{
	if (ping == 0u) gSmokeGridVelocityA[index] = value;
	else gSmokeGridVelocityB[index] = value;
}

void SmokeGridStoreOptical(uint ping, uint index, float4 value)
{
	if (ping == 0u) gSmokeGridOpticalA[index] = value;
	else gSmokeGridOpticalB[index] = value;
}

void SmokeGridStoreDynamics(uint ping, uint index, float4 value)
{
	if (ping == 0u) gSmokeGridDynamicsA[index] = value;
	else gSmokeGridDynamicsB[index] = value;
}

void SmokeGridSampleFields(float3 worldPosition, uint fieldPing,
	out float4 scalar, out float4 velocity, out float4 optical, out float4 dynamics)
{
	scalar = 0.0;
	velocity = 0.0;
	optical = 0.0;
	dynamics = 0.0;
	const float cellSize = max(gSmokeGridConstants.CellSize, 0.0001);
	const float3 gridPosition = worldPosition / cellSize - 0.5;
	const int3 lowerCell = (int3)floor(gridPosition);
	const float3 blend = saturate(gridPosition - (float3)lowerCell);
	[unroll]
	for (uint corner = 0u; corner < 8u; ++corner)
	{
		const uint3 offset = uint3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
		const int3 cell = lowerCell + (int3)offset;
		const int3 brickCoordinate = SmokeGridBrickCoordinate(cell);
		const float3 cornerWeight = lerp(1.0 - blend, blend, (float3)offset);
		const float weight = cornerWeight.x * cornerWeight.y * cornerWeight.z;
		float4 cornerScalar = 0.0;
		float4 cornerVelocity = float4(gSmokeGridConstants.Wind, 0.0);
		float4 cornerOptical = 0.0;
		float4 cornerDynamics = 0.0;
		uint brickIndex;
		if (SmokeGridLookupBrick(brickCoordinate, brickIndex))
		{
			const uint cellIndex = SmokeGridCellIndex(brickIndex, SmokeGridLocalCoordinate(cell, brickCoordinate));
			if (cellIndex < gSmokeGridConstants.CellCapacity)
			{
				cornerScalar = SmokeGridLoadScalar(fieldPing, cellIndex);
				cornerVelocity = SmokeGridLoadVelocity(fieldPing, cellIndex);
				cornerOptical = SmokeGridLoadOptical(fieldPing, cellIndex);
				cornerDynamics = SmokeGridLoadDynamics(fieldPing, cellIndex);
			}
		}
		scalar += cornerScalar * weight;
		velocity += cornerVelocity * weight;
		optical += cornerOptical * weight;
		dynamics += cornerDynamics * weight;
	}
}

float3 SmokeGridBacktrace(float3 worldPosition, float3 velocity, float deltaTime, out bool cflEvent, out bool clampEvent)
{
	float3 displacement = velocity * max(deltaTime, 0.0);
	const float distance = length(displacement);
	cflEvent = distance > max(gSmokeGridConstants.CellSize, 0.0001);
	const float maximum = min(max(gSmokeGridConstants.MaxBacktrace, 0.0),
		max(gSmokeGridConstants.CellSize, 0.0001) * (float)NRI_SMOKE_GRID_BRICK_AXIS);
	clampEvent = distance > maximum && distance > 1e-8;
	if (clampEvent)
		displacement *= maximum / distance;
	return worldPosition - displacement;
}

#endif
