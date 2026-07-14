#pragma once

#include <cstdint>

enum class NRISmokeAuthorityMode : uint32_t
{
	Disabled = 0,
	Particles,
	Grid,
	Compare,
};

struct NRISmokeAuthorityRequest
{
	bool enabled = false;
	uint32_t requestedRepresentation = 0;
	bool gridReady = false;
	bool worldLightingRequired = false;
	bool worldLightingReady = false;
};

struct NRISmokeAuthorityDecision
{
	NRISmokeAuthorityMode mode = NRISmokeAuthorityMode::Disabled;
	uint32_t effectiveRepresentation = 0;
	const char* reason = "disabled";
	const char* fallback = "none";
};

struct NRISmokeAuthoritySnapshot
{
	NRISmokeAuthorityMode mode = NRISmokeAuthorityMode::Disabled;
	uint32_t requestedRepresentation = 0;
	uint32_t effectiveRepresentation = 0;
	uint32_t transitionSerial = 0;
	uint32_t transitionFrame = UINT32_MAX;
	bool operational = false;
	bool worldLightingRequired = false;
	const char* reason = "initial";
	const char* fallback = "none";
};

class NRISmokeAuthority
{
public:
	NRISmokeAuthorityDecision Resolve(const NRISmokeAuthorityRequest& request) const;
	bool Commit(const NRISmokeAuthorityRequest& request, const NRISmokeAuthorityDecision& decision,
		uint32_t frameIndex);
	void Disable(uint32_t requestedRepresentation, uint32_t frameIndex, const char* reason);

	const NRISmokeAuthoritySnapshot& GetSnapshot() const { return mSnapshot; }
	bool RequiresParticles(const NRISmokeAuthorityDecision& decision) const;
	bool RequiresGrid(const NRISmokeAuthorityDecision& decision) const;
	static const char* ModeName(NRISmokeAuthorityMode mode);

private:
	NRISmokeAuthoritySnapshot mSnapshot = {};
};
