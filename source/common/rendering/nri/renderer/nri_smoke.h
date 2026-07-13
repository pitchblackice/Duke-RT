#pragma once

#include "nri_renderer_settings.h"
#include "nri_resources.h"
#include "nri_smoke_contracts.h"
#include "nri_smoke_emitters.h"
#include "v_video.h"

#include <array>
#include <cstdint>
#include <vector>

class NRIRenderer;
struct NRISmokeRouteDesc;

struct NRISmokeStatusSnapshot
{
	bool enabled = false;
	bool mainViewEligible = false;
	bool routeSupported = false;
	uint32_t simulationEpoch = 1;
	uint32_t preparedFrame = UINT32_MAX;
	uint32_t dispatchedFrame = UINT32_MAX;
	uint32_t inputSlot = UINT32_MAX;
	uint32_t outputSlot = UINT32_MAX;
	uint32_t depthSlot = UINT32_MAX;
	uint32_t routeWidth = 0;
	uint32_t routeHeight = 0;
	uint32_t routePlacement = 0;
	uint32_t exposureDomain = 0;
	const char* resetReason = "initial";
	uint32_t froxelWidth = 0;
	uint32_t froxelHeight = 0;
	uint32_t froxelDepth = 0;
	uint32_t particleCapacity = 0;
	uint32_t commandsUploaded = 0;
	uint64_t commandsUploadedTotal = 0;
	uint32_t styleCount = 0;
	uint32_t commandsDropped = 0;
	uint32_t simulationSubsteps = 0;
	uint32_t requestedLightMode = 0;
	uint32_t effectiveLightMode = 0;
	bool filteredVisibilityRequested = false;
	bool filteredVisibilityEffective = false;
	bool forceOpaqueVisibility = false;
	bool shadowTlasReady = false;
	uint64_t residentBytes = 0;
	uint64_t indirectCacheBytes = 0;
	uint64_t emissiveReservoirBytes = 0;
	uint32_t emissiveReuseModeRequested = 0;
	uint32_t emissiveReuseModeEffective = 0;
	bool emissiveReference = false;
	bool emissiveHistoryValid = false;
	bool volumeHistoryRequested = false;
	bool volumeHistoryEffective = false;
	bool volumeHistoryValid = false;
	uint32_t volumeResolvedSlot = UINT32_MAX;
	uint32_t volumeMetaSlot = UINT32_MAX;
	uint32_t volumeHistoryAge = 0;
	uint64_t volumeHistoryBytes = 0;
	const char* volumeHistoryResetReason = "initial";
	uint32_t dlrrModeRequested = 0;
	uint32_t dlrrModeEffective = 0;
	uint32_t indirectCacheModeRequested = 0;
	uint32_t indirectCacheModeEffective = 0;
	uint64_t controlReadbackBytes = 0;
	bool gpuStatsValid = false;
	uint32_t activeParticles = 0;
	uint32_t spawnedParticles = 0;
	uint32_t expiredParticles = 0;
	uint32_t liveEvictions = 0;
	uint32_t columnOverflow = 0;
	uint32_t wideParticlesProjected = 0;
	uint32_t wideGlobalDrops = 0;
	uint32_t fineColumnReferences = 0;
	uint32_t wideCellReferences = 0;
	uint32_t globalDepthReferences = 0;
	uint32_t referenceInvalidLinks = 0;
	uint32_t referenceTraversalLimitExits = 0;
	uint32_t fineTierParticles = 0;
	uint32_t wideTierParticles = 0;
	uint32_t globalTierParticles = 0;
	uint32_t fineOccupiedCells = 0;
	uint32_t wideOccupiedCells = 0;
	uint32_t globalOccupiedSlices = 0;
	uint32_t fineMaximumCellReferences = 0;
	uint32_t wideMaximumCellReferences = 0;
	uint32_t globalMaximumCellReferences = 0;
	uint32_t maximumDepthSpan = 0;
	uint32_t maximumCandidatesPerFroxel = 0;
	uint32_t occupiedCount = 0;
	uint32_t occupiedOverflow = 0;
	uint32_t mediumCandidateTests = 0;
	uint32_t pointFroxelsProcessed = 0;
	uint32_t directionalFroxelsProcessed = 0;
	uint32_t directionalSamples = 0;
	uint32_t directionalShadowRays = 0;
	uint32_t directionalShadowVisible = 0;
	uint32_t directionalShadowOccluded = 0;
	uint32_t directionalRadianceClamps = 0;
	uint32_t emissiveFroxelsProcessed = 0;
	uint32_t emissiveSamples = 0;
	uint32_t emissiveCandidateMisses = 0;
	uint32_t emissiveDistanceRejected = 0;
	uint32_t emissiveFacingRejected = 0;
	uint32_t emissiveShadowRays = 0;
	uint32_t emissiveShadowVisible = 0;
	uint32_t emissiveShadowOccluded = 0;
	uint32_t emissiveContributed = 0;
	uint32_t emissiveRadianceClamps = 0;
	uint32_t emissiveReservoirInitial = 0;
	uint32_t emissiveReservoirInvalid = 0;
	uint32_t emissiveTemporalAccepted = 0;
	uint32_t emissiveTemporalRejected = 0;
	uint32_t emissiveSpatialAccepted = 0;
	uint32_t emissiveSpatialRejected = 0;
	uint32_t emissiveFinalEvaluations = 0;
	uint32_t emissiveSourceClamps = 0;
	uint32_t emissiveMaximumAge = 0;
	uint32_t emissiveReferenceSamples = 0;
	uint32_t emissiveReferenceRays = 0;
	uint32_t emissiveIdentityRejects = 0;
	uint32_t indirectFroxelsProcessed = 0;
	uint32_t indirectLocalityRays = 0;
	uint32_t indirectLocalityAgreement = 0;
	uint32_t indirectLocalityOneSided = 0;
	uint32_t indirectLocalityMismatch = 0;
	uint32_t indirectLocalityInvalid = 0;
	uint32_t indirectReferenceRays = 0;
	uint32_t indirectReferenceHits = 0;
	uint32_t indirectReferenceMisses = 0;
	uint32_t indirectSectorContributions = 0;
	uint32_t indirectSkyContributions = 0;
	uint32_t indirectEmissionContributions = 0;
	uint32_t indirectRadianceClamps = 0;
	uint32_t indirectNanRejects = 0;
	uint32_t indirectTemporalAccepted = 0;
	uint32_t indirectTemporalRejected = 0;
	uint32_t indirectSpatialAccepted = 0;
	uint32_t indirectSpatialRejected = 0;
	uint32_t indirectCacheMaximumAge = 0;
	uint32_t indirectCacheClamps = 0;
	uint32_t indirectCacheResolved = 0;
	uint32_t lightCandidatesTested = 0;
	uint32_t lightDistanceRejected = 0;
	uint32_t lightShadowRays = 0;
	uint32_t lightShadowVisible = 0;
	uint32_t lightShadowOccluded = 0;
	uint32_t lightSoftSamples = 0;
	uint32_t lightRadianceClamps = 0;
	uint32_t filterCandidateHits = 0;
	uint32_t filterAlphaRejects = 0;
	uint32_t filterNoShadowRejects = 0;
	uint32_t filterOneWayRejects = 0;
	uint32_t filterReflectionRejects = 0;
	uint32_t filterPortalContinuations = 0;
	uint32_t filterAcceptedBlockers = 0;
	uint32_t filterMisses = 0;
	uint32_t filterSkipLimitExits = 0;
	uint32_t filterContinuationLimitExits = 0;
	uint32_t filterResourceDowngrades = 0;
};

class NRISmokeSystem
{
public:
	bool Initialize(NRIRenderer& renderer);
	bool PrepareFrame(NRIRenderer& renderer, bool mainViewEligible, const TArray<PathTracingWeaponLightEvent>& weaponEvents);
	bool DispatchRoute(NRIRenderer& renderer, const NRISmokeRouteDesc& route);
	void Reset(const char* reason);
	void Shutdown(NRIRenderer& renderer);
	void PrintStatus(const NRIRenderer& renderer) const;
	void QueueSyntheticInjection();
	const NRISmokeStatusSnapshot& GetStatusSnapshot() const { return mStatus; }

private:
	struct CommandSlot
	{
		NRIBufferResource upload;
		NRIBufferResource device;
		NRIBufferResource styleUpload;
		NRIBufferResource controlReadback;
		nri::DescriptorSet* inputSet = nullptr;
		nri::DescriptorSet* bufferSet = nullptr;
		nri::DescriptorSet* textureSet = nullptr;
		nri::DescriptorSet* outputSet = nullptr;
		nri::DescriptorSet* lightSet = nullptr;
		nri::DescriptorSet* filteredSceneSet = nullptr;
		bool readbackPending = false;
		bool initialized = false;
		bool readbackInitialized = false;
	};

	bool EnsureResources(NRIRenderer& renderer);
	bool CreateBuffer(NRIRenderer& renderer, NRIBufferResource& out, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::MemoryLocation location, bool srv, bool uav);
	bool UploadBytes(NRIRenderer& renderer, NRIBufferResource& upload, const void* data, uint64_t size);
	bool RecordSimulation(NRIRenderer& renderer);
	bool RecordVolume(NRIRenderer& renderer, const NRISmokeRouteDesc& route);
	void DestroyViewResources(NRIRenderer& renderer);
	void DestroyResources(NRIRenderer& renderer);
	void AppendSyntheticCommand(NRIRenderer& renderer);

	NRISmokeSettings mSettings = {};
	NRISmokeStatusSnapshot mStatus = {};
	nri::PipelineLayout* mPipelineLayout = nullptr;
	std::array<nri::Pipeline*, 18> mPipelines = {};
	std::vector<CommandSlot> mCommandSlots;
	NRIBufferResource mStyleBuffer;
	NRIBufferResource mParticles;
	NRIBufferResource mControl;
	NRIBufferResource mFineCells;
	NRIBufferResource mWideCells;
	NRIBufferResource mGlobalDepthCells;
	NRIBufferResource mReferenceNext;
	NRIBufferResource mParticleDirectionalVisibility;
	NRIBufferResource mFroxelMedium;
	NRIBufferResource mFroxelIntegrated;
	NRIBufferResource mFroxelPhase;
	NRIBufferResource mFroxelSource;
	NRIBufferResource mOccupiedFroxelIndices;
	NRIBufferResource mIndirectHistory;
	NRIBufferResource mIndirectScratch;
	NRIBufferResource mEmissiveCurrent;
	NRIBufferResource mEmissiveTemporal;
	NRIBufferResource mEmissiveHistory;
	NRISmokeEmitterSystem mEmitters;
	std::vector<NRISmokeStyleGpu> mStyles;
	std::vector<NRISmokeInjectionCommandGpu> mPendingCommands;
	uint32_t mResourceParticleCapacity = 0;
	uint32_t mResourceFroxelWidth = 0;
	uint32_t mResourceFroxelHeight = 0;
	uint32_t mResourceFroxelDepth = 0;
	uint32_t mResourceStyleCapacity = 0;
	uint32_t mLastPreparedFrame = UINT32_MAX;
	uint32_t mLastSimulatedFrame = UINT32_MAX;
	uint32_t mNextCommandSerial = 1;
	float mAccumulator = 0.0f;
	double mLastGameplaySeconds = -1.0;
	bool mSyntheticRequested = false;
	bool mNeedsClear = true;
	bool mControlCopyPending = false;
	bool mResourcesInitialized = false;
	bool mViewResourcesInitialized = false;
	bool mIndirectHistoryValid = false;
	bool mEmissiveHistoryValid = false;
	uint32_t mLastEmissiveReuseMode = 0;
	uint32_t mLastEmissiveGeneration = 0;
	uint32_t mLastEmissiveFrame = UINT32_MAX;
	bool mVolumeHistoryValid = false;
	uint32_t mLastVolumeFrame = UINT32_MAX;
	uint32_t mLastVolumeWidth = 0;
	uint32_t mLastVolumeHeight = 0;
	uint32_t mLastVolumePlacement = UINT32_MAX;
	uint32_t mLastVolumeSimulationEpoch = 0;
	bool mLastVolumeHistoryEnabled = false;
	uint64_t mLastVolumeLightingHash = 0;
	uint32_t mLastIndirectCacheMode = 0;
	uint64_t mLastIndirectSectorHash = 0;
	uint64_t mLastIndirectSkyKey = 0;
	uint64_t mLastIndirectEmissiveHash = 0;
};
