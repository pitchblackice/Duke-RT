#include "nri_smoke_emitters.h"

#include "nri_scene_lights.h"

#include "coreactor.h"
#include "lightoverlay.h"
#include "printf.h"

#include <algorithm>
#include <cmath>

namespace
{
	void WorldToPathTracingPosition(const DVector3& worldPosition, float outPosition[3])
	{
		outPosition[0] = (float)worldPosition.X;
		outPosition[1] = (float)-worldPosition.Z;
		outPosition[2] = (float)-worldPosition.Y;
	}

	void WorldToPathTracingDirection(const DVector3& worldDirection, float outDirection[3])
	{
		outDirection[0] = (float)worldDirection.X;
		outDirection[1] = (float)-worldDirection.Z;
		outDirection[2] = (float)-worldDirection.Y;
	}

	bool ActorMatchesClass(const DCoreActor* actor, const PClassActor* actorClass)
	{
		return actor != nullptr && actorClass != nullptr && actor->GetClass() != nullptr &&
			(actor->GetClass() == actorClass || actor->GetClass()->IsDescendantOf(actorClass));
	}

	DVector3 ActorForward(const DCoreActor* actor)
	{
		const DVector2 forward = actor->spr.Angles.Yaw.ToVector();
		return DVector3(forward.X, forward.Y, 0.0);
	}

	DVector3 ActorVelocity(const DCoreActor* actor)
	{
		const DVector2 forward = actor->spr.Angles.Yaw.ToVector();
		return DVector3(forward.X * actor->vel.X, forward.Y * actor->vel.X, actor->vel.Z);
	}
}

size_t NRISmokeEmitterSystem::IdentityHash::operator()(const Identity& value) const
{
	size_t hash = (size_t)value.rule * 16777619u;
	hash ^= (size_t)(uint32_t)value.actorIndex + 0x9e3779b9u + (hash << 6) + (hash >> 2);
	hash ^= (size_t)value.actor + 0x9e3779b9u + (hash << 6) + (hash >> 2);
	return hash;
}

void NRISmokeEmitterSystem::Reset()
{
	mActorStates.clear();
	mGeneration = 0;
}

void NRISmokeEmitterSystem::Gather(uint32_t epoch, double gameplayTimeSeconds, const TArray<PathTracingWeaponLightEvent>& weaponEvents,
	const SceneLightSystem& sceneLights,
	std::vector<NRISmokeStyleGpu>& styles, std::vector<NRISmokeInjectionCommandGpu>& commands,
	uint32_t& nextSerial, uint32_t traceMode)
{
	const ResolvedLightOverlaySet& resolved = GetResolvedLightOverlaySet();
	if (mGeneration != resolved.resolvedGeneration)
	{
		mGeneration = resolved.resolvedGeneration;
		mActorStates.clear();
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
		target.temperature = source.temperature;
		target.momentumScale = source.momentumScale;
		target.coolingHalfLife = source.coolingHalfLife;
	}

	for (auto& entry : mActorStates) entry.second.observed = false;
	std::vector<uint32_t> observedPerRule;
	std::vector<uint32_t> emittedPerRule;
	std::vector<uint32_t> particlesPerRule;
	std::vector<uint32_t> deferredPerRule;
	std::vector<uint32_t> activatedPerRule;
	if (traceMode != 0)
	{
		observedPerRule.resize(resolved.smokeActorRules.Size());
		emittedPerRule.resize(resolved.smokeActorRules.Size());
		particlesPerRule.resize(resolved.smokeActorRules.Size());
		deferredPerRule.resize(resolved.smokeActorRules.Size());
		activatedPerRule.resize(resolved.smokeActorRules.Size());
	}
	uint32_t verbosePrinted = 0;
	TSpriteIterator<DCoreActor> iterator;
	while (DCoreActor* actor = iterator.Next())
	{
		if (actor == nullptr || !actor->exists() || (actor->ObjectFlags & OF_EuthanizeMe) != 0 || actor->GetClass() == nullptr) continue;
		for (uint32_t ruleIndex = 0; ruleIndex < resolved.smokeActorRules.Size(); ++ruleIndex)
		{
			const auto& rule = resolved.smokeActorRules[ruleIndex];
			if (!rule.actorClassResolved || !rule.styleResolved || !ActorMatchesClass(actor, rule.actorClass)) continue;
			DCoreActor* owner = actor->GetOwnerActor();
			if (!rule.ownerClassName.IsEmpty() && (!rule.ownerClassResolved || !ActorMatchesClass(owner, rule.ownerClass))) continue;
			if (!rule.excludeOwnerClassName.IsEmpty() &&
				(!rule.excludeOwnerClassResolved || ActorMatchesClass(owner, rule.excludeOwnerClass))) continue;
			Identity identity = { ruleIndex, (int32_t)actor->GetIndex(), actor };
			const DVector3 currentPosition = actor->spr.pos;
			auto stateIt = mActorStates.find(identity);
			if (stateIt == mActorStates.end())
			{
				ActorState state = {};
				state.previousPosition = currentPosition;
				state.previousTimeSeconds = gameplayTimeSeconds;
				stateIt = mActorStates.emplace(identity, state).first;
			}
			ActorState& state = stateIt->second;
			state.observed = true;
			if (traceMode != 0) observedPerRule[ruleIndex]++;
			if (!state.activationLatched)
			{
				const bool appearanceReady = sceneLights.HasActorAppearanceEvidence(identity.actorIndex);
				if (rule.activationPolicy == LightOverlayActorActivationPolicy::Immediate || appearanceReady)
				{
					state.activationLatched = true;
					if (traceMode != 0) activatedPerRule[ruleIndex]++;
				}
				else
				{
					state.previousPosition = currentPosition;
					state.previousTimeSeconds = gameplayTimeSeconds;
					state.spacingRemainder = 0.0f;
					state.intervalRemainder = 0.0;
					if (traceMode != 0) deferredPerRule[ruleIndex]++;
					if (traceMode >= 2 && verbosePrinted < 32u)
					{
						Printf("NRI PT smoke emitter: event=activation-deferred rule=%s class=%s actor=%d identity=%p activation=surface appearance=no\n",
							rule.id.GetChars(), actor->GetClass()->TypeName.GetChars(), actor->GetIndex(), actor);
						verbosePrinted++;
					}
					continue;
				}
			}

			std::vector<DVector3> emissionPositions;
			if (!state.emitted)
			{
				emissionPositions.push_back(currentPosition);
				state.emitted = true;
			}
			else if (rule.trigger == LightOverlaySmokeTrigger::Interval)
			{
				const DVector3 segment = currentPosition - state.previousPosition;
				const double segmentLength = segment.Length();
				uint32_t candidateCount = 0;
				double firstCrossing = 0.0;
				double measure = 0.0;
				const bool spatialCadence = rule.spacing > 0.0f && segmentLength > 0.0;
				if (spatialCadence)
				{
					state.intervalRemainder = 0.0;
					measure = segmentLength;
					const double total = (double)state.spacingRemainder + measure;
					candidateCount = (uint32_t)std::floor(total / (double)rule.spacing);
					firstCrossing = (double)rule.spacing - (double)state.spacingRemainder;
					state.spacingRemainder = (float)std::fmod(total, (double)rule.spacing);
				}
				else
				{
					measure = std::max(0.0, gameplayTimeSeconds - state.previousTimeSeconds);
					const double total = state.intervalRemainder + measure;
					candidateCount = (uint32_t)std::floor(total / (double)rule.intervalSeconds);
					firstCrossing = (double)rule.intervalSeconds - state.intervalRemainder;
					state.intervalRemainder = std::fmod(total, (double)rule.intervalSeconds);
				}

				const uint32_t emitCount = std::min(candidateCount, rule.maxSegmentsPerFrame);
				const uint32_t skipped = candidateCount - emitCount;
				const double stride = spatialCadence ? (double)rule.spacing : (double)rule.intervalSeconds;
				for (uint32_t emissionIndex = 0; emissionIndex < emitCount; ++emissionIndex)
				{
					const double crossing = firstCrossing + (double)(skipped + emissionIndex) * stride;
					const double fraction = measure > 0.0 ? std::clamp(crossing / measure, 0.0, 1.0) : 1.0;
					emissionPositions.push_back(state.previousPosition + segment * fraction);
				}
			}

			state.previousPosition = currentPosition;
			state.previousTimeSeconds = gameplayTimeSeconds;
			const DVector3 forward = ActorForward(actor);
			const DVector3 right(-forward.Y, forward.X, 0.0);
			const DVector3 up(0.0, 0.0, 1.0);
			const DVector3 offset = right * rule.offset[0] + forward * rule.offset[1] + up * rule.offset[2];
			const DVector3 velocity = ActorVelocity(actor) * rule.velocityScale;
			for (const DVector3& emissionPosition : emissionPositions)
			{
				NRISmokeInjectionCommandGpu command = {};
				WorldToPathTracingPosition(emissionPosition + offset, command.position);
				WorldToPathTracingDirection(velocity, command.velocity);
				command.spawnRadius = rule.spawnRadius;
				command.styleIndex = rule.styleIndex;
				command.count = rule.count;
				command.serial = nextSerial++;
				command.densityScale = rule.densityScale;
				command.radiusScale = rule.radiusScale;
				command.velocityCone = rule.velocityCone;
				command.epoch = epoch;
				commands.push_back(command);
				if (traceMode != 0)
				{
					emittedPerRule[ruleIndex]++;
					particlesPerRule[ruleIndex] += command.count;
				}
				if (traceMode >= 2 && verbosePrinted < 32u)
				{
					Printf("NRI PT smoke emitter: event=%s rule=%s class=%s actor=%d identity=%p serial=%u world=(%.3f,%.3f,%.3f) render=(%.3f,%.3f,%.3f) velocity=(%.3f,%.3f,%.3f) style=%u particles=%u\n",
						rule.trigger == LightOverlaySmokeTrigger::Interval ? "interval" : "spawn",
						rule.id.GetChars(), actor->GetClass()->TypeName.GetChars(), actor->GetIndex(), actor, command.serial,
						emissionPosition.X + offset.X, emissionPosition.Y + offset.Y, emissionPosition.Z + offset.Z,
						command.position[0], command.position[1], command.position[2],
						command.velocity[0], command.velocity[1], command.velocity[2], command.styleIndex, command.count);
					verbosePrinted++;
				}
			}
		}
	}
	if (traceMode != 0)
	{
		for (uint32_t ruleIndex = 0; ruleIndex < resolved.smokeActorRules.Size(); ++ruleIndex)
		{
			if (emittedPerRule[ruleIndex] == 0u && deferredPerRule[ruleIndex] == 0u && activatedPerRule[ruleIndex] == 0u) continue;
			const auto& rule = resolved.smokeActorRules[ruleIndex];
			Printf("NRI PT smoke emitter: event=frame-summary rule=%s class=%s observed=%u emitted=%u particles=%u activation=%s deferred=%u activated=%u\n",
				rule.id.GetChars(), rule.actorClassName.GetChars(), observedPerRule[ruleIndex], emittedPerRule[ruleIndex], particlesPerRule[ruleIndex],
				rule.activationPolicy == LightOverlayActorActivationPolicy::Immediate ? "immediate" : "surface",
				deferredPerRule[ruleIndex], activatedPerRule[ruleIndex]);
		}
	}
	for (auto it = mActorStates.begin(); it != mActorStates.end(); )
		it = !it->second.observed ? mActorStates.erase(it) : std::next(it);

	uint32_t eventCommands = 0;
	uint32_t eventParticles = 0;
	for (const PathTracingWeaponLightEvent& event : weaponEvents)
	{
		bool matchedEventRule = false;
		for (const ResolvedLightOverlaySmokeEventRule& rule : resolved.smokeEventRules)
		{
			if (!rule.styleResolved || event.eventId.CompareNoCase(rule.id) != 0)
				continue;
			matchedEventRule = true;
			DVector3 worldPosition = event.worldPosition;
			if (event.hasBasis)
			{
				worldPosition += event.basisRight * rule.offset[0] +
					event.basisForward * rule.offset[1] +
					event.basisUp * rule.offset[2];
			}
			if (event.hasSurfaceNormal && !event.surfaceNormal.isZero())
				worldPosition += event.surfaceNormal.Unit() * rule.normalOffset;

			DVector3 worldVelocity;
			bool directionAvailable = false;
			switch (rule.directionPolicy)
			{
			case LightOverlaySmokeDirectionPolicy::Normal:
				if (event.hasSurfaceNormal && !event.surfaceNormal.isZero())
				{
					worldVelocity = event.surfaceNormal.Unit();
					directionAvailable = true;
				}
				break;
			case LightOverlaySmokeDirectionPolicy::Incoming:
				if (event.hasIncomingDirection && !event.incomingDirection.isZero())
				{
					worldVelocity = event.incomingDirection.Unit();
					directionAvailable = true;
				}
				break;
			case LightOverlaySmokeDirectionPolicy::Aim:
			default:
				break;
			}
			// Aim is also the safe fallback for older producers and events that do
			// not carry the requested optional normal/incoming vector.
			if (!directionAvailable && event.hasBasis && !event.basisForward.isZero())
			{
				worldVelocity = event.basisForward.Unit();
				directionAvailable = true;
			}
			worldVelocity *= rule.velocityScale;

			NRISmokeInjectionCommandGpu command = {};
			WorldToPathTracingPosition(worldPosition, command.position);
			if (directionAvailable)
				WorldToPathTracingDirection(worldVelocity, command.velocity);
			command.spawnRadius = rule.spawnRadius;
			command.styleIndex = rule.styleIndex;
			command.count = rule.count;
			command.serial = nextSerial++;
			command.densityScale = rule.densityScale;
			command.radiusScale = rule.radiusScale;
			command.velocityCone = rule.velocityCone;
			command.epoch = epoch;
			commands.push_back(command);
			eventCommands++;
			eventParticles += command.count;

			if (traceMode != 0 && verbosePrinted < 32u)
			{
				Printf("NRI PT smoke emitter: event=weapon rule=%s source_event=%s source_serial=%llu command_serial=%u style=%u particles=%u render=(%.3f,%.3f,%.3f) direction=(%.3f,%.3f,%.3f) cone=%.3f normal=%u incoming=%u\n",
					rule.id.GetChars(), event.eventId.GetChars(), (unsigned long long)event.serial, command.serial,
					command.styleIndex, command.count, command.position[0], command.position[1], command.position[2],
					command.velocity[0], command.velocity[1], command.velocity[2], command.velocityCone,
					event.hasSurfaceNormal ? 1u : 0u, event.hasIncomingDirection ? 1u : 0u);
				verbosePrinted++;
			}
		}
		if (!matchedEventRule && traceMode != 0)
		{
			Printf("NRI PT smoke emitter: event=weapon-ignored source_event=%s source_serial=%llu reason=no-rule\n",
				event.eventId.GetChars(), (unsigned long long)event.serial);
		}
	}
	if (traceMode != 0 && eventCommands != 0u)
	{
		Printf("NRI PT smoke emitter: event=weapon-frame-summary source_events=%u commands=%u particles=%u\n",
			(uint32_t)weaponEvents.Size(), eventCommands, eventParticles);
	}
}
