#include "Include/SmokeEmissiveReservoir.hlsli"

SmokeEmissiveLaneRecord SmokeBuildEmissiveLane(
	float3 receiverPosition,
	uint laneIndex,
	uint proposalCount,
	uint mediumHash,
	bool diagnostics)
{
	SmokeEmissiveReservoirRecord reservoir = SmokeEmptyEmissiveReservoir();
	uint selectionState = SmokeEmissiveLaneSeed(receiverPosition, laneIndex, 0u, 0x7c11a9e3u);
	[loop]
	for (uint proposal = 0u; proposal < proposalCount; ++proposal)
	{
		uint randomState = SmokeEmissiveLaneSeed(receiverPosition, laneIndex, proposal, 0xe6a7c15bu);
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveSamples, 1u);
		const uint candidateIndex = SmokeSampleEmissivePrimitive(randomState);
		if (candidateIndex == 0xffffffffu)
		{
			if (diagnostics)
				InterlockedAdd(gSmokeControl[0].EmissiveCandidateMisses, 1u);
			continue;
		}
		const EmissivePrimitiveData candidate = gSmokeEmissivePrimitives[candidateIndex];
		SmokeEmissiveReservoirRecord proposalRecord = SmokeEmptyEmissiveReservoir();
		proposalRecord.CandidateIndex = candidateIndex;
		proposalRecord.SampleSeed = randomState;
		proposalRecord.StableKeyLo = candidate.stableKeyLo;
		proposalRecord.StableKeyHi = candidate.stableKeyHi;
		proposalRecord.Generation = gSmokeConstants.CommandCount;
		proposalRecord.Metadata = SmokePackEmissiveMetadata(1u, mediumHash, 0u);
		float3 incidentRadiance, lightDirection;
		float distanceToLight;
		if (!SmokeEvaluateEmissiveIncident(proposalRecord, receiverPosition, diagnostics,
			incidentRadiance, lightDirection, distanceToLight))
			continue;
		const float target = SmokeEmissiveLuminance(incidentRadiance);
		const float pdf = max(candidate.selectionPdf, 1e-6);
		SmokeReservoirMerge(reservoir, proposalRecord, target, target / pdf, 1u, mediumHash, 0u, selectionState);
	}

	SmokeEmissiveLaneRecord lane = SmokeEmptyEmissiveLane();
	if (SmokeEmissiveRecordValid(reservoir))
	{
		lane.CandidateIndex = reservoir.CandidateIndex;
		lane.SampleSeed = reservoir.SampleSeed;
		lane.StableKeyLo = reservoir.StableKeyLo;
		lane.StableKeyHi = reservoir.StableKeyHi;
		lane.Target = reservoir.Target;
		lane.WeightSum = reservoir.WeightSum;
	}
	return lane;
}

SmokeEmissiveReservoirRecord SmokeBuildLegacyEmissiveReservoir(
	uint3 froxel,
	float3 receiverPosition,
	float3 viewRay,
	float4 medium,
	float anisotropy,
	uint proposalCount,
	uint mediumHash,
	bool diagnostics)
{
	SmokeEmissiveReservoirRecord reservoir = SmokeEmptyEmissiveReservoir();
	uint selectionState = SmokeLightingRandomSeed(froxel, 0u, 0x7c11a9e3u);
	[loop]
	for (uint proposal = 0u; proposal < proposalCount; ++proposal)
	{
		uint randomState = SmokeLightingRandomSeed(froxel, proposal, 0xe6a7c15bu);
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveSamples, 1u);
		const uint candidateIndex = SmokeSampleEmissivePrimitive(randomState);
		if (candidateIndex == 0xffffffffu)
		{
			if (diagnostics)
				InterlockedAdd(gSmokeControl[0].EmissiveCandidateMisses, 1u);
			continue;
		}
		const EmissivePrimitiveData candidate = gSmokeEmissivePrimitives[candidateIndex];
		SmokeEmissiveReservoirRecord proposalRecord = SmokeEmptyEmissiveReservoir();
		proposalRecord.CandidateIndex = candidateIndex;
		proposalRecord.SampleSeed = randomState;
		proposalRecord.StableKeyLo = candidate.stableKeyLo;
		proposalRecord.StableKeyHi = candidate.stableKeyHi;
		proposalRecord.Generation = gSmokeConstants.CommandCount;
		proposalRecord.ReceiverPosition = receiverPosition;
		proposalRecord.SigmaT = medium.a;
		proposalRecord.Metadata = SmokePackEmissiveMetadata(1u, mediumHash, 0u);
		float3 integrand, lightDirection;
		float distanceToLight;
		if (!SmokeEvaluateEmissiveCandidate(proposalRecord, receiverPosition, viewRay, anisotropy, diagnostics,
			integrand, lightDirection, distanceToLight))
			continue;
		const float target = SmokeEmissiveLuminance(integrand);
		SmokeReservoirMerge(reservoir, proposalRecord, target, target / max(candidate.selectionPdf, 1e-6),
			1u, mediumHash, 0u, selectionState);
	}
	return reservoir;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint controlCount, occupiedCapacity, mediumCount, phaseCount, reservoirCount, ignoredStride;
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, ignoredStride);
	gSmokeFroxelMedium.GetDimensions(mediumCount, ignoredStride);
	gSmokeFroxelPhase.GetDimensions(phaseCount, ignoredStride);
	gSmokeEmissiveCurrent.GetDimensions(reservoirCount, ignoredStride);
	if (controlCount == 0u || dispatchThreadId.x >= min(gSmokeControl[0].OccupiedCount, occupiedCapacity))
		return;
	const uint froxelIndex = gSmokeOccupiedFroxelIndices[dispatchThreadId.x];
	if (froxelIndex >= SmokeFroxelCount() || froxelIndex >= mediumCount || froxelIndex >= phaseCount || froxelIndex >= reservoirCount)
		return;
	const float4 phase = gSmokeFroxelPhase[froxelIndex];
	if (SmokeAnalyticCarrierReservoirOwns(phase))
		return;
	if (SmokeEmissiveWorldFieldOwnsGrid(phase))
		return;
	const float4 medium = SmokeEmissiveReceiverMedium(froxelIndex, phase,
		gSmokeFroxelMedium[froxelIndex]);
	if (medium.a <= 0.0 || !any(medium.rgb > 0.0))
		return;
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	if (gSmokeConstants.LightMode == 0u || (gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_EMISSIVE) == 0u)
		return;
	if (gSmokeConstants.LightMode >= 2u && !SmokeShadowTracingReady())
		return;
	if (diagnostics)
		InterlockedAdd(gSmokeControl[0].EmissiveFroxelsProcessed, 1u);

	const uint3 froxel = SmokeFroxelCoordinates(froxelIndex);
	const float3 ray = SmokeFroxelRay(froxel.xy);
	const float3 viewRay = normalize(ray);
	const float3 receiverPosition = SmokeFroxelCenter(froxel, ray);
	const float anisotropy = phase.x;
	const uint proposalCount = SmokeEmissiveLaneCount();
	const uint mediumHash = SmokeEmissiveMediumHash(medium, anisotropy);

	if (SmokeEmissiveGridFroxel(phase))
	{
		const uint laneCount = SmokeEmissiveLaneCount();
		SmokeEmissiveLaneRecord lanes[4];
		[unroll]
		for (uint laneIndex = 0u; laneIndex < 4u; ++laneIndex)
		{
			lanes[laneIndex] = SmokeEmptyEmissiveLane();
			if (laneIndex < laneCount)
				lanes[laneIndex] = SmokeBuildEmissiveLane(receiverPosition, laneIndex, proposalCount, mediumHash, diagnostics);
		}
		gSmokeEmissiveCurrent[froxelIndex] = SmokePackEmissiveLanePair(lanes[0], lanes[1]);
		gSmokeEmissiveTemporal[froxelIndex] = SmokePackEmissiveLanePair(lanes[2], lanes[3]);
		if (diagnostics)
		{
			[unroll]
			for (uint laneIndex = 0u; laneIndex < 4u; ++laneIndex)
			{
				if (laneIndex >= laneCount)
					continue;
				if (SmokeEmissiveLaneValid(lanes[laneIndex]))
					InterlockedAdd(gSmokeControl[0].EmissiveReservoirInitial, 1u);
				else
					InterlockedAdd(gSmokeControl[0].EmissiveReservoirInvalid, 1u);
			}
		}
		return;
	}

	SmokeEmissiveReservoirRecord reservoir = SmokeBuildLegacyEmissiveReservoir(
		froxel, receiverPosition, viewRay, medium, anisotropy, proposalCount, mediumHash, diagnostics);
	if (SmokeEmissiveRecordValid(reservoir))
	{
		reservoir.ReceiverPosition = receiverPosition;
		reservoir.SigmaT = medium.a;
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveReservoirInitial, 1u);
	}
	else if (diagnostics)
		InterlockedAdd(gSmokeControl[0].EmissiveReservoirInvalid, 1u);
	gSmokeEmissiveCurrent[froxelIndex] = SmokePackEmissiveReservoir(reservoir);
}
