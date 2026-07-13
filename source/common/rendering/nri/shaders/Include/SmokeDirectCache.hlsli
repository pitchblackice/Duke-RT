#ifndef NRI_SMOKE_DIRECT_CACHE_HLSLI
#define NRI_SMOKE_DIRECT_CACHE_HLSLI

#include "SmokeResources.hlsli"
#include "SmokeFroxel.hlsli"
#include "SmokeIndirectCache.hlsli"

#define NRI_SMOKE_DIRECT_METADATA_AGE_MASK 0xffu
#define NRI_SMOKE_DIRECT_METADATA_FRAME_SHIFT 8u
#define NRI_SMOKE_DIRECT_METADATA_VISIBILITY_SHIFT 16u
#define NRI_SMOKE_DIRECT_METADATA_VALID 0x80000000u

bool SmokeDirectGridEnabled()
{
	return (gSmokeConstants.Flags & NRI_SMOKE_DIRECT_GRID_ENABLED) != 0u;
}

bool SmokeDirectFroxelIsGrid(float4 phase)
{
	return SmokeDirectGridEnabled() && phase.w > 1.5;
}

uint SmokeDirectReceiverSampleCount()
{
	const uint referenceMode = (gSmokeConstants.Flags >> NRI_SMOKE_DIRECT_REFERENCE_SHIFT) & NRI_SMOKE_DIRECT_REFERENCE_MASK;
	if (referenceMode == 1u)
		return 16u;
	if (referenceMode >= 2u)
		return 32u;
	const uint quality = min((gSmokeConstants.Flags >> 5u) & 3u, 2u);
	const uint qualityMinimum = 1u << quality;
	const uint requested = max(clamp(gSmokeConstants.LightSamples, 1u, 4u), qualityMinimum);
	return requested <= 1u ? 1u : (requested == 2u ? 2u : 4u);
}

uint SmokeDirectReuseMode()
{
	return (gSmokeConstants.Flags >> NRI_SMOKE_DIRECT_REUSE_SHIFT) & NRI_SMOKE_DIRECT_REUSE_MASK;
}

float SmokeDirectRadicalInverse(uint value)
{
	value = (value << 16u) | (value >> 16u);
	value = ((value & 0x55555555u) << 1u) | ((value & 0xaaaaaaaau) >> 1u);
	value = ((value & 0x33333333u) << 2u) | ((value & 0xccccccccu) >> 2u);
	value = ((value & 0x0f0f0f0fu) << 4u) | ((value & 0xf0f0f0f0u) >> 4u);
	value = ((value & 0x00ff00ffu) << 8u) | ((value & 0xff00ff00u) >> 8u);
	return (float)value * 2.3283064365386963e-10;
}

uint SmokeDirectWorldKey(float3 worldPosition)
{
	float cellSize = 1.0;
	uint controlCount, ignoredStride;
	gSmokeRenderGridControl.GetDimensions(controlCount, ignoredStride);
	if (controlCount != 0u)
	{
		const float candidate = asfloat(gSmokeRenderGridControl[0].CellSizeBits);
		if (isfinite(candidate) && candidate > 0.0)
			cellSize = candidate;
	}
	const int3 cell = (int3)floor(worldPosition / cellSize);
	uint key = SmokeHash(asuint(cell.x));
	key ^= SmokeHash(asuint(cell.y) + 0x9e3779b9u);
	key ^= SmokeHash(asuint(cell.z) + 0x85ebca6bu);
	return SmokeHash(key ^ SmokeHash(gSmokeConstants.SimulationEpoch));
}

float3 SmokeDirectReceiverUnitSample(uint sampleIndex, uint sampleCount, uint worldKey)
{
	if (sampleCount <= 1u)
		return 0.5;
	const uint sequenceIndex = sampleIndex + 1u;
	const float rotationX = (float)(SmokeHash(worldKey ^ 0x68bc21ebu) & 0xffffu) / 65536.0;
	const float rotationY = (float)(SmokeHash(worldKey ^ 0x02e5be93u) & 0xffffu) / 65536.0;
	const float x = frac(((float)sampleIndex + 0.5) / (float)sampleCount + rotationX);
	const float y = frac(SmokeDirectRadicalInverse(sequenceIndex) + rotationY);
	// A coprime permutation prevents adjacent lanes from occupying adjacent
	// depth strata while still covering the complete nonlinear slice.
	const uint depthIndex = (sampleIndex * 13u + (worldKey & 31u)) % sampleCount;
	const float z = ((float)depthIndex + 0.5) / (float)sampleCount;
	return float3(x, y, z);
}

float3 SmokeDirectReceiverPosition(uint3 froxel, uint sampleIndex, uint sampleCount)
{
	const float3 ray = SmokeFroxelRay(froxel.xy);
	const float3 center = SmokeFroxelCenter(froxel, ray);
	const uint worldKey = SmokeDirectWorldKey(center);
	const float3 unitSample = SmokeDirectReceiverUnitSample(sampleIndex, sampleCount, worldKey);
	const float2 stableUv = (float2(froxel.xy) + unitSample.xy) /
		float2(max(gSmokeConstants.FroxelWidth, 1u), max(gSmokeConstants.FroxelHeight, 1u));
	const float depth = lerp(SmokeSliceNearDepth(froxel.z), SmokeSliceFarDepth(froxel.z), unitSample.z);
	return SmokeWorldPosition(stableUv, depth);
}

uint SmokeDirectRandomSeed(float3 receiverPosition, uint stableKeyLo, uint stableKeyHi, uint sampleIndex)
{
	uint seed = SmokeDirectWorldKey(receiverPosition);
	seed ^= SmokeHash(stableKeyLo ^ SmokeHash(stableKeyHi + 0x165667b1u));
	seed ^= SmokeHash(sampleIndex + 1u);
	if (gSmokeConstants.LightMode >= 3u)
		seed ^= SmokeHash(gSmokeConstants.FrameIndex + 0xc2b2ae35u);
	return SmokeHash(seed);
}

uint SmokeDirectDirectionalKey()
{
	uint key = gSmokeConstants.DirectionalColorPacked;
	key ^= SmokeHash(asuint(gSmokeConstants.DirectionalDirectionX));
	key ^= SmokeHash(asuint(gSmokeConstants.DirectionalDirectionY));
	key ^= SmokeHash(asuint(gSmokeConstants.DirectionalDirectionZ));
	key ^= SmokeHash(asuint(gSmokeConstants.DirectionalAngularSize));
	return SmokeHash(key);
}

uint SmokeDirectPackMetadata(uint age, uint frameIndex, float visibility)
{
	const uint packedVisibility = (uint)round(saturate(visibility) * 255.0);
	return min(age, 255u) |
		((frameIndex & 0xffu) << NRI_SMOKE_DIRECT_METADATA_FRAME_SHIFT) |
		(packedVisibility << NRI_SMOKE_DIRECT_METADATA_VISIBILITY_SHIFT) |
		NRI_SMOKE_DIRECT_METADATA_VALID;
}

bool SmokeDirectRecordValid(SmokeDirectCacheRecord record)
{
	return (record.Metadata & NRI_SMOKE_DIRECT_METADATA_VALID) != 0u &&
		all(isfinite(record.Radiance)) && isfinite(record.SigmaT) &&
		all(isfinite(record.WorldPosition));
}

uint SmokeDirectRecordAge(SmokeDirectCacheRecord record)
{
	return record.Metadata & NRI_SMOKE_DIRECT_METADATA_AGE_MASK;
}

uint SmokeDirectRecordFrame(SmokeDirectCacheRecord record)
{
	return (record.Metadata >> NRI_SMOKE_DIRECT_METADATA_FRAME_SHIFT) & 0xffu;
}

float SmokeDirectRecordVisibility(SmokeDirectCacheRecord record)
{
	return (float)((record.Metadata >> NRI_SMOKE_DIRECT_METADATA_VISIBILITY_SHIFT) & 0xffu) / 255.0;
}

bool SmokeDirectRecordsCompatible(SmokeDirectCacheRecord a, SmokeDirectCacheRecord b, uint3 froxel)
{
	if (!SmokeDirectRecordValid(a) || !SmokeDirectRecordValid(b))
		return false;
	const float sigmaTolerance = max(max(a.SigmaT, b.SigmaT) * 0.25, 0.002);
	return abs(a.SigmaT - b.SigmaT) <= sigmaTolerance &&
		length(a.WorldPosition - b.WorldPosition) <= SmokeIndirectWorldTolerance(froxel) * 1.5;
}

void SmokeDirectAccumulateVisibilityDiagnostics(float visibility, uint sampleCount, bool diagnostics)
{
	if (!diagnostics)
		return;
	InterlockedAdd(gSmokeControl[0].DirectReceiverSamples, sampleCount);
	if (visibility <= 1e-5)
		InterlockedAdd(gSmokeControl[0].DirectVisibilityZero, 1u);
	else if (visibility >= 1.0 - 1e-5)
		InterlockedAdd(gSmokeControl[0].DirectVisibilityOne, 1u);
	else
		InterlockedAdd(gSmokeControl[0].DirectFractionalVisibility, 1u);
}

#endif
