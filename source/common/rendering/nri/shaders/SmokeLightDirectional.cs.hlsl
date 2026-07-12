#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"
#include "Include/SmokeLighting.hlsli"

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
	if (gSmokeConstants.LightMode == 0u || (gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_DIRECTIONAL) == 0u)
		return;
	if (diagnostics)
		InterlockedAdd(gSmokeControl[0].DirectionalFroxelsProcessed, 1u);

	const uint3 froxelCoordinates = SmokeFroxelCoordinates(froxelIndex);
	const float3 ray = SmokeFroxelRay(froxelCoordinates.xy);
	const float3 viewRay = normalize(ray);
	const float3 froxelPosition = SmokeFroxelCenter(froxelCoordinates, ray);
	const float3 centerDirection = SmokeDirectionalDirection();
	const float anisotropy = gSmokeFroxelPhase[froxelIndex].x;
	const bool castsShadow = (gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_DIRECTIONAL_SHADOW) != 0u;
	const uint sampleCount = gSmokeConstants.LightMode >= 3u ? clamp(gSmokeConstants.LightSamples, 1u, 4u) : 1u;
	float3 contribution = 0.0;
	[loop]
	for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
	{
		uint randomState = SmokeLightingRandomSeed(
			froxelCoordinates,
			sampleIndex,
			gSmokeConstants.DirectionalColorPacked ^
			asuint(gSmokeConstants.DirectionalAngularSize) ^
			SmokeHash(asuint(gSmokeConstants.DirectionalDirectionX)) ^
			SmokeHash(asuint(gSmokeConstants.DirectionalDirectionY)) ^
			SmokeHash(asuint(gSmokeConstants.DirectionalDirectionZ)));
		const float3 lightDirection = gSmokeConstants.LightMode >= 3u
			? SmokeSampleDirectionalCone(centerDirection, gSmokeConstants.DirectionalAngularSize, randomState)
			: centerDirection;
		float visibility = 1.0;
		if (gSmokeConstants.LightMode >= 2u && castsShadow)
		{
			if (diagnostics)
				InterlockedAdd(gSmokeControl[0].DirectionalShadowRays, 1u);
			visibility = ((gSmokeConstants.FilteredVisibilityEnabled & 1u) != 0u
				? SmokePointLightVisibleFiltered(froxelPosition, lightDirection, 100000.0, diagnostics)
				: SmokePointLightVisible(froxelPosition, lightDirection, 100000.0, diagnostics)) ? 1.0 : 0.0;
			if (diagnostics)
			{
				if (visibility > 0.0)
					InterlockedAdd(gSmokeControl[0].DirectionalShadowVisible, 1u);
				else
					InterlockedAdd(gSmokeControl[0].DirectionalShadowOccluded, 1u);
			}
		}
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].DirectionalSamples, 1u);
		contribution += SmokeDirectionalColor() * (SmokeHenyeyGreenstein(dot(lightDirection, viewRay), anisotropy) * visibility);
	}

	const float3 unclamped = contribution / (float)sampleCount;
	if (diagnostics && any(unclamped > 32.0))
		InterlockedAdd(gSmokeControl[0].DirectionalRadianceClamps, 1u);
	const float3 source = gSmokeFroxelSource[froxelIndex].rgb + medium.rgb * min(unclamped, 32.0) * gSmokeConstants.RadianceScale;
	gSmokeFroxelSource[froxelIndex] = float4(source, 0.0);
}
