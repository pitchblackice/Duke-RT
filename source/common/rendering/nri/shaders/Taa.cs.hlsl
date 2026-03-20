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

	const float4 currentColor = gComposedInput[pixelPos];
	gHistoryOutput[pixelPos] = currentColor;
}
