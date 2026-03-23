#include "NRI.hlsl"

struct NRITraceConstants
{
	float3 CameraPos;
	uint RenderWidth;
	float3 CameraForward;
	uint RenderHeight;
	float3 CameraRight;
	float TanHalfFovX;
	float3 CameraUp;
	float TanHalfFovY;
	float3 PrevCameraPos;
	uint DisplayWidth;
	float3 PrevCameraForward;
	uint DisplayHeight;
	float3 PrevCameraRight;
	float PrevTanHalfFovX;
	float3 PrevCameraUp;
	float PrevTanHalfFovY;
	float3 LightDirection;
	uint SceneInstanceCount;
	float3 SkyColor;
	uint DebugMode;
	float3 GroundColor;
	uint StaticPrimitiveCount;
	uint FrameIndex;
	uint DynamicPrimitiveCount;
	uint Flags;
	uint StaticMaterialCount;
	uint BootstrapMode;
	uint DynamicMaterialCount;
	uint BounceCounts;
};

NRI_ROOT_CONSTANTS(NRITraceConstants, gTraceConstants, 0, 2);

Texture2D<float4> gInputTexture : register(t0, space0);
Texture2D<float4> gUnused1 : register(t1, space0);
Texture2D<float4> gUnused2 : register(t2, space0);

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float3>, gOutputTexture, u, 0, 1);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gTraceConstants.DisplayWidth || dispatchThreadId.y >= gTraceConstants.DisplayHeight)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	const uint2 inputSize = uint2(max(gTraceConstants.RenderWidth, 1u), max(gTraceConstants.RenderHeight, 1u));
	const uint2 outputSize = uint2(max(gTraceConstants.DisplayWidth, 1u), max(gTraceConstants.DisplayHeight, 1u));
	const uint2 samplePos = min((pixelPos * inputSize) / outputSize, inputSize - 1u);
	const float3 color = saturate(gInputTexture.Load(int3(samplePos, 0)).rgb);
	gOutputTexture[pixelPos] = color;
}
