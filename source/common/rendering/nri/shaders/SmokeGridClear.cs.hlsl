#include "Include/SmokeGridResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint index = dispatchThreadId.x;
	if (index == 0u)
	{
		SmokeGridControl control = (SmokeGridControl)0;
		control.FreeCount = gSmokeGridConstants.BrickCapacity;
		control.Generation = gSmokeGridConstants.SimulationEpoch;
		control.FrameStamp = gSmokeGridConstants.FrameIndex;
		control.BrickCapacity = gSmokeGridConstants.BrickCapacity;
		control.HashCapacity = gSmokeGridConstants.HashCapacity;
		control.CellCapacity = gSmokeGridConstants.CellCapacity;
		control.ActivePing = min(gSmokeGridConstants.ActivePing, 1u);
		control.FieldPing = min(gSmokeGridConstants.FieldPing, 1u);
		control.CellSizeBits = asuint(gSmokeGridConstants.CellSize);
		gSmokeGridControl[0] = control;
		gSmokeGridDispatch[0] = 0u; gSmokeGridDispatch[1] = 1u; gSmokeGridDispatch[2] = 1u;
		gSmokeGridDispatch[3] = 0u; gSmokeGridDispatch[4] = 1u; gSmokeGridDispatch[5] = 1u;
	}
	if (index < gSmokeGridConstants.HashCapacity)
	{
		SmokeGridHashEntry entry = (SmokeGridHashEntry)0;
		entry.BrickIndex = 0xffffffffu;
		gSmokeGridHash[index] = entry;
	}
	if (index < gSmokeGridConstants.BrickCapacity)
	{
		SmokeGridBrick brick = (SmokeGridBrick)0;
		brick.HashSlot = 0xffffffffu;
		gSmokeGridBricks[index] = brick;
		gSmokeGridFreeList[index] = gSmokeGridConstants.BrickCapacity - 1u - index;
		gSmokeGridActiveA[index] = 0xffffffffu;
		gSmokeGridActiveB[index] = 0xffffffffu;
	}
}
