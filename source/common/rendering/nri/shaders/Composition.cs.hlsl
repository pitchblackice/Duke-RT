#include "Include/Shared.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gTraceConstants.RenderWidth || dispatchThreadId.y >= gTraceConstants.RenderHeight)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	const float4 diffuse = gComposedInput[pixelPos];
	const float4 specular = gUpscaledInput[pixelPos];
	gComposedOutput[pixelPos] = float4(saturate(diffuse.rgb + specular.rgb), 1.0);
}
