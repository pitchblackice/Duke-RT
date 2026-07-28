#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

constexpr uint32_t NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK = 512u;
constexpr uint32_t NRI_SMOKE_DORMANT_GRID_HASH_PROBES = 24u;
constexpr uint32_t NRI_SMOKE_DORMANT_GRID_FINE_DESCRIPTOR_COUNT = 18u;
constexpr uint32_t NRI_SMOKE_DORMANT_GRID_STORAGE_DESCRIPTOR_COUNT = 12u;
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
	Evolve = 4u,
	Inject = 5u,
};

enum NRISmokeDormantGridWorkFlags : uint32_t
{
	NRISmokeDormantGridWorkFlag_None = 0u,
	NRISmokeDormantGridWorkFlag_MassKnown = 1u << 0u,
};

enum NRISmokeDormantGridInjectionFlags : uint32_t
{
	NRISmokeDormantGridInjectionFlag_None = 0u,
	// The caller has observed this continuous source while its target already
	// had published fine or coarse authority. Injection must never create a
	// dormant record or replay work after that authority disappears.
	NRISmokeDormantGridInjectionFlag_EstablishedAuthority = 1u << 0u,
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

// One already-coalesced, single-use deposit into an existing dormant record.
// The four field moments use the same representation as the fine-grid payload:
// scalar, momentum/scale, optical, and dynamics. The producer partitions a
// source across target brick coordinates before submission.
struct NRISmokeDormantGridInjectionGpu
{
	int32_t coordinate[3] = {};
	uint32_t generation = 0u;
	uint32_t epoch = 0u;
	uint32_t cadenceSteps = 0u;
	uint32_t sourceId = 0u;
	uint32_t flags = NRISmokeDormantGridInjectionFlag_None;
	float position[3] = {};
	float radius = 0.0f;
	float scalar[4] = {};
	float momentum[4] = {};
	float optical[4] = {};
	float dynamics[4] = {};
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
	uint32_t evolutionCursor = 0u;
	uint32_t evolutionAttempts = 0u;
	uint32_t evolutionSkipped = 0u;
	uint32_t injectionAttempts = 0u;
	uint32_t injectionApplied = 0u;
	uint32_t injectionRejected = 0u;
	uint32_t injectionMissing = 0u;
	uint32_t injectionStale = 0u;
	uint32_t injectionCadenceSteps = 0u;
	uint32_t injectionCells = 0u;
	uint32_t padding[4] = {};
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
	uint32_t injectionCount = 0u;
	float maximumTransportCells = 0.95f;
	uint32_t evolutionInjectionIndex = UINT32_MAX;
	uint32_t padding[9] = {};
};

static_assert(sizeof(NRISmokeDormantGridWorkGpu) == 32u);
static_assert(sizeof(NRISmokeDormantGridHashEntryGpu) == 32u);
static_assert(sizeof(NRISmokeDormantGridRecordGpu) == 56u);
static_assert(sizeof(NRISmokeDormantGridResultGpu) == 32u);
static_assert(sizeof(NRISmokeDormantGridInjectionGpu) == 112u);
static_assert(sizeof(NRISmokeDormantGridControlGpu) == 160u);
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

constexpr bool NRISmokeDormantGridInjectionRequiresEstablishedAuthority(uint32_t flags)
{
	return (flags & NRISmokeDormantGridInjectionFlag_EstablishedAuthority) != 0u;
}

constexpr uint32_t NRISmokeDormantGridEvolutionBase(uint32_t frameIndex,
	uint32_t workCount, uint32_t archiveCapacity)
{
	return archiveCapacity == 0u ? 0u : (frameIndex * workCount) % archiveCapacity;
}

inline float NRISmokeDormantGridDecay(float rate, float elapsedSeconds)
{
	return std::exp(-std::max(rate, 0.0f) * std::max(elapsedSeconds, 0.0f));
}

inline float NRISmokeDormantGridAxisTransportWeight(int32_t source, int32_t destination,
	float displacement)
{
	const float target = std::clamp((float)source + std::clamp(displacement, -0.95f, 0.95f),
		0.0f, 7.0f);
	const int32_t lower = (int32_t)std::floor(target);
	const int32_t upper = std::min(lower + 1, 7);
	const float fraction = target - (float)lower;
	return (lower == destination ? 1.0f - fraction : 0.0f) +
		(upper == destination ? fraction : 0.0f);
}

constexpr uint64_t NRISmokeDormantGridPayloadBytes(uint32_t archiveCapacity)
{
	return (uint64_t)archiveCapacity * NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK *
		4u * sizeof(float) * 4u;
}
