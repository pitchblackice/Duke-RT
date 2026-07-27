#ifndef NRI_SMOKE_VIEW_WORK_RESOURCES_HLSLI
#define NRI_SMOKE_VIEW_WORK_RESOURCES_HLSLI

#include "NRI.hlsl"
#include "SmokeGridData.hlsli"
#include "SmokeViewWorkData.hlsli"

NRI_RESOURCE(RWStructuredBuffer<SmokeGridControl>, gViewGridControl, u, 0, 0);
NRI_RESOURCE(RWStructuredBuffer<SmokeGridBrick>, gViewGridBricks, u, 1, 0);
NRI_RESOURCE(RWStructuredBuffer<float4>, gViewGridOpticalA, u, 2, 0);
NRI_RESOURCE(RWStructuredBuffer<float4>, gViewGridOpticalB, u, 3, 0);
NRI_RESOURCE(RWStructuredBuffer<SmokeViewMask>, gViewTileMasks, u, 4, 0);
NRI_RESOURCE(RWStructuredBuffer<SmokeViewMask>, gViewColumnMasks, u, 5, 0);
NRI_RESOURCE(RWStructuredBuffer<SmokeViewWorkControl>, gViewWorkControl, u, 6, 0);
NRI_RESOURCE(RWStructuredBuffer<uint>, gViewCompactIndices, u, 7, 0);
NRI_RESOURCE(RWStructuredBuffer<uint3>, gViewIndirectArgs, u, 8, 0);
NRI_RESOURCE(RWStructuredBuffer<float4>, gViewDenseMedium, u, 9, 0);
NRI_RESOURCE(RWStructuredBuffer<float4>, gViewDenseSource, u, 10, 0);

NRI_ROOT_CONSTANTS(SmokeViewWorkConstants, gViewConstants, 0, 1);

uint SmokeViewTileCount()
{
	return gViewConstants.TileCountX * gViewConstants.TileCountY;
}

uint SmokeViewColumnCount()
{
	return gViewConstants.FroxelWidth * gViewConstants.FroxelHeight;
}

uint SmokeViewMaskCountBits(uint2 mask)
{
	return countbits(mask.x) + countbits(mask.y);
}

uint2 SmokeViewDepthMask(uint firstSlice, uint lastSlice)
{
	uint2 result = 0u;
	[loop]
	for (uint slice = firstSlice; slice <= lastSlice && slice < gViewConstants.FroxelDepth; ++slice)
		result[slice >> 5u] |= 1u << (slice & 31u);
	return result;
}

#endif
