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
	uint scratchCount, scratchStride;
	gSmokeIndirectScratch.GetDimensions(scratchCount, scratchStride);
	if (froxelIndex >= scratchCount)
		return;
	const float3 visibleDirectionalScattering = max(gSmokeIndirectScratch[froxelIndex].Radiance, 0.0);
	const float3 centerDirection = SmokeDirectionalDirection();
	const float anisotropy = gSmokeFroxelPhase[froxelIndex].x;
	const bool castsShadow = (gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_DIRECTIONAL_SHADOW) != 0u;
	if (gSmokeConstants.LightMode >= 2u && castsShadow && !SmokeShadowTracingReady())
		return;
	const float3 unclamped = visibleDirectionalScattering * SmokeDirectionalColor() *
		SmokeHenyeyGreenstein(dot(centerDirection, viewRay), anisotropy);
	if (diagnostics && any(unclamped > 32.0))
		InterlockedAdd(gSmokeControl[0].DirectionalRadianceClamps, 1u);
	const float3 source = gSmokeFroxelSource[froxelIndex].rgb + min(unclamped, 32.0) * gSmokeConstants.RadianceScale;
	gSmokeFroxelSource[froxelIndex] = float4(source, 0.0);
}
