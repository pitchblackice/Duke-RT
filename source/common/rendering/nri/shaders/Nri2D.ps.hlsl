#include "Nri2DShared.hlsli"

Texture2D InputTexture : register(t0, space1);
SamplerState InputSampler : register(s0, space0);

struct PSInput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
	float4 texel = float4(1.0, 1.0, 1.0, 1.0);
	if ((gNri2DConstants.Flags & NRI2D_FLAG_TEXTURED) != 0)
	{
		// Temporary diagnostic: force textured 2D draws to a solid visible color.
		// If menus/videos become magenta, the remaining failure is texture upload/binding/sampling.
		return float4(1.0, 0.0, 1.0, 1.0);
	}

	if ((gNri2DConstants.Flags & NRI2D_FLAG_ALPHA_FROM_RED) != 0)
	{
		float alpha = max(texel.a, texel.r);
		texel = float4(1.0, 1.0, 1.0, alpha);
	}

	if ((gNri2DConstants.Flags & NRI2D_FLAG_INVERT) != 0)
	{
		texel.rgb = 1.0 - texel.rgb;
	}

	float4 color = texel * gNri2DConstants.VertexColor;
	color.rgb = color.rgb * gNri2DConstants.ObjectColor.rgb + gNri2DConstants.AddColor.rgb;
	color.a *= gNri2DConstants.ObjectColor.a;
	color *= gNri2DConstants.ScreenFade;
	return saturate(color);
}
