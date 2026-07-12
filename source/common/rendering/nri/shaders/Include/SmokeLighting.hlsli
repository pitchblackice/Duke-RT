#ifndef NRI_SMOKE_LIGHTING_HLSLI
#define NRI_SMOKE_LIGHTING_HLSLI

#include "AnalyticLightSampling.hlsli"

#define NRI_SMOKE_RUNTIME_LIGHT_TILE_SIZE 64u
#define NRI_SMOKE_MAX_SELECTED_LIGHTS 32u
#define NRI_SMOKE_TLAS_MASK_SHADOW 0x02u
#define NRI_SMOKE_RUNTIME_LIGHT_FLAG_CASTS_SHADOW 0x1u

struct RuntimePointLightData
{
	float3 position;
	float radius;
	float3 color;
	float intensity;
	uint flags;
	float emitterRadius;
	uint stableKeyLo;
	uint stableKeyHi;
};

struct RuntimeLightTileHeaderData
{
	uint indexOffset;
	uint indexCount;
};

StructuredBuffer<RuntimePointLightData> gSmokeRuntimePointLights : register(t0, space4);
StructuredBuffer<RuntimeLightTileHeaderData> gSmokeRuntimeLightTileHeaders : register(t1, space4);
StructuredBuffer<uint> gSmokeRuntimeLightTileIndices : register(t2, space4);
RaytracingAccelerationStructure gSmokeWorldTlas : register(t0, space5);

float SmokeHenyeyGreenstein(float cosineTheta, float anisotropy)
{
	const float g = clamp(anisotropy, -0.95, 0.95);
	const float gSquared = g * g;
	const float denominatorBase = max(1.0 + gSquared - 2.0 * g * clamp(cosineTheta, -1.0, 1.0), 1e-4);
	return (1.0 - gSquared) / (12.56637061436 * denominatorBase * sqrt(denominatorBase));
}

float3 SmokeSampleReceiverFacingEmitter(RuntimePointLightData light, float3 centerDirection, inout uint randomState)
{
	const float emitterRadius = max(light.emitterRadius, 0.0);
	const float phi = SmokeRandom01(randomState) * 6.28318530718;
	const float radius = sqrt(SmokeRandom01(randomState));
	return SampleAnalyticReceiverFacingDisk(light.position, emitterRadius, centerDirection, float2(cos(phi), sin(phi)) * radius);
}

bool SmokePointLightVisible(float3 receiverPosition, float3 lightDirection, float lightDistance)
{
	if (lightDistance <= 0.052)
		return true;

	RayDesc ray;
	ray.Origin = receiverPosition + lightDirection * 0.05;
	ray.TMin = 0.001;
	ray.Direction = lightDirection;
	ray.TMax = max(lightDistance - 0.051, 0.001);
	const uint rayFlags = RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
	RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
	query.TraceRayInline(gSmokeWorldTlas, rayFlags, NRI_SMOKE_TLAS_MASK_SHADOW, ray);
	while (query.Proceed()) {}
	return query.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

uint SmokeLightRandomSeed(uint3 froxel, RuntimePointLightData light, uint sampleIndex)
{
	uint seed = SmokeHash(froxel.x ^ SmokeHash(froxel.y + 0x9e3779b9u));
	seed ^= SmokeHash(froxel.z + 0x85ebca6bu);
	seed ^= SmokeHash(gSmokeConstants.FrameIndex + 0xc2b2ae35u);
	seed ^= SmokeHash(gSmokeConstants.SimulationEpoch + 0x27d4eb2fu);
	seed ^= SmokeHash(light.stableKeyLo ^ SmokeHash(light.stableKeyHi + 0x165667b1u));
	return SmokeHash(seed ^ SmokeHash(sampleIndex + 1u));
}

RuntimeLightTileHeaderData SmokeGetRuntimeLightTileHeader(uint2 froxelPosition)
{
	RuntimeLightTileHeaderData emptyHeader = { 0u, 0u };
	if (gSmokeConstants.RenderWidth == 0u || gSmokeConstants.RenderHeight == 0u ||
		gSmokeConstants.RuntimeLightTileCountX == 0u || gSmokeConstants.RuntimeLightTileCountY == 0u)
		return emptyHeader;

	const float2 froxelUv = (float2(froxelPosition) + 0.5) /
		float2(max(gSmokeConstants.FroxelWidth, 1u), max(gSmokeConstants.FroxelHeight, 1u));
	const uint2 pixelPosition = min(
		(uint2)(froxelUv * float2(gSmokeConstants.RenderWidth, gSmokeConstants.RenderHeight)),
		uint2(gSmokeConstants.RenderWidth - 1u, gSmokeConstants.RenderHeight - 1u));
	const uint2 tilePosition = min(
		pixelPosition / NRI_SMOKE_RUNTIME_LIGHT_TILE_SIZE,
		uint2(gSmokeConstants.RuntimeLightTileCountX - 1u, gSmokeConstants.RuntimeLightTileCountY - 1u));
	const uint tileIndex = tilePosition.y * gSmokeConstants.RuntimeLightTileCountX + tilePosition.x;
	uint headerCount, ignoredStride;
	gSmokeRuntimeLightTileHeaders.GetDimensions(headerCount, ignoredStride);
	if (tileIndex < headerCount)
		return gSmokeRuntimeLightTileHeaders[tileIndex];
	return emptyHeader;
}

#endif
