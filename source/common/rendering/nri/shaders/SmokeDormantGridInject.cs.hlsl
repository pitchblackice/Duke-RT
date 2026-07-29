#include "Include/SmokeDormantGridResources.hlsli"

groupshared uint sArchiveIndex;
groupshared uint sValid;
groupshared float sKernel[NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK];

uint3 DormantInjectionLocalCoordinate(uint index)
{
	return uint3(index & 7u, (index >> 3u) & 7u, (index >> 6u) & 7u);
}

bool DormantFindInjectionRecord(SmokeDormantGridInjection injection, out uint archiveIndex,
	out bool stale)
{
	archiveIndex = 0xffffffffu;
	stale = false;
	const uint mask = gDormantConstants.ArchiveHashCapacity - 1u;
	const uint base = SmokeDormantGridHashCoordinate(injection.Coordinate) & mask;
	[loop]
	for (uint probe = 0u; probe < min(NRI_SMOKE_DORMANT_GRID_HASH_PROBES,
		gDormantConstants.ArchiveHashCapacity); ++probe)
	{
		const SmokeDormantGridHashEntry entry = gDormantHash[(base + probe) & mask];
		if (entry.State == NRI_SMOKE_DORMANT_EMPTY) return false;
		if (all(entry.Coordinate == injection.Coordinate))
		{
			stale = entry.Generation != injection.Generation ||
				entry.State != NRI_SMOKE_DORMANT_RESIDENT;
			if (stale) return false;
			const SmokeDormantGridRecord record = gDormantRecords[entry.ArchiveIndex];
			stale = record.State != NRI_SMOKE_DORMANT_RESIDENT ||
				record.Generation != injection.Generation ||
				record.Epoch != injection.Epoch;
			if (stale) return false;
			archiveIndex = entry.ArchiveIndex;
			return true;
		}
	}
	return false;
}

[numthreads(8, 8, 8)]
void main(uint3 groupThreadId : SV_GroupThreadID)
{
	const uint localIndex = groupThreadId.x | (groupThreadId.y << 3u) |
		(groupThreadId.z << 6u);
	// A single group serializes the bounded work records. This makes multiple
	// established continuous sources targeting one authority deterministic.
	for (uint workIndex = 0u; workIndex < gDormantConstants.InjectionCount; ++workIndex)
	{
		const SmokeDormantGridInjection injection = gDormantInjections[workIndex];
		if (localIndex == 0u)
		{
			sArchiveIndex = 0xffffffffu;
			sValid = 0u;
			InterlockedAdd(gDormantControl[0].InjectionAttempts, 1u);
			bool stale = false;
			const bool established = (injection.Flags &
				NRI_SMOKE_DORMANT_INJECTION_ESTABLISHED_AUTHORITY) != 0u;
			const bool payloadValid = injection.Epoch == gDormantConstants.SimulationEpoch &&
				injection.CadenceSteps != 0u && established && injection.Radius >= 0.0 &&
				all(isfinite(injection.Position)) && isfinite(injection.Radius) &&
				all(isfinite(injection.Scalar)) && all(isfinite(injection.Momentum)) &&
				all(isfinite(injection.Optical)) && all(isfinite(injection.Dynamics)) &&
				injection.Scalar.x >= 0.0 && all(injection.Optical >= 0.0) &&
				all(injection.Dynamics >= 0.0);
			if (payloadValid && DormantFindInjectionRecord(injection, sArchiveIndex, stale))
				sValid = 1u;
			else
			{
				InterlockedAdd(gDormantControl[0].InjectionRejected, 1u);
				if (stale || injection.Epoch != gDormantConstants.SimulationEpoch)
					InterlockedAdd(gDormantControl[0].InjectionStale, 1u);
				else if (payloadValid)
					InterlockedAdd(gDormantControl[0].InjectionMissing, 1u);
			}
		}
		GroupMemoryBarrierWithGroupSync();
		if (sValid != 0u)
		{
			const uint3 local = DormantInjectionLocalCoordinate(localIndex);
			const float3 cell = ((float3)(injection.Coordinate * 8 + (int3)local) + 0.5) *
				max(gDormantConstants.CellSize, 0.0001);
			const float radius = max(injection.Radius, gDormantConstants.CellSize);
			const float normalized = saturate(1.0 - distance(cell, injection.Position) /
				max(radius, 0.0001));
			sKernel[localIndex] = normalized * normalized * (3.0 - 2.0 * normalized);
			GroupMemoryBarrierWithGroupSync();
			for (uint stride = NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK / 2u;
				stride != 0u; stride >>= 1u)
			{
				if (localIndex < stride) sKernel[localIndex] += sKernel[localIndex + stride];
				GroupMemoryBarrierWithGroupSync();
			}
			const float weight = sKernel[0] > 1e-8 ?
				(normalized * normalized * (3.0 - 2.0 * normalized)) / sKernel[0] :
				(localIndex == 0u ? 1.0 : 0.0);
			const uint cellIndex = DormantCellIndex(sArchiveIndex, localIndex);
			float4 scalar = gDormantScalar[cellIndex];
			float4 velocity = gDormantVelocity[cellIndex];
			float4 optical = gDormantOptical[cellIndex];
			float4 dynamics = gDormantDynamics[cellIndex];
			const float oldMass = max(scalar.x, 0.0);
			const float addedMass = max(injection.Scalar.x, 0.0) * weight;
			const float newMass = oldMass + addedMass;
			if (newMass > 1e-8)
				velocity.xyz = (velocity.xyz * oldMass + injection.Momentum.xyz * weight) /
					newMass;
			velocity.w += injection.Momentum.w * weight;
			scalar += injection.Scalar * weight;
			optical += injection.Optical * weight;
			dynamics += injection.Dynamics * weight;
			gDormantScalar[cellIndex] = scalar;
			gDormantVelocity[cellIndex] = velocity;
			gDormantOptical[cellIndex] = optical;
			gDormantDynamics[cellIndex] = dynamics;
			if (weight > 0.0)
				InterlockedAdd(gDormantControl[0].InjectionCells, 1u);
			DeviceMemoryBarrierWithGroupSync();
			if (localIndex == 0u)
			{
				gDormantRecords[sArchiveIndex].OpticalMass += max(injection.Scalar.x, 0.0);
				InterlockedAdd(gDormantControl[0].InjectionApplied, 1u);
				InterlockedAdd(gDormantControl[0].InjectionCadenceSteps,
					injection.CadenceSteps);
			}
		}
		GroupMemoryBarrierWithGroupSync();
	}
}
