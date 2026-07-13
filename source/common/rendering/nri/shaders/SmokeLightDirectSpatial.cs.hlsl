#include "Include/SmokeDirectCache.hlsli"

static const int3 kSmokeDirectNeighborOffsets[6] = {
	int3(-1, 0, 0), int3(1, 0, 0), int3(0, -1, 0),
	int3(0, 1, 0), int3(0, 0, -1), int3(0, 0, 1)
};

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint controlCount, occupiedCapacity, mediumCount, phaseCount, sourceCount, currentCount, historyCount, ignoredStride;
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, ignoredStride);
	gSmokeFroxelMedium.GetDimensions(mediumCount, ignoredStride);
	gSmokeFroxelPhase.GetDimensions(phaseCount, ignoredStride);
	gSmokeFroxelSource.GetDimensions(sourceCount, ignoredStride);
	gSmokeDirectCurrent.GetDimensions(currentCount, ignoredStride);
	gSmokeDirectHistory.GetDimensions(historyCount, ignoredStride);
	if (controlCount == 0u || dispatchThreadId.x >= min(gSmokeControl[0].OccupiedCount, occupiedCapacity))
		return;

	const uint froxelIndex = gSmokeOccupiedFroxelIndices[dispatchThreadId.x];
	if (froxelIndex >= SmokeFroxelCount() || froxelIndex >= mediumCount || froxelIndex >= phaseCount ||
		froxelIndex >= sourceCount || froxelIndex >= currentCount || froxelIndex >= historyCount ||
		!SmokeDirectFroxelIsGrid(gSmokeFroxelPhase[froxelIndex]))
		return;
	const SmokeDirectCacheRecord center = gSmokeDirectCurrent[froxelIndex];
	if (!SmokeDirectRecordValid(center))
		return;
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	const int3 coordinates = (int3)SmokeFroxelCoordinates(froxelIndex);
	const float centerVisibility = SmokeDirectRecordVisibility(center);
	float3 resolved = max(center.Radiance, 0.0);
	float3 neighborhoodMinimum = resolved;
	float3 neighborhoodMaximum = resolved;
	float weight = 1.0;

	if (SmokeDirectReuseMode() >= 2u)
	{
		[unroll]
		for (uint neighbor = 0u; neighbor < 6u; ++neighbor)
		{
			const int3 candidate = coordinates + kSmokeDirectNeighborOffsets[neighbor];
			if (any(candidate < 0) || candidate.x >= (int)gSmokeConstants.FroxelWidth ||
				candidate.y >= (int)gSmokeConstants.FroxelHeight || candidate.z >= (int)gSmokeConstants.FroxelDepth)
			{
				if (diagnostics) InterlockedAdd(gSmokeControl[0].DirectSpatialRejected, 1u);
				continue;
			}
			const uint candidateIndex = SmokeFroxelIndex((uint)candidate.x, (uint)candidate.y, (uint)candidate.z);
			if (candidateIndex >= mediumCount || candidateIndex >= phaseCount || candidateIndex >= currentCount ||
				gSmokeFroxelMedium[candidateIndex].a <= 0.0 ||
				!SmokeDirectFroxelIsGrid(gSmokeFroxelPhase[candidateIndex]))
			{
				if (diagnostics) InterlockedAdd(gSmokeControl[0].DirectSpatialRejected, 1u);
				continue;
			}
			const SmokeDirectCacheRecord candidateRecord = gSmokeDirectCurrent[candidateIndex];
			const float visibilityDifference = abs(SmokeDirectRecordVisibility(candidateRecord) - centerVisibility);
			if (!SmokeDirectRecordsCompatible(center, candidateRecord, (uint3)coordinates) || visibilityDifference > 0.35)
			{
				if (diagnostics) InterlockedAdd(gSmokeControl[0].DirectSpatialRejected, 1u);
				continue;
			}
			const float candidateWeight = 0.125 * saturate(1.0 - visibilityDifference / 0.35);
			const float3 candidateRadiance = max(candidateRecord.Radiance, 0.0);
			resolved += candidateRadiance * candidateWeight;
			weight += candidateWeight;
			neighborhoodMinimum = min(neighborhoodMinimum, candidateRadiance);
			neighborhoodMaximum = max(neighborhoodMaximum, candidateRadiance);
			if (diagnostics) InterlockedAdd(gSmokeControl[0].DirectSpatialAccepted, 1u);
		}
	}

	resolved /= max(weight, 1e-6);
	const float3 unclamped = resolved;
	resolved = clamp(resolved, neighborhoodMinimum, neighborhoodMaximum);
	if (!all(isfinite(resolved)))
	{
		if (diagnostics) InterlockedAdd(gSmokeControl[0].DirectNanRejects, 1u);
		return;
	}
	if (diagnostics && any(abs(unclamped - resolved) > 1e-6))
		InterlockedAdd(gSmokeControl[0].DirectHistoryClamps, 1u);
	SmokeDirectCacheRecord outputRecord = center;
	outputRecord.Radiance = resolved;
	outputRecord.Metadata = SmokeDirectPackMetadata(max(SmokeDirectRecordAge(center), 1u),
		gSmokeConstants.FrameIndex, centerVisibility);
	gSmokeDirectHistory[froxelIndex] = outputRecord;
	gSmokeFroxelSource[froxelIndex].rgb += resolved * gSmokeConstants.RadianceScale;
	if (diagnostics)
	{
		InterlockedAdd(gSmokeControl[0].DirectHistoryResolved, 1u);
		InterlockedMax(gSmokeControl[0].DirectHistoryMaximumAge, SmokeDirectRecordAge(outputRecord));
	}
}
