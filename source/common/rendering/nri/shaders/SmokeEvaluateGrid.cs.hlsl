#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"

#define NRI_SMOKE_GRID_MAX_FOOTPRINT_SAMPLES 2u
#define NRI_SMOKE_GRID_MAX_DEPTH_SAMPLES 8u

bool SmokeRenderGridLookup(int3 coordinate, out uint brickIndex)
{
	brickIndex = 0xffffffffu;
	uint hashCount, ignoredStride;
	gSmokeRenderGridHash.GetDimensions(hashCount, ignoredStride);
	if (hashCount == 0u || (hashCount & (hashCount - 1u)) != 0u)
		return false;
	const uint mask = hashCount - 1u;
	const uint base = SmokeGridHashCoordinate(coordinate) & mask;
	[loop]
	for (uint probe = 0u; probe < NRI_SMOKE_GRID_HASH_PROBES; ++probe)
	{
		const SmokeGridHashEntry entry = gSmokeRenderGridHash[(base + probe) & mask];
		if (entry.State == NRI_SMOKE_GRID_EMPTY)
			return false;
		if (entry.State == NRI_SMOKE_GRID_RESIDENT && all(entry.Coordinate == coordinate))
		{
			uint brickCount;
			gSmokeRenderGridBricks.GetDimensions(brickCount, ignoredStride);
			if (entry.BrickIndex < brickCount &&
				gSmokeRenderGridBricks[entry.BrickIndex].Generation == entry.Generation &&
				gSmokeRenderGridBricks[entry.BrickIndex].State == NRI_SMOKE_GRID_RESIDENT)
			{
				brickIndex = entry.BrickIndex;
				return true;
			}
		}
	}
	return false;
}

void SmokeRenderGridLoadCell(int3 cell, out float4 scalar, out float4 optical)
{
	scalar = 0.0;
	optical = 0.0;
	const int3 brickCoordinate = SmokeGridBrickCoordinate(cell);
	uint brickIndex;
	if (!SmokeRenderGridLookup(brickCoordinate, brickIndex))
		return;
	const uint cellIndex = brickIndex * NRI_SMOKE_GRID_CELLS_PER_BRICK +
		SmokeGridLocalIndex(SmokeGridLocalCoordinate(cell, brickCoordinate));
	const bool fieldB = gSmokeRenderGridControl[0].FieldPing != 0u;
	scalar = fieldB ? gSmokeRenderGridScalarB[cellIndex] : gSmokeRenderGridScalarA[cellIndex];
	optical = fieldB ? gSmokeRenderGridOpticalB[cellIndex] : gSmokeRenderGridOpticalA[cellIndex];
}

void SmokeRenderGridSample(float3 worldPosition, float cellSize, out float4 scalar, out float4 optical)
{
	const float3 gridPosition = worldPosition / cellSize - 0.5;
	const int3 lower = (int3)floor(gridPosition);
	const float3 blend = frac(gridPosition);
	float4 s[8];
	float4 o[8];
	[unroll]
	for (uint i = 0u; i < 8u; ++i)
	{
		const int3 offset = int3(i & 1u, (i >> 1u) & 1u, (i >> 2u) & 1u);
		SmokeRenderGridLoadCell(lower + offset, s[i], o[i]);
	}
	const float4 s00 = lerp(s[0], s[1], blend.x);
	const float4 s10 = lerp(s[2], s[3], blend.x);
	const float4 s01 = lerp(s[4], s[5], blend.x);
	const float4 s11 = lerp(s[6], s[7], blend.x);
	const float4 o00 = lerp(o[0], o[1], blend.x);
	const float4 o10 = lerp(o[2], o[3], blend.x);
	const float4 o01 = lerp(o[4], o[5], blend.x);
	const float4 o11 = lerp(o[6], o[7], blend.x);
	scalar = lerp(lerp(s00, s10, blend.y), lerp(s01, s11, blend.y), blend.z);
	optical = lerp(lerp(o00, o10, blend.y), lerp(o01, o11, blend.y), blend.z);
}

void SmokeRenderGridIntegrateFroxel(uint3 froxel, float cellSize, out float4 scalar, out float4 optical)
{
	const float sliceNearDepth = SmokeSliceNearDepth(froxel.z);
	const float sliceFarDepth = SmokeSliceFarDepth(froxel.z);
	const float sliceCenterDepth = (sliceNearDepth + sliceFarDepth) * 0.5;
	const float footprintWidth = 2.0 * sliceCenterDepth * gSmokeConstants.TanHalfFovX /
		max((float)gSmokeConstants.FroxelWidth, 1.0);
	const float footprintHeight = 2.0 * sliceCenterDepth * gSmokeConstants.TanHalfFovY /
		max((float)gSmokeConstants.FroxelHeight, 1.0);
	const uint2 footprintSampleCount = uint2(
		footprintWidth > cellSize ? NRI_SMOKE_GRID_MAX_FOOTPRINT_SAMPLES : 1u,
		footprintHeight > cellSize ? NRI_SMOKE_GRID_MAX_FOOTPRINT_SAMPLES : 1u);
	const float3 centerRay = SmokeFroxelRay(froxel.xy);
	const float segmentWorldLength = SmokeWorldSegmentLength(centerRay, sliceNearDepth, sliceFarDepth);
	const uint depthSampleCount = clamp((uint)ceil(segmentWorldLength / cellSize),
		1u, NRI_SMOKE_GRID_MAX_DEPTH_SAMPLES);
	float4 integratedScalar = 0.0;
	float4 integratedOptical = 0.0;
	[loop]
	for (uint footprintY = 0u; footprintY < footprintSampleCount.y; ++footprintY)
	{
		[loop]
		for (uint footprintX = 0u; footprintX < footprintSampleCount.x; ++footprintX)
		{
			const float2 footprintUnit = (float2(footprintX, footprintY) + 0.5) /
				(float2)footprintSampleCount;
			const float2 sampleUv = (float2(froxel.xy) + footprintUnit) /
				float2(max(gSmokeConstants.FroxelWidth, 1u), max(gSmokeConstants.FroxelHeight, 1u));
			[loop]
			for (uint depthSample = 0u; depthSample < depthSampleCount; ++depthSample)
			{
				const float sampleDepthUnit = ((float)depthSample + 0.5) / (float)depthSampleCount;
				const float sampleViewDepth = lerp(sliceNearDepth, sliceFarDepth, sampleDepthUnit);
				const float3 samplePosition = SmokeWorldPosition(sampleUv, sampleViewDepth);
				float4 sampleScalar;
				float4 sampleOptical;
				SmokeRenderGridSample(samplePosition, cellSize, sampleScalar, sampleOptical);
				integratedScalar += sampleScalar;
				integratedOptical += sampleOptical;
			}
		}
	}
	const float sampleWeight = rcp((float)(footprintSampleCount.x * footprintSampleCount.y * depthSampleCount));
	scalar = integratedScalar * sampleWeight;
	optical = integratedOptical * sampleWeight;
}

[numthreads(4, 4, 4)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gSmokeConstants.FroxelWidth ||
		dispatchThreadId.y >= gSmokeConstants.FroxelHeight ||
		dispatchThreadId.z >= gSmokeConstants.FroxelDepth)
		return;
	if ((gSmokeConstants.Flags & NRI_SMOKE_FLAG_COMPARE_REPRESENTATION) != 0u &&
		dispatchThreadId.x < gSmokeConstants.FroxelWidth / 2u)
		return;
	uint controlCount, ignoredStride;
	gSmokeRenderGridControl.GetDimensions(controlCount, ignoredStride);
	if (controlCount == 0u || gSmokeRenderGridControl[0].ResidentCount == 0u)
		return;
	const float cellSize = asfloat(gSmokeRenderGridControl[0].CellSizeBits);
	if (!isfinite(cellSize) || cellSize <= 0.0)
		return;
	const uint froxelIndex = SmokeFroxelIndex(dispatchThreadId.x, dispatchThreadId.y, dispatchThreadId.z);
	float4 scalar;
	float4 optical;
	SmokeRenderGridIntegrateFroxel(dispatchThreadId, cellSize, scalar, optical);
	// Deposition stores density-weighted sigma_t and sigma_s coefficients in
	// inverse world units. Cell size controls sampling support only; dividing the
	// coefficients by it again made the canonical eight-unit grid 8x too faint.
	const float extinction = max(scalar.z * gSmokeConstants.DensityScale, 0.0);
	const float3 scattering = max(optical.rgb * gSmokeConstants.DensityScale, 0.0);
	if (extinction <= 1e-6 || !any(scattering > 0.0))
		return;
	const float anisotropy = optical.w > 1e-6 ? clamp(scalar.w / optical.w, -0.95, 0.95) : 0.0;
	gSmokeFroxelMedium[froxelIndex] = float4(scattering, extinction);
	// The fourth phase lane identifies grid materialization to the shared direct
	// light passes. Particle evaluation retains the value 1.
	gSmokeFroxelPhase[froxelIndex] = float4(anisotropy, optical.w, 1.0, 2.0);
	gSmokeFroxelSource[froxelIndex] = 0.0;
	uint occupiedCapacity;
	gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, ignoredStride);
	uint occupiedSlot = 0u;
	InterlockedAdd(gSmokeControl[0].OccupiedCount, 1u, occupiedSlot);
	if (occupiedSlot < occupiedCapacity)
		gSmokeOccupiedFroxelIndices[occupiedSlot] = froxelIndex;
	else
		InterlockedAdd(gSmokeControl[0].OccupiedOverflow, 1u);
}
