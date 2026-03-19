#include "Nri2DShared.hlsli"

Texture2D InputTexture : register(t0, space1);
SamplerState InputSampler : register(s0, space0);

struct PSInput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
	float4 Color : COLOR0;
};

float4 main(PSInput input) : SV_Target0
{
	float4 texel = float4(1.0, 1.0, 1.0, 1.0);
	if ((Flags & NRI2D_FLAG_TEXTURED) != 0)
	{
		texel = InputTexture.Sample(InputSampler, input.TexCoord);
	}

	if ((Flags & NRI2D_FLAG_ALPHA_FROM_RED) != 0)
	{
		float alpha = max(texel.a, texel.r);
		texel = float4(1.0, 1.0, 1.0, alpha);
	}

	if ((Flags & NRI2D_FLAG_INVERT) != 0)
	{
		texel.rgb = 1.0 - texel.rgb;
	}

	float4 color = texel * input.Color;
	color.rgb = color.rgb * ObjectColor.rgb + AddColor.rgb;
	color.a *= ObjectColor.a;
	color *= ScreenFade;
	return saturate(color);
}
