#include "Include/Shared.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gTraceConstants.OutputWidth || dispatchThreadId.y >= gTraceConstants.OutputHeight)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	const float4 composed = gComposedOutput[pixelPos];
	gFinalOutput[pixelPos] = float4(saturate(composed.rgb), 1.0);
}
