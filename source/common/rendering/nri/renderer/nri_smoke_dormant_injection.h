#pragma once

#include "nri_smoke_continuous_sources.h"
#include "nri_smoke_contracts.h"
#include "nri_smoke_dormant_grid_contracts.h"
#include "nri_smoke_spatial_interest.h"

#include <cstdint>
#include <map>
#include <set>
#include <vector>

struct NRISmokeDormantInjectionBuildInput
{
	uint32_t epoch = 0u;
	uint32_t maximumInjections = UINT32_MAX;
	float cellSize = 8.0f;
	const std::vector<NRISmokeContinuousSourceWorkRequest>* requests = nullptr;
	const std::vector<NRISmokeStyleGpu>* styles = nullptr;
	const std::map<NRISmokeSpatialCoordinate, NRISmokeSpatialBrickObservation>* authorities = nullptr;
	const std::set<NRISmokeSpatialCoordinate>* promotions = nullptr;
};

struct NRISmokeDormantInjectionBuildResult
{
	uint32_t requests = 0u;
	uint32_t routedSources = 0u;
	uint32_t invalidRequests = 0u;
	uint32_t missingAuthorities = 0u;
	uint32_t promotionConflicts = 0u;
	uint32_t capacityRejected = 0u;
	uint32_t cadenceSteps = 0u;
	std::vector<NRISmokeDormantGridInjectionGpu> injections;
	std::set<uint32_t> routedSourceIds;
};

// Converts already-coalesced persistent-source cadence into exact archive
// field moments. It never creates authority and never retains deferred work.
NRISmokeDormantInjectionBuildResult NRIBuildSmokeDormantInjections(
	const NRISmokeDormantInjectionBuildInput& input);
