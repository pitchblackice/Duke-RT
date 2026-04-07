#pragma once

#include "../nri_output.h"
#include "nri_nrd.h"
#include "nri_resources.h"
#include "nri_scene_lights.h"
#include "nri_upscaler.h"
#include "../framegen/nri_framegen.h"

#include "../scene/nri_map_builder.h"
#include "../scene/nri_map_world.h"
#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"
#include "lightoverlay.h"

#include <chrono>
#include <cstdint>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

class NRIRenderDevice;
struct MapRecord;
struct PathTracingActorSpriteTraceEvent;

struct NRIDirectionalLightState
{
	bool enabled = false;
	bool shadow = true;
	bool fromOverlay = false;
	uint32_t ruleId = 0;
	uint64_t stateHash = 0;
	float direction[3] = { 0.3f, 0.85f, -0.4f };
	float color[3] = { 1.0f, 1.0f, 1.0f };
	float angularSize = 0.03f;
};

enum class NRIPTNightVisionMode : uint32_t
{
	None = 0,
	Duke = 1
};

struct NRIPTNightVisionState
{
	NRIPTNightVisionMode mode = NRIPTNightVisionMode::None;
	bool viewEligible = false;
	bool enabled = false;
	float strength01 = 0.0f;
	float remainingSeconds = 0.0f;
};

class NRIRenderer
{
public:
	struct PerfShellTraceStats
	{
		double totalMs = 0.0;
		double initResourcesMs = 0.0;
		double mapWorldMs = 0.0;
		double updateStateMs = 0.0;
		double sceneSelectMs = 0.0;
		double sceneLightsMs = 0.0;
		double residentLightRefreshMs = 0.0;
		double emissiveUpdateMs = 0.0;
		double emissiveTlasMs = 0.0;
		double surfaceProbeMs = 0.0;
		double frameGraphMs = 0.0;
		double otherMs = 0.0;
		double staticSceneMs = 0.0;
		double runtimeMutationMs = 0.0;
		double runtimeMutationAnalyzeMs = 0.0;
		double runtimeMutationRebuildMs = 0.0;
		double runtimeMutationAppendMs = 0.0;
		double runtimeSpaceLinkMs = 0.0;
		double runtimeDebugSphereMs = 0.0;
		double runtimeDebugSphereViewMs = 0.0;
		double runtimeDebugSphereGeoMs = 0.0;
		double runtimeDebugSphereMaterialMs = 0.0;
		double runtimeDebugSphereTuneMs = 0.0;
		double overlayAssembleMs = 0.0;
		double dynamicCaptureMs = 0.0;
		double persistentDynamicMs = 0.0;
		double dynamicAsMs = 0.0;
		double dynamicAsCreateMs = 0.0;
		double dynamicAsScratchMs = 0.0;
		double dynamicAsBuildMs = 0.0;
		double dynamicAsBarrierMs = 0.0;
		double restoreStaticSceneMs = 0.0;
		double copyFinalMs = 0.0;
		uint32_t runtimeMutationDirtyChunks = 0;
		uint32_t runtimeMutationRebuiltChunks = 0;
		uint32_t runtimeMutationHeldChunks = 0;
		uint32_t runtimeMutationReplacedChunks = 0;
		uint32_t runtimeMutationPrimitiveCount = 0;
		uint32_t runtimeMutationMaterialCount = 0;
		uint32_t runtimeSpaceLinkPrimitiveCount = 0;
		uint32_t runtimeSpaceLinkMaterialCount = 0;
		uint32_t runtimeDebugSphereCount = 0;
		uint32_t runtimeDebugSphereLongitudeSegments = 0;
		uint32_t runtimeDebugSphereLatitudeSegments = 0;
		uint32_t runtimeDebugSpherePrimitiveCount = 0;
		uint32_t runtimeDebugSphereMaterialCount = 0;
		uint32_t overlayPrimitiveCount = 0;
		uint32_t overlayMaterialCount = 0;
		uint32_t dynamicAsPrimitiveCount = 0;
		uint32_t dynamicAsVertexCount = 0;
		uint32_t dynamicAsIndexCount = 0;
		uint32_t activePrimitiveCount = 0;
		uint32_t dynamicPrimitiveCount = 0;
		uint32_t activeMaterialCount = 0;
		uint32_t sceneInstanceCount = 0;
		bool usedStaticMapScene = false;
		bool usedDynamicOverlay = false;
		bool usedPersistentDynamicEmissiveCache = false;
	};

	struct PerfResourceTraceStats
	{
		uint32_t waitCalls = 0;
		double waitMs = 0.0;
		uint32_t growEvents = 0;
		uint32_t overwriteEvents = 0;
		uint32_t sceneUploadCalls = 0;
		uint32_t sceneDataUploadCalls = 0;
		uint32_t emissiveUploadCalls = 0;
		uint64_t sceneUploadBytes = 0;
		uint64_t sceneDataUploadBytes = 0;
		uint64_t emissiveUploadBytes = 0;
	};

	explicit NRIRenderer(NRIRenderDevice* frameBuffer);
	~NRIRenderer();

	bool Initialize();
	void Shutdown();
	bool RenderScene(HWDrawInfo& di, int drawmode, bool portal);
	bool PreloadLevelScene(uint32_t outputWidth, uint32_t outputHeight, uint32_t targetWidth, uint32_t targetHeight);
	void ResetHistory();
	void NotifyCameraCut(const char* reason);
	void PrintStatus() const;
	void PrintSceneBufferStatus() const;
	void PrintSceneLightDump(float radius, uint32_t limit) const;
	bool AddRuntimePointLight(const float position[3], const float color[3], float intensity, float radius, uint32_t& outId);
	bool RemoveRuntimePointLight(uint32_t id);
	void ClearRuntimePointLights();
	void PrintRuntimePointLights() const;
	void PrintRuntimeLightClusterStatus() const;
	uint32_t GetRuntimePointLightCount() const;
	bool AddRuntimeDebugSphere(const float center[3], float diameter, float metalness, float roughness, uint32_t& outId);
	bool RemoveRuntimeDebugSphere(uint32_t id);
	void ClearRuntimeDebugSpheres();
	void PrintRuntimeDebugSpheres() const;
	uint32_t GetRuntimeDebugSphereCount() const;
	bool AddSpriteTileLightHeuristic(uint32_t textureId, const float color[3], float intensity, float radius, uint32_t flickerFrames, uint32_t& outRuleId);
	void ClearSpriteTileLightHeuristics();
	void PrintSpriteTileLightHeuristics() const;
	bool AddTextureEmissiveHeuristic(uint32_t textureId, uint32_t emissiveMode, float intensityScale, const float* emissiveColor, bool hasExplicitColor, uint32_t& outRuleId);
	void ClearTextureEmissiveHeuristics();
	void PrintTextureEmissiveHeuristics() const;
	void NotifyGlowControlChange();
	void NotifyMaterialLightingCalibrationChange();
	void NotifyDebugSphereTessellationChange();
	void PrintEmissiveSurfaceDump(float radius, uint32_t limit) const;
	void PrintSectorLightDump(float radius, uint32_t limit) const;
	void PrintSurfaceProbeStatus() const;
	void PrintMapChunkDump(int32_t chunkIndex) const;
	void PrintMapChunkCompare(int32_t chunkIndex) const;
	void TraceActorSpriteEvent(const PathTracingActorSpriteTraceEvent& event);
	bool IsPathTracingSupported() const { return mPathTracingSupported; }
	bool RefreshPathTracingAvailability();
	const char* GetAvailabilityReason() const;
	const PerfShellTraceStats& GetLastPerfShellTraceStats() const { return mLastPerfShellTraceStats; }
	const PerfResourceTraceStats& GetLastPerfResourceTraceStats() const { return mLastPerfResourceTraceStats; }

private:
	enum class FrameTextureSlot : uint32_t
	{
		ViewZ,
		Motion,
		NormalRoughness,
		BaseColorMetalness,
		UnfilteredDiffuse,
		UnfilteredSpecular,
		UnfilteredPenumbra,
		DenoisedDiffuse,
		DenoisedSpecular,
		DenoisedShadow,
		Composed,
		TraceTransparentOutput,
		DirectLighting,
		DirectEmission,
		TaaHistoryPing,
		TaaHistoryPong,
		Validation,
		SrInput,
		RrInput,
		UpscalerDepth,
		RrGuideDiffuseAlbedo,
		RrGuideSpecularAlbedo,
		RrGuideSpecularHitDistance,
		RrGuideNormalRoughness,
		VendorOutput,
		PostSharpenOutput,
		Final,
		Count
	};

	enum class PipelineSlot : uint32_t
	{
		TraceOpaque,
		Composition,
		TraceTransparent,
		Taa,
		RawPresent,
		FinalPresent,
		DlssSrBefore,
		DlssBefore,
		DlssAfter,
		Final,
		Count
	};

	struct CachedTexture
	{
		uint64_t key = 0;
		NRITextureResource resource;
	};

	struct CachedSkyTexture
	{
		uint64_t key = 0;
		nri_scene::PTSkyMode mode = nri_scene::PTSkyMode::None;
		NRITextureResource resource;
	};

	struct SkyState
	{
		nri_scene::PTSkyMode mode = nri_scene::PTSkyMode::None;
		nri_scene::PTSkySourceType sourceType = nri_scene::PTSkySourceType::None;
		FGameTexture* texture = nullptr;
		uint32_t faceMask = 0;
		bool flipTop = false;
	};

	struct PreservedStaticMapSkyState
	{
		bool valid = false;
		uint64_t buildSerial = 0;
		nri_scene::SceneView sceneView;
	};

	struct SceneBufferDebugStats
	{
		const char* label = "";
		uint32_t growthCount = 0;
		uint32_t overwriteCount = 0;
		uint32_t uploadCount = 0;
		uint32_t growEventsLastFrame = 0;
		uint32_t overwriteEventsLastFrame = 0;
		uint64_t bytesUploadedLastFrame = 0;
		uint64_t peakUsedBytes = 0;
	};

	struct SurfaceProbeResult
	{
		bool valid = false;
		bool hit = false;
		uint32_t sceneDataSource = UINT32_MAX;
		uint32_t sceneOwner = 0;
		uint32_t primitiveIndex = UINT32_MAX;
		uint32_t materialIndex = UINT32_MAX;
		uint32_t primitiveFlags = 0;
		uint32_t materialLightingFlags = 0;
		uint32_t textureId = 0;
		uint32_t materialClass = 0;
		uint32_t emissiveMode = 0;
		uint32_t emissiveTextureIndex = UINT32_MAX;
		uint32_t normalTextureIndex = UINT32_MAX;
		uint32_t metallicTextureIndex = UINT32_MAX;
		uint32_t roughnessTextureIndex = UINT32_MAX;
		float lightLevel = 0.0f;
		float alpha = 1.0f;
		float metalnessHint = 0.0f;
		float roughnessHint = 0.45f;
		float averageColor[3] = { 1.0f, 1.0f, 1.0f };
		float emissiveColor[3] = {};
		float glowColor[3] = {};
		float distance = 0.0f;
		float position[3] = {};
		float normal[3] = {};
		nri_scene::SurfaceProvenance provenance = {};
	};

	struct SurfaceProbeEmissiveDiagnostics
	{
		bool sceneLightSurfaceMatch = false;
		bool activeEmissiveSurfaceMatch = false;
		uint32_t sceneLightMaterialIndex = UINT32_MAX;
		uint32_t emissivePrimitiveMatchCount = 0;
	};

	struct SurfaceProbeFrameState
	{
		bool valid = false;
		bool usesStaticMapScene = false;
		bool staticTlasExcludesReplacedChunks = false;
		bool staticProbeExcludesReplacedChunks = false;
		uint32_t staticPrimitiveCount = 0;
		uint32_t runtimeSpaceLinkPrimitiveCount = 0;
		uint32_t runtimeMutationPrimitiveCount = 0;
		uint32_t dynamicPrimitiveCount = 0;
	};

	struct RuntimePointLightGpuData
	{
		float position[3] = {};
		float radius = 0.0f;
		float color[3] = { 1.0f, 1.0f, 1.0f };
		float intensity = 1.0f;
	};

	struct RuntimeDebugSphere
	{
		uint32_t id = 0;
		float center[3] = {};
		float diameter = 0.0f;
		float metalness = 1.0f;
		float roughness = 0.05f;
		uint32_t cachedLongitudeSegments = 0;
		uint32_t cachedLatitudeSegments = 0;
		bool cacheValid = false;
		nri_scene::GeometryData geometry;
		nri_scene::MaterialBridgeData materialBridge;
	};

	struct TransientMuzzleFlashSlot
	{
		uint64_t stableKey = 0;
		uint32_t slotIndex = 0;
		uint32_t ruleId = 0;
		uint64_t sourceEventSerial = 0;
		int32_t emitterActorIndex = -1;
		float renderPosition[3] = {};
		float color[3] = { 1.0f, 1.0f, 1.0f };
		float peakIntensity = 0.0f;
		float radius = 0.0f;
		double activationTimeSeconds = 0.0;
		double endTimeSeconds = 0.0;
		bool occupied = false;
	};

	struct RuntimeLightTileHeaderGpuData
	{
		uint32_t indexOffset = 0;
		uint32_t indexCount = 0;
	};

	struct EmissivePrimitiveHeaderGpuData
	{
		uint32_t activeCount = 0;
		uint32_t dominantIndex = UINT32_MAX;
		uint32_t flags = 0;
		float totalPower = 0.0f;
	};

	struct EmissivePrimitiveGpuData
	{
		uint32_t dataSource = 0;
		uint32_t primitiveIndex = UINT32_MAX;
		uint32_t sourceFlags = 0;
		uint32_t textureId = 0;
		float primitiveArea = 0.0f;
		float powerEstimate = 0.0f;
		float selectionWeight = 0.0f;
		float selectionPdf = 0.0f;
		uint32_t stableKeyLo = 0;
		uint32_t stableKeyHi = 0;
	};

	struct EmissivePrimitiveDebugRecord
	{
		uint64_t stableKey = 0;
		uint64_t surfaceStableKey = 0;
		uint32_t dataSource = 0;
		uint32_t primitiveIndex = UINT32_MAX;
		uint32_t materialIndex = UINT32_MAX;
		uint32_t sourceFlags = 0;
		uint32_t sourceRuleId = 0;
		uint32_t textureId = 0;
		uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_None;
		uint32_t emissiveTextureIndex = UINT32_MAX;
		int32_t actorIndex = -1;
		float center[3] = {};
		float primitiveArea = 0.0f;
		float powerEstimate = 0.0f;
		float selectionWeight = 0.0f;
		float selectionPdf = 0.0f;
		float emissiveColor[3] = {};
		float emissiveIntensity = 0.0f;
	};

	struct EmissiveSamplingBuildContext
	{
		const nri_scene::GeometryData* staticGeometry = nullptr;
		const nri_scene::GeometryData* capturedGeometry = nullptr;
		const nri_scene::GeometryData* runtimeMutationGeometry = nullptr;
		uint32_t runtimeMutationPrimitiveBaseOffset = 0;
		const nri_scene::GeometryData* dynamicGeometry = nullptr;
		uint32_t dynamicPrimitiveBaseOffset = 0;
	};

	struct SectorLightHeaderGpuData
	{
		uint32_t sectorCount = 0;
		uint32_t activeCount = 0;
		uint32_t pulsingCount = 0;
		uint32_t flags = 0;
	};

	struct SectorLightGpuData
	{
		float ambientColor[3] = {};
		float ambientIntensity = 0.0f;
		float hemisphereColor[3] = {};
		float hemisphereAmount = 0.0f;
		float fogAmount = 0.0f;
		float pulseScale = 1.0f;
		uint32_t sourceFlags = 0;
		int32_t paletteIndex = -1;
		int32_t lotag = 0;
		int32_t hitag = 0;
	};

	struct StaticMapSceneCache
	{
		struct ChunkCache
		{
			uint32_t chunkIndex = UINT32_MAX;
			uint32_t vertexOffset = 0;
			uint32_t vertexCount = 0;
			uint32_t indexOffset = 0;
			uint32_t indexCount = 0;
			uint32_t primitiveOffset = 0;
			uint32_t primitiveCount = 0;
			uint32_t materialOffset = 0;
			uint32_t materialCount = 0;
			uint64_t animatedMaterialSignature = 0;
			uint64_t animatedGeometrySignature = 0;
			nri_scene::MaterialBridgeData materialBridge;
			NRIAccelerationStructureResource accelerationStructure;
		};

		bool valid = false;
		bool texturesResident = false;
		bool buffersResident = false;
		bool accelerationResident = false;
		uint64_t buildSerial = 0;
		uint32_t sceneBuildCount = 0;
		uint32_t gpuUploadCount = 0;
		uint32_t accelerationBuildCount = 0;
		uint32_t animatedRefreshCount = 0;
		uint32_t animatedRefreshUploadCount = 0;
		uint32_t animatedGeometryFallbackCount = 0;
		uint32_t reuseCount = 0;
		nri_scene::SceneView sceneView;
		std::vector<nri_scene::SceneView> lightChunkViews;
		nri_scene::GeometryData geometry;
		nri_scene::MaterialBridgeData materialBridge;
		std::vector<nri_scene::MaterialData> gpuMaterials;
		std::vector<ChunkCache> chunks;
		uint32_t tlasInstanceCount = 0;
	};

	struct DynamicSceneFrameState
	{
		uint32_t spriteSurfaceCount = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialCount = 0;
		uint32_t modelCount = 0;
		uint32_t unsupportedModelCount = 0;
		uint32_t mirrorExtendedSurfaceCount = 0;
		uint32_t mirrorExtendedPrimitiveCount = 0;
		uint32_t mirrorExtendedMaterialCount = 0;
		uint32_t mirrorExtendedModelCount = 0;
		uint32_t mirrorExtendedUnsupportedModelCount = 0;
		uint32_t mirrorPlayerSurfaceCount = 0;
		uint32_t mirrorPlayerPrimitiveCount = 0;
		uint32_t mirrorPlayerMaterialCount = 0;
		uint32_t mirrorPlayerModelCount = 0;
		uint32_t mirrorPlayerUnsupportedModelCount = 0;
		uint32_t asBuildCount = 0;
	};

	struct PersistentDynamicEmissiveCache
	{
		bool valid = false;
		uint32_t surfaceCount = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialCount = 0;
		nri_scene::SceneView sceneView;
		nri_scene::GeometryData geometry;
		nri_scene::MaterialBridgeData materialBridge;
	};

	struct ActorSpriteDebugStats
	{
		uint32_t lastPruneChecks = 0;
		uint32_t lastPruneMatches = 0;
		uint32_t lastPruneDroppedMissingActor = 0;
		uint32_t lastPruneDroppedMissingActorIndex = 0;
		uint32_t lastPruneDroppedNullLiveTexture = 0;
		uint32_t lastPruneDroppedTextureMismatch = 0;
		uint32_t lastPruneDroppedPaletteMismatch = 0;
	};

	struct SceneTextureOverflowDebugStats
	{
		uint32_t textureCountLastBuild = 0;
		uint32_t truncatedTextureCountLastBuild = 0;
		uint32_t baseTextureClampCountLastBuild = 0;
		uint32_t normalTextureClampCountLastBuild = 0;
		uint32_t metallicTextureClampCountLastBuild = 0;
		uint32_t roughnessTextureClampCountLastBuild = 0;
		uint32_t emissiveTextureClampCountLastBuild = 0;
		uint64_t totalOverflowBuilds = 0;
		bool warningLogged = false;
	};

	struct DescriptorCoherencyDebugStats
	{
		uint64_t actorMaterialBuilds = 0;
		uint64_t sceneTextureSetUpdates = 0;
		uint64_t sceneDataSetUpdates = 0;
		uint64_t forcedSceneTextureSyncs = 0;
		uint64_t forcedSceneDataSyncs = 0;
		uint64_t lastMaterialBridgeHash = 0;
		uint64_t lastActorSpriteMaterialHash = 0;
		uint64_t lastSceneTextureDescriptorHash = 0;
		uint64_t lastSceneDataDescriptorHash = 0;
		uint32_t lastMaterialCount = 0;
		uint32_t lastTextureCount = 0;
		uint32_t lastActorSpriteSurfaceCount = 0;
		uint32_t lastActorSpriteActorCount = 0;
		uint32_t lastSceneTextureDescriptorCount = 0;
		uint32_t lastSceneDataDescriptorCount = 0;
		uint32_t lastSceneTextureQueuedFrameIndex = 0;
		uint32_t lastSceneDataQueuedFrameIndex = 0;
		uint32_t lastSceneTextureOutstandingQueuedFrames = 0;
		uint32_t lastSceneDataOutstandingQueuedFrames = 0;
		uint64_t lastSceneTextureQueuedFrameFence = 0;
		uint64_t lastSceneDataQueuedFrameFence = 0;
		uint64_t lastSceneTextureSubmittedFence = 0;
		uint64_t lastSceneDataSubmittedFence = 0;
		bool lastSceneTextureForcedSync = false;
		bool lastSceneDataForcedSync = false;
		std::string lastMaterialBuildLabel;
		std::string lastSceneTextureReason;
		std::string lastSceneDataReason;
	};

	struct RuntimeMapMutationCache
	{
		struct ChunkReplacement
		{
			nri_scene::PTMapChunkMutationBaseline baseline;
			uint64_t baselineSignature = 0;
			uint64_t liveSignature = 0;
			uint64_t animatedMaterialSignature = 0;
			uint64_t lastTraceSignature = UINT64_MAX;
			uint64_t lastTraceAnimatedMaterialSignature = UINT64_MAX;
			uint32_t reasonMask = 0;
			uint32_t sectionDirtyCount = 0;
			uint32_t lastTraceReasonMask = UINT32_MAX;
			uint32_t traceCount = 0;
			bool active = false;
			bool valid = false;
			bool sectorDirty = false;
			bool dragged = false;
			bool blindSpot = false;
			bool excludeStaticChunk = false;
			bool lastTraceActive = false;
			bool lastTraceBlindSpot = false;
			bool animationOnlyRefreshed = false;
			bool lastTraceAnimationOnlyRefreshed = false;
			uint32_t surfaceCount = 0;
			uint32_t triangleCount = 0;
			SceneLightSystem::SurfaceIdentityOverrides lightIdentityOverrides;
			nri_scene::SceneView sceneView;
			nri_scene::GeometryData geometry;
			nri_scene::MaterialBridgeData materialBridge;
		};

		std::vector<ChunkReplacement> chunks;
		std::vector<uint8_t> replacedChunkMask;
	};

	struct RuntimeMapMutationFrameState
	{
		bool active = false;
		uint32_t dirtyChunkCount = 0;
		uint32_t replacedChunkCount = 0;
		uint32_t rebuiltChunkCount = 0;
		uint32_t heldChunkCount = 0;
		uint32_t blindSpotChunkCount = 0;
		uint32_t sectorGeometryChunkCount = 0;
		uint32_t sectorMaterialChunkCount = 0;
		uint32_t wallGeometryChunkCount = 0;
		uint32_t wallMaterialChunkCount = 0;
		uint32_t sectorDirtyChunkCount = 0;
		uint32_t sectionDirtyChunkCount = 0;
		uint32_t draggedChunkCount = 0;
		uint32_t animatedRefreshChunkCount = 0;
		uint32_t replacementSurfaceCount = 0;
		uint32_t replacementTriangleCount = 0;
		uint32_t materialCount = 0;
	};

	struct RuntimeSpaceLinkFrameState
	{
		bool active = false;
		bool geoEffectActive = false;
		bool topologyChanged = false;
		bool queryAttempted = false;
		bool queryRejected = false;
		int32_t candidateSectorIndex = -1;
		int32_t candidateSectorLotag = -1;
		int32_t sourceSectorIndex = -1;
		int32_t reportedGeoCount = 0;
		uint32_t viewRootSectorCount = 0;
		uint32_t visibleSectorCount = 0;
		uint32_t providerSectorCount = 0;
		uint32_t geoProviderCount = 0;
		uint32_t providerGroupCount = 0;
		uint32_t localSpaceMatchedProviderCount = 0;
		uint32_t visibleMatchedProviderCount = 0;
		uint32_t linkCount = 0;
		uint32_t translatedChunkCount = 0;
		uint32_t orphanLocalSpaceCount = 0;
		uint32_t unresolvedRuntimePortalCount = 0;
		uint32_t surfaceCount = 0;
		uint32_t triangleCount = 0;
		uint32_t materialCount = 0;
	};

	struct RuntimeChunkTranslationState
	{
		uint32_t chunkIndex = UINT32_MAX;
		float dx = 0.0f;
		float dz = 0.0f;
	};

	struct RuntimeLinkTraceState
	{
		bool valid = false;
		int32_t candidateSectorIndex = -1;
		int32_t sourceSectorIndex = -1;
		bool geoEffectActive = false;
		uint32_t visibleTaggedSectorCount = 0;
		uint32_t visible848SectorCount = 0;
		uint32_t visibleTeleportSectorCount = 0;
		uint32_t taggedVisibleSectorStoredCount = 0;
		std::array<RuntimeTaggedSectorDebugInfo, 8> taggedVisibleSectors = {};
		uint32_t nearbyControlSectorStoredCount = 0;
		std::array<RuntimeTaggedSectorDebugInfo, 12> nearbyControlSectors = {};
		RuntimeLinkDebugState game = {};
	};

	struct SceneInstanceData
	{
		uint32_t primitiveOffset = 0;
		uint32_t dataSource = 0;
		uint32_t reserved0 = 0;
		uint32_t reserved1 = 0;
	};

	enum SceneDataBufferMask : uint32_t
	{
		SceneDataBufferMask_None = 0,
		SceneDataBufferMask_Static = 1 << 0,
		SceneDataBufferMask_Dynamic = 1 << 1,
	};

	bool CreatePipelineLayout();
	bool CreateTaaPipelineLayout();
	bool CreatePresentPipelineLayout();
	bool CreatePipelines();
	bool AllocateDescriptorSets();
	bool EnsureFrameResources(uint32_t outputWidth, uint32_t outputHeight, uint32_t targetWidth, uint32_t targetHeight);
	bool DispatchBootstrapView();
	bool UseFallbackSceneTextures(bool preserveExistingSky, const char* reason = nullptr);
	bool EnsurePaletteTexture(const nri_scene::MaterialBridgeData& materials);
	bool EnsureSceneTextures(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& outGpuMaterials, bool preserveExistingSky, const char* reason = nullptr);
	bool EnsureSkyTexture(const nri_scene::SceneView& sceneView, bool preserveExistingSky);
	bool EnsureStaticMapScene();
	bool RefreshStaticMapAnimatedMaterials();
	bool UploadSceneBuffers(const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials);
	bool UploadSceneBuffers(
		NRIBufferResource& vertexBuffer,
		NRIBufferResource& indexBuffer,
		NRIBufferResource& primitiveBuffer,
		NRIBufferResource& materialBuffer,
		const nri_scene::GeometryData& geometry,
		const std::vector<nri_scene::MaterialData>& materials);
	bool BuildStaticMapAccelerationStructures();
	bool BuildTopLevelAccelerationStructure(const std::vector<nri::TopLevelInstance>& instances, uint32_t sceneBufferMask);
	bool BuildEmissiveTopLevelAccelerationStructure();
	bool BuildDynamicAccelerationStructure(const nri_scene::GeometryData& geometry);
	bool RefreshResidentStaticSceneDataSet();
	bool BuildRuntimeMapMutationOverlay(nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials);
	bool BuildRuntimeSpaceLinkOverlay(HWDrawInfo& di, nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials);
	void BuildRuntimePointLightUpload(std::vector<RuntimePointLightGpuData>& outLights) const;
	uint64_t BuildRuntimeLightPayloadHash() const;
	uint64_t BuildRuntimeLightClusterCameraHash() const;
	uint64_t BuildEmissiveSamplingPayloadHash(const EmissiveSamplingBuildContext& context) const;
	uint64_t BuildSectorLightingPayloadHash() const;
	void BuildEmissiveSamplingUpload(
		const EmissiveSamplingBuildContext& context,
		EmissivePrimitiveHeaderGpuData& outHeader,
		std::vector<EmissivePrimitiveGpuData>& outPrimitives,
		std::vector<float>& outCdf,
		std::vector<EmissivePrimitiveDebugRecord>& outDebugRecords) const;
	bool UpdateEmissiveSamplingBuffers(const EmissiveSamplingBuildContext& context);
	void UpdateBoundSectorLightingState();
	void BuildSectorLightingUpload(
		SectorLightHeaderGpuData& outHeader,
		std::vector<SectorLightGpuData>& outSectors);
	bool UpdateReprojectionBuffer();
	bool UpdateVisibleChunkBuffer();
	bool UpdateVisibleFlatPlaneBuffer();
	bool UpdateSceneDataSet(
		const NRIBufferResource& staticVertexBuffer,
		const NRIBufferResource& staticIndexBuffer,
		const NRIBufferResource& staticPrimitiveBuffer,
		const NRIBufferResource& staticMaterialBuffer,
		const NRIBufferResource& dynamicVertexBuffer,
		const NRIBufferResource& dynamicIndexBuffer,
		const NRIBufferResource& dynamicPrimitiveBuffer,
		const NRIBufferResource& dynamicMaterialBuffer,
		const std::vector<SceneInstanceData>& sceneInstances,
		uint32_t staticPrimitiveCount,
		uint32_t dynamicPrimitiveCount,
		uint32_t staticMaterialCount,
		uint32_t dynamicMaterialCount,
		const char* reason = nullptr);
	bool CommitSceneDataDescriptors(const char* reason);
	void TraceActorSpriteMaterialAssignments(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& outMaterials, const char* traceLabel);
	void TraceSharedDescriptorRewrite(const char* setName, const char* reason, uint64_t descriptorHash, uint32_t descriptorCount, bool sceneTextureSet);
	uint32_t CountPotentialOutstandingQueuedFrames() const;
	void BuildStaticMapInstances(std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances, const std::vector<uint8_t>* replacedChunkMask = nullptr) const;
	void BuildFilteredStaticMapGeometry(const std::vector<uint8_t>& replacedChunkMask, nri_scene::GeometryData& outGeometry) const;
	bool RestoreStaticTopLevelScene();
	bool DispatchFrameGraph(HWDrawInfo& di, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials, int drawmode);
	bool DispatchTraceOpaque(HWDrawInfo& di, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials);
	bool DispatchDenoiser();
	bool DispatchComposition(FrameTextureSlot outputSlot = FrameTextureSlot::Composed);
	bool DispatchTraceTransparent();
	bool DispatchUpscalerPrepass(NRIMainUpscalerKind mainKind);
	bool DispatchRawPresent(FrameTextureSlot inputSlot, FrameTextureSlot secondarySlot = FrameTextureSlot::Count, FrameTextureSlot tertiarySlot = FrameTextureSlot::Count);
	bool DispatchFinalPresent(FrameTextureSlot inputSlot);
	bool DispatchUpscaleChain();
	bool DispatchFinal();
	void RefreshMapWorld();
	bool CheckPathTracingSupport();
	void UpdatePerFrameState(HWDrawInfo& di);
	void UpdateNightVisionState();
	void ResetSceneBufferFrameStats();
	void LogBridgeStats(const nri_scene::SceneDebugStats& stats);
	void PrintMapWorldStatus() const;
	void PrintPortalTraversalStatus() const;
	void PrintStaticMapSceneStatus() const;
	void PrintDynamicSceneStatus() const;
	void PrintTemporalStatus() const;
	void PrintRuntimeMapMutationStatus() const;
	void PrintRuntimeSpaceLinkStatus() const;
	void RequestHistoryReset(const char* reason, bool clearPreviousCameraState = false, bool clearRuntimeChunkTranslationHistory = false);
	void NoteLightHistoryChange(const char* reason);
	void ArmTemporalTraceBudget(const char* reason);
	void TraceTemporalState(const char* stage, NRIMainUpscalerKind resolvedMainUpscaler, NRIPostSharpenKind resolvedPostSharpen, bool runAppTaa, FrameTextureSlot primarySlot, FrameTextureSlot secondarySlot) const;
	void TraceRuntimeLinkEvents(HWDrawInfo& di);
	void TraceRuntimeMapMutationChunk(const nri_scene::PTMapChunk& mapChunk, RuntimeMapMutationCache::ChunkReplacement& replacement);
	void TraceSkyState(const nri_scene::SceneView& sceneView, const char* action, uint64_t resolvedKey);
	void UpdateSurfaceProbe(const nri_scene::GeometryData& geometry, const nri_scene::MaterialBridgeData* materials, bool allowLogging);
	SurfaceProbeEmissiveDiagnostics BuildSurfaceProbeEmissiveDiagnostics(const SurfaceProbeResult& probe) const;
	bool BuildRuntimeDebugSphereOverlay(nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials);
	bool EnsureRuntimeDebugSphereCache(RuntimeDebugSphere& sphere);
	void AppendRuntimeDebugSphereToSceneView(const RuntimeDebugSphere& sphere, nri_scene::SceneView& sceneView) const;
	void RefreshSceneLightSystem(
		bool usedStaticMapScene,
		const nri_scene::SceneView* capturedSceneView,
		const nri_scene::MaterialBridgeData* capturedMaterials,
		const nri_scene::SceneView* dynamicSceneView,
		const nri_scene::MaterialBridgeData* dynamicMaterials);
	void RefreshResolvedMuzzleFlashRuleLookup(const ResolvedLightOverlaySet& resolvedLightOverlays);
	void ResetMuzzleFlashOverlayState(const char* reason);
	const ResolvedLightOverlayMuzzleFlashRule* FindResolvedMuzzleFlashRule(const FString& eventId) const;
	void RefreshTransientMuzzleFlashLights(double currentTimeSeconds);
	void ResetPersistentDynamicEmissiveCache();
	void PrunePersistentDynamicEmissiveCacheToLiveActors();
	bool RebuildPersistentDynamicEmissiveCache(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials);
	void RebuildStartupMutationBaseline();
	void BuildMaterialsWithActorOverrides(nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& outMaterials, const char* traceLabel = nullptr);
	void ApplyEmissiveMaterialOverrides(const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& inOutGpuMaterials) const;
	void ApplyActorShadowMaterialOverrides(const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& inOutGpuMaterials) const;
	void QueueStaticMapSceneLightingInvalidation();
	void InvalidateStaticMapSceneForMaterialLighting();
	void LogFallback(const char* reason);
	void CopyFinalToActiveTarget();
	void UpdateFrameGenerationFrameDesc();
	void UpdateFrameGenerationHistoryPolicy(int debugMode, const NRIFrameGenerationPolicy& frameGenPolicy, bool preserveHistory);
	void NoteSuccessfulRealFrame();
	void CopyTexture(NRITextureResource& source, NRITextureResource& destination);
	void CopyTextureToActiveTarget(NRITextureResource& source);

	void DestroyCachedTextures();
	void DestroyFrameTextures();
	void DestroySceneBuffers();
	void DestroyAccelerationStructures();
	void DestroyStaticMapSceneCache();
	void DestroyBufferResource(NRIBufferResource& resource);
	void DestroyAccelerationStructureResource(NRIAccelerationStructureResource& resource);
	const NRIBufferResource& GetActiveVertexBuffer() const;
	const NRIBufferResource& GetActiveIndexBuffer() const;
	const NRIBufferResource& GetActivePrimitiveBuffer() const;
	const NRIBufferResource& GetActiveMaterialBuffer() const;
	void BindSceneRootDescriptors();

	bool CreateStructuredBuffer(NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after);
	bool EnsureStructuredBuffer(NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after);
	bool CreateBufferWithoutView(NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage);
	void BuildRuntimeLightClusterUpload(
		std::vector<RuntimeLightTileHeaderGpuData>& outHeaders,
		std::vector<uint32_t>& outIndices,
		uint32_t& outTileCountX,
		uint32_t& outTileCountY,
		uint32_t& outTileIndexCount,
		uint32_t& outMaxTileOccupancy) const;
	bool UpdateSamplerSet();
	bool UpdateSceneTextureSet(const std::vector<nri::Descriptor*>& descriptors, const char* reason = nullptr);
	bool UpdateFrameTextureSet();
	bool UpdateFrameTextureSet(nri::DescriptorSet* set, const std::array<nri::Descriptor*, 14>& descriptors);
	bool UpdateOutputSet();
	bool UpdateOutputSet(nri::DescriptorSet* set, const std::array<nri::Descriptor*, 15>& descriptors);
	bool CreateFrameTexture(FrameTextureSlot slot, uint32_t width, uint32_t height, nri::Format format);
	void PrepareSceneTextureInputsForCompute();
	void ResetPerfTraceStats();
	void WaitForCommandsTracked();
	void NotePerfBufferUpload(const SceneBufferDebugStats* stats, uint64_t size, bool growth);
	NRITextureResource& GetFrameTexture(FrameTextureSlot slot) { return mFrameTextures[(size_t)slot]; }
	const NRITextureResource& GetFrameTexture(FrameTextureSlot slot) const { return mFrameTextures[(size_t)slot]; }
	nri::Pipeline* GetPipeline(PipelineSlot slot) const { return mPipelines[(size_t)slot]; }
	NRIMainUpscalerKind GetSelectedMainUpscalerKind() const;
	NRIMainUpscalerKind ResolveMainUpscalerKind(bool logFallback);
	NRIMainUpscalerKind GetResolvedMainUpscalerKindForStatus() const;
	NRIPostSharpenKind GetSelectedPostSharpenKind() const;
	NRIPostSharpenKind ResolvePostSharpenKind(bool logFallback);
	NRIPostSharpenKind GetResolvedPostSharpenKindForStatus() const;
	nri::UpscalerMode GetSelectedUpscalerMode() const;
	bool IsMainUpscalerSupported(NRIMainUpscalerKind kind) const;
	bool IsPostSharpenSupported(NRIPostSharpenKind kind) const;
	void FillMatrix(float* outMatrix, const VSMatrix& matrix) const;
	const char* GetFrameTextureSlotName(FrameTextureSlot slot) const;

	NRIRenderDevice* mFrameBuffer = nullptr;
	nri::PipelineLayout* mPipelineLayout = nullptr;
	nri::PipelineLayout* mTaaPipelineLayout = nullptr;
	nri::PipelineLayout* mPresentPipelineLayout = nullptr;
	std::array<nri::Pipeline*, (size_t)PipelineSlot::Count> mPipelines = {};
	nri::DescriptorSet* mSamplerSet = nullptr;
	nri::DescriptorSet* mSceneTextureSet = nullptr;
	nri::DescriptorSet* mSceneDataSet = nullptr;
	nri::DescriptorSet* mFrameTextureSet = nullptr;
	nri::DescriptorSet* mOutputSet = nullptr;
	nri::DescriptorSet* mCompositionFrameTextureSet = nullptr;
	nri::DescriptorSet* mCompositionOutputSet = nullptr;
	nri::DescriptorSet* mUpscalerPrepassFrameTextureSet = nullptr;
	nri::DescriptorSet* mUpscalerPrepassOutputSet = nullptr;
	nri::DescriptorSet* mTaaFrameTextureSet = nullptr;
	nri::DescriptorSet* mTaaOutputSet = nullptr;
	nri::DescriptorSet* mRawPresentFrameTextureSet = nullptr;
	nri::DescriptorSet* mRawPresentOutputSet = nullptr;
	nri::DescriptorSet* mFinalPresentFrameTextureSet = nullptr;
	nri::DescriptorSet* mFinalPresentOutputSet = nullptr;

	NRITextureResource* GetActiveSkyTexture() { return mActiveSkyTextureIndex < mSkyTextureCache.size() ? &mSkyTextureCache[mActiveSkyTextureIndex].resource : nullptr; }
	const NRITextureResource* GetActiveSkyTexture() const { return mActiveSkyTextureIndex < mSkyTextureCache.size() ? &mSkyTextureCache[mActiveSkyTextureIndex].resource : nullptr; }

	NRITextureResource mPaletteTexture;
	std::array<NRITextureResource, (size_t)FrameTextureSlot::Count> mFrameTextures = {};

	NRIBufferResource mVertexBuffer;
	NRIBufferResource mIndexBuffer;
	NRIBufferResource mPrimitiveBuffer;
	NRIBufferResource mMaterialBuffer;
	NRIBufferResource mStaticVertexBuffer;
	NRIBufferResource mStaticIndexBuffer;
	NRIBufferResource mStaticPrimitiveBuffer;
	NRIBufferResource mStaticMaterialBuffer;
	NRIBufferResource mTlasInstanceBuffer;
	NRIBufferResource mSceneInstanceBuffer;
	NRIBufferResource mPortalBuffer;
	NRIBufferResource mRuntimeLightBuffer;
	NRIBufferResource mRuntimeLightTileHeaderBuffer;
	NRIBufferResource mRuntimeLightTileIndexBuffer;
	NRIBufferResource mEmissivePrimitiveHeaderBuffer;
	NRIBufferResource mEmissivePrimitiveBuffer;
	NRIBufferResource mEmissivePrimitiveCdfBuffer;
	NRIBufferResource mEmissiveTlasInstanceBuffer;
	NRIBufferResource mSectorLightHeaderBuffer;
	NRIBufferResource mSectorLightBuffer;
	NRIBufferResource mReprojectionBuffer;
	NRIBufferResource mVisibleChunkBuffer;
	NRIBufferResource mVisibleFlatPlaneBuffer;
	NRIBufferResource mScratchBuffer;
	NRIBufferResource mTopLevelScratchBuffer;
	SceneBufferDebugStats mVertexBufferStats = { "Vertex" };
	SceneBufferDebugStats mIndexBufferStats = { "Index" };
	SceneBufferDebugStats mPrimitiveBufferStats = { "Primitive" };
	SceneBufferDebugStats mMaterialBufferStats = { "Material" };
	SceneBufferDebugStats mSceneInstanceBufferStats = { "SceneInstance" };
	SceneBufferDebugStats mPortalBufferStats = { "Portal" };
	SceneBufferDebugStats mRuntimeLightBufferStats = { "RuntimeLight" };
	SceneBufferDebugStats mRuntimeLightTileHeaderBufferStats = { "RuntimeLightTileHeader" };
	SceneBufferDebugStats mRuntimeLightTileIndexBufferStats = { "RuntimeLightTileIndex" };
	SceneBufferDebugStats mEmissivePrimitiveHeaderBufferStats = { "EmissivePrimitiveHeader" };
	SceneBufferDebugStats mEmissivePrimitiveBufferStats = { "EmissivePrimitive" };
	SceneBufferDebugStats mEmissivePrimitiveCdfBufferStats = { "EmissivePrimitiveCdf" };
	SceneBufferDebugStats mEmissiveTlasInstanceBufferStats = { "EmissiveTLASInstance" };
	SceneBufferDebugStats mSectorLightHeaderBufferStats = { "SectorLightHeader" };
	SceneBufferDebugStats mSectorLightBufferStats = { "SectorLight" };
	SceneBufferDebugStats mReprojectionBufferStats = { "Reprojection" };
	SceneBufferDebugStats mVisibleChunkBufferStats = { "VisibleChunk" };
	SceneBufferDebugStats mVisibleFlatPlaneBufferStats = { "VisibleFlatPlane" };
	PerfShellTraceStats mLastPerfShellTraceStats = {};
	PerfResourceTraceStats mLastPerfResourceTraceStats = {};

	NRIAccelerationStructureResource mDynamicBottomLevelAS;
	NRIAccelerationStructureResource mTopLevelAS;
	NRIAccelerationStructureResource mEmissiveTopLevelAS;

	std::vector<CachedTexture> mTextureCache;
	std::vector<CachedSkyTexture> mSkyTextureCache;
	NRINrdContext mNrd;
	NRIUpscalerContext mUpscaler;
	nri_scene::PTMapWorld mMapWorld;
	StaticMapSceneCache mStaticMapScene;
	RuntimeMapMutationCache mRuntimeMapMutations;
	DynamicSceneFrameState mDynamicSceneLastFrame = {};
	PersistentDynamicEmissiveCache mPersistentDynamicEmissiveCache = {};
	ActorSpriteDebugStats mActorSpriteDebugStats = {};
	SceneTextureOverflowDebugStats mSceneTextureOverflowStats = {};
	DescriptorCoherencyDebugStats mDescriptorCoherencyDebugStats = {};
	RuntimeMapMutationFrameState mRuntimeMapLastFrame = {};
	RuntimeSpaceLinkFrameState mRuntimeSpaceLinkLastFrame = {};
	RuntimeLinkTraceState mLastRuntimeLinkTraceState = {};
	std::vector<RuntimeChunkTranslationState> mRuntimeChunkTranslationHistory;
	nri_scene::SceneDebugStats mLastStats = {};
	SceneLightSystem mSceneLights;
	NRIDirectionalLightState mDirectionalLightState = {};
	NRIPTNightVisionState mNightVisionState = {};
	std::unordered_map<std::string, ResolvedLightOverlayMuzzleFlashRule> mResolvedMuzzleFlashRuleLookup;
	std::vector<TransientMuzzleFlashSlot> mTransientMuzzleFlashSlots;
	std::vector<SceneLightSystem::SceneAnalyticLight> mTransientMuzzleFlashLights;
	std::array<nri::Descriptor*, 21> mSceneDataDescriptors = {};
	std::array<nri::Descriptor*, 14> mFrameInputDescriptors = {};
	std::array<nri::Descriptor*, 15> mOutputDescriptors = {};
	std::vector<SceneInstanceData> mBoundSceneInstances;
	std::vector<uint32_t> mCurrentVisibleChunkWords;
	std::vector<uint32_t> mCurrentVisibleFlatPlaneWords;
	uint32_t mLastResolvedLightOverlayGeneration = 0;
	uint32_t mFrameIndex = 0;
	uint64_t mFrameGenerationFrameId = 0;
	uint32_t mRenderWidth = 0;
	uint32_t mRenderHeight = 0;
	uint32_t mOutputWidth = 0;
	uint32_t mOutputHeight = 0;
	uint32_t mTargetWidth = 0;
	uint32_t mTargetHeight = 0;
	int32_t mSceneLeft = 0;
	int32_t mSceneTop = 0;
	nri::Format mOutputFormat = nri::Format::UNKNOWN;
	float mCurrentCameraPos[3] = {};
	float mCurrentCameraForward[3] = {};
	float mCurrentCameraRight[3] = {};
	float mCurrentCameraUp[3] = {};
	float mPreviousCameraPos[3] = {};
	float mPreviousCameraForward[3] = {};
	float mPreviousCameraRight[3] = {};
	float mPreviousCameraUp[3] = {};
	float mCurrentTanHalfFovX = 1.0f;
	float mCurrentTanHalfFovY = 1.0f;
	float mPreviousTanHalfFovX = 1.0f;
	float mPreviousTanHalfFovY = 1.0f;
	float mCurrentJitter[2] = {};
	float mPreviousJitter[2] = {};
	float mCurrentViewToClip[16] = {};
	float mPreviousViewToClip[16] = {};
	float mCurrentWorldToView[16] = {};
	float mPreviousWorldToView[16] = {};
	float mSkyColor[3] = { 0.38f, 0.48f, 0.65f };
	float mGroundColor[3] = { 0.08f, 0.08f, 0.08f };
	uint64_t mSkyTextureKey = 0;
	uint32_t mActiveSkyTextureIndex = UINT32_MAX;
	MapRecord* mSkyLevel = nullptr;
	SkyState mSkyState = {};
	SkyState mLastTracedSkyState = {};
	uint64_t mLastTracedSkyResolvedKey = 0;
	bool mHasTracedSkyState = false;
	PreservedStaticMapSkyState mPreservedStaticMapSky = {};
	bool mHasLoggedStats = false;
	bool mHasPreviousCameraState = false;
	bool mHasFrameGenerationRealFrameTime = false;
	bool mHasPendingFrameGenerationRealFrameTime = false;
	bool mHasFrameGenerationTimestamp = false;
	bool mHasFrameGenerationConfigState = false;
	bool mHasDirectionalLightState = false;
	bool mPathTracingSupported = true;
	bool mHasOutputPolicyState = false;
	bool mHasRuntimeLinkTraceState = false;
	SurfaceProbeFrameState mSurfaceProbeFrame = {};
	bool mResetHistory = true;
	std::string mLastHistoryResetReason = "startup";
	float mLastFrameGenerationRealFrameTimeMs = 0.0f;
	float mPendingFrameGenerationRealFrameTimeMs = 0.0f;
	std::chrono::steady_clock::time_point mLastFrameGenerationTimestamp = {};
	std::chrono::steady_clock::time_point mPendingFrameGenerationTimestamp = {};
	bool mLastFrameGenerationRequestedEnabled = false;
	NRIFrameGenerationProvider mLastFrameGenerationRequestedProvider = NRIFrameGenerationProvider::Off;
	NRIFrameGenerationUiMode mLastFrameGenerationResolvedUiMode = NRIFrameGenerationUiMode::Auto;
	bool mUseUpscaledInFinal = false;
	bool mLastTemporalAppTaaEnabled = false;
	bool mHasTemporalExposureState = false;
	bool mUseDenoisedCompositionInputs = false;
	bool mUseSplitShadowDenoiser = false;
	bool mHasLoggedFallback = false;
	bool mUsedStaticMapSceneLastFrame = false;
	bool mUsedDynamicSceneLastFrame = false;
	bool mHasVisibleMirrorPortalLastFrame = false;
	bool mGpuSceneHasDynamicOverlay = false;
	bool mUploadedStaticMapSceneLastFrame = false;
	bool mBuiltStaticMapSceneASLastFrame = false;
	bool mBuiltDynamicSceneASLastFrame = false;
	bool mPendingStaticMapLightingInvalidation = false;
	bool mAllowStartupMutationRebaseline = false;
	bool mPendingStartupMutationRebaseline = false;
	uint64_t mObservedMapWorldBuildSerial = 0;
	uint64_t mStartupMutationRebaselineDeadlineFrame = 0;
	uint64_t mStaticAccelerationBuildSerial = 0;
	uint32_t mActiveTlasInstanceCount = 0;
	uint32_t mBoundStaticPrimitiveCount = 0;
	uint32_t mBoundDynamicPrimitiveCount = 0;
	uint32_t mBoundStaticMaterialCount = 0;
	uint32_t mBoundDynamicMaterialCount = 0;
	uint32_t mBoundPortalCount = 0;
	uint32_t mBoundRuntimeLightCount = 0;
	uint32_t mBoundRuntimeLightTileCountX = 0;
	uint32_t mBoundRuntimeLightTileCountY = 0;
	uint32_t mBoundRuntimeLightTileSize = 0;
	uint32_t mBoundRuntimeLightTileIndexCount = 0;
	uint32_t mBoundRuntimeLightMaxTileOccupancy = 0;
	bool mSceneDataDescriptorsInitialized = false;
	bool mRuntimeLightPayloadCacheValid = false;
	uint64_t mRuntimeLightPayloadHash = 0;
	bool mRuntimeLightClusterCacheValid = false;
	uint64_t mRuntimeLightClusterPayloadHash = 0;
	uint64_t mRuntimeLightClusterCameraHash = 0;
	uint32_t mBoundEmissivePrimitiveCount = 0;
	uint32_t mBoundEmissiveDominantPrimitive = UINT32_MAX;
	uint32_t mBoundEmissiveDominantTile = 0;
	uint32_t mBoundEmissiveDominantFlags = 0;
	uint32_t mBoundEmissiveDominantDataSource = 0;
	bool mEmissiveSamplingPayloadCacheValid = false;
	uint64_t mEmissiveSamplingPayloadHash = 0;
	uint32_t mEmissiveTlasInstanceCount = 0;
	uint32_t mEmissiveTlasStaticInstanceCount = 0;
	uint32_t mEmissiveTlasDynamicInstanceCount = 0;
	uint32_t mEmissiveTlasBuildCount = 0;
	bool mEmissiveTlasInstancePayloadCacheValid = false;
	uint64_t mEmissiveTlasInstancePayloadHash = 0;
	float mBoundEmissiveTotalPower = 0.0f;
	float mBoundEmissiveDominantPower = 0.0f;
	std::vector<EmissivePrimitiveDebugRecord> mBoundEmissivePrimitiveRecords;
	bool mSectorLightingPayloadCacheValid = false;
	uint64_t mSectorLightingPayloadHash = 0;
	uint32_t mBoundSectorLightSectorCount = 0;
	uint32_t mBoundSectorLightActiveCount = 0;
	uint32_t mBoundSectorLightPulsingCount = 0;
	uint32_t mBoundSectorLightDominantSector = UINT32_MAX;
	float mBoundSectorLightDominantContribution = 0.0f;
	uint32_t mNextRuntimePointLightId = 1;
	std::vector<RuntimeDebugSphere> mRuntimeDebugSpheres;
	uint32_t mNextRuntimeDebugSphereId = 1;
	SurfaceProbeResult mLastSurfaceProbe = {};
	SurfaceProbeResult mLastLoggedSurfaceProbe = {};
	int mLastDebugMode = -1;
	int mLastMainUpscalerRequest = -1;
	int mLastPostSharpenRequest = -1;
	NRIPTOutputMode mLastOutputRequestedMode = NRIPTOutputMode::SDR;
	NRIPTOutputMode mLastOutputResolvedMode = NRIPTOutputMode::SDR;
	float mLastTemporalExposure = 1.0f;
	NRIMainUpscalerKind mLastMainUpscalerResolved = NRIMainUpscalerKind::Off;
	NRIPostSharpenKind mLastPostSharpenResolved = NRIPostSharpenKind::Off;
	NRIMainUpscalerKind mLastTemporalHistoryMainUpscaler = NRIMainUpscalerKind::Off;
	NRIPostSharpenKind mLastTemporalPostSharpen = NRIPostSharpenKind::Off;
	FrameTextureSlot mHistoryInputSlot = FrameTextureSlot::TaaHistoryPing;
	FrameTextureSlot mHistoryOutputSlot = FrameTextureSlot::TaaHistoryPong;
	FrameTextureSlot mUpscaledInputSlot = FrameTextureSlot::PostSharpenOutput;
};
