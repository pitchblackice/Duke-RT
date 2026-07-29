#include "Include/SmokeViewWorkResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint dispatchThreadId : SV_DispatchThreadID)
{
	const uint index = dispatchThreadId.x;
	if (index < SmokeViewTileCount())
		gViewTileMasks[index].Words = 0u;
	if (index < SmokeViewColumnCount())
	{
		gViewColumnMasks[index].Words = 0u;
		if (gViewConstants.ExecutionRoute == 2u)
			gViewColumnOffsets[index] = 0u;
	}
	if (index == 0u)
	{
		SmokeViewWorkControl control = (SmokeViewWorkControl)0;
		control.FrameStamp = gViewConstants.FrameIndex;
		control.SimulationEpoch = gViewConstants.SimulationEpoch;
		control.BrickTileTests = SmokeViewTileCount() * gViewConstants.BrickCapacity;
		control.CompactCapacity = gViewConstants.FroxelCapacity;
		gViewWorkControl[0] = control;
		if (gViewConstants.ExecutionRoute == 2u)
		{
			gViewIndirectArgs[0] = uint3(0u, 1u, 1u);
			gViewIndirectArgs[1] = uint3(0u, 1u, 1u);
		}
	}
}
