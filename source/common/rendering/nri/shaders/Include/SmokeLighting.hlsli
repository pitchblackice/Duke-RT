#ifndef NRI_SMOKE_LIGHTING_HLSLI
#define NRI_SMOKE_LIGHTING_HLSLI

#include "AnalyticLightSampling.hlsli"
#include "SceneShadowContracts.hlsli"
#include "EmissiveLightContracts.hlsli"
#include "DirectionalLightSampling.hlsli"

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
#define NRI_SMOKE_LIGHT_SOURCE_POINT 0x1u
#define NRI_SMOKE_LIGHT_SOURCE_DIRECTIONAL 0x2u
#define NRI_SMOKE_LIGHT_SOURCE_DIRECTIONAL_SHADOW 0x4u
#define NRI_SMOKE_LIGHT_SOURCE_EMISSIVE 0x8u
#define NRI_SMOKE_LIGHT_SOURCE_INDIRECT 0x10u

struct SmokeSectorLightHeaderData
{
	uint sectorCount;
	uint activeCount;
	uint pulsingCount;
	uint flags;
};

struct SmokeSectorLightData
{
	float3 ambientColor;
	float ambientIntensity;
	float3 hemisphereColor;
	float hemisphereAmount;
	float fogAmount;
	float pulseScale;
	uint sourceFlags;
	int paletteIndex;
	int lotag;
	int hitag;
};
bool SmokeShadowTracingReady()
{
	return (gSmokeConstants.FilteredVisibilityEnabled & NRI_SMOKE_VISIBILITY_TLAS_READY) != 0u;
}

bool SmokeFilteredVisibilityEffective()
{
	return (gSmokeConstants.FilteredVisibilityEnabled & NRI_SMOKE_VISIBILITY_FILTERED_EFFECTIVE) != 0u;
}

StructuredBuffer<RuntimePointLightData> gSmokeRuntimePointLights : register(t0, space4);
StructuredBuffer<RuntimeLightTileHeaderData> gSmokeRuntimeLightTileHeaders : register(t1, space4);
StructuredBuffer<uint> gSmokeRuntimeLightTileIndices : register(t2, space4);

StructuredBuffer<PrimitiveData> gSmokeStaticPrimitives : register(t0, space6);
StructuredBuffer<MaterialData> gSmokeStaticMaterials : register(t1, space6);
StructuredBuffer<PrimitiveData> gSmokeDynamicPrimitives : register(t2, space6);
StructuredBuffer<MaterialData> gSmokeDynamicMaterials : register(t3, space6);
StructuredBuffer<SceneInstanceData> gSmokeSceneInstances : register(t4, space6);
StructuredBuffer<PortalData> gSmokeScenePortals : register(t5, space6);
StructuredBuffer<PrimitiveData> gSmokePersistentPrimitives : register(t6, space6);
StructuredBuffer<MaterialData> gSmokePersistentMaterials : register(t7, space6);
StructuredBuffer<SceneVertex> gSmokeStaticVertices : register(t8, space6);
StructuredBuffer<SceneVertex> gSmokeDynamicVertices : register(t9, space6);
StructuredBuffer<SceneVertex> gSmokePersistentVertices : register(t10, space6);
StructuredBuffer<EmissivePrimitiveHeaderData> gSmokeEmissivePrimitiveHeaders : register(t11, space6);
StructuredBuffer<EmissivePrimitiveData> gSmokeEmissivePrimitives : register(t12, space6);
StructuredBuffer<float> gSmokeEmissivePrimitiveCdf : register(t13, space6);
StructuredBuffer<EmissiveMaterialResponseData> gSmokeEmissiveMaterialResponses : register(t14, space6);
StructuredBuffer<SmokeSectorLightHeaderData> gSmokeSectorLightHeaders : register(t15, space6);
StructuredBuffer<SmokeSectorLightData> gSmokeSectorLights : register(t16, space6);
Texture2D<float4> gSmokePaletteLookup : register(t18, space6);
TextureCube<float4> gSmokeSkyTexture : register(t19, space6);
Texture2D<float4> gSmokeSceneTextures[NRI_SMOKE_SCENE_TEXTURE_COUNT] : register(t20, space6);
SamplerState gSmokePointWrap : register(s0, space6);
SamplerState gSmokeLinearWrap : register(s1, space6);
SamplerState gSmokePointClamp : register(s2, space6);
RaytracingAccelerationStructure gSmokeWorldTlas : register(t532, space6);

struct SmokeIndirectHit
{
	uint hit;
	uint instanceValid;
	uint dataSource;
	uint primitiveIndex;
	uint sectorIndex;
	uint materialFlags;
	uint instanceSectorIndex;
	uint materialIndex;
	float2 uv;
	float2 padding2;
};

PrimitiveData SmokeGetPrimitive(uint dataSource, uint primitiveIndex);
MaterialData SmokeGetMaterial(uint dataSource, uint materialIndex);
uint SmokeResolveMaterialIndex(SceneInstanceData instanceData, PrimitiveData primitive);

SmokeIndirectHit SmokeTraceIndirectClosest(float3 origin, float3 direction, float maximumDistance)
{
	SmokeIndirectHit result = (SmokeIndirectHit)0;
	result.sectorIndex = 0xffffffffu;
	result.instanceSectorIndex = 0xffffffffu;
	RayDesc ray = { origin + direction * 0.05, 0.001, direction, max(maximumDistance - 0.051, 0.001) };
	RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
	query.TraceRayInline(gSmokeWorldTlas, RAY_FLAG_FORCE_OPAQUE, NRI_TLAS_MASK_ALL_WORKLOADS, ray);
	while (query.Proceed()) {}
	if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
		return result;

	result.hit = 1u;
	uint instanceCount, ignoredStride;
	gSmokeSceneInstances.GetDimensions(instanceCount, ignoredStride);
	const uint instanceIndex = query.CommittedInstanceID();
	if (instanceIndex >= instanceCount)
		return result;
	const SceneInstanceData instanceData = gSmokeSceneInstances[instanceIndex];
	const uint primitiveIndex = instanceData.primitiveBase + query.CommittedPrimitiveIndex();
	const PrimitiveData primitive = SmokeGetPrimitive(instanceData.dataSource, primitiveIndex);
	const uint materialIndex = SmokeResolveMaterialIndex(instanceData, primitive);
	const MaterialData material = SmokeGetMaterial(instanceData.dataSource, materialIndex);
	const float2 bary = query.CommittedTriangleBarycentrics();
	const float3 weights = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
	result.instanceValid = 1u;
	result.dataSource = instanceData.dataSource;
	result.primitiveIndex = primitiveIndex;
	result.sectorIndex = material.sectorIndex;
	result.materialFlags = material.flags;
	result.instanceSectorIndex = instanceData.metadata1;
	result.materialIndex = materialIndex;
	result.uv = primitive.uv0 * weights.x + primitive.uv1 * weights.y + primitive.uv2 * weights.z;
	return result;
}

bool SmokeResolveStaticFlatSector(SmokeIndirectHit hit, out uint sectorIndex)
{
	sectorIndex = 0xffffffffu;
	if (hit.hit == 0u || hit.instanceValid == 0u || hit.dataSource != NRI_SMOKE_SCENE_DATA_SOURCE_STATIC ||
		(hit.materialFlags & MATERIAL_FLAG_FLAT) == 0u || hit.sectorIndex == 0xffffffffu ||
		hit.instanceSectorIndex != hit.sectorIndex)
		return false;
	uint headerCount, headerStride, sectorCount, sectorStride;
	gSmokeSectorLightHeaders.GetDimensions(headerCount, headerStride);
	gSmokeSectorLights.GetDimensions(sectorCount, sectorStride);
	if (headerCount == 0u || (gSmokeSectorLightHeaders[0].flags & 1u) == 0u ||
		hit.sectorIndex >= min(gSmokeSectorLightHeaders[0].sectorCount, sectorCount))
		return false;
	sectorIndex = hit.sectorIndex;
	return true;
}

float3 SmokeSectorAmbientIncidentRadiance(uint sectorIndex)
{
	uint sectorCount, ignoredStride;
	gSmokeSectorLights.GetDimensions(sectorCount, ignoredStride);
	if (sectorIndex >= sectorCount)
		return 0.0;
	const SmokeSectorLightData light = gSmokeSectorLights[sectorIndex];
	// Only the normal-free ambient field has validated incident-radiance semantics.
	// Hemisphere and fog terms remain surface policy and are intentionally excluded.
	return max(light.ambientColor, 0.0) * max(light.ambientIntensity, 0.0);
}

float3 SmokeIndirectReferenceDirection(uint sampleIndex, uint sampleCount, uint3 froxel)
{
	const float z = 1.0 - 2.0 * ((float)sampleIndex + 0.5) / max((float)sampleCount, 1.0);
	const float radius = sqrt(max(1.0 - z * z, 0.0));
	const float rotation = (float)(SmokeHash(froxel.x ^ SmokeHash(froxel.y ^ SmokeHash(froxel.z))) & 0xffffu) * (6.28318530718 / 65536.0);
	const float phi = rotation + (float)sampleIndex * 2.39996322973;
	return float3(radius * cos(phi), z, radius * sin(phi));
}

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

float3 SmokeDirectionalColor()
{
	const uint packed = gSmokeConstants.DirectionalColorPacked;
	return float3(
		(float)(packed & 0xffu),
		(float)((packed >> 8u) & 0xffu),
		(float)((packed >> 16u) & 0xffu)) * (8.0 / 255.0);
}

float3 SmokeDirectionalDirection()
{
	return normalize(float3(
		gSmokeConstants.DirectionalDirectionX,
		gSmokeConstants.DirectionalDirectionY,
		gSmokeConstants.DirectionalDirectionZ));
}

uint SmokeLightingRandomSeed(uint3 froxel, uint sampleIndex, uint familySalt)
{
	uint seed = SmokeHash(froxel.x ^ SmokeHash(froxel.y + 0x9e3779b9u));
	seed ^= SmokeHash(froxel.z + 0x85ebca6bu);
	seed ^= SmokeHash(gSmokeConstants.FrameIndex + 0xc2b2ae35u);
	seed ^= SmokeHash(gSmokeConstants.SimulationEpoch + 0x27d4eb2fu);
	seed ^= SmokeHash(familySalt);
	return SmokeHash(seed ^ SmokeHash(sampleIndex + 1u));
}

uint SmokeDirectionalStableRandomSeed(uint sampleIndex, uint familySalt)
{
	// Every duplicate node in the overlapping carrier windows represents the
	// same world visibility field, so it must use the same stable cone sequence.
	return SmokeHash(SmokeHash(familySalt) ^ SmokeHash(sampleIndex + 1u));
}

float3 SmokeSampleDirectionalCone(float3 centerDirection, float angularRadius, inout uint randomState)
{
	return SampleUniformDirectionalCone(centerDirection, angularRadius, float2(SmokeRandom01(randomState), SmokeRandom01(randomState)));
}

SceneVertex SmokeGetVertex(uint dataSource, uint vertexIndex)
{
	uint count, stride;
	if (dataSource == NRI_SMOKE_SCENE_DATA_SOURCE_DYNAMIC)
	{
		gSmokeDynamicVertices.GetDimensions(count, stride);
		return gSmokeDynamicVertices[min(vertexIndex, max(count, 1u) - 1u)];
	}
	if (dataSource == NRI_SMOKE_SCENE_DATA_SOURCE_PERSISTENT_VOXEL)
	{
		gSmokePersistentVertices.GetDimensions(count, stride);
		return gSmokePersistentVertices[min(vertexIndex, max(count, 1u) - 1u)];
	}
	gSmokeStaticVertices.GetDimensions(count, stride);
	return gSmokeStaticVertices[min(vertexIndex, max(count, 1u) - 1u)];
}

float4 SmokeSampleMaterialColor(MaterialData material, uint textureIndex, float2 uv, bool indexed, bool applyPalette)
{
	if (textureIndex == 0xffffffffu)
		return 0.0;
	const uint safeTextureIndex = min(textureIndex, NRI_SMOKE_SCENE_TEXTURE_COUNT - 1u);
	float4 color = (indexed || (material.flags & MATERIAL_FLAG_POINT_SAMPLED) != 0u)
		? gSmokeSceneTextures[safeTextureIndex].SampleLevel(gSmokePointWrap, uv, 0.0)
		: gSmokeSceneTextures[safeTextureIndex].SampleLevel(gSmokeLinearWrap, uv, 0.0);
	if (indexed && applyPalette)
	{
		const float paletteValue = saturate(color.r) * 255.0;
		const float2 paletteUv = float2((paletteValue + 0.5) / 256.0, ((float)material.paletteIndex + 0.5) / 256.0);
		color = gSmokePaletteLookup.SampleLevel(gSmokePointClamp, paletteUv, 0.0);
	}
	return color;
}

float3 SmokeSampleMaterialEmission(MaterialData material, float2 uv)
{
	const float3 tint = (material.flags & MATERIAL_FLAG_TINT_EMISSION) != 0u ? material.emissiveColor : 1.0.xxx;
	if (material.emissiveMode == 1u)
	{
		const bool indexed = (material.flags & MATERIAL_FLAG_INDEXED) != 0u;
		return SmokeSampleMaterialColor(material, material.textureIndex, uv, indexed, true).rgb * tint;
	}
	if (material.emissiveMode == 2u)
		return material.emissiveColor;
	if (material.emissiveMode == 3u)
	{
		if (material.emissiveTextureIndex == 0xffffffffu)
			return material.emissiveColor;
		const bool indexed = material.emissiveTextureIndex == material.textureIndex && (material.flags & MATERIAL_FLAG_INDEXED) != 0u;
		return SmokeSampleMaterialColor(material, material.emissiveTextureIndex, uv, indexed, indexed).rgb * tint;
	}
	return 0.0;
}

uint SmokeSampleEmissivePrimitive(inout uint randomState)
{
	uint headerCount, headerStride, primitiveCount, primitiveStride, cdfCount, cdfStride;
	gSmokeEmissivePrimitiveHeaders.GetDimensions(headerCount, headerStride);
	gSmokeEmissivePrimitives.GetDimensions(primitiveCount, primitiveStride);
	gSmokeEmissivePrimitiveCdf.GetDimensions(cdfCount, cdfStride);
	if (headerCount == 0u)
		return 0xffffffffu;
	const uint activeCount = min(gSmokeEmissivePrimitiveHeaders[0].activeCount, min(primitiveCount, cdfCount));
	if (activeCount == 0u)
		return 0xffffffffu;
	const float sample = SmokeRandom01(randomState);
	uint low = 0u;
	uint high = activeCount - 1u;
	[unroll]
	for (uint i = 0u; i < 14u && low < high; ++i)
	{
		const uint mid = (low + high) >> 1u;
		if (sample <= gSmokeEmissivePrimitiveCdf[mid])
			high = mid;
		else
			low = mid + 1u;
	}
	return low;
}

float3 SmokeTransformPoint(SceneInstanceData instanceData, float3 localPosition)
{
	const float4 p = float4(localPosition, 1.0);
	return float3(
		dot(instanceData.currentTransformRow0, p),
		dot(instanceData.currentTransformRow1, p),
		dot(instanceData.currentTransformRow2, p));
}

bool SmokeSamplePointOnEmissive(
	EmissivePrimitiveData candidate,
	inout uint randomState,
	out uint primitiveIndex,
	out PrimitiveData primitive,
	out MaterialData material,
	out float3 position,
	out float2 uv,
	out float3 normal,
	out float effectiveArea)
{
	primitiveIndex = candidate.primitiveIndex;
	primitive = (PrimitiveData)0;
	material = (MaterialData)0;
	position = 0.0;
	uv = 0.0;
	normal = 0.0;
	effectiveArea = 0.0;
	const bool placedRange = candidate.sceneInstanceIndex != 0xffffffffu;
	SceneInstanceData instanceData = (SceneInstanceData)0;
	if (placedRange)
	{
		uint sceneInstanceCount, sceneInstanceStride, persistentPrimitiveCount, persistentPrimitiveStride;
		gSmokeSceneInstances.GetDimensions(sceneInstanceCount, sceneInstanceStride);
		gSmokePersistentPrimitives.GetDimensions(persistentPrimitiveCount, persistentPrimitiveStride);
		if (candidate.dataSource != NRI_SMOKE_SCENE_DATA_SOURCE_PERSISTENT_VOXEL ||
			candidate.primitiveCount == 0u || candidate.sceneInstanceIndex >= sceneInstanceCount ||
			candidate.primitiveIndex >= persistentPrimitiveCount || candidate.primitiveCount > persistentPrimitiveCount - candidate.primitiveIndex)
			return false;
		instanceData = gSmokeSceneInstances[candidate.sceneInstanceIndex];
		if (instanceData.dataSource != NRI_SMOKE_SCENE_DATA_SOURCE_PERSISTENT_VOXEL ||
			instanceData.primitiveBase != candidate.primitiveIndex ||
			instanceData.metadata0 != candidate.occurrenceKeyLo ||
			instanceData.metadata1 != candidate.occurrenceKeyHi ||
			instanceData.metadata2 != candidate.occurrenceGeneration)
			return false;
		primitiveIndex = candidate.primitiveIndex + min((uint)(SmokeRandom01(randomState) * candidate.primitiveCount), candidate.primitiveCount - 1u);
	}
	primitive = SmokeGetPrimitive(candidate.dataSource, primitiveIndex);
	const uint materialIndex = placedRange ? SmokeResolveMaterialIndex(instanceData, primitive) : primitive.materialIndex;
	material = SmokeGetMaterial(candidate.dataSource, materialIndex);
	SceneVertex v0 = SmokeGetVertex(candidate.dataSource, primitive.indices.x);
	SceneVertex v1 = SmokeGetVertex(candidate.dataSource, primitive.indices.y);
	SceneVertex v2 = SmokeGetVertex(candidate.dataSource, primitive.indices.z);
	if (placedRange)
	{
		v0.position = SmokeTransformPoint(instanceData, v0.position);
		v1.position = SmokeTransformPoint(instanceData, v1.position);
		v2.position = SmokeTransformPoint(instanceData, v2.position);
		const float3 areaVector = cross(v1.position - v0.position, v2.position - v0.position);
		const float areaVectorLength = length(areaVector);
		if (areaVectorLength <= 1e-8)
			return false;
		normal = areaVector / areaVectorLength;
		effectiveArea = 0.5 * areaVectorLength * candidate.primitiveCount;
	}
	else
	{
		normal = normalize(primitive.normal);
		effectiveArea = candidate.primitiveArea;
	}
	const float rootU = sqrt(saturate(SmokeRandom01(randomState)));
	const float v = SmokeRandom01(randomState);
	const float3 bary = float3(1.0 - rootU, rootU * (1.0 - v), rootU * v);
	uv = primitive.uv0 * bary.x + primitive.uv1 * bary.y + primitive.uv2 * bary.z;
	position = v0.position * bary.x + v1.position * bary.y + v2.position * bary.z;
	return true;
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

	const uint2 pixelPosition = SmokeNativeRenderPixel(froxelPosition);
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
