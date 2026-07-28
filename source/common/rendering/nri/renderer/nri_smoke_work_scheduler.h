#pragma once

#include <cstdint>
#include <limits>

enum class NRISmokeWorkProfile : uint32_t
{
	Reference = 0,
	High = 1,
	Medium = 2,
	Low = 3,
};

enum NRISmokeWorkCapability : uint32_t
{
	NRISmokeWorkCapability_EmissionCommands = 1u << 0u,
	NRISmokeWorkCapability_FirstUseSources = 1u << 1u,
	NRISmokeWorkCapability_RadianceNewInvalid = 1u << 2u,
	NRISmokeWorkCapability_RadianceMaintenance = 1u << 3u,
	NRISmokeWorkCapability_SimulationSubsteps = 1u << 4u,
	NRISmokeWorkCapability_AnalyticCarriers = 1u << 5u,
	NRISmokeWorkCapability_AnalyticLighting = 1u << 6u,
};

struct NRISmokeWorkTable
{
	static constexpr uint32_t Unrestricted = std::numeric_limits<uint32_t>::max();

	uint32_t revision = 6u;
	uint32_t emissionCommands = 256u;
	uint32_t firstUseSources = 8u;
	uint32_t analyticCarriers = 64u;
	uint32_t analyticLightEvents = 64u;
	uint32_t analyticLightAnchors = 4u;
	uint32_t analyticLightSamples = Unrestricted;
	uint32_t froxelPixelSize = Unrestricted;
	uint32_t froxelDepth = Unrestricted;
	uint32_t emissiveLights = Unrestricted;
	uint32_t emissiveBackend = Unrestricted;
	uint32_t lightSamples = Unrestricted;
	uint32_t maximumLightCandidates = Unrestricted;
	uint32_t admissionBrickRequests = Unrestricted;
	uint32_t depositionCellVisits = Unrestricted;
	uint32_t projectionWorkUnits = Unrestricted;
	uint32_t materializedFroxels = Unrestricted;
	uint32_t radianceNewInvalidCells = Unrestricted;
	uint32_t radianceMaintenanceCells = Unrestricted;
	uint32_t worldLinkRays = Unrestricted;
	uint32_t directReceiverSamples = Unrestricted;
	uint32_t dormantPromotions = Unrestricted;
	uint32_t simulationSubsteps = 4u;
	uint32_t supportedCapabilities = 0u;
	uint32_t enforcedCapabilities = 0u;
};

struct NRISmokeWorkSchedulerSnapshot
{
	uint32_t requestedProfile = 0u;
	NRISmokeWorkProfile effectiveProfile = NRISmokeWorkProfile::Reference;
	uint32_t profileChangeSerial = 0u;
	NRISmokeWorkTable table = {};
	uint32_t emissionRequested = 0u;
	uint32_t emissionScheduled = 0u;
	uint32_t emissionDeferred = 0u;
	uint32_t promptRequested = 0u;
	uint32_t promptScheduled = 0u;
	uint32_t promptDeferred = 0u;
	uint32_t analyticRequested = 0u;
	uint32_t analyticScheduled = 0u;
	uint32_t analyticDropped = 0u;
	uint32_t simulationDueSubsteps = 0u;
	uint32_t simulationScheduledSubsteps = 0u;
	uint32_t simulationDeferredSubsteps = 0u;
	uint32_t simulationDebtSubsteps = 0u;
	uint32_t simulationMaximumDebtSubsteps = 0u;
	uint32_t simulationConsecutiveCappedFrames = 0u;
	uint64_t simulationCappedFrames = 0u;
};

// Resolves static work quantities only. This owner deliberately has no GPU
// timing, renderer-headroom, target-frame-time, or predicted-cost input.
class NRISmokeWorkScheduler
{
public:
	const NRISmokeWorkSchedulerSnapshot& Resolve(uint32_t requestedProfile,
		uint32_t referenceMaximumSubsteps);
	void RecordEmission(uint32_t requested, uint32_t scheduled);
	void RecordPrompt(uint32_t requested, uint32_t scheduled);
	void RecordAnalytic(uint32_t requested, uint32_t scheduled);
	void RecordSimulation(uint32_t due, uint32_t scheduled, uint32_t debt);
	void ResetTelemetry();

	const NRISmokeWorkSchedulerSnapshot& GetSnapshot() const { return mSnapshot; }
	static const char* ProfileName(NRISmokeWorkProfile profile);
	static bool Supports(const NRISmokeWorkTable& table, NRISmokeWorkCapability capability);
	static bool Enforces(const NRISmokeWorkTable& table, NRISmokeWorkCapability capability);

private:
	static NRISmokeWorkTable BuildTable(NRISmokeWorkProfile profile,
		uint32_t referenceMaximumSubsteps);

	NRISmokeWorkSchedulerSnapshot mSnapshot = {};
	bool mResolved = false;
};
