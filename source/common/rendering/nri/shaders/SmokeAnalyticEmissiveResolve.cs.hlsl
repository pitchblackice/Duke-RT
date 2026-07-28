#include "Include/SmokeEmissiveReservoir.hlsli"

float SmokeAnalyticEmissiveRectangleKernel(SmokeAnalyticCarrier carrier,
	float3 ray, float nearDepth, float farDepth)
{
	float integral = 0.0;
	[unroll]
	for (uint sampleIndex = 0u; sampleIndex < 4u; ++sampleIndex)
	{
		const float depth = lerp(nearDepth, farDepth, ((float)sampleIndex + 0.5) * 0.25);
		const float3 position = gSmokeConstants.CameraPosition + ray * depth;
		const float3 closest = SmokeInjectionClosestRectanglePoint(position, carrier.Position,
			carrier.HalfAxisU, carrier.HalfAxisV);
		const float normalized = saturate(1.0 - distance(position, closest) /
			max(carrier.Radius, 0.001));
		integral += normalized * normalized * (3.0 - 2.0 * normalized);
	}
	return integral * 0.25;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint controlCount, occupiedCapacity, phaseCapacity, sourceCapacity,
		analyticCapacity, ignoredStride;
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, ignoredStride);
	gSmokeFroxelPhase.GetDimensions(phaseCapacity, ignoredStride);
	gSmokeFroxelSource.GetDimensions(sourceCapacity, ignoredStride);
	gSmokeAnalyticFroxelMedium.GetDimensions(analyticCapacity, ignoredStride);
	if (controlCount == 0u || dispatchThreadId.x >=
		min(gSmokeControl[0].OccupiedCount, occupiedCapacity))
		return;
	const uint froxelIndex = gSmokeOccupiedFroxelIndices[dispatchThreadId.x];
	if (froxelIndex >= min(min(phaseCapacity, sourceCapacity), analyticCapacity))
		return;
	const float4 phase = gSmokeFroxelPhase[froxelIndex];
	if (!SmokeAnalyticCarrierReservoirOwns(phase))
		return;
	const float4 analyticMedium = gSmokeAnalyticFroxelMedium[froxelIndex];
	if (analyticMedium.a <= 0.0 || !any(analyticMedium.rgb > 0.0))
		return;

	const uint3 froxel = SmokeFroxelCoordinates(froxelIndex);
	const float3 ray = SmokeFroxelRay(froxel.xy);
	const float nearDepth = SmokeSliceNearDepth(froxel.z);
	const float farDepth = SmokeSliceFarDepth(froxel.z);
	const uint2 tileCount = SmokeAnalyticTileCount(
		gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight);
	const uint tileIndex = SmokeAnalyticTileIndex(
		froxel.xy / NRI_SMOKE_ANALYTIC_TILE_SIZE, tileCount);
	uint headerCapacity, headerStride;
	gSmokeAnalyticTileHeaders.GetDimensions(headerCapacity, headerStride);
	if (tileIndex >= headerCapacity)
		return;
	const uint candidateCount = min(gSmokeAnalyticTileHeaders[tileIndex].Count,
		NRI_SMOKE_ANALYTIC_MAX_CARRIERS_PER_TILE);
	uint carrierCapacity, carrierStride, indexCapacity, indexStride;
	gSmokeAnalyticCarriers.GetDimensions(carrierCapacity, carrierStride);
	gSmokeAnalyticTileIndices.GetDimensions(indexCapacity, indexStride);
	float bestWeight = 0.0;
	uint bestSlot = 0xffffffffu;
	[loop]
	for (uint candidate = 0u; candidate < candidateCount; ++candidate)
	{
		const uint listIndex = tileIndex * NRI_SMOKE_ANALYTIC_MAX_CARRIERS_PER_TILE + candidate;
		if (listIndex >= indexCapacity)
			break;
		const uint carrierIndex = gSmokeAnalyticTileIndices[listIndex];
		if (carrierIndex >= min(carrierCapacity, gSmokeConstants.ParticleCapacity))
			continue;
		const SmokeAnalyticCarrier carrier = gSmokeAnalyticCarriers[carrierIndex];
		if ((carrier.Flags & NRI_SMOKE_ANALYTIC_CARRIER_ACTIVE) == 0u ||
			carrier.Epoch != gSmokeConstants.SimulationEpoch ||
			carrier.StyleIndex >= gSmokeConstants.StyleCount)
			continue;
		const SmokeStyle style = gSmokeStyles[carrier.StyleIndex];
		const float kernel = carrier.Shape == NRI_SMOKE_INJECTION_SHAPE_RECTANGLE
			? SmokeAnalyticEmissiveRectangleKernel(carrier, ray, nearDepth, farDepth)
			: SmokeSphereSegmentKernelAverage(carrier.Position, carrier.Radius,
				ray, nearDepth, farDepth);
		const float weight = kernel * max(style.Density * carrier.DensityScale, 0.0) *
			(float)min(carrier.RangeCount, 256u);
		if (weight > bestWeight)
		{
			bestWeight = weight;
			bestSlot = SmokeAnalyticCarrierSlot(carrier.Flags);
		}
	}
	uint reservoirCapacity, reservoirStride;
	gSmokeAnalyticEmissiveCurrent.GetDimensions(reservoirCapacity, reservoirStride);
	if (bestSlot == 0xffffffffu || bestSlot >= reservoirCapacity)
		return;
	const SmokeEmissiveReservoirRecord reservoir = SmokeUnpackAnalyticEmissive(
		gSmokeAnalyticEmissiveCurrent[bestSlot]);
	if (!SmokeEmissiveRecordValid(reservoir) || !SmokeEmissiveIdentityValid(reservoir))
		return;
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	const float3 receiverPosition = SmokeFroxelCenter(froxel, ray);
	float3 incident, lightDirection;
	float distanceToLight;
	if (!SmokeEvaluateEmissiveIncident(reservoir, receiverPosition, diagnostics,
		incident, lightDirection, distanceToLight))
		return;
	float visibility = 1.0;
	if (gSmokeConstants.LightMode >= 2u)
	{
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveShadowRays, 1u);
		visibility = (SmokeFilteredVisibilityEffective()
			? SmokeEmissiveVisibleFiltered(receiverPosition, lightDirection, distanceToLight, diagnostics)
			: SmokeEmissiveVisible(receiverPosition, lightDirection, distanceToLight, diagnostics)) ? 1.0 : 0.0;
	}
	const float normalization = reservoir.WeightSum /
		max((float)SmokeEmissiveRecordM(reservoir) * reservoir.Target, 1e-8);
	float3 integrand = incident * SmokeHenyeyGreenstein(
		dot(lightDirection, normalize(ray)), phase.x) *
		(normalization * visibility * gSmokeConstants.RadianceScale);
	const float luminance = SmokeEmissiveLuminance(integrand);
	if (luminance > gSmokeConstants.DeltaTime)
		integrand *= gSmokeConstants.DeltaTime / luminance;
	uint metadata = SmokeFroxelMetadata(gSmokeFroxelSource[froxelIndex].w);
	metadata = SmokeFroxelResolveRadiance(metadata, gSmokeConstants.SimulationEpoch,
		NRI_SMOKE_FALLBACK_EMISSIVE, 0u);
	gSmokeFroxelSource[froxelIndex] = float4(
		gSmokeFroxelSource[froxelIndex].rgb + analyticMedium.rgb * integrand,
		SmokeFroxelMetadataValue(metadata));
}
