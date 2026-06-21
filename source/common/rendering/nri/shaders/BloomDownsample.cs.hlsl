#include "NRI.hlsl"
#include "Include/BloomConstants.hlsli"

NRI_ROOT_CONSTANTS(NRIBloomConstants, gBloomConstants, 0, 2);

Texture2D<float4> gInputTexture : register(t0, space0);
Texture2D<float4> gSecondaryInputTexture : register(t1, space0);

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gOutputTexture, u, 0, 1);

float3 LoadClamped(int2 pixelPos, uint2 size)
{
	const int2 clampedPos = clamp(pixelPos, int2(0, 0), int2(size) - 1);
	return gInputTexture.Load(int3(clampedPos, 0)).rgb;
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
	const int2 basePixel = int2(dispatchThreadId.xy) * 2;

	const float3 a = LoadClamped(basePixel + int2(-2, -2), inputSize);
	const float3 b = LoadClamped(basePixel + int2( 0, -2), inputSize);
	const float3 c = LoadClamped(basePixel + int2( 2, -2), inputSize);
	const float3 d = LoadClamped(basePixel + int2(-2,  0), inputSize);
	const float3 e = LoadClamped(basePixel + int2( 0,  0), inputSize);
	const float3 f = LoadClamped(basePixel + int2( 2,  0), inputSize);
	const float3 g = LoadClamped(basePixel + int2(-2,  2), inputSize);
	const float3 h = LoadClamped(basePixel + int2( 0,  2), inputSize);
	const float3 i = LoadClamped(basePixel + int2( 2,  2), inputSize);
	const float3 j = LoadClamped(basePixel + int2(-1, -1), inputSize);
	const float3 k = LoadClamped(basePixel + int2( 1, -1), inputSize);
	const float3 l = LoadClamped(basePixel + int2(-1,  1), inputSize);
	const float3 m = LoadClamped(basePixel + int2( 1,  1), inputSize);

	float3 color =
		e * 0.125 +
		(a + c + g + i) * 0.03125 +
		(b + d + f + h) * 0.0625 +
		(j + k + l + m) * 0.125;

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
