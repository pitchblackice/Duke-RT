#include "Include/SmokeGridResources.hlsli"

// Correctness reference: one GPU thread snapshots the current active list and
// inserts all 26 neighbors. Newly appended halos are deliberately excluded
// from this pass so the halo cannot recursively expand.
[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x != 0u)
		return;
	const uint snapshotCount = min(SmokeGridActiveCount(), gSmokeGridConstants.BrickCapacity);
	[loop]
	for (uint activeIndex = 0u; activeIndex < snapshotCount; ++activeIndex)
	{
		const uint sourceIndex = SmokeGridActiveBrick(activeIndex);
		if (sourceIndex >= gSmokeGridConstants.BrickCapacity)
			continue;
		const SmokeGridBrick source = gSmokeGridBricks[sourceIndex];
		if (source.State != NRI_SMOKE_GRID_RESIDENT || (source.Flags & NRI_SMOKE_GRID_BRICK_CONTENT) == 0u)
			continue;
		[loop] for (int z = -1; z <= 1; ++z)
		[loop] for (int y = -1; y <= 1; ++y)
		[loop] for (int x = -1; x <= 1; ++x)
		{
			if (x == 0 && y == 0 && z == 0)
				continue;
			uint neighborIndex;
			bool newlyAllocated;
			if (SmokeGridFindOrAllocateBrickSerial(source.Coordinate + int3(x, y, z),
				NRI_SMOKE_GRID_BRICK_HALO, neighborIndex, newlyAllocated) && newlyAllocated)
			{
				gSmokeGridControl[0].HaloAllocations++;
			}
		}
	}
}
