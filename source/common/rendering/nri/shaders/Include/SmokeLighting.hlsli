#ifndef NRI_SMOKE_LIGHTING_HLSLI
#define NRI_SMOKE_LIGHTING_HLSLI

#include "AnalyticLightSampling.hlsli"
#include "SceneShadowContracts.hlsli"

#define NRI_SMOKE_RUNTIME_LIGHT_TILE_SIZE 64u
#define NRI_SMOKE_MAX_SELECTED_LIGHTS 32u
#define NRI_SMOKE_RUNTIME_LIGHT_FLAG_CASTS_SHADOW 0x1u
#define NRI_SMOKE_SCENE_DATA_SOURCE_STATIC 0u
#define NRI_SMOKE_SCENE_DATA_SOURCE_DYNAMIC 1u
#define NRI_SMOKE_SCENE_DATA_SOURCE_PERSISTENT_VOXEL 2u
#define NRI_SMOKE_PORTAL_TRAVERSAL_SPACE_TRANSFER 2u
#define NRI_SMOKE_FILTER_SKIP_LIMIT 64u
#define NRI_SMOKE_FILTER_CONTINUATION_LIMIT 32u
#define NRI_SMOKE_SCENE_TEXTURE_COUNT 512u

StructuredBuffer<RuntimePointLightData> gSmokeRuntimePointLights : register(t0, space4);
StructuredBuffer<RuntimeLightTileHeaderData> gSmokeRuntimeLightTileHeaders : register(t1, space4);
StructuredBuffer<uint> gSmokeRuntimeLightTileIndices : register(t2, space4);
RaytracingAccelerationStructure gSmokeWorldTlas : register(t0, space5);

StructuredBuffer<PrimitiveData> gSmokeStaticPrimitives : register(t0, space6);
StructuredBuffer<MaterialData> gSmokeStaticMaterials : register(t1, space6);
StructuredBuffer<PrimitiveData> gSmokeDynamicPrimitives : register(t2, space6);
StructuredBuffer<MaterialData> gSmokeDynamicMaterials : register(t3, space6);
StructuredBuffer<SceneInstanceData> gSmokeSceneInstances : register(t4, space6);
StructuredBuffer<PortalData> gSmokeScenePortals : register(t5, space6);
StructuredBuffer<PrimitiveData> gSmokePersistentPrimitives : register(t6, space6);
StructuredBuffer<MaterialData> gSmokePersistentMaterials : register(t7, space6);
Texture2D<float4> gSmokeSceneTextures[NRI_SMOKE_SCENE_TEXTURE_COUNT] : register(t16, space6);
SamplerState gSmokePointWrap : register(s0, space6);
SamplerState gSmokeLinearWrap : register(s1, space6);

struct SmokeFilterStats
{
	uint candidateHits;
	uint alphaRejects;
	uint noShadowRejects;
	uint oneWayRejects;
	uint reflectionRejects;
	uint portalContinuations;
	uint acceptedBlockers;
	uint misses;
	uint skipLimitExits;
	uint continuationLimitExits;
};

PrimitiveData SmokeGetPrimitive(uint dataSource, uint primitiveIndex)
{
	if (dataSource == NRI_SMOKE_SCENE_DATA_SOURCE_DYNAMIC)
	{
		uint count, stride;
		gSmokeDynamicPrimitives.GetDimensions(count, stride);
		return gSmokeDynamicPrimitives[min(primitiveIndex, max(count, 1u) - 1u)];
	}
	if (dataSource == NRI_SMOKE_SCENE_DATA_SOURCE_PERSISTENT_VOXEL)
	{
		uint count, stride;
		gSmokePersistentPrimitives.GetDimensions(count, stride);
		return gSmokePersistentPrimitives[min(primitiveIndex, max(count, 1u) - 1u)];
	}
	uint count, stride;
	gSmokeStaticPrimitives.GetDimensions(count, stride);
	return gSmokeStaticPrimitives[min(primitiveIndex, max(count, 1u) - 1u)];
}

MaterialData SmokeGetMaterial(uint dataSource, uint materialIndex)
{
	if (dataSource == NRI_SMOKE_SCENE_DATA_SOURCE_DYNAMIC)
	{
		uint count, stride;
		gSmokeDynamicMaterials.GetDimensions(count, stride);
		return gSmokeDynamicMaterials[min(materialIndex, max(count, 1u) - 1u)];
	}
	if (dataSource == NRI_SMOKE_SCENE_DATA_SOURCE_PERSISTENT_VOXEL)
	{
		uint count, stride;
		gSmokePersistentMaterials.GetDimensions(count, stride);
		return gSmokePersistentMaterials[min(materialIndex, max(count, 1u) - 1u)];
	}
	uint count, stride;
	gSmokeStaticMaterials.GetDimensions(count, stride);
	return gSmokeStaticMaterials[min(materialIndex, max(count, 1u) - 1u)];
}

uint SmokeResolveMaterialIndex(SceneInstanceData instanceData, PrimitiveData primitive)
{
	uint localIndex = primitive.materialIndex;
	if (instanceData.materialCount != 0xffffffffu && instanceData.materialCount > 0u)
		localIndex = min(localIndex, instanceData.materialCount - 1u);
	return instanceData.materialBase + localIndex;
}

float3 SmokeTransformNormal(SceneInstanceData instanceData, float3 localNormal)
{
	const float3 transformed = float3(
		dot(instanceData.currentTransformRow0.xyz, localNormal),
		dot(instanceData.currentTransformRow1.xyz, localNormal),
		dot(instanceData.currentTransformRow2.xyz, localNormal));
	return dot(transformed, transformed) > 1e-8 ? normalize(transformed) : normalize(localNormal);
}

bool SmokeMaterialIsTransparent(MaterialData material, float2 uv)
{
	if ((material.flags & MATERIAL_FLAG_PLAIN_MIRROR) != 0u)
		return false;
	if (material.textureIndex == 0xffffffffu)
	{
		if ((material.flags & MATERIAL_FLAG_INDEXED) != 0u)
			return (material.flags & MATERIAL_FLAG_ALPHA_CLIP) != 0u;
		return true;
	}
	const uint textureIndex = min(material.textureIndex, NRI_SMOKE_SCENE_TEXTURE_COUNT - 1u);
	const bool pointSampled = (material.flags & (MATERIAL_FLAG_INDEXED | MATERIAL_FLAG_POINT_SAMPLED)) != 0u;
	const float4 rawSample = pointSampled
		? gSmokeSceneTextures[textureIndex].SampleLevel(gSmokePointWrap, uv, 0.0)
		: gSmokeSceneTextures[textureIndex].SampleLevel(gSmokeLinearWrap, uv, 0.0);
	if ((material.flags & MATERIAL_FLAG_INDEXED) != 0u)
	{
		if ((material.flags & MATERIAL_FLAG_ALPHA_CLIP) == 0u)
			return false;
		return (uint)round(saturate(rawSample.r) * 255.0) == 0u;
	}
	return rawSample.a < 0.5;
}

void SmokeCommitFilterStats(SmokeFilterStats stats)
{
	if (stats.candidateHits != 0u) InterlockedAdd(gSmokeControl[0].FilterCandidateHits, stats.candidateHits);
	if (stats.alphaRejects != 0u) InterlockedAdd(gSmokeControl[0].FilterAlphaRejects, stats.alphaRejects);
	if (stats.noShadowRejects != 0u) InterlockedAdd(gSmokeControl[0].FilterNoShadowRejects, stats.noShadowRejects);
	if (stats.oneWayRejects != 0u) InterlockedAdd(gSmokeControl[0].FilterOneWayRejects, stats.oneWayRejects);
	if (stats.reflectionRejects != 0u) InterlockedAdd(gSmokeControl[0].FilterReflectionRejects, stats.reflectionRejects);
	if (stats.portalContinuations != 0u) InterlockedAdd(gSmokeControl[0].FilterPortalContinuations, stats.portalContinuations);
	if (stats.acceptedBlockers != 0u) InterlockedAdd(gSmokeControl[0].FilterAcceptedBlockers, stats.acceptedBlockers);
	if (stats.misses != 0u) InterlockedAdd(gSmokeControl[0].FilterMisses, stats.misses);
	if (stats.skipLimitExits != 0u) InterlockedAdd(gSmokeControl[0].FilterSkipLimitExits, stats.skipLimitExits);
	if (stats.continuationLimitExits != 0u) InterlockedAdd(gSmokeControl[0].FilterContinuationLimitExits, stats.continuationLimitExits);
}

bool SmokePointLightVisibleFiltered(float3 receiverPosition, float3 lightDirection, float lightDistance, bool diagnostics)
{
	SmokeFilterStats stats = (SmokeFilterStats)0;
	float3 origin = receiverPosition + lightDirection * 0.05;
	float remainingDistance = max(lightDistance - 0.051, 0.001);
	uint portalBudget = min((gSmokeConstants.FilteredVisibilityEnabled >> 8u) & 0xffu, 8u);
	uint sceneInstanceCount, sceneInstanceStride, portalCount, portalStride;
	gSmokeSceneInstances.GetDimensions(sceneInstanceCount, sceneInstanceStride);
	gSmokeScenePortals.GetDimensions(portalCount, portalStride);
	[loop]
	for (uint continuation = 0u; continuation < NRI_SMOKE_FILTER_CONTINUATION_LIMIT; ++continuation)
	{
		float accumulatedDistance = 0.0;
		bool continuedPortal = false;
		[loop]
		for (uint skip = 0u; skip < NRI_SMOKE_FILTER_SKIP_LIMIT; ++skip)
		{
			const float tMin = accumulatedDistance > 0.0 ? accumulatedDistance + 1e-6 : 0.001;
			if (tMin >= remainingDistance)
			{
				stats.misses++;
				if (diagnostics) SmokeCommitFilterStats(stats);
				return true;
			}
			RayDesc ray = { origin, tMin, lightDirection, remainingDistance };
			RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
			query.TraceRayInline(gSmokeWorldTlas, RAY_FLAG_FORCE_OPAQUE, NRI_TLAS_MASK_ALL_WORKLOADS, ray);
			while (query.Proceed()) {}
			if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
			{
				stats.misses++;
				if (diagnostics) SmokeCommitFilterStats(stats);
				return true;
			}
			stats.candidateHits++;
			const uint instanceId = query.CommittedInstanceID();
			if (instanceId >= sceneInstanceCount)
			{
				stats.acceptedBlockers++;
				if (diagnostics) SmokeCommitFilterStats(stats);
				return false;
			}
			const SceneInstanceData instanceData = gSmokeSceneInstances[instanceId];
			const uint primitiveIndex = instanceData.primitiveBase + query.CommittedPrimitiveIndex();
			const PrimitiveData primitive = SmokeGetPrimitive(instanceData.dataSource, primitiveIndex);
			const uint materialIndex = SmokeResolveMaterialIndex(instanceData, primitive);
			const MaterialData material = SmokeGetMaterial(instanceData.dataSource, materialIndex);
			const float hitDistance = query.CommittedRayT();
			if ((primitive.flags & PRIMITIVE_FLAG_REFLECTION_ONLY) != 0u)
			{
				stats.reflectionRejects++;
				accumulatedDistance = hitDistance;
				continue;
			}
			const float3 normal = SmokeTransformNormal(instanceData, primitive.normal);
			if ((material.flags & MATERIAL_FLAG_ONE_WAY) != 0u && dot(normal, lightDirection) > 0.0)
			{
				stats.oneWayRejects++;
				accumulatedDistance = hitDistance;
				continue;
			}
			const float2 bary = query.CommittedTriangleBarycentrics();
			const float3 weights = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
			const float2 uv = primitive.uv0 * weights.x + primitive.uv1 * weights.y + primitive.uv2 * weights.z;
			if (SmokeMaterialIsTransparent(material, uv))
			{
				stats.alphaRejects++;
				accumulatedDistance = hitDistance;
				continue;
			}
			if ((material.lightingFlags & MATERIAL_LIGHTING_FLAG_NO_SHADOW_CAST) != 0u)
			{
				stats.noShadowRejects++;
				accumulatedDistance = hitDistance;
				continue;
			}
			if (primitive.portalIndex != 0xffffffffu && primitive.portalIndex < portalCount)
			{
				const PortalData portal = gSmokeScenePortals[primitive.portalIndex];
				if (portal.traversalClass == NRI_SMOKE_PORTAL_TRAVERSAL_SPACE_TRANSFER && portalBudget > 0u)
				{
					remainingDistance = max(remainingDistance - hitDistance, 0.0);
					origin = origin + lightDirection * (hitDistance + 0.05) + portal.delta;
					portalBudget--;
					stats.portalContinuations++;
					continuedPortal = true;
					break;
				}
			}
			stats.acceptedBlockers++;
			if (diagnostics) SmokeCommitFilterStats(stats);
			return false;
		}
		if (!continuedPortal)
		{
			stats.skipLimitExits++;
			if (diagnostics) SmokeCommitFilterStats(stats);
			return true;
		}
	}
	stats.continuationLimitExits++;
	if (diagnostics) SmokeCommitFilterStats(stats);
	return true;
}

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

bool SmokePointLightVisible(float3 receiverPosition, float3 lightDirection, float lightDistance, bool diagnostics)
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
	query.TraceRayInline(gSmokeWorldTlas, rayFlags, NRI_TLAS_MASK_ALL_WORKLOADS, ray);
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
