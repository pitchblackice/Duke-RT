#include "Include/Shared.hlsli"
#include "Include/RaytracingShared.hlsli"

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
		[loop]
		for (uint bounce = 0; bounce < 3; ++bounce)
		{
			hit = TracePrimary(rayOrigin, visibleRayDirection);
			if (!hit.hit || !IsMirrorMaterial(hit.materialIndex))
			{
				break;
			}

			rayOrigin = hit.position + hit.normal * 0.05;
			visibleRayDirection = reflect(visibleRayDirection, hit.normal);
		}
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
			color = float4(GetMissColor(visibleRayDirection), 1.0);
			gMotionOutput[pixelPos] = 0.0;
			gViewZOutput[pixelPos] = 0.0;
			gNormalRoughnessOutput[pixelPos] = 0.0;
			gBaseColorOutput[pixelPos] = 0.0;
			gGuideDiffuseOutput[pixelPos] = 0.0;
			gGuideSpecularOutput[pixelPos] = 0.0;
			gGuideSpecHitOutput[pixelPos] = 0.0;
		}
	}
	else
	{
		const float roughness = 0.22;
		const float materialID = 0.0;
		const float currentViewZ = dot(hit.position - gTraceConstants.CameraPos, gTraceConstants.CameraForward);
		const float2 prevUv = ProjectWorldToUv(hit.position, gTraceConstants.PrevCameraPos, gTraceConstants.PrevCameraForward, gTraceConstants.PrevCameraRight, gTraceConstants.PrevCameraUp, gTraceConstants.PrevTanHalfFovX, gTraceConstants.PrevTanHalfFovY);
		const float2 currentUv = ((float2)pixelPos + 0.5) / float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
		float2 motion = 0.0;
		if (all(prevUv >= 0.0) && all(prevUv <= 1.0))
		{
			motion = (prevUv - currentUv) * float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
		}

		const float hitDistance = saturate(hit.distance / 4096.0);
		float4 albedo = 1.0;
		float3 diffuse = 0.0;
		float3 specular = 0.0;
		if (bootstrapFlat)
		{
			const float primitiveHash = (float)(hit.primitiveIndex % 31u) / 30.0;
			diffuse = float3(frac(primitiveHash * 1.7), frac(primitiveHash * 2.3), frac(primitiveHash * 3.1));
		}
		else
		{
			albedo = SampleSurfaceColor(hit.materialIndex, hit.uv);
			if (bootstrapBaseColor)
			{
				diffuse = albedo.rgb;
			}
			else
			{
				const float shadow = directSceneTrace ? 1.0 : ComputeSunShadow(hit.position, hit.normal);
				const float3 viewDir = normalize(-visibleRayDirection);
				const float3 lightDir = normalize(gTraceConstants.LightDirection);
				const float lambert = max(dot(hit.normal, lightDir), 0.0);
				const float lighting = 0.20 + shadow * lambert * 0.80;
				diffuse = albedo.rgb * lighting;
				const float3 halfVector = normalize(lightDir + viewDir);
				const float ndoth = max(dot(hit.normal, halfVector), 0.0);
				const float vdoth = max(dot(viewDir, halfVector), 0.0);
				const float fresnel = pow(1.0 - vdoth, 5.0);
				const float3 dielectricF0 = lerp(float3(0.04, 0.04, 0.04), albedo.rgb, 0.12);
				const float3 specularColor = lerp(dielectricF0, float3(1.0, 1.0, 1.0), fresnel);
				const float specularTerm = pow(ndoth, 12.0) * shadow * (0.5 + 0.5 * lambert);
				specular = specularColor * specularTerm * 0.85;
			}
		}
		gMotionOutput[pixelPos] = float4(motion, 0.0, 1.0);
		gViewZOutput[pixelPos] = float4(currentViewZ, 0.0, 0.0, 1.0);
		gNormalRoughnessOutput[pixelPos] = NRD_FrontEnd_PackNormalAndRoughness(hit.normal, roughness, materialID);
		gBaseColorOutput[pixelPos] = float4(bootstrapFlat ? diffuse : albedo.rgb, 1.0);
		gGuideDiffuseOutput[pixelPos] = float4(diffuse, 1.0);
		gGuideSpecularOutput[pixelPos] = float4(specular, hitDistance);
		gGuideSpecHitOutput[pixelPos] = float4(specular, hitDistance);

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
			color = float4(diffuse, hitDistance);
		}
	}

	gTraceOutput[pixelPos] = color;
	if (directSceneTrace && !bootstrapFlat && !bootstrapBaseColor)
	{
		gFinalOutput[pixelPos] = saturate(color.rgb);
	}
}
