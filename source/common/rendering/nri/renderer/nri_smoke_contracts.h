#pragma once

#include <cstddef>
#include <cstdint>

enum class NRISmokePass : uint32_t
{
	Clear = 0,
	Simulate,
	Spawn,
	Bin,
	EvaluateMedium,
	LightPoint,
	Integrate,
	Composite,
};

struct NRISmokeParticleGpu
{
	float position[3] = {};
	float radius = 0.0f;
	float velocity[3] = {};
	float age = 0.0f;
	float density = 0.0f;
	float lifetime = 0.0f;
	uint32_t styleIndex = 0;
	uint32_t epoch = 0;
	float initialDensity = 0.0f;
	float initialRadius = 0.0f;
	uint32_t serial = 0;
	uint32_t active = 0;
};

struct NRISmokeStyleGpu
{
	float albedo[3] = { 0.5f, 0.5f, 0.5f };
	float extinction = 1.0f;
	float anisotropy = 0.0f;
	float radius = 16.0f;
	float expansionVelocity = 8.0f;
	float lifetime = 3.0f;
	float density = 1.0f;
	float densityHalfLife = 1.5f;
	float riseVelocity = 8.0f;
	float velocityRandom = 2.0f;
	float velocityInherit = 0.0f;
	float buoyancy = 0.0f;
	float drag = 0.5f;
	float turbulence = 1.0f;
	float turbulenceScale = 32.0f;
	float padding[3] = {};
};

struct NRISmokeInjectionCommandGpu
{
	float position[3] = {};
	float spawnRadius = 0.0f;
	float velocity[3] = {};
	uint32_t styleIndex = 0;
	uint32_t count = 1;
	uint32_t serial = 0;
	float densityScale = 1.0f;
	float radiusScale = 1.0f;
	float velocityCone = 0.0f;
	uint32_t epoch = 0;
	uint32_t padding[2] = {};
};

struct NRISmokeControlGpu
{
	uint32_t writeCursor = 0;
	uint32_t activeApprox = 0;
	uint32_t liveEvictions = 0;
	uint32_t columnOverflow = 0;
	uint32_t epoch = 0;
	uint32_t spawned = 0;
	uint32_t expired = 0;
	uint32_t wideParticlesProjected = 0;
	uint32_t lightCandidatesTested = 0;
	uint32_t lightDistanceRejected = 0;
	uint32_t lightShadowRays = 0;
	uint32_t lightShadowVisible = 0;
	uint32_t lightShadowOccluded = 0;
	uint32_t lightSoftSamples = 0;
	uint32_t lightRadianceClamps = 0;
	uint32_t wideGlobalDrops = 0;
	uint32_t filterCandidateHits = 0;
	uint32_t filterAlphaRejects = 0;
	uint32_t filterNoShadowRejects = 0;
	uint32_t filterOneWayRejects = 0;
	uint32_t filterReflectionRejects = 0;
	uint32_t filterPortalContinuations = 0;
	uint32_t filterAcceptedBlockers = 0;
	uint32_t filterMisses = 0;
	uint32_t filterSkipLimitExits = 0;
	uint32_t filterContinuationLimitExits = 0;
	uint32_t filterResourceDowngrades = 0;
	uint32_t fineColumnReferences = 0;
	uint32_t wideCellReferences = 0;
	uint32_t selectionCollisions = 0;
	uint32_t selectionReplacements = 0;
	uint32_t globalDepthReferences = 0;
	uint32_t fineTierParticles = 0;
	uint32_t wideTierParticles = 0;
	uint32_t globalTierParticles = 0;
	uint32_t fineOccupiedCells = 0;
	uint32_t wideOccupiedCells = 0;
	uint32_t globalOccupiedSlices = 0;
	uint32_t fineSelectionCollisions = 0;
	uint32_t wideSelectionCollisions = 0;
	uint32_t globalSelectionCollisions = 0;
	uint32_t fineSelectionReplacements = 0;
	uint32_t wideSelectionReplacements = 0;
	uint32_t globalSelectionReplacements = 0;
	uint32_t fineSelectionLosses = 0;
	uint32_t wideSelectionLosses = 0;
	uint32_t globalSelectionLosses = 0;
	uint32_t maximumDepthSpan = 0;
	uint32_t depthSpanOne = 0;
	uint32_t depthSpanTwoToFour = 0;
	uint32_t depthSpanFiveToSixteen = 0;
	uint32_t depthSpanOverSixteen = 0;
	uint32_t maximumCandidatesPerFroxel = 0;
	uint32_t occupiedCount = 0;
	uint32_t occupiedOverflow = 0;
	uint32_t mediumCandidateTests = 0;
	uint32_t pointFroxelsProcessed = 0;
	uint32_t padding[3] = {};
};

static_assert(sizeof(NRISmokeParticleGpu) == 64);
static_assert(sizeof(NRISmokeStyleGpu) == 80);
static_assert(sizeof(NRISmokeInjectionCommandGpu) == 64);
static_assert(sizeof(NRISmokeControlGpu) == 240);

struct NRISmokeConstants
{
	uint32_t pass = 0;
	uint32_t frameIndex = 0;
	uint32_t simulationEpoch = 0;
	uint32_t particleCapacity = 0;

	uint32_t commandCount = 0;
	uint32_t styleCount = 0;
	uint32_t froxelWidth = 0;
	uint32_t froxelHeight = 0;

	uint32_t froxelDepth = 0;
	uint32_t columnCapacity = 0;
	uint32_t renderWidth = 0;
	uint32_t renderHeight = 0;

	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	uint32_t debugMode = 0;
	uint32_t flags = 0;

	float deltaTime = 0.0f;
	float simulationTime = 0.0f;
	float froxelMaxDistance = 0.0f;
	float depthExponent = 1.0f;

	float densityScale = 1.0f;
	float radianceScale = 1.0f;
	float tanHalfFovX = 1.0f;
	float tanHalfFovY = 1.0f;

	float cameraPosition[3] = {};
	float timeScale = 1.0f;

	float cameraForward[3] = {};
	float cameraForwardPad = 0.0f;

	float cameraRight[3] = {};
	float cameraRightPad = 0.0f;

	float cameraUp[3] = {};
	float cameraUpPad = 0.0f;

	float wind[3] = {};
	float windPad = 0.0f;

	uint32_t lightMode = 0;
	uint32_t lightSamples = 1;
	uint32_t maxLightCandidates = 8;
	uint32_t runtimeLightCount = 0;

	uint32_t runtimeLightTileCountX = 0;
	uint32_t runtimeLightTileCountY = 0;
	uint32_t pointLightsEnabled = 1;
	// bit 0: filtered visibility requested, bit 1: resources ready,
	// bits 8..15: portal traversal depth.
	uint32_t filteredVisibilityEnabled = 0;
};

static_assert(sizeof(NRISmokeConstants) == 208, "NRISmokeConstants must match SmokeConstants.hlsli");
static_assert(offsetof(NRISmokeConstants, cameraPosition) == 96, "NRISmokeConstants camera offset must match HLSL");
static_assert(offsetof(NRISmokeConstants, lightMode) == 176, "NRISmokeConstants lighting offset must match HLSL");
static_assert(offsetof(NRISmokeConstants, runtimeLightTileCountX) == 192, "NRISmokeConstants runtime-light tile offset must match HLSL");
