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
	const float projectionDepth = max(viewDepth, max(particle.Radius * 0.25, 0.001));

	const float2 ndcCenter = float2(
		dot(relativePosition, gSmokeConstants.CameraRight) / max(projectionDepth * gSmokeConstants.TanHalfFovX, 0.001),
		-dot(relativePosition, gSmokeConstants.CameraUp) / max(projectionDepth * gSmokeConstants.TanHalfFovY, 0.001));
	const float2 uvCenter = ndcCenter * 0.5 + 0.5;
	const float2 uvRadius = float2(
		particle.Radius / max(projectionDepth * gSmokeConstants.TanHalfFovX, 0.001),
		particle.Radius / max(projectionDepth * gSmokeConstants.TanHalfFovY, 0.001)) * 0.5;
	int2 minimumColumn = max(int2(floor((uvCenter - uvRadius) * float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight))), int2(0, 0));
	int2 maximumColumn = min(int2(floor((uvCenter + uvRadius) * float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight))), int2(gSmokeConstants.FroxelWidth - 1u, gSmokeConstants.FroxelHeight - 1u));
	if (any(minimumColumn > maximumColumn))
		return;
	SmokeLimitColumnRange(minimumColumn.x, maximumColumn.x);
	SmokeLimitColumnRange(minimumColumn.y, maximumColumn.y);

	bool particleOverflow = false;
	for (int y = minimumColumn.y; y <= maximumColumn.y; ++y)
	{
		for (int x = minimumColumn.x; x <= maximumColumn.x; ++x)
		{
			const uint columnIndex = (uint)y * gSmokeConstants.FroxelWidth + (uint)x;
			if (columnIndex >= actualColumnCount)
				continue;
			uint listIndex = 0u;
			InterlockedAdd(gSmokeColumnCounts[columnIndex], 1u, listIndex);
			const uint targetIndex = columnIndex * gSmokeConstants.ColumnCapacity + listIndex;
			if (listIndex < gSmokeConstants.ColumnCapacity && targetIndex < columnIndexCount)
				gSmokeColumnIndices[targetIndex] = particleIndex;
			else
				particleOverflow = true;
		}
	}
	if (particleOverflow && controlCount != 0u)
		InterlockedAdd(gSmokeControl[0].ColumnOverflow, 1u);
}
