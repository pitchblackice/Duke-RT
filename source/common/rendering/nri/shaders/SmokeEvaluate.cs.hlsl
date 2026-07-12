#include "Include/SmokeResources.hlsli"
#include "Include/SmokeLighting.hlsli"

void SmokeAccumulateCandidate(
	uint particleIndex,
	uint particleCount,
	uint styleCount,
	float3 ray,
	float inverseRayLengthSquared,
	float sliceNearDepth,
	float sliceFarDepth,
	float sliceLength,
	inout float extinction,
	inout float3 scatteringCoefficient,
	inout float weightedAnisotropy,
	inout float anisotropyWeight)
{
	if (particleIndex >= min(gSmokeConstants.ParticleCapacity, particleCount))
		return;
	const SmokeParticle particle = gSmokeParticles[particleIndex];
	if (particle.Active == 0u || particle.Epoch != gSmokeConstants.SimulationEpoch || particle.StyleIndex >= min(gSmokeConstants.StyleCount, styleCount))
		return;
	const SmokeStyle style = gSmokeStyles[particle.StyleIndex];
	const float closestDepth = clamp(dot(particle.Position - gSmokeConstants.CameraPosition, ray) * inverseRayLengthSquared, sliceNearDepth, sliceFarDepth);
	const float3 particleSamplePosition = gSmokeConstants.CameraPosition + ray * closestDepth;
	const float normalizedDistance = length(particleSamplePosition - particle.Position) / max(particle.Radius, 0.001);
	const float axialCoverage = saturate((2.0 * particle.Radius) / sliceLength);
	const float weight = saturate(1.0 - normalizedDistance * normalizedDistance) * axialCoverage;
	const float localExtinction = weight * particle.Density * style.Extinction * gSmokeConstants.DensityScale;
	extinction += localExtinction;
	const float3 localScattering = localExtinction * saturate(style.Albedo);
	scatteringCoefficient += localScattering;
	const float localScatteringWeight = dot(localScattering, float3(0.2126, 0.7152, 0.0722));
	weightedAnisotropy += clamp(style.Anisotropy, -0.95, 0.95) * localScatteringWeight;
	anisotropyWeight += localScatteringWeight;
}

[numthreads(4, 4, 4)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (gSmokeConstants.FroxelWidth == 0u || gSmokeConstants.FroxelHeight == 0u || gSmokeConstants.FroxelDepth == 0u || gSmokeConstants.ColumnCapacity == 0u)
		return;

	if (dispatchThreadId.x >= gSmokeConstants.FroxelWidth || dispatchThreadId.y >= gSmokeConstants.FroxelHeight || dispatchThreadId.z >= gSmokeConstants.FroxelDepth)
		return;
	uint actualColumnCount, columnIndexCount, wideCellCount, wideCellIndexCount, particleCount, styleCount, localFroxelCount, controlCount, ignoredStride;
	gSmokeColumnCounts.GetDimensions(actualColumnCount, ignoredStride);
	gSmokeColumnIndices.GetDimensions(columnIndexCount, ignoredStride);
	gSmokeWideCellCounts.GetDimensions(wideCellCount, ignoredStride);
	gSmokeWideCellIndices.GetDimensions(wideCellIndexCount, ignoredStride);
	gSmokeParticles.GetDimensions(particleCount, ignoredStride);
	gSmokeStyles.GetDimensions(styleCount, ignoredStride);
	gSmokeFroxelLocal.GetDimensions(localFroxelCount, ignoredStride);
	gSmokeControl.GetDimensions(controlCount, ignoredStride);

	const uint columnIndex = dispatchThreadId.y * gSmokeConstants.FroxelWidth + dispatchThreadId.x;
	const uint froxelIndex = SmokeFroxelIndex(dispatchThreadId.x, dispatchThreadId.y, dispatchThreadId.z);
	if (columnIndex >= actualColumnCount || froxelIndex >= localFroxelCount)
		return;
	const uint fineCandidateCount = gSmokeColumnCounts[columnIndex] > 0u ? SmokeSelectionBucketCount() : 0u;
	const float sliceNearDepth = dispatchThreadId.z == 0u ? 0.0 : SmokeSliceFarDepth(dispatchThreadId.z - 1u);
	const float sliceFarDepth = SmokeSliceFarDepth(dispatchThreadId.z);
	const float2 uv = (float2(dispatchThreadId.xy) + 0.5) / float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight);
	const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	const float3 ray = gSmokeConstants.CameraForward +
		gSmokeConstants.CameraRight * (ndc.x * gSmokeConstants.TanHalfFovX) +
		gSmokeConstants.CameraUp * (ndc.y * gSmokeConstants.TanHalfFovY);
	const float inverseRayLengthSquared = rcp(max(dot(ray, ray), 0.000001));
	const float sliceLength = max(sliceFarDepth - sliceNearDepth, 0.001);
	const float froxelDepth = (sliceNearDepth + sliceFarDepth) * 0.5;
	const float3 froxelPosition = gSmokeConstants.CameraPosition + ray * froxelDepth;
	const bool lightingDiagnostics = (gSmokeConstants.Flags & 2u) != 0u && controlCount > 0u;

	float extinction = 0.0;
	float3 scatteringCoefficient = 0.0;
	float weightedAnisotropy = 0.0;
	float anisotropyWeight = 0.0;
	for (uint i = 0u; i < fineCandidateCount; ++i)
	{
		const uint candidateIndex = columnIndex * gSmokeConstants.ColumnCapacity + i;
		if (candidateIndex >= columnIndexCount)
			break;
		const uint packedCandidate = gSmokeColumnIndices[candidateIndex];
		if (packedCandidate != 0u)
			SmokeAccumulateCandidate(SmokeUnpackCandidateIndex(packedCandidate), particleCount, styleCount, ray, inverseRayLengthSquared,
				sliceNearDepth, sliceFarDepth, sliceLength, extinction, scatteringCoefficient, weightedAnisotropy, anisotropyWeight);
	}
	const uint2 wideCellPosition = min(uint2(
		dispatchThreadId.x * NRI_SMOKE_WIDE_GRID_X / gSmokeConstants.FroxelWidth,
		dispatchThreadId.y * NRI_SMOKE_WIDE_GRID_Y / gSmokeConstants.FroxelHeight),
		uint2(NRI_SMOKE_WIDE_GRID_X - 1u, NRI_SMOKE_WIDE_GRID_Y - 1u));
	const uint wideCellIndex = wideCellPosition.y * NRI_SMOKE_WIDE_GRID_X + wideCellPosition.x;
	const uint wideCandidateCount = wideCellIndex < wideCellCount && gSmokeWideCellCounts[wideCellIndex] > 0u ? NRI_SMOKE_WIDE_CELL_CAPACITY : 0u;
	[loop]
	for (uint i = 0u; i < wideCandidateCount; ++i)
	{
		const uint candidateIndex = wideCellIndex * NRI_SMOKE_WIDE_CELL_CAPACITY + i;
		if (candidateIndex >= wideCellIndexCount)
			break;
		const uint packedCandidate = gSmokeWideCellIndices[candidateIndex];
		if (packedCandidate != 0u)
			SmokeAccumulateCandidate(SmokeUnpackCandidateIndex(packedCandidate), particleCount, styleCount, ray, inverseRayLengthSquared,
				sliceNearDepth, sliceFarDepth, sliceLength, extinction, scatteringCoefficient, weightedAnisotropy, anisotropyWeight);
	}

	// Preserve the Phase 2 ambient fallback in every lighting mode. Empty
	// froxels must not perform tile scans or visibility queries.
	float3 scattering = scatteringCoefficient * 0.18;
	if (extinction > 0.0 && any(scatteringCoefficient > 0.0) &&
		gSmokeConstants.LightMode > 0u && gSmokeConstants.PointLightsEnabled != 0u)
	{
		const float anisotropy = anisotropyWeight > 1e-6 ? weightedAnisotropy / anisotropyWeight : 0.0;
		const float3 viewRay = normalize(ray);
		const RuntimeLightTileHeaderData tileHeader = SmokeGetRuntimeLightTileHeader(dispatchThreadId.xy);
		uint lightCount, lightStride, lightIndexCount, lightIndexStride;
		gSmokeRuntimePointLights.GetDimensions(lightCount, lightStride);
		gSmokeRuntimeLightTileIndices.GetDimensions(lightIndexCount, lightIndexStride);
		const uint runtimeLightCount = min(gSmokeConstants.RuntimeLightCount, lightCount);
		const uint selectionCapacity = min(gSmokeConstants.MaxLightCandidates, NRI_SMOKE_MAX_SELECTED_LIGHTS);
		uint selectedLightIndices[NRI_SMOKE_MAX_SELECTED_LIGHTS];
		float selectedLightScores[NRI_SMOKE_MAX_SELECTED_LIGHTS];
		uint selectedLightCount = 0u;

		// Keep only the strongest center-estimated contributors for this froxel.
		// The local array is a hard ceiling even if an upstream tile is crowded.
		[loop]
		for (uint tileCandidate = 0u; tileCandidate < tileHeader.indexCount && selectionCapacity > 0u; ++tileCandidate)
		{
			const uint packedIndex = tileHeader.indexOffset + tileCandidate;
			if (packedIndex >= lightIndexCount)
				break;
			const uint lightIndex = gSmokeRuntimeLightTileIndices[packedIndex];
			if (lightIndex >= runtimeLightCount)
				continue;
			if (lightingDiagnostics)
				InterlockedAdd(gSmokeControl[0].LightCandidatesTested, 1u);
			const RuntimePointLightData light = gSmokeRuntimePointLights[lightIndex];
			const float centerDistance = length(light.position - froxelPosition);
			const float attenuation = EvaluateAnalyticPointLightAttenuation(centerDistance, light.radius, light.intensity);
			const float score = attenuation * dot(max(light.color, 0.0), float3(0.2126, 0.7152, 0.0722));
			if (score <= 0.0)
			{
				if (lightingDiagnostics)
					InterlockedAdd(gSmokeControl[0].LightDistanceRejected, 1u);
				continue;
			}

			if (selectedLightCount < selectionCapacity)
			{
				selectedLightIndices[selectedLightCount] = lightIndex;
				selectedLightScores[selectedLightCount] = score;
				selectedLightCount++;
			}
			else
			{
				uint weakestIndex = 0u;
				[unroll]
				for (uint selectedIndex = 1u; selectedIndex < NRI_SMOKE_MAX_SELECTED_LIGHTS; ++selectedIndex)
				{
					if (selectedIndex < selectedLightCount && selectedLightScores[selectedIndex] < selectedLightScores[weakestIndex])
						weakestIndex = selectedIndex;
				}
				if (score > selectedLightScores[weakestIndex])
				{
					selectedLightIndices[weakestIndex] = lightIndex;
					selectedLightScores[weakestIndex] = score;
				}
			}
		}

		[loop]
		for (uint selectedIndex = 0u; selectedIndex < selectedLightCount; ++selectedIndex)
		{
			const RuntimePointLightData light = gSmokeRuntimePointLights[selectedLightIndices[selectedIndex]];
			const float3 toLightCenter = light.position - froxelPosition;
			const float centerDistanceSquared = dot(toLightCenter, toLightCenter);
			if (centerDistanceSquared <= 1e-4)
				continue;
			const float centerDistance = sqrt(centerDistanceSquared);
			const float attenuation = EvaluateAnalyticPointLightAttenuation(centerDistance, light.radius, light.intensity);
			if (attenuation <= 0.0)
				continue;
			const float3 centerDirection = toLightCenter / centerDistance;
			const uint sampleCount = gSmokeConstants.LightMode >= 3u ? clamp(gSmokeConstants.LightSamples, 1u, 4u) : 1u;
			float3 sampledContribution = 0.0;
			[loop]
			for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
			{
				float3 sampledPosition = light.position;
				if (gSmokeConstants.LightMode >= 3u)
				{
					uint randomState = SmokeLightRandomSeed(dispatchThreadId, light, sampleIndex);
					sampledPosition = SmokeSampleReceiverFacingEmitter(light, centerDirection, randomState);
					if (lightingDiagnostics && light.emitterRadius > 0.0)
						InterlockedAdd(gSmokeControl[0].LightSoftSamples, 1u);
				}
				const float3 toSampledLight = sampledPosition - froxelPosition;
				const float sampledDistanceSquared = dot(toSampledLight, toSampledLight);
				if (sampledDistanceSquared <= 1e-4)
					continue;
				const float sampledDistance = sqrt(sampledDistanceSquared);
				const float3 lightDirection = toSampledLight / sampledDistance;
				float visibility = 1.0;
				const bool castsShadow = (light.flags & NRI_SMOKE_RUNTIME_LIGHT_FLAG_CASTS_SHADOW) != 0u;
				if (gSmokeConstants.LightMode >= 2u && castsShadow)
				{
					if (lightingDiagnostics)
						InterlockedAdd(gSmokeControl[0].LightShadowRays, 1u);
					visibility = ((gSmokeConstants.FilteredVisibilityEnabled & 1u) != 0u
						? SmokePointLightVisibleFiltered(froxelPosition, lightDirection, sampledDistance, lightingDiagnostics)
						: SmokePointLightVisible(froxelPosition, lightDirection, sampledDistance, lightingDiagnostics)) ? 1.0 : 0.0;
					if (lightingDiagnostics)
					{
						if (visibility > 0.0)
							InterlockedAdd(gSmokeControl[0].LightShadowVisible, 1u);
						else
							InterlockedAdd(gSmokeControl[0].LightShadowOccluded, 1u);
					}
				}
				const float phase = SmokeHenyeyGreenstein(dot(lightDirection, viewRay), anisotropy);
				const float3 unclampedLightRadiance = max(light.color, 0.0) * attenuation;
				if (lightingDiagnostics && any(unclampedLightRadiance > 32.0))
					InterlockedAdd(gSmokeControl[0].LightRadianceClamps, 1u);
				const float3 lightRadiance = min(unclampedLightRadiance, 32.0);
				sampledContribution += lightRadiance * (phase * visibility);
			}
			scattering += scatteringCoefficient * (sampledContribution / (float)sampleCount);
		}
	}
	scattering *= gSmokeConstants.RadianceScale;
	gSmokeFroxelLocal[froxelIndex] = float4(scattering, max(extinction, 0.0));
}
