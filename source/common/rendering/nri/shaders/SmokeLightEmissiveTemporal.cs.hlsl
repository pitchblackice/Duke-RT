#include "Include/SmokeEmissiveReservoir.hlsli"

bool SmokeEvaluateEmissiveLaneAtReceiver(
	SmokeEmissiveLaneRecord lane,
	float3 receiverPosition,
	uint proposalCount,
	bool diagnostics,
	out float3 incident,
	out float3 direction)
{
	incident = 0.0;
	direction = 0.0;
	if (!SmokeEmissiveLaneValid(lane))
		return false;
	SmokeEmissiveReservoirRecord record = SmokeEmptyEmissiveReservoir();
	record.CandidateIndex = lane.CandidateIndex;
	record.SampleSeed = lane.SampleSeed;
	record.StableKeyLo = lane.StableKeyLo;
	record.StableKeyHi = lane.StableKeyHi;
	record.Generation = gSmokeConstants.CommandCount;
	float distanceToLight;
	if (!SmokeEvaluateEmissiveIncident(record, receiverPosition, diagnostics, incident, direction, distanceToLight))
		return false;
	const float normalization = lane.WeightSum / max((float)proposalCount * lane.Target, 1e-8);
	if (!isfinite(normalization) || normalization <= 0.0)
		return false;
	if (gSmokeConstants.LightMode >= 2u)
	{
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveShadowRays, 1u);
		const bool visible = SmokeFilteredVisibilityEffective()
			? SmokeEmissiveVisibleFiltered(receiverPosition, direction, distanceToLight, diagnostics)
			: SmokeEmissiveVisible(receiverPosition, direction, distanceToLight, diagnostics);
		if (diagnostics)
		{
			if (visible)
				InterlockedAdd(gSmokeControl[0].EmissiveShadowVisible, 1u);
			else
				InterlockedAdd(gSmokeControl[0].EmissiveShadowOccluded, 1u);
		}
		if (!visible)
		{
			incident = 0.0;
			return true;
		}
	}
	incident *= normalization;
	if (!all(isfinite(incident)))
	{
		incident = 0.0;
		return false;
	}
	return true;
}

bool SmokeEmissiveMomentsSignalCompatible(
	SmokeEmissiveMomentRecord current,
	SmokeEmissiveMomentRecord history)
{
	const float3 currentSigma = sqrt(max(current.SecondMoment - current.MeanRadiance * current.MeanRadiance, 0.0));
	const float3 historySigma = sqrt(max(history.SecondMoment - history.MeanRadiance * history.MeanRadiance, 0.0));
	const float tolerance = 0.025 + 3.0 * max(SmokeEmissiveLuminance(currentSigma), SmokeEmissiveLuminance(historySigma));
	const float currentLuminance = SmokeEmissiveLuminance(current.MeanRadiance);
	const float historyLuminance = SmokeEmissiveLuminance(history.MeanRadiance);
	if (abs(currentLuminance - historyLuminance) > tolerance + max(currentLuminance, historyLuminance) * 0.75)
		return false;
	const float3 currentChromaticity = current.MeanRadiance / max(currentLuminance, 1e-5);
	const float3 historyChromaticity = history.MeanRadiance / max(historyLuminance, 1e-5);
	return distance(currentChromaticity, historyChromaticity) <= 1.5;
}

SmokeEmissiveMomentRecord SmokeEmptyEmissiveMoment()
{
	SmokeEmissiveMomentRecord moment = (SmokeEmissiveMomentRecord)0;
	return moment;
}

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
	const float4 phase = gSmokeFroxelPhase[froxelIndex];
	if (SmokeEmissiveWorldFieldOwnsGrid(phase))
		return;
	const float anisotropy = phase.x;
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;

	if (!SmokeEmissiveGridFroxel(phase))
	{
		SmokeEmissiveReservoirRecord reservoir = SmokeUnpackEmissiveReservoir(gSmokeEmissiveCurrent[froxelIndex]);
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
				const SmokeEmissiveReservoirRecord history = SmokeUnpackEmissiveReservoir(gSmokeEmissiveHistory[previousIndex]);
				if (SmokeEmissiveReservoirCompatible(history, medium, anisotropy, gSmokeConstants.FrameIndex - 1u,
					receiverPosition, SmokeIndirectWorldTolerance(froxel)))
				{
					float3 integrand, lightDirection;
					float distanceToLight;
					if (SmokeEvaluateEmissiveCandidate(history, receiverPosition, normalize(ray), anisotropy, diagnostics,
						integrand, lightDirection, distanceToLight))
					{
						const float target = SmokeEmissiveLuminance(integrand);
						const uint retainedSamples = min(SmokeEmissiveRecordM(history), 32u);
						const float adjustedWeight = SmokeRetargetedEmissiveWeight(history, target, retainedSamples);
						uint selectionState = SmokeLightingRandomSeed(froxel, 0u, 0x7d449b1fu);
						const uint age = min(SmokeEmissiveRecordAge(history) + 1u, 15u);
						SmokeReservoirMerge(reservoir, history, target, adjustedWeight, retainedSamples,
							SmokeEmissiveMediumHash(medium, anisotropy), age, selectionState);
						accepted = true;
						if (diagnostics)
							InterlockedMax(gSmokeControl[0].EmissiveMaximumAge, age);
					}
				}
			}
		}
		const uint3 froxel = SmokeFroxelCoordinates(froxelIndex);
		reservoir.ReceiverPosition = SmokeFroxelCenter(froxel, SmokeFroxelRay(froxel.xy));
		reservoir.SigmaT = medium.a;
		if (diagnostics && temporalEnabled)
		{
			if (accepted)
				InterlockedAdd(gSmokeControl[0].EmissiveTemporalAccepted, 1u);
			else
				InterlockedAdd(gSmokeControl[0].EmissiveTemporalRejected, 1u);
		}
		gSmokeEmissiveTemporal[froxelIndex] = SmokePackEmissiveReservoir(reservoir);
		return;
	}

	const uint3 froxel = SmokeFroxelCoordinates(froxelIndex);
	const float3 ray = SmokeFroxelRay(froxel.xy);
	const float3 receiverPosition = SmokeFroxelCenter(froxel, ray);
	const uint requestedLaneCount = SmokeEmissiveLaneCount();
	const uint proposalCount = requestedLaneCount;
	const SmokeEmissiveStorageRecord firstPair = gSmokeEmissiveCurrent[froxelIndex];
	const SmokeEmissiveStorageRecord secondPair = gSmokeEmissiveTemporal[froxelIndex];
	float3 incidentSum = 0.0;
	float3 secondMomentSum = 0.0;
	float3 directionSum = 0.0;
	uint evaluatedLaneCount = 0u;
	[unroll]
	for (uint laneIndex = 0u; laneIndex < 4u; ++laneIndex)
	{
		if (laneIndex >= requestedLaneCount)
			continue;
		SmokeEmissiveLaneRecord lane = SmokeEmptyEmissiveLane();
		if (laneIndex < 2u)
			lane = SmokeUnpackEmissiveLane(firstPair, laneIndex & 1u);
		else
			lane = SmokeUnpackEmissiveLane(secondPair, laneIndex & 1u);
		float3 laneIncident, laneDirection;
		if (!SmokeEvaluateEmissiveLaneAtReceiver(lane, receiverPosition, proposalCount, diagnostics,
			laneIncident, laneDirection))
			continue;
		++evaluatedLaneCount;
		incidentSum += laneIncident;
		secondMomentSum += laneIncident * laneIncident;
		directionSum += laneDirection * SmokeEmissiveLuminance(laneIncident);
	}

	SmokeEmissiveMomentRecord current = SmokeEmptyEmissiveMoment();
	current.MeanRadiance = incidentSum / (float)requestedLaneCount;
	current.SecondMoment = secondMomentSum / (float)requestedLaneCount;
	current.ReceiverPosition = receiverPosition;
	current.SigmaT = medium.a;
	current.Direction = SmokePackEmissiveDirection(directionSum);
	current.Metadata = SmokePackEmissiveMomentMetadata(
		(float)evaluatedLaneCount / (float)requestedLaneCount,
		SmokeEmissiveMediumHash(medium, anisotropy), 0u, requestedLaneCount);
	const bool currentValid = evaluatedLaneCount > 0u && SmokeEmissiveLuminance(current.MeanRadiance) > 1e-8 &&
		SmokeEmissiveMomentValid(current);
	if (!currentValid)
	{
		gSmokeEmissiveCurrent[froxelIndex] = SmokePackEmissiveMoment(SmokeEmptyEmissiveMoment());
		if (diagnostics && SmokeEmissiveReuseMode() >= 1u)
			InterlockedAdd(gSmokeControl[0].EmissiveTemporalRejected, 1u);
		return;
	}

	SmokeEmissiveMomentRecord resolved = current;
	const bool temporalEnabled = SmokeEmissiveReuseMode() >= 1u &&
		(gSmokeConstants.Flags & NRI_SMOKE_EMISSIVE_HISTORY_VALID) != 0u;
	bool historyValid = false;
	if (temporalEnabled)
	{
		uint previousIndex;
		if (SmokePreviousFroxel(receiverPosition, previousIndex) && previousIndex < historyCount)
		{
			const SmokeEmissiveMomentRecord history = SmokeUnpackEmissiveMoment(gSmokeEmissiveHistory[previousIndex]);
			historyValid = SmokeEmissiveMomentCompatible(history, medium, anisotropy,
				gSmokeConstants.FrameIndex - 1u, receiverPosition, SmokeIndirectWorldTolerance(froxel), requestedLaneCount) &&
				SmokeEmissiveMomentsSignalCompatible(current, history);
			if (historyValid)
			{
				const float historyWeight = min(0.875, 0.5 + 0.025 * (float)SmokeEmissiveMomentAge(history));
				resolved.MeanRadiance = lerp(current.MeanRadiance, history.MeanRadiance, historyWeight);
				resolved.SecondMoment = lerp(current.SecondMoment, history.SecondMoment, historyWeight);
				const float3 currentDirection = SmokeUnpackEmissiveDirection(current.Direction);
				const float3 historyDirection = SmokeUnpackEmissiveDirection(history.Direction);
				resolved.Direction = SmokePackEmissiveDirection(lerp(currentDirection, historyDirection, historyWeight));
				const uint age = min(SmokeEmissiveMomentAge(history) + 1u, 15u);
				resolved.Metadata = SmokePackEmissiveMomentMetadata(
					min(SmokeEmissiveMomentConfidence(current), SmokeEmissiveMomentConfidence(history)),
					SmokeEmissiveMediumHash(medium, anisotropy), age, requestedLaneCount);
				if (diagnostics)
					InterlockedMax(gSmokeControl[0].EmissiveMaximumAge, age);
			}
		}
	}
	if (diagnostics && temporalEnabled)
	{
		if (historyValid)
			InterlockedAdd(gSmokeControl[0].EmissiveTemporalAccepted, 1u);
		else
			InterlockedAdd(gSmokeControl[0].EmissiveTemporalRejected, 1u);
	}
	gSmokeEmissiveCurrent[froxelIndex] = SmokePackEmissiveMoment(resolved);
}
