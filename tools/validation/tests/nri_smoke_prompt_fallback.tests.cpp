#include "nri_smoke_prompt_fallback.h"
#include "nri_smoke_pulses.h"

#include <cstdlib>
#include <iostream>

namespace
{
void Require(bool condition, const char* message)
{
	if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

NRISmokeInjectionCommandGpu Interactive(uint32_t serial)
{
	NRISmokeInjectionCommandGpu command = {};
	command.serial = serial;
	command.count = 1u;
	command.rangeCount = 1u;
	command.epoch = 9u;
	command.sourceId = serial + 1u;
	command.pulseIdLow = serial + 1u;
	command.sourceMetadata = (uint32_t)NRISmokeInjectionSourceClass::InteractiveEvent;
	return command;
}
}

int main()
{
	NRISmokePulseOwner pulses;
	NRISmokePromptFallback prompt;
	for (uint32_t serial = 0u; serial < 72u; ++serial)
	{
		pulses.Enqueue({ Interactive(serial) });
		auto selected = pulses.PendingCommands();
		std::vector<NRISmokePromptRangeIdentity> retained;
		prompt.Prepare(selected, serial, 8.0f, retained);
		Require(selected.size() == 1u && retained.size() == 1u,
			"an acknowledged sticky slot must be reusable beyond ledger capacity");
		std::vector<NRISmokeInjectionCommandGpu> planned;
		uint64_t token = 0u;
		Require(pulses.Plan(selected, planned, token) && pulses.CommitRetaining(token, planned),
			"prompt authority must retain the exact planned range");
		NRISmokePromptOutcomeGpu outcome = {};
		outcome.pulseIdLow = planned[0].pulseIdLow;
		outcome.pulseIdHigh = planned[0].pulseIdHigh;
		outcome.rangeBegin = planned[0].rangeBegin;
		outcome.rangeCount = planned[0].rangeCount;
		outcome.outcome = (uint32_t)NRISmokePromptOutcome::GridCommitted;
		prompt.CommitGridHandoffs(pulses, { outcome });
		Require(pulses.PendingCommands().empty(), "exact grid ack must release CPU pulse authority");
	}
	Require(prompt.GetSnapshot().gridHandoffs == 72u,
		"more than 64 sequential identities must hand off without permanent fallback");

	pulses.Enqueue({ Interactive(100u), Interactive(101u), Interactive(102u), Interactive(103u),
		Interactive(104u), Interactive(105u), Interactive(106u), Interactive(107u), Interactive(108u) });
	auto saturated = pulses.PendingCommands();
	std::vector<NRISmokePromptRangeIdentity> retained;
	const auto saturatedResult = prompt.Prepare(saturated, 100u, 8.0f, retained);
	Require(saturated.size() == NRISmokePromptFallback::FixedFallbackCarrierQuantity && retained.size() == 8u,
		"prompt work must stay at the fixed eight-range quantity");
	Require(pulses.PendingCommands().size() == 9u,
		"overflow prompt work must remain pending rather than disappear");
	Require(saturatedResult.deferredRanges == 1u && saturatedResult.deferredMass == 1u &&
		saturatedResult.deferredBrickWork != 0u,
		"prompt overflow must report exact deferred count, nominal mass, and estimated work");

	NRISmokePromptFallback rollbackOwner;
	std::vector<NRISmokeInjectionCommandGpu> prior = { Interactive(200u) };
	rollbackOwner.Prepare(prior, 200u, 8.0f, retained);
	rollbackOwner.Commit(200u); // Submitted; ack is deliberately delayed.
	std::vector<NRISmokeInjectionCommandGpu> retry = { prior[0] };
	rollbackOwner.Prepare(retry, 201u, 8.0f, retained);
	rollbackOwner.Rollback();
	std::vector<NRISmokeInjectionCommandGpu> newcomers = { Interactive(201u), Interactive(202u),
		Interactive(203u), Interactive(204u), Interactive(205u), Interactive(206u),
		Interactive(207u), Interactive(208u) };
	rollbackOwner.Prepare(newcomers, 202u, 8.0f, retained);
	Require(newcomers.size() == 7u,
		"record rollback must retain a previously submitted sticky slot until exact ack");
	rollbackOwner.Reset();
	newcomers = { Interactive(211u), Interactive(212u), Interactive(213u), Interactive(214u),
		Interactive(215u), Interactive(216u), Interactive(217u), Interactive(218u) };
	rollbackOwner.Prepare(newcomers, 203u, 8.0f, retained);
	Require(newcomers.size() == 8u, "epoch/resource reset must release every sticky prompt slot");

	NRISmokePromptFallback profiledOwner;
	std::vector<NRISmokeInjectionCommandGpu> profiled = { Interactive(300u), Interactive(301u),
		Interactive(302u), Interactive(303u), Interactive(304u) };
	const auto profiledResult = profiledOwner.Prepare(profiled, 300u, 8.0f, retained, 2u);
	Require(profiled.size() == 2u && retained.size() == 2u && profiledResult.deferredRanges == 3u,
		"a static first-use profile must cap scheduled sticky slots without dropping overflow");
	Require(profiledOwner.GetSnapshot().maximumFallbackCarrierQuantity == 2u,
		"prompt status must publish the effective profile quantity");

	NRISmokePromptFallback switchedOwner;
	std::vector<NRISmokeInjectionCommandGpu> initialSlots = { Interactive(400u), Interactive(401u),
		Interactive(402u), Interactive(403u), Interactive(404u), Interactive(405u),
		Interactive(406u), Interactive(407u) };
	switchedOwner.Prepare(initialSlots, 400u, 8.0f, retained, 8u);
	const NRISmokeInjectionCommandGpu stickySlotSeven = initialSlots[7];
	switchedOwner.Commit(400u);
	std::vector<NRISmokeInjectionCommandGpu> afterReduction = { Interactive(408u), stickySlotSeven,
		Interactive(409u) };
	const auto reducedResult = switchedOwner.Prepare(afterReduction, 401u, 8.0f, retained, 2u);
	Require(afterReduction.size() == 1u && retained.size() == 1u && reducedResult.deferredRanges == 2u,
		"a reduced profile must schedule an existing high-slot identity without duplicating a full ledger");
	Require(afterReduction[0].pulseIdLow == stickySlotSeven.pulseIdLow,
		"profile reduction must preserve the exact sticky identity before admitting newcomers");
	Require(((stickySlotSeven.sourceMetadata & NRI_SMOKE_SOURCE_METADATA_PROMPT_SLOT_MASK) >>
		NRI_SMOKE_SOURCE_METADATA_PROMPT_SLOT_SHIFT) == 7u,
		"the profile-switch fixture must exercise a sticky identity outside the reduced slot prefix");

	std::cout << "Smoke prompt sticky-slot transaction tests passed.\n";
	return 0;
}
