#include "Include/SmokeEmissiveReservoir.hlsli"

SmokeEmissiveReservoirRecord SmokeBuildAnalyticCarrierReservoir(
	SmokeAnalyticCarrier carrier, uint slot, uint mediumHash, bool diagnostics)
{
	SmokeEmissiveReservoirRecord reservoir = SmokeEmptyEmissiveReservoir();
	const uint proposalCount = SmokeEmissiveLaneCount();
	uint selectionState = SmokeEmissiveLaneSeed(carrier.Position, slot, 0u, 0xa13c9e57u);
	[loop]
	for (uint proposal = 0u; proposal < proposalCount; ++proposal)
	{
		uint randomState = SmokeEmissiveLaneSeed(carrier.Position, slot, proposal, 0x63d83595u);
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveSamples, 1u);
		const uint candidateIndex = SmokeSampleEmissivePrimitive(randomState);
		if (candidateIndex == 0xffffffffu)
			continue;
		const EmissivePrimitiveData candidate = gSmokeEmissivePrimitives[candidateIndex];
		SmokeEmissiveReservoirRecord proposalRecord = SmokeEmptyEmissiveReservoir();
		proposalRecord.CandidateIndex = candidateIndex;
		proposalRecord.SampleSeed = randomState;
		proposalRecord.StableKeyLo = candidate.stableKeyLo;
		proposalRecord.StableKeyHi = candidate.stableKeyHi;
		proposalRecord.Generation = gSmokeConstants.CommandCount;
		proposalRecord.Metadata = SmokePackEmissiveMetadata(1u, mediumHash, 0u);
		float3 incident, direction;
		float distanceToLight;
		if (!SmokeEvaluateEmissiveIncident(proposalRecord, carrier.Position, diagnostics,
			incident, direction, distanceToLight))
			continue;
		const float target = SmokeEmissiveLuminance(incident);
		SmokeReservoirMerge(reservoir, proposalRecord, target,
			target / max(candidate.selectionPdf, 1e-6), 1u, mediumHash, 0u, selectionState);
	}
	return reservoir;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint carrierCapacity, carrierStride, currentCapacity, currentStride;
	gSmokeAnalyticCarriers.GetDimensions(carrierCapacity, carrierStride);
	gSmokeAnalyticEmissiveCurrent.GetDimensions(currentCapacity, currentStride);
	const uint carrierIndex = dispatchThreadId.x;
	if (carrierIndex >= min(min(carrierCapacity, NRI_SMOKE_ANALYTIC_MAX_CARRIERS),
		gSmokeConstants.ParticleCapacity))
		return;
	const SmokeAnalyticCarrier carrier = gSmokeAnalyticCarriers[carrierIndex];
	if ((carrier.Flags & NRI_SMOKE_ANALYTIC_CARRIER_ACTIVE) == 0u ||
		carrier.Epoch != gSmokeConstants.SimulationEpoch ||
		carrier.StyleIndex >= gSmokeConstants.StyleCount)
		return;
	const uint slot = SmokeAnalyticCarrierSlot(carrier.Flags);
	const uint generation = SmokeAnalyticCarrierGeneration(carrier.Flags);
	if (slot >= currentCapacity)
		return;
	const SmokeStyle style = gSmokeStyles[carrier.StyleIndex];
	const uint mediumHash = SmokeHash(carrier.StyleIndex ^ 0x4f1bbcdcu) & 0x1fu;
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	SmokeEmissiveReservoirRecord reservoir = SmokeBuildAnalyticCarrierReservoir(
		carrier, slot, mediumHash, diagnostics);

	uint historyCapacity, historyStride;
	gSmokeAnalyticEmissiveHistory.GetDimensions(historyCapacity, historyStride);
	const bool temporalEnabled = SmokeEmissiveReuseMode() >= 1u &&
		(gSmokeConstants.Flags & NRI_SMOKE_EMISSIVE_HISTORY_VALID) != 0u;
	if (temporalEnabled && slot < historyCapacity)
	{
		const SmokeAnalyticEmissiveStorageRecord historyStorage =
			gSmokeAnalyticEmissiveHistory[slot];
		const SmokeEmissiveReservoirRecord history =
			SmokeUnpackAnalyticEmissive(historyStorage);
		if (SmokeAnalyticEmissiveIdentityMatches(historyStorage, slot, generation,
			carrier.Epoch) && SmokeEmissiveRecordValid(history) &&
			SmokeEmissiveIdentityValid(history))
		{
			float3 incident, direction;
			float distanceToLight;
			if (SmokeEvaluateEmissiveIncident(history, carrier.Position, diagnostics,
				incident, direction, distanceToLight))
			{
				const float target = SmokeEmissiveLuminance(incident);
				const uint retainedSamples = min(SmokeEmissiveRecordM(history), 32u);
				const float adjustedWeight = SmokeRetargetedEmissiveWeight(
					history, target, retainedSamples);
				uint selectionState = SmokeEmissiveLaneSeed(carrier.Position, slot, 0u,
					0x9e52d7a1u);
				const uint age = min(SmokeEmissiveRecordAge(history) + 1u, 15u);
				SmokeReservoirMerge(reservoir, history, target, adjustedWeight,
					retainedSamples, mediumHash, age, selectionState);
				if (diagnostics)
				{
					InterlockedAdd(gSmokeControl[0].EmissiveTemporalAccepted, 1u);
					InterlockedMax(gSmokeControl[0].EmissiveMaximumAge, age);
				}
			}
		}
		else if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveTemporalRejected, 1u);
	}
	const float density = max(style.Density * carrier.DensityScale, 0.0) *
		(float)min(carrier.RangeCount, 256u);
	reservoir.ReceiverPosition = carrier.Position;
	reservoir.SigmaT = density * max(style.Extinction, 0.0) * gSmokeConstants.DensityScale;
	gSmokeAnalyticEmissiveCurrent[slot] = SmokePackAnalyticEmissive(
		reservoir, slot, generation, carrier.Epoch);
}
