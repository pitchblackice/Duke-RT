#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"

float SmokeAnalyticRectangleKernelAverage(SmokeAnalyticCarrier carrier, float3 ray,
	float nearDepth, float farDepth)
{
	// A bounded four-point depth quadrature avoids the prompt path's center-only
	// rectangle test. Sphere carriers use the exact segment integral below.
	float integral = 0.0;
	[unroll]
	for (uint sampleIndex = 0u; sampleIndex < 4u; ++sampleIndex)
	{
		const float depth = lerp(nearDepth, farDepth, ((float)sampleIndex + 0.5) * 0.25);
		const float3 position = gSmokeConstants.CameraPosition + ray * depth;
		const float3 closest = SmokeInjectionClosestRectanglePoint(position, carrier.Position,
			carrier.HalfAxisU, carrier.HalfAxisV);
		const float normalized = saturate(1.0 - distance(position, closest) / max(carrier.Radius, 0.001));
		integral += normalized * normalized * (3.0 - 2.0 * normalized);
	}
	return integral * 0.25;
}

[numthreads(4, 4, 4)]
void main(uint3 froxel : SV_DispatchThreadID)
{
	if (froxel.x >= gSmokeConstants.FroxelWidth || froxel.y >= gSmokeConstants.FroxelHeight ||
		froxel.z >= gSmokeConstants.FroxelDepth)
		return;
	const uint2 tileCount = SmokeAnalyticTileCount(
		gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight);
	if (any(tileCount == 0u))
		return;
	const uint2 tile = froxel.xy / NRI_SMOKE_ANALYTIC_TILE_SIZE;
	const uint tileIndex = SmokeAnalyticTileIndex(tile, tileCount);
	uint headerCapacity, headerStride;
	gSmokeAnalyticTileHeaders.GetDimensions(headerCapacity, headerStride);
	if (tileIndex >= headerCapacity)
		return;
	const SmokeAnalyticTileHeader header = gSmokeAnalyticTileHeaders[tileIndex];
	const uint candidateCount = min(header.Count, NRI_SMOKE_ANALYTIC_MAX_CARRIERS_PER_TILE);
	if (candidateCount == 0u)
		return;

	const float nearDepth = SmokeSliceNearDepth(froxel.z);
	const float farDepth = SmokeSliceFarDepth(froxel.z);
	const float3 ray = SmokeFroxelRay(froxel.xy);
	float extinction = 0.0;
	float3 scattering = 0.0;
	float weightedAnisotropy = 0.0;
	float anisotropyWeight = 0.0;
	uint contributors = 0u;
	uint carrierCapacity, carrierStride;
	uint indexCapacity, indexStride;
	gSmokeAnalyticCarriers.GetDimensions(carrierCapacity, carrierStride);
	gSmokeAnalyticTileIndices.GetDimensions(indexCapacity, indexStride);
	[loop]
	for (uint candidate = 0u; candidate < candidateCount; ++candidate)
	{
		const uint candidateIndex = tileIndex * NRI_SMOKE_ANALYTIC_MAX_CARRIERS_PER_TILE + candidate;
		if (candidateIndex >= indexCapacity)
			break;
		const uint carrierIndex = gSmokeAnalyticTileIndices[candidateIndex];
		if (carrierIndex >= min(min(carrierCapacity, NRI_SMOKE_ANALYTIC_MAX_CARRIERS),
			gSmokeConstants.ParticleCapacity))
			continue;
		const SmokeAnalyticCarrier carrier = gSmokeAnalyticCarriers[carrierIndex];
		if ((carrier.Flags & NRI_SMOKE_ANALYTIC_CARRIER_ACTIVE) == 0u ||
			carrier.Epoch != gSmokeConstants.SimulationEpoch ||
			carrier.StyleIndex >= gSmokeConstants.StyleCount ||
			!isfinite(carrier.Radius) || carrier.Radius <= 0.0 ||
			!isfinite(carrier.DensityScale) || carrier.DensityScale <= 0.0 ||
			carrier.RangeCount == 0u)
			continue;
		const SmokeStyle style = gSmokeStyles[carrier.StyleIndex];
		const float kernel = carrier.Shape == NRI_SMOKE_INJECTION_SHAPE_RECTANGLE
			? SmokeAnalyticRectangleKernelAverage(carrier, ray, nearDepth, farDepth)
			: SmokeSphereSegmentKernelAverage(carrier.Position, carrier.Radius, ray, nearDepth, farDepth);
		if (kernel <= 0.0)
			continue;
		const float density = max(style.Density * carrier.DensityScale, 0.0) *
			(float)min(carrier.RangeCount, 256u);
		const float sigmaT = kernel * density * max(style.Extinction, 0.0) * gSmokeConstants.DensityScale;
		if (sigmaT <= 1e-6)
			continue;
		const float3 sigmaS = sigmaT * saturate(style.Albedo);
		extinction += sigmaT;
		scattering += sigmaS;
		const float weight = dot(sigmaS, float3(0.2126, 0.7152, 0.0722));
		weightedAnisotropy += weight * clamp(style.Anisotropy, -0.95, 0.95);
		anisotropyWeight += weight;
		contributors++;
	}
	if (extinction <= 1e-6)
		return;

	const uint index = SmokeFroxelIndex(froxel.x, froxel.y, froxel.z);
	uint mediumCapacity, mediumStride;
	uint analyticCapacity, analyticStride;
	gSmokeFroxelMedium.GetDimensions(mediumCapacity, mediumStride);
	gSmokeAnalyticFroxelMedium.GetDimensions(analyticCapacity, analyticStride);
	if (index >= min(mediumCapacity, analyticCapacity))
		return;
	const float4 previousMedium = gSmokeFroxelMedium[index];
	const float4 previousPhase = gSmokeFroxelPhase[index];
	const float4 previousSource = gSmokeFroxelSource[index];
	const bool wasOccupied = previousMedium.w > 1e-6;
	const float4 analyticMedium = float4(scattering, extinction);
	gSmokeAnalyticFroxelMedium[index] = analyticMedium;
	gSmokeFroxelMedium[index] = previousMedium + analyticMedium;
	const float previousWeight = max(previousPhase.y, 0.0);
	const float combinedWeight = previousWeight + anisotropyWeight;
	const float anisotropy = combinedWeight > 1e-6
		? (previousPhase.x * previousWeight + weightedAnisotropy) / combinedWeight : 0.0;
	const uint previousOwnership = SmokeFroxelCarrierOwnership(previousPhase);
	gSmokeFroxelPhase[index] = float4(anisotropy, combinedWeight,
		max(previousPhase.z, 0.0) + (float)contributors,
		(float)(previousOwnership | NRI_SMOKE_FROXEL_CARRIER_ANALYTIC));

	// Analytic carriers start from a conservative source and are refined by the
	// shared point, directional, emissive, and indirect passes. They never claim
	// grid-world radiance ownership.
	const uint previousMetadata = SmokeFroxelMetadata(previousSource.w);
	if (wasOccupied && SmokeFroxelCarrierValid(previousMetadata) && SmokeFroxelRadianceValid(previousMetadata))
		gSmokeFroxelSource[index] = previousSource;
	else
	{
		uint metadata = SmokeFroxelCarrierMetadata(gSmokeConstants.SimulationEpoch);
		metadata = SmokeFroxelResolveRadiance(metadata, gSmokeConstants.SimulationEpoch,
			NRI_SMOKE_FALLBACK_ENVIRONMENT, 0u);
		gSmokeFroxelSource[index] = float4(scattering * 0.02,
			SmokeFroxelMetadataValue(metadata));
	}
	if (!wasOccupied)
	{
		uint occupiedCapacity, occupiedStride, occupiedSlot;
		gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, occupiedStride);
		InterlockedAdd(gSmokeControl[0].OccupiedCount, 1u, occupiedSlot);
		if (occupiedSlot < occupiedCapacity)
			gSmokeOccupiedFroxelIndices[occupiedSlot] = index;
		else
			InterlockedAdd(gSmokeControl[0].OccupiedOverflow, 1u);
	}
}
