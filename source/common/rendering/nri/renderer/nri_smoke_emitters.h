#pragma once

#include "nri_smoke_contracts.h"
#include "nri_smoke_analytic_carriers.h"
#include "nri_smoke_analytic_trail_bridge.h"
#include "nri_smoke_continuous_sources.h"
#include "nri_smoke_interest.h"
#include "nri_smoke_pulses.h"
#include "v_video.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

class SceneLightSystem;

class NRISmokeEmitterSystem
{
public:
	void Gather(uint32_t epoch, double gameplayTimeSeconds, const TArray<PathTracingWeaponLightEvent>& weaponEvents,
		const SceneLightSystem& sceneLights,
		std::vector<NRISmokeStyleGpu>& styles, std::vector<NRISmokeInjectionCommandGpu>& commands,
		std::vector<NRISmokePulseEnqueueInfo>& commandEnqueueInfo,
		std::vector<NRISmokeAnalyticTrailObservationBatch>& trailObservations,
		std::vector<NRISmokeAnalyticCarrierRequest>& analyticRequests,
		uint32_t& nextSerial, uint32_t traceMode, const NRISmokeInterestSnapshot& interest,
		float gridCellSize, uint32_t gridBrickCapacity);
	void Reset();
	uint32_t GetGeneration() const { return mGeneration; }
	void SetContinuousSourceWorkQuantity(uint32_t quantity) { mContinuousSourceWorkQuantity = quantity; }
	const NRISmokeContinuousSourceSnapshot& GetContinuousSourceSnapshot() const { return mContinuousSources.GetSnapshot(); }

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
		double activationTimeSeconds = 0.0;
		float spacingRemainder = 0.0f;
		double intervalRemainder = 0.0;
		double startDistanceTraveled = 0.0;
		uint64_t continuousStableKey = 0;
		uint64_t continuousCadenceOrdinal = 0;
		uint64_t trailUpdateOrdinal = 0;
		bool activationLatched = false;
		bool appearanceObserved = false;
		bool sourceTracePublished = false;
		bool authorityTracePublished = false;
		bool authorityTraceAppearanceReady = false;
		bool authorityTraceActivationLatched = false;
		bool authorityTraceCadenceActive = false;
		bool startTimeElapsed = false;
		bool emitted = false;
		bool observed = false;
	};
	struct MapEmitterState
	{
		double previousTimeSeconds = 0.0;
		double logicalElapsedSeconds = 0.0;
		double intervalRemainder = 0.0;
		uint64_t nextCadenceOrdinal = 0;
		uint32_t coalescedDebt = 0;
		NRISmokeInterestTier previousTier = NRISmokeInterestTier::Dormant;
		bool initialized = false;
		bool emitted = false;
	};

	uint32_t mGeneration = 0;
	FString mActiveMapName;
	std::unordered_map<Identity, ActorState, IdentityHash> mActorStates;
	std::unordered_map<uint32_t, MapEmitterState> mMapEmitterStates;
	NRISmokeContinuousSourceOwner mContinuousSources;
	uint64_t mNextContinuousSourceGeneration = 0;
	uint32_t mContinuousSourceWorkQuantity = 8u;
	MapEmitterState mEditorPreviewState;
	FString mEditorPreviewMapName;
	FString mEditorPreviewRuleId;
};
