#include "Include/SmokeViewWorkResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint dispatchThreadId : SV_DispatchThreadID)
{
	const uint column = dispatchThreadId.x;
	if (column >= SmokeViewColumnCount() || gViewWorkControl[0].Overflow != 0u)
		return;
	const uint2 mask = gViewColumnMasks[column].Words;
	uint destination = gViewColumnOffsets[column];
	const uint x = column % gViewConstants.FroxelWidth;
	const uint y = column / gViewConstants.FroxelWidth;
	[loop]
	for (uint z = 0u; z < gViewConstants.FroxelDepth; ++z)
	{
		if ((mask[z >> 5u] & (1u << (z & 31u))) == 0u)
			continue;
		if (destination >= gViewConstants.FroxelCapacity)
		{
			InterlockedAdd(gViewWorkControl[0].Overflow, 1u);
			return;
		}
		gViewCompactIndices[destination++] = (z * gViewConstants.FroxelHeight + y) *
			gViewConstants.FroxelWidth + x;
		InterlockedAdd(gViewWorkControl[0].ScatterWrites, 1u);
	}
}
