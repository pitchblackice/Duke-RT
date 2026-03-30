#include "Include/Shared.hlsli"

float3 SanitizeColor(float3 value)
{
	if (any(isnan(value)) || any(isinf(value)))
	{
		return 0.0;
	}

	return value;
}

bool UseRelaxDenoiser()
{
	return (gTraceConstants.ReservedTrace1 & 0xffu) == 1u;
}

bool UseSplitShadowDenoiser()
{
	return (gTraceConstants.Flags & 0x20u) != 0;
}

bool UseDirectionalPlaceholderLight()
{
	return (gTraceConstants.Flags & 0x80u) != 0;
}

float3 UnpackDenoisedRadiance(float4 packed)
{
	return UseRelaxDenoiser() ? RELAX_BackEnd_UnpackRadiance(packed).rgb : REBLUR_BackEnd_UnpackRadianceAndNormHitDist(packed).rgb;
}

float3 GeneratePrimaryRay(uint2 pixelPos)
{
	float2 resolution = float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
	float2 uv = ((float2)pixelPos + 0.5) / resolution;
	float2 ndc = uv * 2.0 - 1.0;
	ndc.y = -ndc.y;

	float3 ray =
		gTraceConstants.CameraForward +
		ndc.x * gTraceConstants.TanHalfFovX * gTraceConstants.CameraRight +
		ndc.y * gTraceConstants.TanHalfFovY * gTraceConstants.CameraUp;

	return normalize(ray);
}

float GetMaterialID(uint2 pixelPos)
{
	float materialID = 0.0;
	NRD_FrontEnd_UnpackNormalAndRoughness(gNormalRoughnessInput[pixelPos], materialID);
	return materialID;
}

bool IsEmissiveMaterial(float materialID)
{
	return materialID >= 1.5 && materialID < 2.5;
}

bool IsSpecularSpecialMaterial(float materialID)
{
	return materialID >= 2.5;
}

float3 ComposeLighting(uint2 pixelPos, float3 diffuseSignal, float3 specularSignal, float3 directLighting, float3 directEmission)
{
	const float viewZ = abs(gViewZInput.Load(int3(pixelPos, 0)).x);
	if (viewZ >= NRD_INF * 0.5)
	{
		return directEmission;
	}

	// Current composition contract after slices 1-3:
	// - diffuse/specular: denoised transport (sun, sampled emissive, indirect)
	// - directLighting: stable raw direct-composition bucket (ambient + runtime point lights)
	// - directEmission: actual emissive-hit / fullbright surface output
	return directEmission + directLighting + diffuseSignal + specularSignal;
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

	const float3 rawDiffuseSignal = SanitizeColor(UnpackDenoisedRadiance(gComposedInput.Load(int3(pixelPos, 0))));
	const float3 rawSpecular = SanitizeColor(UnpackDenoisedRadiance(gUpscaledInput.Load(int3(pixelPos, 0))));
	const float3 filteredDiffuseSignal = SanitizeColor(UnpackDenoisedRadiance(gGuideDiffuseInput.Load(int3(pixelPos, 0))));
	const float3 filteredSpecular = SanitizeColor(UnpackDenoisedRadiance(gGuideSpecularInput.Load(int3(pixelPos, 0))));
	const float3 composedDirectLighting = SanitizeColor(gDirectLightingInput.Load(int3(pixelPos, 0)).rgb);
	const float3 surfaceDirectEmission = SanitizeColor(gDirectEmissionInput.Load(int3(pixelPos, 0)).rgb);
	const float materialID = GetMaterialID(pixelPos);

	const float3 filteredComposed = ComposeLighting(pixelPos, filteredDiffuseSignal, filteredSpecular, composedDirectLighting, surfaceDirectEmission);
	const float3 rawComposed = ComposeLighting(pixelPos, rawDiffuseSignal, rawSpecular, composedDirectLighting, surfaceDirectEmission);
	const bool specialMaterialRawFallback = UseSplitShadowDenoiser() && IsSpecularSpecialMaterial(materialID);
	float3 composed = specialMaterialRawFallback ? rawComposed : filteredComposed;
	const uint splitMode = gTraceConstants.ReservedTrace0;
	if (splitMode != 0u)
	{
		const bool leftSide = pixelPos.x * 2u < gTraceConstants.RenderWidth;
		const bool rawOnLeft = splitMode == 1u;
		composed = (leftSide == rawOnLeft) ? rawComposed : filteredComposed;
		if (specialMaterialRawFallback)
		{
			composed = rawComposed;
		}

		const int dividerX = int(gTraceConstants.RenderWidth / 2u);
		if (abs((int)pixelPos.x - dividerX) <= 1)
		{
			composed = 1.0;
		}
	}

	gComposedOutput[pixelPos] = float4(SanitizeColor(composed), 1.0);
}
