#include "Include/SmokeEmissiveReservoir.hlsli"
#include "Include/SmokeGridLightingResources.hlsli"
#include "Include/SmokeGridTransmittance.hlsli"

void SmokeGridLightWriteTarget(uint cellIndex, SmokeGridLightRecord record, SmokeGridLightRecord selfShadowRecord)
{
	if ((gSmokeConstants.Flags & NRI_SMOKE_GRID_LIGHT_FIELD_PING) != 0u)
	{
		gSmokeGridLightHistory[cellIndex] = record;
		if (SmokeSelfShadowEnabled(gSmokeConstants.DebugMode))
			gSmokeGridLightSelfShadowHistory[cellIndex] = selfShadowRecord;
	}
	else
	{
		gSmokeGridLightCurrent[cellIndex] = record;
		if (SmokeSelfShadowEnabled(gSmokeConstants.DebugMode))
			gSmokeGridLightSelfShadowCurrent[cellIndex] = selfShadowRecord;
	}
}

uint SmokeGridLightSeedForCell(int3 cell, uint sampleIndex)
{
	uint seed = SmokeHash(asuint(cell.x) ^ SmokeHash(asuint(cell.y)) ^ SmokeHash(asuint(cell.z)) ^
		SmokeHash(gSmokeConstants.SimulationEpoch) ^ SmokeHash(sampleIndex + 0x9e3779b9u));
	// Mode 2 is deterministic but progressive; mode 3 adds a second scramble.
	if (gSmokeConstants.LightMode >= 2u)
		seed ^= SmokeHash(gSmokeConstants.FrameIndex);
	if (gSmokeConstants.LightMode >= 3u)
		seed ^= SmokeHash(gSmokeConstants.FrameIndex + 0x85ebca6bu);
	return SmokeHash(seed);
}

bool SmokeGridLightProposalValid(SmokeGridLightProposal proposal, SmokeGridBrick brick)
{
	return proposal.Count > 0u && proposal.Count <= NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY &&
		proposal.BrickGeneration == brick.Generation &&
		proposal.SimulationEpoch == gSmokeConstants.SimulationEpoch &&
		proposal.FrameStamp == gSmokeConstants.FrameIndex;
}

float SmokeGridLightLocalPdf(SmokeGridLightProposal proposal, uint candidateIndex)
{
	const uint count = min(proposal.Count, NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY);
	[unroll]
	for (uint slot = 0u; slot < NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY; ++slot)
	{
		if (slot < count && proposal.CandidateIndices[slot] == candidateIndex)
			return rcp((float)count);
	}
	return 0.0;
}

bool SmokeGridLightDrawEmissiveProposal(
	SmokeGridLightProposal proposal,
	bool localReady,
	float localMix,
	inout uint randomState,
	out uint candidateIndex,
	out EmissivePrimitiveData candidate,
	out float proposalPdf)
{
	candidateIndex = 0xffffffffu;
	candidate = (EmissivePrimitiveData)0;
	proposalPdf = 0.0;
	if (localReady && SmokeRandom01(randomState) < localMix)
	{
		const uint localIndex = min((uint)(SmokeRandom01(randomState) * proposal.Count), proposal.Count - 1u);
		candidateIndex = proposal.CandidateIndices[localIndex];
		InterlockedAdd(gSmokeGridLightControl[0].ProposalLocalSamples, 1u);
	}
	else
	{
		candidateIndex = SmokeSampleEmissivePrimitive(randomState);
		InterlockedAdd(gSmokeGridLightControl[0].ProposalGlobalSamples, 1u);
	}
	InterlockedAdd(gSmokeGridLightControl[0].Samples, 1u);
	if (candidateIndex == 0xffffffffu)
	{
		InterlockedAdd(gSmokeGridLightControl[0].Missing, 1u);
		return false;
	}
	uint candidateCapacity, candidateStride;
	gSmokeEmissivePrimitives.GetDimensions(candidateCapacity, candidateStride);
	if (candidateIndex >= candidateCapacity)
	{
		InterlockedAdd(gSmokeGridLightControl[0].StructuralErrors, 1u);
		return false;
	}
	candidate = gSmokeEmissivePrimitives[candidateIndex];
	const float localPdf = localReady ? SmokeGridLightLocalPdf(proposal, candidateIndex) : 0.0;
	proposalPdf = (1.0 - localMix) * candidate.selectionPdf + localMix * localPdf;
	if (!isfinite(proposalPdf) || proposalPdf <= 0.0)
	{
		InterlockedAdd(gSmokeGridLightControl[0].StructuralErrors, 1u);
		return false;
	}
	return true;
}

bool SmokeGridLightEvaluateJointEmissiveRis(
	SmokeGridLightProposal proposal,
	bool localReady,
	float localMix,
	uint setSeed,
	float3 receiverPosition,
	uint requestedCandidates,
	out float3 estimator,
	out float3 lightDirection,
	out float distanceToLight,
	out uint pointProposals,
	out uint zeroProposals,
	out uint risRejects)
{
	estimator = 0.0;
	lightDirection = 0.0;
	distanceToLight = 0.0;
	pointProposals = 0u;
	zeroProposals = 0u;
	risRejects = 0u;
	const uint candidateCount = clamp(requestedCandidates, 1u, 8u);
	float targetSum = 0.0;
	float selectedTarget = 0.0;
	float3 selectedEstimator = 0.0;
	float3 selectedDirection = 0.0;
	float selectedDistance = 0.0;
	uint selectionState = SmokeHash(setSeed ^ 0x68bc21ebu);
	[loop]
	for (uint proposalIndex = 0u; proposalIndex < candidateCount; ++proposalIndex)
	{
		// Proposal zero preserves the exact legacy candidate and point sequence,
		// making K=1 a strict A/B fallback. Later proposals are independent.
		uint randomState = proposalIndex == 0u ?
			setSeed : SmokeHash(setSeed ^ SmokeHash(proposalIndex + 0x02e5be93u));
		uint candidateIndex;
		EmissivePrimitiveData candidate;
		float proposalPdf;
		pointProposals++;
		if (!SmokeGridLightDrawEmissiveProposal(proposal, localReady, localMix, randomState,
			candidateIndex, candidate, proposalPdf))
		{
			zeroProposals++;
			continue;
		}
		SmokeEmissiveReservoirRecord record = SmokeEmptyEmissiveReservoir();
		record.CandidateIndex = candidateIndex;
		record.SampleSeed = randomState;
		record.StableKeyLo = candidate.stableKeyLo;
		record.StableKeyHi = candidate.stableKeyHi;
		record.Generation = gSmokeConstants.CommandCount;
		float3 pointIncident, pointDirection;
		float pointDistance;
		if (!SmokeEvaluateWorldEmissiveIncident(record, receiverPosition, false,
			pointIncident, pointDirection, pointDistance))
		{
			zeroProposals++;
			continue;
		}
		const float3 pointEstimator = pointIncident / proposalPdf;
		const float pointTarget = SmokeEmissiveLuminance(pointEstimator);
		if (!all(isfinite(pointEstimator)) || !isfinite(pointTarget) || pointTarget <= 1e-8)
		{
			risRejects++;
			continue;
		}
		const float nextTargetSum = targetSum + pointTarget;
		if (!isfinite(nextTargetSum))
		{
			risRejects++;
			return false;
		}
		if (SmokeRandom01(selectionState) * nextTargetSum < pointTarget)
		{
			selectedTarget = pointTarget;
			selectedEstimator = pointEstimator;
			selectedDirection = pointDirection;
			selectedDistance = pointDistance;
		}
		targetSum = nextTargetSum;
	}
	if (targetSum <= 1e-8 || selectedTarget <= 1e-8)
		return false;
	// pointEstimator already contains the inverse conditional point density and
	// the selected candidate's exact local/global mixture PDF. This is only the
	// fixed-K RIS correction; physical zero proposals remain in the denominator.
	const float risNormalization = targetSum / ((float)candidateCount * selectedTarget);
	if (!isfinite(risNormalization) || risNormalization <= 0.0)
	{
		risRejects++;
		return false;
	}
	estimator = selectedEstimator * risNormalization;
	lightDirection = selectedDirection;
	distanceToLight = selectedDistance;
	if (!all(isfinite(estimator)))
	{
		risRejects++;
		estimator = 0.0;
		lightDirection = 0.0;
		distanceToLight = 0.0;
		return false;
	}
	return true;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint activeCapacity, ignoredStride;
	gSmokeGridLightActive.GetDimensions(activeCapacity, ignoredStride);
	const uint activeCount = min(gSmokeGridLightControl[0].ActiveCount, activeCapacity);
	if (dispatchThreadId.x >= activeCount)
		return;
	const uint cellIndex = gSmokeGridLightActive[dispatchThreadId.x];
	const uint brickIndex = cellIndex / NRI_SMOKE_GRID_CELLS_PER_BRICK;
	const uint localIndex = cellIndex % NRI_SMOKE_GRID_CELLS_PER_BRICK;
	const uint3 local = uint3(localIndex & 7u, (localIndex >> 3u) & 7u, (localIndex >> 6u) & 7u);
	const SmokeGridBrick brick = gSmokeRenderGridBricks[brickIndex];
	const float cellSize = max(asfloat(gSmokeRenderGridControl[0].CellSizeBits), 0.0001);
	const int3 cell = SmokeGridCellCoordinate(brick.Coordinate, local);
	const float3 receiverPosition = SmokeGridLightCellCenter(cell, cellSize);
	const bool referenceSampling = (gSmokeConstants.Flags & NRI_SMOKE_EMISSIVE_REFERENCE) != 0u;
	const uint sampleCount = referenceSampling ? 32u : 1u;
	const uint pointCandidateCount = SmokeEmissivePointCandidateCount();
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	const bool innerRisDiagnostics = diagnostics && !referenceSampling;
	const bool localRequested = (gSmokeConstants.Flags & NRI_SMOKE_GRID_LIGHT_LOCAL_PROPOSALS) != 0u;
	const SmokeGridLightProposal proposal = gSmokeGridLightProposals[brickIndex];
	const bool localReady = localRequested && SmokeGridLightProposalValid(proposal, brick);
	const float localMix = localReady ? NRI_SMOKE_GRID_LIGHT_LOCAL_MIX : 0.0;
	if (localRequested && !localReady)
		InterlockedAdd(gSmokeGridLightControl[0].ProposalFallbacks, 1u);
	float3 mean[6];
	float3 second[6];
	float3 selfShadowMean[6];
	float3 selfShadowSecond[6];
	[unroll] for (uint lobe = 0u; lobe < 6u; ++lobe)
	{
		mean[lobe] = 0.0;
		second[lobe] = 0.0;
		selfShadowMean[lobe] = 0.0;
		selfShadowSecond[lobe] = 0.0;
	}
	uint physicalZero = 0u;
	uint visible = 0u;
	float transmittanceSum = 0.0;
	uint transmittanceCount = 0u;
	[loop]
	for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
	{
		uint randomState = SmokeGridLightSeedForCell(cell, sampleIndex);
		float3 estimator, lightDirection;
		float lightDistance;
		uint innerPointProposals, innerZeroProposals, innerRisRejects;
		bool incidentValid = false;
		if (referenceSampling)
		{
			// Keep the frozen 32-sample reference independent from the RIS
			// implementation so it remains a useful energy/variance oracle.
			uint candidateIndex;
			EmissivePrimitiveData candidate;
			float proposalPdf;
			innerPointProposals = 1u;
			incidentValid = SmokeGridLightDrawEmissiveProposal(proposal, localReady, localMix, randomState,
				candidateIndex, candidate, proposalPdf);
			if (incidentValid)
			{
				SmokeEmissiveReservoirRecord record = SmokeEmptyEmissiveReservoir();
				record.CandidateIndex = candidateIndex;
				record.SampleSeed = randomState;
				record.StableKeyLo = candidate.stableKeyLo;
				record.StableKeyHi = candidate.stableKeyHi;
				record.Generation = gSmokeConstants.CommandCount;
				float3 incident;
				incidentValid = SmokeEvaluateWorldEmissiveIncident(record, receiverPosition, false,
					incident, lightDirection, lightDistance);
				if (incidentValid)
					estimator = incident / proposalPdf;
			}
			innerZeroProposals = incidentValid ? 0u : 1u;
			innerRisRejects = 0u;
		}
		else
		{
			if (innerRisDiagnostics)
				InterlockedAdd(gSmokeControl[0].EmissiveInnerRisSets, 1u);
			incidentValid = SmokeGridLightEvaluateJointEmissiveRis(proposal, localReady, localMix, randomState,
				receiverPosition, pointCandidateCount, estimator, lightDirection, lightDistance,
				innerPointProposals, innerZeroProposals, innerRisRejects);
		}
		if (!incidentValid)
		{
			if (innerRisDiagnostics)
			{
				InterlockedAdd(gSmokeControl[0].EmissiveInnerPointProposals, innerPointProposals);
				InterlockedAdd(gSmokeControl[0].EmissiveInnerZeroProposals, innerZeroProposals);
				InterlockedAdd(gSmokeControl[0].EmissiveInnerRisRejects, innerRisRejects);
			}
			physicalZero++;
			continue;
		}
		if (innerRisDiagnostics)
		{
			InterlockedAdd(gSmokeControl[0].EmissiveInnerPointProposals, innerPointProposals);
			InterlockedAdd(gSmokeControl[0].EmissiveInnerZeroProposals, innerZeroProposals);
			InterlockedAdd(gSmokeControl[0].EmissiveInnerRisRejects, innerRisRejects);
			InterlockedAdd(gSmokeControl[0].EmissiveInnerSelections, 1u);
		}
		bool isVisible = true;
		float blockerDistance = -1.0;
		if (gSmokeConstants.LightMode >= 2u)
		{
			if (innerRisDiagnostics)
				InterlockedAdd(gSmokeControl[0].EmissiveInnerVisibilityRays, 1u);
			isVisible = SmokeFilteredVisibilityEffective() ?
				SmokeEmissiveVisibleFiltered(receiverPosition, lightDirection, lightDistance, false, blockerDistance) :
				SmokeEmissiveVisible(receiverPosition, lightDirection, lightDistance, false, blockerDistance);
			if (innerRisDiagnostics)
			{
				if (isVisible)
				{
					InterlockedAdd(gSmokeControl[0].EmissiveInnerVisibilityVisible, 1u);
				}
				else
				{
					// World-light receivers live at coarse cell centers rather than on a
					// known exterior surface. Split blockers by where they occur so a
					// support-geometry fix cannot accidentally turn real walls into leaks.
					const float hitFromReceiver = blockerDistance >= 0.0 ? 0.05 + blockerDistance : 0.0;
					const float emitterRemainder = max(lightDistance - hitFromReceiver, 0.0);
					const float cellReach = cellSize * 0.8660254 + 0.05;
					if (blockerDistance >= 0.0 && hitFromReceiver <= 0.1)
						InterlockedAdd(gSmokeControl[0].EmissiveInnerBlockerReceiverImmediate, 1u);
					else if (blockerDistance >= 0.0 && emitterRemainder <= cellReach)
						InterlockedAdd(gSmokeControl[0].EmissiveInnerBlockerEmitterCell, 1u);
					else if (blockerDistance >= 0.0 && hitFromReceiver <= cellReach)
						InterlockedAdd(gSmokeControl[0].EmissiveInnerBlockerReceiverCell, 1u);
					else
						InterlockedAdd(gSmokeControl[0].EmissiveInnerBlockerInterior, 1u);
				}
			}
		}
		float mediumTransmittance = 1.0;
		if (SmokeSelfShadowEnabled(gSmokeConstants.DebugMode))
		{
			uint marchSteps;
			bool truncated;
			mediumTransmittance = SmokeGridMediumTransmittance(receiverPosition, lightDirection, lightDistance,
				marchSteps, truncated);
			InterlockedAdd(gSmokeGridLightControl[0].SelfShadowSamples, 1u);
			InterlockedAdd(gSmokeGridLightControl[0].SelfShadowSteps, marchSteps);
			if (truncated) InterlockedAdd(gSmokeGridLightControl[0].SelfShadowTruncated, 1u);
			if (!isfinite(mediumTransmittance))
			{
				mediumTransmittance = 0.0;
				InterlockedAdd(gSmokeGridLightControl[0].SelfShadowNanRejects, 1u);
			}
			else if (mediumTransmittance <= 1e-5) InterlockedAdd(gSmokeGridLightControl[0].SelfShadowTransmittanceZero, 1u);
			else if (mediumTransmittance >= 1.0 - 1e-5) InterlockedAdd(gSmokeGridLightControl[0].SelfShadowTransmittanceOne, 1u);
			else InterlockedAdd(gSmokeGridLightControl[0].SelfShadowTransmittancePartial, 1u);
			transmittanceSum += saturate(mediumTransmittance);
			transmittanceCount++;
		}
		if (!isVisible)
		{
			physicalZero++;
			continue;
		}
		visible++;
		float weights[6];
		float weightSum = 0.0;
		[unroll]
		for (uint lobe = 0u; lobe < 6u; ++lobe)
		{
			weights[lobe] = max(dot(lightDirection, (float3)NRI_SMOKE_GRID_LIGHT_LOBE_AXES[lobe]), 0.0);
			weightSum += weights[lobe];
		}
		[unroll]
		for (uint lobe = 0u; lobe < 6u; ++lobe)
		{
			const float3 contribution = estimator * (weights[lobe] / max(weightSum, 1e-6));
			mean[lobe] += contribution / (float)sampleCount;
			second[lobe] += contribution * contribution / (float)sampleCount;
			const float3 selfShadowContribution = contribution * mediumTransmittance;
			selfShadowMean[lobe] += selfShadowContribution / (float)sampleCount;
			selfShadowSecond[lobe] += selfShadowContribution * selfShadowContribution / (float)sampleCount;
		}
	}
	SmokeGridLightRecord output = (SmokeGridLightRecord)0;
	SmokeGridLightRecord selfShadowOutput = (SmokeGridLightRecord)0;
	bool finite = true;
	[unroll]
	for (uint lobe = 0u; lobe < 6u; ++lobe)
	{
		finite = finite && all(isfinite(mean[lobe])) && all(isfinite(second[lobe])) &&
			all(mean[lobe] <= 65504.0) && all(second[lobe] <= 65504.0) &&
			all(isfinite(selfShadowMean[lobe])) && all(isfinite(selfShadowSecond[lobe])) &&
			all(selfShadowMean[lobe] <= 65504.0) && all(selfShadowSecond[lobe] <= 65504.0);
	}
	if (!finite)
	{
		InterlockedAdd(gSmokeGridLightControl[0].OverflowRejects, 1u);
		SmokeGridLightWriteTarget(cellIndex, output, selfShadowOutput);
		return;
	}
	[unroll]
	for (uint lobe = 0u; lobe < 6u; ++lobe)
	{
		SmokeGridLightStoreLobe(output, lobe, mean[lobe], second[lobe]);
		SmokeGridLightStoreLobe(selfShadowOutput, lobe, selfShadowMean[lobe], selfShadowSecond[lobe]);
	}
	uint evidence = NRI_SMOKE_GRID_LIGHT_EVIDENCE_SUPPORT | NRI_SMOKE_GRID_LIGHT_EVIDENCE_VALID;
	if (physicalZero > 0u) evidence |= NRI_SMOKE_GRID_LIGHT_EVIDENCE_PHYSICAL_ZERO;
	if (visible > 0u) evidence |= NRI_SMOKE_GRID_LIGHT_EVIDENCE_VISIBLE;
	SmokeGridLightSetMetadata(output, brick.Generation, gSmokeConstants.SimulationEpoch,
		sampleCount, gSmokeConstants.FrameIndex & 0xffu, (float)sampleCount / 64.0, evidence,
		gSmokeConstants.FrameIndex, 0u);
	SmokeGridLightSetMetadata(selfShadowOutput, brick.Generation, gSmokeConstants.SimulationEpoch,
		sampleCount, gSmokeConstants.FrameIndex & 0xffu, (float)sampleCount / 8.0, evidence,
		gSmokeConstants.FrameIndex, 0u);
	const uint blockOffset = SmokeHash(asuint(cell.x) ^ SmokeHash(asuint(cell.y)) ^ SmokeHash(asuint(cell.z))) & 7u;
	SmokeGridLightSetSelfShadowEvidence(selfShadowOutput, (gSmokeConstants.FrameIndex + blockOffset) >> 3u,
		transmittanceCount > 0u ? transmittanceSum / (float)transmittanceCount : 1.0);
	SmokeGridLightWriteTarget(cellIndex, output, selfShadowOutput);
	InterlockedAdd(gSmokeGridLightControl[0].ScheduledCount, 1u);
	if (physicalZero > 0u) InterlockedAdd(gSmokeGridLightControl[0].PhysicalZero, physicalZero);
	if (visible > 0u) InterlockedAdd(gSmokeGridLightControl[0].Visible, visible);
}
