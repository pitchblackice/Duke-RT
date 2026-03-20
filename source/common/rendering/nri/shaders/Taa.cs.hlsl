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
	uint PrimitiveCount;
	float3 SkyColor;
	uint DebugMode;
	float3 GroundColor;
	uint MaterialCount;
	uint FrameIndex;
	uint Flags;
	uint BootstrapMode;
	float Padding;
};

NRI_ROOT_CONSTANTS(NRITraceConstants, gTraceConstants, 0, 2);

Texture2D<float4> gHistoryInput : register(t0, space0);
Texture2D<float4> gMotionInput : register(t1, space0);
Texture2D<float4> gComposedInput : register(t2, space0);

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gHistoryOutput, u, 0, 1);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint width = 0;
	uint height = 0;
	gComposedInput.GetDimensions(width, height);
	if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	const float2 resolution = float2(width, height);
	const float2 uv = ((float2)pixelPos + 0.5) / resolution;

	if (gTraceConstants.DebugMode == 15)
	{
		const float checker = fmod(floor(uv.x * 16.0) + floor(uv.y * 16.0), 2.0);
		const float3 color = lerp(float3(0.1, 0.8, 0.2), float3(0.95, 0.15, 0.75), checker);
		gHistoryOutput[pixelPos] = float4(color, 1.0);
		return;
	}

	const float4 currentColor = gComposedInput[pixelPos];
	gHistoryOutput[pixelPos] = currentColor;
}
