#include "Include/SmokeEmissiveReservoir.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint controlCount, occupiedCapacity, mediumCount, phaseCount, currentCount, temporalCount, historyCount, ignoredStride;
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, ignoredStride);
	gSmokeFroxelMedium.GetDimensions(mediumCount, ignoredStride);
	gSmokeFroxelPhase.GetDimensions(phaseCount, ignoredStride);
	gSmokeEmissiveCurrent.GetDimensions(currentCount, ignoredStride);
	gSmokeEmissiveTemporal.GetDimensions(temporalCount, ignoredStride);
	gSmokeEmissiveHistory.GetDimensions(historyCount, ignoredStride);
	if (controlCount == 0u || dispatchThreadId.x >= min(gSmokeControl[0].OccupiedCount, occupiedCapacity))
		return;
	const uint froxelIndex = gSmokeOccupiedFroxelIndices[dispatchThreadId.x];
	if (froxelIndex >= SmokeFroxelCount() || froxelIndex >= mediumCount || froxelIndex >= phaseCount ||
		froxelIndex >= currentCount || froxelIndex >= temporalCount)
		return;
	const float4 medium = gSmokeFroxelMedium[froxelIndex];
	const float anisotropy = gSmokeFroxelPhase[froxelIndex].x;
	SmokeEmissiveReservoirRecord reservoir = gSmokeEmissiveCurrent[froxelIndex];
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	const bool temporalEnabled = SmokeEmissiveReuseMode() >= 1u &&
		(gSmokeConstants.Flags & NRI_SMOKE_EMISSIVE_HISTORY_VALID) != 0u;
	bool accepted = false;
	if (temporalEnabled)
	{
		const uint3 froxel = SmokeFroxelCoordinates(froxelIndex);
		const float3 ray = SmokeFroxelRay(froxel.xy);
		const float3 receiverPosition = SmokeFroxelCenter(froxel, ray);
		uint previousIndex;
		if (SmokePreviousFroxel(receiverPosition, previousIndex) && previousIndex < historyCount)
		{
			const SmokeEmissiveReservoirRecord history = gSmokeEmissiveHistory[previousIndex];
			if (SmokeEmissiveReservoirCompatible(history, medium, anisotropy, gSmokeConstants.FrameIndex - 1u,
				receiverPosition, SmokeIndirectWorldTolerance(froxel)))
			{
				float3 integrand, lightDirection;
				float distanceToLight;
				if (SmokeEvaluateEmissiveCandidate(history, receiverPosition, normalize(ray), anisotropy, diagnostics,
					integrand, lightDirection, distanceToLight))
				{
					const float target = SmokeEmissiveLuminance(integrand);
					const float adjustedWeight = history.WeightSum * target / max(history.Target, 1e-8);
					uint selectionState = SmokeLightingRandomSeed(froxel, 0u, 0x7d449b1fu);
					const uint age = min(SmokeEmissiveRecordAge(history) + 1u, 15u);
					SmokeReservoirMerge(reservoir, history, target, adjustedWeight,
						min(SmokeEmissiveRecordM(history), 32u), SmokeEmissiveMediumHash(medium, anisotropy), age, selectionState);
					accepted = true;
					if (diagnostics)
						InterlockedMax(gSmokeControl[0].EmissiveMaximumAge, age);
				}
			}
		}
	}
	reservoir.ReceiverPosition = SmokeFroxelCenter(SmokeFroxelCoordinates(froxelIndex), SmokeFroxelRay(SmokeFroxelCoordinates(froxelIndex).xy));
	reservoir.SigmaT = medium.a;
	if (diagnostics && temporalEnabled)
	{
		if (accepted)
			InterlockedAdd(gSmokeControl[0].EmissiveTemporalAccepted, 1u);
		else
			InterlockedAdd(gSmokeControl[0].EmissiveTemporalRejected, 1u);
	}
	gSmokeEmissiveTemporal[froxelIndex] = reservoir;
}
