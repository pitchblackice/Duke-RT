#include "Include/Shared.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gTraceConstants.RenderWidth || dispatchThreadId.y >= gTraceConstants.RenderHeight)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	const float4 baseColor = gBaseColorInput[pixelPos];
	const float viewZ = abs(gViewZInput[pixelPos].x);

	gGuideDiffuseOutput[pixelPos] = float4(baseColor.rgb, 1.0);
	gGuideSpecularOutput[pixelPos] = float4(baseColor.rgb, 1.0);
	gGuideSpecHitOutput[pixelPos] = float4(viewZ, 0.0, 0.0, 1.0);
}
