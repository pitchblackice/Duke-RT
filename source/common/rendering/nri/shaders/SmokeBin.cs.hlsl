#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"

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
		const uint bucket = particleIndex % NRI_SMOKE_FINE_CELL_CAPACITY;
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
					const uint targetIndex = cellIndex * NRI_SMOKE_FINE_CELL_CAPACITY + bucket;
					if (cellIndex >= fineCellCount || targetIndex >= fineCellIndexCount)
					{
						invalidTarget = true;
						continue;
					}
					uint previousCount = 0u;
					InterlockedAdd(gSmokeFineCellCounts[cellIndex], 1u, previousCount);
					uint previous = 0u;
					InterlockedMax(gSmokeFineCellIndices[targetIndex], packedCandidate, previous);
					if (diagnostics)
					{
						InterlockedAdd(gSmokeControl[0].FineColumnReferences, 1u);
						if (previousCount == 0u) InterlockedAdd(gSmokeControl[0].FineOccupiedCells, 1u);
						if (previous != 0u)
						{
							InterlockedAdd(gSmokeControl[0].SelectionCollisions, 1u);
							InterlockedAdd(gSmokeControl[0].FineSelectionCollisions, 1u);
							InterlockedAdd(gSmokeControl[0].FineSelectionLosses, 1u);
							if (packedCandidate > previous)
							{
								InterlockedAdd(gSmokeControl[0].SelectionReplacements, 1u);
								InterlockedAdd(gSmokeControl[0].FineSelectionReplacements, 1u);
							}
						}
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
		const uint bucket = particleIndex % NRI_SMOKE_WIDE_CELL_CAPACITY;
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
					const uint targetIndex = cellIndex * NRI_SMOKE_WIDE_CELL_CAPACITY + bucket;
					if (cellIndex >= wideCellCount || targetIndex >= wideCellIndexCount)
					{
						invalidTarget = true;
						continue;
					}
					uint previousCount = 0u;
					InterlockedAdd(gSmokeWideCellCounts[cellIndex], 1u, previousCount);
					uint previous = 0u;
					InterlockedMax(gSmokeWideCellIndices[targetIndex], packedCandidate, previous);
					if (diagnostics)
					{
						InterlockedAdd(gSmokeControl[0].WideCellReferences, 1u);
						if (previousCount == 0u) InterlockedAdd(gSmokeControl[0].WideOccupiedCells, 1u);
						if (previous != 0u)
						{
							InterlockedAdd(gSmokeControl[0].SelectionCollisions, 1u);
							InterlockedAdd(gSmokeControl[0].WideSelectionCollisions, 1u);
							InterlockedAdd(gSmokeControl[0].WideSelectionLosses, 1u);
							if (packedCandidate > previous)
							{
								InterlockedAdd(gSmokeControl[0].SelectionReplacements, 1u);
								InterlockedAdd(gSmokeControl[0].WideSelectionReplacements, 1u);
							}
						}
					}
				}
			}
		}
	}
	else
	{
		if (diagnostics) InterlockedAdd(gSmokeControl[0].GlobalTierParticles, 1u);
		const uint bucket = particleIndex % NRI_SMOKE_GLOBAL_DEPTH_CAPACITY;
		[loop]
		for (uint z = minimumDepthSlice; z <= maximumDepthSlice; ++z)
		{
			const uint targetIndex = z * NRI_SMOKE_GLOBAL_DEPTH_CAPACITY + bucket;
			if (z >= globalDepthCount || targetIndex >= globalDepthIndexCount)
			{
				invalidTarget = true;
				continue;
			}
			uint previousCount = 0u;
			InterlockedAdd(gSmokeGlobalDepthCounts[z], 1u, previousCount);
			uint previous = 0u;
			InterlockedMax(gSmokeGlobalDepthIndices[targetIndex], packedCandidate, previous);
			if (diagnostics)
			{
				InterlockedAdd(gSmokeControl[0].GlobalDepthReferences, 1u);
				if (previousCount == 0u) InterlockedAdd(gSmokeControl[0].GlobalOccupiedSlices, 1u);
				if (previous != 0u)
				{
					InterlockedAdd(gSmokeControl[0].SelectionCollisions, 1u);
					InterlockedAdd(gSmokeControl[0].GlobalSelectionCollisions, 1u);
					InterlockedAdd(gSmokeControl[0].GlobalSelectionLosses, 1u);
					if (packedCandidate > previous)
					{
						InterlockedAdd(gSmokeControl[0].SelectionReplacements, 1u);
						InterlockedAdd(gSmokeControl[0].GlobalSelectionReplacements, 1u);
					}
				}
			}
		}
	}

	if (invalidTarget && controlCount != 0u)
	{
		InterlockedAdd(gSmokeControl[0].ColumnOverflow, 1u);
		if (!useFineTier && !useWideTier) InterlockedAdd(gSmokeControl[0].WideGlobalDrops, 1u);
	}
}
