#include "nri_smoke_work_scheduler.h"

#include <algorithm>

namespace
{
	constexpr uint32_t kSupportedCapabilities =
		NRISmokeWorkCapability_EmissionCommands |
		NRISmokeWorkCapability_FirstUseSources |
		NRISmokeWorkCapability_RadianceNewInvalid |
		NRISmokeWorkCapability_RadianceMaintenance |
		NRISmokeWorkCapability_SimulationSubsteps |
		NRISmokeWorkCapability_AnalyticCarriers;

	constexpr uint32_t kAlwaysEnforcedCapabilities =
		NRISmokeWorkCapability_EmissionCommands |
		NRISmokeWorkCapability_FirstUseSources |
		NRISmokeWorkCapability_SimulationSubsteps |
		NRISmokeWorkCapability_AnalyticCarriers;
}

NRISmokeWorkTable NRISmokeWorkScheduler::BuildTable(NRISmokeWorkProfile profile,
	uint32_t referenceMaximumSubsteps)
{
	NRISmokeWorkTable table = {};
	table.supportedCapabilities = kSupportedCapabilities;
	table.enforcedCapabilities = kAlwaysEnforcedCapabilities;
	switch (profile)
	{
	case NRISmokeWorkProfile::High:
		table.emissionCommands = 128u;
		table.firstUseSources = 8u;
		table.froxelPixelSize = 16u;
		table.froxelDepth = 48u;
		table.emissiveLights = 1u;
		table.emissiveBackend = 2u;
		table.lightSamples = 4u;
		table.maximumLightCandidates = 8u;
		table.radianceNewInvalidCells = 16384u;
		table.radianceMaintenanceCells = 65536u;
		table.simulationSubsteps = 1u;
		break;
	case NRISmokeWorkProfile::Medium:
		table.emissionCommands = 32u;
		table.firstUseSources = 4u;
		table.froxelPixelSize = 24u;
		table.froxelDepth = 32u;
		table.emissiveLights = 1u;
		table.emissiveBackend = 2u;
		table.lightSamples = 4u;
		table.maximumLightCandidates = 8u;
		table.radianceNewInvalidCells = 8192u;
		table.radianceMaintenanceCells = 32768u;
		table.simulationSubsteps = 1u;
		break;
	case NRISmokeWorkProfile::Low:
		table.emissionCommands = 16u;
		table.firstUseSources = 2u;
		table.froxelPixelSize = 32u;
		table.froxelDepth = 24u;
		table.emissiveLights = 0u;
		table.emissiveBackend = 0u;
		table.lightSamples = 1u;
		table.maximumLightCandidates = 4u;
		table.radianceNewInvalidCells = 4096u;
		table.radianceMaintenanceCells = 8192u;
		table.simulationSubsteps = 1u;
		break;
	case NRISmokeWorkProfile::Reference:
	default:
		table.simulationSubsteps = std::clamp(referenceMaximumSubsteps, 1u, 8u);
		return table;
	}
	table.enforcedCapabilities |= NRISmokeWorkCapability_RadianceNewInvalid |
		NRISmokeWorkCapability_RadianceMaintenance;
	return table;
}

const NRISmokeWorkSchedulerSnapshot& NRISmokeWorkScheduler::Resolve(uint32_t requestedProfile,
	uint32_t referenceMaximumSubsteps)
{
	const NRISmokeWorkProfile effective = requestedProfile <= (uint32_t)NRISmokeWorkProfile::Low
		? (NRISmokeWorkProfile)requestedProfile : NRISmokeWorkProfile::Reference;
	const bool profileChanged = !mResolved || mSnapshot.effectiveProfile != effective;
	mSnapshot.requestedProfile = requestedProfile;
	mSnapshot.effectiveProfile = effective;
	mSnapshot.table = BuildTable(effective, referenceMaximumSubsteps);
	if (profileChanged)
	{
		mSnapshot.profileChangeSerial++;
		mSnapshot.simulationConsecutiveCappedFrames = 0u;
		mResolved = true;
	}
	return mSnapshot;
}

void NRISmokeWorkScheduler::RecordEmission(uint32_t requested, uint32_t scheduled)
{
	mSnapshot.emissionRequested = requested;
	mSnapshot.emissionScheduled = std::min(scheduled, requested);
	mSnapshot.emissionDeferred = requested - mSnapshot.emissionScheduled;
}

void NRISmokeWorkScheduler::RecordPrompt(uint32_t requested, uint32_t scheduled)
{
	mSnapshot.promptRequested = requested;
	mSnapshot.promptScheduled = std::min(scheduled, requested);
	mSnapshot.promptDeferred = requested - mSnapshot.promptScheduled;
}

void NRISmokeWorkScheduler::RecordAnalytic(uint32_t requested, uint32_t scheduled)
{
	mSnapshot.analyticRequested = requested;
	mSnapshot.analyticScheduled = std::min(scheduled, requested);
	mSnapshot.analyticDropped = requested - mSnapshot.analyticScheduled;
}

void NRISmokeWorkScheduler::RecordSimulation(uint32_t due, uint32_t scheduled, uint32_t debt)
{
	mSnapshot.simulationDueSubsteps = due;
	mSnapshot.simulationScheduledSubsteps = std::min(scheduled, due);
	mSnapshot.simulationDeferredSubsteps = due - mSnapshot.simulationScheduledSubsteps;
	mSnapshot.simulationDebtSubsteps = debt;
	mSnapshot.simulationMaximumDebtSubsteps = std::max(mSnapshot.simulationMaximumDebtSubsteps, debt);
	if (mSnapshot.simulationDeferredSubsteps != 0u)
	{
		mSnapshot.simulationCappedFrames++;
		mSnapshot.simulationConsecutiveCappedFrames++;
	}
	else
		mSnapshot.simulationConsecutiveCappedFrames = 0u;
}

void NRISmokeWorkScheduler::ResetTelemetry()
{
	const uint32_t requested = mSnapshot.requestedProfile;
	const NRISmokeWorkProfile effective = mSnapshot.effectiveProfile;
	const uint32_t serial = mSnapshot.profileChangeSerial;
	const NRISmokeWorkTable table = mSnapshot.table;
	mSnapshot = {};
	mSnapshot.requestedProfile = requested;
	mSnapshot.effectiveProfile = effective;
	mSnapshot.profileChangeSerial = serial;
	mSnapshot.table = table;
}

const char* NRISmokeWorkScheduler::ProfileName(NRISmokeWorkProfile profile)
{
	switch (profile)
	{
	case NRISmokeWorkProfile::High: return "high";
	case NRISmokeWorkProfile::Medium: return "medium";
	case NRISmokeWorkProfile::Low: return "low";
	case NRISmokeWorkProfile::Reference:
	default: return "reference";
	}
}

bool NRISmokeWorkScheduler::Supports(const NRISmokeWorkTable& table, NRISmokeWorkCapability capability)
{
	return (table.supportedCapabilities & (uint32_t)capability) != 0u;
}

bool NRISmokeWorkScheduler::Enforces(const NRISmokeWorkTable& table, NRISmokeWorkCapability capability)
{
	return (table.enforcedCapabilities & (uint32_t)capability) != 0u;
}
