#include "nri_smoke_prompt_fallback.h"
#include "nri_smoke_pulses.h"

#include <cmath>
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

std::vector<NRISmokeStyleGpu> Styles()
{
	std::vector<NRISmokeStyleGpu> styles(1);
	styles[0].radius = 10.0f;
	styles[0].expansionVelocity = 2.0f;
	styles[0].lifetime = 3.0f;
	styles[0].densityHalfLife = 1.0f;
	return styles;
}

NRISmokePulseEnqueueInfo Latest(double authoredGameplaySeconds)
{
	NRISmokePulseEnqueueInfo info = {};
	info.authoredGameplaySeconds = authoredGameplaySeconds;
	info.maximumLatencySeconds = 0.2f;
	info.queuePolicy = NRISmokePulseQueuePolicy::Latest;
	info.transitory = true;
	return info;
}
}

int main()
{
	NRISmokePulseOwner pulses;
	NRISmokePromptFallback prompt;
	const auto styles = Styles();
	for (uint32_t serial = 0u; serial < 72u; ++serial)
	{
		pulses.Enqueue({ Interactive(serial) }, serial / 60.0);
		auto selected = pulses.PendingCommands();
		std::vector<NRISmokePromptRangeIdentity> retained;
		prompt.Prepare(selected, pulses, serial, serial / 60.0, styles, 8.0f, retained);
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
	const auto saturatedResult = prompt.Prepare(saturated, pulses, 100u, 100.0 / 60.0,
		styles, 8.0f, retained);
	Require(saturated.size() == NRISmokePromptFallback::FixedFallbackCarrierQuantity && retained.size() == 8u,
		"prompt work must stay at the fixed eight-range quantity");
	Require(pulses.PendingCommands().size() == 9u,
		"overflow prompt work must remain pending rather than disappear");
	Require(saturatedResult.deferredRanges == 1u && saturatedResult.deferredMass == 1u &&
		saturatedResult.deferredBrickWork != 0u,
		"prompt overflow must report exact deferred count, nominal mass, and estimated work");

	NRISmokePromptFallback rollbackOwner;
	std::vector<NRISmokeInjectionCommandGpu> prior = { Interactive(200u) };
	rollbackOwner.Prepare(prior, pulses, 200u, 200.0 / 60.0, styles, 8.0f, retained);
	rollbackOwner.Commit(200u); // Submitted; ack is deliberately delayed.
	std::vector<NRISmokeInjectionCommandGpu> retry = { prior[0] };
	rollbackOwner.Prepare(retry, pulses, 201u, 201.0 / 60.0, styles, 8.0f, retained);
	rollbackOwner.Rollback();
	std::vector<NRISmokeInjectionCommandGpu> newcomers = { Interactive(201u), Interactive(202u),
		Interactive(203u), Interactive(204u), Interactive(205u), Interactive(206u),
		Interactive(207u), Interactive(208u) };
	rollbackOwner.Prepare(newcomers, pulses, 202u, 202.0 / 60.0, styles, 8.0f, retained);
	Require(newcomers.size() == 7u,
		"record rollback must retain a previously submitted sticky slot until exact ack");
	rollbackOwner.Reset();
	newcomers = { Interactive(211u), Interactive(212u), Interactive(213u), Interactive(214u),
		Interactive(215u), Interactive(216u), Interactive(217u), Interactive(218u) };
	rollbackOwner.Prepare(newcomers, pulses, 203u, 203.0 / 60.0, styles, 8.0f, retained);
	Require(newcomers.size() == 8u, "epoch/resource reset must release every sticky prompt slot");

	NRISmokePromptFallback profiledOwner;
	std::vector<NRISmokeInjectionCommandGpu> profiled = { Interactive(300u), Interactive(301u),
		Interactive(302u), Interactive(303u), Interactive(304u) };
	const auto profiledResult = profiledOwner.Prepare(profiled, pulses, 300u, 5.0,
		styles, 8.0f, retained, 2u);
	Require(profiled.size() == 2u && retained.size() == 2u && profiledResult.deferredRanges == 3u,
		"a static first-use profile must cap scheduled sticky slots without dropping overflow");
	Require(profiledOwner.GetSnapshot().maximumFallbackCarrierQuantity == 2u,
		"prompt status must publish the effective profile quantity");

	NRISmokePromptFallback switchedOwner;
	std::vector<NRISmokeInjectionCommandGpu> initialSlots = { Interactive(400u), Interactive(401u),
		Interactive(402u), Interactive(403u), Interactive(404u), Interactive(405u),
		Interactive(406u), Interactive(407u) };
	switchedOwner.Prepare(initialSlots, pulses, 400u, 400.0 / 60.0,
		styles, 8.0f, retained, 8u);
	const NRISmokeInjectionCommandGpu stickySlotSeven = initialSlots[7];
	switchedOwner.Commit(400u);
	std::vector<NRISmokeInjectionCommandGpu> afterReduction = { Interactive(408u), stickySlotSeven,
		Interactive(409u) };
	const auto reducedResult = switchedOwner.Prepare(afterReduction, pulses, 401u, 401.0 / 60.0,
		styles, 8.0f, retained, 2u);
	Require(afterReduction.size() == 1u && retained.size() == 1u && reducedResult.deferredRanges == 2u,
		"a reduced profile must schedule an existing high-slot identity without duplicating a full ledger");
	Require(afterReduction[0].pulseIdLow == stickySlotSeven.pulseIdLow,
		"profile reduction must preserve the exact sticky identity before admitting newcomers");
	Require(((stickySlotSeven.sourceMetadata & NRI_SMOKE_SOURCE_METADATA_PROMPT_SLOT_MASK) >>
		NRI_SMOKE_SOURCE_METADATA_PROMPT_SLOT_SHIFT) == 7u,
		"the profile-switch fixture must exercise a sticky identity outside the reduced slot prefix");

	NRISmokePulseOwner agingPulses;
	NRISmokePromptFallback agingOwner;
	agingPulses.Enqueue({ Interactive(500u) }, 10.0);
	auto agingCommand = agingPulses.PendingCommands();
	agingOwner.Prepare(agingCommand, agingPulses, 500u, 10.0, styles, 8.0f, retained, 1u);
	std::vector<NRISmokeInjectionCommandGpu> agingPlan;
	uint64_t agingToken = 0u;
	Require(agingPulses.Plan(agingCommand, agingPlan, agingToken) &&
		agingPulses.CommitRetaining(agingToken, agingPlan),
		"aging fallback must retain pulse authority before grid handoff");
	agingOwner.Commit(500u);
	agingCommand = agingPulses.PendingCommands();
	agingOwner.Prepare(agingCommand, agingPulses, 501u, 11.5, styles, 8.0f, retained, 1u);
	Require(std::abs(agingCommand[0].spawnRadius - 13.0f) < 1e-5f,
		"fallback radius must expand from authored state with simulation age");
	Require(std::abs(agingCommand[0].densityScale - std::exp2(-1.5f)) < 1e-5f,
		"fallback density mass must follow the authored density half-life");
	agingOwner.Commit(501u);
	agingOwner.RetireExpired(agingPulses, 13.0, styles);
	Require(agingPulses.PendingCommands().empty(),
		"fallback must retire exact pulse authority at the authored lifetime");
	Require(agingPulses.GetSnapshot().committedRanges == 0u &&
		agingPulses.GetSnapshot().committedMass == 0u &&
		agingPulses.GetSnapshot().fallbackRetiredRanges == 1u &&
		agingPulses.GetSnapshot().fallbackRetiredMass == 1u,
		"analytic expiry must remain distinct from a committed grid deposit");
	Require(agingOwner.GetSnapshot().expiredFallbackRanges == 1u &&
		agingOwner.GetSnapshot().expiredFallbackMass == 1u &&
		agingOwner.GetSnapshot().expiryAcknowledgeFailures == 0u,
		"fallback expiry telemetry must close exact retired range and mass");
	agingPulses.Enqueue({ Interactive(501u) }, 13.0);
	auto replacement = agingPulses.PendingCommands();
	agingOwner.Prepare(replacement, agingPulses, 502u, 13.0, styles, 8.0f, retained, 1u);
	Require(replacement.size() == 1u,
		"an expired fallback must release its fixed slot for later interactive smoke");

	NRISmokePromptFallback smallSourceOwner;
	auto smallSource = std::vector<NRISmokeInjectionCommandGpu> { Interactive(600u) };
	smallSource[0].spawnRadius = 1.5f;
	smallSource[0].radiusScale = 0.025f;
	smallSourceOwner.Prepare(smallSource, pulses, 600u, 20.0, styles, 8.0f, retained, 1u);
	Require(std::abs(smallSource[0].spawnRadius - 8.0f) < 1e-5f,
		"sub-cell muzzle fallback support must match the grid's one-cell minimum");

	NRISmokePulseOwner deferredPulses;
	NRISmokePromptFallback deferredOwner;
	deferredPulses.Enqueue({ Interactive(700u) }, 10.0);
	auto deferredSelection = deferredPulses.PendingCommands();
	deferredOwner.Prepare(deferredSelection, deferredPulses, 700u, 10.0,
		styles, 8.0f, retained, 1u);
	std::vector<NRISmokeInjectionCommandGpu> deferredPlan;
	uint64_t deferredToken = 0u;
	Require(deferredPulses.Plan(deferredSelection, deferredPlan, deferredToken) &&
		deferredPulses.CommitRetaining(deferredToken, deferredPlan),
		"the blocker must retain the sole prompt slot");
	deferredOwner.Commit(700u);
	deferredPulses.Enqueue({ Interactive(701u) }, 10.25);
	deferredSelection = deferredPulses.PendingCommands();
	const auto blockedResult = deferredOwner.Prepare(deferredSelection, deferredPulses,
		701u, 10.25, styles, 8.0f, retained, 1u);
	Require(deferredSelection.size() == 1u && blockedResult.deferredRanges == 1u,
		"a newcomer must remain pending while the sole sticky slot is occupied");
	Require(deferredPulses.Plan(deferredSelection, deferredPlan, deferredToken) &&
		deferredPulses.CommitRetaining(deferredToken, deferredPlan),
		"the blocker retry must preserve both pending pulse clocks");
	deferredOwner.Commit(701u);
	NRISmokePromptOutcomeGpu blockerOutcome = {};
	blockerOutcome.pulseIdLow = deferredSelection[0].pulseIdLow;
	blockerOutcome.pulseIdHigh = deferredSelection[0].pulseIdHigh;
	blockerOutcome.rangeBegin = deferredSelection[0].rangeBegin;
	blockerOutcome.rangeCount = deferredSelection[0].rangeCount;
	blockerOutcome.outcome = (uint32_t)NRISmokePromptOutcome::GridCommitted;
	deferredOwner.CommitGridHandoffs(deferredPulses, { blockerOutcome });
	deferredSelection = deferredPulses.PendingCommands();
	deferredOwner.Prepare(deferredSelection, deferredPulses, 702u, 11.5,
		styles, 8.0f, retained, 1u);
	Require(deferredSelection.size() == 1u &&
		std::abs(deferredSelection[0].spawnRadius - 12.5f) < 1e-5f &&
		std::abs(deferredSelection[0].densityScale - std::exp2(-1.25f)) < 1e-5f,
		"a deferred pulse must enter fallback at its authored age instead of being reborn");
	Require(deferredPulses.GetSnapshot().authoredClockCount == 1u,
		"the completed blocker's clock must be released while the deferred pulse retains its clock");

	auto expiryStyles = styles;
	expiryStyles.push_back(styles[0]);
	expiryStyles[1].lifetime = 1.0f;
	NRISmokePulseOwner stalePulses;
	NRISmokePromptFallback staleOwner;
	auto longBlocker = Interactive(800u);
	auto staleCommand = Interactive(801u);
	staleCommand.styleIndex = 1u;
	stalePulses.Enqueue({ longBlocker }, 20.0);
	stalePulses.Enqueue({ staleCommand }, 20.1);
	auto staleSelection = stalePulses.PendingCommands();
	staleOwner.Prepare(staleSelection, stalePulses, 800u, 20.1,
		expiryStyles, 8.0f, retained, 1u);
	Require(staleSelection.size() == 1u,
		"the short-lived fixture must begin deferred behind a long-lived blocker");
	std::vector<NRISmokeInjectionCommandGpu> stalePlan;
	uint64_t staleToken = 0u;
	Require(stalePulses.Plan(staleSelection, stalePlan, staleToken) &&
		stalePulses.CommitRetaining(staleToken, stalePlan),
		"the stale fixture must retain the blocker transaction");
	staleOwner.Commit(800u);
	staleOwner.RetireExpired(stalePulses, 21.2, expiryStyles);
	Require(stalePulses.PendingCommands().size() == 1u &&
		stalePulses.PendingCommands()[0].styleIndex == 0u,
		"a never-published pulse must expire while deferred instead of appearing belatedly");
	Require(stalePulses.GetSnapshot().deferredExpiredRanges == 1u &&
		stalePulses.GetSnapshot().deferredExpiredMass == 1u &&
		staleOwner.GetSnapshot().expiredDeferredRanges == 1u &&
		staleOwner.GetSnapshot().expiredDeferredMass == 1u,
		"deferred expiry must publish exact range and mass accounting");

	NRISmokePulseOwner rebasedPulses;
	rebasedPulses.Enqueue({ Interactive(900u) }, 8.0);
	rebasedPulses.RebaseSimulationClock(10.0, 0.0);
	Require(std::abs(rebasedPulses.AuthoredSimulationSeconds(
		rebasedPulses.PendingCommands()[0], 99.0) + 2.0) < 1e-9,
		"authority clock rebase must preserve the pulse's existing simulation age");
	rebasedPulses.Reset();
	Require(rebasedPulses.GetSnapshot().authoredClockCount == 0u,
		"pulse reset must release every authored-time sidecar entry");

	NRISmokePulseOwner visiblePulses;
	NRISmokePromptFallback visibleOwner;
	auto visibleCommand = Interactive(950u);
	visibleCommand.sourceId = 42u;
	visiblePulses.Enqueue({ visibleCommand }, { Latest(30.0) }, 12.0, 30.0);
	auto visibleSelection = visiblePulses.PendingCommands();
	visibleOwner.Prepare(visibleSelection, visiblePulses, 950u, 12.0,
		styles, 8.0f, retained, 1u);
	std::vector<NRISmokeInjectionCommandGpu> visiblePlan;
	uint64_t visibleToken = 0u;
	Require(visiblePulses.Plan(visibleSelection, visiblePlan, visibleToken) &&
		visiblePulses.CommitRetaining(visibleToken, visiblePlan),
		"the visibility fixture must retain prompt authority through dispatch");
	visibleOwner.Commit(950u, visiblePulses);
	auto laterCommand = Interactive(951u);
	laterCommand.sourceId = 42u;
	visiblePulses.Enqueue({ laterCommand }, { Latest(30.1) }, 12.1, 30.1);
	Require(visiblePulses.PendingCommands().size() == 2u &&
		visiblePulses.GetSnapshot().supersededPulses == 0u,
		"a committed prompt dispatch must protect visible work from latest replacement");

	std::cout << "Smoke prompt sticky-slot transaction tests passed.\n";
	return 0;
}
