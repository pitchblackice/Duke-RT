#include "NRI.hlsl"
#include "NRD.hlsli"

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
	uint PortalCount;
	uint RuntimeLightCount;
	uint PortalDepth;
	uint ReservedTrace0;
	uint ReservedTrace1;
};

NRI_ROOT_CONSTANTS(NRITraceConstants, gTraceConstants, 0, 5);

Texture2D<float4> gViewZInput : register(t2, space3);
Texture2D<float4> gNormalRoughnessInput : register(t3, space3);
Texture2D<float4> gBaseColorInput : register(t4, space3);
Texture2D<float4> gComposedInput : register(t5, space3);

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gRrGuideNormalRoughnessOutput, u, 5, 4);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gVendorInputOutput, u, 8, 4);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gRrGuideDiffuseAlbedoOutput, u, 9, 4);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gRrGuideSpecularAlbedoOutput, u, 10, 4);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float>, gRrGuideSpecHitDistanceOutput, u, 11, 4);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float>, gUpscalerDepthOutput, u, 12, 4);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gTraceConstants.RenderWidth || dispatchThreadId.y >= gTraceConstants.RenderHeight)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	const float4 baseColor = saturate(gBaseColorInput[pixelPos]);
	const float4 composed = gComposedInput[pixelPos];
	const float viewZ = abs(gViewZInput[pixelPos].x);

	float materialID = 0.0;
	const float4 unpackedNormalRoughness = NRD_FrontEnd_UnpackNormalAndRoughness(gNormalRoughnessInput[pixelPos], materialID);

	gVendorInputOutput[pixelPos] = composed;
	gRrGuideDiffuseAlbedoOutput[pixelPos] = float4(baseColor.rgb, 1.0);
	gRrGuideSpecularAlbedoOutput[pixelPos] = float4(baseColor.rgb, 1.0);
	gRrGuideSpecHitDistanceOutput[pixelPos] = viewZ;
	gUpscalerDepthOutput[pixelPos] = viewZ;
	gRrGuideNormalRoughnessOutput[pixelPos] = float4(unpackedNormalRoughness.xyz, saturate(unpackedNormalRoughness.w));
}
