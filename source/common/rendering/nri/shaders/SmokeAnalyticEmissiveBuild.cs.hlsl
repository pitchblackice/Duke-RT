#include "Include/SmokeEmissiveReservoir.hlsli"

static const float3 NRI_SMOKE_ANALYTIC_LIGHT_LOBE_AXES[6] = {
	float3(1.0, 0.0, 0.0), float3(-1.0, 0.0, 0.0),
	float3(0.0, 1.0, 0.0), float3(0.0, -1.0, 0.0),
	float3(0.0, 0.0, 1.0), float3(0.0, 0.0, -1.0)
};

float3 SmokeAnalyticCarrierSupportExtent(SmokeAnalyticCarrier carrier)
{
	return carrier.Shape == NRI_SMOKE_INJECTION_SHAPE_RECTANGLE
		? abs(carrier.HalfAxisU) + abs(carrier.HalfAxisV) + carrier.Radius
		: carrier.Radius.xxx;
}

float3 SmokeAnalyticLightAnchor(uint anchorIndex, float3 lower, float3 upper)
{
	const float3 center = (lower + upper) * 0.5;
	const float3 extent = (upper - lower) * 0.28867513;
	if (anchorIndex == 0u) return center + extent * float3(1.0, 1.0, 1.0);
	if (anchorIndex == 1u) return center + extent * float3(-1.0, -1.0, 1.0);
	if (anchorIndex == 2u) return center + extent * float3(-1.0, 1.0, -1.0);
	return center + extent * float3(1.0, -1.0, -1.0);
}

bool SmokeStoreAnalyticLightAnchor(uint groupSlot, uint groupGeneration, uint epoch,
	uint anchorIndex, uint sampleCount, float3 anchorPosition, float3 lobes[6])
{
	SmokeAnalyticEmissiveStorageRecord record = (SmokeAnalyticEmissiveStorageRecord)0;
	[unroll]
	for (uint lobe = 0u; lobe < 6u; ++lobe)
		SmokeAnalyticLightStoreLobe(record, lobe, lobes[lobe]);
	record.Data2.yzw = asuint(anchorPosition);
	record.Data3 = uint4(groupSlot, groupGeneration, epoch,
		NRI_SMOKE_ANALYTIC_LIGHT_RECORD_VALID | ((anchorIndex & 3u) << 8u) |
		min(sampleCount, 255u));
	const uint bankIndex = groupSlot * NRI_SMOKE_ANALYTIC_LIGHT_ANCHORS_PER_BANK +
		(anchorIndex % NRI_SMOKE_ANALYTIC_LIGHT_ANCHORS_PER_BANK);
	uint capacity, stride;
	if (anchorIndex < NRI_SMOKE_ANALYTIC_LIGHT_ANCHORS_PER_BANK)
	{
		gSmokeAnalyticEmissiveCurrent.GetDimensions(capacity, stride);
		if (bankIndex >= capacity) return false;
		gSmokeAnalyticEmissiveCurrent[bankIndex] = record;
	}
	else
	{
		gSmokeAnalyticEmissiveHistory.GetDimensions(capacity, stride);
		if (bankIndex >= capacity) return false;
		gSmokeAnalyticEmissiveHistory[bankIndex] = record;
	}
	return true;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint carrierCapacity, carrierStride;
	gSmokeAnalyticCarriers.GetDimensions(carrierCapacity, carrierStride);
	const uint carrierIndex = dispatchThreadId.x;
	const uint activeCapacity = min(min(carrierCapacity, NRI_SMOKE_ANALYTIC_MAX_CARRIERS),
		gSmokeConstants.ParticleCapacity);
	if (carrierIndex >= activeCapacity)
		return;
	const SmokeAnalyticCarrier owner = gSmokeAnalyticCarriers[carrierIndex];
	if ((owner.Flags & NRI_SMOKE_ANALYTIC_CARRIER_ACTIVE) == 0u ||
		owner.Epoch != gSmokeConstants.SimulationEpoch ||
		(owner.LightSampleCountAndFlags & NRI_SMOKE_ANALYTIC_LIGHT_GROUP_OWNER) == 0u ||
		(owner.LightSampleCountAndFlags & NRI_SMOKE_ANALYTIC_LIGHT_BUILD_PENDING) == 0u ||
		owner.LightAnchorCount == 0u)
		return;

	float3 supportLower = float3(3.402823466e+38, 3.402823466e+38, 3.402823466e+38);
	float3 supportUpper = -supportLower;
	[loop]
	for (uint index = 0u; index < activeCapacity; ++index)
	{
		const SmokeAnalyticCarrier carrier = gSmokeAnalyticCarriers[index];
		if ((carrier.Flags & NRI_SMOKE_ANALYTIC_CARRIER_ACTIVE) == 0u ||
			carrier.Epoch != owner.Epoch || carrier.LightGroupSlot != owner.LightGroupSlot ||
			carrier.LightGroupGeneration != owner.LightGroupGeneration)
			continue;
		const float3 extent = SmokeAnalyticCarrierSupportExtent(carrier);
		supportLower = min(supportLower, carrier.Position - extent);
		supportUpper = max(supportUpper, carrier.Position + extent);
	}
	if (!all(isfinite(supportLower)) || !all(isfinite(supportUpper)))
		return;

	const uint anchorCount = min(owner.LightAnchorCount, 4u);
	const uint sampleCount = clamp(owner.LightSampleCountAndFlags & 0xffu, 1u, 4u);
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	if (diagnostics)
	{
		InterlockedAdd(gSmokeControl[0].AnalyticLightBuildEvents, 1u);
		InterlockedAdd(gSmokeControl[0].AnalyticLightSamplesRequested,
			anchorCount * sampleCount);
	}
	[loop]
	for (uint anchorIndex = 0u; anchorIndex < anchorCount; ++anchorIndex)
	{
		if (diagnostics) InterlockedAdd(gSmokeControl[0].AnalyticLightAnchorsBuilt, 1u);
		const float3 anchorPosition = SmokeAnalyticLightAnchor(anchorIndex,
			supportLower, supportUpper);
		float3 lobes[6];
		[unroll] for (uint lobe = 0u; lobe < 6u; ++lobe) lobes[lobe] = 0.0;
		[loop]
		for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
		{
			if (diagnostics) InterlockedAdd(gSmokeControl[0].AnalyticLightSamplesExecuted, 1u);
			uint randomState = SmokeEmissiveLaneSeed(anchorPosition, owner.LightGroupSlot,
				anchorIndex * 4u + sampleIndex, owner.LightGroupGeneration ^ 0x6b50d76bu);
			if (diagnostics) InterlockedAdd(gSmokeControl[0].EmissiveSamples, 1u);
			const uint candidateIndex = SmokeSampleEmissivePrimitive(randomState);
			if (candidateIndex == 0xffffffffu) continue;
			const EmissivePrimitiveData candidate = gSmokeEmissivePrimitives[candidateIndex];
			SmokeEmissiveReservoirRecord proposal = SmokeEmptyEmissiveReservoir();
			proposal.CandidateIndex = candidateIndex;
			proposal.SampleSeed = randomState;
			proposal.StableKeyLo = candidate.stableKeyLo;
			proposal.StableKeyHi = candidate.stableKeyHi;
			proposal.Generation = gSmokeConstants.CommandCount;
			float3 incident, lightDirection;
			float lightDistance;
			if (!SmokeEvaluateEmissiveIncident(proposal, anchorPosition, diagnostics,
				incident, lightDirection, lightDistance))
				continue;
			if (diagnostics) InterlockedAdd(gSmokeControl[0].AnalyticLightEvaluations, 1u);
			bool visible = true;
			if (gSmokeConstants.LightMode >= 2u)
			{
				if (diagnostics) InterlockedAdd(gSmokeControl[0].EmissiveShadowRays, 1u);
				if (diagnostics) InterlockedAdd(gSmokeControl[0].AnalyticLightBuildVisibilityRays, 1u);
				visible = SmokeFilteredVisibilityEffective()
					? SmokeEmissiveVisibleFiltered(anchorPosition, lightDirection, lightDistance, diagnostics)
					: SmokeEmissiveVisible(anchorPosition, lightDirection, lightDistance, diagnostics);
			}
			if (!visible) continue;
			const float3 estimator = incident / max(candidate.selectionPdf, 1e-6);
			float weights[6];
			float weightSum = 0.0;
			[unroll]
			for (uint lobe = 0u; lobe < 6u; ++lobe)
			{
				weights[lobe] = max(dot(lightDirection,
					NRI_SMOKE_ANALYTIC_LIGHT_LOBE_AXES[lobe]), 0.0);
				weightSum += weights[lobe];
			}
			[unroll]
			for (uint lobe = 0u; lobe < 6u; ++lobe)
				lobes[lobe] += estimator * (weights[lobe] /
					max(weightSum * (float)sampleCount, 1e-6));
		}
		const bool stored = SmokeStoreAnalyticLightAnchor(owner.LightGroupSlot,
			owner.LightGroupGeneration, owner.Epoch, anchorIndex, sampleCount,
			anchorPosition, lobes);
		if (diagnostics)
		{
			if (stored) InterlockedAdd(gSmokeControl[0].AnalyticLightAnchorsValid, 1u);
			else InterlockedAdd(gSmokeControl[0].AnalyticLightAnchorsInvalid, 1u);
		}
	}
}
