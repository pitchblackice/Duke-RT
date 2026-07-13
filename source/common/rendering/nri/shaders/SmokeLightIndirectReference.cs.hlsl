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
	if (medium.a <= 0.0 || !any(medium.rgb > 0.0) || gSmokeConstants.IndirectScale <= 0.0 ||
		(gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_INDIRECT) == 0u)
		return;

	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	if (diagnostics)
		InterlockedAdd(gSmokeControl[0].IndirectFroxelsProcessed, 1u);
	const uint3 froxel = SmokeFroxelCoordinates(froxelIndex);
	const float3 froxelRay = SmokeFroxelRay(froxel.xy);
	const float3 viewRay = normalize(froxelRay);
	const float3 position = SmokeFroxelCenter(froxel, froxelRay);
	// World Z-up maps to negative renderer Y in the canonical PT transform.
	const float3 worldUp = float3(0.0, -1.0, 0.0);
	const SmokeIndirectHit upHit = SmokeTraceIndirectClosest(position, worldUp, gSmokeConstants.FroxelMaxDistance);
	const SmokeIndirectHit downHit = SmokeTraceIndirectClosest(position, -worldUp, gSmokeConstants.FroxelMaxDistance);
	if (diagnostics)
		InterlockedAdd(gSmokeControl[0].IndirectLocalityRays, 2u);

	uint upSector, downSector;
	const bool upValid = SmokeResolveStaticFlatSector(upHit, upSector);
	const bool downValid = SmokeResolveStaticFlatSector(downHit, downSector);
	if (!upValid || !downValid || upSector != downSector)
	{
		if (diagnostics)
		{
			if (upValid != downValid)
				InterlockedAdd(gSmokeControl[0].IndirectLocalityOneSided, 1u);
			else if (upValid && downValid)
				InterlockedAdd(gSmokeControl[0].IndirectLocalityMismatch, 1u);
			else
				InterlockedAdd(gSmokeControl[0].IndirectLocalityInvalid, 1u);
		}
		return;
	}
	if (diagnostics)
		InterlockedAdd(gSmokeControl[0].IndirectLocalityAgreement, 1u);

	float3 incidentRadiance = SmokeSectorAmbientIncidentRadiance(upSector);
	if (diagnostics && any(incidentRadiance > 0.0))
		InterlockedAdd(gSmokeControl[0].IndirectSectorContributions, 1u);
	const float anisotropy = gSmokeFroxelPhase[froxelIndex].x;
	float3 sampledRadiance = 0.0;
	[unroll]
	for (uint sampleIndex = 0u; sampleIndex < NRI_SMOKE_INDIRECT_SAMPLE_COUNT; ++sampleIndex)
	{
		const float3 direction = SmokeIndirectReferenceDirection(sampleIndex, froxel);
		const SmokeIndirectHit hit = SmokeTraceIndirectClosest(position, direction, gSmokeConstants.FroxelMaxDistance);
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].IndirectReferenceRays, 1u);
		float3 rayRadiance = 0.0;
		if (hit.hit == 0u)
		{
			rayRadiance = max(gSmokeSkyTexture.SampleLevel(gSmokeLinearWrap, direction, 0.0).rgb, 0.0);
			if (diagnostics)
			{
				InterlockedAdd(gSmokeControl[0].IndirectReferenceMisses, 1u);
				if (any(rayRadiance > 0.0)) InterlockedAdd(gSmokeControl[0].IndirectSkyContributions, 1u);
			}
		}
		else if (hit.instanceValid != 0u)
		{
			const MaterialData material = SmokeGetMaterial(hit.dataSource, hit.materialIndex);
			rayRadiance = max(SmokeSampleMaterialEmission(material, hit.uv), 0.0);
			if (diagnostics)
			{
				InterlockedAdd(gSmokeControl[0].IndirectReferenceHits, 1u);
				if (any(rayRadiance > 0.0)) InterlockedAdd(gSmokeControl[0].IndirectEmissionContributions, 1u);
			}
		}
		const float phaseOverUniformPdf = SmokeHenyeyGreenstein(dot(direction, viewRay), anisotropy) * 12.56637061436;
		sampledRadiance += rayRadiance * phaseOverUniformPdf;
	}
	incidentRadiance += sampledRadiance / (float)NRI_SMOKE_INDIRECT_SAMPLE_COUNT;
	if (!all(isfinite(incidentRadiance)))
	{
		if (diagnostics) InterlockedAdd(gSmokeControl[0].IndirectNanRejects, 1u);
		return;
	}
	if (diagnostics && any(incidentRadiance > 32.0))
		InterlockedAdd(gSmokeControl[0].IndirectRadianceClamps, 1u);
	incidentRadiance = min(incidentRadiance, 32.0);
	gSmokeFroxelSource[froxelIndex].rgb += medium.rgb * incidentRadiance *
		(gSmokeConstants.IndirectScale * gSmokeConstants.RadianceScale);
}
