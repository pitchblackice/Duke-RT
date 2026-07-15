#pragma once

#include "nri_smoke_contracts.h"
#include "v_video.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

class NRISmokeEmitterSystem
{
public:
	void Gather(uint32_t epoch, double gameplayTimeSeconds, const TArray<PathTracingWeaponLightEvent>& weaponEvents,
		std::vector<NRISmokeStyleGpu>& styles, std::vector<NRISmokeInjectionCommandGpu>& commands,
		uint32_t& nextSerial, uint32_t traceMode);
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
	struct ActorState
	{
		DVector3 previousPosition;
		double previousTimeSeconds = 0.0;
		float spacingRemainder = 0.0f;
		double intervalRemainder = 0.0;
		bool emitted = false;
		bool observed = false;
	};

	uint32_t mGeneration = 0;
	std::unordered_map<Identity, ActorState, IdentityHash> mActorStates;
};
