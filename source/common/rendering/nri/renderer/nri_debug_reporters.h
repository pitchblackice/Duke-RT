#pragma once

#include "nri_frame_resources.h"
#include "nri_resources.h"

#include <cstdint>
#include <vector>

struct NRIBufferStatusSnapshot
{
	const char* label = "";
	uint64_t usedBytes = 0;
	uint64_t capacityBytes = 0;
	uint64_t usedItems = 0;
	uint64_t capacityItems = 0;
	uint32_t uploadCount = 0;
	uint32_t growthCount = 0;
	uint32_t overwriteCount = 0;
	uint64_t bytesUploadedLastFrame = 0;
	uint32_t growEventsLastFrame = 0;
	uint32_t overwriteEventsLastFrame = 0;
	uint64_t peakUsedBytes = 0;
};

struct NRISceneBufferStatusSnapshot
{
	uint64_t totalUsedBytes = 0;
	uint64_t totalCapacityBytes = 0;
	uint64_t lastFrameUploadBytes = 0;
	uint32_t lastFrameGrowEvents = 0;
	uint32_t lastFrameOverwriteEvents = 0;
	std::vector<NRIBufferStatusSnapshot> buffers;
};

struct NRISelfTestRouteSnapshot
{
	const char* routeName = "unknown";
	const char* presenterName = "unknown";
	const char* ownerName = "unknown";
	const char* passes = "unknown";
	bool denoiserRun = false;
	bool upscalerRun = false;
	bool exposureRun = false;
};

struct NRISelfTestSummarySnapshot
{
	uint32_t traceFrameIndex = 0;
	uint32_t engineFrameIndex = 0;
	const char* mapName = "none";
	const char* levelName = "none";
	const char* graphicsApiName = "unknown";
	bool worldActive = false;
	bool menuActive = false;
	bool gameplayFrame = false;
	bool portal = false;
	int drawmode = 0;
	NRISelfTestRouteSnapshot route;
	int debugMode = 0;
	const char* presentKind = "unknown";
	uint32_t renderWidth = 0;
	uint32_t renderHeight = 0;
	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	uint32_t swapchainFormat = 0;
	bool hdr = false;
	uint32_t primitiveCount = 0;
	uint32_t materialCount = 0;
	uint32_t sceneInstanceCount = 0;
	uint32_t staticInstanceCount = 0;
	uint32_t dynamicInstanceCount = 0;
	uint32_t persistentVoxelInstanceCount = 0;
	uint32_t emissiveInstanceCount = 0;
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
	uint64_t vertexBytes = 0;
	uint64_t indexBytes = 0;
	uint64_t primitiveBytes = 0;
	uint64_t materialBytes = 0;
	uint64_t instanceBytes = 0;
	uint64_t sceneSignature = 0;
	uint64_t materialSignature = 0;
	uint64_t instanceSignature = 0;
	uint64_t skySignature = 0;
	const char* skyMode = "unknown";
	const char* skySource = "unknown";
	uint64_t skyKey = 0;
	float skyBrightness = 0.0f;
	const char* skyAction = "unknown";
	bool autoExposure = false;
	bool exposureTexture = false;
	float exposure = 1.0f;
	float targetExposure = 1.0f;
	float adaptedExposure = 1.0f;
	float meteredLogLuminance = 0.0f;
	bool exposureStatsValid = false;
	uint64_t exposureStatsFrame = 0;
	bool finalValid = false;
	const char* exposureReason = "unknown";
};

struct NRITextureStatusSnapshot
{
	const char* slotName = "unknown";
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t access = 0;
	uint32_t layout = 0;
	uint32_t stages = 0;
};

struct NRITemporalStatusSnapshot
{
	int debugMode = 0;
	const char* requestedMainUpscaler = "unknown";
	const char* resolvedMainUpscaler = "unknown";
	const char* requestedPostSharpen = "unknown";
	const char* resolvedPostSharpen = "unknown";
	bool taa = false;
	bool guiCapture = false;
	int lastDebugMode = 0;
	const char* lastMainUpscaler = "unknown";
	const char* lastPostSharpen = "unknown";
	bool resetHistory = false;
	bool previousCamera = false;
	NRITextureStatusSnapshot historyInput;
	NRITextureStatusSnapshot historyOutput;
	const char* presentSlotName = "unknown";
	const char* upscaledSlotName = "unknown";
	bool useUpscaled = false;
	const char* historyDomain = "unknown";
	const char* presentDomain = "unknown";
	float temporalExposure = 1.0f;
	float presentExposure = 1.0f;
	float exposureStops = 0.0f;
	float resetThresholdStops = 0.0f;
	bool autoExposure = false;
	bool exposureTexture = false;
	bool taaApply = false;
};

struct NRITemporalTraceSnapshot
{
	const char* stage = "unknown";
	uint32_t frameIndex = 0;
	int debugMode = 0;
	const char* resolvedMainUpscaler = "unknown";
	const char* resolvedPostSharpen = "unknown";
	bool runAppTaa = false;
	bool guiCapture = false;
	const char* primaryDomain = "unknown";
	const char* secondaryDomain = "unknown";
	float temporalExposure = 1.0f;
	float primaryPresentExposure = 1.0f;
	float secondaryPresentExposure = 1.0f;
	bool resetHistory = false;
	const char* resetReason = "none";
	bool previousCamera = false;
	NRITextureStatusSnapshot historyInput;
	NRITextureStatusSnapshot historyOutput;
	NRITextureStatusSnapshot primary;
	NRITextureStatusSnapshot secondary;
	bool useUpscaled = false;
};

struct NRIPortalTraversalStatusSnapshot
{
	bool available = false;
	uint32_t depth = 0;
	uint32_t reflective = 0;
	uint32_t transfer = 0;
	uint32_t runtimeBound = 0;
	uint32_t hittableSurfaces = 0;
	uint32_t pendingPlanePortals = 0;
};

struct NRIResidentMapChunkRegistryStatusSnapshot
{
	bool available = false;
	uint64_t buildSerial = 0;
	uint32_t chunkCount = 0;
	uint32_t activeChunkCount = 0;
	uint32_t mappedChunkCount = 0;
	uint32_t accelerationResidentChunkCount = 0;
	uint32_t animatedCandidateChunkCount = 0;
	uint32_t animatedRefreshSuppressedChunkCount = 0;
	uint32_t mapWorldChunkCount = 0;
	uint32_t boundsValidCount = 0;
	uint32_t boundsInvalidCount = 0;
	float nearDistance = 0.0f;
	uint32_t visibleCount = 0;
	uint32_t invisibleNearCount = 0;
	uint32_t invisibleFarCount = 0;
	uint32_t invisibleUnknownCount = 0;
	uint32_t sampleChunkIndex = UINT32_MAX;
	float sampleCenter[3] = {};
	float sampleRadius = 0.0f;
	float sampleDistance = 0.0f;
	const char* sampleTier = "none";
};

class NRIRendererDiagnostics
{
public:
	void ResetSelfTestRouteSnapshot();
	void SetSelfTestRouteSnapshot(const char* routeName, const char* presenterName, const char* ownerName, const char* passes, bool denoiserRun, bool upscalerRun, bool exposureRun);
	const NRISelfTestRouteSnapshot& GetSelfTestRouteSnapshot() const { return mSelfTestRoute; }
	void EmitSelfTestSummary(const NRISelfTestSummarySnapshot& snapshot) const;

private:
	NRISelfTestRouteSnapshot mSelfTestRoute = {};
};

NRIBufferStatusSnapshot BuildNRIBufferStatusSnapshot(const NRIBufferResource& resource, const SceneBufferDebugStats& stats);
void PrintNRISceneBufferStatusSnapshot(const NRISceneBufferStatusSnapshot& snapshot);
void PrintNRITemporalStatusSnapshot(const NRITemporalStatusSnapshot& snapshot);
void PrintNRITemporalTraceSnapshot(const NRITemporalTraceSnapshot& snapshot);
void PrintNRIPortalTraversalStatusSnapshot(const NRIPortalTraversalStatusSnapshot& snapshot);
void PrintNRIResidentMapChunkRegistryStatusSnapshot(const NRIResidentMapChunkRegistryStatusSnapshot& snapshot);
