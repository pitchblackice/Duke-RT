#ifndef RAZE_NRI_2D_SHARED_HLSLI
#define RAZE_NRI_2D_SHARED_HLSLI

#include "NRI.hlsl"

struct Nri2DConstants
{
	float2 InvViewportSize;
	uint Flags;
	float ScreenFade;

	float4 ObjectColor;
	float4 AddColor;
	float4 VertexColor;

	float4x4 ModelMatrix;
	float4x4 TexMatrix;
};

NRI_ROOT_CONSTANTS(Nri2DConstants, gNri2DConstants, 0, 2);

static const uint NRI2D_FLAG_TEXTURED = 1u << 0;
static const uint NRI2D_FLAG_ALPHA_FROM_RED = 1u << 1;
static const uint NRI2D_FLAG_INVERT = 1u << 2;

float4 NriTransformPosition(float3 p)
{
	return mul(gNri2DConstants.ModelMatrix, float4(p.x, p.y, p.z, 1.0));
}

float2 NriTransformTexcoord(float2 uv)
{
	return mul(gNri2DConstants.TexMatrix, float4(uv, 0.0, 1.0)).xy;
}

#endif
