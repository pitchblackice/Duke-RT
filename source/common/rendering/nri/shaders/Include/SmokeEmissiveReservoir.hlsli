#ifndef NRI_SMOKE_EMISSIVE_RESERVOIR_HLSLI
#define NRI_SMOKE_EMISSIVE_RESERVOIR_HLSLI

#include "SmokeResources.hlsli"
#include "SmokeFroxel.hlsli"
#include "SmokeLighting.hlsli"
#include "SmokeIndirectCache.hlsli"

#define NRI_SMOKE_EMISSIVE_HISTORY_VALID 0x100u
#define NRI_SMOKE_EMISSIVE_REUSE_SHIFT 9u
#define NRI_SMOKE_EMISSIVE_REUSE_MASK 3u
#define NRI_SMOKE_EMISSIVE_REFERENCE 0x800u
#define NRI_SMOKE_EMISSIVE_RECORD_VALID 0x80000000u

uint SmokeEmissiveReuseMode()
{
	return (gSmokeConstants.Flags >> NRI_SMOKE_EMISSIVE_REUSE_SHIFT) & NRI_SMOKE_EMISSIVE_REUSE_MASK;
}

uint SmokeStableEmissiveReferenceSeed(uint3 froxel, uint sampleIndex)
{
	uint seed = SmokeHash(froxel.x ^ SmokeHash(froxel.y + 0x6d2b79f5u));
	seed ^= SmokeHash(froxel.z + 0x9e3779b9u);
	seed ^= SmokeHash(gSmokeConstants.SimulationEpoch + 0x85ebca6bu);
	return SmokeHash(seed ^ SmokeHash(sampleIndex + 0x7c7e6f19u));
}

uint SmokeEmissiveMediumHash(float4 medium, float anisotropy)
{
	const uint3 albedo = (uint3)round(saturate(medium.rgb / max(medium.a, 1e-5)) * 15.0);
	const uint g = (uint)round(saturate(anisotropy * 0.5 + 0.5) * 15.0);
	const uint sigma = (uint)round(saturate(medium.a * 0.25) * 15.0);
	return SmokeHash(albedo.x | (albedo.y << 4u) | (albedo.z << 8u) | (g << 12u) | (sigma << 16u)) & 0x1fu;
}

uint SmokeEmissiveRecordM(SmokeEmissiveReservoirRecord record) { return record.Metadata & 0xffu; }
uint SmokeEmissiveRecordMedium(SmokeEmissiveReservoirRecord record) { return (record.Metadata >> 8u) & 0x1fu; }
uint SmokeEmissiveRecordAge(SmokeEmissiveReservoirRecord record) { return (record.Metadata >> 13u) & 0xfu; }
uint SmokeEmissiveRecordFrame(SmokeEmissiveReservoirRecord record) { return (record.Metadata >> 17u) & 0x3fu; }

uint SmokePackEmissiveMetadata(uint representedSamples, uint mediumHash, uint age)
{
	return min(representedSamples, 255u) | ((mediumHash & 0x1fu) << 8u) |
		(min(age, 15u) << 13u) | ((gSmokeConstants.FrameIndex & 0x3fu) << 17u) |
		NRI_SMOKE_EMISSIVE_RECORD_VALID;
}

SmokeEmissiveReservoirRecord SmokeEmptyEmissiveReservoir()
{
	SmokeEmissiveReservoirRecord record = (SmokeEmissiveReservoirRecord)0;
	record.CandidateIndex = 0xffffffffu;
	return record;
}

bool SmokeEmissiveRecordValid(SmokeEmissiveReservoirRecord record)
{
	return (record.Metadata & NRI_SMOKE_EMISSIVE_RECORD_VALID) != 0u && record.CandidateIndex != 0xffffffffu &&
		record.Generation == gSmokeConstants.CommandCount && SmokeEmissiveRecordM(record) > 0u &&
		isfinite(record.Target) && record.Target > 1e-8 && isfinite(record.WeightSum) && record.WeightSum > 0.0;
}

bool SmokeEmissiveIdentityValid(SmokeEmissiveReservoirRecord record)
{
	uint candidateCount, ignoredStride;
	gSmokeEmissivePrimitives.GetDimensions(candidateCount, ignoredStride);
	// Proposal records deliberately have no target or accumulated weight yet;
	// identity validation must be usable before the first target evaluation.
	if (record.CandidateIndex == 0xffffffffu || record.CandidateIndex >= candidateCount ||
		record.Generation != gSmokeConstants.CommandCount)
		return false;
	const EmissivePrimitiveData candidate = gSmokeEmissivePrimitives[record.CandidateIndex];
	return candidate.stableKeyLo == record.StableKeyLo && candidate.stableKeyHi == record.StableKeyHi;
}

float SmokeEmissiveLuminance(float3 value)
{
	return dot(max(value, 0.0), float3(0.2126, 0.7152, 0.0722));
}

bool SmokeEvaluateEmissiveCandidate(
	SmokeEmissiveReservoirRecord record,
	float3 receiverPosition,
	float3 viewRay,
	float anisotropy,
	bool diagnostics,
	out float3 integrand,
	out float3 lightDirection,
	out float distanceToLight)
{
	integrand = 0.0;
	lightDirection = 0.0;
	distanceToLight = 0.0;
	if (!SmokeEmissiveIdentityValid(record))
	{
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveIdentityRejects, 1u);
		return false;
	}
	const EmissivePrimitiveData candidate = gSmokeEmissivePrimitives[record.CandidateIndex];
	uint randomState = record.SampleSeed;
	uint sampledPrimitiveIndex;
	PrimitiveData primitive;
	MaterialData material;
	float2 lightUv;
	float3 lightNormal;
	float3 lightPosition;
	float effectiveArea;
	if (!SmokeSamplePointOnEmissive(candidate, randomState, sampledPrimitiveIndex, primitive, material,
		lightPosition, lightUv, lightNormal, effectiveArea))
	{
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveCandidateMisses, 1u);
		return false;
	}
	float3 lightRadiance = SmokeSampleMaterialEmission(material, lightUv) * max(material.emissiveIntensity, 0.0);
	lightRadiance *= max(candidate.emissionScale, 0.0);
	if (!all(isfinite(lightRadiance)) || !any(lightRadiance > 0.0))
		return false;
	const float3 toLight = lightPosition - receiverPosition;
	const float distanceSquared = dot(toLight, toLight);
	if (distanceSquared <= 0.0001 || !isfinite(distanceSquared))
	{
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveDistanceRejected, 1u);
		return false;
	}
	distanceToLight = sqrt(distanceSquared);
	lightDirection = toLight / distanceToLight;
	const float emitterCosine = max(dot(lightNormal, -lightDirection), 0.0);
	if (emitterCosine <= 0.0)
	{
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveFacingRejected, 1u);
		return false;
	}
	const float projectedArea = max(effectiveArea * emitterCosine, 0.001);
	const float falloffScale = max(material.emissiveMaskScale, 0.25);
	const float attenuatedDistanceSquared = pow(max(distanceSquared, 0.01), falloffScale);
	const float solidAngle = min(projectedArea / max(12.56637061436 * attenuatedDistanceSquared, 0.01), 1.0);
	if (diagnostics && any(lightRadiance > 32.0))
		InterlockedAdd(gSmokeControl[0].EmissiveRadianceClamps, 1u);
	lightRadiance = min(lightRadiance, 32.0);
	integrand = lightRadiance * (SmokeHenyeyGreenstein(dot(lightDirection, viewRay), anisotropy) * solidAngle);
	return all(isfinite(integrand)) && SmokeEmissiveLuminance(integrand) > 1e-8;
}

void SmokeReservoirMerge(
	inout SmokeEmissiveReservoirRecord reservoir,
	SmokeEmissiveReservoirRecord candidate,
	float currentTarget,
	float candidateWeight,
	uint representedSamples,
	uint mediumHash,
	uint age,
	inout uint randomState)
{
	if (!isfinite(candidateWeight) || candidateWeight <= 0.0 || !isfinite(currentTarget) || currentTarget <= 1e-8)
		return;
	const float newWeightSum = reservoir.WeightSum + candidateWeight;
	if (reservoir.CandidateIndex == 0xffffffffu || SmokeRandom01(randomState) * newWeightSum < candidateWeight)
	{
		reservoir.CandidateIndex = candidate.CandidateIndex;
		reservoir.SampleSeed = candidate.SampleSeed;
		reservoir.StableKeyLo = candidate.StableKeyLo;
		reservoir.StableKeyHi = candidate.StableKeyHi;
		reservoir.Target = currentTarget;
		reservoir.Generation = candidate.Generation;
	}
	reservoir.WeightSum = newWeightSum;
	reservoir.Metadata = SmokePackEmissiveMetadata(SmokeEmissiveRecordM(reservoir) + representedSamples, mediumHash, age);
}

float SmokeRetargetedEmissiveWeight(
	SmokeEmissiveReservoirRecord record,
	float currentTarget,
	uint retainedSamples)
{
	const uint sourceSamples = SmokeEmissiveRecordM(record);
	if (sourceSamples == 0u || retainedSamples == 0u || !isfinite(currentTarget) || currentTarget <= 1e-8)
		return 0.0;
	// Reuse deliberately bounds represented samples. Preserve average sample
	// mass when truncating M; importing the full WeightSum with a smaller M
	// makes energy multiply across temporal and spatial passes.
	const float retainedFraction = (float)retainedSamples / (float)sourceSamples;
	return record.WeightSum * currentTarget / max(record.Target, 1e-8) * retainedFraction;
}

bool SmokeEmissiveReservoirCompatible(
	SmokeEmissiveReservoirRecord record,
	float4 medium,
	float anisotropy,
	uint expectedPreviousFrame,
	float3 receiverPosition,
	float worldTolerance)
{
	return SmokeEmissiveRecordValid(record) && SmokeEmissiveIdentityValid(record) &&
		SmokeEmissiveRecordMedium(record) == SmokeEmissiveMediumHash(medium, anisotropy) &&
		SmokeEmissiveRecordFrame(record) == (expectedPreviousFrame & 0x3fu) &&
		all(isfinite(record.ReceiverPosition)) && isfinite(record.SigmaT) &&
		abs(record.SigmaT - medium.a) <= max(max(record.SigmaT, medium.a) * 0.25, 0.002) &&
		distance(record.ReceiverPosition, receiverPosition) <= worldTolerance;
}

#endif
