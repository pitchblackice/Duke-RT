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
	const uint2 outputExtent = uint2(max(gBloomConstants.OutputWidth, 1u), max(gBloomConstants.OutputHeight, 1u));
	const int2 centerPixel = int2(min((dispatchThreadId.xy * inputSize) / outputExtent, inputSize - 1u));
	const int radius = 1 + (int)round(saturate(gBloomConstants.Sigma) * 2.0);
	const float sigma = lerp(0.65, 2.0, saturate(gBloomConstants.Sigma));
	const float invTwoSigmaSq = 0.5 / max(sigma * sigma, 0.0001);

	float3 upsampled = 0.0;
	float weightSum = 0.0;
	[loop]
	for (int y = -radius; y <= radius; ++y)
	{
		[loop]
		for (int x = -radius; x <= radius; ++x)
		{
			const float weight = exp(-(float)(x * x + y * y) * invTwoSigmaSq);
			upsampled += LoadClamped(centerPixel + int2(x, y), inputSize) * weight;
			weightSum += weight;
		}
	}
	upsampled /= max(weightSum, 0.0001);

	const float3 existing = gOutputTexture[dispatchThreadId.xy].rgb;
	gOutputTexture[dispatchThreadId.xy] = float4(max(existing + upsampled, 0.0), 1.0);
}
