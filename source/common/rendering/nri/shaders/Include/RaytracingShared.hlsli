#ifndef RAZE_NRI_PT_RAYTRACING_SHARED_HLSLI
#define RAZE_NRI_PT_RAYTRACING_SHARED_HLSLI

#include "Shared.hlsli"

struct HitData
{
	bool hit;
	uint primitiveIndex;
	float2 barycentrics;
	float distance;
	float3 position;
	float3 normal;
	float2 uv;
	uint materialIndex;
};

MaterialData GetMaterialData(uint materialIndex)
{
	return gMaterials[min(materialIndex, max(gTraceConstants.MaterialCount, 1u) - 1u)];
}

float3 ResolveHitNormal(uint materialIndex, float3 geometricNormal, float3 rayDirection)
{
	float3 normal = normalize(geometricNormal);
	return normal;
}

float3 GeneratePrimaryRay(uint2 pixelPos)
{
	float2 resolution = float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
	float2 uv = ((float2)pixelPos + 0.5) / resolution;
	float2 ndc = uv * 2.0 - 1.0;
	ndc.y = -ndc.y;

	float3 ray =
		gTraceConstants.CameraForward +
		ndc.x * gTraceConstants.TanHalfFovX * gTraceConstants.CameraRight +
		ndc.y * gTraceConstants.TanHalfFovY * gTraceConstants.CameraUp;

	return normalize(ray);
}

float2 ProjectWorldToUv(float3 worldPos, float3 cameraPos, float3 cameraForward, float3 cameraRight, float3 cameraUp, float tanHalfFovX, float tanHalfFovY)
{
	float3 relative = worldPos - cameraPos;
	float viewZ = dot(relative, cameraForward);
	if (viewZ <= 0.001)
	{
		return float2(-1.0, -1.0);
	}

	float ndcX = dot(relative, cameraRight) / max(viewZ * tanHalfFovX, 1e-5);
	float ndcY = dot(relative, cameraUp) / max(viewZ * tanHalfFovY, 1e-5);
	return float2(ndcX * 0.5 + 0.5, 0.5 - ndcY * 0.5);
}

float4 SampleSurfaceColor(uint materialIndex, float2 uv)
{
	MaterialData material = GetMaterialData(materialIndex);
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

bool IsMirrorMaterial(uint materialIndex)
{
	return (GetMaterialData(materialIndex).flags & MATERIAL_FLAG_MIRROR) != 0;
}

bool ShouldIgnoreOneWayHit(uint materialIndex, float3 geometricNormal, float3 rayDirection)
{
	if ((GetMaterialData(materialIndex).flags & MATERIAL_FLAG_ONE_WAY) == 0)
	{
		return false;
	}

	return dot(normalize(geometricNormal), rayDirection) > 0.0;
}

uint ResolvePrimitiveIndex(uint instanceId, uint localPrimitiveIndex)
{
	return instanceId + localPrimitiveIndex;
}

bool IntersectPrimitiveTriangle(float3 origin, float3 direction, uint primitiveIndex, out float hitT, out float3 barycentrics)
{
	hitT = 0.0;
	barycentrics = 0.0;
	const PrimitiveData primitive = gPrimitives[min(primitiveIndex, gTraceConstants.PrimitiveCount - 1u)];
	const SceneVertex v0 = gVertices[primitive.indices.x];
	const SceneVertex v1 = gVertices[primitive.indices.y];
	const SceneVertex v2 = gVertices[primitive.indices.z];
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
	HitData bestHit = (HitData)0;
	bestHit.distance = 1e30;

	[loop]
	for (uint primitiveIndex = 0; primitiveIndex < gTraceConstants.PrimitiveCount; ++primitiveIndex)
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

		const PrimitiveData primitive = gPrimitives[primitiveIndex];
		if (ShouldIgnoreOneWayHit(primitive.materialIndex, primitive.normal, direction))
		{
			continue;
		}

		bestHit.hit = true;
		bestHit.primitiveIndex = primitiveIndex;
		bestHit.barycentrics = barycentrics.yz;
		bestHit.distance = hitT;
		bestHit.position = origin + direction * hitT;
		bestHit.normal = ResolveHitNormal(primitive.materialIndex, primitive.normal, direction);
		bestHit.uv = primitive.uv0 * barycentrics.x + primitive.uv1 * barycentrics.y + primitive.uv2 * barycentrics.z;
		bestHit.materialIndex = primitive.materialIndex;
	}

	return bestHit;
}

bool TraceClosestSurface(float3 startOrigin, float3 direction, float maxDistance, out HitData hitData)
{
	hitData = (HitData)0;
	float accumulatedDistance = 0.0;

	[loop]
	for (uint skipCount = 0u; skipCount < 8u; ++skipCount)
	{
		const float remainingDistance = maxDistance - accumulatedDistance;
		if (remainingDistance <= 0.001)
		{
			return false;
		}

		RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> rayQuery;
		RayDesc ray = { startOrigin + direction * accumulatedDistance, 0.001, direction, remainingDistance };
		rayQuery.TraceRayInline(gWorldTlas, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, ray);

		while (rayQuery.Proceed()) {}

		if (rayQuery.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
		{
			return false;
		}

		const uint primitiveIndex = ResolvePrimitiveIndex(rayQuery.CommittedInstanceID(), rayQuery.CommittedPrimitiveIndex());
		const PrimitiveData primitive = gPrimitives[min(primitiveIndex, gTraceConstants.PrimitiveCount - 1)];
		const float committedDistance = rayQuery.CommittedRayT();
		if (ShouldIgnoreOneWayHit(primitive.materialIndex, primitive.normal, direction))
		{
			accumulatedDistance += committedDistance + 0.01;
			continue;
		}

		const float2 bary = rayQuery.CommittedTriangleBarycentrics();
		const float3 weights = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
		const float hitDistance = accumulatedDistance + committedDistance;
		hitData.hit = true;
		hitData.primitiveIndex = primitiveIndex;
		hitData.barycentrics = bary;
		hitData.distance = hitDistance;
		hitData.position = startOrigin + direction * hitDistance;
		hitData.normal = ResolveHitNormal(primitive.materialIndex, primitive.normal, direction);
		hitData.uv = primitive.uv0 * weights.x + primitive.uv1 * weights.y + primitive.uv2 * weights.z;
		hitData.materialIndex = primitive.materialIndex;
		return true;
	}

	return false;
}

HitData TracePrimary(float3 origin, float3 direction)
{
	HitData hitData = (HitData)0;
	TraceClosestSurface(origin, direction, 100000.0, hitData);
	return hitData;
}

float ComputeSunShadow(float3 position, float3 normal, float3 lightDirection)
{
	HitData shadowHit = (HitData)0;
	return TraceClosestSurface(position + normal * 0.05, lightDirection, 100000.0, shadowHit) ? 0.0 : 1.0;
}

float3 GetMissColor(float3 direction)
{
	return gSkyTexture.SampleLevel(gLinearClamp, normalize(direction), 0.0).rgb;
}

#endif
