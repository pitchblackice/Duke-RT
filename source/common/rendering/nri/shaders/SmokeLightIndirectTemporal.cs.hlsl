#include "Include/SmokeIndirectCache.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint controlCount, occupiedCapacity, mediumCount, phaseCount, scratchCount, historyCount, ignoredStride;
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, ignoredStride);
	gSmokeFroxelMedium.GetDimensions(mediumCount, ignoredStride);
	gSmokeFroxelPhase.GetDimensions(phaseCount, ignoredStride);
	gSmokeIndirectScratch.GetDimensions(scratchCount, ignoredStride);
	gSmokeIndirectHistory.GetDimensions(historyCount, ignoredStride);
	if (controlCount == 0u || dispatchThreadId.x >= min(gSmokeControl[0].OccupiedCount, occupiedCapacity))
		return;

	const uint froxelIndex = gSmokeOccupiedFroxelIndices[dispatchThreadId.x];
	if (froxelIndex >= SmokeFroxelCount() || froxelIndex >= mediumCount || froxelIndex >= phaseCount ||
		froxelIndex >= scratchCount || froxelIndex >= historyCount)
		return;
	SmokeIndirectCacheRecord current = gSmokeIndirectScratch[froxelIndex];
	if (!SmokeIndirectRecordValid(current))
		return;
	if (SmokeIndirectCacheMode() < 2u || (gSmokeConstants.Flags & NRI_SMOKE_INDIRECT_HISTORY_VALID) == 0u)
		return;
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;

	const uint3 froxel = SmokeFroxelCoordinates(froxelIndex);
	const float3 ray = SmokeFroxelRay(froxel.xy);
	const float3 worldPosition = SmokeFroxelCenter(froxel, ray);
	uint previousIndex;
	if (!SmokePreviousFroxel(worldPosition, previousIndex) || previousIndex >= historyCount)
	{
		if (diagnostics) InterlockedAdd(gSmokeControl[0].IndirectTemporalRejected, 1u);
		return;
	}
	const SmokeIndirectCacheRecord history = gSmokeIndirectHistory[previousIndex];
	if (!SmokeIndirectRecordsCompatible(current, history) ||
		SmokeIndirectRecordFrame(history) != ((gSmokeConstants.FrameIndex - 1u) & 0x3fu) ||
		length(history.WorldPosition - worldPosition) > SmokeIndirectWorldTolerance(froxel))
	{
		if (diagnostics) InterlockedAdd(gSmokeControl[0].IndirectTemporalRejected, 1u);
		return;
	}

	const float currentLuminance = dot(max(current.Radiance, 0.0), float3(0.2126, 0.7152, 0.0722));
	const float historyLuminance = dot(max(history.Radiance, 0.0), float3(0.2126, 0.7152, 0.0722));
	if (currentLuminance <= 1e-5 && historyLuminance > 1e-5)
	{
		if (diagnostics) InterlockedAdd(gSmokeControl[0].IndirectTemporalRejected, 1u);
		return;
	}
	const float maximumHistoryLuminance = max(currentLuminance * 4.0, 1e-5);
	float3 boundedHistory = max(history.Radiance, 0.0);
	if (historyLuminance > maximumHistoryLuminance)
	{
		boundedHistory *= maximumHistoryLuminance / historyLuminance;
		if (diagnostics) InterlockedAdd(gSmokeControl[0].IndirectCacheClamps, 1u);
	}
	const uint age = min(SmokeIndirectRecordAge(history) + 1u, 15u);
	const float historyWeight = min((float)(age - 1u) / (float)age, 0.875);
	current.Radiance = lerp(max(current.Radiance, 0.0), boundedHistory, historyWeight);
	current.Metadata = SmokePackIndirectMetadata(SmokeIndirectRecordSector(current),
		gSmokeFroxelMedium[froxelIndex], gSmokeFroxelPhase[froxelIndex].x, age, gSmokeConstants.FrameIndex);
	gSmokeIndirectScratch[froxelIndex] = current;
	if (diagnostics)
	{
		InterlockedAdd(gSmokeControl[0].IndirectTemporalAccepted, 1u);
		InterlockedMax(gSmokeControl[0].IndirectCacheMaximumAge, age);
	}
}
