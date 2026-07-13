#include "Include/SmokeGridResources.hlsli"

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x != 0u)
		return;
	const uint nextActivePing = 1u - min(gSmokeGridConstants.ActivePing, 1u);
	if (nextActivePing == 0u)
		gSmokeGridControl[0].ActiveCountA = 0u;
	else
		gSmokeGridControl[0].ActiveCountB = 0u;
	gSmokeGridControl[0].OccupiedBricks = 0u;
	gSmokeGridControl[0].EmptyBricks = 0u;
	gSmokeGridControl[0].FieldHashLo = 0u;
	gSmokeGridControl[0].FieldHashHi = 0u;
	gSmokeGridControl[0].FrameStamp = gSmokeGridConstants.FrameIndex;
}
