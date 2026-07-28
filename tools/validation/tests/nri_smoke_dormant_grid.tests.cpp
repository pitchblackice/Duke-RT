#include "nri_smoke_dormant_grid_contracts.h"

#include <cassert>
#include <cmath>
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

	assert(sizeof(NRISmokeDormantGridInjectionGpu) == 112u);
	assert(offsetof(NRISmokeDormantGridInjectionGpu, scalar) == 48u);
	assert(offsetof(NRISmokeDormantGridInjectionGpu, dynamics) == 96u);
	assert(sizeof(NRISmokeDormantGridControlGpu) == 160u);
	assert(NRISmokeDormantGridInjectionRequiresEstablishedAuthority(
		NRISmokeDormantGridInjectionFlag_EstablishedAuthority));
	assert(!NRISmokeDormantGridInjectionRequiresEstablishedAuthority(
		NRISmokeDormantGridInjectionFlag_None));

	// Round-robin selection is a pure function of frame and the fixed work
	// quantity. It cannot expand in response to timing headroom.
	assert(NRISmokeDormantGridEvolutionBase(0u, 4u, 16u) == 0u);
	assert(NRISmokeDormantGridEvolutionBase(1u, 4u, 16u) == 4u);
	assert(NRISmokeDormantGridEvolutionBase(4u, 4u, 16u) == 0u);

	// Density and temperature moments use exponential decay over all elapsed
	// frames. Advection weights sum to one, including edge clamping, so no
	// archive mass is lost merely because transport reaches a brick boundary.
	const float density = NRISmokeDormantGridDecay(0.5f, 2.0f);
	const float temperature = density * NRISmokeDormantGridDecay(0.25f, 2.0f);
	assert(std::abs(density - std::exp(-1.0f)) < 1e-6f);
	assert(std::abs(temperature - std::exp(-1.5f)) < 1e-6f);
	for (int32_t source = 0; source < 8; ++source)
	{
		for (float displacement : { -0.95f, -0.25f, 0.0f, 0.4f, 0.95f })
		{
			float sum = 0.0f;
			for (int32_t destination = 0; destination < 8; ++destination)
				sum += NRISmokeDormantGridAxisTransportWeight(source, destination,
					displacement);
			assert(std::abs(sum - 1.0f) < 1e-6f);
		}
	}
	return 0;
}
