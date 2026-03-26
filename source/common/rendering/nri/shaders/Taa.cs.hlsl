#include "NRI.hlsl"

#define NRI_FLAG_RESET_HISTORY 0x1u
#define TAA_HISTORY_FRAME_CAP 32.0
#define TAA_SIGMA_SCALE 1.5
#define TAA_REJECTION_SCALE 8.0
#define TAA_HISTORY_EPSILON 1e-4

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

Texture2D<float4> gHistoryInput : register(t0, space0);
Texture2D<float4> gMotionInput : register(t1, space0);
Texture2D<float4> gComposedInput : register(t2, space0);

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gHistoryOutput, u, 0, 1);

float3 EncodeColor(float3 color)
{
	color = max(color, 0.0);
	return color / (1.0 + color);
}

float3 DecodeColor(float3 encoded)
{
	encoded = clamp(encoded, 0.0, 0.999);
	return encoded / max(1.0 - encoded, TAA_HISTORY_EPSILON);
}

float4 LoadHistory(int2 pixelPos, uint2 size)
{
	const int2 clampedPos = clamp(pixelPos, int2(0, 0), int2(size) - 1);
	return gHistoryInput.Load(int3(clampedPos, 0));
}

float4 SampleHistoryBilinear(float2 uv, uint2 size)
{
	const float2 texelPos = uv * float2(size) - 0.5;
	const int2 basePos = int2(floor(texelPos));
	const float2 blend = frac(texelPos);
	const float4 h00 = LoadHistory(basePos, size);
	const float4 h10 = LoadHistory(basePos + int2(1, 0), size);
	const float4 h01 = LoadHistory(basePos + int2(0, 1), size);
	const float4 h11 = LoadHistory(basePos + int2(1, 1), size);
	const float4 hx0 = lerp(h00, h10, blend.x);
	const float4 hx1 = lerp(h01, h11, blend.x);
	return lerp(hx0, hx1, blend.y);
}

float MaxComponent(float3 value)
{
	return max(value.x, max(value.y, value.z));
}

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
	const uint2 size = uint2(width, height);
	const float2 resolution = float2(size);
	const float2 uv = ((float2)pixelPos + 0.5) / resolution;
	const bool resetHistory = (gTraceConstants.Flags & NRI_FLAG_RESET_HISTORY) != 0u;

	const float4 centerMotion = gMotionInput[pixelPos];
	const bool want5x5 = centerMotion.w < 0.0;
	const int radius = want5x5 ? 2 : 1;
	float sum = 0.0;
	float3 mean = 0.0;
	float3 meanSquares = 0.0;
	float3 neighborhoodMin = 1e6;
	float3 neighborhoodMax = -1e6;
	float minDepthLike = abs(centerMotion.w);
	float2 selectedMotion = centerMotion.xy;

	[loop]
	for (int y = -radius; y <= radius; ++y)
	{
		[loop]
		for (int x = -radius; x <= radius; ++x)
		{
			if (!want5x5 && max(abs(x), abs(y)) > 1)
			{
				continue;
			}

			const int2 samplePos = clamp(int2(pixelPos) + int2(x, y), int2(0, 0), int2(size) - 1);
			const float3 encodedColor = EncodeColor(gComposedInput.Load(int3(samplePos, 0)).rgb);
			const float4 sampleMotion = gMotionInput.Load(int3(samplePos, 0));
			const float weight = exp(-0.5 * float(x * x + y * y));

			mean += encodedColor * weight;
			meanSquares += encodedColor * encodedColor * weight;
			sum += weight;
			neighborhoodMin = min(neighborhoodMin, encodedColor);
			neighborhoodMax = max(neighborhoodMax, encodedColor);

			const float depthLike = abs(sampleMotion.w);
			if (depthLike > 0.0 && depthLike < minDepthLike)
			{
				minDepthLike = depthLike;
				selectedMotion = sampleMotion.xy;
			}
		}
	}

	mean /= max(sum, TAA_HISTORY_EPSILON);
	meanSquares /= max(sum, TAA_HISTORY_EPSILON);
	const float3 sigma = sqrt(max(meanSquares - mean * mean, 0.0)) * TAA_SIGMA_SCALE;
	const float3 clampMin = max(neighborhoodMin, mean - sigma);
	const float3 clampMax = min(neighborhoodMax, mean + sigma);
	const float2 prevUv = uv + selectedMotion / resolution;
	const bool historyInScreen = all(prevUv >= 0.0) && all(prevUv <= 1.0);

	float3 currentColor = max(gComposedInput[pixelPos].rgb, 0.0);
	float4 historySample = float4(currentColor, 0.0);
	if (!resetHistory && historyInScreen)
	{
		historySample = SampleHistoryBilinear(prevUv, size);
	}

	const float3 historyEncoded = EncodeColor(historySample.rgb);
	const float3 clampedHistoryEncoded = clamp(historyEncoded, clampMin, clampMax);
	const float rejection = saturate(MaxComponent(abs(historyEncoded - clampedHistoryEncoded)) * TAA_REJECTION_SCALE);
	const bool rejectHistory = resetHistory || !historyInScreen || rejection >= 0.75;

	float effectiveHistoryFrames = 1.0 + saturate(historySample.w) * (TAA_HISTORY_FRAME_CAP - 1.0);
	effectiveHistoryFrames = lerp(effectiveHistoryFrames, 1.0, rejection);

	float currentWeight = 1.0 / (effectiveHistoryFrames + 1.0);
	currentWeight = max(currentWeight, rejection);
	if (want5x5)
	{
		currentWeight = max(currentWeight, 0.125);
	}
	if (rejectHistory)
	{
		currentWeight = 1.0;
		effectiveHistoryFrames = 1.0;
	}

	const float3 clampedHistory = DecodeColor(clampedHistoryEncoded);
	const float3 resultColor = lerp(clampedHistory, currentColor, currentWeight);
	const float nextHistoryFrames = rejectHistory ? 1.0 : min(effectiveHistoryFrames + 1.0, TAA_HISTORY_FRAME_CAP);
	const float nextHistoryAlpha = (nextHistoryFrames - 1.0) / (TAA_HISTORY_FRAME_CAP - 1.0);
	gHistoryOutput[pixelPos] = float4(resultColor, nextHistoryAlpha);
}
