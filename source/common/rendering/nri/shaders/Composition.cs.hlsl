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

float3 EvaluateSunDiffuseLighting(float3 normal, float3 lightDir, float shadow)
{
	const float lambert = max(dot(normal, lightDir), 0.0);
	const float lighting = 0.20 + shadow * lambert * 0.80;
	return lighting.xxx;
}

float3 EvaluateDirectSunDiffuse(float3 albedo, float3 normal, float3 lightDir)
{
	const float lambert = max(dot(normal, lightDir), 0.0);
	return albedo * (lambert * 0.80);
}

float3 EvaluateSunSpecular(float3 albedo, float metalness, float3 normal, float3 viewDir, float3 lightDir, float shadow)
{
	const float lambert = max(dot(normal, lightDir), 0.0);
	const float3 halfVector = normalize(lightDir + viewDir);
	const float ndoth = max(dot(normal, halfVector), 0.0);
	const float vdoth = max(dot(viewDir, halfVector), 0.0);
	const float fresnel = pow(1.0 - vdoth, 5.0);
	const float3 dielectricF0 = lerp(float3(0.04, 0.04, 0.04), albedo, metalness);
	const float3 specularColor = lerp(dielectricF0, float3(1.0, 1.0, 1.0), fresnel);
	const float specularTerm = pow(ndoth, 12.0) * shadow * (0.5 + 0.5 * lambert);
	return specularColor * specularTerm * 0.85;
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

float GetRawSunShadow(uint2 pixelPos)
{
	// Composition aliases t10 through gGuideSpecHitInput, but the descriptor set binds the raw SIGMA front-end output there.
	return saturate(gGuideSpecHitInput.Load(int3(pixelPos, 0)).y);
}

float GetFilteredSunShadow(uint2 pixelPos)
{
	return saturate(SIGMA_BackEnd_UnpackShadow(gShadowInput.Load(int3(pixelPos, 0))).x);
}

void ApplyShadowAwareSunCorrection(uint2 pixelPos, float materialID, float3 normal, float3 albedo, float metalness, float3 viewDir, bool useFilteredShadow, inout float3 diffuseSignal, inout float3 specularSignal)
{
	if (!UseSplitShadowDenoiser() || !UseDirectionalPlaceholderLight() || IsEmissiveMaterial(materialID) || IsSpecularSpecialMaterial(materialID))
	{
		return;
	}

	const float rawShadow = GetRawSunShadow(pixelPos);
	const float targetShadow = useFilteredShadow ? GetFilteredSunShadow(pixelPos) : rawShadow;
	const float shadowDelta = targetShadow - rawShadow;
	if (abs(shadowDelta) <= 1e-4)
	{
		return;
	}

	const float3 lightDir = normalize(gTraceConstants.LightDirection);
	diffuseSignal += EvaluateDirectSunDiffuse(albedo, normal, lightDir) * shadowDelta;
	specularSignal += EvaluateSunSpecular(albedo, metalness, normal, viewDir, lightDir, 1.0) * shadowDelta;
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
	const float3 normal = NRD_FrontEnd_UnpackNormalAndRoughness(gNormalRoughnessInput[pixelPos]).xyz;
	const float metalness = saturate(gBaseColorInput.Load(int3(pixelPos, 0)).a);
	const float3 albedo = SanitizeColor(gBaseColorInput.Load(int3(pixelPos, 0)).rgb);
	const float3 viewDir = normalize(-GeneratePrimaryRay(pixelPos));

	float3 adjustedRawDiffuseSignal = rawDiffuseSignal;
	float3 adjustedRawSpecular = rawSpecular;
	float3 adjustedFilteredDiffuseSignal = filteredDiffuseSignal;
	float3 adjustedFilteredSpecular = filteredSpecular;
	ApplyShadowAwareSunCorrection(pixelPos, materialID, normal, albedo, metalness, viewDir, false, adjustedRawDiffuseSignal, adjustedRawSpecular);
	ApplyShadowAwareSunCorrection(pixelPos, materialID, normal, albedo, metalness, viewDir, true, adjustedFilteredDiffuseSignal, adjustedFilteredSpecular);

	const float3 filteredComposed = ComposeLighting(pixelPos, adjustedFilteredDiffuseSignal, adjustedFilteredSpecular, composedDirectLighting, surfaceDirectEmission);
	const float3 rawComposed = ComposeLighting(pixelPos, adjustedRawDiffuseSignal, adjustedRawSpecular, composedDirectLighting, surfaceDirectEmission);
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
