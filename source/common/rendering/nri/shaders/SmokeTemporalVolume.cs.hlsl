#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"
#include "Include/SmokeIndirectCache.hlsli"

#define NRI_SMOKE_VOLUME_HISTORY_VALID 0x1000u
#define NRI_SMOKE_VOLUME_HISTORY_ENABLED 0x2000u

float3 SmokeNormalizedVolumeRadiance(float4 volume)
{
	return max(volume.rgb, 0.0) / max(1.0 - exp(-max(volume.a, 0.0)), 1e-5);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint2 dimensions = uint2(gSmokeConstants.RenderWidth, gSmokeConstants.RenderHeight);
	if (dispatchThreadId.x >= dimensions.x || dispatchThreadId.y >= dimensions.y)
		return;
	const uint2 pixel = dispatchThreadId.xy;
	const float4 current = gSmokeVolumeCurrentInput.Load(int3(pixel, 0));
	const float4 currentMeta = gSmokeVolumeCurrentMetaInput.Load(int3(pixel, 0));
	if (!all(isfinite(current)) || current.a <= 1e-6 || currentMeta.x <= 1e-6)
	{
		gSmokeVolumeHistoryOutput[pixel] = 0.0;
		gSmokeVolumeMetaOutput[pixel] = 0.0;
		return;
	}

	bool accepted = (gSmokeConstants.Flags & (NRI_SMOKE_VOLUME_HISTORY_VALID | NRI_SMOKE_VOLUME_HISTORY_ENABLED)) ==
		(NRI_SMOKE_VOLUME_HISTORY_VALID | NRI_SMOKE_VOLUME_HISTORY_ENABLED);
	float4 history = current;
	float4 historyMeta = 0.0;
	if (accepted)
	{
		const float2 stableUv = SmokePrimarySampleUv(pixel);
		const float representativeDepth = currentMeta.y * gSmokeConstants.FroxelMaxDistance;
		const float3 worldPosition = SmokeWorldPosition(stableUv, representativeDepth);
		float2 previousStableUv, previousStorageUv;
		accepted = SmokePreviousUv(worldPosition, previousStableUv, previousStorageUv);
		if (accepted)
		{
			const uint2 previousPixel = min((uint2)(previousStorageUv * float2(dimensions)), dimensions - 1u);
			history = gSmokeVolumeHistoryInput.Load(int3(previousPixel, 0));
			historyMeta = gSmokeVolumeMetaInput.Load(int3(previousPixel, 0));
			accepted = all(isfinite(history)) && all(isfinite(historyMeta)) && history.a > 0.0 && historyMeta.x > 0.0 &&
				abs(historyMeta.x - currentMeta.x) <= 0.35 &&
				abs(historyMeta.y - currentMeta.y) <= 2.0 / max((float)gSmokeConstants.FroxelDepth, 1.0) &&
				abs(historyMeta.z - currentMeta.z) <= 0.05;
		}
	}

	float historyWeight = 0.0;
	if (accepted)
	{
		float minimumTau = current.a;
		float maximumTau = current.a;
		float3 minimumNormalized = SmokeNormalizedVolumeRadiance(current);
		float3 maximumNormalized = minimumNormalized;
		[unroll]
		for (int y = -1; y <= 1; ++y)
		{
			[unroll]
			for (int x = -1; x <= 1; ++x)
			{
				const int2 neighborPixel = clamp(int2(pixel) + int2(x, y), int2(0, 0), int2(dimensions) - 1);
				const float4 neighbor = gSmokeVolumeCurrentInput.Load(int3(neighborPixel, 0));
				if (neighbor.a <= 1e-6 || !all(isfinite(neighbor)))
					continue;
				minimumTau = min(minimumTau, neighbor.a);
				maximumTau = max(maximumTau, neighbor.a);
				const float3 normalized = SmokeNormalizedVolumeRadiance(neighbor);
				minimumNormalized = min(minimumNormalized, normalized);
				maximumNormalized = max(maximumNormalized, normalized);
			}
		}
		const float clampedTau = clamp(history.a, minimumTau, maximumTau);
		const float3 clampedNormalized = clamp(SmokeNormalizedVolumeRadiance(history), minimumNormalized, maximumNormalized);
		history = float4(clampedNormalized * (1.0 - exp(-clampedTau)), clampedTau);
		historyWeight = min(max(historyMeta.w, 0.125), 0.75);
	}
	const float4 resolved = lerp(current, history, historyWeight);
	const float age = accepted ? min(historyMeta.w + 0.125, 1.0) : 0.125;
	gSmokeVolumeHistoryOutput[pixel] = resolved;
	gSmokeVolumeMetaOutput[pixel] = float4(currentMeta.xyz, age);
}
