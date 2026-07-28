#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint32_t NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK = 512u;
constexpr uint32_t NRI_SMOKE_DORMANT_GRID_HASH_PROBES = 24u;
constexpr uint32_t NRI_SMOKE_DORMANT_GRID_FINE_DESCRIPTOR_COUNT = 18u;
constexpr uint32_t NRI_SMOKE_DORMANT_GRID_STORAGE_DESCRIPTOR_COUNT = 11u;
constexpr uint32_t NRI_SMOKE_DORMANT_GRID_EVALUATION_DESCRIPTOR_COUNT = 7u;

enum class NRISmokeDormantGridFineDescriptor : uint32_t
{
	Control = 0u,
	Hash,
	Bricks,
	FreeList,
	ActiveA,
	ActiveB,
	ScalarA,
	ScalarB,
	VelocityA,
	VelocityB,
	OpticalA,
	OpticalB,
	DynamicsA,
	DynamicsB,
	Deposit0,
	Deposit1,
	Deposit2,
	Deposit3,
	Count,
};

static_assert((uint32_t)NRISmokeDormantGridFineDescriptor::Count ==
	NRI_SMOKE_DORMANT_GRID_FINE_DESCRIPTOR_COUNT);

enum class NRISmokeDormantGridState : uint32_t
{
	Empty = 0u,
	Claimed = 1u,
	Resident = 2u,
	Rehydrating = 3u,
	Tombstone = 4u,
};

enum class NRISmokeDormantGridOutcome : uint32_t
{
	None = 0u,
	Archived = 1u,
	Rehydrated = 2u,
	RetainedFine = 3u,
	RetainedCoarse = 4u,
	StaleEpoch = 5u,
	StaleGeneration = 6u,
	ArchiveFull = 7u,
	HashFailure = 8u,
	ValidationFailure = 9u,
	FineCapacity = 10u,
	FineActiveCapacity = 11u,
};

enum class NRISmokeDormantGridPass : uint32_t
{
	Clear = 0u,
	Archive = 1u,
	CompactFineActive = 2u,
	Rehydrate = 3u,
};

enum NRISmokeDormantGridWorkFlags : uint32_t
{
	NRISmokeDormantGridWorkFlag_None = 0u,
	NRISmokeDormantGridWorkFlag_MassKnown = 1u << 0u,
};

struct NRISmokeDormantGridWorkGpu
{
	int32_t coordinate[3] = {};
	uint32_t generation = 0u;
	uint32_t epoch = 0u;
	uint32_t lastSimulationFrame = 0u;
	float opticalMass = 0.0f;
	uint32_t flags = NRISmokeDormantGridWorkFlag_None;
};

struct NRISmokeDormantGridHashEntryGpu
{
	int32_t coordinate[3] = {};
	uint32_t archiveIndex = UINT32_MAX;
	uint32_t generation = 0u;
	uint32_t state = 0u;
	uint32_t padding[2] = {};
};

struct NRISmokeDormantGridRecordGpu
{
	int32_t coordinate[3] = {};
	uint32_t hashSlot = UINT32_MAX;
	uint32_t generation = 0u;
	uint32_t state = 0u;
	uint32_t fineGeneration = 0u;
	uint32_t epoch = 0u;
	uint32_t lastSimulationFrame = 0u;
	uint32_t flags = 0u;
	float opticalMass = 0.0f;
	uint32_t padding[3] = {};
};

struct NRISmokeDormantGridResultGpu
{
	int32_t coordinate[3] = {};
	uint32_t inputGeneration = 0u;
	uint32_t outputGeneration = 0u;
	uint32_t outcome = 0u;
	uint32_t archiveIndex = UINT32_MAX;
	uint32_t fineIndex = UINT32_MAX;
};

struct NRISmokeDormantGridControlGpu
{
	uint32_t archiveCapacity = 0u;
	uint32_t hashCapacity = 0u;
	uint32_t freeCount = 0u;
	uint32_t residentCount = 0u;
	uint32_t archiveAttempts = 0u;
	uint32_t archivePublished = 0u;
	uint32_t archiveRetainedFine = 0u;
	uint32_t archiveFull = 0u;
	uint32_t archiveHashFailures = 0u;
	uint32_t archiveStale = 0u;
	uint32_t archiveValidationFailures = 0u;
	uint32_t rehydrateAttempts = 0u;
	uint32_t rehydratePublished = 0u;
	uint32_t rehydrateRetainedCoarse = 0u;
	uint32_t rehydrateFineCapacity = 0u;
	uint32_t rehydrateHashFailures = 0u;
	uint32_t rehydrateStale = 0u;
	uint32_t maximumArchiveProbe = 0u;
	uint32_t maximumFineProbe = 0u;
	uint32_t epoch = 0u;
	uint32_t frameIndex = 0u;
	uint32_t archiveWorkExecuted = 0u;
	uint32_t rehydrateWorkExecuted = 0u;
	uint32_t evolutionWorkExecuted = 0u;
	uint32_t fineActiveCompactions = 0u;
	uint32_t fineActiveEntriesRemoved = 0u;
	uint32_t padding[6] = {};
};

struct NRISmokeDormantGridConstants
{
	uint32_t pass = 0u;
	uint32_t frameIndex = 0u;
	uint32_t simulationEpoch = 0u;
	uint32_t fieldPing = 0u;
	uint32_t fineBrickCapacity = 0u;
	uint32_t fineHashCapacity = 0u;
	uint32_t fineCellCapacity = 0u;
	uint32_t archiveCapacity = 0u;
	uint32_t archiveHashCapacity = 0u;
	uint32_t demotionCount = 0u;
	uint32_t promotionCount = 0u;
	uint32_t evolutionCount = 0u;
	float cellSize = 8.0f;
	float deltaTime = 0.0f;
	float opticalMassRelativeTolerance = 0.25f;
	uint32_t activePing = 0u;
	float cameraPosition[3] = {};
	uint32_t padding0 = 0u;
	uint32_t padding[12] = {};
};

static_assert(sizeof(NRISmokeDormantGridWorkGpu) == 32u);
static_assert(sizeof(NRISmokeDormantGridHashEntryGpu) == 32u);
static_assert(sizeof(NRISmokeDormantGridRecordGpu) == 56u);
static_assert(sizeof(NRISmokeDormantGridResultGpu) == 32u);
static_assert(sizeof(NRISmokeDormantGridControlGpu) == 128u);
static_assert(sizeof(NRISmokeDormantGridConstants) == 128u);
static_assert(offsetof(NRISmokeDormantGridRecordGpu, fineGeneration) == 24u);

constexpr bool NRISmokeDormantGridMayReleaseFine(NRISmokeDormantGridOutcome outcome)
{
	return outcome == NRISmokeDormantGridOutcome::Archived;
}

constexpr bool NRISmokeDormantGridMayRetireCoarse(NRISmokeDormantGridOutcome outcome)
{
	return outcome == NRISmokeDormantGridOutcome::Rehydrated;
}

constexpr bool NRISmokeDormantGridShouldCompareExpectedMass(uint32_t flags)
{
	return (flags & NRISmokeDormantGridWorkFlag_MassKnown) != 0u;
}

constexpr uint64_t NRISmokeDormantGridPayloadBytes(uint32_t archiveCapacity)
{
	return (uint64_t)archiveCapacity * NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK *
		4u * sizeof(float) * 4u;
}
