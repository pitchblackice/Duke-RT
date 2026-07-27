#include "Include/SmokeViewWorkResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint dispatchThreadId : SV_DispatchThreadID)
{
	const uint column = dispatchThreadId.x;
	if (column >= SmokeViewColumnCount())
		return;
	gViewColumnOffsets[column] = SmokeViewMaskCountBits(gViewColumnMasks[column].Words);
}
