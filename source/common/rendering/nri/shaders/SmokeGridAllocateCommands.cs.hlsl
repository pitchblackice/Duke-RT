#include "Include/SmokeGridResources.hlsli"

#define NRI_SMOKE_GRID_MAX_ADMISSION_SOURCES 256u

groupshared uint gSmokeGridSourceCommandCursor[NRI_SMOKE_GRID_MAX_ADMISSION_SOURCES];
groupshared uint gSmokeGridSourceBrickCursor[NRI_SMOKE_GRID_MAX_ADMISSION_SOURCES];

uint SmokeGridGreatestCommonDivisor(uint a, uint b)
{
	[loop]
	while (b != 0u)
	{
		const uint remainder = a % b;
		a = b;
		b = remainder;
	}
	return a;
}

uint SmokeGridStableFootprintOrdinal(uint ordinal, uint count, uint sourceId)
{
	if (count <= 1u)
		return 0u;
	uint hash = sourceId ^ count ^ 0x9e3779b9u;
	hash ^= hash >> 16u;
	hash *= 0x7feb352du;
	hash ^= hash >> 15u;
	const uint start = hash % count;
	uint stride = ((hash >> 16u) | 1u) % count;
	if (stride == 0u)
		stride = 1u;
	[loop]
	while (SmokeGridGreatestCommonDivisor(stride, count) != 1u)
	{
		stride++;
		if (stride >= count)
			stride = 1u;
	}
	return (start + ordinal * stride) % count;
}

bool SmokeGridCommandFootprint(SmokeInjectionCommand command, SmokeStyle style,
	out int3 minimumBrick, out uint3 brickExtent, out uint brickCount)
{
	const float cellSize = max(gSmokeGridConstants.CellSize, 0.0001);
	const float radius = min(max(max(command.SpawnRadius, style.Radius * command.RadiusScale), cellSize),
		cellSize * 16.0);
	float3 halfAxisU, halfAxisV;
	SmokeInjectionRectangleHalfAxes(command, halfAxisU, halfAxisV);
	const float3 sourceExtent = abs(halfAxisU) + abs(halfAxisV) + radius;
	const int3 minimumCell = (int3)floor((command.Position - sourceExtent) / cellSize);
	const int3 maximumCell = (int3)floor((command.Position + sourceExtent) / cellSize);
	minimumBrick = SmokeGridBrickCoordinate(minimumCell);
	const int3 maximumBrick = SmokeGridBrickCoordinate(maximumCell);
	brickExtent = (uint3)(maximumBrick - minimumBrick + 1);
	if (!SmokeInjectionTraversalFits(brickExtent, 4096u))
	{
		brickCount = 0u;
		return false;
	}
	brickCount = brickExtent.x * brickExtent.y * brickExtent.z;
	return true;
}

int3 SmokeGridFootprintCoordinate(int3 minimumBrick, uint3 brickExtent,
	uint ordinal, uint sourceId)
{
	const uint count = brickExtent.x * brickExtent.y * brickExtent.z;
	const uint stableOrdinal = SmokeGridStableFootprintOrdinal(ordinal, count, sourceId);
	const uint x = stableOrdinal % brickExtent.x;
	const uint y = (stableOrdinal / brickExtent.x) % brickExtent.y;
	const uint z = stableOrdinal / (brickExtent.x * brickExtent.y);
	return minimumBrick + int3(x, y, z);
}

void SmokeGridRecordRejection(uint sourceSlot, uint sourceCapacity, uint reason)
{
	if (sourceSlot < sourceCapacity)
	{
		if (reason == 0u) gSmokeGridSourceStats[sourceSlot].RejectedCapacity++;
		else if (reason == 1u) gSmokeGridSourceStats[sourceSlot].RejectedProbe++;
		else gSmokeGridSourceStats[sourceSlot].RejectedInvalid++;
	}
	gSmokeGridControl[0].AdmissionRejected++;
	if (reason == 0u) gSmokeGridControl[0].AdmissionCapacityRejected++;
	else if (reason == 1u) gSmokeGridControl[0].AdmissionProbeRejected++;
	else gSmokeGridControl[0].AdmissionInvalidRejected++;
}

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x != 0u)
		return;

	uint commandCapacity, styleCapacity, sourceCapacity, ignoredStride;
	gSmokeGridCommands.GetDimensions(commandCapacity, ignoredStride);
	gSmokeGridStyles.GetDimensions(styleCapacity, ignoredStride);
	gSmokeGridSourceStats.GetDimensions(sourceCapacity, ignoredStride);
	const uint validCommandCount = min(gSmokeGridConstants.CommandCount, commandCapacity);
	const uint validStyleCount = min(gSmokeGridConstants.StyleCount, styleCapacity);
	const uint validSourceCapacity = min(sourceCapacity, NRI_SMOKE_GRID_MAX_ADMISSION_SOURCES);
	[loop]
	for (uint sourceSlot = 0u; sourceSlot < validSourceCapacity; ++sourceSlot)
	{
		gSmokeGridSourceStats[sourceSlot] = (SmokeGridSourceStats)0;
		gSmokeGridSourceCommandCursor[sourceSlot] = 0u;
		gSmokeGridSourceBrickCursor[sourceSlot] = 0u;
	}
	gSmokeGridControl[0].AdmissionSourceCount = 0u;
	gSmokeGridControl[0].AdmissionRequested = 0u;
	gSmokeGridControl[0].AdmissionExisting = 0u;
	gSmokeGridControl[0].AdmissionAdmitted = 0u;
	gSmokeGridControl[0].AdmissionRejected = 0u;
	gSmokeGridControl[0].AdmissionCapacityRejected = 0u;
	gSmokeGridControl[0].AdmissionProbeRejected = 0u;
	gSmokeGridControl[0].AdmissionInvalidRejected = 0u;
	gSmokeGridControl[0].Generation = gSmokeGridConstants.SimulationEpoch;
	gSmokeGridControl[0].FrameStamp = gSmokeGridConstants.FrameIndex;

	uint sourceCount = 0u;
	[loop]
	for (uint commandIndex = 0u; commandIndex < validCommandCount; ++commandIndex)
	{
		const SmokeInjectionCommand command = gSmokeGridCommands[commandIndex];
		if (command.Epoch != gSmokeGridConstants.SimulationEpoch || command.StyleIndex >= validStyleCount ||
			command.SourceSlot >= validSourceCapacity)
		{
			SmokeGridRecordRejection(command.SourceSlot, validSourceCapacity, 2u);
			continue;
		}
		SmokeGridSourceStats stats = gSmokeGridSourceStats[command.SourceSlot];
		if (stats.Commands != 0u && stats.SourceId != command.SourceId)
		{
			SmokeGridRecordRejection(command.SourceSlot, validSourceCapacity, 2u);
			continue;
		}
		stats.SourceId = command.SourceId;
		stats.SourceClass = SmokeInjectionSourceClass(command);
		stats.Priority = SmokeInjectionSourcePriority(command);
		stats.Commands++;
		const SmokeStyle style = gSmokeGridStyles[command.StyleIndex];
		const float requestedMass = max(style.Density * command.DensityScale, 0.0) *
			(float)min(command.Count, 256u);
		const uint requestedMassQ = (uint)min(requestedMass *
			gSmokeGridConstants.MassQuantization, 4294967295.0);
		stats.RequestedMassQ += requestedMassQ;
		gSmokeGridSourceStats[command.SourceSlot] = stats;
		gSmokeGridControl[0].RequestedMassQ += requestedMassQ;
		gSmokeGridControl[0].CommandsProcessed++;
		sourceCount = max(sourceCount, command.SourceSlot + 1u);
	}
	gSmokeGridControl[0].AdmissionSourceCount = sourceCount;

	// A round permits at most one new-key decision per source. Resident hits are
	// handled before capacity and do not consume the source's turn.
	const uint maximumRounds = validCommandCount * 4096u;
	[loop]
	for (uint round = 0u; round < maximumRounds; ++round)
	{
		bool madeProgress = false;
		[loop]
		for (uint sourceOrdinal = 0u; sourceOrdinal < sourceCount; ++sourceOrdinal)
		{
			const uint sourceSlot = (sourceOrdinal + gSmokeGridConstants.FrameIndex + round) %
				max(sourceCount, 1u);
			if (sourceSlot >= validSourceCapacity || gSmokeGridSourceStats[sourceSlot].Commands == 0u)
				continue;
			bool completedTurn = false;
			[loop]
			while (!completedTurn && gSmokeGridSourceCommandCursor[sourceSlot] < validCommandCount)
			{
				const uint commandIndex = gSmokeGridSourceCommandCursor[sourceSlot];
				const SmokeInjectionCommand command = gSmokeGridCommands[commandIndex];
				if (command.Epoch != gSmokeGridConstants.SimulationEpoch ||
					command.StyleIndex >= validStyleCount || command.SourceSlot != sourceSlot)
				{
					gSmokeGridSourceCommandCursor[sourceSlot]++;
					gSmokeGridSourceBrickCursor[sourceSlot] = 0u;
					continue;
				}
				int3 minimumBrick;
				uint3 brickExtent;
				uint brickCount;
				if (!SmokeGridCommandFootprint(command, gSmokeGridStyles[command.StyleIndex],
					minimumBrick, brickExtent, brickCount))
				{
					SmokeGridRecordRejection(sourceSlot, validSourceCapacity, 2u);
					gSmokeGridSourceCommandCursor[sourceSlot]++;
					gSmokeGridSourceBrickCursor[sourceSlot] = 0u;
					continue;
				}
				if (gSmokeGridSourceBrickCursor[sourceSlot] >= brickCount)
				{
					gSmokeGridSourceCommandCursor[sourceSlot]++;
					gSmokeGridSourceBrickCursor[sourceSlot] = 0u;
					continue;
				}

				const uint footprintOrdinal = gSmokeGridSourceBrickCursor[sourceSlot]++;
				const int3 coordinate = SmokeGridFootprintCoordinate(minimumBrick, brickExtent,
					footprintOrdinal, command.SourceId);
				gSmokeGridSourceStats[sourceSlot].RequestedBricks++;
				gSmokeGridControl[0].AdmissionRequested++;
				madeProgress = true;
				uint brickIndex;
				if (SmokeGridFindBrickSerial(coordinate, brickIndex))
				{
					gSmokeGridSourceStats[sourceSlot].ExistingHits++;
					gSmokeGridControl[0].AdmissionExisting++;
					gSmokeGridBricks[brickIndex].Flags |= NRI_SMOKE_GRID_BRICK_CONTENT;
					gSmokeGridBricks[brickIndex].IdleFrames = 0u;
					continue;
				}

				if (gSmokeGridControl[0].FreeCount == 0u)
				{
					SmokeGridRecordRejection(sourceSlot, validSourceCapacity, 0u);
					completedTurn = true;
					continue;
				}
				bool newlyAllocated = false;
				if (SmokeGridFindOrAllocateBrickSerial(coordinate, NRI_SMOKE_GRID_BRICK_CONTENT,
					brickIndex, newlyAllocated) && newlyAllocated)
				{
					gSmokeGridSourceStats[sourceSlot].AdmittedNew++;
					gSmokeGridSourceStats[sourceSlot].AdmittedKeyHash ^=
						SmokeGridHashCoordinate(coordinate);
					gSmokeGridControl[0].AdmissionAdmitted++;
				}
				else
				{
					SmokeGridRecordRejection(sourceSlot, validSourceCapacity,
						gSmokeGridControl[0].FreeCount == 0u ? 0u : 1u);
				}
				completedTurn = true;
			}
		}
		if (!madeProgress)
			break;
	}
}
