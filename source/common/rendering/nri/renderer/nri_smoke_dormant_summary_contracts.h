#pragma once

#include <cstddef>
#include <cstdint>

// This is an experimental coarse-state contract. Nothing in the production
// grid consumes it yet: an archived row may become authoritative only after a
// later grid integration can publish the row before releasing its fine brick.
struct NRISmokeDormantSummaryGpu
{
	float boundsMin[3] = {};
	float opticalMass = 0.0f;
	float boundsMax[3] = {};
	float thermalMass = 0.0f;
	float centroid[3] = {};
	float densityMass = 0.0f;
	float velocity[3] = {};
	float velocityWeight = 0.0f;
	uint32_t sourceId = 0;
	uint32_t sourceClass = 0;
	uint32_t lastSimulationTick = 0;
	uint32_t epoch = 0;
	uint32_t brickGeneration = 0;
	uint32_t summaryGeneration = 0;
	uint32_t flags = 0;
	uint32_t padding = 0;
};

static_assert(sizeof(NRISmokeDormantSummaryGpu) == 96);
static_assert(offsetof(NRISmokeDormantSummaryGpu, sourceId) == 64);
static_assert(offsetof(NRISmokeDormantSummaryGpu, brickGeneration) == 80);
