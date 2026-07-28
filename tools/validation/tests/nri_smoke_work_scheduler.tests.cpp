#include "nri_smoke_work_scheduler.h"

#include <cstdlib>
#include <iostream>

namespace
{
void Require(bool condition, const char* message)
{
	if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
}

int main()
{
	NRISmokeWorkScheduler scheduler;
	const auto& reference = scheduler.Resolve(0u, 7u);
	Require(reference.effectiveProfile == NRISmokeWorkProfile::Reference,
		"profile zero must preserve reference behavior");
	Require(reference.table.emissionCommands == 256u && reference.table.firstUseSources == 8u &&
		reference.table.simulationSubsteps == 7u,
		"reference must retain existing command, prompt, and authored substep limits");
	Require(reference.table.radianceNewInvalidCells == NRISmokeWorkTable::Unrestricted &&
		!NRISmokeWorkScheduler::Enforces(reference.table, NRISmokeWorkCapability_RadianceNewInvalid),
		"reference radiance must not advertise a fake profile cap");
	Require(reference.table.materializedFroxels == NRISmokeWorkTable::Unrestricted,
		"unsupported materialization work must remain explicitly unrestricted");
	Require(reference.table.analyticCarriers == 64u &&
		NRISmokeWorkScheduler::Enforces(reference.table, NRISmokeWorkCapability_AnalyticCarriers),
		"analytic carriers must have an explicit static admission quantity");
	Require(reference.table.analyticLightEvents == 64u &&
		reference.table.analyticLightAnchors == 4u &&
		reference.table.analyticLightSamples == NRISmokeWorkTable::Unrestricted,
		"reference analytic lighting must preserve the resolved authored sample quantity");

	const auto& high = scheduler.Resolve(1u, 8u);
	Require(high.table.emissionCommands == 128u && high.table.firstUseSources == 8u &&
		high.table.froxelPixelSize == 16u && high.table.froxelDepth == 48u &&
		high.table.emissiveLights == 1u && high.table.emissiveBackend == 2u &&
		high.table.lightSamples == 4u && high.table.maximumLightCandidates == 8u &&
		high.table.radianceNewInvalidCells == 16384u &&
		high.table.radianceMaintenanceCells == 65536u && high.table.simulationSubsteps == 1u &&
		high.table.analyticCarriers == 64u && high.table.analyticLightEvents == 64u &&
		high.table.analyticLightAnchors == 4u && high.table.analyticLightSamples == 4u,
		"high table must remain immutable");
	const uint32_t highSerial = high.profileChangeSerial;
	Require(scheduler.Resolve(1u, 1u).profileChangeSerial == highSerial,
		"irrelevant setting changes must not change a static profile");

	const auto& medium = scheduler.Resolve(2u, 8u);
	Require(medium.table.emissionCommands == 32u && medium.table.firstUseSources == 4u &&
		medium.table.froxelPixelSize == 24u && medium.table.froxelDepth == 32u &&
		medium.table.emissiveLights == 1u && medium.table.emissiveBackend == 2u &&
		medium.table.lightSamples == 4u && medium.table.maximumLightCandidates == 8u &&
		medium.table.radianceNewInvalidCells == 8192u &&
		medium.table.radianceMaintenanceCells == 32768u && medium.table.simulationSubsteps == 1u &&
		medium.table.analyticCarriers == 64u && medium.table.analyticLightEvents == 64u &&
		medium.table.analyticLightAnchors == 4u && medium.table.analyticLightSamples == 4u,
		"medium table must remain immutable");
	const auto& low = scheduler.Resolve(3u, 8u);
	Require(low.table.emissionCommands == 16u && low.table.firstUseSources == 2u &&
		low.table.froxelPixelSize == 32u && low.table.froxelDepth == 24u &&
		low.table.emissiveLights == 0u && low.table.emissiveBackend == 0u &&
		low.table.lightSamples == 1u && low.table.maximumLightCandidates == 4u &&
		low.table.radianceNewInvalidCells == 4096u &&
		low.table.radianceMaintenanceCells == 8192u && low.table.simulationSubsteps == 1u &&
		low.table.analyticCarriers == 64u && low.table.analyticLightEvents == 0u &&
		low.table.analyticLightAnchors == 0u && low.table.analyticLightSamples == 0u,
		"low table must remain immutable");
	Require(NRISmokeWorkScheduler::Enforces(low.table, NRISmokeWorkCapability_RadianceMaintenance),
		"static profiles must enforce both radiance lanes");

	const auto& invalid = scheduler.Resolve(999u, 3u);
	Require(invalid.effectiveProfile == NRISmokeWorkProfile::Reference &&
		invalid.requestedProfile == 999u && invalid.table.simulationSubsteps == 3u,
		"invalid values must fail visibly to reference behavior");

	scheduler.RecordEmission(40u, 16u);
	scheduler.RecordPrompt(7u, 2u);
	scheduler.RecordAnalytic(9u, 4u);
	scheduler.RecordSimulation(5u, 1u, 4u);
	const auto& capped = scheduler.GetSnapshot();
	Require(capped.emissionDeferred == 24u && capped.promptDeferred == 5u,
		"fixed CPU work must close as scheduled plus deferred");
	Require(capped.analyticRequested == 9u && capped.analyticScheduled == 4u && capped.analyticDropped == 5u,
		"analytic admission must close as scheduled plus immediately dropped");
	Require(capped.simulationDeferredSubsteps == 4u && capped.simulationDebtSubsteps == 4u &&
		capped.simulationMaximumDebtSubsteps == 4u && capped.simulationConsecutiveCappedFrames == 1u &&
		capped.simulationCappedFrames == 1u,
		"simulation cap telemetry must retain exact debt");
	scheduler.RecordSimulation(1u, 1u, 4u);
	Require(scheduler.GetSnapshot().simulationConsecutiveCappedFrames == 0u &&
		scheduler.GetSnapshot().simulationMaximumDebtSubsteps == 4u,
		"an uncapped frame must end the capped streak without hiding retained debt");

	std::cout << "Smoke static work scheduler tests passed.\n";
	return 0;
}
