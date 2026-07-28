#include "Include/SmokeDormantGridResources.hlsli"

groupshared uint sFineIndex;
groupshared uint sArchiveIndex;
groupshared uint sArchiveHashSlot;
groupshared uint sArchiveGeneration;
groupshared uint sValid;
groupshared uint sInvalidPayload;
groupshared uint sMassQ;

void RetainFine(uint workIndex, SmokeDormantGridWork work, uint outcome)
{
	SmokeDormantGridResult result = (SmokeDormantGridResult)0;
	result.Coordinate = work.Coordinate;
	result.InputGeneration = work.Generation;
	result.Outcome = outcome;
	result.ArchiveIndex = 0xffffffffu;
	result.FineIndex = sFineIndex;
	gDormantResults[workIndex] = result;
	gDormantControl[0].ArchiveRetainedFine++;
}

[numthreads(64, 1, 1)]
void main(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
	const uint workIndex = groupId.x;
	if (workIndex >= gDormantConstants.DemotionCount)
		return;
	const SmokeDormantGridWork work = gDormantDemotions[workIndex];
	if (groupThreadId.x == 0u)
	{
		sFineIndex = sArchiveIndex = sArchiveHashSlot = 0xffffffffu;
		sArchiveGeneration = 0u;
		sValid = 0u;
		sInvalidPayload = 0u;
		sMassQ = 0u;
		if (workIndex == 0u) gDormantControl[0].FrameIndex = gDormantConstants.FrameIndex;
		gDormantControl[0].ArchiveAttempts++;
		gDormantControl[0].ArchiveWorkExecuted++;
		if (work.Epoch != gDormantConstants.SimulationEpoch ||
			gDormantControl[0].Epoch != gDormantConstants.SimulationEpoch)
		{
			RetainFine(workIndex, work, NRI_SMOKE_DORMANT_OUTCOME_STALE_EPOCH);
			gDormantControl[0].ArchiveStale++;
		}
		else if (work.Generation == 0u || gDormantConstants.FineHashCapacity == 0u)
		{
			RetainFine(workIndex, work, NRI_SMOKE_DORMANT_OUTCOME_STALE_GENERATION);
			gDormantControl[0].ArchiveStale++;
		}
		else
		{
			const uint fineMask = gDormantConstants.FineHashCapacity - 1u;
			const uint base = SmokeDormantGridHashCoordinate(work.Coordinate) & fineMask;
			[loop]
			for (uint probe = 0u; probe < min(NRI_SMOKE_DORMANT_GRID_HASH_PROBES,
				gDormantConstants.FineHashCapacity); ++probe)
			{
				const SmokeGridHashEntry entry = gDormantFineHash[(base + probe) & fineMask];
				if (entry.State == NRI_SMOKE_GRID_EMPTY) break;
				if (entry.State != NRI_SMOKE_GRID_RESIDENT || entry.Generation != work.Generation ||
					!all(entry.Coordinate == work.Coordinate) ||
					entry.BrickIndex >= gDormantConstants.FineBrickCapacity) continue;
				const SmokeGridBrick brick = gDormantFineBricks[entry.BrickIndex];
				if (brick.State == NRI_SMOKE_GRID_RESIDENT && brick.Generation == work.Generation &&
					brick.HashSlot == ((base + probe) & fineMask) && all(brick.Coordinate == work.Coordinate))
					sFineIndex = entry.BrickIndex;
				break;
			}
			if (sFineIndex == 0xffffffffu)
			{
				RetainFine(workIndex, work, NRI_SMOKE_DORMANT_OUTCOME_STALE_GENERATION);
				gDormantControl[0].ArchiveStale++;
			}
			else if (!DormantPopArchiveFree(sArchiveIndex))
			{
				RetainFine(workIndex, work, NRI_SMOKE_DORMANT_OUTCOME_ARCHIVE_FULL);
				gDormantControl[0].ArchiveFull++;
			}
			else
			{
				const uint archiveMask = gDormantConstants.ArchiveHashCapacity - 1u;
				const uint archiveBase = SmokeDormantGridHashCoordinate(work.Coordinate) & archiveMask;
				bool coordinateConflict = false;
				[loop]
				for (uint probe = 0u; probe < min(NRI_SMOKE_DORMANT_GRID_HASH_PROBES,
					gDormantConstants.ArchiveHashCapacity); ++probe)
				{
					const uint slot = (archiveBase + probe) & archiveMask;
					const SmokeDormantGridHashEntry existing = gDormantHash[slot];
					if ((existing.State == NRI_SMOKE_DORMANT_RESIDENT ||
						existing.State == NRI_SMOKE_DORMANT_CLAIMED) &&
						all(existing.Coordinate == work.Coordinate))
					{
						coordinateConflict = true;
						break;
					}
					uint original;
					InterlockedCompareExchange(gDormantHash[slot].State,
						NRI_SMOKE_DORMANT_EMPTY, NRI_SMOKE_DORMANT_CLAIMED, original);
					if (original == NRI_SMOKE_DORMANT_TOMBSTONE)
						InterlockedCompareExchange(gDormantHash[slot].State,
							NRI_SMOKE_DORMANT_TOMBSTONE, NRI_SMOKE_DORMANT_CLAIMED, original);
					if (original == NRI_SMOKE_DORMANT_EMPTY || original == NRI_SMOKE_DORMANT_TOMBSTONE)
					{
						sArchiveHashSlot = slot;
						gDormantControl[0].MaximumArchiveProbe = max(
							gDormantControl[0].MaximumArchiveProbe, probe + 1u);
						break;
					}
				}
				if (coordinateConflict || sArchiveHashSlot == 0xffffffffu)
				{
					DormantPushArchiveFree(sArchiveIndex);
					RetainFine(workIndex, work, NRI_SMOKE_DORMANT_OUTCOME_HASH_FAILURE);
					gDormantControl[0].ArchiveHashFailures++;
				}
				else
				{
					SmokeDormantGridRecord prior = gDormantRecords[sArchiveIndex];
					sArchiveGeneration = prior.Generation + 1u;
					if (sArchiveGeneration == 0u) sArchiveGeneration = 1u;
					SmokeDormantGridRecord record = (SmokeDormantGridRecord)0;
					record.Coordinate = work.Coordinate;
					record.HashSlot = sArchiveHashSlot;
					record.Generation = sArchiveGeneration;
					record.State = NRI_SMOKE_DORMANT_CLAIMED;
					record.FineGeneration = work.Generation;
					record.Epoch = work.Epoch;
					record.LastSimulationFrame = work.LastSimulationFrame;
					gDormantRecords[sArchiveIndex] = record;
					gDormantHash[sArchiveHashSlot].Coordinate = work.Coordinate;
					gDormantHash[sArchiveHashSlot].ArchiveIndex = sArchiveIndex;
					gDormantHash[sArchiveHashSlot].Generation = sArchiveGeneration;
					sValid = 1u;
				}
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (sValid == 0u) return;

	const uint fineBase = sFineIndex * NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK;
	const uint archiveBase = sArchiveIndex * NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK;
	for (uint localIndex = groupThreadId.x; localIndex < NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK;
		localIndex += 64u)
	{
		const uint fineCell = fineBase + localIndex;
		const uint archiveCell = archiveBase + localIndex;
		const float4 scalar = DormantLoadFineScalar(min(gDormantConstants.FieldPing, 1u), fineCell);
		const float4 velocity = DormantLoadFineVelocity(min(gDormantConstants.FieldPing, 1u), fineCell);
		const float4 optical = DormantLoadFineOptical(min(gDormantConstants.FieldPing, 1u), fineCell);
		const float4 dynamics = DormantLoadFineDynamics(min(gDormantConstants.FieldPing, 1u), fineCell);
		if (!all(isfinite(scalar)) || !all(isfinite(velocity)) || !all(isfinite(optical)) ||
			!all(isfinite(dynamics)) || scalar.x < 0.0 || any(optical < 0.0) || any(dynamics < 0.0))
		{
			uint ignored;
			InterlockedOr(sInvalidPayload, 1u, ignored);
		}
		uint ignoredMass;
		InterlockedAdd(sMassQ, (uint)min(max(scalar.x, 0.0) * 4096.0, 4294967295.0), ignoredMass);
		gDormantScalar[archiveCell] = scalar;
		gDormantVelocity[archiveCell] = velocity;
		gDormantOptical[archiveCell] = optical;
		gDormantDynamics[archiveCell] = dynamics;
	}
	GroupMemoryBarrierWithGroupSync();

	if (groupThreadId.x == 0u)
	{
		const float measuredMass = (float)sMassQ / 4096.0;
		const bool expectedMassKnown =
			(work.Flags & NRI_SMOKE_DORMANT_WORK_MASS_KNOWN) != 0u;
		const float tolerance = max(0.001, abs(work.OpticalMass) *
			max(gDormantConstants.OpticalMassRelativeTolerance, 0.0));
		if (sInvalidPayload != 0u || !isfinite(measuredMass) ||
			(expectedMassKnown && (!isfinite(work.OpticalMass) || work.OpticalMass < 0.0 ||
				abs(measuredMass - work.OpticalMass) > tolerance)))
		{
			gDormantHash[sArchiveHashSlot].State = NRI_SMOKE_DORMANT_TOMBSTONE;
			gDormantRecords[sArchiveIndex].State = NRI_SMOKE_DORMANT_EMPTY;
			DormantPushArchiveFree(sArchiveIndex);
			RetainFine(workIndex, work, NRI_SMOKE_DORMANT_OUTCOME_VALIDATION_FAILURE);
			gDormantControl[0].ArchiveValidationFailures++;
			sValid = 0u;
		}
		else
		{
			gDormantRecords[sArchiveIndex].OpticalMass = measuredMass;
			DeviceMemoryBarrier();
			gDormantRecords[sArchiveIndex].State = NRI_SMOKE_DORMANT_RESIDENT;
			gDormantHash[sArchiveHashSlot].State = NRI_SMOKE_DORMANT_RESIDENT;
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (sValid == 0u) return;

	// Coarse authority is already fully published. Remove fine lookup authority
	// before clearing storage; no reader can observe a partially cleared brick.
	if (groupThreadId.x == 0u)
	{
		const SmokeGridBrick brick = gDormantFineBricks[sFineIndex];
		gDormantFineHash[brick.HashSlot].State = NRI_SMOKE_GRID_TOMBSTONE;
	}
	GroupMemoryBarrierWithGroupSync();

	for (uint localIndex = groupThreadId.x; localIndex < NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK;
		localIndex += 64u)
	{
		const uint fineCell = fineBase + localIndex;
		DormantStoreFineFields(0u, fineCell, 0.0, 0.0, 0.0, 0.0);
		DormantStoreFineFields(1u, fineCell, 0.0, 0.0, 0.0, 0.0);
		gDormantFineDeposit0[fineCell] = 0;
		gDormantFineDeposit1[fineCell] = 0;
		gDormantFineDeposit2[fineCell] = 0;
		gDormantFineDeposit3[fineCell] = 0;
	}
	GroupMemoryBarrierWithGroupSync();
	if (groupThreadId.x == 0u)
	{
		gDormantFineBricks[sFineIndex].State = NRI_SMOKE_GRID_EMPTY;
		DormantPushFineFree(sFineIndex);
		uint priorResident;
		InterlockedAdd(gDormantFineControl[0].ResidentCount, 0xffffffffu, priorResident);
		uint archiveResident;
		InterlockedAdd(gDormantControl[0].ResidentCount, 1u, archiveResident);
		gDormantControl[0].ArchivePublished++;
		SmokeDormantGridResult result = (SmokeDormantGridResult)0;
		result.Coordinate = work.Coordinate;
		result.InputGeneration = work.Generation;
		result.OutputGeneration = sArchiveGeneration;
		result.Outcome = NRI_SMOKE_DORMANT_OUTCOME_ARCHIVED;
		result.ArchiveIndex = sArchiveIndex;
		result.FineIndex = sFineIndex;
		gDormantResults[workIndex] = result;
	}
}
