#include "Include/SmokeEmissiveReservoir.hlsli"

void SmokeAccumulateReference(
	uint froxelIndex,
	uint3 froxel,
	float4 medium,
	float anisotropy,
	float3 receiverPosition,
	float3 viewRay,
	bool diagnostics)
{
	const uint sampleCount = 32u;
	float3 estimate = 0.0;
	[loop]
	for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
	{
		uint randomState = SmokeStableEmissiveReferenceSeed(froxel, sampleIndex);
		const uint candidateIndex = SmokeSampleEmissivePrimitive(randomState);
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveReferenceSamples, 1u);
		if (candidateIndex == 0xffffffffu)
			continue;
		const EmissivePrimitiveData candidate = gSmokeEmissivePrimitives[candidateIndex];
		SmokeEmissiveReservoirRecord record = SmokeEmptyEmissiveReservoir();
		record.CandidateIndex = candidateIndex;
		record.SampleSeed = randomState;
		record.StableKeyLo = candidate.stableKeyLo;
		record.StableKeyHi = candidate.stableKeyHi;
		record.Generation = gSmokeConstants.CommandCount;
		record.ReceiverPosition = receiverPosition;
		record.SigmaT = medium.a;
		record.Metadata = SmokePackEmissiveMetadata(1u, SmokeEmissiveMediumHash(medium, anisotropy), 0u);
		float3 integrand, lightDirection;
		float distanceToLight;
		if (!SmokeEvaluateEmissiveCandidate(record, receiverPosition, viewRay, anisotropy, diagnostics,
			integrand, lightDirection, distanceToLight))
			continue;
		float visibility = 1.0;
		if (gSmokeConstants.LightMode >= 2u)
		{
			if (diagnostics)
			{
				InterlockedAdd(gSmokeControl[0].EmissiveReferenceRays, 1u);
				InterlockedAdd(gSmokeControl[0].EmissiveShadowRays, 1u);
			}
			visibility = (SmokeFilteredVisibilityEffective()
				? SmokePointLightVisibleFiltered(receiverPosition, lightDirection, distanceToLight, diagnostics)
				: SmokePointLightVisible(receiverPosition, lightDirection, distanceToLight, diagnostics)) ? 1.0 : 0.0;
		}
		estimate += integrand * visibility / max(candidate.selectionPdf, 1e-6);
	}
	estimate /= (float)sampleCount;
	const float3 sourceContribution = medium.rgb * estimate * gSmokeConstants.RadianceScale;
	gSmokeFroxelSource[froxelIndex] = float4(gSmokeFroxelSource[froxelIndex].rgb + sourceContribution, 0.0);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint controlCount, occupiedCapacity, mediumCount, phaseCount, sourceCount, temporalCount, historyCount, ignoredStride;
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, ignoredStride);
	gSmokeFroxelMedium.GetDimensions(mediumCount, ignoredStride);
	gSmokeFroxelPhase.GetDimensions(phaseCount, ignoredStride);
	gSmokeFroxelSource.GetDimensions(sourceCount, ignoredStride);
	gSmokeEmissiveTemporal.GetDimensions(temporalCount, ignoredStride);
	gSmokeEmissiveHistory.GetDimensions(historyCount, ignoredStride);
	if (controlCount == 0u || dispatchThreadId.x >= min(gSmokeControl[0].OccupiedCount, occupiedCapacity))
		return;
	const uint froxelIndex = gSmokeOccupiedFroxelIndices[dispatchThreadId.x];
	if (froxelIndex >= SmokeFroxelCount() || froxelIndex >= mediumCount || froxelIndex >= phaseCount ||
		froxelIndex >= sourceCount || froxelIndex >= temporalCount || froxelIndex >= historyCount)
		return;
	const float4 medium = gSmokeFroxelMedium[froxelIndex];
	if (medium.a <= 0.0 || !any(medium.rgb > 0.0))
		return;
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	const uint3 froxel = SmokeFroxelCoordinates(froxelIndex);
	const float3 ray = SmokeFroxelRay(froxel.xy);
	const float3 viewRay = normalize(ray);
	const float3 receiverPosition = SmokeFroxelCenter(froxel, ray);
	const float anisotropy = gSmokeFroxelPhase[froxelIndex].x;
	if ((gSmokeConstants.Flags & NRI_SMOKE_EMISSIVE_REFERENCE) != 0u)
	{
		SmokeAccumulateReference(froxelIndex, froxel, medium, anisotropy, receiverPosition, viewRay, diagnostics);
		gSmokeEmissiveHistory[froxelIndex] = gSmokeEmissiveTemporal[froxelIndex];
		return;
	}

	SmokeEmissiveReservoirRecord reservoir = gSmokeEmissiveTemporal[froxelIndex];
	const uint reuseMode = SmokeEmissiveReuseMode();
	if (reuseMode >= 2u && SmokeEmissiveRecordValid(reservoir))
	{
		static const int3 offsets[6] = {
			int3(1, 0, 0), int3(-1, 0, 0), int3(0, 1, 0),
			int3(0, -1, 0), int3(0, 0, 1), int3(0, 0, -1)
		};
		const uint neighborCount = 2u + 2u * min((gSmokeConstants.Flags >> 5u) & 3u, 2u);
		uint randomState = SmokeLightingRandomSeed(froxel, 0u, 0x7e1c83a5u);
		[loop]
		for (uint i = 0u; i < neighborCount; ++i)
		{
			const int3 neighbor = int3(froxel) + offsets[(i + (gSmokeConstants.FrameIndex & 1u) * 3u) % 6u];
			if (any(neighbor < 0) || any(neighbor >= int3(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight, gSmokeConstants.FroxelDepth)))
				continue;
			const uint neighborIndex = SmokeFroxelIndex((uint)neighbor.x, (uint)neighbor.y, (uint)neighbor.z);
			const SmokeEmissiveReservoirRecord candidate = gSmokeEmissiveTemporal[neighborIndex];
			if (!SmokeEmissiveReservoirCompatible(candidate, medium, anisotropy, gSmokeConstants.FrameIndex,
				receiverPosition, SmokeIndirectWorldTolerance(froxel) * 2.0))
			{
				if (diagnostics)
					InterlockedAdd(gSmokeControl[0].EmissiveSpatialRejected, 1u);
				continue;
			}
			float3 integrand, lightDirection;
			float distanceToLight;
			if (!SmokeEvaluateEmissiveCandidate(candidate, receiverPosition, viewRay, anisotropy, diagnostics,
				integrand, lightDirection, distanceToLight))
				continue;
			const float target = SmokeEmissiveLuminance(integrand);
			const uint retainedSamples = min(SmokeEmissiveRecordM(candidate), 16u);
			const float adjustedWeight = SmokeRetargetedEmissiveWeight(candidate, target, retainedSamples);
			SmokeReservoirMerge(reservoir, candidate, target, adjustedWeight,
				retainedSamples, SmokeEmissiveMediumHash(medium, anisotropy),
				max(SmokeEmissiveRecordAge(reservoir), SmokeEmissiveRecordAge(candidate)), randomState);
			if (diagnostics)
				InterlockedAdd(gSmokeControl[0].EmissiveSpatialAccepted, 1u);
		}
	}

	if (!SmokeEmissiveRecordValid(reservoir))
	{
		gSmokeEmissiveHistory[froxelIndex] = SmokeEmptyEmissiveReservoir();
		return;
	}
	float3 integrand, lightDirection;
	float distanceToLight;
	if (!SmokeEvaluateEmissiveCandidate(reservoir, receiverPosition, viewRay, anisotropy, diagnostics,
		integrand, lightDirection, distanceToLight))
	{
		gSmokeEmissiveHistory[froxelIndex] = SmokeEmptyEmissiveReservoir();
		return;
	}
	float visibility = 1.0;
	if (gSmokeConstants.LightMode >= 2u)
	{
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveShadowRays, 1u);
		visibility = (SmokeFilteredVisibilityEffective()
			? SmokePointLightVisibleFiltered(receiverPosition, lightDirection, distanceToLight, diagnostics)
			: SmokePointLightVisible(receiverPosition, lightDirection, distanceToLight, diagnostics)) ? 1.0 : 0.0;
		if (diagnostics)
		{
			if (visibility > 0.0)
				InterlockedAdd(gSmokeControl[0].EmissiveShadowVisible, 1u);
			else
				InterlockedAdd(gSmokeControl[0].EmissiveShadowOccluded, 1u);
		}
	}
	const float normalization = reservoir.WeightSum /
		max((float)SmokeEmissiveRecordM(reservoir) * reservoir.Target, 1e-8);
	float3 incidentContribution = integrand * (normalization * visibility * gSmokeConstants.RadianceScale);
	const float unclampedLuminance = SmokeEmissiveLuminance(incidentContribution);
	if (unclampedLuminance > gSmokeConstants.DeltaTime)
	{
		incidentContribution *= gSmokeConstants.DeltaTime / unclampedLuminance;
		if (diagnostics)
		{
			InterlockedAdd(gSmokeControl[0].EmissiveSourceClamps, 1u);
			InterlockedAdd(gSmokeControl[0].EmissiveRemovedEnergy,
				(uint)min((unclampedLuminance - gSmokeConstants.DeltaTime) * 1024.0, 4294967295.0));
		}
	}
	const float3 sourceContribution = medium.rgb * incidentContribution;
	gSmokeFroxelSource[froxelIndex] = float4(gSmokeFroxelSource[froxelIndex].rgb + sourceContribution, 0.0);
	reservoir.Metadata = SmokePackEmissiveMetadata(SmokeEmissiveRecordM(reservoir),
		SmokeEmissiveMediumHash(medium, anisotropy), SmokeEmissiveRecordAge(reservoir));
	reservoir.ReceiverPosition = receiverPosition;
	reservoir.SigmaT = medium.a;
	gSmokeEmissiveHistory[froxelIndex] = reservoir;
	if (diagnostics)
	{
		InterlockedAdd(gSmokeControl[0].EmissiveFinalEvaluations, 1u);
		if (visibility > 0.0)
			InterlockedAdd(gSmokeControl[0].EmissiveContributed, 1u);
	}
}
