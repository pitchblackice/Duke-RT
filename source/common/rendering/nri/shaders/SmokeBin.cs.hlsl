#include "Include/SmokeResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (gSmokeConstants.FroxelWidth == 0u || gSmokeConstants.FroxelHeight == 0u || gSmokeConstants.ColumnCapacity == 0u)
		return;

	uint particleCount, controlCount, actualColumnCount, columnIndexCount, ignoredStride;
	gSmokeParticles.GetDimensions(particleCount, ignoredStride);
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeColumnCounts.GetDimensions(actualColumnCount, ignoredStride);
	gSmokeColumnIndices.GetDimensions(columnIndexCount, ignoredStride);
	const uint particleIndex = dispatchThreadId.x;
	if (particleIndex >= min(gSmokeConstants.ParticleCapacity, particleCount))
		return;

	const SmokeParticle particle = gSmokeParticles[particleIndex];
	if (particle.Active == 0u || particle.Epoch != gSmokeConstants.SimulationEpoch)
		return;

	const float3 relativePosition = particle.Position - gSmokeConstants.CameraPosition;
	const float viewDepth = dot(relativePosition, gSmokeConstants.CameraForward);
	if (viewDepth + particle.Radius <= 0.0 || viewDepth - particle.Radius >= gSmokeConstants.FroxelMaxDistance)
		return;
	int2 minimumColumn = int2(0, 0);
	int2 maximumColumn = int2(gSmokeConstants.FroxelWidth - 1u, gSmokeConstants.FroxelHeight - 1u);
	// A sphere crossing the camera plane can cover any screen column. Avoid the
	// unstable projected-center approximation used by the original path.
	if (viewDepth > particle.Radius)
	{
		const float projectionDepth = max(viewDepth, 0.001);
		const float2 ndcCenter = float2(
			dot(relativePosition, gSmokeConstants.CameraRight) / max(projectionDepth * gSmokeConstants.TanHalfFovX, 0.001),
			-dot(relativePosition, gSmokeConstants.CameraUp) / max(projectionDepth * gSmokeConstants.TanHalfFovY, 0.001));
		const float2 uvCenter = ndcCenter * 0.5 + 0.5;
		const float2 uvRadius = float2(
			particle.Radius / max(projectionDepth * gSmokeConstants.TanHalfFovX, 0.001),
			particle.Radius / max(projectionDepth * gSmokeConstants.TanHalfFovY, 0.001)) * 0.5;
		minimumColumn = max(int2(floor((uvCenter - uvRadius) * float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight))), int2(0, 0));
		maximumColumn = min(int2(floor((uvCenter + uvRadius) * float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight))), int2(gSmokeConstants.FroxelWidth - 1u, gSmokeConstants.FroxelHeight - 1u));
	}
	if (any(minimumColumn > maximumColumn))
		return;
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u && controlCount > 0u;
	const uint packedCandidate = SmokePackCandidate(particle, particleIndex);
	const bool wideParticle = maximumColumn.x - minimumColumn.x + 1 > (int)NRI_SMOKE_MAX_BIN_COLUMNS_PER_AXIS ||
		maximumColumn.y - minimumColumn.y + 1 > (int)NRI_SMOKE_MAX_BIN_COLUMNS_PER_AXIS;
	if (wideParticle)
	{
		uint wideCellCount, wideIndexCount;
		gSmokeWideCellCounts.GetDimensions(wideCellCount, ignoredStride);
		gSmokeWideCellIndices.GetDimensions(wideIndexCount, ignoredStride);
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].WideParticlesProjected, 1u);
		const uint2 minimumCell = min(uint2(
			(uint)minimumColumn.x * NRI_SMOKE_WIDE_GRID_X / gSmokeConstants.FroxelWidth,
			(uint)minimumColumn.y * NRI_SMOKE_WIDE_GRID_Y / gSmokeConstants.FroxelHeight),
			uint2(NRI_SMOKE_WIDE_GRID_X - 1u, NRI_SMOKE_WIDE_GRID_Y - 1u));
		const uint2 maximumCell = min(uint2(
			(uint)maximumColumn.x * NRI_SMOKE_WIDE_GRID_X / gSmokeConstants.FroxelWidth,
			(uint)maximumColumn.y * NRI_SMOKE_WIDE_GRID_Y / gSmokeConstants.FroxelHeight),
			uint2(NRI_SMOKE_WIDE_GRID_X - 1u, NRI_SMOKE_WIDE_GRID_Y - 1u));
		const uint bucket = particleIndex % NRI_SMOKE_WIDE_CELL_CAPACITY;
		bool invalidTarget = false;
		[loop]
		for (uint cellY = minimumCell.y; cellY <= maximumCell.y; ++cellY)
		{
			[loop]
			for (uint cellX = minimumCell.x; cellX <= maximumCell.x; ++cellX)
			{
				const uint cellIndex = cellY * NRI_SMOKE_WIDE_GRID_X + cellX;
				const uint targetIndex = cellIndex * NRI_SMOKE_WIDE_CELL_CAPACITY + bucket;
				if (cellIndex >= wideCellCount || targetIndex >= wideIndexCount)
				{
					invalidTarget = true;
					continue;
				}
				InterlockedAdd(gSmokeWideCellCounts[cellIndex], 1u);
				uint previous = 0u;
				InterlockedMax(gSmokeWideCellIndices[targetIndex], packedCandidate, previous);
				if (diagnostics)
				{
					InterlockedAdd(gSmokeControl[0].WideCellReferences, 1u);
					if (previous != 0u) InterlockedAdd(gSmokeControl[0].SelectionCollisions, 1u);
					if (previous != 0u && packedCandidate > previous) InterlockedAdd(gSmokeControl[0].SelectionReplacements, 1u);
				}
			}
		}
		if (invalidTarget && controlCount > 0u)
			InterlockedAdd(gSmokeControl[0].ColumnOverflow, 1u);
		return;
	}

	SmokeLimitColumnRange(minimumColumn.x, maximumColumn.x);
	SmokeLimitColumnRange(minimumColumn.y, maximumColumn.y);

	const uint bucketCount = SmokeSelectionBucketCount();
	if (bucketCount == 0u)
		return;
	const uint bucket = particleIndex % bucketCount;
	bool invalidTarget = false;
	for (int y = minimumColumn.y; y <= maximumColumn.y; ++y)
	{
		for (int x = minimumColumn.x; x <= maximumColumn.x; ++x)
		{
			const uint columnIndex = (uint)y * gSmokeConstants.FroxelWidth + (uint)x;
			if (columnIndex >= actualColumnCount)
				continue;
			InterlockedAdd(gSmokeColumnCounts[columnIndex], 1u);
			const uint targetIndex = columnIndex * gSmokeConstants.ColumnCapacity + bucket;
			if (targetIndex >= columnIndexCount)
			{
				invalidTarget = true;
				continue;
			}
			uint previous = 0u;
			InterlockedMax(gSmokeColumnIndices[targetIndex], packedCandidate, previous);
			if (diagnostics)
			{
				InterlockedAdd(gSmokeControl[0].FineColumnReferences, 1u);
				if (previous != 0u) InterlockedAdd(gSmokeControl[0].SelectionCollisions, 1u);
				if (previous != 0u && packedCandidate > previous) InterlockedAdd(gSmokeControl[0].SelectionReplacements, 1u);
			}
		}
	}
	if (invalidTarget && controlCount != 0u)
		InterlockedAdd(gSmokeControl[0].ColumnOverflow, 1u);
}
