#ifndef RAZE_NRI_PT_DISPLAY_MAPPING_HLSLI
#define RAZE_NRI_PT_DISPLAY_MAPPING_HLSLI

#include "PresentConstants.hlsli"

float SanitizeFiniteChannel(float value)
{
	if (isnan(value) || isinf(value))
	{
		return 0.0;
	}

	return max(value, 0.0);
}

float3 SanitizeFiniteColor(float3 color)
{
	return float3(
		SanitizeFiniteChannel(color.x),
		SanitizeFiniteChannel(color.y),
		SanitizeFiniteChannel(color.z));
}

float3 ApplyManualExposure(float3 color, float exposure)
{
	return color * max(exposure, 0.0);
}

float3 ApplyPresentSceneContrast(float3 color, float contrast)
{
	const float safeContrast = max(contrast, 0.0);
	const float pivot = 0.18;
	return max((color - pivot.xxx) * safeContrast + pivot.xxx, 0.0);
}

float ApplyDisplayToe(float value, float toe)
{
	const float safeValue = saturate(value);
	const float safeToe = max(toe, 1e-3);
	const float curved = pow(safeValue, safeToe);
	const float weight = saturate(1.0 - safeValue * 2.0);
	return lerp(safeValue, curved, weight);
}

float ApplyDisplayShoulder(float value, float shoulder)
{
	const float safeValue = saturate(value);
	const float safeShoulder = max(shoulder, 1e-3);
	const float curved = 1.0 - pow(1.0 - safeValue, safeShoulder);
	const float weight = saturate(safeValue * 2.0 - 1.0);
	return lerp(safeValue, curved, weight);
}

float3 ApplyDisplayToeAndShoulder(float3 color, float toe, float shoulder)
{
	return float3(
		ApplyDisplayShoulder(ApplyDisplayToe(color.x, toe), shoulder),
		ApplyDisplayShoulder(ApplyDisplayToe(color.y, toe), shoulder),
		ApplyDisplayShoulder(ApplyDisplayToe(color.z, toe), shoulder));
}

float3 ApplyDisplaySaturation(float3 color, float saturation)
{
	const float safeSaturation = max(saturation, 0.0);
	const float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
	return max(lerp(luma.xxx, color, safeSaturation), 0.0);
}

float3 ApplyDisplayCalibration(float3 color, float saturation, float toe, float shoulder)
{
	const float3 shaped = ApplyDisplayToeAndShoulder(saturate(color), toe, shoulder);
	return saturate(ApplyDisplaySaturation(shaped, saturation));
}

float3 TonemapHable(float3 color)
{
	const float A = 0.15;
	const float B = 0.50;
	const float C = 0.10;
	const float D = 0.20;
	const float E = 0.02;
	const float F = 0.30;
	const float whitePoint = 11.2;
	const float3 numerator = ((color * (A * color + C * B) + D * E) / (color * (A * color + B) + D * F)) - E / F;
	const float white = (((whitePoint * (A * whitePoint + C * B) + D * E) / (whitePoint * (A * whitePoint + B) + D * F)) - E / F);
	return numerator / max(white, 1e-5);
}

float3 TonemapAcesFitted(float3 color)
{
	return saturate((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14));
}

float3 TonemapReinhard(float3 color)
{
	return color / (1.0 + color);
}

float3 ApplySdrTonemap(float3 color, uint tonemapMode)
{
	if (tonemapMode == NRI_PT_TONEMAP_ACES_FITTED)
	{
		return TonemapAcesFitted(color);
	}
	if (tonemapMode == NRI_PT_TONEMAP_REINHARD)
	{
		return TonemapReinhard(color);
	}

	return max(TonemapHable(color), 0.0);
}

float GetHdrSafePaperWhiteNits(float paperWhiteNits, float displaySdrLuminance, float displayMaxLuminance)
{
	const float safeDisplaySdr = max(displaySdrLuminance, 1.0);
	const float safeDisplayMax = max(displayMaxLuminance, safeDisplaySdr);
	return clamp(max(paperWhiteNits, safeDisplaySdr), safeDisplaySdr, safeDisplayMax);
}

float GetHdrPaperWhiteScale(float paperWhiteNits, float displaySdrLuminance, float displayMaxLuminance)
{
	return GetHdrSafePaperWhiteNits(paperWhiteNits, displaySdrLuminance, displayMaxLuminance) / 80.0;
}

float GetHdrHeadroomInPaperWhites(float paperWhiteNits, float displaySdrLuminance, float displayMaxLuminance)
{
	const float safeDisplaySdr = max(displaySdrLuminance, 1.0);
	const float safeDisplayMax = max(displayMaxLuminance, safeDisplaySdr);
	const float safePaperWhite = GetHdrSafePaperWhiteNits(paperWhiteNits, displaySdrLuminance, displayMaxLuminance);
	return max(safeDisplayMax / safePaperWhite, 1.0);
}

float GetHdrMaxOutputScale(float paperWhiteNits, float displaySdrLuminance, float displayMaxLuminance)
{
	return GetHdrPaperWhiteScale(paperWhiteNits, displaySdrLuminance, displayMaxLuminance) *
		GetHdrHeadroomInPaperWhites(paperWhiteNits, displaySdrLuminance, displayMaxLuminance);
}

float3 ApplyHdrOutputMapping(
	float3 color,
	uint tonemapMode,
	float saturation,
	float shoulder,
	float toe,
	float paperWhiteNits,
	float displaySdrLuminance,
	float displayMaxLuminance)
{
	const float paperWhiteScale = GetHdrPaperWhiteScale(paperWhiteNits, displaySdrLuminance, displayMaxLuminance);
	const float headroomInPaperWhites = GetHdrHeadroomInPaperWhites(paperWhiteNits, displaySdrLuminance, displayMaxLuminance);
	const float referenceInput = 1.0 / headroomInPaperWhites;
	const float referenceCurve = max(ApplySdrTonemap(referenceInput.xxx, tonemapMode).x, 1e-5);
	const float3 mappedPaperWhiteUnits = ApplySdrTonemap(color / headroomInPaperWhites, tonemapMode) / referenceCurve;
	const float3 normalized = saturate(mappedPaperWhiteUnits / headroomInPaperWhites.xxx);
	const float3 calibrated = ApplyDisplayCalibration(normalized, saturation, toe, shoulder);
	const float3 restoredPaperWhiteUnits = min(calibrated * headroomInPaperWhites, headroomInPaperWhites.xxx);
	return max(restoredPaperWhiteUnits * paperWhiteScale, 0.0);
}

float LinearToSrgbChannel(float value)
{
	value = max(value, 0.0);
	if (value <= 0.0031308)
	{
		return value * 12.92;
	}

	return 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}

float3 LinearToSrgb(float3 color)
{
	return float3(
		LinearToSrgbChannel(color.x),
		LinearToSrgbChannel(color.y),
		LinearToSrgbChannel(color.z));
}

float InterleavedGradientNoise(uint2 pixelPos, uint frameIndex)
{
	uint state = pixelPos.x * 0x1f123bb5u + pixelPos.y * 0x5f356495u + frameIndex * 0x6c8e9cf5u + 0x9e3779b9u;
	state ^= state >> 16u;
	state *= 0x7feb352du;
	state ^= state >> 15u;
	state *= 0x846ca68bu;
	state ^= state >> 16u;
	return (float)(state & 0x00ffffffu) * (1.0 / 16777215.0);
}

float3 ApplySdrTransferAndDither(float3 color, uint2 pixelPos, uint frameIndex)
{
	const float dither = InterleavedGradientNoise(pixelPos, frameIndex) - 0.5;
	const float3 srgb = LinearToSrgb(color);
	return saturate(srgb + dither / 255.0);
}

float3 ApplyLegacyClampOutputMapping(float3 color)
{
	return saturate(SanitizeFiniteColor(color));
}

float3 ApplyDebugRadianceDisplayMapping(float3 color, uint2 pixelPos, uint frameIndex)
{
	const float3 toneMapped = ApplySdrTonemap(SanitizeFiniteColor(color), NRI_PT_TONEMAP_HABLE);
	return ApplySdrTransferAndDither(toneMapped, pixelPos, frameIndex);
}

float3 ApplyDebugNormalizedDisplayMapping(float3 color, uint2 pixelPos, uint frameIndex)
{
	const float3 normalized = saturate(SanitizeFiniteColor(color));
	return ApplySdrTransferAndDither(normalized, pixelPos, frameIndex);
}

float3 ApplyPresentDisplayMapping(
	float3 color,
	uint2 pixelPos,
	uint frameIndex,
	uint outputMode,
	uint tonemapMode,
	uint outputFlags,
	float exposure,
	float contrast,
	float saturation,
	float shoulder,
	float toe,
	float paperWhiteNits,
	float displaySdrLuminance,
	float displayMaxLuminance)
{
	const bool hdrSwapChainActive =
		outputMode != NRI_PT_OUTPUT_MODE_SDR &&
		(outputFlags & NRI_PRESENT_OUTPUT_FLAG_HDR_SWAPCHAIN_ACTIVE) != 0u;
	const float3 sanitized = SanitizeFiniteColor(color);
	const float3 exposed = ApplyManualExposure(sanitized, exposure);
	const float3 calibratedScene = ApplyPresentSceneContrast(exposed, contrast);
	if (hdrSwapChainActive)
	{
		return ApplyHdrOutputMapping(calibratedScene, tonemapMode, saturation, shoulder, toe, paperWhiteNits, displaySdrLuminance, displayMaxLuminance);
	}

	const float3 toneMapped = ApplySdrTonemap(calibratedScene, tonemapMode);
	const float3 calibratedDisplay = ApplyDisplayCalibration(toneMapped, saturation, toe, shoulder);
	return ApplySdrTransferAndDither(calibratedDisplay, pixelPos, frameIndex);
}

#endif
