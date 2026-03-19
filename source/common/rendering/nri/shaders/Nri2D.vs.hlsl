#include "Nri2DShared.hlsli"

struct VSInput
{
	float3 Position : POSITION;
	float2 TexCoord : TEXCOORD0;
	float4 Color : COLOR0;
};

struct VSOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
	float4 Color : COLOR0;
};

VSOutput main(VSInput input)
{
	VSOutput output;

	float4 p = NriTransformPosition(float3(input.Position.x, input.Position.z, input.Position.y));
	float2 ndc = float2(p.x * InvViewportSize.x * 2.0 - 1.0, 1.0 - p.y * InvViewportSize.y * 2.0);

	output.Position = float4(ndc, p.z, 1.0);
	output.TexCoord = NriTransformTexcoord(input.TexCoord);
	output.Color = input.Color;
	return output;
}
