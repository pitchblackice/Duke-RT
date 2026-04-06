#include "NRI.hlsl"
#include "Include/PresentConstants.hlsli"
#include "Include/DisplayMapping.hlsli"

NRI_ROOT_CONSTANTS(NRIPresentConstants, gPresentConstants, 0, 2);

Texture2D<float4> gInputTexture : register(t0, space0);
Texture2D<float4> gUnused1 : register(t1, space0);
Texture2D<float4> gUnused2 : register(t2, space0);

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float3>, gOutputTexture, u, 0, 1);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint2 targetSize;
	gOutputTexture.GetDimensions(targetSize.x, targetSize.y);
	const uint packedSceneOrigin = gPresentConstants.PackedSceneOrigin;
	const int2 sceneOrigin = int2((int)(packedSceneOrigin << 16) >> 16, (int)packedSceneOrigin >> 16);
	if (dispatchThreadId.x >= targetSize.x || dispatchThreadId.y >= targetSize.y)
	{
		return;
	}

	const uint2 targetPixelPos = dispatchThreadId.xy;
	const uint2 outputSize = uint2(max(gPresentConstants.DisplayWidth, 1u), max(gPresentConstants.DisplayHeight, 1u));
	const int2 pixelPos = int2(targetPixelPos) - sceneOrigin;
	if (pixelPos.x < 0 || pixelPos.y < 0 || pixelPos.x >= (int)outputSize.x || pixelPos.y >= (int)outputSize.y)
	{
		gOutputTexture[targetPixelPos] = 0.0;
		return;
	}

	const uint2 inputSize = uint2(max(gPresentConstants.InputWidth, 1u), max(gPresentConstants.InputHeight, 1u));
	const uint2 samplePos = min((uint2(pixelPos) * inputSize) / outputSize, inputSize - 1u);
	const float3 color = ApplyPresentDisplayMapping(
		gInputTexture.Load(int3(samplePos, 0)).rgb,
		targetPixelPos,
		gPresentConstants.FrameIndex,
		gPresentConstants.OutputMode,
		gPresentConstants.TonemapMode,
		gPresentConstants.OutputFlags,
		gPresentConstants.Exposure,
		gPresentConstants.PaperWhiteNits,
		gPresentConstants.DisplaySdrLuminance,
		gPresentConstants.DisplayMaxLuminance);
	gOutputTexture[targetPixelPos] = color;
}
