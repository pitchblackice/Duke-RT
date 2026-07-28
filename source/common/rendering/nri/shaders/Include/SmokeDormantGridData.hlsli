#ifndef NRI_SMOKE_DORMANT_GRID_DATA_HLSLI
#define NRI_SMOKE_DORMANT_GRID_DATA_HLSLI

#define NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK 512u
#define NRI_SMOKE_DORMANT_GRID_HASH_PROBES 24u
#define NRI_SMOKE_DORMANT_EMPTY 0u
#define NRI_SMOKE_DORMANT_CLAIMED 1u
#define NRI_SMOKE_DORMANT_RESIDENT 2u
#define NRI_SMOKE_DORMANT_REHYDRATING 3u
#define NRI_SMOKE_DORMANT_TOMBSTONE 4u
#define NRI_SMOKE_DORMANT_OUTCOME_NONE 0u
#define NRI_SMOKE_DORMANT_OUTCOME_ARCHIVED 1u
#define NRI_SMOKE_DORMANT_OUTCOME_REHYDRATED 2u
#define NRI_SMOKE_DORMANT_OUTCOME_RETAINED_FINE 3u
#define NRI_SMOKE_DORMANT_OUTCOME_RETAINED_COARSE 4u
#define NRI_SMOKE_DORMANT_OUTCOME_STALE_EPOCH 5u
#define NRI_SMOKE_DORMANT_OUTCOME_STALE_GENERATION 6u
#define NRI_SMOKE_DORMANT_OUTCOME_ARCHIVE_FULL 7u
#define NRI_SMOKE_DORMANT_OUTCOME_HASH_FAILURE 8u
#define NRI_SMOKE_DORMANT_OUTCOME_VALIDATION_FAILURE 9u
#define NRI_SMOKE_DORMANT_OUTCOME_FINE_CAPACITY 10u
#define NRI_SMOKE_DORMANT_OUTCOME_FINE_ACTIVE_CAPACITY 11u
#define NRI_SMOKE_DORMANT_WORK_MASS_KNOWN 1u
#define NRI_SMOKE_DORMANT_INJECTION_ESTABLISHED_AUTHORITY 1u

struct SmokeDormantGridWork
{
	int3 Coordinate;
	uint Generation;
	uint Epoch;
	uint LastSimulationFrame;
	float OpticalMass;
	uint Flags;
};

struct SmokeDormantGridHashEntry
{
	int3 Coordinate;
	uint ArchiveIndex;
	uint Generation;
	uint State;
	uint2 Padding;
};

struct SmokeDormantGridRecord
{
	int3 Coordinate;
	uint HashSlot;
	uint Generation;
	uint State;
	uint FineGeneration;
	uint Epoch;
	uint LastSimulationFrame;
	uint Flags;
	float OpticalMass;
	uint3 Padding;
};

struct SmokeDormantGridResult
{
	int3 Coordinate;
	uint InputGeneration;
	uint OutputGeneration;
	uint Outcome;
	uint ArchiveIndex;
	uint FineIndex;
};

struct SmokeDormantGridInjection
{
	int3 Coordinate;
	uint Generation;
	uint Epoch;
	uint CadenceSteps;
	uint SourceId;
	uint Flags;
	float3 Position;
	float Radius;
	float4 Scalar;
	float4 Momentum;
	float4 Optical;
	float4 Dynamics;
};

struct SmokeDormantGridControl
{
	uint ArchiveCapacity;
	uint HashCapacity;
	uint FreeCount;
	uint ResidentCount;
	uint ArchiveAttempts;
	uint ArchivePublished;
	uint ArchiveRetainedFine;
	uint ArchiveFull;
	uint ArchiveHashFailures;
	uint ArchiveStale;
	uint ArchiveValidationFailures;
	uint RehydrateAttempts;
	uint RehydratePublished;
	uint RehydrateRetainedCoarse;
	uint RehydrateFineCapacity;
	uint RehydrateHashFailures;
	uint RehydrateStale;
	uint MaximumArchiveProbe;
	uint MaximumFineProbe;
	uint Epoch;
	uint FrameIndex;
	uint ArchiveWorkExecuted;
	uint RehydrateWorkExecuted;
	uint EvolutionWorkExecuted;
	uint FineActiveCompactions;
	uint FineActiveEntriesRemoved;
	uint EvolutionCursor;
	uint EvolutionAttempts;
	uint EvolutionSkipped;
	uint InjectionAttempts;
	uint InjectionApplied;
	uint InjectionRejected;
	uint InjectionMissing;
	uint InjectionStale;
	uint InjectionCadenceSteps;
	uint InjectionCells;
	uint4 Padding;
};

struct SmokeDormantGridConstants
{
	uint Pass;
	uint FrameIndex;
	uint SimulationEpoch;
	uint FieldPing;
	uint FineBrickCapacity;
	uint FineHashCapacity;
	uint FineCellCapacity;
	uint ArchiveCapacity;
	uint ArchiveHashCapacity;
	uint DemotionCount;
	uint PromotionCount;
	uint EvolutionCount;
	float CellSize;
	float DeltaTime;
	float OpticalMassRelativeTolerance;
	uint ActivePing;
	float3 CameraPosition;
	uint Padding0;
	uint InjectionCount;
	float MaximumTransportCells;
	uint EvolutionInjectionIndex;
	uint Padding1;
	uint4 Padding2;
	uint4 Padding3;
};

uint SmokeDormantGridHashCoordinate(int3 coordinate)
{
	uint value = asuint(coordinate.x) * 0x8da6b343u;
	value ^= asuint(coordinate.y) * 0xd8163841u;
	value ^= asuint(coordinate.z) * 0xcb1ab31fu;
	value ^= value >> 16u;
	value *= 0x7feb352du;
	value ^= value >> 15u;
	return value;
}

#endif
