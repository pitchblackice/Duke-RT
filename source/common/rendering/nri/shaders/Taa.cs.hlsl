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
	const float2 motion = gMotionInput[pixelPos].xy;
	const float2 previousUv = uv + motion / resolution;

	float4 historyColor = currentColor;
	float historyWeight = 0.0;
	if ((gTraceConstants.Flags & 0x1u) == 0 && all(previousUv >= 0.0) && all(previousUv <= 1.0))
	{
		historyColor = gHistoryInput.SampleLevel(gLinearClamp, previousUv, 0.0);
		const float motionMagnitude = length(motion);
		historyWeight = saturate(0.92 - motionMagnitude * 0.04);
	}

	gHistoryOutput[pixelPos] = lerp(currentColor, historyColor, historyWeight);
}
