#include "Include/Shared.hlsli"
#include "Include/RaytracingShared.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gTraceConstants.OutputWidth || dispatchThreadId.y >= gTraceConstants.OutputHeight)
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
	}
	else
	{
		const float4 albedo = SampleSurfaceColor(hit.materialIndex, hit.uv);
		const float shadow = ComputeSunShadow(hit.position, hit.normal);
		const float lambert = max(dot(hit.normal, gTraceConstants.LightDirection), 0.0);
		const float lighting = 0.20 + shadow * lambert * 0.80;

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
			color = float4(albedo.rgb * lighting, 1.0);
		}
	}

	gTraceOutput[pixelPos] = color;
}
