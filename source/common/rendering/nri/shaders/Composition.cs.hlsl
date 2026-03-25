#include "Include/Shared.hlsli"

float3 SanitizeColor(float3 value)
{
	if (any(isnan(value)) || any(isinf(value)))
	{
		return 0.0;
	}

	return value;
}

float3 ResolveDiffuseContribution(uint2 pixelPos, float3 diffuseSignal)
{
	const float viewZ = abs(gViewZInput.Load(int3(pixelPos, 0)).x);
	if (viewZ >= NRD_INF * 0.5)
	{
		return diffuseSignal;
	}

	float materialID = 0.0;
	NRD_FrontEnd_UnpackNormalAndRoughness(gNormalRoughnessInput[pixelPos], materialID);
	if (materialID >= 1.5 && materialID < 2.5)
	{
		return diffuseSignal;
	}

	return diffuseSignal * saturate(gBaseColorInput[pixelPos].rgb);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gTraceConstants.RenderWidth || dispatchThreadId.y >= gTraceConstants.RenderHeight)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	if (gTraceConstants.DebugMode == 15u)
	{
		const uint checker = ((pixelPos.x / 32u) ^ (pixelPos.y / 32u)) & 1u;
		gComposedOutput[pixelPos] = checker != 0u ? float4(0.95, 0.95, 0.95, 1.0) : float4(0.08, 0.08, 0.08, 1.0);
		return;
	}

	const float3 rawDiffuseSignal = SanitizeColor(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(gComposedInput.Load(int3(pixelPos, 0))).rgb);
	const float3 rawSpecular = SanitizeColor(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(gUpscaledInput.Load(int3(pixelPos, 0))).rgb);
	const float3 filteredDiffuseSignal = SanitizeColor(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(gGuideDiffuseInput.Load(int3(pixelPos, 0))).rgb);
	const float3 filteredSpecular = SanitizeColor(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(gGuideSpecularInput.Load(int3(pixelPos, 0))).rgb);
	const float3 rawDiffuse = ResolveDiffuseContribution(pixelPos, rawDiffuseSignal);
	const float3 filteredDiffuse = ResolveDiffuseContribution(pixelPos, filteredDiffuseSignal);

	float3 composed = filteredDiffuse + filteredSpecular;
	const uint splitMode = gTraceConstants.ReservedTrace0;
	if (splitMode != 0u)
	{
		const float3 rawComposed = rawDiffuse + rawSpecular;
		const bool leftSide = pixelPos.x * 2u < gTraceConstants.RenderWidth;
		const bool rawOnLeft = splitMode == 1u;
		composed = (leftSide == rawOnLeft) ? rawComposed : composed;

		const int dividerX = int(gTraceConstants.RenderWidth / 2u);
		if (abs((int)pixelPos.x - dividerX) <= 1)
		{
			composed = 1.0;
		}
	}

	gComposedOutput[pixelPos] = float4(saturate(composed), 1.0);
}
