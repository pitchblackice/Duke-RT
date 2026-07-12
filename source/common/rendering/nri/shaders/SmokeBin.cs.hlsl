#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"

#define NRI_SMOKE_CANDIDATE_TIER_FINE 0u
#define NRI_SMOKE_CANDIDATE_TIER_WIDE 1u
#define NRI_SMOKE_CANDIDATE_TIER_GLOBAL 2u

void SmokeInterlockedMaxTierCandidate(uint tier, uint index, uint value, out uint originalValue)
{
	if (tier == NRI_SMOKE_CANDIDATE_TIER_FINE)
		InterlockedMax(gSmokeFineCellIndices[index], value, originalValue);
	else if (tier == NRI_SMOKE_CANDIDATE_TIER_WIDE)
		InterlockedMax(gSmokeWideCellIndices[index], value, originalValue);
	else
		InterlockedMax(gSmokeGlobalDepthIndices[index], value, originalValue);
}

void SmokeRecordTierCollision(uint tier)
{
	InterlockedAdd(gSmokeControl[0].SelectionCollisions, 1u);
	if (tier == NRI_SMOKE_CANDIDATE_TIER_FINE)
		InterlockedAdd(gSmokeControl[0].FineSelectionCollisions, 1u);
	else if (tier == NRI_SMOKE_CANDIDATE_TIER_WIDE)
		InterlockedAdd(gSmokeControl[0].WideSelectionCollisions, 1u);
	else
		InterlockedAdd(gSmokeControl[0].GlobalSelectionCollisions, 1u);
}

void SmokeRecordTierFullDrop(uint tier, bool replaced)
{
	if (tier == NRI_SMOKE_CANDIDATE_TIER_FINE)
	{
		InterlockedAdd(gSmokeControl[0].FineSelectionLosses, 1u);
		if (replaced) InterlockedAdd(gSmokeControl[0].FineSelectionReplacements, 1u);
	}
	else if (tier == NRI_SMOKE_CANDIDATE_TIER_WIDE)
	{
		InterlockedAdd(gSmokeControl[0].WideSelectionLosses, 1u);
		if (replaced) InterlockedAdd(gSmokeControl[0].WideSelectionReplacements, 1u);
	}
	else
	{
		InterlockedAdd(gSmokeControl[0].GlobalSelectionLosses, 1u);
		if (replaced) InterlockedAdd(gSmokeControl[0].GlobalSelectionReplacements, 1u);
	}
	if (replaced)
		InterlockedAdd(gSmokeControl[0].SelectionReplacements, 1u);
}

bool SmokeInsertTierCandidate(
	uint tier,
	uint baseIndex,
	uint capacity,
	uint indexCount,
	uint packedCandidate,
	bool diagnostics,
	out bool invalidTarget)
{
	invalidTarget = capacity == 0u || baseIndex >= indexCount || capacity > indexCount - baseIndex;
	if (invalidTarget)
		return false;

	uint carry = packedCandidate;
	bool collided = false;
	[loop]
	for (uint probe = 0u; probe < capacity; ++probe)
	{
		uint originalValue = 0u;
		SmokeInterlockedMaxTierCandidate(tier, baseIndex + probe, carry, originalValue);
		if (originalValue == 0u)
		{
			if (diagnostics && collided)
				SmokeRecordTierCollision(tier);
			return true;
		}
		if (originalValue == carry)
		{
			if (diagnostics && collided)
				SmokeRecordTierCollision(tier);
			return true;
		}
		collided = true;
		carry = min(carry, originalValue);
	}

	if (diagnostics)
	{
		SmokeRecordTierCollision(tier);
		SmokeRecordTierFullDrop(tier, carry != packedCandidate);
	}
	// Every atomic conserves max(slot, carry) in the slot and forwards the
	// lower value. Reaching capacity means exactly one weakest reference is
	// discarded; the incoming candidate survived iff a different value fell out.
	return carry != packedCandidate;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (gSmokeConstants.FroxelWidth == 0u || gSmokeConstants.FroxelHeight == 0u || gSmokeConstants.FroxelDepth == 0u)
		return;

	uint particleCount, controlCount, fineCellCount, fineCellIndexCount, wideCellCount, wideCellIndexCount;
	uint globalDepthCount, globalDepthIndexCount, ignoredStride;
	gSmokeParticles.GetDimensions(particleCount, ignoredStride);
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeFineCellCounts.GetDimensions(fineCellCount, ignoredStride);
	gSmokeFineCellIndices.GetDimensions(fineCellIndexCount, ignoredStride);
	gSmokeWideCellCounts.GetDimensions(wideCellCount, ignoredStride);
	gSmokeWideCellIndices.GetDimensions(wideCellIndexCount, ignoredStride);
	gSmokeGlobalDepthCounts.GetDimensions(globalDepthCount, ignoredStride);
	gSmokeGlobalDepthIndices.GetDimensions(globalDepthIndexCount, ignoredStride);
	const uint particleIndex = dispatchThreadId.x;
	if (particleIndex >= min(gSmokeConstants.ParticleCapacity, particleCount))
		return;

	const SmokeParticle particle = gSmokeParticles[particleIndex];
	if (particle.Active == 0u || particle.Epoch != gSmokeConstants.SimulationEpoch)
		return;

	const float3 relativePosition = particle.Position - gSmokeConstants.CameraPosition;
	const float viewDepth = dot(relativePosition, gSmokeConstants.CameraForward);
	const float minimumViewDepth = viewDepth - particle.Radius;
	const float maximumViewDepth = viewDepth + particle.Radius;
	if (maximumViewDepth <= 0.0 || minimumViewDepth >= gSmokeConstants.FroxelMaxDistance)
		return;

	int2 minimumColumn = int2(0, 0);
	int2 maximumColumn = int2(gSmokeConstants.FroxelWidth - 1u, gSmokeConstants.FroxelHeight - 1u);
	if (!SmokeProjectSphereToFroxelBounds(particle.Position, particle.Radius, minimumColumn, maximumColumn))
		return;
	if (any(minimumColumn > maximumColumn))
		return;

	const uint minimumDepthSlice = SmokeDepthSlice(max(minimumViewDepth, 0.0));
	const uint maximumDepthSlice = SmokeDepthSlice(min(maximumViewDepth, gSmokeConstants.FroxelMaxDistance));
	const uint depthSpan = maximumDepthSlice - minimumDepthSlice + 1u;
	const uint fineWidth = (uint)(maximumColumn.x - minimumColumn.x + 1);
	const uint fineHeight = (uint)(maximumColumn.y - minimumColumn.y + 1);
	const uint fineXYReferences = fineWidth * fineHeight;
	const bool useFineTier = fineXYReferences <= NRI_SMOKE_MAX_TIER_REFERENCES / depthSpan;
	const uint2 minimumWideCell = min(uint2(
		(uint)minimumColumn.x * NRI_SMOKE_WIDE_GRID_X / gSmokeConstants.FroxelWidth,
		(uint)minimumColumn.y * NRI_SMOKE_WIDE_GRID_Y / gSmokeConstants.FroxelHeight),
		uint2(NRI_SMOKE_WIDE_GRID_X - 1u, NRI_SMOKE_WIDE_GRID_Y - 1u));
	const uint2 maximumWideCell = min(uint2(
		(uint)maximumColumn.x * NRI_SMOKE_WIDE_GRID_X / gSmokeConstants.FroxelWidth,
		(uint)maximumColumn.y * NRI_SMOKE_WIDE_GRID_Y / gSmokeConstants.FroxelHeight),
		uint2(NRI_SMOKE_WIDE_GRID_X - 1u, NRI_SMOKE_WIDE_GRID_Y - 1u));
	const uint wideXYReferences = (maximumWideCell.x - minimumWideCell.x + 1u) * (maximumWideCell.y - minimumWideCell.y + 1u);
	const bool useWideTier = !useFineTier && wideXYReferences <= NRI_SMOKE_MAX_TIER_REFERENCES / depthSpan;
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u && controlCount > 0u;
	const uint packedCandidate = SmokePackCandidate(particle, particleIndex);
	bool invalidTarget = false;

	if (diagnostics)
	{
		InterlockedMax(gSmokeControl[0].MaximumDepthSpan, depthSpan);
		if (depthSpan == 1u) InterlockedAdd(gSmokeControl[0].DepthSpanOne, 1u);
		else if (depthSpan <= 4u) InterlockedAdd(gSmokeControl[0].DepthSpanTwoToFour, 1u);
		else if (depthSpan <= 16u) InterlockedAdd(gSmokeControl[0].DepthSpanFiveToSixteen, 1u);
		else InterlockedAdd(gSmokeControl[0].DepthSpanOverSixteen, 1u);
	}

	if (useFineTier)
	{
		if (diagnostics) InterlockedAdd(gSmokeControl[0].FineTierParticles, 1u);
		[loop]
		for (uint z = minimumDepthSlice; z <= maximumDepthSlice; ++z)
		{
			[loop]
			for (int y = minimumColumn.y; y <= maximumColumn.y; ++y)
			{
				[loop]
				for (int x = minimumColumn.x; x <= maximumColumn.x; ++x)
				{
					const uint cellIndex = SmokeFroxelIndex((uint)x, (uint)y, z);
					const uint baseIndex = cellIndex * NRI_SMOKE_FINE_CELL_CAPACITY;
					if (cellIndex >= fineCellCount)
					{
						invalidTarget = true;
						continue;
					}
					uint previousCount = 0u;
					InterlockedAdd(gSmokeFineCellCounts[cellIndex], 1u, previousCount);
					bool insertionInvalid = false;
					SmokeInsertTierCandidate(NRI_SMOKE_CANDIDATE_TIER_FINE, baseIndex, NRI_SMOKE_FINE_CELL_CAPACITY,
						fineCellIndexCount, packedCandidate, diagnostics, insertionInvalid);
					invalidTarget = invalidTarget || insertionInvalid;
					if (diagnostics)
					{
						InterlockedAdd(gSmokeControl[0].FineColumnReferences, 1u);
						if (previousCount == 0u) InterlockedAdd(gSmokeControl[0].FineOccupiedCells, 1u);
					}
				}
			}
		}
	}
	else if (useWideTier)
	{
		if (diagnostics)
		{
			InterlockedAdd(gSmokeControl[0].WideParticlesProjected, 1u);
			InterlockedAdd(gSmokeControl[0].WideTierParticles, 1u);
		}
		[loop]
		for (uint z = minimumDepthSlice; z <= maximumDepthSlice; ++z)
		{
			[loop]
			for (uint y = minimumWideCell.y; y <= maximumWideCell.y; ++y)
			{
				[loop]
				for (uint x = minimumWideCell.x; x <= maximumWideCell.x; ++x)
				{
					const uint cellIndex = SmokeWideCellIndex(x, y, z);
					const uint baseIndex = cellIndex * NRI_SMOKE_WIDE_CELL_CAPACITY;
					if (cellIndex >= wideCellCount)
					{
						invalidTarget = true;
						continue;
					}
					uint previousCount = 0u;
					InterlockedAdd(gSmokeWideCellCounts[cellIndex], 1u, previousCount);
					bool insertionInvalid = false;
					SmokeInsertTierCandidate(NRI_SMOKE_CANDIDATE_TIER_WIDE, baseIndex, NRI_SMOKE_WIDE_CELL_CAPACITY,
						wideCellIndexCount, packedCandidate, diagnostics, insertionInvalid);
					invalidTarget = invalidTarget || insertionInvalid;
					if (diagnostics)
					{
						InterlockedAdd(gSmokeControl[0].WideCellReferences, 1u);
						if (previousCount == 0u) InterlockedAdd(gSmokeControl[0].WideOccupiedCells, 1u);
					}
				}
			}
		}
	}
	else
	{
		if (diagnostics) InterlockedAdd(gSmokeControl[0].GlobalTierParticles, 1u);
		[loop]
		for (uint z = minimumDepthSlice; z <= maximumDepthSlice; ++z)
		{
			const uint baseIndex = z * NRI_SMOKE_GLOBAL_DEPTH_CAPACITY;
			if (z >= globalDepthCount)
			{
				invalidTarget = true;
				continue;
			}
			uint previousCount = 0u;
			InterlockedAdd(gSmokeGlobalDepthCounts[z], 1u, previousCount);
			bool insertionInvalid = false;
			SmokeInsertTierCandidate(NRI_SMOKE_CANDIDATE_TIER_GLOBAL, baseIndex, NRI_SMOKE_GLOBAL_DEPTH_CAPACITY,
				globalDepthIndexCount, packedCandidate, diagnostics, insertionInvalid);
			invalidTarget = invalidTarget || insertionInvalid;
			if (diagnostics)
			{
				InterlockedAdd(gSmokeControl[0].GlobalDepthReferences, 1u);
				if (previousCount == 0u) InterlockedAdd(gSmokeControl[0].GlobalOccupiedSlices, 1u);
			}
		}
	}

	if (invalidTarget && controlCount != 0u)
	{
		InterlockedAdd(gSmokeControl[0].ColumnOverflow, 1u);
		if (!useFineTier && !useWideTier) InterlockedAdd(gSmokeControl[0].WideGlobalDrops, 1u);
	}
}
