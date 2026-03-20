#include "Include/Shared.hlsli"

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
		const float hitMetric = saturate(gGuideSpecHitInput[pixelPos].x / 4096.0);
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
	else if ((gTraceConstants.Flags & 0x2u) != 0)
	{
		composed = gUpscaledInput.SampleLevel(gLinearClamp, uv, 0.0);
	}
	else
	{
		composed = gHistoryInput.SampleLevel(gLinearClamp, uv, 0.0);
	}

	gFinalOutput[pixelPos] = float4(saturate(composed.rgb), 1.0);
}
