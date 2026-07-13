#include "Include/SmokeGridResources.hlsli"

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
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
