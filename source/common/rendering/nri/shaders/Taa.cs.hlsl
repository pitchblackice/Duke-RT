#include "NRI.hlsl"

#define NRI_FLAG_RESET_HISTORY 0x1u
#define TAA_HISTORY_FRAME_CAP 12.0
#define TAA_BASE_BLEND (1.0 / TAA_HISTORY_FRAME_CAP)
#define TAA_SIGMA_SCALE 1.0
#define TAA_REJECTION_SCALE 2.5
#define TAA_HISTORY_EPSILON 1e-4
#define TAA_MOTION_BLEND_SCALE 1.5
#define TAA_MOTION_REJECT_PIXELS 2.0
#define TAA_MOTION_MIN_WEIGHT 0.2

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

Texture2D<float4> gHistoryInput : register(t0, space0);
Texture2D<float4> gMotionInput : register(t1, space0);
Texture2D<float4> gComposedInput : register(t2, space0);

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gHistoryOutput, u, 0, 1);

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

float3 LoadCurrentColor(int2 pixelPos, uint2 size)
{
	const int2 clampedPos = clamp(pixelPos, int2(0, 0), int2(size) - 1);
	return max(gComposedInput.Load(int3(clampedPos, 0)).rgb, 0.0);
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
	const float3 currentColor = LoadCurrentColor(int2(pixelPos), size);

	if (gTraceConstants.DebugMode == 26u)
	{
		gHistoryOutput[pixelPos] = float4(currentColor, 1.0);
		return;
	}

	const float4 centerMotion = gMotionInput[pixelPos];
	const bool unreliableHistory = centerMotion.w <= 0.0;
	const int radius = 1;
	float sum = 0.0;
	float3 mean = 0.0;
	float3 meanSquares = 0.0;
	float3 neighborhoodMin = 1e6;
	float3 neighborhoodMax = -1e6;
	float2 selectedMotion = centerMotion.xy;

	[loop]
	for (int y = -radius; y <= radius; ++y)
	{
		[loop]
		for (int x = -radius; x <= radius; ++x)
		{
			const int2 samplePos = clamp(int2(pixelPos) + int2(x, y), int2(0, 0), int2(size) - 1);
			const float3 sampleColor = LoadCurrentColor(samplePos, size);
			const float weight = exp(-0.5 * float(x * x + y * y));

			mean += sampleColor * weight;
			meanSquares += sampleColor * sampleColor * weight;
			sum += weight;
			neighborhoodMin = min(neighborhoodMin, sampleColor);
			neighborhoodMax = max(neighborhoodMax, sampleColor);
		}
	}

	mean /= max(sum, TAA_HISTORY_EPSILON);
	meanSquares /= max(sum, TAA_HISTORY_EPSILON);
	const float3 sigma = sqrt(max(meanSquares - mean * mean, 0.0)) * TAA_SIGMA_SCALE;
	const float3 clampMin = max(neighborhoodMin, mean - sigma);
	const float3 clampMax = min(neighborhoodMax, mean + sigma);
	const float2 prevUv = uv + selectedMotion / resolution;
	const bool historyInScreen = !unreliableHistory && all(prevUv >= 0.0) && all(prevUv <= 1.0);

	float4 historySample = float4(currentColor, 0.0);
	if (!resetHistory && historyInScreen)
	{
		historySample = SampleHistoryBilinear(prevUv, size);
	}

	const float3 historyColor = max(historySample.rgb, 0.0);
	const float3 clampedHistory = clamp(historyColor, clampMin, clampMax);
	const float divergence = MaxComponent(abs(historyColor - clampedHistory));
	const float historyScale = max(MaxComponent(currentColor), MaxComponent(clampedHistory));
	const float motionPixels = length(selectedMotion);
	const float motionRejection = saturate(motionPixels / TAA_MOTION_BLEND_SCALE);
	const float clampRejection = saturate(divergence / max(historyScale, 1.0) * TAA_REJECTION_SCALE);
	const float rejection = max(clampRejection, motionRejection);
	const bool rejectHistory = resetHistory || !historyInScreen || motionPixels >= TAA_MOTION_REJECT_PIXELS;

	float effectiveHistoryFrames = 1.0 + saturate(historySample.w) * (TAA_HISTORY_FRAME_CAP - 1.0);
	effectiveHistoryFrames = lerp(effectiveHistoryFrames, 1.0, rejection);

	float currentWeight = max(1.0 / (effectiveHistoryFrames + 1.0), TAA_BASE_BLEND);
	currentWeight = max(currentWeight, rejection);
	if (motionPixels > 0.0)
	{
		currentWeight = max(currentWeight, TAA_MOTION_MIN_WEIGHT);
	}
	if (rejectHistory)
	{
		currentWeight = 1.0;
		effectiveHistoryFrames = 1.0;
	}

	const float3 resultColor = lerp(clampedHistory, currentColor, currentWeight);
	const float nextHistoryFrames = rejectHistory ? 1.0 : min(effectiveHistoryFrames + 1.0, TAA_HISTORY_FRAME_CAP);
	const float nextHistoryAlpha = (nextHistoryFrames - 1.0) / (TAA_HISTORY_FRAME_CAP - 1.0);
	gHistoryOutput[pixelPos] = float4(resultColor, nextHistoryAlpha);
}
