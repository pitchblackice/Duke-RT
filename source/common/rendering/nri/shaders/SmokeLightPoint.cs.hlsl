#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"
#include "Include/SmokeLighting.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (gSmokeConstants.FroxelWidth == 0u || gSmokeConstants.FroxelHeight == 0u || gSmokeConstants.FroxelDepth == 0u)
		return;

	uint controlCount, occupiedCapacity, mediumCount, phaseCount, sourceCount, ignoredStride;
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, ignoredStride);
	gSmokeFroxelMedium.GetDimensions(mediumCount, ignoredStride);
	gSmokeFroxelPhase.GetDimensions(phaseCount, ignoredStride);
	gSmokeFroxelSource.GetDimensions(sourceCount, ignoredStride);
	if (controlCount == 0u || dispatchThreadId.x >= min(gSmokeControl[0].OccupiedCount, occupiedCapacity))
		return;

	const uint froxelIndex = gSmokeOccupiedFroxelIndices[dispatchThreadId.x];
	const uint froxelCount = SmokeFroxelCount();
	if (froxelIndex >= froxelCount || froxelIndex >= mediumCount || froxelIndex >= phaseCount || froxelIndex >= sourceCount)
		return;
	const uint3 froxelPositionIndex = SmokeFroxelCoordinates(froxelIndex);
	const float3 ray = SmokeFroxelRay(froxelPositionIndex.xy);
	const float3 froxelPosition = SmokeFroxelCenter(froxelPositionIndex, ray);
	const float4 medium = gSmokeFroxelMedium[froxelIndex];
	const float anisotropy = gSmokeFroxelPhase[froxelIndex].x;
	const bool lightingDiagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	if (lightingDiagnostics)
		InterlockedAdd(gSmokeControl[0].PointFroxelsProcessed, 1u);

	// Preserve the original ambient fallback even when the point-light family is
	// disabled or unavailable. Later light families add to this source field.
	float3 scattering = medium.rgb * 0.18;
	if (medium.a > 0.0 && any(medium.rgb > 0.0) &&
		gSmokeConstants.LightMode > 0u && (gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_POINT) != 0u)
	{
		const float3 viewRay = normalize(ray);
		const RuntimeLightTileHeaderData tileHeader = SmokeGetRuntimeLightTileHeader(froxelPositionIndex.xy);
		uint lightCount, lightStride, lightIndexCount, lightIndexStride;
		gSmokeRuntimePointLights.GetDimensions(lightCount, lightStride);
		gSmokeRuntimeLightTileIndices.GetDimensions(lightIndexCount, lightIndexStride);
		const uint runtimeLightCount = min(gSmokeConstants.RuntimeLightCount, lightCount);
		const uint selectionCapacity = min(gSmokeConstants.MaxLightCandidates, NRI_SMOKE_MAX_SELECTED_LIGHTS);
		uint selectedLightIndices[NRI_SMOKE_MAX_SELECTED_LIGHTS];
		float selectedLightScores[NRI_SMOKE_MAX_SELECTED_LIGHTS];
		uint selectedLightCount = 0u;

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
					uint randomState = SmokeLightRandomSeed(froxelPositionIndex, light, sampleIndex);
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
			scattering += medium.rgb * (sampledContribution / (float)sampleCount);
		}
	}
	gSmokeFroxelSource[froxelIndex] = float4(scattering * gSmokeConstants.RadianceScale, 0.0);
}
