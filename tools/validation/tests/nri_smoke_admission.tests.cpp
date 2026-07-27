#include "nri_smoke_admission.h"

#include <cstdlib>
#include <iostream>
#include <set>
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

NRISmokeInjectionCommandGpu Command(NRISmokeInjectionSourceClass sourceClass,
	uint32_t sourceId, uint32_t serial, float radius = 8.0f)
{
	NRISmokeInjectionCommandGpu command = {};
	command.sourceMetadata = NRIPackSmokeSourceMetadata(sourceClass);
	command.sourceId = sourceId;
	command.serial = serial;
	command.spawnRadius = radius;
	command.radiusScale = 1.0f;
	return command;
}
}

int main()
{
	static_assert(sizeof(NRISmokeInjectionCommandGpu) == 112u);
	static_assert(offsetof(NRISmokeInjectionCommandGpu, sourceId) == 84u);
	static_assert(offsetof(NRISmokeInjectionCommandGpu, sourceSlot) == 88u);
	static_assert(offsetof(NRISmokeInjectionCommandGpu, sourceMetadata) == 92u);
	static_assert(offsetof(NRISmokeInjectionCommandGpu, rangeBegin) == 96u);
	static_assert(offsetof(NRISmokeInjectionCommandGpu, rangeCount) == 100u);
	Require(NRIGetSmokeSourceClass(NRIPackSmokeSourceMetadata(
		NRISmokeInjectionSourceClass::InteractiveEvent, 17u)) ==
		NRISmokeInjectionSourceClass::InteractiveEvent,
		"packed metadata must preserve source class");
	Require(NRIMakeSmokeSourceId("MAP", "E1L1", "roof") ==
		NRIMakeSmokeSourceId("map", "e1l1", "ROOF"),
		"stable source identity must be ASCII case insensitive");

	std::vector<NRISmokeInjectionCommandGpu> gathered;
	for (uint32_t source = 0u; source < 300u; ++source)
		gathered.push_back(Command(NRISmokeInjectionSourceClass::AmbientMap, 1000u + source, source));
	const uint32_t eventId = NRIMakeSmokeSourceId("event", "e1l1", "rpg");
	gathered.push_back(Command(NRISmokeInjectionSourceClass::InteractiveEvent, eventId, 500u));

	NRISmokeAdmissionScheduler firstScheduler;
	std::vector<NRISmokeInjectionCommandGpu> selected0;
	const auto first = firstScheduler.SelectGridCommands(gathered, 256u, 0u, 8.0f, selected0);
	Require(first.Closes() && first.gathered == 301u && first.uploaded == 256u && first.rejected == 45u,
		"every gathered command must close to upload or explicit terminal rejection");
	Require(first.interactiveGathered == 1u && first.interactiveUploaded == 1u,
		"interactive first use must survive an over-cap ambient batch");
	Require(std::any_of(selected0.begin(), selected0.end(), [eventId](const auto& command)
		{ return command.sourceId == eventId; }),
		"selected batch must contain the interactive event");

	std::reverse(gathered.begin(), gathered.end());
	NRISmokeAdmissionScheduler permutationScheduler;
	std::vector<NRISmokeInjectionCommandGpu> permuted;
	permutationScheduler.SelectGridCommands(gathered, 256u, 0u, 8.0f, permuted);
	Require(permuted.size() == selected0.size(), "permuted result must retain capacity");
	for (uint32_t index = 0u; index < selected0.size(); ++index)
		Require(permuted[index].sourceId == selected0[index].sourceId &&
			permuted[index].serial == selected0[index].serial &&
			permuted[index].sourceSlot == selected0[index].sourceSlot,
			"source service must not depend on gathered rule order");

	std::reverse(gathered.begin(), gathered.end());
	std::vector<NRISmokeInjectionCommandGpu> selected1;
	firstScheduler.SelectGridCommands(gathered, 256u, 1u, 8.0f, selected1);
	std::set<uint32_t> served;
	for (const auto& command : selected0) served.insert(command.sourceId);
	for (const auto& command : selected1) served.insert(command.sourceId);
	Require(served.size() > selected0.size(),
		"frame rotation must advance service through an over-cap source set");

	std::cout << "Smoke admission metadata and CPU fairness tests passed.\n";
	return 0;
}
