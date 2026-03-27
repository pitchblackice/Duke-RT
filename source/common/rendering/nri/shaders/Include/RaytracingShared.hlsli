#ifndef RAZE_NRI_PT_RAYTRACING_SHARED_HLSLI
#define RAZE_NRI_PT_RAYTRACING_SHARED_HLSLI

#include "Shared.hlsli"

struct HitData
{
	bool hit;
	uint dataSource;
	uint primitiveIndex;
	uint portalIndex;
	float2 barycentrics;
	float distance;
	float3 position;
	float3 normal;
	float2 uv;
	uint materialIndex;
};

static const uint SCENE_DATA_SOURCE_STATIC = 0u;
static const uint SCENE_DATA_SOURCE_DYNAMIC = 1u;
static const uint PORTAL_TRAVERSAL_CLASS_NONE = 0u;
static const uint PORTAL_TRAVERSAL_CLASS_REFLECTIVE = 1u;
static const uint PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER = 2u;
static const uint PORTAL_TRAVERSAL_CLASS_RUNTIME_BOUND = 3u;

HitData MakeEmptyHitData()
{
	HitData hitData = (HitData)0;
	hitData.primitiveIndex = 0xffffffffu;
	hitData.materialIndex = 0xffffffffu;
	hitData.portalIndex = 0xffffffffu;
	return hitData;
}

SceneInstanceData GetSceneInstanceData(uint instanceId)
{
	return gSceneInstances[min(instanceId, max(gTraceConstants.SceneInstanceCount, 1u) - 1u)];
}

uint GetPrimitiveCount(uint dataSource)
{
	return dataSource == SCENE_DATA_SOURCE_DYNAMIC ? gTraceConstants.DynamicPrimitiveCount : gTraceConstants.StaticPrimitiveCount;
}

uint GetMaterialCount(uint dataSource)
{
	return dataSource == SCENE_DATA_SOURCE_DYNAMIC ? gTraceConstants.DynamicMaterialCount : gTraceConstants.StaticMaterialCount;
}

PrimitiveData GetPrimitiveData(uint dataSource, uint primitiveIndex)
{
	if (dataSource == SCENE_DATA_SOURCE_DYNAMIC)
	{
		return gDynamicPrimitives[min(primitiveIndex, max(gTraceConstants.DynamicPrimitiveCount, 1u) - 1u)];
	}

	return gStaticPrimitives[min(primitiveIndex, max(gTraceConstants.StaticPrimitiveCount, 1u) - 1u)];
}

SceneVertex GetVertexData(uint dataSource, uint vertexIndex)
{
	if (dataSource == SCENE_DATA_SOURCE_DYNAMIC)
	{
		return gDynamicVertices[vertexIndex];
	}

	return gStaticVertices[vertexIndex];
}

MaterialData GetMaterialData(uint materialIndex, uint dataSource)
{
	if (dataSource == SCENE_DATA_SOURCE_DYNAMIC)
	{
		return gDynamicMaterials[min(materialIndex, max(gTraceConstants.DynamicMaterialCount, 1u) - 1u)];
	}

	return gStaticMaterials[min(materialIndex, max(gTraceConstants.StaticMaterialCount, 1u) - 1u)];
}

PortalData GetPortalData(uint portalIndex)
{
	PortalData portal = (PortalData)0;
	portal.targetLocalSpaceIndex = 0xffffffffu;
	if (gTraceConstants.PortalCount == 0u || portalIndex == 0xffffffffu)
	{
		return portal;
	}

	return gScenePortals[min(portalIndex, gTraceConstants.PortalCount - 1u)];
}

float3 ResolveHitNormal(uint materialIndex, uint dataSource, float3 geometricNormal, float3 rayDirection)
{
	return normalize(geometricNormal);
}

float3 ResolveHitBarycentricWeights(HitData hit)
{
	return float3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics.x, hit.barycentrics.y);
}

float3 ResolveHitVertexPosition(HitData hit, bool previous)
{
	const PrimitiveData primitive = GetPrimitiveData(hit.dataSource, hit.primitiveIndex);
	const SceneVertex v0 = GetVertexData(hit.dataSource, primitive.indices.x);
	const SceneVertex v1 = GetVertexData(hit.dataSource, primitive.indices.y);
	const SceneVertex v2 = GetVertexData(hit.dataSource, primitive.indices.z);
	const float3 weights = ResolveHitBarycentricWeights(hit);
	const float3 p0 = previous ? v0.prevPosition : v0.position;
	const float3 p1 = previous ? v1.prevPosition : v1.position;
	const float3 p2 = previous ? v2.prevPosition : v2.position;
	return p0 * weights.x + p1 * weights.y + p2 * weights.z;
}

float3 GeneratePrimaryRay(uint2 pixelPos)
{
	float2 resolution = float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
	float2 jitter = GetCurrentTemporalJitter();
	float2 uv = ((float2)pixelPos + 0.5 + jitter) / resolution;
	float2 ndc = uv * 2.0 - 1.0;
	ndc.y = -ndc.y;

	float3 ray =
		gTraceConstants.CameraForward +
		ndc.x * gTraceConstants.TanHalfFovX * gTraceConstants.CameraRight +
		ndc.y * gTraceConstants.TanHalfFovY * gTraceConstants.CameraUp;

	return normalize(ray);
}

float2 ProjectWorldToUv(float3 worldPos, float3 cameraPos, float3 cameraForward, float3 cameraRight, float3 cameraUp, float tanHalfFovX, float tanHalfFovY, float2 jitter)
{
	float3 relative = worldPos - cameraPos;
	float viewZ = dot(relative, cameraForward);
	if (viewZ <= 0.001)
	{
		return float2(-1.0, -1.0);
	}

	float ndcX = dot(relative, cameraRight) / max(viewZ * tanHalfFovX, 1e-5);
	float ndcY = dot(relative, cameraUp) / max(viewZ * tanHalfFovY, 1e-5);
	float2 uv = float2(ndcX * 0.5 + 0.5, 0.5 - ndcY * 0.5);
	return uv - jitter / float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
}

float4 SampleSurfaceColor(uint materialIndex, uint dataSource, float2 uv)
{
	MaterialData material = GetMaterialData(materialIndex, dataSource);
	const bool indexed = (material.flags & MATERIAL_FLAG_INDEXED) != 0;
	float4 color = 0.0;
	if (indexed)
	{
		color = gSceneTextures[min(material.textureIndex, MAX_SCENE_TEXTURES - 1)].SampleLevel(gPointWrap, uv, 0.0);
	}
	else
	{
		color = gSceneTextures[min(material.textureIndex, MAX_SCENE_TEXTURES - 1)].SampleLevel(gLinearWrap, uv, 0.0);
	}

	if (indexed)
	{
		float paletteValue = saturate(color.r) * 255.0;
		float2 paletteUv = float2((paletteValue + 0.5) / 256.0, ((float)material.paletteIndex + 0.5) / 256.0);
		color = gPaletteLookup.SampleLevel(gPointClamp, paletteUv, 0.0);
	}

	color.rgb *= material.lightLevel;
	return color;
}

float4 SampleSurfaceColorRaw(uint materialIndex, uint dataSource, float2 uv)
{
	MaterialData material = GetMaterialData(materialIndex, dataSource);
	const bool indexed = (material.flags & MATERIAL_FLAG_INDEXED) != 0;
	if (indexed)
	{
		return gSceneTextures[min(material.textureIndex, MAX_SCENE_TEXTURES - 1)].SampleLevel(gPointWrap, uv, 0.0);
	}

	return gSceneTextures[min(material.textureIndex, MAX_SCENE_TEXTURES - 1)].SampleLevel(gLinearWrap, uv, 0.0);
}

bool IsTransparentSurfaceSample(uint materialIndex, uint dataSource, float2 uv)
{
	const MaterialData material = GetMaterialData(materialIndex, dataSource);
	const float4 rawSample = SampleSurfaceColorRaw(materialIndex, dataSource, uv);
	if ((material.flags & MATERIAL_FLAG_INDEXED) != 0)
	{
		// In the paletted path, color index 0 is reserved as transparent.
		const uint paletteIndex = (uint)round(saturate(rawSample.r) * 255.0);
		return paletteIndex == 0u;
	}

	return rawSample.a < 0.5;
}

bool IsMirrorMaterial(uint materialIndex, uint dataSource)
{
	return (GetMaterialData(materialIndex, dataSource).flags & MATERIAL_FLAG_MIRROR) != 0;
}

uint GetPortalTraversalDepth()
{
	return gTraceConstants.PortalDepth;
}

bool ResolvePortalHit(HitData hit, out PortalData portalData)
{
	portalData = GetPortalData(hit.portalIndex);
	return hit.portalIndex != 0xffffffffu && portalData.traversalClass != PORTAL_TRAVERSAL_CLASS_NONE;
}

bool ShouldIgnoreOneWayHit(uint materialIndex, uint dataSource, float3 geometricNormal, float3 rayDirection)
{
	if ((GetMaterialData(materialIndex, dataSource).flags & MATERIAL_FLAG_ONE_WAY) == 0)
	{
		return false;
	}

	return dot(normalize(geometricNormal), rayDirection) > 0.0;
}

uint ResolvePrimitiveIndex(SceneInstanceData instanceData, uint localPrimitiveIndex)
{
	return instanceData.primitiveOffset + localPrimitiveIndex;
}

bool IntersectPrimitiveTriangle(float3 origin, float3 direction, uint primitiveIndex, out float hitT, out float3 barycentrics)
{
	hitT = 0.0;
	barycentrics = 0.0;
	const PrimitiveData primitive = GetPrimitiveData(SCENE_DATA_SOURCE_DYNAMIC, primitiveIndex);
	const SceneVertex v0 = gDynamicVertices[primitive.indices.x];
	const SceneVertex v1 = gDynamicVertices[primitive.indices.y];
	const SceneVertex v2 = gDynamicVertices[primitive.indices.z];
	const float3 edge1 = v1.position - v0.position;
	const float3 edge2 = v2.position - v0.position;
	const float3 p = cross(direction, edge2);
	const float det = dot(edge1, p);
	if (abs(det) < 1e-5)
	{
		return false;
	}

	const float invDet = 1.0 / det;
	const float3 t = origin - v0.position;
	const float u = dot(t, p) * invDet;
	if (u < 0.0 || u > 1.0)
	{
		return false;
	}

	const float3 q = cross(t, edge1);
	const float v = dot(direction, q) * invDet;
	if (v < 0.0 || (u + v) > 1.0)
	{
		return false;
	}

	const float candidateT = dot(edge2, q) * invDet;
	if (candidateT <= 0.001)
	{
		return false;
	}

	hitT = candidateT;
	barycentrics = float3(1.0 - u - v, u, v);
	return true;
}

HitData TraceBootstrapGeometry(float3 origin, float3 direction)
{
	HitData bestHit = MakeEmptyHitData();
	bestHit.distance = 1e30;

	[loop]
	for (uint primitiveIndex = 0; primitiveIndex < gTraceConstants.DynamicPrimitiveCount; ++primitiveIndex)
	{
		float hitT = 0.0;
		float3 barycentrics = 0.0;
		if (!IntersectPrimitiveTriangle(origin, direction, primitiveIndex, hitT, barycentrics))
		{
			continue;
		}

		if (hitT >= bestHit.distance)
		{
			continue;
		}

		const PrimitiveData primitive = GetPrimitiveData(SCENE_DATA_SOURCE_DYNAMIC, primitiveIndex);
		if (ShouldIgnoreOneWayHit(primitive.materialIndex, SCENE_DATA_SOURCE_DYNAMIC, primitive.normal, direction))
		{
			continue;
		}

		const float2 uv = primitive.uv0 * barycentrics.x + primitive.uv1 * barycentrics.y + primitive.uv2 * barycentrics.z;
		if (IsTransparentSurfaceSample(primitive.materialIndex, SCENE_DATA_SOURCE_DYNAMIC, uv))
		{
			continue;
		}

		bestHit.hit = true;
		bestHit.dataSource = SCENE_DATA_SOURCE_DYNAMIC;
		bestHit.primitiveIndex = primitiveIndex;
		bestHit.portalIndex = primitive.portalIndex;
		bestHit.barycentrics = barycentrics.yz;
		bestHit.distance = hitT;
		bestHit.position = origin + direction * hitT;
		bestHit.normal = ResolveHitNormal(primitive.materialIndex, SCENE_DATA_SOURCE_DYNAMIC, primitive.normal, direction);
		bestHit.uv = uv;
		bestHit.materialIndex = primitive.materialIndex;
	}

	return bestHit;
}

bool TraceClosestSurface(float3 startOrigin, float3 direction, float maxDistance, out HitData hitData)
{
	hitData = MakeEmptyHitData();
	float accumulatedDistance = 0.0;

	[loop]
	for (uint skipCount = 0u; skipCount < 8u; ++skipCount)
	{
		const float remainingDistance = maxDistance - accumulatedDistance;
		if (remainingDistance <= 0.001)
		{
			return false;
		}

		RayQuery<RAY_FLAG_FORCE_OPAQUE> rayQuery;
		RayDesc ray = { startOrigin + direction * accumulatedDistance, 0.001, direction, remainingDistance };
		rayQuery.TraceRayInline(gWorldTlas, RAY_FLAG_FORCE_OPAQUE, 0xFF, ray);

		while (rayQuery.Proceed()) {}

		if (rayQuery.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
		{
			return false;
		}

		const SceneInstanceData instanceData = GetSceneInstanceData(rayQuery.CommittedInstanceID());
		const uint primitiveIndex = ResolvePrimitiveIndex(instanceData, rayQuery.CommittedPrimitiveIndex());
		const PrimitiveData primitive = GetPrimitiveData(instanceData.dataSource, primitiveIndex);
		const float committedDistance = rayQuery.CommittedRayT();
		if (ShouldIgnoreOneWayHit(primitive.materialIndex, instanceData.dataSource, primitive.normal, direction))
		{
			accumulatedDistance += committedDistance + 0.01;
			continue;
		}

		const float2 bary = rayQuery.CommittedTriangleBarycentrics();
		const float3 weights = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
		const float2 uv = primitive.uv0 * weights.x + primitive.uv1 * weights.y + primitive.uv2 * weights.z;
		if (IsTransparentSurfaceSample(primitive.materialIndex, instanceData.dataSource, uv))
		{
			accumulatedDistance += committedDistance + 0.01;
			continue;
		}

		const float hitDistance = accumulatedDistance + committedDistance;
		hitData.hit = true;
		hitData.dataSource = instanceData.dataSource;
		hitData.primitiveIndex = primitiveIndex;
		hitData.portalIndex = primitive.portalIndex;
		hitData.barycentrics = bary;
		hitData.distance = hitDistance;
		hitData.position = startOrigin + direction * hitDistance;
		hitData.normal = ResolveHitNormal(primitive.materialIndex, instanceData.dataSource, primitive.normal, direction);
		hitData.uv = uv;
		hitData.materialIndex = primitive.materialIndex;
		return true;
	}

	return false;
}

bool TraceScenePath(float3 startOrigin, float3 startDirection, float maxDistance, uint mirrorBudget, uint portalBudget, out HitData hitData, out float3 exitDirection)
{
	hitData = MakeEmptyHitData();
	exitDirection = startDirection;
	float3 origin = startOrigin;
	float3 direction = startDirection;
	float remainingDistance = maxDistance;

	[loop]
	for (uint continuationStep = 0u; continuationStep < 32u; ++continuationStep)
	{
		if (!TraceClosestSurface(origin, direction, remainingDistance, hitData))
		{
			exitDirection = direction;
			return false;
		}

		PortalData portalData = (PortalData)0;
		const bool hasPortalData = ResolvePortalHit(hitData, portalData);
		const bool reflectivePortal = hasPortalData && portalData.traversalClass == PORTAL_TRAVERSAL_CLASS_REFLECTIVE;
		const bool transferPortal = hasPortalData && portalData.traversalClass == PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER;
		const bool reflectiveSurface = reflectivePortal || IsMirrorMaterial(hitData.materialIndex, hitData.dataSource);

		if (reflectiveSurface && mirrorBudget > 0u)
		{
			remainingDistance = max(remainingDistance - hitData.distance, 0.0);
			origin = hitData.position + hitData.normal * 0.05;
			direction = reflect(direction, hitData.normal);
			exitDirection = direction;
			mirrorBudget--;
			continue;
		}

		if (transferPortal && portalBudget > 0u)
		{
			remainingDistance = max(remainingDistance - hitData.distance, 0.0);
			origin = hitData.position + direction * 0.05 + portalData.delta;
			exitDirection = direction;
			portalBudget--;
			continue;
		}

		exitDirection = direction;
		return true;
	}

	exitDirection = direction;
	return false;
}

HitData TracePrimary(float3 origin, float3 direction, out float3 exitDirection)
{
	HitData hitData = MakeEmptyHitData();
	const uint mirrorBudget = max(1u, (gTraceConstants.BounceCounts >> 16) & 0xffffu);
	TraceScenePath(origin, direction, 100000.0, mirrorBudget, GetPortalTraversalDepth(), hitData, exitDirection);
	return hitData;
}

HitData TracePrimary(float3 origin, float3 direction)
{
	float3 exitDirection = direction;
	return TracePrimary(origin, direction, exitDirection);
}

float ComputeSunShadow(float3 position, float3 normal, float3 lightDirection, out float shadowHitDistance)
{
	HitData shadowHit = MakeEmptyHitData();
	float3 ignoredDirection = lightDirection;
	const bool blocked = TraceScenePath(position + normal * 0.05, lightDirection, 100000.0, 0u, GetPortalTraversalDepth(), shadowHit, ignoredDirection);
	shadowHitDistance = blocked ? shadowHit.distance : NRD_FP16_MAX;
	return blocked ? 0.0 : 1.0;
}

float ComputeSunShadow(float3 position, float3 normal, float3 lightDirection)
{
	float shadowHitDistance = 0.0;
	return ComputeSunShadow(position, normal, lightDirection, shadowHitDistance);
}

float ComputePointLightShadow(float3 position, float3 normal, float3 lightDirection, float lightDistance)
{
	if (lightDistance <= 0.051)
	{
		return 1.0;
	}

	HitData shadowHit = MakeEmptyHitData();
	float3 ignoredDirection = lightDirection;
	const float maxDistance = max(lightDistance - 0.05, 0.001);
	const bool blocked = TraceScenePath(position + normal * 0.05, lightDirection, maxDistance, 0u, GetPortalTraversalDepth(), shadowHit, ignoredDirection);
	return blocked ? 0.0 : 1.0;
}

float3 GetMissColor(float3 direction)
{
	return gSkyTexture.SampleLevel(gLinearClamp, normalize(direction), 0.0).rgb;
}

#endif
