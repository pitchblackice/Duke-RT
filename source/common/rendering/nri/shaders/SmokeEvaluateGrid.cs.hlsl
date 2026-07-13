#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"
#include "Include/SmokeLighting.hlsli"

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

float SmokeRenderGridDirectionalVisibility(float3 worldPosition, bool diagnostics)
{
	if (gSmokeConstants.LightMode == 0u ||
		(gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_DIRECTIONAL) == 0u)
		return 0.0;
	const bool castsShadow = (gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_DIRECTIONAL_SHADOW) != 0u;
	if (gSmokeConstants.LightMode < 2u || !castsShadow)
		return 1.0;
	if (!SmokeShadowTracingReady())
		return 0.0;
	const float3 centerDirection = SmokeDirectionalDirection();
	const uint sampleCount = gSmokeConstants.LightMode >= 3u ? clamp(gSmokeConstants.LightSamples, 1u, 4u) : 1u;
	float visibleSamples = 0.0;
	[loop]
	for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
	{
		uint randomState = SmokeDirectionalStableRandomSeed(sampleIndex,
			SmokeHash(asuint(worldPosition.x)) ^ SmokeHash(asuint(worldPosition.y)) ^ SmokeHash(asuint(worldPosition.z)) ^
			gSmokeConstants.DirectionalColorPacked);
		const float3 lightDirection = gSmokeConstants.LightMode >= 3u
			? SmokeSampleDirectionalCone(centerDirection, gSmokeConstants.DirectionalAngularSize, randomState)
			: centerDirection;
		if (diagnostics)
		{
			InterlockedAdd(gSmokeControl[0].DirectionalSamples, 1u);
			InterlockedAdd(gSmokeControl[0].DirectionalShadowRays, 1u);
		}
		const bool visible = SmokeFilteredVisibilityEffective()
			? SmokePointLightVisibleFiltered(worldPosition, lightDirection, 100000.0, diagnostics)
			: SmokePointLightVisible(worldPosition, lightDirection, 100000.0, diagnostics);
		if (visible)
		{
			visibleSamples += 1.0;
			if (diagnostics) InterlockedAdd(gSmokeControl[0].DirectionalShadowVisible, 1u);
		}
		else if (diagnostics)
			InterlockedAdd(gSmokeControl[0].DirectionalShadowOccluded, 1u);
	}
	return visibleSamples / (float)sampleCount;
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
	const float3 ray = SmokeFroxelRay(dispatchThreadId.xy);
	const float3 worldPosition = SmokeFroxelCenter(dispatchThreadId, ray);
	float4 scalar;
	float4 optical;
	SmokeRenderGridSample(worldPosition, cellSize, scalar, optical);
	// Deposition stores density-weighted sigma_t and sigma_s coefficients in
	// inverse world units. Cell size controls sampling support only; dividing the
	// coefficients by it again made the canonical eight-unit grid 8x too faint.
	const float extinction = max(scalar.z * gSmokeConstants.DensityScale, 0.0);
	const float3 scattering = max(optical.rgb * gSmokeConstants.DensityScale, 0.0);
	if (extinction <= 1e-6 || !any(scattering > 0.0))
		return;
	const float anisotropy = optical.w > 1e-6 ? clamp(scalar.w / optical.w, -0.95, 0.95) : 0.0;
	gSmokeFroxelMedium[froxelIndex] = float4(scattering, extinction);
	gSmokeFroxelPhase[froxelIndex] = float4(anisotropy, optical.w, 1.0, 1.0);
	gSmokeFroxelSource[froxelIndex] = 0.0;
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	const float visibility = SmokeRenderGridDirectionalVisibility(worldPosition, diagnostics);
	SmokeIndirectCacheRecord directional = (SmokeIndirectCacheRecord)0;
	directional.Radiance = scattering * visibility;
	directional.WorldPosition = worldPosition;
	directional.SigmaT = extinction;
	gSmokeIndirectScratch[froxelIndex] = directional;
	uint occupiedCapacity;
	gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, ignoredStride);
	uint occupiedSlot = 0u;
	InterlockedAdd(gSmokeControl[0].OccupiedCount, 1u, occupiedSlot);
	if (occupiedSlot < occupiedCapacity)
		gSmokeOccupiedFroxelIndices[occupiedSlot] = froxelIndex;
	else
		InterlockedAdd(gSmokeControl[0].OccupiedOverflow, 1u);
}
