#include "NRI.hlsl"
#include "NRD.hlsli"
#include "Include/TraceConstants.hlsli"

NRI_ROOT_CONSTANTS(NRITraceSceneConstants, gTraceConstants, 0, 5);

struct ReprojectionData
{
	float4 currentViewToClipMatrix[4];
	float4 previousViewToClipMatrix[4];
	float4 currentWorldToViewMatrix[4];
	float4 previousWorldToViewMatrix[4];
	float2 currentJitter;
	float2 previousJitter;
};

StructuredBuffer<ReprojectionData> gReprojectionDataBuffer : register(t18, space2);
Texture2D<float4> gViewZInput : register(t2, space3);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float>, gUpscalerDepthOutput, u, 12, 4);

float4 MultiplyVsMatrixPoint(float4 v, float4 matrixColumns[4])
{
	return float4(
		dot(v, float4(matrixColumns[0].x, matrixColumns[1].x, matrixColumns[2].x, matrixColumns[3].x)),
		dot(v, float4(matrixColumns[0].y, matrixColumns[1].y, matrixColumns[2].y, matrixColumns[3].y)),
		dot(v, float4(matrixColumns[0].z, matrixColumns[1].z, matrixColumns[2].z, matrixColumns[3].z)),
		dot(v, float4(matrixColumns[0].w, matrixColumns[1].w, matrixColumns[2].w, matrixColumns[3].w)));
}

float3 ReconstructViewPosition(uint2 pixelPos, float viewZ)
{
	const float2 resolution = float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
	float2 uv = ((float2)pixelPos + 0.5) / resolution;
	float2 ndc = uv * 2.0 - 1.0;
	ndc.y = -ndc.y;

	return float3(
		ndc.x * gTraceConstants.TanHalfFovX * viewZ,
		ndc.y * gTraceConstants.TanHalfFovY * viewZ,
		-viewZ);
}

float ConvertViewZToClipDepth(uint2 pixelPos, float viewZ)
{
	if (abs(viewZ) >= NRD_INF * 0.5)
	{
		return 1.0;
	}

	const ReprojectionData reprojection = gReprojectionDataBuffer[0];
	const float3 viewPos = ReconstructViewPosition(pixelPos, viewZ);
	const float4 clip = MultiplyVsMatrixPoint(float4(viewPos, 1.0), reprojection.currentViewToClipMatrix);
	return clip.w > 1e-5 ? clip.z / clip.w : 1.0;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gTraceConstants.RenderWidth || dispatchThreadId.y >= gTraceConstants.RenderHeight)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	const float viewZ = abs(gViewZInput[pixelPos].x);
	gUpscalerDepthOutput[pixelPos] = ConvertViewZToClipDepth(pixelPos, viewZ);
}
