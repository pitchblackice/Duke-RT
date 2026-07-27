#include "Include/SmokeGridResources.hlsli"

void SmokeGridCompactDrainedHash()
{
	if ((gSmokeGridConstants.Flags & NRI_SMOKE_GRID_FLAG_COMPACT_DRAINED_HASH) == 0u ||
		gSmokeGridControl[0].ResidentCount != 0u)
		return;

	const uint activeCount = min(SmokeGridActiveCount(), gSmokeGridConstants.BrickCapacity);
	bool hasNonEmptySlot = false;
	bool safeToReset = activeCount == 0u &&
		gSmokeGridControl[0].FreeCount == gSmokeGridConstants.BrickCapacity;
	[loop]
	for (uint slot = 0u; slot < gSmokeGridConstants.HashCapacity; ++slot)
	{
		const uint state = gSmokeGridHash[slot].State;
		hasNonEmptySlot = hasNonEmptySlot || state != NRI_SMOKE_GRID_EMPTY;
		safeToReset = safeToReset &&
			(state == NRI_SMOKE_GRID_EMPTY || state == NRI_SMOKE_GRID_TOMBSTONE);
	}

	if (!hasNonEmptySlot)
		return;

	gSmokeGridControl[0].HashRebuildAttempts++;
	if (!safeToReset)
	{
		gSmokeGridControl[0].HashRebuildFailures++;
		return;
	}

	[loop]
	for (uint slot = 0u; slot < gSmokeGridConstants.HashCapacity; ++slot)
	{
		SmokeGridHashEntry entry = (SmokeGridHashEntry)0;
		entry.BrickIndex = 0xffffffffu;
		gSmokeGridHash[slot] = entry;
	}
	DeviceMemoryBarrier();
	gSmokeGridControl[0].HashRebuildSuccesses++;
}

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	SmokeGridCompactDrainedHash();
	if ((gSmokeGridConstants.Flags & NRI_SMOKE_GRID_FLAG_HASH_HEALTH) != 0u)
	{
		gSmokeGridControl[0].HashEmpty = 0u;
		gSmokeGridControl[0].HashClaimed = 0u;
		gSmokeGridControl[0].HashResident = 0u;
		gSmokeGridControl[0].HashNew = 0u;
		gSmokeGridControl[0].HashTombstone = 0u;
		gSmokeGridControl[0].HashInvalidState = 0u;
		gSmokeGridControl[0].HashInvalidMapping = 0u;
		gSmokeGridControl[0].BorrowedResident = 0u;
		gSmokeGridControl[0].FirstUseCoreCapacity = SmokeGridFirstUseCoreCapacity();
		[loop]
		for (uint slot = 0u; slot < gSmokeGridConstants.HashCapacity; ++slot)
		{
			const SmokeGridHashEntry entry = gSmokeGridHash[slot];
			if (entry.State == NRI_SMOKE_GRID_EMPTY) gSmokeGridControl[0].HashEmpty++;
			else if (entry.State == NRI_SMOKE_GRID_CLAIMED) gSmokeGridControl[0].HashClaimed++;
			else if (entry.State == NRI_SMOKE_GRID_RESIDENT) gSmokeGridControl[0].HashResident++;
			else if (entry.State == NRI_SMOKE_GRID_NEW) gSmokeGridControl[0].HashNew++;
			else if (entry.State == NRI_SMOKE_GRID_TOMBSTONE) gSmokeGridControl[0].HashTombstone++;
			else gSmokeGridControl[0].HashInvalidState++;
			if (entry.State == NRI_SMOKE_GRID_RESIDENT || entry.State == NRI_SMOKE_GRID_NEW)
			{
				bool valid = entry.BrickIndex < gSmokeGridConstants.BrickCapacity;
				if (valid)
				{
					const SmokeGridBrick brick = gSmokeGridBricks[entry.BrickIndex];
					valid = brick.HashSlot == slot && brick.Generation == entry.Generation &&
						brick.State == entry.State && all(brick.Coordinate == entry.Coordinate);
				}
				if (!valid)
					gSmokeGridControl[0].HashInvalidMapping++;
				else if ((gSmokeGridBricks[entry.BrickIndex].Flags &
					NRI_SMOKE_GRID_BRICK_BORROWED_FIRST_USE) != 0u)
					gSmokeGridControl[0].BorrowedResident++;
			}
		}
	}
	const uint activeCount = min(SmokeGridActiveCount(), gSmokeGridConstants.BrickCapacity);
	gSmokeGridDispatch[0] = activeCount;
	gSmokeGridDispatch[1] = 1u;
	gSmokeGridDispatch[2] = 1u;
	gSmokeGridDispatch[3] = (activeCount + 63u) / 64u;
	gSmokeGridDispatch[4] = 1u;
	gSmokeGridDispatch[5] = 1u;
	gSmokeGridControl[0].BrickCapacity = gSmokeGridConstants.BrickCapacity;
	gSmokeGridControl[0].HashCapacity = gSmokeGridConstants.HashCapacity;
	gSmokeGridControl[0].CellCapacity = gSmokeGridConstants.CellCapacity;
	gSmokeGridControl[0].ActivePing = min(gSmokeGridConstants.ActivePing, 1u);
	gSmokeGridControl[0].FieldPing = min(gSmokeGridConstants.FieldPing, 1u);
	gSmokeGridControl[0].CellSizeBits = asuint(gSmokeGridConstants.CellSize);
}
