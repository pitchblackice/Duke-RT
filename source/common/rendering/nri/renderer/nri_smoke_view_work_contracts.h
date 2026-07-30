#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint32_t NRI_SMOKE_VIEW_TILE_AXIS = 8u;
constexpr uint32_t NRI_SMOKE_VIEW_MASK_WORDS = 2u;
constexpr uint32_t NRI_SMOKE_VIEW_MAX_DEPTH = 64u;

enum class NRISmokeViewWorkPass : uint32_t
{
	Clear = 0,
	ProjectTiles,
	ExpandColumns,
	CountColumns,
	PrefixColumns,
	ScatterFroxels,
	Finalize,
	CompareDense,
	Count,
};

struct NRISmokeViewMaskGpu
{
	uint32_t words[NRI_SMOKE_VIEW_MASK_WORDS] = {};
};

// GPU-written counters deliberately distinguish the bounded input work from
// the resulting masks. None of these counters is an admission or truncation
// mechanism: overflow is a correctness failure reserved for later compaction.
struct NRISmokeViewWorkControlGpu
{
	uint32_t frameStamp = 0;
	uint32_t simulationEpoch = 0;
	uint32_t brickTileTests = 0;
	uint32_t residentBrickTileTests = 0;
	uint32_t opticalCellTests = 0;
	uint32_t contributingBrickTilePairs = 0;
	uint32_t projectedSpheres = 0;
	uint32_t projectedSpans = 0;
	uint32_t attemptedMarks = 0;
	uint32_t duplicateMerges = 0;
	uint32_t uniqueFroxels = 0;
	uint32_t uniqueColumns = 0;
	uint32_t overflow = 0;
	uint32_t falseNegatives = 0;
	uint32_t falsePositives = 0;
	uint32_t tauErrorBits = 0;
	uint32_t opacityErrorBits = 0;
	uint32_t radianceErrorBits = 0;
	uint32_t boundaryFalseNegatives = 0;
	uint32_t denseContributing = 0;
	uint32_t outputHashLo = 0;
	uint32_t outputHashHi = 0;
	uint32_t evaluationRoute = 0;
	uint32_t evaluationDispatched = 0;
	uint32_t evaluationSelected = 0;
	uint32_t evaluationSkipped = 0;
	uint32_t nearPlaneSpans = 0;
	uint32_t cameraInsideSpans = 0;
	uint32_t behindCameraRejects = 0;
	uint32_t offscreenRejects = 0;
	uint32_t emptyBrickTilePairs = 0;
	uint32_t compactCount = 0;
	uint32_t prefixColumns = 0;
	uint32_t scatterWrites = 0;
	uint32_t compactCapacity = 0;
};

struct NRISmokeViewIndirectArgsGpu
{
	uint32_t x = 0;
	uint32_t y = 1;
	uint32_t z = 1;
};

struct NRISmokeViewWorkConstants
{
	uint32_t pass = 0;
	uint32_t frameIndex = 0;
	uint32_t simulationEpoch = 0;
	uint32_t brickCapacity = 0;

	uint32_t froxelWidth = 0;
	uint32_t froxelHeight = 0;
	uint32_t froxelDepth = 0;
	uint32_t tileCountX = 0;

	uint32_t tileCountY = 0;
	uint32_t fieldPing = 0;
	float cellSize = 0.0f;
	float opticalThreshold = 0.0f;

	float froxelMaxDistance = 0.0f;
	float depthExponent = 1.0f;
	float tanHalfFovX = 1.0f;
	float tanHalfFovY = 1.0f;

	float cameraPosition[3] = {};
	uint32_t executionRoute = 0;

	float cameraForward[3] = {};
	uint32_t froxelCapacity = 0;

	float cameraRight[3] = {};
	float padding2 = 0.0f;

	float cameraUp[3] = {};
	float padding3 = 0.0f;
};

struct NRISmokeViewWorkLayout
{
	uint32_t tileCountX = 0;
	uint32_t tileCountY = 0;
	uint32_t tileCount = 0;
	uint32_t columnCount = 0;
	uint64_t brickTilePairBound = 0;
	uint64_t opticalCellTestBound = 0;
	uint64_t preparationUnitBound = 0;
};

static_assert(sizeof(NRISmokeViewMaskGpu) == 8);
static_assert(sizeof(NRISmokeViewWorkControlGpu) == 140);
static_assert(sizeof(NRISmokeViewIndirectArgsGpu) == 12);
static_assert(sizeof(NRISmokeViewWorkConstants) == 128);
