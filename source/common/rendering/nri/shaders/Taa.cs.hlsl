#include "Include/Shared.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gTraceConstants.RenderWidth || dispatchThreadId.y >= gTraceConstants.RenderHeight)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	const float2 resolution = float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
	const float2 uv = ((float2)pixelPos + 0.5) / resolution;

	if (gTraceConstants.DebugMode == 15)
	{
		const float checker = fmod(floor(uv.x * 16.0) + floor(uv.y * 16.0), 2.0);
		const float3 color = lerp(float3(0.1, 0.8, 0.2), float3(0.95, 0.15, 0.75), checker);
		gHistoryOutput[pixelPos] = float4(color, 1.0);
		gComposedOutput[pixelPos] = float4(color, 1.0);
		return;
	}

	const float4 currentColor = gComposedInput[pixelPos];
	gHistoryOutput[pixelPos] = currentColor;
}
