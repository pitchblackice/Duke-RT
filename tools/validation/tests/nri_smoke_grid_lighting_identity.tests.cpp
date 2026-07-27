#include "nri_smoke_grid_lighting_identity.h"

#include <cstdlib>
#include <iostream>
#include <limits>

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

NRISmokeGridLightingFrameIdentity BaseIdentity()
{
	NRISmokeGridLightingFrameIdentity identity = {};
	identity.smokeEpoch = 7;
	identity.unifiedTlasPayloadHash = 11;
	identity.unifiedTlasScenePayloadHash = 12;
	identity.unifiedTlasInstanceCount = 13;
	identity.worldBlasContentGeneration = 14;
	identity.sceneDescriptorGeneration = 15;
	identity.materialGeneration = 16;
	identity.portalPayloadHash = 17;
	identity.portalDepth = 3;
	identity.visibilityBackend = 2;
	identity.visibilityFilterIdentity = 18;
	identity.emissivePayloadIdentity = 19;
	identity.emissiveEstimatorKey = 20;
	return identity;
}

void RequireOnlyTopologyChanges(NRISmokeGridLightingIdentity& owner,
	const NRISmokeGridLightingFrameIdentity& identity, const char* message)
{
	const auto before = owner.Update(BaseIdentity());
	const auto after = owner.Update(identity);
	Require(after.topologyChanged && !after.proposalChanged &&
		after.topologyGeneration == before.topologyGeneration + 1u &&
		after.proposalGeneration == before.proposalGeneration, message);
}
}

int main()
{
	const auto base = BaseIdentity();
	NRISmokeGridLightingIdentity stable;
	const auto first = stable.Update(base);
	Require(first.topologyChanged && first.proposalChanged &&
		first.topologyGeneration == 1u && first.proposalGeneration == 1u &&
		first.topologyCacheable && first.proposalCacheable,
		"first identity must publish nonzero cache generations");
	const auto unchanged = stable.Update(base);
	Require(!unchanged.topologyChanged && !unchanged.proposalChanged &&
		unchanged.topologyGeneration == 1u && unchanged.proposalGeneration == 1u,
		"unchanged input must retain both generations");

	auto changed = base;
	changed.unifiedTlasPayloadHash++;
	RequireOnlyTopologyChanges(stable, changed, "TLAS payload changes must invalidate topology only");
	changed = base; changed.unifiedTlasScenePayloadHash++;
	RequireOnlyTopologyChanges(stable, changed, "TLAS scene payload changes must invalidate topology only");
	changed = base; changed.unifiedTlasInstanceCount++;
	RequireOnlyTopologyChanges(stable, changed, "TLAS count changes must invalidate topology only");
	changed = base; changed.worldBlasContentGeneration++;
	RequireOnlyTopologyChanges(stable, changed, "BLAS changes must invalidate topology only");
	changed = base; changed.sceneDescriptorGeneration++;
	RequireOnlyTopologyChanges(stable, changed, "descriptor changes must invalidate topology only");
	changed = base; changed.materialGeneration++;
	RequireOnlyTopologyChanges(stable, changed, "material changes must invalidate topology only");
	changed = base; changed.portalPayloadHash++;
	RequireOnlyTopologyChanges(stable, changed, "portal payload changes must invalidate topology only");
	changed = base; changed.portalDepth++;
	RequireOnlyTopologyChanges(stable, changed, "portal-depth changes must invalidate topology only");
	changed = base; changed.visibilityBackend++;
	RequireOnlyTopologyChanges(stable, changed, "visibility backend changes must invalidate topology only");
	changed = base; changed.visibilityFilterIdentity++;
	RequireOnlyTopologyChanges(stable, changed, "visibility-filter changes must invalidate topology only");

	NRISmokeGridLightingIdentity proposal;
	const auto proposalFirst = proposal.Update(base);
	changed = base;
	changed.emissivePayloadIdentity++;
	auto proposalChanged = proposal.Update(changed);
	Require(!proposalChanged.topologyChanged && proposalChanged.proposalChanged &&
		proposalChanged.topologyGeneration == proposalFirst.topologyGeneration &&
		proposalChanged.proposalGeneration == proposalFirst.proposalGeneration + 1u,
		"emissive payload changes must invalidate proposals only");
	changed.emissiveEstimatorKey++;
	proposalChanged = proposal.Update(changed);
	Require(!proposalChanged.topologyChanged && proposalChanged.proposalChanged,
		"estimator changes must invalidate proposals only");

	changed.smokeEpoch++;
	const auto epochChanged = proposal.Update(changed);
	Require(epochChanged.topologyChanged && epochChanged.proposalChanged,
		"smoke epoch changes must invalidate both cache families");
	proposal.Reset();
	const auto reset = proposal.Update(changed);
	Require(reset.topologyChanged && reset.proposalChanged &&
		reset.topologyGeneration == epochChanged.topologyGeneration + 1u &&
		reset.proposalGeneration == epochChanged.proposalGeneration + 1u,
		"reset must advance both generations even for unchanged input");

	uint32_t next = 0;
	Require(NRIAdvanceSmokeGridLightingGeneration(0u, next) && next == 1u,
		"zero must advance to the first nonzero identity");
	Require(!NRIAdvanceSmokeGridLightingGeneration(std::numeric_limits<uint32_t>::max(), next) &&
		next == std::numeric_limits<uint32_t>::max(),
		"generation wrap must fail closed without recycling an identity");

	std::cout << "Smoke grid lighting identity policy tests passed.\n";
	return 0;
}
