#include "nri_smoke_authority.h"

#include <algorithm>

NRISmokeAuthorityDecision NRISmokeAuthority::Resolve(const NRISmokeAuthorityRequest& request) const
{
	NRISmokeAuthorityDecision decision = {};
	if (!request.enabled)
		return decision;

	const uint32_t requested = std::min(request.requestedRepresentation, 2u);
	if (requested == 0u)
	{
		decision.mode = NRISmokeAuthorityMode::Particles;
		decision.reason = "particle-requested";
		return decision;
	}
	if (!request.gridReady)
	{
		decision.mode = NRISmokeAuthorityMode::Particles;
		decision.reason = "particle-grid-unavailable";
		decision.fallback = "grid-unavailable";
		return decision;
	}
	if (request.worldLightingRequired && !request.worldLightingReady)
	{
		decision.mode = NRISmokeAuthorityMode::Particles;
		decision.reason = "particle-world-lighting-unavailable";
		decision.fallback = "world-lighting-unavailable";
		return decision;
	}

	decision.mode = requested == 2u ? NRISmokeAuthorityMode::Compare : NRISmokeAuthorityMode::Grid;
	decision.effectiveRepresentation = requested;
	decision.reason = requested == 2u ? "compare-ready" : "grid-ready";
	return decision;
}

bool NRISmokeAuthority::Commit(const NRISmokeAuthorityRequest& request,
	const NRISmokeAuthorityDecision& decision, uint32_t frameIndex)
{
	const bool changed = !mSnapshot.operational || mSnapshot.mode != decision.mode;
	mSnapshot.requestedRepresentation = std::min(request.requestedRepresentation, 2u);
	mSnapshot.effectiveRepresentation = decision.effectiveRepresentation;
	mSnapshot.mode = decision.mode;
	mSnapshot.operational = decision.mode != NRISmokeAuthorityMode::Disabled;
	mSnapshot.worldLightingRequired = request.worldLightingRequired;
	mSnapshot.reason = decision.reason;
	mSnapshot.fallback = decision.fallback;
	if (changed)
	{
		mSnapshot.transitionSerial++;
		mSnapshot.transitionFrame = frameIndex;
	}
	return changed;
}

void NRISmokeAuthority::Disable(uint32_t requestedRepresentation, uint32_t frameIndex, const char* reason)
{
	const bool changed = mSnapshot.operational || mSnapshot.mode != NRISmokeAuthorityMode::Disabled;
	mSnapshot.requestedRepresentation = std::min(requestedRepresentation, 2u);
	mSnapshot.effectiveRepresentation = 0u;
	mSnapshot.mode = NRISmokeAuthorityMode::Disabled;
	mSnapshot.operational = false;
	mSnapshot.worldLightingRequired = false;
	mSnapshot.reason = reason != nullptr ? reason : "disabled";
	mSnapshot.fallback = "none";
	if (changed)
	{
		mSnapshot.transitionSerial++;
		mSnapshot.transitionFrame = frameIndex;
	}
}

bool NRISmokeAuthority::RequiresParticles(const NRISmokeAuthorityDecision& decision) const
{
	return decision.mode == NRISmokeAuthorityMode::Particles || decision.mode == NRISmokeAuthorityMode::Compare;
}

bool NRISmokeAuthority::RequiresGrid(const NRISmokeAuthorityDecision& decision) const
{
	return decision.mode == NRISmokeAuthorityMode::Grid || decision.mode == NRISmokeAuthorityMode::Compare;
}

const char* NRISmokeAuthority::ModeName(NRISmokeAuthorityMode mode)
{
	switch (mode)
	{
	case NRISmokeAuthorityMode::Particles: return "particles";
	case NRISmokeAuthorityMode::Grid: return "grid";
	case NRISmokeAuthorityMode::Compare: return "compare";
	default: return "disabled";
	}
}
