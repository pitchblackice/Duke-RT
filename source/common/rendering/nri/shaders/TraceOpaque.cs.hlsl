#include "Include/Shared.hlsli"
#include "Include/RaytracingShared.hlsli"

uint Hash32(uint value)
{
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	value ^= value >> 16;
	return value;
}

float RandomFloat01(inout uint state)
{
	state = Hash32(state);
	return (float)(state & 0x00ffffffu) * (1.0 / 16777216.0);
}

float3 BuildOrthonormalTangent(float3 n)
{
	const float3 up = abs(n.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
	return normalize(cross(up, n));
}

float3 SampleSunDirection(float3 lightDir, uint2 pixelPos, uint frameIndex)
{
	uint rngState = pixelPos.x * 73856093u ^ pixelPos.y * 19349663u ^ (frameIndex + 1u) * 83492791u;
	const float sunAngularRadius = 0.03;
	const float cosTheta = lerp(cos(sunAngularRadius), 1.0, RandomFloat01(rngState));
	const float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
	const float phi = 6.28318530718 * RandomFloat01(rngState);
	const float3 tangent = BuildOrthonormalTangent(lightDir);
	const float3 bitangent = normalize(cross(lightDir, tangent));
	return normalize(lightDir * cosTheta + tangent * (cos(phi) * sinTheta) + bitangent * (sin(phi) * sinTheta));
}

float3 SampleCosineHemisphere(float3 normal, inout uint rngState)
{
	const float u1 = RandomFloat01(rngState);
	const float u2 = RandomFloat01(rngState);
	const float r = sqrt(u1);
	const float phi = 6.28318530718 * u2;
	const float x = r * cos(phi);
	const float y = r * sin(phi);
	const float z = sqrt(saturate(1.0 - u1));
	const float3 tangent = BuildOrthonormalTangent(normal);
	const float3 bitangent = normalize(cross(normal, tangent));
	return normalize(tangent * x + bitangent * y + normal * z);
}

float3 SampleSpecularLobe(float3 reflectionDir, float roughness, inout uint rngState)
{
	if (roughness <= 0.02)
	{
		return reflectionDir;
	}

	const float3 blurred = SampleCosineHemisphere(reflectionDir, rngState);
	const float blurAmount = saturate(roughness * roughness * 1.5);
	return normalize(lerp(reflectionDir, blurred, blurAmount));
}

uint GetLightBounceCount()
{
	return gTraceConstants.BounceCounts & 0xffffu;
}

uint GetMirrorBounceCount()
{
	return (gTraceConstants.BounceCounts >> 16) & 0xffffu;
}

bool UseSplitShadowDenoiser()
{
	return (gTraceConstants.Flags & 0x20u) != 0;
}

bool UseDirectionalPlaceholderLight()
{
	return (gTraceConstants.Flags & 0x80u) != 0;
}

bool UseRelaxDenoiser()
{
	return (gTraceConstants.ReservedTrace1 & 0xffu) == 1u;
}

uint GetEmissiveDirectSampleCount()
{
	return clamp((gTraceConstants.ReservedTrace1 >> 8u) & 0xffu, 1u, 4u);
}

float3 EvaluateSunDiffuseLighting(float3 normal, float3 lightDir, float shadow)
{
	const float lambert = max(dot(normal, lightDir), 0.0);
	const float lighting = 0.20 + shadow * lambert * 0.80;
	return lighting.xxx;
}

float3 EvaluateAmbientDiffuse(float3 albedo)
{
	return albedo * 0.20;
}

float3 EvaluateDirectSunDiffuse(float3 albedo, float3 normal, float3 lightDir)
{
	const float lambert = max(dot(normal, lightDir), 0.0);
	return albedo * (lambert * 0.80);
}

float3 EvaluateSunSpecular(float3 albedo, float metalness, float3 normal, float3 viewDir, float3 lightDir, float shadow)
{
	const float lambert = max(dot(normal, lightDir), 0.0);
	const float3 halfVector = normalize(lightDir + viewDir);
	const float ndoth = max(dot(normal, halfVector), 0.0);
	const float vdoth = max(dot(viewDir, halfVector), 0.0);
	const float fresnel = pow(1.0 - vdoth, 5.0);
	const float3 dielectricF0 = lerp(float3(0.04, 0.04, 0.04), albedo, metalness);
	const float3 specularColor = lerp(dielectricF0, float3(1.0, 1.0, 1.0), fresnel);
	const float specularTerm = pow(ndoth, 12.0) * shadow * (0.5 + 0.5 * lambert);
	return specularColor * specularTerm * 0.85;
}

float EvaluatePointLightAttenuation(float distance, float radius, float intensity)
{
	if (radius <= 0.0 || distance >= radius)
	{
		return 0.0;
	}

	const float normalizedDistance = saturate(distance / radius);
	const float softRange = 1.0 - normalizedDistance;
	const float smoothRange = softRange * softRange * (3.0 - 2.0 * softRange);
	const float shapedFalloff = rcp(1.0 + 4.0 * normalizedDistance * normalizedDistance);
	return intensity * smoothRange * smoothRange * shapedFalloff;
}

RuntimeLightTileHeaderData GetRuntimeLightTileHeader(uint2 pixelPos)
{
	RuntimeLightTileHeaderData header = { 0u, 0u };
	const uint2 tileCounts = uint2(gTraceConstants.ReservedTrace0 & 0xffffu, gTraceConstants.ReservedTrace0 >> 16u);
	static const uint kRuntimeLightTileSize = 64u;
	if (gTraceConstants.RuntimeLightCount == 0u ||
		tileCounts.x == 0u ||
		tileCounts.y == 0u)
	{
		return header;
	}

	const uint tileX = min(pixelPos.x / kRuntimeLightTileSize, tileCounts.x - 1u);
	const uint tileY = min(pixelPos.y / kRuntimeLightTileSize, tileCounts.y - 1u);
	return gRuntimeLightTileHeaders[tileY * tileCounts.x + tileX];
}

float GetSurfaceRoughness(MaterialData material)
{
	return clamp(material.roughnessHint, 0.02, 1.0);
}

static const float kSunAngularRadius = 0.03;
static const float kTanSunAngularRadius = 0.0300090032;

static const float4 kReblurHitDistanceParams = float4(3.0, 0.1, 20.0, -25.0);

float GetNormalizedReblurHitDistance(float hitDistance, float viewZ, float roughness)
{
	const float trimmedHitDistance = NRD_FrontEnd_TrimHitDistance(max(hitDistance, 0.0), 0.001);
	return REBLUR_FrontEnd_GetNormHitDist(trimmedHitDistance, abs(viewZ), kReblurHitDistanceParams, roughness);
}

float4 PackReblurDiffuseRadiance(float3 radiance, float hitDistance, float viewZ)
{
	const float normHitDistance = GetNormalizedReblurHitDistance(hitDistance, viewZ, 1.0);
	return REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normHitDistance, true);
}

float4 PackReblurSpecularRadiance(float3 radiance, float hitDistance, float viewZ, float roughness)
{
	const float normHitDistance = GetNormalizedReblurHitDistance(hitDistance, viewZ, roughness);
	return REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normHitDistance, true);
}

float4 PackDiffuseRadiance(float3 radiance, float hitDistance, float viewZ)
{
	if (UseRelaxDenoiser())
	{
		return RELAX_FrontEnd_PackRadianceAndHitDist(radiance, max(hitDistance, 0.0), true);
	}

	return PackReblurDiffuseRadiance(radiance, hitDistance, viewZ);
}

float4 PackSpecularRadiance(float3 radiance, float hitDistance, float viewZ, float roughness)
{
	if (UseRelaxDenoiser())
	{
		return RELAX_FrontEnd_PackRadianceAndHitDist(radiance, max(hitDistance, 0.0), true);
	}

	return PackReblurSpecularRadiance(radiance, hitDistance, viewZ, roughness);
}

float GetSurfaceMetalness(MaterialData material)
{
	return saturate(material.metalnessHint);
}

float GetSurfaceMaterialID(MaterialData material)
{
	return min((float)material.materialClass, 3.0);
}

bool IsMaterialEmissive(MaterialData material)
{
	return material.emissiveMode != 0u && material.emissiveIntensity > 0.0;
}

float3 EvaluateMaterialEmission(uint materialIndex, uint dataSource, MaterialData material, float2 uv)
{
	if (material.emissiveMode != 0u)
	{
		return SampleMaterialEmissionSource(materialIndex, dataSource, uv) * material.emissiveIntensity;
	}
	return 0.0;
}

uint GetEmissivePrimitiveCount()
{
	return gEmissivePrimitiveHeaders[0].activeCount;
}

uint GetSectorLightCount()
{
	return gSectorLightHeaders[0].sectorCount;
}

uint SampleEmissivePrimitiveIndex(inout uint rngState)
{
	const uint emissiveCount = GetEmissivePrimitiveCount();
	if (emissiveCount == 0u)
	{
		return 0xffffffffu;
	}

	const float r = RandomFloat01(rngState);
	uint low = 0u;
	uint high = emissiveCount - 1u;
	[unroll]
	for (uint i = 0u; i < 12u && low < high; ++i)
	{
		const uint mid = (low + high) >> 1u;
		if (r <= gEmissivePrimitiveCdf[mid])
		{
			high = mid;
		}
		else
		{
			low = mid + 1u;
		}
	}

	return low;
}

float3 SamplePointOnPrimitive(uint dataSource, PrimitiveData primitive, inout uint rngState, out float2 outUv, out float3 outNormal)
{
	const SceneVertex v0 = GetVertexData(dataSource, primitive.indices.x);
	const SceneVertex v1 = GetVertexData(dataSource, primitive.indices.y);
	const SceneVertex v2 = GetVertexData(dataSource, primitive.indices.z);
	const float u1 = RandomFloat01(rngState);
	const float u2 = RandomFloat01(rngState);
	const float rootU1 = sqrt(saturate(u1));
	const float3 barycentrics = float3(1.0 - rootU1, rootU1 * (1.0 - u2), rootU1 * u2);
	outUv = primitive.uv0 * barycentrics.x + primitive.uv1 * barycentrics.y + primitive.uv2 * barycentrics.z;
	outNormal = normalize(primitive.normal);
	return v0.position * barycentrics.x + v1.position * barycentrics.y + v2.position * barycentrics.z;
}

void EvaluateSampledEmissiveLighting(
	float3 position,
	float3 normal,
	float3 viewDir,
	float3 albedo,
	float metalness,
	inout uint rngState,
	bool traceVisibility,
	out float3 outDiffuse,
	out float3 outSpecular,
	out uint outPrimitiveIndex,
	out uint outDataSource,
	out bool outOccluded,
	out float2 outEmitterUv,
	out float3 outEmitterRadiance)
{
	outDiffuse = 0.0;
	outSpecular = 0.0;
	outPrimitiveIndex = 0xffffffffu;
	outDataSource = 0u;
	outOccluded = false;
	outEmitterUv = 0.0;
	outEmitterRadiance = 0.0;

	const uint candidateIndex = SampleEmissivePrimitiveIndex(rngState);
	if (candidateIndex == 0xffffffffu)
	{
		return;
	}

	const EmissivePrimitiveData candidate = gEmissivePrimitives[candidateIndex];
	outPrimitiveIndex = candidate.primitiveIndex;
	outDataSource = candidate.dataSource;
	const PrimitiveData primitive = GetPrimitiveData(candidate.dataSource, candidate.primitiveIndex);
	float2 lightUv = 0.0;
	float3 lightNormal = 0.0;
	const float3 lightPosition = SamplePointOnPrimitive(candidate.dataSource, primitive, rngState, lightUv, lightNormal);
	outEmitterUv = lightUv;
	const MaterialData lightMaterial = GetMaterialData(primitive.materialIndex, candidate.dataSource);
	const float3 lightColor = SampleMaterialEmissionSource(primitive.materialIndex, candidate.dataSource, lightUv) * lightMaterial.emissiveIntensity;
	outEmitterRadiance = lightColor;
	if (all(lightColor <= 0.0))
	{
		return;
	}

	const float3 toLight = lightPosition - position;
	const float lightDistanceSq = dot(toLight, toLight);
	if (lightDistanceSq <= 0.0001)
	{
		return;
	}

	const float lightDistance = sqrt(lightDistanceSq);
	const float3 lightDir = toLight / lightDistance;
	const float lambert = max(dot(normal, lightDir), 0.0);
	if (lambert <= 0.0)
	{
		return;
	}

	const float emitterLambert = max(dot(lightNormal, -lightDir), 0.0);
	if (emitterLambert <= 0.0)
	{
		return;
	}

	if (traceVisibility)
	{
		const float visibility = UseFastEmissiveShadow() ?
			ComputeFastPointLightShadow(position, normal, lightDir, lightDistance) :
			ComputePointLightShadow(position, normal, lightDir, lightDistance);
		if (visibility <= 0.0)
		{
			outOccluded = true;
			return;
		}
	}

	const float pdf = max(candidate.selectionPdf, 1e-4);
	const float projectedArea = max(candidate.primitiveArea * emitterLambert, 0.001);
	const float solidAngleEstimate = min(projectedArea / max(12.56637061436 * lightDistanceSq, 0.01), 1.0);
	const float sampleWeight = min(solidAngleEstimate / pdf, 16.0);
	outDiffuse = albedo * (lambert * 0.80) * lightColor * sampleWeight;
	outSpecular = EvaluateSunSpecular(albedo, metalness, normal, viewDir, lightDir, 1.0) * lightColor * sampleWeight;
}

float3 GetEmissivePrimitiveDebugColor(uint primitiveIndex, uint dataSource)
{
	if (primitiveIndex == 0xffffffffu)
	{
		return 0.0;
	}

	const float seed = (float)(primitiveIndex + 1u);
	const float3 hashedColor = 0.15 + 0.85 * float3(
		frac(seed * 0.1031),
		frac(seed * 0.11369 + 0.17),
		frac(seed * 0.13787 + 0.31));
	const float3 sourceTint = dataSource == 0u ? float3(1.0, 0.85, 0.55) : float3(0.55, 0.9, 1.0);
	return saturate(hashedColor * sourceTint);
}

float3 EvaluateSectorLightingSource(MaterialData material, float3 normal)
{
	if ((gSectorLightHeaders[0].flags & 0x1u) == 0u)
	{
		return 0.0;
	}

	const uint sectorIndex = material.sectorIndex;
	if (sectorIndex == 0xffffffffu || sectorIndex >= GetSectorLightCount())
	{
		return 0.0;
	}

	const SectorLightData sectorLight = gSectorLights[sectorIndex];
	const float contribution = sectorLight.ambientIntensity + abs(sectorLight.hemisphereAmount) + sectorLight.fogAmount;
	if (contribution <= 0.0)
	{
		return 0.0;
	}

	const float upFactor = saturate(normal.z * 0.5 + 0.5);
	const float hemisphereTerm = max(1.0 + sectorLight.hemisphereAmount * lerp(-1.0, 1.0, upFactor), 0.0);
	const float fogTerm = sectorLight.fogAmount * lerp(0.35, 1.0, upFactor);
	const float intensity = sectorLight.ambientIntensity * hemisphereTerm + fogTerm;
	return sectorLight.ambientColor * intensity;
}

float3 EvaluateSectorLighting(MaterialData material, float3 normal, float3 albedo)
{
	const float3 sourceLighting = EvaluateSectorLightingSource(material, normal);
	const float neutralAlbedo = dot(albedo, float3(0.2126, 0.7152, 0.0722));
	return sourceLighting * neutralAlbedo;
}

float3 GetSurfaceSpecularColor(float3 albedo, float metalness)
{
	return lerp(float3(0.04, 0.04, 0.04), albedo, metalness);
}

float3 TraceIndirectDiffuse(HitData surfaceHit, float3 surfaceAlbedo, uint2 pixelPos, uint frameIndex, uint bounceCount, out float outHitDistance)
{
	outHitDistance = 0.0;
	if (bounceCount == 0u)
	{
		return 0.0;
	}

	uint rngState = pixelPos.x * 73856093u ^ pixelPos.y * 19349663u ^ (frameIndex + 1u) * 83492791u ^ 0x9e3779b9u;
	float3 throughput = surfaceAlbedo;
	float3 indirectRadiance = 0.0;
	float3 origin = surfaceHit.position + surfaceHit.normal * 0.05;
	float3 direction = SampleCosineHemisphere(surfaceHit.normal, rngState);
	bool hasSecondaryHitDistance = false;
	float accumulatedSecondaryHitDistance = 0.0;

	[loop]
	for (uint bounce = 0u; bounce < bounceCount; ++bounce)
	{
		float3 tracedDirection = direction;
		const HitData bounceHit = TracePrimary(origin, direction, tracedDirection);
		if (!bounceHit.hit)
		{
			if (!hasSecondaryHitDistance)
			{
				accumulatedSecondaryHitDistance = NRD_INF;
				hasSecondaryHitDistance = true;
			}
			indirectRadiance += throughput * GetMissColor(tracedDirection);
			break;
		}

		accumulatedSecondaryHitDistance += bounceHit.distance;
		hasSecondaryHitDistance = true;
		const MaterialData bounceMaterial = GetMaterialData(bounceHit.materialIndex, bounceHit.dataSource);
		if ((bounceMaterial.flags & (MATERIAL_FLAG_MIRROR | MATERIAL_FLAG_PORTAL)) != 0)
		{
			break;
		}

		const float4 bounceAlbedo = SampleMaterialBaseColor(bounceHit.materialIndex, bounceHit.dataSource, bounceHit.uv);
		if (IsMaterialEmissive(bounceMaterial))
		{
			indirectRadiance += throughput * EvaluateMaterialEmission(bounceHit.materialIndex, bounceHit.dataSource, bounceMaterial, bounceHit.uv);
			break;
		}

		indirectRadiance += throughput * EvaluateSectorLighting(bounceMaterial, bounceHit.normal, bounceAlbedo.rgb);

		const float bounceMetalness = GetSurfaceMetalness(bounceMaterial);
		const float3 bounceViewDir = normalize(-tracedDirection);
		float3 bounceEmissiveDiffuse = 0.0;
		float3 bounceEmissiveSpecular = 0.0;
		uint bounceEmissivePrimitiveIndex = 0xffffffffu;
		uint bounceEmissiveDataSource = 0u;
		bool bounceEmissiveOccluded = false;
		float2 bounceEmissiveUv = 0.0;
		float3 bounceEmissiveRadiance = 0.0;
		EvaluateSampledEmissiveLighting(
			bounceHit.position,
			bounceHit.normal,
			bounceViewDir,
			bounceAlbedo.rgb,
			bounceMetalness,
			rngState,
			true,
			bounceEmissiveDiffuse,
			bounceEmissiveSpecular,
			bounceEmissivePrimitiveIndex,
			bounceEmissiveDataSource,
			bounceEmissiveOccluded,
			bounceEmissiveUv,
			bounceEmissiveRadiance);
		indirectRadiance += throughput * (bounceEmissiveDiffuse + bounceEmissiveSpecular);

		if (UseDirectionalPlaceholderLight())
		{
			const float3 bounceLightDir = SampleSunDirection(normalize(gTraceConstants.LightDirection), pixelPos + uint2(bounce + 1u, bounce * 3u + 1u), frameIndex + bounce + 1u);
			const float bounceShadow = ComputeSunShadow(bounceHit.position, bounceHit.normal, bounceLightDir);
			indirectRadiance += throughput * bounceAlbedo.rgb * EvaluateSunDiffuseLighting(bounceHit.normal, bounceLightDir, bounceShadow);
		}
		throughput *= bounceAlbedo.rgb * 0.65;
		if (max(throughput.r, max(throughput.g, throughput.b)) < 0.01)
		{
			break;
		}

		origin = bounceHit.position + bounceHit.normal * 0.05;
		direction = SampleCosineHemisphere(bounceHit.normal, rngState);
	}

	outHitDistance = hasSecondaryHitDistance ? accumulatedSecondaryHitDistance : 0.0;
	return indirectRadiance;
}

float3 TraceIndirectSpecular(HitData surfaceHit, float4 surfaceAlbedo, float3 viewDir, uint2 pixelPos, uint frameIndex, float roughness, uint bounceCount, out float outHitDistance)
{
	outHitDistance = 0.0;
	if (bounceCount == 0u)
	{
		return 0.0;
	}

	uint rngState = pixelPos.x * 73856093u ^ pixelPos.y * 19349663u ^ (frameIndex + 1u) * 83492791u ^ 0x85ebca6bu;
	float3 throughput = GetSurfaceSpecularColor(surfaceAlbedo.rgb, 0.0);
	float3 indirectRadiance = 0.0;
	float3 origin = surfaceHit.position + surfaceHit.normal * 0.05;
	float3 direction = SampleSpecularLobe(reflect(-viewDir, surfaceHit.normal), roughness, rngState);
	bool hasSecondaryHitDistance = false;

	[loop]
	for (uint bounce = 0u; bounce < bounceCount; ++bounce)
	{
		float3 tracedDirection = direction;
		const HitData bounceHit = TracePrimary(origin, direction, tracedDirection);
		if (!bounceHit.hit)
		{
			if (!hasSecondaryHitDistance)
			{
				outHitDistance = NRD_INF;
				hasSecondaryHitDistance = true;
			}
			indirectRadiance += throughput * GetMissColor(tracedDirection);
			break;
		}

		if (!hasSecondaryHitDistance)
		{
			outHitDistance = bounceHit.distance;
			hasSecondaryHitDistance = true;
		}
		const MaterialData bounceMaterial = GetMaterialData(bounceHit.materialIndex, bounceHit.dataSource);
		if ((bounceMaterial.flags & MATERIAL_FLAG_PORTAL) != 0)
		{
			break;
		}

		const float bounceRoughness = GetSurfaceRoughness(bounceMaterial);
		const float bounceMetalness = GetSurfaceMetalness(bounceMaterial);
		const float4 bounceAlbedo = SampleMaterialBaseColor(bounceHit.materialIndex, bounceHit.dataSource, bounceHit.uv);
		if (IsMaterialEmissive(bounceMaterial))
		{
			indirectRadiance += throughput * EvaluateMaterialEmission(bounceHit.materialIndex, bounceHit.dataSource, bounceMaterial, bounceHit.uv);
			break;
		}

		indirectRadiance += throughput * EvaluateSectorLighting(bounceMaterial, bounceHit.normal, bounceAlbedo.rgb);

		const float3 bounceViewDir = normalize(-tracedDirection);
		float3 bounceEmissiveDiffuse = 0.0;
		float3 bounceEmissiveSpecular = 0.0;
		uint bounceEmissivePrimitiveIndex = 0xffffffffu;
		uint bounceEmissiveDataSource = 0u;
		bool bounceEmissiveOccluded = false;
		float2 bounceEmissiveUv = 0.0;
		float3 bounceEmissiveRadiance = 0.0;
		EvaluateSampledEmissiveLighting(
			bounceHit.position,
			bounceHit.normal,
			bounceViewDir,
			bounceAlbedo.rgb,
			bounceMetalness,
			rngState,
			true,
			bounceEmissiveDiffuse,
			bounceEmissiveSpecular,
			bounceEmissivePrimitiveIndex,
			bounceEmissiveDataSource,
			bounceEmissiveOccluded,
			bounceEmissiveUv,
			bounceEmissiveRadiance);
		indirectRadiance += throughput * (bounceEmissiveDiffuse + bounceEmissiveSpecular);

		if (UseDirectionalPlaceholderLight())
		{
			const float3 bounceLightDir = SampleSunDirection(normalize(gTraceConstants.LightDirection), pixelPos + uint2(bounce * 5u + 1u, bounce * 7u + 3u), frameIndex + bounce + 1u);
			const float bounceShadow = ComputeSunShadow(bounceHit.position, bounceHit.normal, bounceLightDir);
			indirectRadiance += throughput * (
				bounceAlbedo.rgb * EvaluateSunDiffuseLighting(bounceHit.normal, bounceLightDir, bounceShadow) +
				EvaluateSunSpecular(bounceAlbedo.rgb, bounceMetalness, bounceHit.normal, bounceViewDir, bounceLightDir, bounceShadow));
		}

		throughput *= GetSurfaceSpecularColor(bounceAlbedo.rgb, bounceMetalness) * (0.9 - bounceRoughness * 0.35);
		if (max(throughput.r, max(throughput.g, throughput.b)) < 0.01)
		{
			break;
		}

		origin = bounceHit.position + bounceHit.normal * 0.05;
		direction = SampleSpecularLobe(reflect(tracedDirection, bounceHit.normal), bounceRoughness, rngState);
	}

	return indirectRadiance;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gTraceConstants.RenderWidth || dispatchThreadId.y >= gTraceConstants.RenderHeight)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	float3 visibleRayDirection = GeneratePrimaryRay(pixelPos);
	float3 rayOrigin = gTraceConstants.CameraPos;
	const uint bootstrapMode = gTraceConstants.BootstrapMode;
	const bool bootstrapSceneDirect = bootstrapMode == 11 || bootstrapMode == 12;
	const bool directSceneTrace = bootstrapSceneDirect || ((gTraceConstants.Flags & 0x8u) != 0);
	const bool bootstrapFlat = bootstrapMode == 11;
	const bool bootstrapBaseColor = bootstrapMode == 12;

	HitData hit = (HitData)0;
	if (directSceneTrace)
	{
		hit = TraceBootstrapGeometry(rayOrigin, visibleRayDirection);
	}
	else
	{
		float3 tracedVisibleDirection = visibleRayDirection;
		hit = TracePrimary(rayOrigin, visibleRayDirection, tracedVisibleDirection);
		visibleRayDirection = tracedVisibleDirection;
	}

	float4 color = 0.0;
	if (!hit.hit)
	{
		if (bootstrapFlat || bootstrapBaseColor)
		{
			const float3 sentinel = bootstrapFlat ? float3(1.0, 0.0, 1.0) : float3(1.0, 0.5, 0.0);
			color = float4(sentinel, 1.0);
			gMotionOutput[pixelPos] = float4(0.0, 0.0, 0.0, -1.0);
			gViewZOutput[pixelPos] = float4(1.0, 0.0, 0.0, 1.0);
			gNormalRoughnessOutput[pixelPos] = 0.0;
			gBaseColorOutput[pixelPos] = float4(sentinel, 1.0);
			gGuideDiffuseOutput[pixelPos] = float4(sentinel, 1.0);
			gGuideSpecularOutput[pixelPos] = float4(0.0, 0.0, 0.0, 1.0);
			gGuideSpecHitOutput[pixelPos] = float4(0.0, 0.0, 0.0, 1.0);
			gShadowPenumbraOutput[pixelPos] = float4(SIGMA_FrontEnd_PackPenumbra(NRD_FP16_MAX, kTanSunAngularRadius), 1.0, 0.0, 1.0);
			gDirectLightingOutput[pixelPos] = 0.0;
			gDirectEmissionOutput[pixelPos] = float4(sentinel, 1.0);
			if (gTraceConstants.DebugMode == 5)
			{
				gDirectLightingOutput[pixelPos] = 0.0;
				gDirectEmissionOutput[pixelPos] = 0.0;
			}
			else if (gTraceConstants.DebugMode == 34 || gTraceConstants.DebugMode == 35 || gTraceConstants.DebugMode == 36 || gTraceConstants.DebugMode == 37 || gTraceConstants.DebugMode == 38 || gTraceConstants.DebugMode == 39 || gTraceConstants.DebugMode == 40 || gTraceConstants.DebugMode == 42 || gTraceConstants.DebugMode == 43 || gTraceConstants.DebugMode == 44 || gTraceConstants.DebugMode == 45)
			{
				gDirectLightingOutput[pixelPos] = 0.0;
				gDirectEmissionOutput[pixelPos] = 0.0;
			}
		}
		else
		{
			const float3 missColor = GetMissColor(visibleRayDirection);
			const float4 packedDiffuse = PackDiffuseRadiance(missColor, NRD_INF, NRD_INF);
			color = (gTraceConstants.DebugMode >= 1 && gTraceConstants.DebugMode <= 4) ? float4(missColor, 1.0) : packedDiffuse;
			gMotionOutput[pixelPos] = float4(0.0, 0.0, 0.0, -NRD_INF);
			gViewZOutput[pixelPos] = float4(NRD_INF, 0.0, 0.0, 1.0);
			gNormalRoughnessOutput[pixelPos] = 0.0;
			gBaseColorOutput[pixelPos] = float4(missColor, 0.0);
			// Keep sky misses out of NRD's ordinary noisy radiance inputs and composite them via direct emission.
			gGuideDiffuseOutput[pixelPos] = 0.0;
			gGuideSpecularOutput[pixelPos] = 0.0;
			gGuideSpecHitOutput[pixelPos] = 0.0;
			gShadowPenumbraOutput[pixelPos] = float4(SIGMA_FrontEnd_PackPenumbra(NRD_FP16_MAX, kTanSunAngularRadius), 1.0, 0.0, 1.0);
			gDirectLightingOutput[pixelPos] = 0.0;
			gDirectEmissionOutput[pixelPos] = float4(missColor, 1.0);
			if (gTraceConstants.DebugMode == 5)
			{
				gDirectLightingOutput[pixelPos] = 0.0;
				gDirectEmissionOutput[pixelPos] = 0.0;
			}
			else if (gTraceConstants.DebugMode == 34 || gTraceConstants.DebugMode == 35 || gTraceConstants.DebugMode == 36 || gTraceConstants.DebugMode == 37 || gTraceConstants.DebugMode == 38 || gTraceConstants.DebugMode == 39 || gTraceConstants.DebugMode == 40 || gTraceConstants.DebugMode == 42 || gTraceConstants.DebugMode == 43 || gTraceConstants.DebugMode == 44 || gTraceConstants.DebugMode == 45)
			{
				gDirectLightingOutput[pixelPos] = 0.0;
				gDirectEmissionOutput[pixelPos] = 0.0;
				color = float4(1.0, 0.0, 1.0, 1.0);
			}
		}
	}
	else
	{
		const float3 currentHitPosition = ResolveHitVertexPosition(hit, false);
		const float currentViewZ = dot(currentHitPosition - gTraceConstants.CameraPos, gTraceConstants.CameraForward);
		const float2 currentJitter = GetCurrentTemporalJitter();
		const float2 previousJitter = GetPreviousTemporalJitter();
		const float3 previousHitPosition = ResolveHitVertexPosition(hit, true);
		const float2 basisCurrentUvRaw = ProjectWorldToUvRaw(currentHitPosition, gTraceConstants.CameraPos, gTraceConstants.CameraForward, gTraceConstants.CameraRight, gTraceConstants.CameraUp, gTraceConstants.TanHalfFovX, gTraceConstants.TanHalfFovY);
		const float2 basisPrevUvRaw = ProjectWorldToUvRaw(previousHitPosition, gTraceConstants.PrevCameraPos, gTraceConstants.PrevCameraForward, gTraceConstants.PrevCameraRight, gTraceConstants.PrevCameraUp, gTraceConstants.PrevTanHalfFovX, gTraceConstants.PrevTanHalfFovY);
		float2 currentUvRaw = 0.0;
		float2 prevUvRaw = 0.0;
		const bool basisCurrentUvValid = all(basisCurrentUvRaw >= 0.0) && all(basisCurrentUvRaw <= 1.0);
		const bool basisPrevUvValid = all(basisPrevUvRaw >= 0.0) && all(basisPrevUvRaw <= 1.0);
		const bool currentUvValid = ProjectWorldToUvMatrixRaw(currentHitPosition, false, currentUvRaw);
		const bool prevUvValid = ProjectWorldToUvMatrixRaw(previousHitPosition, true, prevUvRaw);
		const float2 basisCurrentUv = ApplyTemporalJitterToUv(basisCurrentUvRaw, currentJitter);
		const float2 basisPrevUv = ApplyTemporalJitterToUv(basisPrevUvRaw, previousJitter);
		const float2 currentUv = ApplyTemporalJitterToUv(currentUvRaw, currentJitter);
		const float2 prevUv = ApplyTemporalJitterToUv(prevUvRaw, previousJitter);
		const float previousViewZ = dot(previousHitPosition - gTraceConstants.PrevCameraPos, gTraceConstants.PrevCameraForward);
		float3 motion = 0.0;
		if (currentUvValid && prevUvValid)
		{
			motion.xy = (prevUvRaw - currentUvRaw) * float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight) + (currentJitter - previousJitter);
			motion.z = previousViewZ - currentViewZ;
		}

		float4 albedo = 1.0;
		float3 diffuse = 0.0;
		float3 specular = 0.0;
		float3 directLighting = 0.0;
		float3 directEmission = 0.0;
		float3 analyticDirectLighting = 0.0;
		float3 emissiveDirectLighting = 0.0;
		float2 emissiveSampleUv = 0.0;
		float3 emissiveSampleRadiance = 0.0;
		uint emissiveSamplePrimitiveIndex = 0xffffffffu;
		uint emissiveSampleDataSource = 0u;
		float emissiveSampleVisibleFraction = 0.0;
		float emissiveSampleOccludedFraction = 0.0;
		float3 sectorSourceLighting = 0.0;
		float3 sectorAmbientLighting = 0.0;
		float diffuseHitDistance = 0.0;
		float specularHitDistance = 0.0;
		float shadowVisibility = 1.0;
		float shadowPenumbra = 0.0;
		float roughness = 1.0;
		if (bootstrapFlat)
		{
			const float primitiveHash = (float)(hit.primitiveIndex % 31u) / 30.0;
			diffuse = float3(frac(primitiveHash * 1.7), frac(primitiveHash * 2.3), frac(primitiveHash * 3.1));
		}
		else
		{
			albedo = SampleMaterialBaseColor(hit.materialIndex, hit.dataSource, hit.uv);
			const MaterialData material = GetMaterialData(hit.materialIndex, hit.dataSource);
			const bool fullbright = (material.flags & MATERIAL_FLAG_FULLBRIGHT) != 0;
			const bool emissiveMaterial = IsMaterialEmissive(material);
			roughness = GetSurfaceRoughness(material);
			const float metalness = GetSurfaceMetalness(material);
			const float materialID = GetSurfaceMaterialID(material);
			if (bootstrapBaseColor)
			{
				diffuse = albedo.rgb;
				directEmission = albedo.rgb;
			}
			else if (emissiveMaterial || fullbright)
			{
				diffuse = 0.0;
				specular = 0.0;
				directEmission = EvaluateMaterialEmission(hit.materialIndex, hit.dataSource, material, hit.uv);
				if (!emissiveMaterial)
				{
					directEmission = albedo.rgb;
				}
			}
			else
			{
				const bool useDirectionalLight = UseDirectionalPlaceholderLight();
				const float3 lightDir = directSceneTrace ? normalize(gTraceConstants.LightDirection) : SampleSunDirection(normalize(gTraceConstants.LightDirection), pixelPos, gTraceConstants.FrameIndex);
				float shadowHitDistance = 0.0;
				const float shadow = useDirectionalLight ? (directSceneTrace ? 1.0 : ComputeSunShadow(hit.position, hit.normal, lightDir, shadowHitDistance)) : 0.0;
				shadowVisibility = shadow;
				if (useDirectionalLight && !directSceneTrace)
				{
					shadowPenumbra = SIGMA_FrontEnd_PackPenumbra(shadowHitDistance, kTanSunAngularRadius);
				}
				const float3 viewDir = normalize(-visibleRayDirection);
				sectorSourceLighting = EvaluateSectorLightingSource(material, hit.normal);
				sectorAmbientLighting = EvaluateSectorLighting(material, hit.normal, albedo.rgb);
				directLighting += EvaluateAmbientDiffuse(albedo.rgb) + sectorAmbientLighting;
				const float3 directSunDiffuse = useDirectionalLight ? EvaluateDirectSunDiffuse(albedo.rgb, hit.normal, lightDir) : 0.0;
				const float3 directSunSpecular = useDirectionalLight ? EvaluateSunSpecular(albedo.rgb, metalness, hit.normal, viewDir, lightDir, 1.0) : 0.0;
				diffuse += directSunDiffuse * shadow;
				specular += directSunSpecular * shadow;

				const RuntimeLightTileHeaderData runtimeLightTile = GetRuntimeLightTileHeader(pixelPos);
				[loop]
				for (uint runtimeLightCandidate = 0u; runtimeLightCandidate < runtimeLightTile.indexCount; ++runtimeLightCandidate)
				{
					const uint runtimeLightIndex = gRuntimeLightTileIndices[runtimeLightTile.indexOffset + runtimeLightCandidate];
					if (runtimeLightIndex >= gTraceConstants.RuntimeLightCount)
					{
						continue;
					}

					const RuntimePointLightData runtimeLight = gRuntimePointLights[runtimeLightIndex];
					const float3 toLight = runtimeLight.position - hit.position;
					const float lightDistanceSq = dot(toLight, toLight);
					if (lightDistanceSq <= 0.0001)
					{
						continue;
					}

					const float lightDistance = sqrt(lightDistanceSq);
					if (lightDistance >= runtimeLight.radius)
					{
						continue;
					}

					const float3 runtimeLightDir = toLight / lightDistance;
					const float lambert = max(dot(hit.normal, runtimeLightDir), 0.0);
					if (lambert <= 0.0)
					{
						continue;
					}

					const float runtimeShadow = directSceneTrace ? 1.0 : ComputePointLightShadow(hit.position, hit.normal, runtimeLightDir, lightDistance);
					if (runtimeShadow <= 0.0)
					{
						continue;
					}

					const float attenuation = EvaluatePointLightAttenuation(lightDistance, runtimeLight.radius, runtimeLight.intensity);
					if (attenuation <= 0.0)
					{
						continue;
					}

					const float3 lightColor = runtimeLight.color * attenuation;
					const float3 analyticDiffuse = albedo.rgb * (lambert * 0.80) * lightColor * runtimeShadow;
					const float3 analyticSpecular = EvaluateSunSpecular(albedo.rgb, metalness, hit.normal, viewDir, runtimeLightDir, 1.0) * lightColor * runtimeShadow;
					analyticDirectLighting += analyticDiffuse + analyticSpecular;
					directLighting += analyticDiffuse + analyticSpecular;
				}

				float3 emissiveSampleDiffuse = 0.0;
				float3 emissiveSampleSpecular = 0.0;
				uint emissiveSampleRng = pixelPos.x ^ (pixelPos.y << 16u) ^ (gTraceConstants.FrameIndex + 1u) * 0x9e3779b9u;
				const uint emissiveSampleCount = directSceneTrace ? 1u : GetEmissiveDirectSampleCount();
				[loop]
				for (uint emissiveSampleIndex = 0u; emissiveSampleIndex < emissiveSampleCount; ++emissiveSampleIndex)
				{
					float3 sampleDiffuse = 0.0;
					float3 sampleSpecular = 0.0;
					uint samplePrimitiveIndex = 0xffffffffu;
					uint sampleDataSource = 0u;
					bool sampleOccluded = false;
					float2 sampleUv = 0.0;
					float3 sampleRadiance = 0.0;
					EvaluateSampledEmissiveLighting(
						hit.position,
						hit.normal,
						viewDir,
						albedo.rgb,
						metalness,
						emissiveSampleRng,
						!directSceneTrace,
						sampleDiffuse,
						sampleSpecular,
						samplePrimitiveIndex,
						sampleDataSource,
						sampleOccluded,
						sampleUv,
						sampleRadiance);

					if (emissiveSamplePrimitiveIndex == 0xffffffffu && samplePrimitiveIndex != 0xffffffffu)
					{
						emissiveSamplePrimitiveIndex = samplePrimitiveIndex;
						emissiveSampleDataSource = sampleDataSource;
					}

					if (all(emissiveSampleRadiance <= 0.0) && any(sampleRadiance > 0.0))
					{
						emissiveSampleUv = sampleUv;
						emissiveSampleRadiance = sampleRadiance;
					}

					const bool sampleContributed = any((sampleDiffuse + sampleSpecular) > 0.0);
					if (sampleOccluded)
					{
						emissiveSampleOccludedFraction += 1.0;
					}
					else if (sampleContributed)
					{
						emissiveSampleVisibleFraction += 1.0;
					}

					emissiveSampleDiffuse += sampleDiffuse;
					emissiveSampleSpecular += sampleSpecular;
				}

				const float invEmissiveSampleCount = rcp((float)emissiveSampleCount);
				emissiveSampleVisibleFraction *= invEmissiveSampleCount;
				emissiveSampleOccludedFraction *= invEmissiveSampleCount;
				emissiveSampleDiffuse *= invEmissiveSampleCount;
				emissiveSampleSpecular *= invEmissiveSampleCount;
				emissiveDirectLighting += emissiveSampleDiffuse + emissiveSampleSpecular;
				directLighting += emissiveSampleDiffuse + emissiveSampleSpecular;

				const uint lightBounceCount = GetLightBounceCount();
				if (!directSceneTrace && lightBounceCount > 0u)
				{
					diffuse += TraceIndirectDiffuse(hit, albedo.rgb, pixelPos, gTraceConstants.FrameIndex, lightBounceCount, diffuseHitDistance);
					specular += TraceIndirectSpecular(hit, albedo, viewDir, pixelPos, gTraceConstants.FrameIndex, roughness, lightBounceCount, specularHitDistance);
				}
			}

			gNormalRoughnessOutput[pixelPos] = NRD_FrontEnd_PackNormalAndRoughness(hit.normal, roughness, materialID);
			gBaseColorOutput[pixelPos] = float4(bootstrapFlat ? diffuse : albedo.rgb, metalness);
		}
		gMotionOutput[pixelPos] = float4(motion, currentViewZ);
		gViewZOutput[pixelPos] = float4(currentViewZ, 0.0, 0.0, 1.0);
		const float4 packedDiffuse = PackDiffuseRadiance(diffuse, diffuseHitDistance, currentViewZ);
		const float4 packedSpecular = PackSpecularRadiance(specular, specularHitDistance, currentViewZ, roughness);
		if (bootstrapFlat)
		{
			gNormalRoughnessOutput[pixelPos] = NRD_FrontEnd_PackNormalAndRoughness(hit.normal, 1.0, 0.0);
			gBaseColorOutput[pixelPos] = float4(diffuse, 0.0);
		}
		gGuideDiffuseOutput[pixelPos] = packedDiffuse;
		gGuideSpecularOutput[pixelPos] = packedSpecular;
		gGuideSpecHitOutput[pixelPos] = float4(specular, packedSpecular.w);
		gShadowPenumbraOutput[pixelPos] = float4(shadowPenumbra, shadowVisibility, 0.0, 1.0);
		gDirectLightingOutput[pixelPos] = float4(directLighting, 1.0);
		gDirectEmissionOutput[pixelPos] = float4(directEmission, 1.0);
		if (gTraceConstants.DebugMode == 5)
		{
			gDirectLightingOutput[pixelPos] = float4(
				currentUvValid ? 1.0 : 0.0,
				prevUvValid ? 1.0 : 0.0,
				basisCurrentUvValid ? 1.0 : 0.0,
				basisPrevUvValid ? 1.0 : 0.0);
			gDirectEmissionOutput[pixelPos] = float4(currentUv, prevUv);
		}
		else if (gTraceConstants.DebugMode == 42)
		{
			const float2 currentUvNonJittered = ApplyTemporalJitterToUv(currentUvRaw, -currentJitter);
			const float2 prevUvNonJittered = ApplyTemporalJitterToUv(prevUvRaw, -previousJitter);
			gDirectLightingOutput[pixelPos] = float4(prevUvNonJittered, currentUvNonJittered);
			gDirectEmissionOutput[pixelPos] = float4(currentUvValid ? 1.0 : 0.0, prevUvValid ? 1.0 : 0.0, 0.0, 0.0);
		}
		else if (gTraceConstants.DebugMode == 43)
		{
			const float2 currentBasisUvNonJittered = ApplyTemporalJitterToUv(basisCurrentUvRaw, -currentJitter);
			const float2 prevBasisUvNonJittered = ApplyTemporalJitterToUv(basisPrevUvRaw, -previousJitter);
			gDirectLightingOutput[pixelPos] = float4(prevBasisUvNonJittered, currentBasisUvNonJittered);
			gDirectEmissionOutput[pixelPos] = float4(basisCurrentUvValid ? 1.0 : 0.0, basisPrevUvValid ? 1.0 : 0.0, 0.0, 0.0);
		}
		else if (gTraceConstants.DebugMode == 34)
		{
			const float2 actualPixelUv = ((float2)pixelPos + 0.5) / float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
			const float2 reprojectionErrorPixels = (currentUv - actualPixelUv) * float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
			const float reprojectionErrorMagnitude = length(reprojectionErrorPixels);
			gDirectLightingOutput[pixelPos] = float4(reprojectionErrorPixels, reprojectionErrorMagnitude, currentUvValid ? 1.0 : 0.0);
		}
		else if (gTraceConstants.DebugMode == 35)
		{
			const float3 hitPositionDelta = hit.position - currentHitPosition;
			gDirectLightingOutput[pixelPos] = float4(hitPositionDelta, 1.0);
		}

		if (gTraceConstants.DebugMode == 1)
		{
			color = float4(hit.normal * 0.5 + 0.5, 1.0);
		}
		else if (gTraceConstants.DebugMode == 2)
		{
			color = float4(frac(hit.uv), 0.0, 1.0);
		}
		else if (gTraceConstants.DebugMode == 3)
		{
			float id = (float)(hit.materialIndex % 19u) / 18.0;
			color = float4(id, frac(id * 1.7), frac(id * 2.3), 1.0);
		}
		else if (gTraceConstants.DebugMode == 4)
		{
			float id = (float)(hit.primitiveIndex % 29u) / 28.0;
			color = float4(frac(id * 1.1), frac(id * 1.9), frac(id * 2.7), 1.0);
		}
		else if (gTraceConstants.DebugMode == 26)
		{
			color = float4(analyticDirectLighting, 1.0);
		}
		else if (gTraceConstants.DebugMode == 27)
		{
			color = float4(directEmission, 1.0);
		}
		else if (gTraceConstants.DebugMode == 28)
		{
			color = float4(emissiveDirectLighting, 1.0);
		}
		else if (gTraceConstants.DebugMode == 29)
		{
			color = float4(sectorSourceLighting, 1.0);
		}
		else if (gTraceConstants.DebugMode == 30)
		{
			color = float4(frac(emissiveSampleUv), 0.0, 1.0);
		}
		else if (gTraceConstants.DebugMode == 31)
		{
			color = float4(emissiveSampleRadiance, 1.0);
		}
		else if (gTraceConstants.DebugMode == 32)
		{
			color = float4(GetEmissivePrimitiveDebugColor(emissiveSamplePrimitiveIndex, emissiveSampleDataSource), 1.0);
		}
		else if (gTraceConstants.DebugMode == 33)
		{
			const float rejectedFraction = saturate(1.0 - emissiveSampleVisibleFraction - emissiveSampleOccludedFraction);
			color = float4(emissiveSampleOccludedFraction, emissiveSampleVisibleFraction, rejectedFraction, 1.0);
		}
		else if (gTraceConstants.DebugMode == 34)
		{
			if (!currentUvValid)
			{
				color = float4(1.0, 0.0, 1.0, 1.0);
			}
			else
			{
				const float2 actualPixelUv = ((float2)pixelPos + 0.5) / float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
				const float2 errorPixels = (currentUv - actualPixelUv) * float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
				const float2 signedMagnitude = sign(errorPixels) * sqrt(saturate(abs(errorPixels) / 8.0));
				const float magnitude = saturate(log2(1.0 + length(errorPixels)) / 3.0);
				color = float4(signedMagnitude * 0.5 + 0.5, magnitude, 1.0);
			}
		}
		else if (gTraceConstants.DebugMode == 35)
		{
			const float magnitude = length(hit.position - currentHitPosition);
			const float mapped = saturate(log2(1.0 + magnitude * 256.0) / 8.0);
			float3 heat = lerp(float3(0.02, 0.02, 0.08), float3(0.10, 0.75, 0.25), saturate(mapped * 2.0));
			heat = lerp(heat, float3(0.95, 0.85, 0.20), saturate((mapped - 0.45) * 2.5));
			heat = lerp(heat, float3(1.0, 0.28, 0.10), saturate((mapped - 0.8) * 5.0));
			color = float4(heat, 1.0);
		}
		else if (gTraceConstants.DebugMode == 36)
		{
			if (!basisCurrentUvValid)
			{
				color = float4(1.0, 0.0, 1.0, 1.0);
			}
			else
			{
				const float2 actualPixelUv = ((float2)pixelPos + 0.5) / float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
				const float2 errorPixels = (basisCurrentUv - actualPixelUv) * float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
				const float2 signedMagnitude = sign(errorPixels) * sqrt(saturate(abs(errorPixels) / 8.0));
				const float magnitude = saturate(log2(1.0 + length(errorPixels)) / 3.0);
				color = float4(signedMagnitude * 0.5 + 0.5, magnitude, 1.0);
			}
		}
		else if (gTraceConstants.DebugMode == 37)
		{
			const uint split0 = gTraceConstants.RenderWidth / 3u;
			const uint split1 = (gTraceConstants.RenderWidth * 2u) / 3u;
			const bool onDivider = abs((int)pixelPos.x - (int)split0) <= 1 || abs((int)pixelPos.x - (int)split1) <= 1;
			if (onDivider)
			{
				color = float4(1.0, 1.0, 1.0, 1.0);
			}
			else if (!basisCurrentUvValid)
			{
				color = float4(1.0, 0.0, 1.0, 1.0);
			}
			else
			{
				const float2 resolution = float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
				const float2 jitterUv = currentJitter / resolution;
				const float2 actualPixelUv = ((float2)pixelPos + 0.5) / resolution;
				float2 candidateUv = basisCurrentUv;
				float3 tint = float3(0.92, 0.92, 0.92);

				if (pixelPos.x >= split0 && pixelPos.x < split1)
				{
					// Middle third: no jitter compensation.
					candidateUv = basisCurrentUv + jitterUv;
					tint = float3(0.85, 1.00, 0.85);
				}
				else if (pixelPos.x >= split1)
				{
					// Right third: opposite-sign overshoot to make the choice visible even under a large base bias.
					candidateUv = basisCurrentUv + jitterUv * 4.0;
					tint = float3(1.00, 0.85, 0.85);
				}
				else
				{
					// Left third: current subtract-once behavior.
					tint = float3(0.85, 0.90, 1.00);
				}

				const float2 errorPixels = (candidateUv - actualPixelUv) * resolution;
				const float2 signedMagnitude = sign(errorPixels) * sqrt(saturate(abs(errorPixels) / 8.0));
				const float magnitude = saturate(log2(1.0 + length(errorPixels)) / 3.0);
				const float2 mappedSigned = signedMagnitude * 0.5 + 0.5;
				const float3 baseColor = float3(mappedSigned.x, mappedSigned.y, magnitude);
				color = float4(baseColor * tint, 1.0);
			}
		}
		else if (gTraceConstants.DebugMode == 38)
		{
			const uint split0 = gTraceConstants.RenderWidth / 3u;
			const uint split1 = (gTraceConstants.RenderWidth * 2u) / 3u;
			const bool onDivider = abs((int)pixelPos.x - (int)split0) <= 1 || abs((int)pixelPos.x - (int)split1) <= 1;
			if (onDivider)
			{
				color = float4(1.0, 1.0, 1.0, 1.0);
			}
			else if (!basisCurrentUvValid)
			{
				color = float4(1.0, 0.0, 1.0, 1.0);
			}
			else
			{
				const float2 resolution = float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
				const float2 jitterUv = currentJitter / resolution;
				const float2 actualNonJitteredUv = ((float2)pixelPos + 0.5) / resolution;
				const float2 actualJitteredUv = ((float2)pixelPos + 0.5 + currentJitter) / resolution;
				float2 candidateUv = basisCurrentUv + jitterUv;
				float2 referenceUv = actualNonJitteredUv;
				float3 tint = float3(0.85, 0.90, 1.00);

				if (pixelPos.x >= split0 && pixelPos.x < split1)
				{
					// Middle third: raw projected UV against jittered traced-sample UV.
					candidateUv = basisCurrentUv + jitterUv;
					referenceUv = actualJitteredUv;
					tint = float3(0.85, 1.00, 0.85);
				}
				else if (pixelPos.x >= split1)
				{
					// Right third: current subtract-once projector against non-jittered UV.
					candidateUv = basisCurrentUv;
					referenceUv = actualNonJitteredUv;
					tint = float3(1.00, 0.85, 0.85);
				}

				const float2 errorPixels = (candidateUv - referenceUv) * resolution;
				const float2 signedMagnitude = sign(errorPixels) * sqrt(saturate(abs(errorPixels) / 8.0));
				const float magnitude = saturate(log2(1.0 + length(errorPixels)) / 3.0);
				const float2 mappedSigned = signedMagnitude * 0.5 + 0.5;
				const float3 baseColor = float3(mappedSigned.x, mappedSigned.y, magnitude);
				color = float4(baseColor * tint, 1.0);
			}
		}
		else if (gTraceConstants.DebugMode == 39)
		{
			const float magnitude = length(previousHitPosition - currentHitPosition);
			const float mapped = saturate(log2(1.0 + magnitude * 256.0) / 8.0);
			float3 heat = lerp(float3(0.01, 0.01, 0.05), float3(0.08, 0.40, 0.90), saturate(mapped * 2.0));
			heat = lerp(heat, float3(0.10, 0.80, 0.30), saturate((mapped - 0.30) * 2.5));
			heat = lerp(heat, float3(0.95, 0.85, 0.20), saturate((mapped - 0.60) * 3.0));
			heat = lerp(heat, float3(1.0, 0.25, 0.10), saturate((mapped - 0.85) * 6.0));
			color = float4(heat, 1.0);
		}
		else if (gTraceConstants.DebugMode == 40)
		{
			const float3 toHit = currentHitPosition - gTraceConstants.CameraPos;
			const float alongRay = max(dot(toHit, GeneratePrimaryRay(pixelPos)), 0.0);
			const float3 closestPointOnRay = gTraceConstants.CameraPos + GeneratePrimaryRay(pixelPos) * alongRay;
			const float closureError = length(currentHitPosition - closestPointOnRay);
			const float mapped = saturate(log2(1.0 + closureError * 256.0) / 8.0);
			float3 heat = lerp(float3(0.01, 0.01, 0.05), float3(0.08, 0.40, 0.90), saturate(mapped * 2.0));
			heat = lerp(heat, float3(0.10, 0.80, 0.30), saturate((mapped - 0.30) * 2.5));
			heat = lerp(heat, float3(0.95, 0.85, 0.20), saturate((mapped - 0.60) * 3.0));
			heat = lerp(heat, float3(1.0, 0.25, 0.10), saturate((mapped - 0.85) * 6.0));
			color = float4(heat, 1.0);
		}
		else if (gTraceConstants.DebugMode == 44)
		{
			const float2 motionPixels = motion.xy;
			const float2 signedMagnitude = sign(motionPixels) * sqrt(saturate(abs(motionPixels) / 8.0));
			const float magnitude = saturate(log2(1.0 + length(motionPixels)) / 3.0);
			color = float4(signedMagnitude * 0.5 + 0.5, magnitude, 1.0);
		}
		else if (gTraceConstants.DebugMode == 45)
		{
			if (!(currentUvValid && prevUvValid))
			{
				color = float4(1.0, 0.0, 1.0, 1.0);
			}
			else
			{
				const float2 currentUvNonJittered = ApplyTemporalJitterToUv(currentUvRaw, -currentJitter);
				const float2 prevUvNonJittered = ApplyTemporalJitterToUv(prevUvRaw, -previousJitter);
				const float2 reconstructedPrevUv = currentUvNonJittered + motion.xy / float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
				const float2 errorPixels = (reconstructedPrevUv - prevUvNonJittered) * float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
				const float2 signedMagnitude = sign(errorPixels) * sqrt(saturate(abs(errorPixels) / 8.0));
				const float magnitude = saturate(log2(1.0 + length(errorPixels)) / 3.0);
				color = float4(signedMagnitude * 0.5 + 0.5, magnitude, 1.0);
			}
		}
		else
		{
			color = packedDiffuse;
		}
	}

	gTraceOutput[pixelPos] = color;
	if (directSceneTrace && !bootstrapFlat && !bootstrapBaseColor)
	{
		gFinalOutput[pixelPos] = saturate(color.rgb);
	}
}
