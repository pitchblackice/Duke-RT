#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint32_t NRI_SMOKE_GRID_LIGHT_LOBE_COUNT = 6u;
constexpr uint32_t NRI_SMOKE_GRID_LIGHT_RECORD_WORDS = 24u;
constexpr uint32_t NRI_SMOKE_GRID_LIGHT_MAX_HISTORY = 64u;
constexpr uint32_t NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY = 16u;

enum class NRISmokeGridLightingPass : uint32_t
{
	Prepare = 0,
	BuildActive,
	BuildProposals,
	Seed,
	Temporal,
	BuildLinks,
	Filter,
	Count,
};

enum class NRISmokeEmissiveBackend : uint32_t
{
	Auto = 0,
	Legacy,
	World,
	Compare,
};

// Six RGB means and six RGB second moments occupy 36 fp16 values (18 words).
// The remaining words carry generation-safe temporal evidence and padding.
struct NRISmokeGridLightRecordGpu
{
	uint32_t data[NRI_SMOKE_GRID_LIGHT_RECORD_WORDS] = {};
};

struct NRISmokeGridLightControlGpu
{
	uint32_t activeCount = 0;
	uint32_t supportCount = 0;
	uint32_t sourceCount = 0;
	uint32_t scheduledCount = 0;
	uint32_t samples = 0;
	uint32_t visible = 0;
	uint32_t physicalZero = 0;
	uint32_t missing = 0;
	uint32_t structuralErrors = 0;
	uint32_t overflowRejects = 0;
	uint32_t temporalAccepted = 0;
	uint32_t temporalRejected = 0;
	uint32_t linksOpen = 0;
	uint32_t linksBlocked = 0;
	uint32_t linksStale = 0;
	uint32_t cornerAccepted = 0;
	uint32_t cornerRejected = 0;
	uint32_t filterAccepted = 0;
	uint32_t filterRejected = 0;
	uint32_t maximumAge = 0;
	uint32_t frameStamp = 0;
	uint32_t simulationEpoch = 0;
	uint32_t fieldPing = 0;
	uint32_t flags = 0;
	uint32_t proposalListsBuilt = 0;
	uint32_t proposalCandidatesTested = 0;
	uint32_t proposalCandidatesAccepted = 0;
	uint32_t proposalLocalSamples = 0;
	uint32_t proposalGlobalSamples = 0;
	uint32_t proposalFallbacks = 0;
	uint32_t proposalTruncations = 0;
	uint32_t proposalMaximumCount = 0;
};

struct NRISmokeGridLightProposalGpu
{
	uint32_t candidateIndices[NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY] = {};
	uint32_t count = 0;
	uint32_t brickGeneration = 0;
	uint32_t simulationEpoch = 0;
	uint32_t frameStamp = 0;
};

struct NRISmokeGridLightingStatusSnapshot
{
	bool requested = false;
	bool initialized = false;
	bool resourcesReady = false;
	bool filterRequested = false;
	bool filterAllocated = false;
	uint32_t requestedBackend = 0;
	uint32_t effectiveBackend = 0;
	uint32_t cellCapacity = 0;
	uint32_t fieldPing = 0;
	uint32_t simulationEpoch = 0;
	uint32_t lastUpdatedFrame = UINT32_MAX;
	uint64_t fieldBytes = 0;
	uint64_t workBytes = 0;
	uint64_t linkBytes = 0;
	uint64_t proposalBytes = 0;
	uint64_t filterBytes = 0;
	uint64_t totalBytes = 0;
	const char* authority = "disabled";
	const char* failureReason = "not-requested";
	const char* filterDecision = "not-requested";
	const char* proposalDecision = "global-cdf/no-measured-starvation";
};

static_assert(sizeof(NRISmokeGridLightRecordGpu) == 96);
static_assert(sizeof(NRISmokeGridLightControlGpu) == 128);
static_assert(sizeof(NRISmokeGridLightProposalGpu) == 80);
