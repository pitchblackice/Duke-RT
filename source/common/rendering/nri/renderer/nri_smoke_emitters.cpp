#include "nri_smoke_emitters.h"

#include "coreactor.h"
#include "lightoverlay.h"

#include <algorithm>

size_t NRISmokeEmitterSystem::IdentityHash::operator()(const Identity& value) const
{
	size_t hash = (size_t)value.rule * 16777619u;
	hash ^= (size_t)(uint32_t)value.actorIndex + 0x9e3779b9u + (hash << 6) + (hash >> 2);
	hash ^= (size_t)value.actor + 0x9e3779b9u + (hash << 6) + (hash >> 2);
	return hash;
}

void NRISmokeEmitterSystem::Reset()
{
	mEmitted.clear();
	mGeneration = 0;
}

void NRISmokeEmitterSystem::Gather(uint32_t epoch, std::vector<NRISmokeStyleGpu>& styles, std::vector<NRISmokeInjectionCommandGpu>& commands, uint32_t& nextSerial)
{
	const ResolvedLightOverlaySet& resolved = GetResolvedLightOverlaySet();
	if (mGeneration != resolved.resolvedGeneration)
	{
		mGeneration = resolved.resolvedGeneration;
		mEmitted.clear();
	}
	styles.clear();
	styles.resize(std::max<uint32_t>(1u, resolved.smokeStyles.Size()));
	for (const auto& source : resolved.smokeStyles)
	{
		if (source.styleIndex >= styles.size()) continue;
		NRISmokeStyleGpu& target = styles[source.styleIndex];
		std::copy(source.albedo, source.albedo + 3, target.albedo);
		target.extinction = source.extinction;
		target.anisotropy = source.anisotropy;
		target.radius = source.radius;
		target.expansionVelocity = source.expansionVelocity;
		target.lifetime = source.lifetime;
		target.density = source.density;
		target.densityHalfLife = source.densityHalfLife;
		target.riseVelocity = source.riseVelocity;
		target.velocityRandom = source.velocityRandom;
		target.velocityInherit = source.velocityInherit;
		target.buoyancy = source.buoyancy;
		target.drag = source.drag;
		target.turbulence = source.turbulence;
		target.turbulenceScale = source.turbulenceScale;
	}

	std::unordered_set<Identity, IdentityHash> observed;
	TSpriteIterator<DCoreActor> iterator;
	while (DCoreActor* actor = iterator.Next())
	{
		if (actor == nullptr || !actor->exists() || (actor->ObjectFlags & OF_EuthanizeMe) != 0 || actor->GetClass() == nullptr) continue;
		for (uint32_t ruleIndex = 0; ruleIndex < resolved.smokeActorRules.Size(); ++ruleIndex)
		{
			const auto& rule = resolved.smokeActorRules[ruleIndex];
			if (!rule.actorClassResolved || !rule.styleResolved || rule.actorClass == nullptr ||
				(actor->GetClass() != rule.actorClass && !actor->GetClass()->IsDescendantOf(rule.actorClass))) continue;
			Identity identity = { ruleIndex, (int32_t)actor->GetIndex(), actor };
			observed.insert(identity);
			if (mEmitted.find(identity) != mEmitted.end()) continue;
			if (commands.size() >= 256u) continue;
			NRISmokeInjectionCommandGpu command = {};
			command.position[0] = (float)actor->spr.pos.X;
			command.position[1] = (float)-actor->spr.pos.Y;
			command.position[2] = (float)-actor->spr.pos.Z;
			command.spawnRadius = rule.spawnRadius;
			command.styleIndex = rule.styleIndex;
			command.count = rule.count;
			command.serial = nextSerial++;
			command.epoch = epoch;
			commands.push_back(command);
			mEmitted.insert(identity);
		}
	}
	for (auto it = mEmitted.begin(); it != mEmitted.end(); )
		it = observed.find(*it) == observed.end() ? mEmitted.erase(it) : std::next(it);
}
