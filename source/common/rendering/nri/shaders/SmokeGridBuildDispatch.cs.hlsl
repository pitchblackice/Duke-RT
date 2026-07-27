#include "Include/SmokeGridResources.hlsli"

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if ((gSmokeGridConstants.Flags & 1u) != 0u)
	{
		gSmokeGridControl[0].HashEmpty = 0u;
		gSmokeGridControl[0].HashClaimed = 0u;
		gSmokeGridControl[0].HashResident = 0u;
		gSmokeGridControl[0].HashNew = 0u;
		gSmokeGridControl[0].HashTombstone = 0u;
		gSmokeGridControl[0].HashInvalidState = 0u;
		gSmokeGridControl[0].HashInvalidMapping = 0u;
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
				if (!valid) gSmokeGridControl[0].HashInvalidMapping++;
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
