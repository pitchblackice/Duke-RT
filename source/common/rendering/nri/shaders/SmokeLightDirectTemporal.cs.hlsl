#include "Include/SmokeDirectCache.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint controlCount, occupiedCapacity, mediumCount, phaseCount, currentCount, historyCount, ignoredStride;
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, ignoredStride);
	gSmokeFroxelMedium.GetDimensions(mediumCount, ignoredStride);
	gSmokeFroxelPhase.GetDimensions(phaseCount, ignoredStride);
	gSmokeDirectCurrent.GetDimensions(currentCount, ignoredStride);
	gSmokeDirectHistory.GetDimensions(historyCount, ignoredStride);
	if (controlCount == 0u || dispatchThreadId.x >= min(gSmokeControl[0].OccupiedCount, occupiedCapacity))
		return;

	const uint froxelIndex = gSmokeOccupiedFroxelIndices[dispatchThreadId.x];
	if (froxelIndex >= SmokeFroxelCount() || froxelIndex >= mediumCount || froxelIndex >= phaseCount ||
		froxelIndex >= currentCount || froxelIndex >= historyCount ||
		!SmokeDirectFroxelIsGrid(gSmokeFroxelPhase[froxelIndex]))
		return;
	SmokeDirectCacheRecord current = gSmokeDirectCurrent[froxelIndex];
	if (!SmokeDirectRecordValid(current))
		return;
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	if (SmokeDirectReuseMode() < 1u || (gSmokeConstants.Flags & NRI_SMOKE_DIRECT_HISTORY_VALID) == 0u)
	{
		if (diagnostics) InterlockedAdd(gSmokeControl[0].DirectTemporalRejected, 1u);
		return;
	}

	uint previousIndex;
	if (!SmokePreviousFroxel(current.WorldPosition, previousIndex) || previousIndex >= historyCount)
	{
		if (diagnostics) InterlockedAdd(gSmokeControl[0].DirectTemporalRejected, 1u);
		return;
	}
	const SmokeDirectCacheRecord history = gSmokeDirectHistory[previousIndex];
	const uint3 froxel = SmokeFroxelCoordinates(froxelIndex);
	if (!SmokeDirectRecordsCompatible(current, history, froxel) ||
		SmokeDirectRecordFrame(history) != ((gSmokeConstants.FrameIndex - 1u) & 0xffu))
	{
		if (diagnostics) InterlockedAdd(gSmokeControl[0].DirectTemporalRejected, 1u);
		return;
	}
	// A temporal average must not turn a newly blocked receiver into a light
	// leak, nor retain a point light after it leaves this world-space cell.
	// Fractional penumbra samples can still fluctuate within the bounded gate.
	const float currentVisibility = SmokeDirectRecordVisibility(current);
	const float historyVisibility = SmokeDirectRecordVisibility(history);
	const float currentCombined = SmokeDirectRecordCombinedVisibility(current);
	const float currentLuminance = dot(max(current.Radiance, 0.0), float3(0.2126, 0.7152, 0.0722));
	const float historyLuminance = dot(max(history.Radiance, 0.0), float3(0.2126, 0.7152, 0.0722));
	const float signalTolerance = max(max(currentLuminance, historyLuminance) * 0.75, 0.02);
	if (abs(currentVisibility - historyVisibility) > 0.5 ||
		abs(currentLuminance - historyLuminance) > signalTolerance)
	{
		if (diagnostics) InterlockedAdd(gSmokeControl[0].DirectTemporalRejected, 1u);
		return;
	}

	const uint age = min(SmokeDirectRecordAge(history) + 1u, 255u);
	const float historyWeight = min((float)(age - 1u) / (float)age, 0.875);
	const float visibility = lerp(currentVisibility, historyVisibility, historyWeight);
	float resolvedCombined = currentCombined;
	float resolvedTransmittance = current.MediumTransmittance;
	uint mediumAge = 0u;
	uint mediumBlock = SmokeDirectMediumBlock(current);
	if (SmokeSelfShadowEnabled(gSmokeConstants.DebugMode) && SmokeDirectMediumValid(current) &&
		SmokeDirectMediumValid(history) && SmokeDirectMediumSelfShadow(history) &&
		SmokeDirectMediumFrame(history) == ((gSmokeConstants.FrameIndex - 1u) & 0xffu) &&
		SmokeDirectMediumBlock(history) == mediumBlock && SmokeDirectMediumAge(history) < 7u)
	{
		mediumAge = SmokeDirectMediumAge(history) + 1u;
		const float mediumAlpha = rcp((float)(mediumAge + 1u));
		resolvedTransmittance = lerp(history.MediumTransmittance, current.MediumTransmittance, mediumAlpha);
		resolvedCombined = lerp(SmokeDirectRecordCombinedVisibility(history), currentCombined, mediumAlpha);
	}
	else if (!SmokeSelfShadowEnabled(gSmokeConstants.DebugMode))
	{
		resolvedTransmittance = 1.0;
		resolvedCombined = visibility;
		mediumBlock = 0u;
	}
	if (!all(isfinite(current.Radiance)) || !isfinite(visibility))
	{
		if (diagnostics) InterlockedAdd(gSmokeControl[0].DirectNanRejects, 1u);
		return;
	}
	// Radiance contains current-view attenuation and HG phase. Reuse only the
	// view-independent fractional visibility; blending prior-camera radiance
	// makes a stationary plume contract or expand during camera rotation.
	current.Metadata = SmokeDirectPackMetadata(age, gSmokeConstants.FrameIndex, visibility, resolvedCombined);
	current.MediumTransmittance = saturate(resolvedTransmittance);
	current.MediumMetadata = SmokeDirectPackMediumMetadata(mediumAge, gSmokeConstants.FrameIndex, mediumBlock);
	gSmokeDirectCurrent[froxelIndex] = current;
	if (diagnostics)
	{
		InterlockedAdd(gSmokeControl[0].DirectTemporalAccepted, 1u);
		InterlockedMax(gSmokeControl[0].DirectHistoryMaximumAge, age);
	}
}
