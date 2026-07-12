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
	uint64_t residentBytes = 0;
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
	uint32_t selectionCollisions = 0;
	uint32_t selectionReplacements = 0;
	uint32_t fineTierParticles = 0;
	uint32_t wideTierParticles = 0;
	uint32_t globalTierParticles = 0;
	uint32_t fineOccupiedCells = 0;
	uint32_t wideOccupiedCells = 0;
	uint32_t globalOccupiedSlices = 0;
	uint32_t fineSelectionLosses = 0;
	uint32_t wideSelectionLosses = 0;
	uint32_t globalSelectionLosses = 0;
	uint32_t maximumDepthSpan = 0;
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
	std::array<nri::Pipeline*, 7> mPipelines = {};
	std::vector<CommandSlot> mCommandSlots;
	NRIBufferResource mStyleBuffer;
	NRIBufferResource mParticles;
	NRIBufferResource mControl;
	NRIBufferResource mFineCellCounts;
	NRIBufferResource mFineCellIndices;
	NRIBufferResource mWideCellCounts;
	NRIBufferResource mWideCellIndices;
	NRIBufferResource mGlobalDepthCounts;
	NRIBufferResource mGlobalDepthIndices;
	NRIBufferResource mFroxelLocal;
	NRIBufferResource mFroxelIntegrated;
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
};
