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

float3 ApplyHdrOutputMapping(float3 color, float paperWhiteNits, float displaySdrLuminance, float displayMaxLuminance)
{
	const float safePaperWhite = max(paperWhiteNits, 1.0);
	const float safeDisplaySdr = max(displaySdrLuminance, 1.0);
	const float safeDisplayMax = max(displayMaxLuminance, safeDisplaySdr);
	const float paperWhiteScale = safePaperWhite / safeDisplaySdr;
	const float maxScale = safeDisplaySdr / safeDisplayMax;
	return max(color * paperWhiteScale * maxScale, 0.0);
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

float3 ApplyPresentDisplayMapping(
	float3 color,
	uint2 pixelPos,
	uint frameIndex,
	uint outputMode,
	uint tonemapMode,
	uint outputFlags,
	float exposure,
	float paperWhiteNits,
	float displaySdrLuminance,
	float displayMaxLuminance)
{
	const bool hdrSwapChainActive =
		outputMode != NRI_PT_OUTPUT_MODE_SDR &&
		(outputFlags & NRI_PRESENT_OUTPUT_FLAG_HDR_SWAPCHAIN_ACTIVE) != 0u;
	const float3 sanitized = SanitizeFiniteColor(color);
	const float3 exposed = ApplyManualExposure(sanitized, exposure);
	if (hdrSwapChainActive)
	{
		return ApplyHdrOutputMapping(exposed, paperWhiteNits, displaySdrLuminance, displayMaxLuminance);
	}

	const float3 toneMapped = ApplySdrTonemap(exposed, tonemapMode);
	return ApplySdrTransferAndDither(toneMapped, pixelPos, frameIndex);
}

#endif
