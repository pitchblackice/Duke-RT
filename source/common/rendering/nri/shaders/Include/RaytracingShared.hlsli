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
	float4 color = gSceneTextures[min(material.textureIndex, MAX_SCENE_TEXTURES - 1)].SampleLevel(gLinearClamp, uv, 0.0);

	if ((material.flags & MATERIAL_FLAG_INDEXED) != 0)
	{
		float paletteValue = saturate(color.r) * 255.0;
		float2 paletteUv = float2((paletteValue + 0.5) / 256.0, ((float)material.paletteIndex + 0.5) / 256.0);
		color = gPaletteLookup.SampleLevel(gLinearClamp, paletteUv, 0.0);
	}

	color.rgb *= material.lightLevel;
	return color;
}

bool IsMirrorMaterial(uint materialIndex)
{
	return (GetMaterialData(materialIndex).flags & MATERIAL_FLAG_MIRROR) != 0;
}

HitData TracePrimary(float3 origin, float3 direction)
{
	HitData hitData = (HitData)0;

	RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> rayQuery;
	RayDesc ray = { origin, 0.001, direction, 100000.0 };
	rayQuery.TraceRayInline(gWorldTlas, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, ray);

	while (rayQuery.Proceed()) {}

	if (rayQuery.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
	{
		return hitData;
	}

	const uint primitiveIndex = rayQuery.CommittedPrimitiveIndex();
	const PrimitiveData primitive = gPrimitives[min(primitiveIndex, gTraceConstants.PrimitiveCount - 1)];
	const SceneVertex v0 = gVertices[primitive.indices.x];
	const SceneVertex v1 = gVertices[primitive.indices.y];
	const SceneVertex v2 = gVertices[primitive.indices.z];
	const float2 bary = rayQuery.CommittedTriangleBarycentrics();
	const float3 weights = float3(1.0 - bary.x - bary.y, bary.x, bary.y);

	hitData.hit = true;
	hitData.primitiveIndex = primitiveIndex;
	hitData.barycentrics = bary;
	hitData.distance = rayQuery.CommittedRayT();
	hitData.position = origin + direction * hitData.distance;
	hitData.normal = normalize(primitive.normal);
	hitData.uv = primitive.uv0 * weights.x + primitive.uv1 * weights.y + primitive.uv2 * weights.z;
	hitData.materialIndex = primitive.materialIndex;
	return hitData;
}

float ComputeSunShadow(float3 position, float3 normal)
{
	RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> shadowQuery;
	RayDesc shadowRay = { position + normal * 0.05, 0.001, gTraceConstants.LightDirection, 100000.0 };
	shadowQuery.TraceRayInline(gWorldTlas, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, shadowRay);

	while (shadowQuery.Proceed()) {}

	return shadowQuery.CommittedStatus() == COMMITTED_NOTHING ? 1.0 : 0.0;
}

float3 GetMissColor(float3 direction)
{
	float skyT = saturate(direction.y * 0.5 + 0.5);
	return lerp(gTraceConstants.GroundColor, gTraceConstants.SkyColor, skyT);
}

#endif
