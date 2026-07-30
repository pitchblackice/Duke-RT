#include "Include/SmokeDormantGridResources.hlsli"

// One group owns one archive record. The exact 8^3 payload fits in the 32 KiB
// group-shared budget, allowing an in-place conservative update without a
// second archive ping or cross-record races.
groupshared float4 sScalar[NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK];
groupshared float4 sVelocity[NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK];
groupshared float4 sOptical[NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK];
groupshared float4 sDynamics[NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK];

uint3 DormantLocalCoordinate(uint index)
{
	return uint3(index & 7u, (index >> 3u) & 7u, (index >> 6u) & 7u);
}

uint DormantLocalIndex(uint3 coordinate)
{
	return coordinate.x | (coordinate.y << 3u) | (coordinate.z << 6u);
}

float DormantAxisTransportWeight(int source, int destination, float displacement)
{
	const float target = clamp((float)source + displacement, 0.0, 7.0);
	const int lower = (int)floor(target);
	const int upper = min(lower + 1, 7);
	const float fraction = target - (float)lower;
	float weight = lower == destination ? 1.0 - fraction : 0.0;
	weight += upper == destination ? fraction : 0.0;
	return weight;
}

bool DormantEvolutionInjectionRecord(uint injectionIndex, out uint archiveIndex)
{
	archiveIndex = 0xffffffffu;
	const SmokeDormantGridInjection injection = gDormantInjections[injectionIndex];
	if (injection.Epoch != gDormantConstants.SimulationEpoch ||
		(injection.Flags & NRI_SMOKE_DORMANT_INJECTION_ESTABLISHED_AUTHORITY) == 0u)
		return false;
	const uint mask = gDormantConstants.ArchiveHashCapacity - 1u;
	const uint base = SmokeDormantGridHashCoordinate(injection.Coordinate) & mask;
	[loop]
	for (uint probe = 0u; probe < min(NRI_SMOKE_DORMANT_GRID_HASH_PROBES,
		gDormantConstants.ArchiveHashCapacity); ++probe)
	{
		const SmokeDormantGridHashEntry entry = gDormantHash[(base + probe) & mask];
		if (entry.State == NRI_SMOKE_DORMANT_EMPTY) return false;
		if (entry.State == NRI_SMOKE_DORMANT_RESIDENT &&
			entry.Generation == injection.Generation &&
			all(entry.Coordinate == injection.Coordinate))
		{
			archiveIndex = entry.ArchiveIndex;
			if (archiveIndex >= gDormantConstants.ArchiveCapacity) return false;
			const SmokeDormantGridRecord record = gDormantRecords[archiveIndex];
			return record.State == NRI_SMOKE_DORMANT_RESIDENT &&
				record.Generation == injection.Generation &&
				record.Epoch == injection.Epoch;
		}
	}
	return false;
}

[numthreads(8, 8, 8)]
void main(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
	const uint capacity = max(gDormantConstants.ArchiveCapacity, 1u);
	const bool injectionTarget = gDormantConstants.EvolutionInjectionIndex != 0xffffffffu;
	const uint workCount = injectionTarget ? 1u :
		min(gDormantConstants.EvolutionCount, capacity);
	if (groupId.x >= workCount) return;
	const uint base = (gDormantConstants.FrameIndex * workCount) % capacity;
	uint archiveIndex = (base + groupId.x) % capacity;
	if (injectionTarget && !DormantEvolutionInjectionRecord(
		gDormantConstants.EvolutionInjectionIndex, archiveIndex))
	{
		if (groupThreadId.x == 0u && groupThreadId.y == 0u && groupThreadId.z == 0u)
		{
			InterlockedAdd(gDormantControl[0].EvolutionAttempts, 1u);
			InterlockedAdd(gDormantControl[0].EvolutionSkipped, 1u);
		}
		return;
	}
	const uint localIndex = DormantLocalIndex(groupThreadId);
	const SmokeDormantGridRecord record = gDormantRecords[archiveIndex];
	if (localIndex == 0u)
	{
		InterlockedAdd(gDormantControl[0].EvolutionAttempts, 1u);
		if (!injectionTarget && groupId.x == 0u)
			gDormantControl[0].EvolutionCursor = (base + workCount) % capacity;
	}
	if (record.State != NRI_SMOKE_DORMANT_RESIDENT ||
		record.Epoch != gDormantConstants.SimulationEpoch ||
		record.LastSimulationFrame >= gDormantConstants.FrameIndex)
	{
		if (localIndex == 0u)
			InterlockedAdd(gDormantControl[0].EvolutionSkipped, 1u);
		return;
	}

	const uint cellIndex = DormantCellIndex(archiveIndex, localIndex);
	sScalar[localIndex] = gDormantScalar[cellIndex];
	sVelocity[localIndex] = gDormantVelocity[cellIndex];
	sOptical[localIndex] = gDormantOptical[cellIndex];
	sDynamics[localIndex] = gDormantDynamics[cellIndex];
	GroupMemoryBarrierWithGroupSync();

	const uint elapsedFrames = gDormantConstants.FrameIndex - record.LastSimulationFrame;
	const float deltaTime = max(gDormantConstants.DeltaTime, 0.0) * (float)elapsedFrames;
	const float inverseCellSize = rcp(max(gDormantConstants.CellSize, 0.0001));
	const float maximumTransport = clamp(gDormantConstants.MaximumTransportCells, 0.0, 0.95);
	const int3 destination = (int3)groupThreadId;
	float4 outputScalar = 0.0;
	float4 outputMomentum = 0.0;
	float4 outputOptical = 0.0;
	float4 outputDynamics = 0.0;

	// Maximum displacement is less than one cell, so only the 3^3 source
	// neighborhood can contribute. Clamping target positions at brick edges
	// keeps every source weight inside the authority and conserves each moment.
	[unroll]
	for (int zOffset = -1; zOffset <= 1; ++zOffset)
	{
		[unroll]
		for (int yOffset = -1; yOffset <= 1; ++yOffset)
		{
			[unroll]
			for (int xOffset = -1; xOffset <= 1; ++xOffset)
			{
				const int3 sourceCoordinate = destination + int3(xOffset, yOffset, zOffset);
				if (any(sourceCoordinate < 0) || any(sourceCoordinate > 7)) continue;
				const uint sourceIndex = DormantLocalIndex((uint3)sourceCoordinate);
				const float4 scalar = sScalar[sourceIndex];
				const float4 velocity = sVelocity[sourceIndex];
				const float4 optical = sOptical[sourceIndex];
				const float4 dynamics = sDynamics[sourceIndex];
				const float mass = max(scalar.x, 0.0);
				const float inverseMass = mass > 1e-8 ? rcp(mass) : 0.0;
				const float densityRate = max(dynamics.x * inverseMass, 0.0) /
					max(gDormantConstants.DensityHalfLifeScale, 0.001);
				const float coolingRate = max(dynamics.y * inverseMass, 0.0) /
					max(gDormantConstants.CoolingScale, 0.001);
				const float densityDecay = exp(-densityRate * deltaTime);
				const float coolingDecay = exp(-coolingRate * deltaTime);
				const float3 displacement = clamp(velocity.xyz * deltaTime * inverseCellSize,
					-maximumTransport.xxx, maximumTransport.xxx);
				const float weight =
					DormantAxisTransportWeight(sourceCoordinate.x, destination.x, displacement.x) *
					DormantAxisTransportWeight(sourceCoordinate.y, destination.y, displacement.y) *
					DormantAxisTransportWeight(sourceCoordinate.z, destination.z, displacement.z);
				if (weight <= 0.0) continue;

				float4 decayedScalar = scalar * densityDecay;
				decayedScalar.y *= coolingDecay;
				outputScalar += decayedScalar * weight;
				outputMomentum.xyz += velocity.xyz * (mass * densityDecay * weight);
				outputMomentum.w += velocity.w * densityDecay * weight;
				outputOptical += optical * (densityDecay * weight);
				outputDynamics += dynamics * (densityDecay * weight);
			}
		}
	}

	const float outputMass = max(outputScalar.x, 0.0);
	float4 outputVelocity = float4(outputMass > 1e-8 ?
		outputMomentum.xyz / outputMass : 0.0, outputMomentum.w);
	outputScalar.xyz = max(outputScalar.xyz, 0.0);
	outputOptical = max(outputOptical, 0.0);
	outputDynamics = max(outputDynamics, 0.0);
	outputOptical.xyz = min(outputOptical.xyz, outputScalar.z.xxx);
	const float previousDenominator = outputOptical.w;
	const float anisotropy = previousDenominator > 1e-8 ?
		clamp(outputScalar.w / previousDenominator, -0.95, 0.95) : 0.0;
	outputOptical.w = dot(outputOptical.xyz, float3(0.2126, 0.7152, 0.0722));
	outputScalar.w = anisotropy * outputOptical.w;
	if (!all(isfinite(outputScalar)) || !all(isfinite(outputVelocity)) ||
		!all(isfinite(outputOptical)) || !all(isfinite(outputDynamics)))
	{
		outputScalar = 0.0;
		outputVelocity = 0.0;
		outputOptical = 0.0;
		outputDynamics = 0.0;
	}

	// Do not overwrite shared inputs until every thread has completed its gather.
	GroupMemoryBarrierWithGroupSync();
	sScalar[localIndex] = outputScalar;
	gDormantScalar[cellIndex] = outputScalar;
	gDormantVelocity[cellIndex] = outputVelocity;
	gDormantOptical[cellIndex] = outputOptical;
	gDormantDynamics[cellIndex] = outputDynamics;
	DeviceMemoryBarrierWithGroupSync();
	if (localIndex == 0u)
	{
		float opticalMass = 0.0;
		[unroll]
		for (uint i = 0u; i < NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK; ++i)
			opticalMass += max(sScalar[i].x, 0.0);
		gDormantRecords[archiveIndex].OpticalMass = opticalMass;
		gDormantRecords[archiveIndex].LastSimulationFrame = gDormantConstants.FrameIndex;
		InterlockedAdd(gDormantControl[0].EvolutionWorkExecuted, 1u);
	}
}
