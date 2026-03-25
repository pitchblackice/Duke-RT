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

float3 EvaluateSunDiffuseLighting(float3 normal, float3 lightDir, float shadow)
{
	const float lambert = max(dot(normal, lightDir), 0.0);
	const float lighting = 0.20 + shadow * lambert * 0.80;
	return lighting.xxx;
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

float GetSurfaceRoughness(MaterialData material)
{
	return clamp(material.roughnessHint, 0.02, 1.0);
}

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

float GetSurfaceMetalness(MaterialData material)
{
	return saturate(material.metalnessHint);
}

float GetSurfaceMaterialID(MaterialData material)
{
	return min((float)material.materialClass, 3.0);
}

float3 GetSurfaceSpecularColor(float3 albedo, float metalness)
{
	return lerp(float3(0.04, 0.04, 0.04), albedo, metalness);
}

float3 TraceIndirectDiffuse(HitData surfaceHit, uint2 pixelPos, uint frameIndex, uint bounceCount, out float outHitDistance)
{
	outHitDistance = 0.0;
	if (bounceCount == 0u)
	{
		return 0.0;
	}

	uint rngState = pixelPos.x * 73856093u ^ pixelPos.y * 19349663u ^ (frameIndex + 1u) * 83492791u ^ 0x9e3779b9u;
	float3 throughput = 1.0;
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

		const float4 bounceAlbedo = SampleSurfaceColor(bounceHit.materialIndex, bounceHit.dataSource, bounceHit.uv);
		if ((bounceMaterial.flags & MATERIAL_FLAG_FULLBRIGHT) != 0)
		{
			indirectRadiance += throughput * bounceAlbedo.rgb;
			break;
		}

		const float3 bounceLightDir = SampleSunDirection(normalize(gTraceConstants.LightDirection), pixelPos + uint2(bounce + 1u, bounce * 3u + 1u), frameIndex + bounce + 1u);
		const float bounceShadow = ComputeSunShadow(bounceHit.position, bounceHit.normal, bounceLightDir);
		indirectRadiance += throughput * bounceAlbedo.rgb * EvaluateSunDiffuseLighting(bounceHit.normal, bounceLightDir, bounceShadow);
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
		const float4 bounceAlbedo = SampleSurfaceColor(bounceHit.materialIndex, bounceHit.dataSource, bounceHit.uv);
		if ((bounceMaterial.flags & MATERIAL_FLAG_FULLBRIGHT) != 0)
		{
			indirectRadiance += throughput * bounceAlbedo.rgb;
			break;
		}

		const float3 bounceLightDir = SampleSunDirection(normalize(gTraceConstants.LightDirection), pixelPos + uint2(bounce * 5u + 1u, bounce * 7u + 3u), frameIndex + bounce + 1u);
		const float bounceShadow = ComputeSunShadow(bounceHit.position, bounceHit.normal, bounceLightDir);
		const float3 bounceViewDir = normalize(-tracedDirection);
		indirectRadiance += throughput * (
			bounceAlbedo.rgb * EvaluateSunDiffuseLighting(bounceHit.normal, bounceLightDir, bounceShadow) +
			EvaluateSunSpecular(bounceAlbedo.rgb, bounceMetalness, bounceHit.normal, bounceViewDir, bounceLightDir, bounceShadow));

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
			gMotionOutput[pixelPos] = 0.0;
			gViewZOutput[pixelPos] = float4(1.0, 0.0, 0.0, 1.0);
			gNormalRoughnessOutput[pixelPos] = 0.0;
			gBaseColorOutput[pixelPos] = float4(sentinel, 1.0);
			gGuideDiffuseOutput[pixelPos] = float4(sentinel, 1.0);
			gGuideSpecularOutput[pixelPos] = float4(0.0, 0.0, 0.0, 1.0);
			gGuideSpecHitOutput[pixelPos] = float4(0.0, 0.0, 0.0, 1.0);
		}
		else
		{
			const float3 missColor = GetMissColor(visibleRayDirection);
			const float4 packedDiffuse = PackReblurDiffuseRadiance(missColor, NRD_INF, NRD_INF);
			color = (gTraceConstants.DebugMode >= 1 && gTraceConstants.DebugMode <= 4) ? float4(missColor, 1.0) : packedDiffuse;
			gMotionOutput[pixelPos] = 0.0;
			gViewZOutput[pixelPos] = float4(NRD_INF, 0.0, 0.0, 1.0);
			gNormalRoughnessOutput[pixelPos] = 0.0;
			gBaseColorOutput[pixelPos] = float4(missColor, 0.0);
			gGuideDiffuseOutput[pixelPos] = packedDiffuse;
			gGuideSpecularOutput[pixelPos] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(0.0, 0.0, true);
			gGuideSpecHitOutput[pixelPos] = 0.0;
		}
	}
	else
	{
		const float currentViewZ = dot(hit.position - gTraceConstants.CameraPos, gTraceConstants.CameraForward);
		const float3 previousHitPosition = ResolveHitVertexPosition(hit, true);
		const float2 prevUv = ProjectWorldToUv(previousHitPosition, gTraceConstants.PrevCameraPos, gTraceConstants.PrevCameraForward, gTraceConstants.PrevCameraRight, gTraceConstants.PrevCameraUp, gTraceConstants.PrevTanHalfFovX, gTraceConstants.PrevTanHalfFovY);
		const float2 projectedCurrentUv = ProjectWorldToUv(hit.position, gTraceConstants.CameraPos, gTraceConstants.CameraForward, gTraceConstants.CameraRight, gTraceConstants.CameraUp, gTraceConstants.TanHalfFovX, gTraceConstants.TanHalfFovY);
		const float2 fallbackCurrentUv = ((float2)pixelPos + 0.5) / float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
		const bool currentUvValid = all(projectedCurrentUv >= 0.0) && all(projectedCurrentUv <= 1.0);
		const float2 currentUv = currentUvValid ? projectedCurrentUv : fallbackCurrentUv;
		const float previousViewZ = dot(previousHitPosition - gTraceConstants.PrevCameraPos, gTraceConstants.PrevCameraForward);
		float3 motion = 0.0;
		if (all(prevUv >= 0.0) && all(prevUv <= 1.0))
		{
			motion.xy = (prevUv - currentUv) * float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
			motion.z = previousViewZ - currentViewZ;
		}

		float4 albedo = 1.0;
		float3 diffuse = 0.0;
		float3 specular = 0.0;
		float diffuseHitDistance = 0.0;
		float specularHitDistance = 0.0;
		float roughness = 1.0;
		if (bootstrapFlat)
		{
			const float primitiveHash = (float)(hit.primitiveIndex % 31u) / 30.0;
			diffuse = float3(frac(primitiveHash * 1.7), frac(primitiveHash * 2.3), frac(primitiveHash * 3.1));
		}
		else
		{
			albedo = SampleSurfaceColor(hit.materialIndex, hit.dataSource, hit.uv);
			const MaterialData material = GetMaterialData(hit.materialIndex, hit.dataSource);
			const bool fullbright = (material.flags & MATERIAL_FLAG_FULLBRIGHT) != 0;
			roughness = GetSurfaceRoughness(material);
			const float metalness = GetSurfaceMetalness(material);
			const float materialID = GetSurfaceMaterialID(material);
			if (bootstrapBaseColor)
			{
				diffuse = albedo.rgb;
			}
			else if (fullbright)
			{
				diffuse = albedo.rgb;
				specular = 0.0;
			}
			else
			{
				const float3 lightDir = directSceneTrace ? normalize(gTraceConstants.LightDirection) : SampleSunDirection(normalize(gTraceConstants.LightDirection), pixelPos, gTraceConstants.FrameIndex);
				const float shadow = directSceneTrace ? 1.0 : ComputeSunShadow(hit.position, hit.normal, lightDir);
				const float3 viewDir = normalize(-visibleRayDirection);
				diffuse = EvaluateSunDiffuseLighting(hit.normal, lightDir, shadow);
				specular = EvaluateSunSpecular(albedo.rgb, metalness, hit.normal, viewDir, lightDir, shadow);
				const uint lightBounceCount = GetLightBounceCount();
				if (!directSceneTrace && lightBounceCount > 0u)
				{
					diffuse += TraceIndirectDiffuse(hit, pixelPos, gTraceConstants.FrameIndex, lightBounceCount, diffuseHitDistance);
					specular += TraceIndirectSpecular(hit, albedo, viewDir, pixelPos, gTraceConstants.FrameIndex, roughness, lightBounceCount, specularHitDistance);
				}
			}

			gNormalRoughnessOutput[pixelPos] = NRD_FrontEnd_PackNormalAndRoughness(hit.normal, roughness, materialID);
			gBaseColorOutput[pixelPos] = float4(bootstrapFlat ? diffuse : albedo.rgb, metalness);
		}
		gMotionOutput[pixelPos] = float4(motion, 1.0);
		gViewZOutput[pixelPos] = float4(currentViewZ, 0.0, 0.0, 1.0);
		const float4 packedDiffuse = PackReblurDiffuseRadiance(diffuse, diffuseHitDistance, currentViewZ);
		const float4 packedSpecular = PackReblurSpecularRadiance(specular, specularHitDistance, currentViewZ, roughness);
		if (bootstrapFlat)
		{
			gNormalRoughnessOutput[pixelPos] = NRD_FrontEnd_PackNormalAndRoughness(hit.normal, 1.0, 0.0);
			gBaseColorOutput[pixelPos] = float4(diffuse, 0.0);
		}
		gGuideDiffuseOutput[pixelPos] = packedDiffuse;
		gGuideSpecularOutput[pixelPos] = packedSpecular;
		gGuideSpecHitOutput[pixelPos] = float4(specular, packedSpecular.w);

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
