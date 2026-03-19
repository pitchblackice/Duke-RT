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
	const float3 rayDirection = GeneratePrimaryRay(pixelPos);
	const HitData hit = TracePrimary(gTraceConstants.CameraPos, rayDirection);

	float4 color = 0.0;
	if (!hit.hit)
	{
		color = float4(GetMissColor(rayDirection), 1.0);
		gMotionOutput[pixelPos] = 0.0;
		gViewZOutput[pixelPos] = 0.0;
		gNormalRoughnessOutput[pixelPos] = 0.0;
		gBaseColorOutput[pixelPos] = 0.0;
		gGuideSpecularOutput[pixelPos] = 0.0;
	}
	else
	{
		const float4 albedo = SampleSurfaceColor(hit.materialIndex, hit.uv);
		const float4 surfaceNormal = float4(hit.normal, 0.08);
		const float currentViewZ = dot(hit.position - gTraceConstants.CameraPos, gTraceConstants.CameraForward);
		const float2 prevUv = ProjectWorldToUv(hit.position, gTraceConstants.PrevCameraPos, gTraceConstants.PrevCameraForward, gTraceConstants.PrevCameraRight, gTraceConstants.PrevCameraUp, gTraceConstants.PrevTanHalfFovX, gTraceConstants.PrevTanHalfFovY);
		const float2 currentUv = ((float2)pixelPos + 0.5) / float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
		float2 motion = 0.0;
		if (all(prevUv >= 0.0) && all(prevUv <= 1.0))
		{
			motion = (prevUv - currentUv) * float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
		}

		const float shadow = ComputeSunShadow(hit.position, hit.normal);
		const float lambert = max(dot(hit.normal, gTraceConstants.LightDirection), 0.0);
		const float lighting = 0.20 + shadow * lambert * 0.80;
		const float3 diffuse = albedo.rgb * lighting;
		const float3 halfVector = normalize(gTraceConstants.LightDirection - rayDirection);
		const float specularTerm = pow(max(dot(hit.normal, halfVector), 0.0), 32.0) * shadow;
		const float hitDistance = saturate(hit.distance / 4096.0);
		const float3 specular = albedo.rgb * specularTerm * 0.2;
		gMotionOutput[pixelPos] = float4(motion, 0.0, 1.0);
		gViewZOutput[pixelPos] = float4(currentViewZ, 0.0, 0.0, 1.0);
		gNormalRoughnessOutput[pixelPos] = surfaceNormal;
		gBaseColorOutput[pixelPos] = float4(albedo.rgb, 1.0);
		gGuideSpecularOutput[pixelPos] = float4(specular, hitDistance);

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
}
