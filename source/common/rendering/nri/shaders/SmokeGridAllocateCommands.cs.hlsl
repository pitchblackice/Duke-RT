#include "Include/SmokeGridResources.hlsli"

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x != 0u)
		return;

	uint commandCapacity, styleCapacity, ignoredStride;
	gSmokeGridCommands.GetDimensions(commandCapacity, ignoredStride);
	gSmokeGridStyles.GetDimensions(styleCapacity, ignoredStride);
	const uint validCommandCount = min(gSmokeGridConstants.CommandCount, commandCapacity);
	const uint validStyleCount = min(gSmokeGridConstants.StyleCount, styleCapacity);
	const float cellSize = max(gSmokeGridConstants.CellSize, 0.0001);
	[loop]
	for (uint commandIndex = 0u; commandIndex < validCommandCount; ++commandIndex)
	{
		const SmokeInjectionCommand command = gSmokeGridCommands[commandIndex];
		if (command.Epoch != gSmokeGridConstants.SimulationEpoch || command.StyleIndex >= validStyleCount)
			continue;
		const SmokeStyle style = gSmokeGridStyles[command.StyleIndex];
		const float radius = min(max(max(command.SpawnRadius, style.Radius * command.RadiusScale), cellSize), cellSize * 16.0);
		float3 halfAxisU, halfAxisV;
		SmokeInjectionRectangleHalfAxes(command, halfAxisU, halfAxisV);
		const float3 sourceExtent = abs(halfAxisU) + abs(halfAxisV) + radius;
		const int3 minimumCell = (int3)floor((command.Position - sourceExtent) / cellSize);
		const int3 maximumCell = (int3)floor((command.Position + sourceExtent) / cellSize);
		const int3 minimumBrick = SmokeGridBrickCoordinate(minimumCell);
		const int3 maximumBrick = SmokeGridBrickCoordinate(maximumCell);
		[loop] for (int z = minimumBrick.z; z <= maximumBrick.z; ++z)
		[loop] for (int y = minimumBrick.y; y <= maximumBrick.y; ++y)
		[loop] for (int x = minimumBrick.x; x <= maximumBrick.x; ++x)
		{
			uint brickIndex;
			bool newlyAllocated;
			SmokeGridFindOrAllocateBrickSerial(int3(x, y, z), NRI_SMOKE_GRID_BRICK_CONTENT, brickIndex, newlyAllocated);
		}
		const float requestedMass = max(style.Density * command.DensityScale, 0.0) * (float)min(command.Count, 256u);
		gSmokeGridControl[0].RequestedMassQ += (uint)min(requestedMass * gSmokeGridConstants.MassQuantization, 4294967295.0);
		gSmokeGridControl[0].CommandsProcessed++;
	}
}
