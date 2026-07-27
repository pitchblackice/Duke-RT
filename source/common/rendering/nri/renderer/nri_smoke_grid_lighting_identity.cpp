#include "nri_smoke_grid_lighting_identity.h"

#include <limits>

bool NRIAdvanceSmokeGridLightingGeneration(uint32_t current, uint32_t& next)
{
	if (current == std::numeric_limits<uint32_t>::max())
	{
		next = current;
		return false;
	}
	next = current + 1u;
	return next != 0u;
}

bool NRISmokeGridLightingIdentity::TopologyChanged(const NRISmokeGridLightingFrameIdentity& identity) const
{
	return !mHasIdentity || mForceReset ||
		identity.smokeEpoch != mLastIdentity.smokeEpoch ||
		identity.unifiedTlasPayloadHash != mLastIdentity.unifiedTlasPayloadHash ||
		identity.unifiedTlasScenePayloadHash != mLastIdentity.unifiedTlasScenePayloadHash ||
		identity.unifiedTlasInstanceCount != mLastIdentity.unifiedTlasInstanceCount ||
		identity.worldBlasContentGeneration != mLastIdentity.worldBlasContentGeneration ||
		identity.sceneDescriptorGeneration != mLastIdentity.sceneDescriptorGeneration ||
		identity.materialGeneration != mLastIdentity.materialGeneration ||
		identity.portalPayloadHash != mLastIdentity.portalPayloadHash ||
		identity.portalDepth != mLastIdentity.portalDepth ||
		identity.visibilityBackend != mLastIdentity.visibilityBackend ||
		identity.visibilityFilterIdentity != mLastIdentity.visibilityFilterIdentity;
}

bool NRISmokeGridLightingIdentity::ProposalChanged(const NRISmokeGridLightingFrameIdentity& identity) const
{
	return !mHasIdentity || mForceReset ||
		identity.smokeEpoch != mLastIdentity.smokeEpoch ||
		identity.emissivePayloadIdentity != mLastIdentity.emissivePayloadIdentity ||
		identity.emissiveEstimatorKey != mLastIdentity.emissiveEstimatorKey;
}

bool NRISmokeGridLightingIdentity::AdvanceTopology()
{
	uint32_t next = mTopologyGeneration;
	if (!mTopologyCacheable || !NRIAdvanceSmokeGridLightingGeneration(mTopologyGeneration, next))
	{
		mTopologyCacheable = false;
		return false;
	}
	mTopologyGeneration = next;
	return true;
}

bool NRISmokeGridLightingIdentity::AdvanceProposal()
{
	uint32_t next = mProposalGeneration;
	if (!mProposalCacheable || !NRIAdvanceSmokeGridLightingGeneration(mProposalGeneration, next))
	{
		mProposalCacheable = false;
		return false;
	}
	mProposalGeneration = next;
	return true;
}

NRISmokeGridLightingIdentitySnapshot NRISmokeGridLightingIdentity::Update(
	const NRISmokeGridLightingFrameIdentity& identity)
{
	NRISmokeGridLightingIdentitySnapshot snapshot = {};
	snapshot.topologyChanged = TopologyChanged(identity);
	snapshot.proposalChanged = ProposalChanged(identity);
	if (snapshot.topologyChanged)
		AdvanceTopology();
	if (snapshot.proposalChanged)
		AdvanceProposal();

	mLastIdentity = identity;
	mHasIdentity = true;
	mForceReset = false;
	snapshot.topologyGeneration = mTopologyGeneration;
	snapshot.proposalGeneration = mProposalGeneration;
	snapshot.topologyCacheable = mTopologyCacheable && mTopologyGeneration != 0u;
	snapshot.proposalCacheable = mProposalCacheable && mProposalGeneration != 0u;
	return snapshot;
}

void NRISmokeGridLightingIdentity::Reset()
{
	mForceReset = true;
}
