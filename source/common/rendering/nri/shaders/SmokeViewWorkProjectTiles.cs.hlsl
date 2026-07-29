#include "Include/SmokeViewWorkResources.hlsli"

groupshared uint gBrickContributes;
groupshared uint2 gTileMask;

uint SmokeViewDepthSlice(float viewDepth)
{
	const float normalized = saturate(viewDepth / max(gViewConstants.FroxelMaxDistance, 0.001));
	const float slice = pow(normalized, 1.0 / max(gViewConstants.DepthExponent, 0.001));
	return min((uint)(slice * gViewConstants.FroxelDepth), gViewConstants.FroxelDepth - 1u);
}

void SmokeViewProjectSphere(float3 center, float radius, uint2 tile,
	inout uint2 mask, inout uint attempted, inout uint duplicates)
{
	const float3 relative = center - gViewConstants.CameraPosition;
	const float viewX = dot(relative, gViewConstants.CameraRight);
	const float viewY = dot(relative, gViewConstants.CameraUp);
	const float viewZ = dot(relative, gViewConstants.CameraForward);
	if (viewZ + radius <= 0.0)
	{
		InterlockedAdd(gViewWorkControl[0].BehindCameraRejects, 1u);
		return;
	}

	InterlockedAdd(gViewWorkControl[0].ProjectedSpheres, 1u);
	const bool cameraInside = dot(relative, relative) <= radius * radius;
	const bool crossesNear = viewZ <= radius;
	float2 minimumUv = 0.0;
	float2 maximumUv = 1.0;
	if (!cameraInside && !crossesNear)
	{
		const float nearZ = max(viewZ - radius, 1e-4);
		const float minNdcX = (viewX - radius) / (nearZ * max(gViewConstants.TanHalfFovX, 1e-4));
		const float maxNdcX = (viewX + radius) / (nearZ * max(gViewConstants.TanHalfFovX, 1e-4));
		const float minNdcY = (viewY - radius) / (nearZ * max(gViewConstants.TanHalfFovY, 1e-4));
		const float maxNdcY = (viewY + radius) / (nearZ * max(gViewConstants.TanHalfFovY, 1e-4));
		minimumUv = float2(minNdcX * 0.5 + 0.5, 0.5 - maxNdcY * 0.5);
		maximumUv = float2(maxNdcX * 0.5 + 0.5, 0.5 - minNdcY * 0.5);
		if (maximumUv.x < 0.0 || minimumUv.x > 1.0 || maximumUv.y < 0.0 || minimumUv.y > 1.0)
		{
			InterlockedAdd(gViewWorkControl[0].OffscreenRejects, 1u);
			return;
		}
	}
	else if (cameraInside)
		InterlockedAdd(gViewWorkControl[0].CameraInsideSpans, 1u);
	else
		InterlockedAdd(gViewWorkControl[0].NearPlaneSpans, 1u);

	const float2 tileMinimum = float2(tile * NRI_SMOKE_VIEW_TILE_AXIS) /
		float2(gViewConstants.FroxelWidth, gViewConstants.FroxelHeight);
	const float2 tileMaximum = float2(min((tile + 1u) * NRI_SMOKE_VIEW_TILE_AXIS,
		uint2(gViewConstants.FroxelWidth, gViewConstants.FroxelHeight))) /
		float2(gViewConstants.FroxelWidth, gViewConstants.FroxelHeight);
	if (maximumUv.x < tileMinimum.x || minimumUv.x > tileMaximum.x ||
		maximumUv.y < tileMinimum.y || minimumUv.y > tileMaximum.y)
		return;

	const float minimumDepth = max(viewZ - radius, 0.0);
	const float maximumDepth = min(viewZ + radius, gViewConstants.FroxelMaxDistance);
	if (maximumDepth < minimumDepth)
		return;
	const uint firstSlice = SmokeViewDepthSlice(minimumDepth);
	const uint lastSlice = SmokeViewDepthSlice(maximumDepth);
	const uint2 span = SmokeViewDepthMask(firstSlice, lastSlice);
	const uint2 overlap = mask & span;
	const uint spanBits = SmokeViewMaskCountBits(span);
	attempted += spanBits;
	duplicates += SmokeViewMaskCountBits(overlap);
	mask |= span;
	InterlockedAdd(gViewWorkControl[0].ProjectedSpans, 1u);
}

[numthreads(64, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint lane : SV_GroupIndex)
{
	const uint tileIndex = groupId.x;
	if (tileIndex >= SmokeViewTileCount())
		return;
	if (lane == 0u)
		gTileMask = 0u;
	GroupMemoryBarrierWithGroupSync();

	const uint2 tile = uint2(tileIndex % gViewConstants.TileCountX,
		tileIndex / gViewConstants.TileCountX);
	uint attempted = 0u;
	uint duplicates = 0u;
	[loop]
	for (uint brickIndex = 0u; brickIndex < gViewConstants.BrickCapacity; ++brickIndex)
	{
		if (lane == 0u)
			gBrickContributes = 0u;
		GroupMemoryBarrierWithGroupSync();
		const SmokeGridBrick brick = gViewGridBricks[brickIndex];
		const bool resident = brick.State == NRI_SMOKE_GRID_RESIDENT;
		if (resident)
		{
			[loop]
			for (uint cellOffset = lane; cellOffset < NRI_SMOKE_GRID_CELLS_PER_BRICK; cellOffset += 64u)
			{
				const uint cellIndex = brickIndex * NRI_SMOKE_GRID_CELLS_PER_BRICK + cellOffset;
				const float4 optical = gViewConstants.FieldPing != 0u ?
					gViewGridOpticalB[cellIndex] : gViewGridOpticalA[cellIndex];
				if (any(abs(optical) > gViewConstants.OpticalThreshold))
					InterlockedOr(gBrickContributes, 1u);
			}
		}
		GroupMemoryBarrierWithGroupSync();
		if (lane == 0u && resident)
		{
			InterlockedAdd(gViewWorkControl[0].ResidentBrickTileTests, 1u);
			InterlockedAdd(gViewWorkControl[0].OpticalCellTests, NRI_SMOKE_GRID_CELLS_PER_BRICK);
			if (gBrickContributes != 0u)
			{
				InterlockedAdd(gViewWorkControl[0].ContributingBrickTilePairs, 1u);
				const float3 center = ((float3)brick.Coordinate * NRI_SMOKE_GRID_BRICK_AXIS +
					(NRI_SMOKE_GRID_BRICK_AXIS * 0.5)) * gViewConstants.CellSize;
				// One full cell of support on every side conservatively covers the
				// trilinear reconstruction footprint across neighboring bricks.
				const float radius = sqrt(3.0) * (NRI_SMOKE_GRID_BRICK_AXIS * 0.5 + 1.0) *
					gViewConstants.CellSize;
				SmokeViewProjectSphere(center, radius, tile, gTileMask, attempted, duplicates);
			}
			else
				InterlockedAdd(gViewWorkControl[0].EmptyBrickTilePairs, 1u);
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (lane == 0u)
	{
		gViewTileMasks[tileIndex].Words = gTileMask;
		InterlockedAdd(gViewWorkControl[0].AttemptedMarks, attempted);
		InterlockedAdd(gViewWorkControl[0].DuplicateMerges, duplicates);
	}
}
