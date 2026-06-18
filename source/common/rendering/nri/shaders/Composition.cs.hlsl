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

bool UseDirectionalPlaceholderShadow()
{
	return (gTraceConstants.Flags & NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW) != 0;
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

float3 EvaluateDirectSunDiffuse(float3 albedo, float3 normal, float3 lightDir)
{
	const float lambert = max(dot(normal, lightDir), 0.0);
	return albedo * (lambert * 0.80);
}

float3 GetSurfaceDiffuseColor(float3 albedo, float metalness)
{
	return albedo * (1.0 - metalness);
}

float3 GetSurfaceSpecularColor(float3 albedo, float metalness)
{
	return lerp(float3(0.04, 0.04, 0.04), albedo, metalness);
}

void GetNrdPrimaryMaterialFactors(float3 normal, float3 viewDir, float3 baseColor, float metalness, float roughness, out float3 diffuseFactor, out float3 specularFactor)
{
	const float3 albedo = GetSurfaceDiffuseColor(baseColor, metalness);
	const float3 rf0 = GetSurfaceSpecularColor(baseColor, metalness);
	NRD_MaterialFactors(normalize(normal), normalize(viewDir), albedo, rf0, roughness, diffuseFactor, specularFactor);
}

float GetDirectLightingCompositionWeight(float metalness, float roughness)
{
	const float metallicSuppression = smoothstep(0.75, 1.0, metalness);
	const float glossySuppression = 1.0 - smoothstep(0.02, 0.50, roughness);
	return 1.0 - metallicSuppression * glossySuppression;
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

float GetRawSunShadow(uint2 pixelPos)
{
	// Composition aliases t10 through gGuideSpecHitInput, but the descriptor set binds the raw SIGMA front-end output there.
	return saturate(gGuideSpecHitInput.Load(int3(pixelPos, 0)).y);
}

float GetFilteredSunShadow(uint2 pixelPos)
{
	return saturate(SIGMA_BackEnd_UnpackShadow(gShadowInput.Load(int3(pixelPos, 0))).x);
}

void ApplyShadowAwareDirectLightingCorrection(uint2 pixelPos, float materialID, float3 normal, float3 albedo, float metalness, float3 viewDir, bool useFilteredShadow, inout float3 directLighting)
{
	if (!UseSplitShadowDenoiser() || !UseDirectionalPlaceholderLight() || !UseDirectionalPlaceholderShadow() || IsEmissiveMaterial(materialID) || IsSpecularSpecialMaterial(materialID))
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
	directLighting += EvaluateDirectSunDiffuse(GetSurfaceDiffuseColor(albedo, metalness), normal, lightDir) * GetDirectionalPlaceholderColor() * shadowDelta;
	directLighting += EvaluateSunSpecular(albedo, metalness, normal, viewDir, lightDir, 1.0) * GetDirectionalPlaceholderColor() * shadowDelta;
}

float3 ComposeLighting(uint2 pixelPos, float3 diffuseSignal, float3 specularSignal, float3 directLighting, float3 directEmission, float3 normal, float3 albedo, float metalness, float roughness, float3 viewDir)
{
	const float viewZ = abs(gViewZInput.Load(int3(pixelPos, 0)).x);
	if (viewZ >= NRD_INF * 0.5)
	{
		return directEmission;
	}

	// Phase-2 composition contract:
	// - diffuse/specular: primary-hit demodulated radiance from NRD
	// - directLighting: direct-composition bucket (ambient + placeholder sun + runtime point lights)
	//   with composition optionally correcting only the placeholder-sun shadow from raw to filtered SIGMA output
	// - directEmission: actual emissive-hit / fullbright surface output
	float3 diffuseFactor = 1.0;
	float3 specularFactor = 1.0;
	GetNrdPrimaryMaterialFactors(normal, viewDir, albedo, metalness, roughness, diffuseFactor, specularFactor);
	const float3 shadedDiffuse = diffuseSignal * diffuseFactor;
	const float3 shadedSpecular = specularSignal * specularFactor;
	const float3 composedDirectLighting = directLighting * GetDirectLightingCompositionWeight(metalness, roughness);

	// This remodulates the NRD-facing transport back into the beauty signal using the same
	// primary material factors that TraceOpaque used for de-modulation.
	return directEmission + composedDirectLighting + shadedDiffuse + shadedSpecular;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gTraceConstants.RenderWidth || dispatchThreadId.y >= gTraceConstants.RenderHeight)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	const float3 rawDiffuseSignal = SanitizeColor(UnpackDenoisedRadiance(gComposedInput.Load(int3(pixelPos, 0))));
	const float3 rawSpecular = SanitizeColor(UnpackDenoisedRadiance(gUpscaledInput.Load(int3(pixelPos, 0))));
	const float3 filteredDiffuseSignal = SanitizeColor(UnpackDenoisedRadiance(gGuideDiffuseInput.Load(int3(pixelPos, 0))));
	const float3 filteredSpecular = SanitizeColor(UnpackDenoisedRadiance(gGuideSpecularInput.Load(int3(pixelPos, 0))));
	const float3 composedDirectLighting = SanitizeColor(gDirectLightingInput.Load(int3(pixelPos, 0)).rgb);
	const float3 surfaceDirectEmission = SanitizeColor(gDirectEmissionInput.Load(int3(pixelPos, 0)).rgb);
	const float4 baseColorMetalness = gBaseColorInput.Load(int3(pixelPos, 0));
	float materialID = 0.0;
	const float4 unpackedNormalRoughness = NRD_FrontEnd_UnpackNormalAndRoughness(gNormalRoughnessInput[pixelPos], materialID);
	const float3 normal = unpackedNormalRoughness.xyz;
	const float roughness = saturate(unpackedNormalRoughness.w);
	const float metalness = saturate(baseColorMetalness.a);
	const float3 albedo = SanitizeColor(baseColorMetalness.rgb);
	const float3 viewDir = normalize(-GeneratePrimaryRay(pixelPos));

	float3 adjustedRawDirectLighting = composedDirectLighting;
	float3 adjustedFilteredDirectLighting = composedDirectLighting;
	ApplyShadowAwareDirectLightingCorrection(pixelPos, materialID, normal, albedo, metalness, viewDir, false, adjustedRawDirectLighting);
	ApplyShadowAwareDirectLightingCorrection(pixelPos, materialID, normal, albedo, metalness, viewDir, true, adjustedFilteredDirectLighting);

	const float3 filteredComposed = ComposeLighting(pixelPos, filteredDiffuseSignal, filteredSpecular, adjustedFilteredDirectLighting, surfaceDirectEmission, normal, albedo, metalness, roughness, viewDir);
	const float3 rawComposed = ComposeLighting(pixelPos, rawDiffuseSignal, rawSpecular, adjustedRawDirectLighting, surfaceDirectEmission, normal, albedo, metalness, roughness, viewDir);
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
