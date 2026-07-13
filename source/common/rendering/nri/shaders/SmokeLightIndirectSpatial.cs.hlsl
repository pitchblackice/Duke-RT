#include "Include/SmokeIndirectCache.hlsli"

static const int3 kSmokeIndirectNeighborOffsets[6] = {
	int3(-1, 0, 0), int3(1, 0, 0), int3(0, -1, 0),
	int3(0, 1, 0), int3(0, 0, -1), int3(0, 0, 1)
};

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint controlCount, occupiedCapacity, mediumCount, phaseCount, sourceCount, scratchCount, historyCount, ignoredStride;
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, ignoredStride);
	gSmokeFroxelMedium.GetDimensions(mediumCount, ignoredStride);
	gSmokeFroxelPhase.GetDimensions(phaseCount, ignoredStride);
	gSmokeFroxelSource.GetDimensions(sourceCount, ignoredStride);
	gSmokeIndirectScratch.GetDimensions(scratchCount, ignoredStride);
	gSmokeIndirectHistory.GetDimensions(historyCount, ignoredStride);
	if (controlCount == 0u || dispatchThreadId.x >= min(gSmokeControl[0].OccupiedCount, occupiedCapacity))
		return;

	const uint froxelIndex = gSmokeOccupiedFroxelIndices[dispatchThreadId.x];
	if (froxelIndex >= SmokeFroxelCount() || froxelIndex >= mediumCount || froxelIndex >= phaseCount ||
		froxelIndex >= sourceCount || froxelIndex >= scratchCount || froxelIndex >= historyCount)
		return;
	const float4 medium = gSmokeFroxelMedium[froxelIndex];
	SmokeIndirectCacheRecord center = gSmokeIndirectScratch[froxelIndex];
	if (medium.a <= 0.0 || !SmokeIndirectRecordValid(center))
		return;
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;

	float3 resolved = max(center.Radiance, 0.0);
	float weight = 1.0;
	if (SmokeIndirectCacheMode() >= 3u)
	{
		const int3 coordinates = (int3)SmokeFroxelCoordinates(froxelIndex);
		[unroll]
		for (uint neighbor = 0u; neighbor < 6u; ++neighbor)
		{
			const int3 candidate = coordinates + kSmokeIndirectNeighborOffsets[neighbor];
			if (any(candidate < 0) || candidate.x >= (int)gSmokeConstants.FroxelWidth ||
				candidate.y >= (int)gSmokeConstants.FroxelHeight || candidate.z >= (int)gSmokeConstants.FroxelDepth)
			{
				if (diagnostics) InterlockedAdd(gSmokeControl[0].IndirectSpatialRejected, 1u);
				continue;
			}
			const uint candidateIndex = SmokeFroxelIndex((uint)candidate.x, (uint)candidate.y, (uint)candidate.z);
			if (candidateIndex >= mediumCount || candidateIndex >= scratchCount ||
				gSmokeFroxelMedium[candidateIndex].a <= 0.0)
			{
				if (diagnostics) InterlockedAdd(gSmokeControl[0].IndirectSpatialRejected, 1u);
				continue;
			}
			const SmokeIndirectCacheRecord candidateRecord = gSmokeIndirectScratch[candidateIndex];
			if (!SmokeIndirectRecordsCompatible(center, candidateRecord) ||
				length(candidateRecord.WorldPosition - center.WorldPosition) > SmokeIndirectWorldTolerance((uint3)coordinates) * 2.0)
			{
				if (diagnostics) InterlockedAdd(gSmokeControl[0].IndirectSpatialRejected, 1u);
				continue;
			}
			resolved += max(candidateRecord.Radiance, 0.0) * 0.25;
			weight += 0.25;
			if (diagnostics) InterlockedAdd(gSmokeControl[0].IndirectSpatialAccepted, 1u);
		}
	}
	resolved /= weight;
	const uint age = SmokeIndirectRecordAge(center);
	SmokeIndirectCacheRecord outputRecord;
	outputRecord.Radiance = resolved;
	outputRecord.SigmaT = medium.a;
	outputRecord.WorldPosition = center.WorldPosition;
	outputRecord.Metadata = SmokePackIndirectMetadata(
		SmokeIndirectRecordSector(center), medium, gSmokeFroxelPhase[froxelIndex].x, age, gSmokeConstants.FrameIndex);
	gSmokeIndirectHistory[froxelIndex] = outputRecord;
	gSmokeFroxelSource[froxelIndex].rgb += medium.rgb * resolved *
		(gSmokeConstants.IndirectScale * gSmokeConstants.RadianceScale);
	if (diagnostics) InterlockedAdd(gSmokeControl[0].IndirectCacheResolved, 1u);
}
