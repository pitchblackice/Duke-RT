#include "Include/SmokeEmissiveReservoir.hlsli"
#include "Include/SmokeGridLightingResources.hlsli"

void SmokeGridLightWriteTarget(uint cellIndex, SmokeGridLightRecord record)
{
	if ((gSmokeConstants.Flags & NRI_SMOKE_GRID_LIGHT_FIELD_PING) != 0u)
		gSmokeGridLightHistory[cellIndex] = record;
	else
		gSmokeGridLightCurrent[cellIndex] = record;
}

uint SmokeGridLightSeedForCell(int3 cell, uint sampleIndex)
{
	uint seed = SmokeHash(asuint(cell.x) ^ SmokeHash(asuint(cell.y)) ^ SmokeHash(asuint(cell.z)) ^
		SmokeHash(gSmokeConstants.SimulationEpoch) ^ SmokeHash(sampleIndex + 0x9e3779b9u));
	if (gSmokeConstants.LightMode >= 3u)
		seed ^= SmokeHash(gSmokeConstants.FrameIndex);
	return SmokeHash(seed);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint activeCapacity, ignoredStride;
	gSmokeGridLightActive.GetDimensions(activeCapacity, ignoredStride);
	const uint activeCount = min(gSmokeGridLightControl[0].ActiveCount, activeCapacity);
	if (dispatchThreadId.x >= activeCount)
		return;
	const uint cellIndex = gSmokeGridLightActive[dispatchThreadId.x];
	const uint brickIndex = cellIndex / NRI_SMOKE_GRID_CELLS_PER_BRICK;
	const uint localIndex = cellIndex % NRI_SMOKE_GRID_CELLS_PER_BRICK;
	const uint3 local = uint3(localIndex & 7u, (localIndex >> 3u) & 7u, (localIndex >> 6u) & 7u);
	const SmokeGridBrick brick = gSmokeRenderGridBricks[brickIndex];
	const float cellSize = max(asfloat(gSmokeRenderGridControl[0].CellSizeBits), 0.0001);
	const int3 cell = SmokeGridCellCoordinate(brick.Coordinate, local);
	const float3 receiverPosition = SmokeGridLightCellCenter(cell, cellSize);
	const uint sampleCount = (gSmokeConstants.Flags & NRI_SMOKE_EMISSIVE_REFERENCE) != 0u ? 32u : 1u;
	float3 mean[6];
	float3 second[6];
	[unroll] for (uint lobe = 0u; lobe < 6u; ++lobe) { mean[lobe] = 0.0; second[lobe] = 0.0; }
	uint physicalZero = 0u;
	uint visible = 0u;
	[loop]
	for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
	{
		uint randomState = SmokeGridLightSeedForCell(cell, sampleIndex);
		const uint candidateIndex = SmokeSampleEmissivePrimitive(randomState);
		InterlockedAdd(gSmokeGridLightControl[0].Samples, 1u);
		if (candidateIndex == 0xffffffffu)
		{
			InterlockedAdd(gSmokeGridLightControl[0].Missing, 1u);
			physicalZero++;
			continue;
		}
		const EmissivePrimitiveData candidate = gSmokeEmissivePrimitives[candidateIndex];
		SmokeEmissiveReservoirRecord record = SmokeEmptyEmissiveReservoir();
		record.CandidateIndex = candidateIndex;
		record.SampleSeed = randomState;
		record.StableKeyLo = candidate.stableKeyLo;
		record.StableKeyHi = candidate.stableKeyHi;
		record.Generation = gSmokeConstants.CommandCount;
		float3 incident, lightDirection;
		float lightDistance;
		if (!SmokeEvaluateEmissiveIncident(record, receiverPosition, false, incident, lightDirection, lightDistance))
		{
			physicalZero++;
			continue;
		}
		bool isVisible = true;
		if (gSmokeConstants.LightMode >= 2u)
			isVisible = SmokeFilteredVisibilityEffective() ?
				SmokePointLightVisibleFiltered(receiverPosition, lightDirection, lightDistance, false) :
				SmokePointLightVisible(receiverPosition, lightDirection, lightDistance, false);
		if (!isVisible)
		{
			physicalZero++;
			continue;
		}
		visible++;
		const float3 estimator = incident / max(candidate.selectionPdf, 1e-6);
		float weights[6];
		float weightSum = 0.0;
		[unroll]
		for (uint lobe = 0u; lobe < 6u; ++lobe)
		{
			weights[lobe] = max(dot(lightDirection, (float3)NRI_SMOKE_GRID_LIGHT_LOBE_AXES[lobe]), 0.0);
			weightSum += weights[lobe];
		}
		[unroll]
		for (uint lobe = 0u; lobe < 6u; ++lobe)
		{
			const float3 contribution = estimator * (weights[lobe] / max(weightSum, 1e-6));
			mean[lobe] += contribution / (float)sampleCount;
			second[lobe] += contribution * contribution / (float)sampleCount;
		}
	}
	SmokeGridLightRecord output = (SmokeGridLightRecord)0;
	bool finite = true;
	[unroll]
	for (uint lobe = 0u; lobe < 6u; ++lobe)
	{
		finite = finite && all(isfinite(mean[lobe])) && all(isfinite(second[lobe])) &&
			all(mean[lobe] <= 65504.0) && all(second[lobe] <= 65504.0);
		SmokeGridLightStoreLobe(output, lobe, finite ? mean[lobe] : 0.0, finite ? second[lobe] : 0.0);
	}
	if (!finite)
	{
		InterlockedAdd(gSmokeGridLightControl[0].OverflowRejects, 1u);
		SmokeGridLightWriteTarget(cellIndex, output);
		return;
	}
	uint evidence = NRI_SMOKE_GRID_LIGHT_EVIDENCE_SUPPORT | NRI_SMOKE_GRID_LIGHT_EVIDENCE_VALID;
	if (physicalZero > 0u) evidence |= NRI_SMOKE_GRID_LIGHT_EVIDENCE_PHYSICAL_ZERO;
	if (visible > 0u) evidence |= NRI_SMOKE_GRID_LIGHT_EVIDENCE_VISIBLE;
	SmokeGridLightSetMetadata(output, brick.Generation, gSmokeConstants.SimulationEpoch,
		sampleCount, gSmokeConstants.FrameIndex & 0xffu, (float)sampleCount / 64.0, evidence,
		gSmokeConstants.FrameIndex, 0u);
	SmokeGridLightWriteTarget(cellIndex, output);
	InterlockedAdd(gSmokeGridLightControl[0].ScheduledCount, 1u);
	if (physicalZero > 0u) InterlockedAdd(gSmokeGridLightControl[0].PhysicalZero, physicalZero);
	if (visible > 0u) InterlockedAdd(gSmokeGridLightControl[0].Visible, visible);
}
