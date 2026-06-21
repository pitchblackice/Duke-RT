#include "NRI.hlsl"
#include "Include/BloomConstants.hlsli"

NRI_ROOT_CONSTANTS(NRIBloomConstants, gBloomConstants, 0, 2);

Texture2D<float4> gInputTexture : register(t0, space0);
Texture2D<float4> gSecondaryInputTexture : register(t1, space0);

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gOutputTexture, u, 0, 1);

float3 SanitizeColor(float3 color)
{
	if (any(isnan(color)) || any(isinf(color)))
	{
		return 0.0;
	}

	return max(color, 0.0);
}

float3 LoadClamped(uint2 pixelPos, uint2 maxPixel)
{
	return SanitizeColor(gInputTexture.Load(int3(min(pixelPos, maxPixel), 0)).rgb);
}

float Bt709Luminance(float3 color)
{
	return dot(color, float3(0.2126, 0.7152, 0.0722));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint2 outputSize;
	gOutputTexture.GetDimensions(outputSize.x, outputSize.y);
	if (dispatchThreadId.x >= outputSize.x || dispatchThreadId.y >= outputSize.y)
	{
		return;
	}

	const uint2 inputSize = uint2(max(gBloomConstants.InputWidth, 1u), max(gBloomConstants.InputHeight, 1u));
	const uint2 maxPixel = inputSize - 1u;
	const uint2 basePixel = dispatchThreadId.xy * 2u;

	float3 color =
		(
			LoadClamped(basePixel, maxPixel) +
			LoadClamped(basePixel + uint2(1u, 0u), maxPixel) +
			LoadClamped(basePixel + uint2(0u, 1u), maxPixel) +
			LoadClamped(basePixel + uint2(1u, 1u), maxPixel)
		) * 0.25;

	if ((gBloomConstants.Flags & NRI_BLOOM_FLAG_THRESHOLD) != 0u)
	{
		const float cutoff = max(gBloomConstants.Cutoff, 0.0001);
		const float fuzziness = saturate(gBloomConstants.Fuzziness);
		const float edge0 = max(0.0, cutoff * (1.0 - fuzziness));
		const float edge1 = max(edge0 + 0.0001, cutoff);
		color *= smoothstep(edge0, edge1, Bt709Luminance(color));
	}

	gOutputTexture[dispatchThreadId.xy] = float4(max(color, 0.0), 1.0);
}
