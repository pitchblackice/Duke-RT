#include "nri_smoke_pulses.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
void Require(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAILED: " << message << '\n';
		std::exit(1);
	}
}

NRISmokeInjectionCommandGpu Command(uint32_t serial, uint32_t count, uint32_t epoch = 7u)
{
	NRISmokeInjectionCommandGpu command = {};
	command.serial = serial;
	command.count = count;
	command.epoch = epoch;
	command.sourceId = 1000u + serial;
	return command;
}

uint64_t PulseId(const NRISmokeInjectionCommandGpu& command)
{
	return (uint64_t(command.pulseIdHigh) << 32u) | command.pulseIdLow;
}
}

int main()
{
	static_assert(sizeof(NRISmokeInjectionCommandGpu) == 112u);
	static_assert(offsetof(NRISmokeInjectionCommandGpu, rangeBegin) == 96u);
	static_assert(offsetof(NRISmokeInjectionCommandGpu, rangeCount) == 100u);
	static_assert(offsetof(NRISmokeInjectionCommandGpu, pulseIdLow) == 104u);
	static_assert(offsetof(NRISmokeInjectionCommandGpu, pulseIdHigh) == 108u);

	NRISmokePulseOwner owner;
	owner.Enqueue({ Command(1u, 10u), Command(2u, 5u) });
	Require(owner.PendingCommands().size() == 2u && owner.GetSnapshot().pendingMass == 15u,
		"enqueue must retain every authored pulse and nominal mass");
	Require(PulseId(owner.PendingCommands()[0]) != 0u &&
		PulseId(owner.PendingCommands()[0]) != PulseId(owner.PendingCommands()[1]),
		"each authored pulse must receive a stable nonzero identity");
	Require(owner.PendingCommands()[0].rangeBegin == 0u && owner.PendingCommands()[0].rangeCount == 10u,
		"an authored pulse must begin as one complete range");

	auto middle = owner.PendingCommands()[0];
	middle.rangeBegin = 2u;
	middle.rangeCount = 4u;
	std::vector<NRISmokeInjectionCommandGpu> planned;
	uint64_t token = 0u;
	Require(owner.Plan({ middle }, planned, token) && token != 0u && planned.size() == 1u,
		"a contained pulse range must produce an immutable plan token");
	Require(owner.GetSnapshot().pendingMass == 15u && owner.GetSnapshot().planActive,
		"planning must not advance pending mass");
	Require(owner.Rollback(token), "the active plan must roll back with its exact token");
	Require(owner.GetSnapshot().pendingMass == 15u && !owner.GetSnapshot().planActive,
		"rollback must preserve all pending progress");
	Require(!owner.Commit(token), "a rolled-back token must not commit later");

	Require(owner.Plan({ middle }, planned, token), "the rolled-back range must be replannable");
	Require(owner.Commit(token), "the replanned range must commit");
	Require(owner.PendingCommands().size() == 3u && owner.GetSnapshot().pendingMass == 11u,
		"committing a middle range must retain disjoint left/right ranges and exact mass");
	Require(owner.PendingCommands()[0].rangeBegin == 0u && owner.PendingCommands()[0].rangeCount == 2u &&
		owner.PendingCommands()[1].rangeBegin == 6u && owner.PendingCommands()[1].rangeCount == 4u,
		"committed progress must split the pending pulse without overlap");

	auto overlapA = owner.PendingCommands()[1];
	overlapA.rangeCount = 3u;
	auto overlapB = overlapA;
	overlapB.rangeBegin++;
	overlapB.rangeCount = 2u;
	Require(!owner.Plan({ overlapA, overlapB }, planned, token),
		"one plan must reject overlapping ranges of the same pulse");
	Require(!owner.GetSnapshot().planActive && owner.GetSnapshot().pendingMass == 11u,
		"a rejected plan must not mutate queue state");

	NRISmokePulseOwner promptOwner;
	promptOwner.Enqueue({ Command(20u, 6u) });
	auto promptRange = promptOwner.PendingCommands()[0];
	promptRange.rangeBegin = 1u;
	promptRange.rangeCount = 3u;
	Require(promptOwner.Plan({ promptRange }, planned, token),
		"a prompt range must be transactionally plannable");
	Require(promptOwner.CommitRetaining(token, { promptRange }) &&
		promptOwner.GetSnapshot().pendingMass == 6u,
		"fallback authority must retain all nominal mass until GPU grid acknowledgement");
	Require(promptOwner.Acknowledge(promptRange.pulseIdLow, promptRange.pulseIdHigh,
		promptRange.rangeBegin, promptRange.rangeCount),
		"the exact stable prompt identity must accept one grid handoff");
	Require(promptOwner.GetSnapshot().pendingMass == 3u &&
		promptOwner.GetSnapshot().committedMass == 3u,
		"prompt handoff must close the acknowledged mass exactly");
	Require(!promptOwner.Acknowledge(promptRange.pulseIdLow, promptRange.pulseIdHigh,
		promptRange.rangeBegin, promptRange.rangeCount),
		"a delayed duplicate grid acknowledgement must not consume mass twice");

	owner.RebaseEpoch(12u);
	for (const auto& command : owner.PendingCommands())
		Require(command.epoch == 12u, "epoch rebasing must preserve ranges while updating compatibility");

	std::vector<NRISmokeInjectionCommandGpu> passThrough = owner.PendingCommands();
	Require(owner.Plan(passThrough, planned, token), "all pending ranges must support pass-through planning");
	uint64_t plannedMass = 0u;
	for (const auto& command : planned) plannedMass += command.rangeCount;
	Require(plannedMass == 11u, "pass-through planning must conserve pending nominal mass");
	Require(owner.Commit(token) && owner.PendingCommands().empty(),
		"committing the pass-through plan must drain the queue exactly once");
	Require(owner.GetSnapshot().enqueuedMass == owner.GetSnapshot().committedMass &&
		owner.GetSnapshot().pendingMass == 0u,
		"committed plus pending mass must close to authored mass");

	owner.Enqueue({ Command(3u, 7u, 12u) });
	const uint32_t resetCount = owner.Reset();
	Require(resetCount == 1u && owner.GetSnapshot().resetMass == 7u && owner.PendingCommands().empty(),
		"reset must explicitly account for every discarded pulse and unit of mass");

	std::cout << "Smoke pulse transaction and ranged-mass tests passed.\n";
	return 0;
}
