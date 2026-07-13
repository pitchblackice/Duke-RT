#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"
#include "Include/SmokeLighting.hlsli"

uint SmokeStableEmissiveSeed(uint3 froxelCoordinates, uint sampleIndex)
{
	uint seed = SmokeHash(froxelCoordinates.x ^ SmokeHash(froxelCoordinates.y + 0x6d2b79f5u));
	seed ^= SmokeHash(froxelCoordinates.z + 0x9e3779b9u);
	seed ^= SmokeHash(gSmokeConstants.SimulationEpoch + 0x85ebca6bu);
	seed ^= SmokeHash(sampleIndex + 0xc2b2ae35u);
	return SmokeHash(seed);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint controlCount, occupiedCapacity, mediumCount, phaseCount, sourceCount, ignoredStride;
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, ignoredStride);
	gSmokeFroxelMedium.GetDimensions(mediumCount, ignoredStride);
	gSmokeFroxelPhase.GetDimensions(phaseCount, ignoredStride);
	gSmokeFroxelSource.GetDimensions(sourceCount, ignoredStride);
	if (controlCount == 0u || dispatchThreadId.x >= min(gSmokeControl[0].OccupiedCount, occupiedCapacity))
		return;

	const uint froxelIndex = gSmokeOccupiedFroxelIndices[dispatchThreadId.x];
	if (froxelIndex >= SmokeFroxelCount() || froxelIndex >= mediumCount || froxelIndex >= phaseCount || froxelIndex >= sourceCount)
		return;
	const float4 medium = gSmokeFroxelMedium[froxelIndex];
	if (medium.a <= 0.0 || !any(medium.rgb > 0.0))
		return;

	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	if (gSmokeConstants.LightMode == 0u || (gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_EMISSIVE) == 0u)
		return;
	if (gSmokeConstants.LightMode >= 2u && !SmokeShadowTracingReady())
		return;
	if (diagnostics)
		InterlockedAdd(gSmokeControl[0].EmissiveFroxelsProcessed, 1u);

	const uint3 froxelCoordinates = SmokeFroxelCoordinates(froxelIndex);
	const float3 ray = SmokeFroxelRay(froxelCoordinates.xy);
	const float3 viewRay = normalize(ray);
	const float3 froxelPosition = SmokeFroxelCenter(froxelCoordinates, ray);
	const float anisotropy = gSmokeFroxelPhase[froxelIndex].x;
	const uint sampleCount = gSmokeConstants.LightMode >= 3u ? clamp(gSmokeConstants.LightSamples, 1u, 4u) : 1u;
	float3 contribution = 0.0;
	[loop]
	for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
	{
		uint randomState = gSmokeConstants.LightMode >= 3u
			? SmokeLightingRandomSeed(froxelCoordinates, sampleIndex, 0xe6a7c15bu)
			: SmokeStableEmissiveSeed(froxelCoordinates, sampleIndex);
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
		PrimitiveData primitive = (PrimitiveData)0;
		MaterialData material = (MaterialData)0;
		uint sampledPrimitiveIndex = candidate.primitiveIndex;
		float2 lightUv;
		float3 lightNormal;
		float3 lightPosition;
		float effectiveArea;
		if (!SmokeSamplePointOnEmissive(candidate, randomState, sampledPrimitiveIndex, primitive, material, lightPosition, lightUv, lightNormal, effectiveArea))
		{
			if (diagnostics)
				InterlockedAdd(gSmokeControl[0].EmissiveCandidateMisses, 1u);
			continue;
		}
		float3 lightRadiance = SmokeSampleMaterialEmission(material, lightUv) * max(material.emissiveIntensity, 0.0);
		lightRadiance *= max(candidate.emissionScale, 0.0);
		if (!any(lightRadiance > 0.0))
			continue;

		const float3 toLight = lightPosition - froxelPosition;
		const float distanceSquared = dot(toLight, toLight);
		if (distanceSquared <= 0.0001)
		{
			if (diagnostics)
				InterlockedAdd(gSmokeControl[0].EmissiveDistanceRejected, 1u);
			continue;
		}
		const float distanceToLight = sqrt(distanceSquared);
		const float3 lightDirection = toLight / distanceToLight;
		const float emitterCosine = max(dot(lightNormal, -lightDirection), 0.0);
		if (emitterCosine <= 0.0)
		{
			if (diagnostics)
				InterlockedAdd(gSmokeControl[0].EmissiveFacingRejected, 1u);
			continue;
		}

		float visibility = 1.0;
		if (gSmokeConstants.LightMode >= 2u)
		{
			if (diagnostics)
				InterlockedAdd(gSmokeControl[0].EmissiveShadowRays, 1u);
			visibility = (SmokeFilteredVisibilityEffective()
				? SmokePointLightVisibleFiltered(froxelPosition, lightDirection, distanceToLight, diagnostics)
				: SmokePointLightVisible(froxelPosition, lightDirection, distanceToLight, diagnostics)) ? 1.0 : 0.0;
			if (diagnostics)
			{
				if (visibility > 0.0)
					InterlockedAdd(gSmokeControl[0].EmissiveShadowVisible, 1u);
				else
					InterlockedAdd(gSmokeControl[0].EmissiveShadowOccluded, 1u);
			}
		}

		const float pdf = max(candidate.selectionPdf, 1e-4);
		const float projectedArea = max(effectiveArea * emitterCosine, 0.001);
		const float falloffScale = max(material.emissiveMaskScale, 0.25);
		const float attenuatedDistanceSquared = pow(max(distanceSquared, 0.01), falloffScale);
		const float solidAngle = min(projectedArea / max(12.56637061436 * attenuatedDistanceSquared, 0.01), 1.0);
		const float sampleWeight = min(solidAngle / pdf, 16.0);
		if (diagnostics && any(lightRadiance > 32.0))
			InterlockedAdd(gSmokeControl[0].EmissiveRadianceClamps, 1u);
		lightRadiance = min(lightRadiance, 32.0);
		contribution += lightRadiance * (SmokeHenyeyGreenstein(dot(lightDirection, viewRay), anisotropy) * sampleWeight * visibility);
		if (diagnostics && visibility > 0.0)
			InterlockedAdd(gSmokeControl[0].EmissiveContributed, 1u);
	}

	const float3 source = gSmokeFroxelSource[froxelIndex].rgb + medium.rgb * (contribution / (float)sampleCount) * gSmokeConstants.RadianceScale;
	gSmokeFroxelSource[froxelIndex] = float4(source, 0.0);
}
