#pragma once

#include "nri_smoke_contracts.h"

#include <cstdint>
#include <unordered_set>
#include <vector>

class NRISmokeEmitterSystem
{
public:
	void Gather(uint32_t epoch, std::vector<NRISmokeStyleGpu>& styles, std::vector<NRISmokeInjectionCommandGpu>& commands, uint32_t& nextSerial, uint32_t traceMode);
	void Reset();
	uint32_t GetGeneration() const { return mGeneration; }

private:
	struct Identity
	{
		uint32_t rule = 0;
		int32_t actorIndex = -1;
		const void* actor = nullptr;
		bool operator==(const Identity& other) const { return rule == other.rule && actorIndex == other.actorIndex && actor == other.actor; }
	};
	struct IdentityHash
	{
		size_t operator()(const Identity& value) const;
	};

	uint32_t mGeneration = 0;
	std::unordered_set<Identity, IdentityHash> mEmitted;
};
