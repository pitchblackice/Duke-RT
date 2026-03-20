#include "Include/Shared.hlsli"

float3 BootstrapPattern(float2 uv, float3 cameraForward, float3 skyColor, float3 groundColor, uint frameIndex)
{
	const float2 centered = uv * 2.0 - 1.0;
	const float aspect = (float)gTraceConstants.DisplayWidth / max((float)gTraceConstants.DisplayHeight, 1.0);
	float2 gridUv = float2(centered.x * aspect, centered.y);
	float3 color = lerp(groundColor, skyColor, saturate(uv.y));
	color = lerp(color, abs(normalize(cameraForward)) * 0.75 + 0.1, 0.35);

	const float borderMask = step(0.96, max(abs(centered.x), abs(centered.y)));
	const float crossMask = step(abs(gridUv.x), 0.01) + step(abs(gridUv.y), 0.01);
	const float gridMask = step(frac((gridUv.x + 8.0) * 8.0), 0.02) + step(frac((gridUv.y + 8.0) * 8.0), 0.02);
	const float framePulse = ((frameIndex & 31u) < 16u) ? 1.0 : 0.35;

	color = lerp(color, float3(0.02, 0.02, 0.02), saturate(gridMask * 0.35));
	color = lerp(color, float3(1.0, 1.0, 1.0), saturate(crossMask));
	color = lerp(color, float3(framePulse, 0.25, 1.0 - framePulse * 0.5), borderMask);
	return saturate(color);
}

float3 BootstrapPlane(float2 uv)
{
	const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	float3 rayDir = normalize(
		gTraceConstants.CameraForward +
		ndc.x * gTraceConstants.TanHalfFovX * gTraceConstants.CameraRight +
		ndc.y * gTraceConstants.TanHalfFovY * gTraceConstants.CameraUp);

	float3 color = lerp(gTraceConstants.GroundColor * 0.5, gTraceConstants.SkyColor, saturate(rayDir.y * 0.5 + 0.5));
	const float planeY = 0.0;
	if (rayDir.y < -0.0001)
	{
		const float t = (planeY - gTraceConstants.CameraPos.y) / rayDir.y;
		if (t > 0.0)
		{
			const float3 hitPos = gTraceConstants.CameraPos + rayDir * t;
			const float checker = fmod(floor(hitPos.x * 0.125) + floor(hitPos.z * 0.125), 2.0);
			const float gridX = step(frac(abs(hitPos.x) * 0.125), 0.035);
			const float gridZ = step(frac(abs(hitPos.z) * 0.125), 0.035);
			const float gridMask = saturate(gridX + gridZ);
			const float3 warm = float3(0.62, 0.43, 0.24);
			const float3 cool = float3(0.22, 0.24, 0.28);
			float3 base = lerp(cool, warm, checker);
			const float distanceFade = saturate(1.0 / (1.0 + t * 0.03));
			const float stripe = 0.5 + 0.5 * sin(hitPos.x * 0.05 + gTraceConstants.FrameIndex * 0.02);
			base = lerp(base, base.bgr, stripe * 0.15);
			base = lerp(base, float3(0.98, 0.96, 0.9), gridMask * 0.85);
			const float sun = saturate(dot(normalize(float3(0.0, 1.0, 0.0)), normalize(gTraceConstants.LightDirection))) * 0.35 + 0.65;
			color = base * distanceFade * sun;
		}
	}

	return saturate(color);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gTraceConstants.DisplayWidth || dispatchThreadId.y >= gTraceConstants.DisplayHeight)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	const float2 uv = ((float2)pixelPos + 0.5) / float2(gTraceConstants.DisplayWidth, gTraceConstants.DisplayHeight);
	float4 composed = 0.0;

	if ((gTraceConstants.Flags & 0x4u) != 0)
	{
		float3 color = BootstrapPattern(uv, gTraceConstants.CameraForward, gTraceConstants.SkyColor, gTraceConstants.GroundColor, gTraceConstants.FrameIndex);
		if (gTraceConstants.MaterialCount == 1)
		{
			color = BootstrapPlane(uv);
		}
		gFinalOutput[pixelPos] = float4(saturate(color), 1.0);
		return;
	}

	if (gTraceConstants.DebugMode == 5)
	{
		const float2 motion = gMotionInput[pixelPos].xy / max(float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight), 1.0);
		composed = float4(motion * 0.5 + 0.5, 0.5, 1.0);
	}
	else if (gTraceConstants.DebugMode == 6)
	{
		const float viewZ = abs(gViewZInput[pixelPos].x);
		const float normalized = saturate(viewZ / 4096.0);
		composed = float4(normalized.xxx, 1.0);
	}
	else if (gTraceConstants.DebugMode == 7)
	{
		composed = float4(saturate(gBaseColorInput[pixelPos].rgb), 1.0);
	}
	else if (gTraceConstants.DebugMode == 8)
	{
		composed = float4(NRD_FrontEnd_UnpackNormalAndRoughness(gNormalRoughnessInput[pixelPos]).xyz * 0.5 + 0.5, 1.0);
	}
	else if (gTraceConstants.DebugMode == 9)
	{
		composed = float4(saturate(gValidationInput[pixelPos].rgb), 1.0);
	}
	else if (gTraceConstants.DebugMode == 10)
	{
		composed = float4(saturate(gGuideDiffuseInput[pixelPos].rgb), 1.0);
	}
	else if (gTraceConstants.DebugMode == 11)
	{
		composed = float4(saturate(gGuideSpecularInput[pixelPos].rgb), 1.0);
	}
	else if (gTraceConstants.DebugMode == 12)
	{
		const float hitMetric = saturate(gGuideSpecHitInput[pixelPos].w);
		composed = float4(hitMetric.xxx, 1.0);
	}
	else if (gTraceConstants.DebugMode == 13)
	{
		composed = float4(saturate(gHistoryInput.SampleLevel(gLinearClamp, uv, 0.0).rgb), 1.0);
	}
	else if (gTraceConstants.DebugMode == 14)
	{
		composed = float4(saturate(gUpscaledInput.SampleLevel(gLinearClamp, uv, 0.0).rgb), 1.0);
	}
	else if (gTraceConstants.DebugMode == 15)
	{
		composed = float4(saturate(gComposedInput[pixelPos].rgb), 1.0);
	}
	else if ((gTraceConstants.Flags & 0x2u) != 0)
	{
		composed = gUpscaledInput.SampleLevel(gLinearClamp, uv, 0.0);
	}
	else
	{
		composed = gComposedInput[pixelPos];
	}

	gFinalOutput[pixelPos] = float4(saturate(composed.rgb), 1.0);
}
