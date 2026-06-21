#include "NRI.hlsl"
#include "Include/BloomConstants.hlsli"

NRI_ROOT_CONSTANTS(NRIBloomConstants, gBloomConstants, 0, 2);

Texture2D<float4> gInputTexture : register(t0, space0);
Texture2D<float4> gBloomTexture : register(t1, space0);

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gOutputTexture, u, 0, 1);

bool AnyNonFinite(float3 value)
{
	return any(isnan(value)) || any(isinf(value));
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
	const uint2 scenePixel = min((dispatchThreadId.xy * inputSize) / outputExtent, inputSize - 1u);

	uint2 bloomSize;
	gBloomTexture.GetDimensions(bloomSize.x, bloomSize.y);
	bloomSize = max(bloomSize, uint2(1u, 1u));
	const uint2 bloomPixel = min((dispatchThreadId.xy * bloomSize) / outputExtent, bloomSize - 1u);

	float3 sceneColor = gInputTexture.Load(int3(scenePixel, 0)).rgb;
	const float3 bloomColor = gBloomTexture.Load(int3(bloomPixel, 0)).rgb;
	if (AnyNonFinite(sceneColor))
	{
		sceneColor = 0.0;
	}

	const float intensity = max(gBloomConstants.Intensity, 0.0);
	float3 outputColor = sceneColor + max(bloomColor, 0.0) * intensity;
	if ((gBloomConstants.Flags & NRI_BLOOM_FLAG_ENERGY_CONSTRAINED) != 0u)
	{
		outputColor /= 1.0 + intensity;
	}

	gOutputTexture[dispatchThreadId.xy] = float4(outputColor, 1.0);
}
