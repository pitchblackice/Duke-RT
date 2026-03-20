#include "Include/Shared.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gTraceConstants.RenderWidth || dispatchThreadId.y >= gTraceConstants.RenderHeight)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	if (gTraceConstants.DebugMode == 15)
	{
		const float2 uv = ((float2)pixelPos + 0.5) / float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
		const float checker = fmod(floor(uv.x * 16.0) + floor(uv.y * 16.0), 2.0);
		const float3 color = lerp(float3(0.15, 0.85, 0.25), float3(0.95, 0.2, 0.8), checker);
		gComposedOutput[pixelPos] = float4(color, 1.0);
		return;
	}

	const float4 diffuse = gComposedInput[pixelPos];
	const float4 specular = gUpscaledInput[pixelPos];
	gComposedOutput[pixelPos] = float4(saturate(diffuse.rgb + specular.rgb), 1.0);
}
