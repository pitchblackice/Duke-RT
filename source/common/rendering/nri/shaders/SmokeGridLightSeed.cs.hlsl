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

uint SmokeGridLightStableWorldKey(int3 cell)
{
	return SmokeHash(asuint(cell.x) ^ SmokeHash(asuint(cell.y)) ^ SmokeHash(asuint(cell.z)));
}

bool SmokeGridLightClaimNewInvalid()
{
	uint ticket;
	InterlockedAdd(gSmokeGridLightControl[0].RadianceNewInvalidTickets, 1u, ticket);
	if (ticket >= gSmokeGridLightControl[0].RadianceNewInvalidQuantity)
		return false;
	InterlockedAdd(gSmokeGridLightControl[0].RadianceNewInvalidScheduled, 1u);
	return true;
}

bool SmokeGridLightClaimMaintenance()
{
	uint ticket;
	InterlockedAdd(gSmokeGridLightControl[0].RadianceMaintenanceTickets, 1u, ticket);
	if (ticket >= gSmokeGridLightControl[0].RadianceMaintenanceQuantity)
		return false;
	InterlockedAdd(gSmokeGridLightControl[0].RadianceMaintenanceScheduled, 1u);
	return true;
}

bool SmokeGridLightScheduleRadiance(uint cellIndex, int3 cell, SmokeGridBrick brick)
{
	const uint partitionCount = max(gSmokeGridLightControl[0].RadiancePartitionCount, 1u);
	if (partitionCount <= 1u)
		return true;

	const bool targetHistory = (gSmokeConstants.Flags & NRI_SMOKE_GRID_LIGHT_FIELD_PING) != 0u;
	SmokeGridLightRecord prior;
	if (targetHistory)
		prior = gSmokeGridLightCurrent[cellIndex];
	else
		prior = gSmokeGridLightHistory[cellIndex];
	const bool historyValid = SmokeGridLightRecordValid(prior, brick.Generation, gSmokeConstants.SimulationEpoch);
	const uint partition = SmokeGridLightStableWorldKey(cell) % partitionCount;
	const bool partitionDue = partition == (gSmokeConstants.FrameIndex % partitionCount);
	bool scheduled = false;
	if (!historyValid)
	{
		InterlockedAdd(gSmokeGridLightControl[0].RadianceNewInvalidRequested, 1u);
		scheduled = partitionDue && SmokeGridLightClaimNewInvalid();
		if (!scheduled)
			InterlockedAdd(gSmokeGridLightControl[0].RadianceNewInvalidDeferred, 1u);
	}
	else
	{
		InterlockedAdd(gSmokeGridLightControl[0].RadianceMaintenanceRequested, 1u);
		const uint historyAge = SmokeGridLightAge(prior);
		const bool ageDue = historyAge >= gSmokeGridLightControl[0].RadianceMaximumAge;
		scheduled = (partitionDue || ageDue) && SmokeGridLightClaimMaintenance();
		if (!scheduled)
			InterlockedAdd(gSmokeGridLightControl[0].RadianceMaintenanceDeferred, 1u);
	}
	if (scheduled)
		return true;

	SmokeGridLightRecord retained = (SmokeGridLightRecord)0;
	SmokeGridLightRecord retainedShadow = (SmokeGridLightRecord)0;
	if (historyValid)
	{
		retained = prior;
		const uint retainedAge = min(SmokeGridLightAge(prior) + 1u, 65535u);
		SmokeGridLightSetMetadata(retained, brick.Generation, gSmokeConstants.SimulationEpoch,
			SmokeGridLightSampleCount(prior), SmokeGridLightSequence(prior), SmokeGridLightConfidence(prior),
			SmokeGridLightEvidence(prior), SmokeGridLightLastUpdate(prior), retainedAge);
		if (SmokeSelfShadowEnabled(gSmokeConstants.DebugMode))
		{
			if (targetHistory)
				retainedShadow = gSmokeGridLightSelfShadowCurrent[cellIndex];
			else
				retainedShadow = gSmokeGridLightSelfShadowHistory[cellIndex];
		}
		InterlockedAdd(gSmokeGridLightControl[0].RadianceHistoryRetained, 1u);
		InterlockedMax(gSmokeGridLightControl[0].MaximumAge, retainedAge);
		if (retainedAge > gSmokeGridLightControl[0].RadianceMaximumAge)
			InterlockedAdd(gSmokeGridLightControl[0].RadianceAgeOverflows, 1u);
	}
	else
		InterlockedAdd(gSmokeGridLightControl[0].RadianceHistoryMissing, 1u);
	SmokeGridLightWriteTarget(cellIndex, retained, retainedShadow);
	return false;
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
	const uint diagnosticCandidate = SmokeEmissiveDiagnosticCandidate();
	if (diagnosticCandidate != 0xffffffffu)
	{
		candidateIndex = diagnosticCandidate;
		InterlockedAdd(gSmokeGridLightControl[0].Samples, 1u);
		uint headerCount, headerStride, candidateCapacity, candidateStride;
		gSmokeEmissivePrimitiveHeaders.GetDimensions(headerCount, headerStride);
		gSmokeEmissivePrimitives.GetDimensions(candidateCapacity, candidateStride);
		const uint activeCount = headerCount > 0u ?
			min(gSmokeEmissivePrimitiveHeaders[0].activeCount, candidateCapacity) : 0u;
		if (candidateIndex >= activeCount)
		{
			InterlockedAdd(gSmokeGridLightControl[0].Missing, 1u);
			return false;
		}
		candidate = gSmokeEmissivePrimitives[candidateIndex];
		// Diagnostic targeting is a delta distribution over this one bound
		// candidate. K still draws independent points on its primitive/range.
		proposalPdf = 1.0;
		return true;
	}
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

struct SmokeGridLightVisibilityStats
{
	uint Rays;
	uint Visible;
	uint BlockerReceiverImmediate;
	uint BlockerReceiverCell;
	uint BlockerEmitterCell;
	uint BlockerInterior;
};

#define NRI_SMOKE_TARGET_BLOCKER_NONE 0u
#define NRI_SMOKE_TARGET_BLOCKER_EXACT 1u
#define NRI_SMOKE_TARGET_BLOCKER_RANGE 2u
#define NRI_SMOKE_TARGET_BLOCKER_OTHER 3u

uint SmokeGridLightClassifyBlockerIdentity(
	SmokeEmissiveSampleIdentity sampleIdentity,
	SmokeVisibilityBlocker blocker)
{
	if (blocker.Valid == 0u || blocker.DataSource == 0xffffffffu ||
		blocker.DataSource != sampleIdentity.DataSource)
		return NRI_SMOKE_TARGET_BLOCKER_OTHER;
	const bool placed = sampleIdentity.SceneInstanceIndex != 0xffffffffu;
	if (placed && blocker.InstanceId != sampleIdentity.SceneInstanceIndex)
		return NRI_SMOKE_TARGET_BLOCKER_OTHER;
	if (blocker.PrimitiveIndex == sampleIdentity.PrimitiveIndex)
		return NRI_SMOKE_TARGET_BLOCKER_EXACT;
	if (placed && blocker.PrimitiveIndex >= sampleIdentity.RangeBase &&
		blocker.PrimitiveIndex - sampleIdentity.RangeBase < sampleIdentity.RangeCount)
		return NRI_SMOKE_TARGET_BLOCKER_RANGE;
	return NRI_SMOKE_TARGET_BLOCKER_OTHER;
}

void SmokeGridLightClassifyBlocker(
	float blockerDistance,
	float lightDistance,
	float cellSize,
	inout SmokeGridLightVisibilityStats stats)
{
	const float hitFromReceiver = blockerDistance >= 0.0 ? 0.05 + blockerDistance : 0.0;
	const float emitterRemainder = max(lightDistance - hitFromReceiver, 0.0);
	const float cellReach = cellSize * 0.8660254 + 0.05;
	if (blockerDistance >= 0.0 && hitFromReceiver <= 0.1)
		stats.BlockerReceiverImmediate++;
	else if (blockerDistance >= 0.0 && emitterRemainder <= cellReach)
		stats.BlockerEmitterCell++;
	else if (blockerDistance >= 0.0 && hitFromReceiver <= cellReach)
		stats.BlockerReceiverCell++;
	else
		stats.BlockerInterior++;
}

void SmokeGridLightRecordTargetVisibility(
	bool visible,
	SmokeEmissiveSampleIdentity sampleIdentity,
	SmokeVisibilityBlocker blocker)
{
	InterlockedAdd(gSmokeControl[0].EmissiveTargetVisibilityRays, 1u);
	if (visible)
	{
		InterlockedAdd(gSmokeControl[0].EmissiveTargetVisibilityVisible, 1u);
		return;
	}
	const uint relation = SmokeGridLightClassifyBlockerIdentity(sampleIdentity, blocker);
	if (relation == NRI_SMOKE_TARGET_BLOCKER_EXACT)
		InterlockedAdd(gSmokeControl[0].EmissiveTargetBlockerExact, 1u);
	else if (relation == NRI_SMOKE_TARGET_BLOCKER_RANGE)
		InterlockedAdd(gSmokeControl[0].EmissiveTargetBlockerRange, 1u);
	else
	{
		InterlockedAdd(gSmokeControl[0].EmissiveTargetBlockerOther, 1u);
		uint previousClaim;
		InterlockedCompareExchange(gSmokeControl[0].EmissiveTargetWitnessClaim, 0u, 1u, previousClaim);
		if (previousClaim == 0u)
		{
			gSmokeControl[0].EmissiveTargetWitnessCandidate = sampleIdentity.CandidateIndex;
			gSmokeControl[0].EmissiveTargetWitnessRelation = NRI_SMOKE_TARGET_BLOCKER_OTHER;
			gSmokeControl[0].EmissiveTargetWitnessSamplePrimitive = sampleIdentity.PrimitiveIndex;
			gSmokeControl[0].EmissiveTargetWitnessSampleMaterial = sampleIdentity.MaterialIndex;
			gSmokeControl[0].EmissiveTargetWitnessBlockerDataSource = blocker.DataSource;
			gSmokeControl[0].EmissiveTargetWitnessBlockerInstance = blocker.InstanceId;
			gSmokeControl[0].EmissiveTargetWitnessBlockerPrimitive = blocker.PrimitiveIndex;
			gSmokeControl[0].EmissiveTargetWitnessBlockerMaterial = blocker.MaterialIndex;
			gSmokeControl[0].EmissiveTargetWitnessDistanceBits = asuint(blocker.Distance);
		}
	}
}

bool SmokeGridLightEvaluateJointEmissiveRis(
	SmokeGridLightProposal proposal,
	bool localReady,
	float localMix,
	uint setSeed,
	float3 receiverPosition,
	float cellSize,
	uint requestedCandidates,
	bool visibilityDiagnostics,
	bool targetVisibilityDiagnostics,
	out float3 estimator,
	out float3 lightDirection,
	out float distanceToLight,
	out uint pointProposals,
	out uint zeroProposals,
	out uint risRejects,
	out SmokeGridLightVisibilityStats visibilityStats)
{
	estimator = 0.0;
	lightDirection = 0.0;
	distanceToLight = 0.0;
	pointProposals = 0u;
	zeroProposals = 0u;
	risRejects = 0u;
	visibilityStats = (SmokeGridLightVisibilityStats)0;
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
		SmokeEmissiveSampleIdentity sampleIdentity = SmokeEmptyEmissiveSampleIdentity();
		const bool pointIncidentValid = targetVisibilityDiagnostics ?
			SmokeEvaluateWorldEmissiveIncidentWithPrimitive(record, receiverPosition, false,
				pointIncident, pointDirection, pointDistance, sampleIdentity) :
			SmokeEvaluateWorldEmissiveIncident(record, receiverPosition, false,
				pointIncident, pointDirection, pointDistance);
		if (!pointIncidentValid)
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
		if (gSmokeConstants.LightMode >= 2u)
		{
			visibilityStats.Rays++;
			SmokeVisibilityBlocker blocker = SmokeEmptyVisibilityBlocker();
			float blockerDistance = -1.0;
			bool pointVisible;
			if (targetVisibilityDiagnostics)
			{
				pointVisible = SmokeFilteredVisibilityEffective() ?
					SmokeEmissiveVisibleFilteredWithBlocker(receiverPosition, pointDirection, pointDistance, visibilityDiagnostics, blocker) :
					SmokeEmissiveVisibleWithBlocker(receiverPosition, pointDirection, pointDistance, visibilityDiagnostics, blocker);
				blockerDistance = blocker.Distance;
				SmokeGridLightRecordTargetVisibility(pointVisible, sampleIdentity, blocker);
			}
			else
			{
				pointVisible = SmokeFilteredVisibilityEffective() ?
					SmokeEmissiveVisibleFiltered(receiverPosition, pointDirection, pointDistance, visibilityDiagnostics, blockerDistance) :
					SmokeEmissiveVisible(receiverPosition, pointDirection, pointDistance, visibilityDiagnostics, blockerDistance);
			}
			if (!pointVisible)
			{
				zeroProposals++;
				SmokeGridLightClassifyBlocker(blockerDistance, pointDistance, cellSize, visibilityStats);
				continue;
			}
			visibilityStats.Visible++;
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
	if (!SmokeGridLightScheduleRadiance(cellIndex, cell, brick))
		return;
	const float3 receiverPosition = SmokeGridLightCellCenter(cell, cellSize);
	const bool referenceSampling = (gSmokeConstants.Flags & NRI_SMOKE_EMISSIVE_REFERENCE) != 0u;
	const uint sampleCount = referenceSampling ? 32u : 1u;
	const uint pointCandidateCount = SmokeEmissivePointCandidateCount();
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	const bool innerRisDiagnostics = diagnostics && !referenceSampling;
	const bool diagnosticTargetActive = SmokeEmissiveDiagnosticCandidate() != 0xffffffffu;
	bool diagnosticSourceCell = false;
	if (diagnostics)
	{
		const bool fieldB = gSmokeRenderGridControl[0].FieldPing != 0u;
		const float4 scalar = fieldB ? gSmokeRenderGridScalarB[cellIndex] : gSmokeRenderGridScalarA[cellIndex];
		diagnosticSourceCell = scalar.z > 1e-6;
	}
	const bool targetVisibilityDiagnostics = diagnostics && diagnosticTargetActive && diagnosticSourceCell;
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
		SmokeGridLightVisibilityStats innerVisibilityStats = (SmokeGridLightVisibilityStats)0;
		SmokeEmissiveSampleIdentity sampleIdentity = SmokeEmptyEmissiveSampleIdentity();
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
				incidentValid = targetVisibilityDiagnostics ?
					SmokeEvaluateWorldEmissiveIncidentWithPrimitive(record, receiverPosition, false,
						incident, lightDirection, lightDistance, sampleIdentity) :
					SmokeEvaluateWorldEmissiveIncident(record, receiverPosition, false,
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
				receiverPosition, cellSize, pointCandidateCount, innerRisDiagnostics && diagnosticSourceCell,
				targetVisibilityDiagnostics,
				estimator, lightDirection, lightDistance,
				innerPointProposals, innerZeroProposals, innerRisRejects, innerVisibilityStats);
		}
		if (innerRisDiagnostics)
		{
			InterlockedAdd(gSmokeControl[0].EmissiveInnerPointProposals, innerPointProposals);
			InterlockedAdd(gSmokeControl[0].EmissiveInnerZeroProposals, innerZeroProposals);
			InterlockedAdd(gSmokeControl[0].EmissiveInnerRisRejects, innerRisRejects);
			InterlockedAdd(gSmokeControl[0].EmissiveInnerVisibilityRays, innerVisibilityStats.Rays);
			if (diagnosticSourceCell)
			{
				InterlockedAdd(gSmokeControl[0].EmissiveInnerSourceVisibilityRays, innerVisibilityStats.Rays);
				InterlockedAdd(gSmokeControl[0].EmissiveInnerVisibilityVisible, innerVisibilityStats.Visible);
				InterlockedAdd(gSmokeControl[0].EmissiveInnerBlockerReceiverImmediate,
					innerVisibilityStats.BlockerReceiverImmediate);
				InterlockedAdd(gSmokeControl[0].EmissiveInnerBlockerReceiverCell,
					innerVisibilityStats.BlockerReceiverCell);
				InterlockedAdd(gSmokeControl[0].EmissiveInnerBlockerEmitterCell,
					innerVisibilityStats.BlockerEmitterCell);
				InterlockedAdd(gSmokeControl[0].EmissiveInnerBlockerInterior,
					innerVisibilityStats.BlockerInterior);
			}
		}
		bool isVisible = true;
		if (referenceSampling && incidentValid && gSmokeConstants.LightMode >= 2u)
		{
			innerVisibilityStats.Rays++;
			SmokeVisibilityBlocker blocker = SmokeEmptyVisibilityBlocker();
			float blockerDistance = -1.0;
			if (targetVisibilityDiagnostics)
			{
				isVisible = SmokeFilteredVisibilityEffective() ?
					SmokeEmissiveVisibleFilteredWithBlocker(receiverPosition, lightDirection, lightDistance, false, blocker) :
					SmokeEmissiveVisibleWithBlocker(receiverPosition, lightDirection, lightDistance, false, blocker);
				blockerDistance = blocker.Distance;
				SmokeGridLightRecordTargetVisibility(isVisible, sampleIdentity, blocker);
			}
			else
			{
				isVisible = SmokeFilteredVisibilityEffective() ?
					SmokeEmissiveVisibleFiltered(receiverPosition, lightDirection, lightDistance, false, blockerDistance) :
					SmokeEmissiveVisible(receiverPosition, lightDirection, lightDistance, false, blockerDistance);
			}
			if (isVisible)
				innerVisibilityStats.Visible++;
			else
				SmokeGridLightClassifyBlocker(blockerDistance, lightDistance, cellSize, innerVisibilityStats);
		}
		if (!incidentValid)
		{
			physicalZero++;
			continue;
		}
		if (innerRisDiagnostics)
		{
			InterlockedAdd(gSmokeControl[0].EmissiveInnerSelections, 1u);
			if (diagnosticSourceCell)
				InterlockedAdd(gSmokeControl[0].EmissiveInnerSourceSelections, 1u);
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
		if (innerRisDiagnostics && diagnosticSourceCell && visible > 0u)
			InterlockedAdd(gSmokeControl[0].EmissiveInnerSourceOverflow, 1u);
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
