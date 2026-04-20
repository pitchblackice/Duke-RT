#include "Nri2DShared.hlsli"

struct VSInput
{
	float3 Position : POSITION;
	float2 TexCoord : TEXCOORD0;
	float4 Color : COLOR;
};

struct VSOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
	float4 Color : COLOR;
};

VSOutput main(VSInput input)
{
	VSOutput output;

	// F2DDrawer uploads screen-space vertices as (x, y, z). Preserve that order here.
	float4 p = NriTransformPosition(input.Position);
	float2 ndc = float2(p.x * gNri2DConstants.InvViewportSize.x * 2.0 - 1.0, 1.0 - p.y * gNri2DConstants.InvViewportSize.y * 2.0);

	output.Position = float4(ndc, p.z, 1.0);
	output.TexCoord = NriTransformTexcoord(input.TexCoord);
	output.Color = input.Color;
	return output;
}
