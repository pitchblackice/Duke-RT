#include "Include/SmokeGridResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
	const uint commandIndex = groupId.x;
	uint commandCapacity, styleCapacity, ignoredStride;
	gSmokeGridCommands.GetDimensions(commandCapacity, ignoredStride);
	gSmokeGridStyles.GetDimensions(styleCapacity, ignoredStride);
	if (commandIndex >= min(gSmokeGridConstants.CommandCount, commandCapacity)) return;
	const SmokeInjectionCommand command = gSmokeGridCommands[commandIndex];
	if (!SmokeInjectionPromptEligible(command)) return;
	const uint slot = SmokeInjectionPromptSlot(command);
	if (slot >= NRI_SMOKE_PROMPT_FALLBACK_QUANTITY ||
		gSmokePromptOutcomes[slot].Outcome != NRI_SMOKE_PROMPT_OUTCOME_GRID_NEW ||
		gSmokePromptOutcomes[slot].CommandIndex != commandIndex) return;
	if (groupThreadId.x == 0u)
	{
		gSmokePromptOutcomes[slot].RequestedBricks = 0u;
		gSmokePromptOutcomes[slot].AdmittedBricks = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	if (command.Epoch != gSmokeGridConstants.SimulationEpoch || command.StyleIndex >= min(gSmokeGridConstants.StyleCount, styleCapacity))
	{
		if (groupThreadId.x == 0u) gSmokePromptOutcomes[slot].Outcome = NRI_SMOKE_PROMPT_OUTCOME_FALLBACK;
		return;
	}
	const SmokeStyle style = gSmokeGridStyles[command.StyleIndex];
	const float radius = min(max(max(command.SpawnRadius, style.Radius * command.RadiusScale),
		gSmokeGridConstants.CellSize), gSmokeGridConstants.CellSize * 16.0);
	float3 halfAxisU, halfAxisV;
	SmokeInjectionRectangleHalfAxes(command, halfAxisU, halfAxisV);
	const float3 sourceExtent = abs(halfAxisU) + abs(halfAxisV) + radius;
	const int3 minimumCell = (int3)floor((command.Position - sourceExtent) / gSmokeGridConstants.CellSize);
	const int3 maximumCell = (int3)floor((command.Position + sourceExtent) / gSmokeGridConstants.CellSize);
	const uint3 extent = (uint3)(maximumCell - minimumCell + 1);
	if (!SmokeInjectionTraversalFits(extent, 262144u))
	{
		if (groupThreadId.x == 0u) gSmokePromptOutcomes[slot].Outcome = NRI_SMOKE_PROMPT_OUTCOME_FALLBACK;
		return;
	}
	const uint cellCount = extent.x * extent.y * extent.z;
	[loop]
	for (uint ordinal = groupThreadId.x; ordinal < cellCount; ordinal += 64u)
	{
		const uint x = ordinal % extent.x;
		const uint y = (ordinal / extent.x) % extent.y;
		const uint z = ordinal / (extent.x * extent.y);
		const int3 cell = minimumCell + int3(x, y, z);
		const float3 position = ((float3)cell + 0.5) * gSmokeGridConstants.CellSize;
		const float3 closest = SmokeInjectionClosestRectanglePoint(position, command.Position, halfAxisU, halfAxisV);
		const float normalized = saturate(1.0 - distance(position, closest) / max(radius, 1e-4));
		if (normalized <= 0.0) continue;
		InterlockedAdd(gSmokePromptOutcomes[slot].RequestedBricks, 1u);
		uint brickIndex;
		if (SmokeGridLookupBrick(SmokeGridBrickCoordinate(cell), brickIndex))
			InterlockedAdd(gSmokePromptOutcomes[slot].AdmittedBricks, 1u);
	}
}
