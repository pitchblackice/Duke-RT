#ifndef NRI_SMOKE_INDIRECT_CACHE_HLSLI
#define NRI_SMOKE_INDIRECT_CACHE_HLSLI

#include "SmokeResources.hlsli"
#include "SmokeFroxel.hlsli"

#define NRI_SMOKE_INDIRECT_CACHE_MODE_SHIFT 2u
#define NRI_SMOKE_INDIRECT_CACHE_MODE_MASK 3u
#define NRI_SMOKE_INDIRECT_HISTORY_VALID 0x10u
#define NRI_SMOKE_INDIRECT_SAMPLE_SHIFT 5u
#define NRI_SMOKE_INDIRECT_SAMPLE_MASK 3u

struct SmokeReprojectionData
{
	float4 currentViewToClipMatrix[4];
	float4 previousViewToClipMatrix[4];
	float4 currentWorldToViewMatrix[4];
	float4 previousWorldToViewMatrix[4];
};

StructuredBuffer<SmokeReprojectionData> gSmokeReprojectionData : register(t17, space6);

uint SmokeIndirectCacheMode()
{
	return (gSmokeConstants.Flags >> NRI_SMOKE_INDIRECT_CACHE_MODE_SHIFT) & NRI_SMOKE_INDIRECT_CACHE_MODE_MASK;
}

uint SmokeIndirectReferenceSampleCount()
{
	return 1u << min((gSmokeConstants.Flags >> NRI_SMOKE_INDIRECT_SAMPLE_SHIFT) & NRI_SMOKE_INDIRECT_SAMPLE_MASK, 2u);
}

uint SmokeIndirectMediumHash(float4 medium, float anisotropy)
{
	const uint3 albedo = (uint3)round(saturate(medium.rgb / max(medium.a, 1e-5)) * 15.0);
	const uint g = (uint)round(saturate(anisotropy * 0.5 + 0.5) * 15.0);
	return SmokeHash(albedo.x | (albedo.y << 4u) | (albedo.z << 8u) | (g << 12u)) & 0x1fu;
}

uint SmokePackIndirectMetadata(uint sectorIndex, float4 medium, float anisotropy, uint age, uint frameIndex)
{
	return min(sectorIndex, 0xffffu) |
		(SmokeIndirectMediumHash(medium, anisotropy) << 16u) |
		(min(age, 15u) << 21u) |
		((frameIndex & 0x3fu) << 25u) |
		0x80000000u;
}

bool SmokeIndirectRecordValid(SmokeIndirectCacheRecord record)
{
	return (record.Metadata & 0x80000000u) != 0u && all(isfinite(record.Radiance)) &&
		isfinite(record.SigmaT) && all(isfinite(record.WorldPosition));
}

uint SmokeIndirectRecordSector(SmokeIndirectCacheRecord record) { return record.Metadata & 0xffffu; }
uint SmokeIndirectRecordMediumHash(SmokeIndirectCacheRecord record) { return (record.Metadata >> 16u) & 0x1fu; }
uint SmokeIndirectRecordAge(SmokeIndirectCacheRecord record) { return (record.Metadata >> 21u) & 0xfu; }
uint SmokeIndirectRecordFrame(SmokeIndirectCacheRecord record) { return (record.Metadata >> 25u) & 0x3fu; }

bool SmokeIndirectRecordsCompatible(SmokeIndirectCacheRecord a, SmokeIndirectCacheRecord b)
{
	if (!SmokeIndirectRecordValid(a) || !SmokeIndirectRecordValid(b) ||
		SmokeIndirectRecordSector(a) != SmokeIndirectRecordSector(b) ||
		SmokeIndirectRecordMediumHash(a) != SmokeIndirectRecordMediumHash(b))
		return false;
	const float sigmaTolerance = max(max(a.SigmaT, b.SigmaT) * 0.25, 0.002);
	return abs(a.SigmaT - b.SigmaT) <= sigmaTolerance;
}

float SmokeIndirectWorldTolerance(uint3 froxel)
{
	const float nearDepth = SmokeSliceNearDepth(froxel.z);
	const float farDepth = SmokeSliceFarDepth(froxel.z);
	const float centerDepth = (nearDepth + farDepth) * 0.5;
	const float cellX = centerDepth * gSmokeConstants.TanHalfFovX * 2.0 / max((float)gSmokeConstants.FroxelWidth, 1.0);
	const float cellY = centerDepth * gSmokeConstants.TanHalfFovY * 2.0 / max((float)gSmokeConstants.FroxelHeight, 1.0);
	return max(length(float3(cellX, cellY, farDepth - nearDepth)) * 1.5, 0.1);
}

float4 SmokeMultiplyMatrixPoint(float4 value, float4 matrixColumns[4])
{
	return float4(
		dot(value, float4(matrixColumns[0].x, matrixColumns[1].x, matrixColumns[2].x, matrixColumns[3].x)),
		dot(value, float4(matrixColumns[0].y, matrixColumns[1].y, matrixColumns[2].y, matrixColumns[3].y)),
		dot(value, float4(matrixColumns[0].z, matrixColumns[1].z, matrixColumns[2].z, matrixColumns[3].z)),
		dot(value, float4(matrixColumns[0].w, matrixColumns[1].w, matrixColumns[2].w, matrixColumns[3].w)));
}

bool SmokePreviousFroxel(float3 worldPosition, out uint previousIndex)
{
	previousIndex = 0u;
	uint reprojectionCount, ignoredStride;
	gSmokeReprojectionData.GetDimensions(reprojectionCount, ignoredStride);
	if (reprojectionCount == 0u)
		return false;
	const SmokeReprojectionData reprojection = gSmokeReprojectionData[0];
	const float4 previousView = SmokeMultiplyMatrixPoint(float4(worldPosition, 1.0), reprojection.previousWorldToViewMatrix);
	const float previousDepth = -previousView.z;
	if (!isfinite(previousDepth) || previousDepth <= 0.001 || previousDepth >= gSmokeConstants.FroxelMaxDistance)
		return false;
	const float4 previousClip = SmokeMultiplyMatrixPoint(previousView, reprojection.previousViewToClipMatrix);
	if (!all(isfinite(previousClip)) || previousClip.w <= 1e-5)
		return false;
	const float2 ndc = previousClip.xy / previousClip.w;
	const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
	if (any(uv < 0.0) || any(uv >= 1.0))
		return false;
	const uint2 xy = min((uint2)(uv * float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight)),
		uint2(gSmokeConstants.FroxelWidth - 1u, gSmokeConstants.FroxelHeight - 1u));
	previousIndex = SmokeFroxelIndex(xy.x, xy.y, SmokeDepthSlice(previousDepth));
	return true;
}

#endif
