#include "nri_smoke_emitters.h"

#include "nri_scene_lights.h"

#include "coreactor.h"
#include "lightoverlay.h"
#include "lightoverlay_smoke_editor.h"
#include "printf.h"

#include <algorithm>
#include <cmath>

namespace
{
	constexpr size_t kMapEmitterCommandCeiling = 192u;

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

	void SetPointSourceShape(NRISmokeInjectionCommandGpu& command)
	{
		command.shape = static_cast<uint32_t>(NRISmokeInjectionShape::Sphere);
		std::fill(command.halfAxisU, command.halfAxisU + 3, 0.0f);
		std::fill(command.halfAxisV, command.halfAxisV + 3, 0.0f);
	}

	bool BuildMapEmitterRectangleBasis(const float sourceNormal[3], float rotationDegrees,
		float width, float height, DVector3& outNormal, DVector3& outAxisU, DVector3& outAxisV)
	{
		if (!std::isfinite(rotationDegrees) || !std::isfinite(width) || !std::isfinite(height))
		{
			return false;
		}
		outNormal = DVector3(sourceNormal[0], sourceNormal[1], sourceNormal[2]);
		if (!std::isfinite(outNormal.X) || !std::isfinite(outNormal.Y) || !std::isfinite(outNormal.Z) ||
			outNormal.isZero())
		{
			return false;
		}
		outNormal.MakeUnit();

		const DVector3 reference = std::abs(outNormal.Z) < 0.999 ?
			DVector3(0.0, 0.0, 1.0) : DVector3(0.0, 1.0, 0.0);
		DVector3 baseU = reference ^ outNormal;
		if (baseU.isZero())
		{
			return false;
		}
		baseU.MakeUnit();
		DVector3 baseV = outNormal ^ baseU;
		baseV.MakeUnit();

		const double radians = (double)rotationDegrees * (3.14159265358979323846 / 180.0);
		const double cosine = std::cos(radians);
		const double sine = std::sin(radians);
		const DVector3 rotatedU = baseU * cosine + baseV * sine;
		const DVector3 rotatedV = baseV * cosine - baseU * sine;
		outAxisU = rotatedU * (std::max(0.0f, width) * 0.5);
		outAxisV = rotatedV * (std::max(0.0f, height) * 0.5);
		return true;
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
	mMapEmitterStates.clear();
	mEditorPreviewState = {};
	mEditorPreviewMapName = "";
	mEditorPreviewRuleId = "";
	mActiveMapName = "";
	mGeneration = 0;
}

void NRISmokeEmitterSystem::Gather(uint32_t epoch, double gameplayTimeSeconds, const TArray<PathTracingWeaponLightEvent>& weaponEvents,
	const SceneLightSystem& sceneLights,
	std::vector<NRISmokeStyleGpu>& styles, std::vector<NRISmokeInjectionCommandGpu>& commands,
	uint32_t& nextSerial, uint32_t traceMode)
{
	const ResolvedLightOverlaySet& resolved = GetResolvedLightOverlaySet();
	if (mGeneration != resolved.resolvedGeneration || mActiveMapName.CompareNoCase(resolved.activeMapName) != 0)
	{
		mGeneration = resolved.resolvedGeneration;
		mActiveMapName = resolved.activeMapName;
		mActorStates.clear();
		mMapEmitterStates.clear();
		mEditorPreviewState = {};
		mEditorPreviewMapName = "";
		mEditorPreviewRuleId = "";
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
	std::vector<uint32_t> timeDeferredPerRule;
	std::vector<uint32_t> activatedPerRule;
	if (traceMode != 0)
	{
		observedPerRule.resize(resolved.smokeActorRules.Size());
		emittedPerRule.resize(resolved.smokeActorRules.Size());
		particlesPerRule.resize(resolved.smokeActorRules.Size());
		deferredPerRule.resize(resolved.smokeActorRules.Size());
		timeDeferredPerRule.resize(resolved.smokeActorRules.Size());
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
					state.activationTimeSeconds = gameplayTimeSeconds;
					state.startTimeElapsed = rule.startTime <= 0.0f;
					if (traceMode != 0) activatedPerRule[ruleIndex]++;
				}
				else
				{
					state.previousPosition = currentPosition;
					state.previousTimeSeconds = gameplayTimeSeconds;
					state.activationTimeSeconds = gameplayTimeSeconds;
					state.spacingRemainder = 0.0f;
					state.intervalRemainder = 0.0;
					state.startDistanceTraveled = 0.0;
					state.startTimeElapsed = false;
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
			DVector3 cadenceStartPosition = state.previousPosition;
			double cadenceStartTimeSeconds = state.previousTimeSeconds;
			if (!state.emitted)
			{
				// Time and distance advance independently from activation. If both
				// are authored, only the tail after the later crossing enters cadence.
				const DVector3 startSegment = currentPosition - state.previousPosition;
				const double startSegmentLength = startSegment.Length();
				const double elapsedSeconds = std::max(0.0, gameplayTimeSeconds - state.previousTimeSeconds);
				double timeCrossingFraction = 0.0;
				double distanceCrossingFraction = 0.0;
				bool timeReady = state.startTimeElapsed || rule.startTime <= 0.0f;
				if (!timeReady)
				{
					const double previousElapsed = std::max(0.0, state.previousTimeSeconds - state.activationTimeSeconds);
					const double elapsedSinceActivation = std::max(0.0, gameplayTimeSeconds - state.activationTimeSeconds);
					if (elapsedSinceActivation >= (double)rule.startTime)
					{
						timeCrossingFraction = elapsedSeconds > 0.0 ?
							std::clamp(((double)rule.startTime - previousElapsed) / elapsedSeconds, 0.0, 1.0) : 1.0;
						timeReady = true;
						state.startTimeElapsed = true;
					}
					else
					{
						if (traceMode != 0) timeDeferredPerRule[ruleIndex]++;
						if (traceMode >= 2 && verbosePrinted < 32u)
						{
							Printf("NRI PT smoke emitter: event=starttime-deferred rule=%s class=%s actor=%d identity=%p elapsed=%.3f starttime=%.3f\n",
								rule.id.GetChars(), actor->GetClass()->TypeName.GetChars(), actor->GetIndex(), actor,
								elapsedSinceActivation, rule.startTime);
							verbosePrinted++;
						}
					}
				}

				bool distanceReady = rule.startDistance <= 0.0f || state.startDistanceTraveled >= (double)rule.startDistance;
				if (!distanceReady)
				{
					const double remainingDistance = std::max(0.0, (double)rule.startDistance - state.startDistanceTraveled);
					if (startSegmentLength >= remainingDistance && startSegmentLength > 0.0)
					{
						distanceCrossingFraction = std::clamp(remainingDistance / startSegmentLength, 0.0, 1.0);
						state.startDistanceTraveled = rule.startDistance;
						distanceReady = true;
					}
					else
					{
						state.startDistanceTraveled += startSegmentLength;
					}
				}

				if (timeReady && distanceReady)
				{
					const double crossingFraction = std::max(timeCrossingFraction, distanceCrossingFraction);
					cadenceStartPosition = state.previousPosition + startSegment * crossingFraction;
					cadenceStartTimeSeconds = state.previousTimeSeconds + elapsedSeconds * crossingFraction;
					emissionPositions.push_back(cadenceStartPosition);
					state.emitted = true;
				}
			}
			if (state.emitted && rule.trigger == LightOverlaySmokeTrigger::Interval)
			{
				const DVector3 segment = currentPosition - cadenceStartPosition;
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
					measure = std::max(0.0, gameplayTimeSeconds - cadenceStartTimeSeconds);
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
					emissionPositions.push_back(cadenceStartPosition + segment * fraction);
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
				SetPointSourceShape(command);
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
			if (emittedPerRule[ruleIndex] == 0u && deferredPerRule[ruleIndex] == 0u &&
				timeDeferredPerRule[ruleIndex] == 0u && activatedPerRule[ruleIndex] == 0u) continue;
			const auto& rule = resolved.smokeActorRules[ruleIndex];
			Printf("NRI PT smoke emitter: event=frame-summary rule=%s class=%s observed=%u emitted=%u particles=%u activation=%s deferred=%u activated=%u starttime=%.3f time_deferred=%u\n",
				rule.id.GetChars(), rule.actorClassName.GetChars(), observedPerRule[ruleIndex], emittedPerRule[ruleIndex], particlesPerRule[ruleIndex],
				rule.activationPolicy == LightOverlayActorActivationPolicy::Immediate ? "immediate" : "surface",
				deferredPerRule[ruleIndex], activatedPerRule[ruleIndex], rule.startTime, timeDeferredPerRule[ruleIndex]);
		}
	}
	for (auto it = mActorStates.begin(); it != mActorStates.end(); )
		it = !it->second.observed ? mActorStates.erase(it) : std::next(it);

	struct MapEmissionStats
	{
		uint32_t active = 0u;
		uint32_t emitted = 0u;
		uint32_t particles = 0u;
		uint32_t skipped = 0u;
	};
	auto emitMapRule = [&](const ResolvedLightOverlayMapSmokeEmitterRule& rule,
		MapEmitterState& state, const char* eventName) -> MapEmissionStats
	{
		MapEmissionStats stats = {};
		stats.active = 1u;
		DVector3 normal;
		DVector3 axisU;
		DVector3 axisV;
		if (!BuildMapEmitterRectangleBasis(rule.normal, rule.rotation, rule.size[0], rule.size[1],
			normal, axisU, axisV))
		{
			if (traceMode >= 2 && verbosePrinted < 32u)
			{
				Printf("NRI PT smoke emitter: event=%s-ignored map=%s rule=%s reason=invalid-basis\n",
					eventName, resolved.activeMapName.GetChars(), rule.id.GetChars());
				verbosePrinted++;
			}
			return stats;
		}

		uint32_t emitCount = 0u;
		if (!state.emitted)
		{
			state.emitted = true;
			state.previousTimeSeconds = gameplayTimeSeconds;
			state.intervalRemainder = 0.0;
			emitCount = 1u;
		}
		else
		{
			const double elapsedSeconds = std::max(0.0, gameplayTimeSeconds - state.previousTimeSeconds);
			state.previousTimeSeconds = gameplayTimeSeconds;
			const double intervalSeconds = std::max(0.001, (double)rule.intervalSeconds);
			const double total = state.intervalRemainder + elapsedSeconds;
			const uint32_t candidateCount = (uint32_t)std::floor(total / intervalSeconds);
			state.intervalRemainder = std::fmod(total, intervalSeconds);
			emitCount = std::min(candidateCount, rule.maxSegmentsPerFrame);
			stats.skipped = candidateCount - emitCount;
		}
		const size_t availableCommands = commands.size() < kMapEmitterCommandCeiling ?
			kMapEmitterCommandCeiling - commands.size() : 0u;
		const uint32_t admittedCount = (uint32_t)std::min<size_t>(emitCount, availableCommands);
		stats.skipped += emitCount - admittedCount;
		emitCount = admittedCount;

		const DVector3 center(rule.position[0], rule.position[1], rule.position[2]);
		const DVector3 offsetCenter = center + normal * rule.offset;
		const DVector3 velocity = normal * rule.velocityScale;
		for (uint32_t emissionIndex = 0; emissionIndex < emitCount; ++emissionIndex)
		{
			NRISmokeInjectionCommandGpu command = {};
			WorldToPathTracingPosition(offsetCenter, command.position);
			WorldToPathTracingDirection(velocity, command.velocity);
			WorldToPathTracingDirection(axisU, command.halfAxisU);
			WorldToPathTracingDirection(axisV, command.halfAxisV);
			command.spawnRadius = rule.spawnRadius;
			command.styleIndex = rule.styleIndex;
			command.count = rule.count;
			command.serial = nextSerial++;
			command.densityScale = rule.densityScale;
			command.radiusScale = rule.radiusScale;
			command.velocityCone = rule.velocityCone;
			command.epoch = epoch;
			command.shape = static_cast<uint32_t>(NRISmokeInjectionShape::Rectangle);
			commands.push_back(command);
			stats.emitted++;
			stats.particles += command.count;
			if (traceMode >= 2 && verbosePrinted < 32u)
			{
				Printf("NRI PT smoke emitter: event=%s map=%s rule=%s command_serial=%u style=%u particles=%u render=(%.3f,%.3f,%.3f) velocity=(%.3f,%.3f,%.3f) axis_u=(%.3f,%.3f,%.3f) axis_v=(%.3f,%.3f,%.3f) shape=rectangle\n",
					eventName, resolved.activeMapName.GetChars(), rule.id.GetChars(), command.serial, command.styleIndex, command.count,
					command.position[0], command.position[1], command.position[2],
					command.velocity[0], command.velocity[1], command.velocity[2],
					command.halfAxisU[0], command.halfAxisU[1], command.halfAxisU[2],
					command.halfAxisV[0], command.halfAxisV[1], command.halfAxisV[2]);
				verbosePrinted++;
			}
		}
		return stats;
	};

	MapSmokeEmitterEditorRuntimePreview editorPreview = {};
	const bool hasEditorPreview = GetMapSmokeEmitterEditorRuntimePreview(editorPreview) && editorPreview.active &&
		resolved.currentMapAvailable && editorPreview.rule.mapName.CompareNoCase(resolved.activeMapName) == 0;
	for (uint32_t ruleIndex = 0; ruleIndex < resolved.mapSmokeEmitterRules.Size(); ++ruleIndex)
	{
		const auto& rule = resolved.mapSmokeEmitterRules[ruleIndex];
		if (!resolved.currentMapAvailable || !rule.styleResolved ||
			!rule.hasPosition || !rule.hasNormal || !rule.hasSize ||
			rule.mapName.CompareNoCase(resolved.activeMapName) != 0)
		{
			continue;
		}
		if (hasEditorPreview && editorPreview.suppressPersistedRule &&
			rule.mapName.CompareNoCase(editorPreview.rule.mapName) == 0 &&
			rule.id.CompareNoCase(editorPreview.rule.id) == 0)
		{
			continue;
		}
		const MapEmissionStats stats = emitMapRule(rule, mMapEmitterStates[ruleIndex], "map");
		if (traceMode != 0)
		{
			Printf("NRI PT smoke emitter: event=map-frame-summary map=%s rule=%s active=%u emitted=%u particles=%u skipped=%u interval=%.3f shape=rectangle\n",
				resolved.activeMapName.GetChars(), rule.id.GetChars(), stats.active, stats.emitted,
				stats.particles, stats.skipped, rule.intervalSeconds);
		}
	}

	if (hasEditorPreview)
	{
		const ResolvedLightOverlaySmokeStyle* previewStyle = nullptr;
		for (const auto& style : resolved.smokeStyles)
		{
			if (style.id.CompareNoCase(editorPreview.rule.styleId) == 0)
			{
				previewStyle = &style;
				break;
			}
		}
		if (previewStyle != nullptr && editorPreview.rule.hasPosition &&
			editorPreview.rule.hasNormal && editorPreview.rule.hasSize)
		{
			if (mEditorPreviewMapName.CompareNoCase(editorPreview.rule.mapName) != 0 ||
				mEditorPreviewRuleId.CompareNoCase(editorPreview.rule.id) != 0)
			{
				mEditorPreviewState = {};
				mEditorPreviewMapName = editorPreview.rule.mapName;
				mEditorPreviewRuleId = editorPreview.rule.id;
			}
			ResolvedLightOverlayMapSmokeEmitterRule previewRule = {};
			static_cast<ParsedLightOverlayMapSmokeEmitterRule&>(previewRule) = editorPreview.rule;
			previewRule.styleIndex = previewStyle->styleIndex;
			previewRule.styleResolved = true;
			const MapEmissionStats stats = emitMapRule(previewRule, mEditorPreviewState, "map-preview");
			if (traceMode != 0)
			{
				Printf("NRI PT smoke emitter: event=map-preview-frame-summary map=%s rule=%s active=%u emitted=%u particles=%u skipped=%u interval=%.3f revision=%u shape=rectangle\n",
					resolved.activeMapName.GetChars(), previewRule.id.GetChars(), stats.active, stats.emitted,
					stats.particles, stats.skipped, previewRule.intervalSeconds, editorPreview.revision);
			}
		}
	}
	else
	{
		mEditorPreviewState = {};
		mEditorPreviewMapName = "";
		mEditorPreviewRuleId = "";
	}

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
			SetPointSourceShape(command);
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
