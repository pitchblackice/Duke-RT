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

NRI_ROOT_CONSTANTS(NRITraceConstants, gTraceConstants, 0, 2);

static const uint NRI_FLAG_RAW_PRESENT_ADD_SECONDARY = 0x10u;

Texture2D<float4> gInputTexture : register(t0, space0);
Texture2D<float4> gUnused1 : register(t1, space0);
Texture2D<float4> gUnused2 : register(t2, space0);

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float3>, gOutputTexture, u, 0, 1);

float3 ToneMapDebugRadiance(float3 value)
{
	value = max(value, 0.0);
	value *= 4.0;
	return value / (1.0 + value);
}

bool UseRelaxDenoiser()
{
	return (gTraceConstants.ReservedTrace1 & 0xffu) == 1u;
}

float3 UnpackDebugRadiance(float4 packed)
{
	return UseRelaxDenoiser() ? RELAX_BackEnd_UnpackRadiance(packed).rgb : REBLUR_BackEnd_UnpackRadianceAndNormHitDist(packed).rgb;
}

static const float4 kReblurHitDistanceParams = float4(3.0, 0.1, 20.0, -25.0);

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
	float3 color = gInputTexture.Load(int3(samplePos, 0)).rgb;
	if ((gTraceConstants.Flags & NRI_FLAG_RAW_PRESENT_ADD_SECONDARY) != 0u)
	{
		color += gUnused1.Load(int3(samplePos, 0)).rgb;
	}
	if (gTraceConstants.DebugMode == 10u || gTraceConstants.DebugMode == 11u || gTraceConstants.DebugMode == 16u || gTraceConstants.DebugMode == 17u)
	{
		color = ToneMapDebugRadiance(UnpackDebugRadiance(gInputTexture.Load(int3(samplePos, 0))));
	}
	else
	if (gTraceConstants.DebugMode == 12u)
	{
		const float viewZ = abs(gUnused1.Load(int3(samplePos, 0)).x);
		if (viewZ >= NRD_INF * 0.5)
		{
			color = float3(1.0, 0.0, 0.0);
		}
		else if (UseRelaxDenoiser())
		{
			const float hitDistance = max(gInputTexture.Load(int3(samplePos, 0)).a, 0.0);
			const float mapped = saturate(log2(1.0 + hitDistance) / 12.0);
			color = mapped.xxx;
		}
		else
		{
			const float normalizedHitDistance = saturate(gInputTexture.Load(int3(samplePos, 0)).a);
			float materialID = 0.0;
			const float roughness = NRD_FrontEnd_UnpackNormalAndRoughness(gUnused2.Load(int3(samplePos, 0)), materialID).w;
			const float hitDistance = REBLUR_GetHitDist(normalizedHitDistance, viewZ, kReblurHitDistanceParams, roughness);
			const float mapped = saturate(log2(1.0 + max(hitDistance, 0.0)) / 12.0);
			color = mapped.xxx;
		}
	}
	else
	{
		color = saturate(color);
	}
	gOutputTexture[pixelPos] = color;
}
