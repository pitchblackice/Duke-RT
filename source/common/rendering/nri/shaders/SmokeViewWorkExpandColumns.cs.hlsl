#include "Include/SmokeViewWorkResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint dispatchThreadId : SV_DispatchThreadID)
{
	const uint columnIndex = dispatchThreadId.x;
	if (columnIndex >= SmokeViewColumnCount())
		return;
	const uint x = columnIndex % gViewConstants.FroxelWidth;
	const uint y = columnIndex / gViewConstants.FroxelWidth;
	const uint tileIndex = (y / NRI_SMOKE_VIEW_TILE_AXIS) * gViewConstants.TileCountX +
		(x / NRI_SMOKE_VIEW_TILE_AXIS);
	const uint2 mask = gViewTileMasks[tileIndex].Words;
	gViewColumnMasks[columnIndex].Words = mask;
	const uint count = SmokeViewMaskCountBits(mask);
	if (count != 0u)
		InterlockedAdd(gViewWorkControl[0].UniqueColumns, 1u);
	InterlockedAdd(gViewWorkControl[0].UniqueFroxels, count);
}
