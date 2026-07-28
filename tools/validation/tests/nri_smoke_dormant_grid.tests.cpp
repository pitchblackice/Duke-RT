#include "nri_smoke_dormant_grid_contracts.h"

#include <cassert>
#include <cstdint>

int main()
{
	using Outcome = NRISmokeDormantGridOutcome;
	for (uint32_t raw = (uint32_t)Outcome::None;
		raw <= (uint32_t)Outcome::FineActiveCapacity; ++raw)
	{
		const Outcome outcome = (Outcome)raw;
		assert(NRISmokeDormantGridMayReleaseFine(outcome) ==
			(outcome == Outcome::Archived));
		assert(NRISmokeDormantGridMayRetireCoarse(outcome) ==
			(outcome == Outcome::Rehydrated));
	}

	// Archive-full, validation, stale-generation, and interrupted allocation
	// outcomes all retain the currently published authority.
	assert(!NRISmokeDormantGridMayReleaseFine(Outcome::ArchiveFull));
	assert(!NRISmokeDormantGridMayReleaseFine(Outcome::ValidationFailure));
	assert(!NRISmokeDormantGridMayReleaseFine(Outcome::StaleGeneration));
	assert(!NRISmokeDormantGridMayRetireCoarse(Outcome::FineCapacity));
	assert(!NRISmokeDormantGridMayRetireCoarse(Outcome::FineActiveCapacity));
	assert(!NRISmokeDormantGridMayRetireCoarse(Outcome::HashFailure));
	assert(!NRISmokeDormantGridShouldCompareExpectedMass(
		NRISmokeDormantGridWorkFlag_None));
	assert(NRISmokeDormantGridShouldCompareExpectedMass(
		NRISmokeDormantGridWorkFlag_MassKnown));

	// The first implementation preserves all four float4 fields from the final
	// fine ping. This is 32 KiB per record, versus roughly 96 KiB released from
	// two fine pings plus four deposit fields.
	assert(NRISmokeDormantGridPayloadBytes(1u) == 32u * 1024u);
	assert(NRISmokeDormantGridPayloadBytes(256u) == 8u * 1024u * 1024u);
	return 0;
}
