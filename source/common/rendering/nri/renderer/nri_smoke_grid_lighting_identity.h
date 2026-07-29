#pragma once

#include <cstdint>

struct NRISmokeGridLightingFrameIdentity
{
	uint32_t smokeEpoch = 0;

	uint64_t unifiedTlasPayloadHash = 0;
	uint64_t unifiedTlasScenePayloadHash = 0;
	uint32_t unifiedTlasInstanceCount = 0;
	uint64_t worldBlasContentGeneration = 0;
	uint64_t sceneDescriptorGeneration = 0;
	uint64_t materialGeneration = 0;
	uint64_t portalPayloadHash = 0;
	uint32_t portalDepth = 0;
	uint32_t visibilityBackend = 0;
	uint64_t visibilityFilterIdentity = 0;

	uint64_t emissivePayloadIdentity = 0;
	uint32_t emissiveEstimatorKey = 0;
};

struct NRISmokeGridLightingIdentitySnapshot
{
	uint32_t topologyGeneration = 0;
	uint32_t proposalGeneration = 0;
	bool topologyChanged = false;
	bool proposalChanged = false;
	bool topologyCacheable = false;
	bool proposalCacheable = false;
};

// Returns false instead of recycling a nonzero GPU identity after wrap.
bool NRIAdvanceSmokeGridLightingGeneration(uint32_t current, uint32_t& next);

class NRISmokeGridLightingIdentity
{
public:
	NRISmokeGridLightingIdentitySnapshot Update(const NRISmokeGridLightingFrameIdentity& identity);
	void Reset();

private:
	bool TopologyChanged(const NRISmokeGridLightingFrameIdentity& identity) const;
	bool ProposalChanged(const NRISmokeGridLightingFrameIdentity& identity) const;
	bool AdvanceTopology();
	bool AdvanceProposal();

	NRISmokeGridLightingFrameIdentity mLastIdentity = {};
	uint32_t mTopologyGeneration = 0;
	uint32_t mProposalGeneration = 0;
	bool mHasIdentity = false;
	bool mForceReset = false;
	bool mTopologyCacheable = true;
	bool mProposalCacheable = true;
};
