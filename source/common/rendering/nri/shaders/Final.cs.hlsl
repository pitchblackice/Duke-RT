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
	float3 color = BootstrapPattern(uv, gTraceConstants.CameraForward, gTraceConstants.SkyColor, gTraceConstants.GroundColor, gTraceConstants.FrameIndex);
	const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	const float3 planeNormal = normalize(gTraceConstants.CameraUp);
	const float3 planeRight = normalize(gTraceConstants.CameraRight);
	const float3 planeForward = normalize(cross(planeNormal, planeRight));
	const float3 planePoint = gTraceConstants.CameraPos - planeNormal * 96.0;
	float3 rayDir = normalize(
		gTraceConstants.CameraForward +
		ndc.x * gTraceConstants.TanHalfFovX * gTraceConstants.CameraRight +
		ndc.y * gTraceConstants.TanHalfFovY * gTraceConstants.CameraUp);
	const float denom = dot(rayDir, planeNormal);
	if (abs(denom) > 0.0001)
	{
		const float t = dot(planePoint - gTraceConstants.CameraPos, planeNormal) / denom;
		if (t > 0.0)
		{
			const float3 hitPos = gTraceConstants.CameraPos + rayDir * t;
			const float localX = dot(hitPos - planePoint, planeRight);
			const float localZ = dot(hitPos - planePoint, planeForward);
			const float checker = fmod(floor(localX * 0.125) + floor(localZ * 0.125), 2.0);
			const float gridX = step(frac(abs(localX) * 0.125), 0.035);
			const float gridZ = step(frac(abs(localZ) * 0.125), 0.035);
			const float gridMask = saturate(gridX + gridZ);
			const float3 warm = float3(0.62, 0.43, 0.24);
			const float3 cool = float3(0.22, 0.24, 0.28);
			float3 base = lerp(cool, warm, checker);
			const float distanceFade = saturate(1.0 / (1.0 + t * 0.03));
			const float stripe = 0.5 + 0.5 * sin(localX * 0.05 + gTraceConstants.FrameIndex * 0.02);
			base = lerp(base, base.bgr, stripe * 0.15);
			base = lerp(base, float3(0.98, 0.96, 0.9), gridMask * 0.85);
			const float sun = saturate(dot(planeNormal, normalize(gTraceConstants.LightDirection))) * 0.35 + 0.65;
			color = base * distanceFade * sun;
		}
	}

	return saturate(color);
}

bool IntersectTriangle(float3 rayOrigin, float3 rayDir, float3 v0, float3 v1, float3 v2, out float outT, out float3 outBarycentrics)
{
	outT = 0.0;
	outBarycentrics = 0.0;
	const float3 edge1 = v1 - v0;
	const float3 edge2 = v2 - v0;
	const float3 p = cross(rayDir, edge2);
	const float det = dot(edge1, p);
	if (abs(det) < 1e-5)
	{
		return false;
	}

	const float invDet = 1.0 / det;
	const float3 t = rayOrigin - v0;
	const float u = dot(t, p) * invDet;
	if (u < 0.0 || u > 1.0)
	{
		return false;
	}

	const float3 q = cross(t, edge1);
	const float v = dot(rayDir, q) * invDet;
	if (v < 0.0 || (u + v) > 1.0)
	{
		return false;
	}

	const float hitT = dot(edge2, q) * invDet;
	if (hitT <= 0.0)
	{
		return false;
	}

	outT = hitT;
	outBarycentrics = float3(1.0 - u - v, u, v);
	return true;
}

float3 BootstrapTriangle(float2 uv)
{
	float3 color = BootstrapPattern(uv, gTraceConstants.CameraForward, gTraceConstants.SkyColor, gTraceConstants.GroundColor, gTraceConstants.FrameIndex);
	const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	const float3 rayDir = normalize(
		gTraceConstants.CameraForward +
		ndc.x * gTraceConstants.TanHalfFovX * gTraceConstants.CameraRight +
		ndc.y * gTraceConstants.TanHalfFovY * gTraceConstants.CameraUp);

	const float3 center = gTraceConstants.CameraPos + gTraceConstants.CameraForward * 256.0;
	const float3 v0 = center + gTraceConstants.CameraUp * 72.0;
	const float3 v1 = center - gTraceConstants.CameraRight * 80.0 - gTraceConstants.CameraUp * 56.0;
	const float3 v2 = center + gTraceConstants.CameraRight * 80.0 - gTraceConstants.CameraUp * 56.0;
	float hitT = 0.0;
	float3 bary = 0.0;
	if (IntersectTriangle(gTraceConstants.CameraPos, rayDir, v0, v1, v2, hitT, bary))
	{
		color = bary;
		const float edge = min(bary.x, min(bary.y, bary.z));
		color = lerp(color, float3(1.0, 1.0, 1.0), step(edge, 0.04));
	}

	return saturate(color);
}

float3 BootstrapTexturedQuad(float2 uv)
{
	float3 color = BootstrapPattern(uv, gTraceConstants.CameraForward, gTraceConstants.SkyColor, gTraceConstants.GroundColor, gTraceConstants.FrameIndex);
	const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	const float3 rayDir = normalize(
		gTraceConstants.CameraForward +
		ndc.x * gTraceConstants.TanHalfFovX * gTraceConstants.CameraRight +
		ndc.y * gTraceConstants.TanHalfFovY * gTraceConstants.CameraUp);

	const float3 center = gTraceConstants.CameraPos + gTraceConstants.CameraForward * 320.0 + gTraceConstants.CameraUp * 12.0;
	const float3 quadRight = gTraceConstants.CameraRight * 128.0;
	const float3 quadUp = gTraceConstants.CameraUp * 96.0;
	const float3 v0 = center - quadRight + quadUp;
	const float3 v1 = center - quadRight - quadUp;
	const float3 v2 = center + quadRight - quadUp;
	const float3 v3 = center + quadRight + quadUp;

	float hitT = 0.0;
	float3 bary = 0.0;
	float2 surfaceUv = 0.0;
	if (IntersectTriangle(gTraceConstants.CameraPos, rayDir, v0, v1, v2, hitT, bary))
	{
		surfaceUv = float2(bary.z, bary.y + bary.z);
	}
	else if (IntersectTriangle(gTraceConstants.CameraPos, rayDir, v0, v2, v3, hitT, bary))
	{
		surfaceUv = float2(bary.y + bary.z, bary.z);
	}
	else
	{
		return saturate(color);
	}

	const float checker = fmod(floor(surfaceUv.x * 8.0) + floor(surfaceUv.y * 8.0), 2.0);
	float3 texel = lerp(float3(0.15, 0.18, 0.72), float3(0.95, 0.78, 0.18), checker);
	const float gridMask = step(frac(surfaceUv.x * 8.0), 0.04) + step(frac(surfaceUv.y * 8.0), 0.04);
	texel = lerp(texel, float3(1.0, 1.0, 1.0), saturate(gridMask));
	texel *= 0.8 + 0.2 * sin((surfaceUv.x + surfaceUv.y + gTraceConstants.FrameIndex * 0.01) * 12.0);
	return saturate(texel);
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
		if (gTraceConstants.BootstrapMode == 1)
		{
			color = BootstrapPlane(uv);
		}
		else if (gTraceConstants.BootstrapMode == 2)
		{
			color = BootstrapTriangle(uv);
		}
		else if (gTraceConstants.BootstrapMode == 3)
		{
			color = BootstrapTexturedQuad(uv);
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
