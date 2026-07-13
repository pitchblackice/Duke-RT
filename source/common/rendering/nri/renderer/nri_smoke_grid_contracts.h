#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint32_t NRI_SMOKE_GRID_BRICK_AXIS = 8u;
constexpr uint32_t NRI_SMOKE_GRID_CELLS_PER_BRICK = 512u;

enum class NRISmokeGridPass : uint32_t
{
	Clear = 0,
	AllocateCommands,
	BuildDispatch,
	PrepareBricks,
	Deposit,
	ResolveDeposit,
	AllocateHalo,
	BeginRebuild,
	AdvectVelocity,
	AdvectFields,
	Rebuild,
};

struct NRISmokeGridHashEntryGpu
{
	int32_t coordinate[3] = {};
	uint32_t brickIndex = UINT32_MAX;
	uint32_t generation = 0;
	uint32_t state = 0;
	uint32_t padding[2] = {};
};

struct NRISmokeGridBrickGpu
{
	int32_t coordinate[3] = {};
	uint32_t hashSlot = UINT32_MAX;
	uint32_t generation = 0;
	uint32_t state = 0;
	uint32_t idleFrames = 0;
	uint32_t flags = 0;
};

struct NRISmokeGridControlGpu
{
	uint32_t activeCountA = 0;
	uint32_t activeCountB = 0;
	uint32_t residentCount = 0;
	uint32_t freeCount = 0;
	uint32_t allocated = 0;
	uint32_t reclaimed = 0;
	uint32_t allocationFailures = 0;
	uint32_t probeFailures = 0;
	uint32_t maximumProbe = 0;
	uint32_t commandsProcessed = 0;
	uint32_t requestedMassQ = 0;
	uint32_t depositedMassQ = 0;
	uint32_t rejectedMassQ = 0;
	uint32_t saturatedDeposits = 0;
	uint32_t haloAllocations = 0;
	uint32_t occupiedBricks = 0;
	uint32_t emptyBricks = 0;
	uint32_t cflClamps = 0;
	uint32_t backtraceClamps = 0;
	uint32_t nanRejects = 0;
	uint32_t fieldHashLo = 0;
	uint32_t fieldHashHi = 0;
	uint32_t depositionCells = 0;
	uint32_t depositionRejected = 0;
	uint32_t generation = 0;
	uint32_t frameStamp = 0;
	uint32_t brickCapacity = 0;
	uint32_t hashCapacity = 0;
	uint32_t cellCapacity = 0;
	uint32_t activePing = 0;
	uint32_t fieldPing = 0;
	uint32_t cellSizeBits = 0;
};

struct NRISmokeGridDispatchGpu
{
	uint32_t x = 0;
	uint32_t y = 1;
	uint32_t z = 1;
};

struct NRISmokeGridConstants
{
	uint32_t pass = 0;
	uint32_t frameIndex = 0;
	uint32_t simulationEpoch = 0;
	uint32_t commandCount = 0;

	uint32_t styleCount = 0;
	uint32_t brickCapacity = 0;
	uint32_t hashCapacity = 0;
	uint32_t cellCapacity = 0;

	uint32_t activePing = 0;
	uint32_t fieldPing = 0;
	uint32_t flags = 0;
	uint32_t representation = 0;

	float cellSize = 8.0f;
	float deltaTime = 0.0f;
	float timeScale = 1.0f;
	float maxBacktrace = 32.0f;

	float wind[3] = {};
	float buoyancy = 1.0f;

	float velocityDamping = 0.15f;
	float windCoupling = 0.5f;
	float densityHalfLifeScale = 1.0f;
	float coolingScale = 1.0f;

	float maxVelocity = 128.0f;
	float activeThreshold = 0.0001f;
	uint32_t reclaimGrace = 120;
	float massQuantization = 4096.0f;

	float momentumQuantization = 256.0f;
	float padding[3] = {};
};

static_assert(sizeof(NRISmokeGridHashEntryGpu) == 32);
static_assert(sizeof(NRISmokeGridBrickGpu) == 32);
static_assert(sizeof(NRISmokeGridControlGpu) == 128);
static_assert(sizeof(NRISmokeGridDispatchGpu) == 12);
static_assert(sizeof(NRISmokeGridConstants) == 128);
