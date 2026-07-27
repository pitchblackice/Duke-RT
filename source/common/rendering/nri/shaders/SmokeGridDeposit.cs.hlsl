#include "Include/SmokeGridResources.hlsli"

int SmokeGridQuantize(float value, float scale)
{
	if (!isfinite(value) || !isfinite(scale) || scale <= 0.0)
		return 0;
	const float scaled = clamp(value * scale, -1073741824.0, 1073741824.0);
	return (int)round(scaled);
}

[numthreads(64, 1, 1)]
void main(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
	uint commandCapacity, styleCapacity, ignoredStride;
	gSmokeGridCommands.GetDimensions(commandCapacity, ignoredStride);
	gSmokeGridStyles.GetDimensions(styleCapacity, ignoredStride);
	const uint commandIndex = groupId.x;
	if (commandIndex >= min(gSmokeGridConstants.CommandCount, commandCapacity))
		return;
	const SmokeInjectionCommand command = gSmokeGridCommands[commandIndex];
	if (command.Epoch != gSmokeGridConstants.SimulationEpoch || command.StyleIndex >= min(gSmokeGridConstants.StyleCount, styleCapacity))
		return;
	const SmokeStyle style = gSmokeGridStyles[command.StyleIndex];
	uint sourceCapacity, sourceStride;
	gSmokeGridSourceStats.GetDimensions(sourceCapacity, sourceStride);
	const bool sourceStatsValid = command.SourceSlot < sourceCapacity &&
		gSmokeGridSourceStats[command.SourceSlot].SourceId == command.SourceId;
	const float radius = min(max(max(command.SpawnRadius, style.Radius * command.RadiusScale), gSmokeGridConstants.CellSize),
		gSmokeGridConstants.CellSize * 16.0);
	float3 halfAxisU, halfAxisV;
	SmokeInjectionRectangleHalfAxes(command, halfAxisU, halfAxisV);
	const float3 sourceExtent = abs(halfAxisU) + abs(halfAxisV) + radius;
	const int3 minimumCell = (int3)floor((command.Position - sourceExtent) / gSmokeGridConstants.CellSize);
	const int3 maximumCell = (int3)floor((command.Position + sourceExtent) / gSmokeGridConstants.CellSize);
	const uint3 extent = (uint3)(maximumCell - minimumCell + 1);
	if (!SmokeInjectionTraversalFits(extent, 262144u))
		return;
	const uint cellCount = extent.x * extent.y * extent.z;
	const float commandMass = max(style.Density * command.DensityScale, 0.0) * (float)min(command.RangeCount, 256u);
	const float inverseCellSize = rcp(max(gSmokeGridConstants.CellSize, 0.0001));
	const float radiusCells = radius * inverseCellSize;
	const float halfUCells = length(halfAxisU) * inverseCellSize;
	const float halfVCells = length(halfAxisV) * inverseCellSize;
	const float rectangleAreaCells = 4.0 * halfUCells * halfVCells;
	const float rectanglePerimeterCells = 4.0 * (halfUCells + halfVCells);
	const float kernelNormalization = max(1.0,
		rectangleAreaCells * radiusCells +
		(3.0 * 3.14159265359 / 20.0) * rectanglePerimeterCells * radiusCells * radiusCells +
		(4.0 * 3.14159265359 / 15.0) * radiusCells * radiusCells * radiusCells);
	[loop]
	for (uint ordinal = groupThreadId.x; ordinal < cellCount; ordinal += 64u)
	{
		const uint x = ordinal % extent.x;
		const uint y = (ordinal / extent.x) % extent.y;
		const uint z = ordinal / (extent.x * extent.y);
		const int3 cell = minimumCell + int3(x, y, z);
		const float3 cellPosition = ((float3)cell + 0.5) * gSmokeGridConstants.CellSize;
		const float3 closestSourcePosition = SmokeInjectionClosestRectanglePoint(
			cellPosition, command.Position, halfAxisU, halfAxisV);
		const float distanceToSource = distance(cellPosition, closestSourcePosition);
		const float normalized = saturate(1.0 - distanceToSource / max(radius, 1e-4));
		const float kernel = normalized * normalized * (3.0 - 2.0 * normalized);
		if (kernel <= 0.0)
			continue;
		const float mass = commandMass * kernel / kernelNormalization;
		const uint massQ = (uint)max(SmokeGridQuantize(mass,
			gSmokeGridConstants.MassQuantization), 0);
		const int3 brickCoordinate = SmokeGridBrickCoordinate(cell);
		uint brickIndex;
		if (!SmokeGridLookupBrick(brickCoordinate, brickIndex))
		{
			InterlockedAdd(gSmokeGridControl[0].DepositionRejected, 1u);
			InterlockedAdd(gSmokeGridControl[0].RejectedMassQ,
				massQ);
			if (sourceStatsValid)
				InterlockedAdd(gSmokeGridSourceStats[command.SourceSlot].RejectedMassQ, massQ);
			continue;
		}
		const uint cellIndex = SmokeGridCellIndex(brickIndex, SmokeGridLocalCoordinate(cell, brickCoordinate));
		const float extinction = mass * max(style.Extinction, 0.0);
		const float3 scattering = extinction * saturate(style.Albedo);
		const float anisotropyWeight = dot(scattering, float3(0.2126, 0.7152, 0.0722));
		const float densityRate = 0.69314718056 /
			max(SmokeSourceFinite(style.DensityHalfLife, 0.001), 0.001);
		const float coolingRate = 0.69314718056 /
			max(SmokeSourceFinite(style.CoolingHalfLife, 0.001), 0.001);
		uint randomState = SmokeSourceCellSeed(command.Serial, cell);
		const float3 randomDirection = SmokeSourceRandomDirection(randomState);
		const float3 velocityAxis = SmokeInjectionVelocityAxis(command, halfAxisU, halfAxisV);
		const float3 velocityDirection = SmokeSourceVelocityDirection(velocityAxis,
			command.VelocityCone, randomDirection, randomState);
		const float3 radialOffset = cellPosition - SmokeSourceFinite3(closestSourcePosition, cellPosition);
		const float radialLengthSquared = dot(radialOffset, radialOffset);
		const float3 expansionDirection = radialLengthSquared > 1e-8 ?
			radialOffset * rsqrt(radialLengthSquared) : randomDirection;
		const float3 inheritedVelocity = SmokeSourceFinite3(command.Velocity, 0.0) *
			SmokeSourceFinite(style.VelocityInherit, 0.0) *
			SmokeSourceFinite(style.MomentumScale, 1.0);
		const float3 stochasticVelocity = velocityDirection *
			SmokeSourceFinite(style.VelocityRandom, 0.0);
		const float3 expansionVelocity = expansionDirection *
			SmokeSourceFinite(style.ExpansionVelocity, 0.0);
		const float3 riseVelocity = float3(0.0,
			SmokeSourceFinite(style.RiseVelocity, 0.0), 0.0);
		const float3 injectionVelocity = SmokeSourceLimitVelocity(inheritedVelocity +
			stochasticVelocity + expansionVelocity + riseVelocity, gSmokeGridConstants.MaxVelocity);
		const float3 momentum = injectionVelocity * mass;
		int original;
		InterlockedAdd(gSmokeGridDeposit0[cellIndex].x, SmokeGridQuantize(mass, gSmokeGridConstants.MassQuantization), original);
		// Store thermal buoyancy as one moment. This preserves the authored
		// single-style force and makes mixed-style deposition linear.
		InterlockedAdd(gSmokeGridDeposit0[cellIndex].y, SmokeGridQuantize(mass *
			max(SmokeSourceFinite(style.Temperature, 0.0), 0.0) *
			max(SmokeSourceFinite(style.Buoyancy, 0.0), 0.0),
			gSmokeGridConstants.MassQuantization), original);
		InterlockedAdd(gSmokeGridDeposit0[cellIndex].z, SmokeGridQuantize(extinction, gSmokeGridConstants.MassQuantization), original);
		InterlockedAdd(gSmokeGridDeposit0[cellIndex].w, SmokeGridQuantize(anisotropyWeight * clamp(style.Anisotropy, -0.95, 0.95), gSmokeGridConstants.MassQuantization), original);
		InterlockedAdd(gSmokeGridDeposit1[cellIndex].x, SmokeGridQuantize(momentum.x, gSmokeGridConstants.MomentumQuantization), original);
		InterlockedAdd(gSmokeGridDeposit1[cellIndex].y, SmokeGridQuantize(momentum.y, gSmokeGridConstants.MomentumQuantization), original);
		InterlockedAdd(gSmokeGridDeposit1[cellIndex].z, SmokeGridQuantize(momentum.z, gSmokeGridConstants.MomentumQuantization), original);
		InterlockedAdd(gSmokeGridDeposit1[cellIndex].w, SmokeGridQuantize(mass * densityRate, gSmokeGridConstants.MassQuantization), original);
		InterlockedAdd(gSmokeGridDeposit2[cellIndex].x, SmokeGridQuantize(scattering.x, gSmokeGridConstants.MassQuantization), original);
		InterlockedAdd(gSmokeGridDeposit2[cellIndex].y, SmokeGridQuantize(scattering.y, gSmokeGridConstants.MassQuantization), original);
		InterlockedAdd(gSmokeGridDeposit2[cellIndex].z, SmokeGridQuantize(scattering.z, gSmokeGridConstants.MassQuantization), original);
		InterlockedAdd(gSmokeGridDeposit2[cellIndex].w, SmokeGridQuantize(anisotropyWeight, gSmokeGridConstants.MassQuantization), original);
		InterlockedAdd(gSmokeGridDeposit3[cellIndex].x, SmokeGridQuantize(mass * coolingRate, gSmokeGridConstants.MassQuantization), original);
		InterlockedAdd(gSmokeGridDeposit3[cellIndex].y, SmokeGridQuantize(mass *
			max(SmokeSourceFinite(style.Turbulence, 0.0), 0.0),
			gSmokeGridConstants.MassQuantization), original);
		InterlockedAdd(gSmokeGridDeposit3[cellIndex].z, SmokeGridQuantize(mass *
			max(SmokeSourceFinite(style.Drag, 0.0), 0.0),
			gSmokeGridConstants.MassQuantization), original);
		InterlockedAdd(gSmokeGridDeposit3[cellIndex].w, SmokeGridQuantize(mass *
			max(abs(SmokeSourceFinite(style.TurbulenceScale, gSmokeGridConstants.CellSize)), 0.0001),
			gSmokeGridConstants.MassQuantization), original);
		InterlockedAdd(gSmokeGridControl[0].DepositedMassQ, massQ);
		InterlockedAdd(gSmokeGridControl[0].DepositionCells, 1u);
		if (sourceStatsValid)
		{
			InterlockedAdd(gSmokeGridSourceStats[command.SourceSlot].DepositedMassQ, massQ);
			InterlockedAdd(gSmokeGridSourceStats[command.SourceSlot].DepositionCells, 1u);
		}
	}
}
