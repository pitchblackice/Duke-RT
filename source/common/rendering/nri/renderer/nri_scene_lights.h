#pragma once

#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"
#include "lightoverlay.h"
#include "v_video.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

enum class SceneLightRecordSource : uint32_t
{
	None = 0,
	CapturedScene,
	StaticMapScene,
	RuntimeMutationScene,
	DynamicScene,
	PersistentVoxelScene,
};

enum SceneAnalyticLightSourceFlags : uint32_t
{
	SceneAnalyticLightSourceFlag_None = 0,
	SceneAnalyticLightSourceFlag_Manual = 1u << 0,
	SceneAnalyticLightSourceFlag_SpriteTileHeuristic = 1u << 1,
	SceneAnalyticLightSourceFlag_ActorOverlay = 1u << 2,
	SceneAnalyticLightSourceFlag_MapOverlay = 1u << 3,
	SceneAnalyticLightSourceFlag_MuzzleFlash = 1u << 4,
};

enum SceneAnalyticLightFlags : uint32_t
{
	SceneAnalyticLightFlag_None = 0,
	SceneAnalyticLightFlag_CastsShadow = 1u << 0,
};

enum SceneEmissiveSurfaceSourceFlags : uint32_t
{
	SceneEmissiveSurfaceSourceFlag_None = 0,
	SceneEmissiveSurfaceSourceFlag_AutoFullbright = 1u << 0,
	SceneEmissiveSurfaceSourceFlag_AutoTextureGlow = 1u << 1,
	SceneEmissiveSurfaceSourceFlag_AutoGlowmap = 1u << 2,
	SceneEmissiveSurfaceSourceFlag_ExplicitTextureRule = 1u << 3,
	SceneEmissiveSurfaceSourceFlag_LightOverlayOverride = 1u << 4,
};

enum SceneSectorLightSourceFlags : uint32_t
{
	SceneSectorLightSourceFlag_None = 0,
	SceneSectorLightSourceFlag_Heuristic = 1u << 0,
	SceneSectorLightSourceFlag_PaletteFilter = 1u << 1,
	SceneSectorLightSourceFlag_LotagFilter = 1u << 2,
	SceneSectorLightSourceFlag_FogPresent = 1u << 3,
	SceneSectorLightSourceFlag_Pulsing = 1u << 4,
};

enum SceneLightDiagnosticFlags : uint32_t
{
	SceneLightDiagnosticFlag_None = 0,
	SceneLightDiagnosticFlag_PreviousMatch = 1u << 0,
	SceneLightDiagnosticFlag_PropertyChanged = 1u << 1,
	SceneLightDiagnosticFlag_Rebound = 1u << 2,
	SceneLightDiagnosticFlag_Added = 1u << 3,
};

struct NRIRuntimePointLightGpuData
{
	float position[3] = {};
	float radius = 0.0f;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	float intensity = 1.0f;
	uint32_t flags = 0;
	uint32_t reserved[3] = {};
};

struct NRISectorLightHeaderGpuData
{
	uint32_t sectorCount = 0;
	uint32_t activeCount = 0;
	uint32_t pulsingCount = 0;
	uint32_t flags = 0;
};

struct NRISectorLightGpuData
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

struct NRILightingSettings
{
	float emissiveMinPower = 0.0f;
	float emissiveMinSurface = 0.0f;
	float glowScale = 1.0f;
	float glowReach = 1.0f;
	float glowFalloff = 1.0f;
	bool sectorLighting = false;
	float sectorAmbientScale = 1.0f;
	float sectorHemisphereScale = 1.0f;
	float sectorFogScale = 1.0f;
	float sectorClamp = 1.0f;
	int sectorFilterPalette = -1;
	int sectorFilterMinShade = -128;
	int sectorFilterMaxShade = 127;
	int sectorFilterLotag = -1;
	int sectorPulseFrames = 0;
	float sectorPulseAmount = 0.0f;
	float sectorEmissionSignalStrength = 1.0f;
	float sectorEmissionResponseMin = 0.25f;
	float sectorEmissionResponseMax = 3.0f;
};

class SceneLightSystem
{
public:
	struct SurfaceIdentityOverrides
	{
		std::vector<uint64_t> opaqueWalls;
		std::vector<uint64_t> opaqueFlats;
		std::vector<uint64_t> opaqueSprites;

		void Clear()
		{
			opaqueWalls.clear();
			opaqueFlats.clear();
			opaqueSprites.clear();
		}
	};

	struct SceneAnalyticLight
	{
		uint32_t id = 0;
		uint64_t stableKey = 0;
		uint32_t sourceFlags = SceneAnalyticLightSourceFlag_None;
		uint32_t flags = SceneAnalyticLightFlag_CastsShadow;
		uint32_t sourceRuleId = 0;
		SceneLightRecordSource source = SceneLightRecordSource::None;
		int32_t actorIndex = -1;
		uint32_t textureId = 0;
		float position[3] = {};
		float color[3] = { 1.0f, 1.0f, 1.0f };
		float intensity = 1.0f;
		float radius = 0.0f;
	};

	struct AnalyticLightHeuristicRule
	{
		uint32_t ruleId = 0;
		uint32_t textureId = 0;
		float color[3] = { 1.0f, 1.0f, 1.0f };
		float intensity = 1.0f;
		float radius = 0.0f;
		uint32_t flickerFrames = 0;
	};

	struct AnalyticLightRegistry
	{
		struct ActorOverlayRule
		{
			uint32_t ruleId = 0;
			bool hasTileFilter = false;
			uint32_t tileFilter = 0;
			uint32_t flags = SceneAnalyticLightFlag_CastsShadow;
			float color[3] = { 1.0f, 1.0f, 1.0f };
			float intensity = 0.0f;
			float radius = 0.0f;
			float offset[3] = {};
			bool hasNudgeFromSurface = false;
			float nudgeFromSurfaceDistance = 0.0f;
			uint32_t flickerFrames = 0;
			bool hasRandomIntensity = false;
			float randomIntensityRange[2] = { 0.0f, 0.0f };
		};

		struct MapOverlayRule
		{
			uint32_t ruleId = 0;
			uint64_t stableKey = 0;
			SceneLightRecordSource source = SceneLightRecordSource::None;
			float position[3] = {};
			float color[3] = { 1.0f, 1.0f, 1.0f };
			float intensity = 0.0f;
			float radius = 0.0f;
			uint32_t flickerFrames = 0;
			bool hasSectorResponse = false;
			bool sectorResponse = true;
			bool hasSignalSector = false;
			int32_t signalSector = -1;
			bool hasResponseIntensity = false;
			float responseIntensity = 1.0f;
			bool hasResponseMin = false;
			float responseMin = 0.25f;
			bool hasResponseMax = false;
			float responseMax = 3.0f;
			bool hasResponseInputMin = false;
			float responseInputMin = 0.0f;
			bool hasResponseInputMax = false;
			float responseInputMax = 1.0f;
		};

		std::vector<SceneAnalyticLight> manualLights;
		std::vector<SceneAnalyticLight> transientLights;
		std::vector<AnalyticLightHeuristicRule> spriteTileRules;
		std::vector<SceneAnalyticLight> activeLights;
		std::vector<uint64_t> activeTopologyKeys;
		std::unordered_map<uint64_t, uint64_t> activePropertyHashes;
		std::unordered_map<uint64_t, uint64_t> activeBindingHashes;
		std::unordered_map<uint64_t, uint32_t> activeDiagnosticFlags;
		std::vector<uint64_t> addedTopologyKeys;
		std::vector<uint64_t> removedTopologyKeys;
		std::vector<uint64_t> reboundTopologyKeys;
		uint32_t matchedSurfaceCount = 0;
		uint32_t actorOverlayRuleCount = 0;
		uint32_t actorOverlayMatchedSurfaceCount = 0;
		uint32_t mapOverlayRuleCount = 0;
		uint32_t transientMuzzleSlotCount = 0;
		uint32_t transientMuzzleActiveCount = 0;
		uint32_t dedupedMatchCount = 0;
		uint32_t truncatedLightCount = 0;
		uint32_t nextRuleId = 1;
		bool topologyChanged = false;
		bool propertiesChanged = false;
		bool lastBuildTopologyChanged = false;
		bool lastBuildPropertiesChanged = false;
	};

	struct EmissiveSurfaceRegistry
	{
		struct EmissiveHeuristicRule
		{
			uint32_t ruleId = 0;
			uint32_t textureId = 0;
			uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_UseBaseTexture;
			float intensityScale = 1.0f;
			float emissiveColor[3] = { 1.0f, 1.0f, 1.0f };
			bool hasExplicitColor = false;
		};

		struct EmissiveSurfaceRecord
		{
			uint64_t stableKey = 0;
			uint32_t sourceFlags = SceneEmissiveSurfaceSourceFlag_None;
			uint32_t sourceRuleId = 0;
			uint32_t overrideRuleId = 0;
			SceneLightRecordSource source = SceneLightRecordSource::None;
			int32_t actorIndex = -1;
			int32_t sectorIndex = -1;
			int32_t authoredSectorIndex = -1;
			int32_t wallIndex = -1;
			uint32_t textureId = 0;
			uint32_t emissiveTextureIndex = UINT32_MAX;
			uint32_t materialIndex = UINT32_MAX;
			uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_None;
			float center[3] = {};
			float boundsRadius = 0.0f;
			float surfaceArea = 0.0f;
			float emissiveColor[3] = {};
			float emissiveIntensity = 0.0f;
			float reachScale = 1.0f;
			bool hasSectorResponseParams = false;
			float sectorResponseIntensity = 1.0f;
			float sectorResponseMin = 0.25f;
			float sectorResponseMax = 3.0f;
			bool hasSectorResponseInputRange = false;
			float sectorResponseInputMin = 0.0f;
			float sectorResponseInputMax = 1.0f;
			bool hasSectorResponseIntensityMin = false;
			float sectorResponseIntensityMin = 0.0f;
			bool hasSectorResponseIntensityMax = false;
			float sectorResponseIntensityMax = 4.0f;
			bool hasSectorResponseReachMin = false;
			float sectorResponseReachMin = 0.0f;
			bool hasSectorResponseReachMax = false;
			float sectorResponseReachMax = 24.0f;
			bool materialResponseEnabled = false;
			bool materialResponseExplicit = false;
			bool hasMaterialResponseParams = false;
			bool hasMaterialResponseMin = false;
			bool hasMaterialResponseMax = false;
			float materialResponseMin = 0.0f;
			float materialResponseMax = 1.0f;
			float powerEstimate = 0.0f;
			bool sectorResponseEnabled = true;
		};

		std::vector<EmissiveHeuristicRule> textureRules;
		std::vector<EmissiveSurfaceRecord> activeSurfaces;
		std::vector<uint64_t> activeTopologyKeys;
		std::unordered_map<uint64_t, uint64_t> activePropertyHashes;
		std::unordered_map<uint64_t, uint64_t> activeBindingHashes;
		std::unordered_map<uint64_t, uint32_t> activeDiagnosticFlags;
		std::vector<uint64_t> addedTopologyKeys;
		std::vector<uint64_t> removedTopologyKeys;
		std::vector<uint64_t> reboundTopologyKeys;
		float totalPowerEstimate = 0.0f;
		uint32_t autoTaggedCount = 0;
		uint32_t explicitRuleMatchCount = 0;
		uint32_t overrideRuleCount = 0;
		uint32_t overrideMatchedSurfaceCount = 0;
		uint32_t materialResponseRuleCount = 0;
		uint32_t materialResponseMatchedSurfaceCount = 0;
		uint32_t truncatedSurfaceCount = 0;
		uint32_t nextRuleId = 1;
		bool topologyChanged = false;
		bool propertiesChanged = false;
		bool materialBindingChanged = false;
		bool materialPropertiesChanged = false;
		bool lastBuildTopologyChanged = false;
		bool lastBuildPropertiesChanged = false;
	};

	struct EmissiveOverrideRule
	{
		uint32_t ruleId = 0;
		bool hasSectorFilter = false;
		int32_t sectorFilter = -1;
		bool hasWallFilter = false;
		int32_t wallFilter = -1;
		bool hasTileFilter = false;
		uint32_t tileFilter = 0;
		bool hasIntensityScale = false;
		float intensityScale = 1.0f;
		bool hasReachScale = false;
		float reachScale = 1.0f;
		bool hasSectorResponse = false;
		bool sectorResponse = true;
		bool hasSignalSector = false;
		int32_t signalSector = -1;
		bool hasResponseIntensity = false;
		float responseIntensity = 1.0f;
		bool hasResponseMin = false;
		float responseMin = 0.25f;
		bool hasResponseMax = false;
		float responseMax = 3.0f;
		bool hasResponseInputMin = false;
		float responseInputMin = 0.0f;
		bool hasResponseInputMax = false;
		float responseInputMax = 1.0f;
		bool hasResponseIntensityMin = false;
		float responseIntensityMin = 0.0f;
		bool hasResponseIntensityMax = false;
		float responseIntensityMax = 4.0f;
		bool hasResponseReachMin = false;
		float responseReachMin = 0.0f;
		bool hasResponseReachMax = false;
		float responseReachMax = 24.0f;
		bool hasMaterialResponse = false;
		bool materialResponse = false;
		bool hasMaterialResponseMin = false;
		float materialResponseMin = 0.0f;
		bool hasMaterialResponseMax = false;
		float materialResponseMax = 1.0f;
	};

	struct EmissiveMaterialResponseRule
	{
		uint32_t ruleId = 0;
		std::vector<uint32_t> textureIds;
		std::vector<std::pair<uint32_t, uint32_t>> textureRanges;
		std::vector<std::string> textureNames;
		bool hasMaterialResponse = false;
		bool materialResponse = true;
		bool hasMaterialResponseMin = false;
		float materialResponseMin = 0.0f;
		bool hasMaterialResponseMax = false;
		float materialResponseMax = 1.0f;
	};

	struct SectorLightingRegistry
	{
		struct SectorLightRecord
		{
			uint32_t sectorIndex = UINT32_MAX;
			uint32_t sourceFlags = SceneSectorLightSourceFlag_None;
			int32_t paletteIndex = -1;
			int32_t lotag = 0;
			int32_t hitag = 0;
			int32_t averageShade = 0;
			int32_t rawAverageShade = 0;
			float rawLightLevel = 0.0f;
			float rawFloorLight = 0.0f;
			float rawCeilingLight = 0.0f;
			float rawAmbientIntensity = 0.0f;
			float rawHemisphereAmount = 0.0f;
			float rawFogAmount = 0.0f;
			float rawResponseBrightness = 0.0f;
			float rawResponseSignal = 0.0f;
			float emitterResponseScale = 1.0f;
			float ambientColor[3] = {};
			float ambientIntensity = 0.0f;
			float hemisphereAmount = 0.0f;
			float fogAmount = 0.0f;
			float pulseScale = 1.0f;
		};

		std::vector<SectorLightRecord> sectors;
		std::vector<uint32_t> activeSectorIndices;
		std::vector<uint32_t> rawActiveSectorIndices;
		std::vector<uint32_t> activeTopologyKeys;
		uint32_t sectorCount = 0;
		uint32_t eligibleSectorCount = 0;
		uint32_t rawActiveSectorCount = 0;
		uint32_t rawNonNeutralSectorCount = 0;
		uint32_t responseBoostSectorCount = 0;
		uint32_t responseDimSectorCount = 0;
		uint32_t responseNeutralSectorCount = 0;
		uint32_t activeSectorCount = 0;
		uint32_t fogSectorCount = 0;
		uint32_t pulsingSectorCount = 0;
		bool topologyChanged = false;
	};

	struct EnvironmentLightingState
	{
	};

	struct SurfaceRecord
	{
		uint64_t identityKey = 0;
		SceneLightRecordSource source = SceneLightRecordSource::None;
		uint32_t materialIndex = UINT32_MAX;
		float center[3] = {};
		float boundsRadius = 0.0f;
		float surfaceArea = 0.0f;
		nri_scene::SurfaceProvenance provenance = {};
		nri_scene::MaterialLightingMetadata material = {};
	};

	struct FrameAppendStats
	{
		uint32_t totalRecordCount = 0;
		uint32_t staticRecordCount = 0;
		uint32_t runtimeMutationRecordCount = 0;
		uint32_t dynamicRecordCount = 0;
		uint32_t capturedRecordCount = 0;
		uint32_t persistentVoxelRecordCount = 0;
	};

	void Reset();
	void ResetLevelState();
	void BeginFrame(uint64_t frameSerial);
	void AppendSceneView(
		const nri_scene::SceneView& sceneView,
		const nri_scene::MaterialBridgeData& materials,
		SceneLightRecordSource source,
		uint32_t materialIndexBase = 0,
		uint32_t materialLookupIndexBase = 0,
		const SurfaceIdentityOverrides* identityOverrides = nullptr);
	void AppendSpriteSurfaces(
		const std::vector<nri_scene::SurfaceRef>& surfaces,
		const nri_scene::MaterialBridgeData& materials,
		SceneLightRecordSource source,
		uint32_t materialIndexBase = 0,
		uint32_t materialLookupIndexBase = 0,
		const std::vector<uint64_t>* identityOverrides = nullptr);
	void AppendSurfaceRecords(
		const std::vector<SurfaceRecord>& records,
		uint32_t materialIndexBase = 0);
	SurfaceRecord BuildSurfaceRecord(
		const nri_scene::SurfaceRef& surface,
		const nri_scene::MaterialBridgeData& materials,
		SceneLightRecordSource source,
		uint32_t materialIndex = 0,
		uint32_t materialLookupIndex = 0,
		uint64_t identityOverride = 0) const;
	void RebuildAnalyticLights(
		uint32_t flickerTimeIndex,
		uint32_t renderFrameIndex,
		uint32_t maxActiveLights,
		const std::unordered_map<int32_t, std::vector<AnalyticLightRegistry::ActorOverlayRule>>* actorOverlayRules = nullptr,
		const std::vector<AnalyticLightRegistry::MapOverlayRule>* mapOverlayRules = nullptr);
	void RebuildEmissiveSurfaces(
		uint32_t maxActiveSurfaces,
		const std::vector<EmissiveOverrideRule>* overrideRules = nullptr,
		const std::vector<EmissiveMaterialResponseRule>* materialResponseRules = nullptr);
	void RebuildSectorLighting(uint32_t frameIndex, uint32_t sectorCount);
	void BuildRuntimePointLightUpload(std::vector<NRIRuntimePointLightGpuData>& outLights) const;
	uint64_t BuildRuntimeLightPayloadHash() const;
	void BuildSectorLightingUpload(
		float sectorLightMultiplier,
		bool sectorLightingEnabled,
		NRISectorLightHeaderGpuData& outHeader,
		std::vector<NRISectorLightGpuData>& outSectors) const;
	uint64_t BuildSectorLightingPayloadHash(float sectorLightMultiplier, bool sectorLightingEnabled) const;

	bool AddRuntimePointLight(const float position[3], const float color[3], float intensity, float radius, uint32_t maxLights, uint32_t& outId);
	bool UpdateRuntimePointLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius);
	bool RemoveRuntimePointLight(uint32_t id);
	bool ClearRuntimePointLights();
	void ResetRuntimePointLights();
	void PrintRuntimePointLights(uint32_t maxLights) const;
	void RefreshResolvedMuzzleFlashRuleLookup(const ResolvedLightOverlaySet& resolvedLightOverlays);
	void ResetMuzzleFlashOverlayState(const char* reason, uint32_t discardedEventCount, bool debug);
	size_t GetResolvedMuzzleFlashRuleCount() const { return mResolvedMuzzleFlashRuleLookup.size(); }
	std::string FormatResolvedMuzzleFlashRuleIdList(size_t limit = 16) const;
	void RefreshTransientMuzzleFlashLights(double currentTimeSeconds, const TArray<PathTracingWeaponLightEvent>& pendingEvents, bool debug);
	bool IsEmissiveSurfaceSectorResponseEligible(const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface) const;
	bool IsEmissiveSurfaceMaterialResponseEligible(const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface) const;
	float ResolveSectorEmissionScale(const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface, bool& outApplied) const;
	float ResolveSectorEmissionIntensityScale(const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface, float scale) const;
	float ResolveSectorEmissionReachScale(const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface, float scale) const;
	float ResolveEmissiveMaterialResponseScale(const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface, bool& outApplied) const;
	uint64_t BuildEmissiveSectorResponsePayloadHash() const;
	void ResetEmissiveSectorResponseCaches();
	void NotifyEmissiveSectorResponseEditModeChanges(uint32_t frameIndex, const float currentCameraPos[3]);
	void TraceEmissiveSectorResponseChange(uint32_t frameIndex, const float currentCameraPos[3], bool traceEnabled);
	bool AddManualAnalyticLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius);
	bool UpdateManualAnalyticLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius);
	bool RemoveManualAnalyticLight(uint32_t id);
	void ClearManualAnalyticLights();
	uint32_t GetManualAnalyticLightCount() const { return (uint32_t)mAnalyticLights.manualLights.size(); }
	void SetTransientAnalyticLights(const std::vector<SceneAnalyticLight>& lights);

	bool AddSpriteTileHeuristic(uint32_t textureId, const float color[3], float intensity, float radius, uint32_t flickerFrames, uint32_t& outRuleId);
	bool ClearSpriteTileHeuristics();
	void PrintSpriteTileLightHeuristics() const;

	bool AddTextureEmissiveHeuristic(uint32_t textureId, uint32_t emissiveMode, float intensityScale, const float* emissiveColor, bool hasExplicitColor, uint32_t& outRuleId);
	bool ClearTextureEmissiveHeuristics();
	void PrintTextureEmissiveHeuristics() const;
	bool MaterialWouldEmit(const nri_scene::MaterialLightingMetadata& metadata) const;
	bool ApplyEmissiveMaterialSettings(const nri_scene::MaterialLightingMetadata& metadata, nri_scene::MaterialData& inOutMaterial) const;

	bool HasRecords() const { return !mSurfaceRecords.empty(); }
	uint64_t GetFrameSerial() const { return mFrameSerial; }
	const std::vector<SurfaceRecord>& GetSurfaceRecords() const { return mSurfaceRecords; }
	const FrameAppendStats& GetFrameAppendStats() const { return mFrameAppendStats; }
	static uint64_t ComputeSurfaceIdentityKey(
		SceneLightRecordSource source,
		const nri_scene::SurfaceProvenance& provenance,
		const float center[3]);

	const AnalyticLightRegistry& GetAnalyticLights() const { return mAnalyticLights; }
	const EmissiveSurfaceRegistry& GetEmissiveSurfaces() const { return mEmissiveSurfaces; }
	const SectorLightingRegistry& GetSectorLighting() const { return mSectorLighting; }
	const EnvironmentLightingState& GetEnvironmentLighting() const { return mEnvironmentLighting; }

	bool ConsumeAnalyticLightTopologyChanged();
	bool ConsumeAnalyticLightPropertiesChanged();
	bool ConsumeEmissiveSurfaceTopologyChanged();
	bool ConsumeEmissiveSurfacePropertiesChanged();
	bool ConsumeEmissiveMaterialBindingChanged();
	bool ConsumeEmissiveMaterialPropertiesChanged();
	bool ConsumeSectorLightingTopologyChanged();

private:
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

	static NRILightingSettings CaptureSettings();
	void AppendSurfaceRecord(SurfaceRecord record, uint32_t materialIndexBase);
	void AppendSurfaceList(
		const std::vector<nri_scene::SurfaceRef>& surfaces,
		const nri_scene::MaterialBridgeData& materials,
		SceneLightRecordSource source,
		uint32_t materialIndexBase,
		uint32_t materialLookupIndexBase,
		uint32_t& inOutLocalMaterialIndex,
		const std::vector<uint64_t>* identityOverrides);

	AnalyticLightRegistry mAnalyticLights = {};
	EmissiveSurfaceRegistry mEmissiveSurfaces = {};
	SectorLightingRegistry mSectorLighting = {};
	EnvironmentLightingState mEnvironmentLighting = {};
	std::vector<SurfaceRecord> mSurfaceRecords;
	FrameAppendStats mFrameAppendStats = {};
	uint64_t mFrameSerial = 0;
	uint32_t mNextRuntimePointLightId = 1;
	std::unordered_map<std::string, ResolvedLightOverlayMuzzleFlashRule> mResolvedMuzzleFlashRuleLookup;
	std::vector<TransientMuzzleFlashSlot> mTransientMuzzleFlashSlots;
	std::vector<SceneAnalyticLight> mTransientMuzzleFlashLights;
	bool mEmissiveSectorResponseTraceCacheValid = false;
	uint64_t mEmissiveSectorResponseTraceHash = 0;
	bool mEmissiveSectorResponseNotifyCacheValid = false;
	uint32_t mLastEmissiveSectorResponseNotifyFrame = 0;
	std::vector<float> mEmissiveSectorResponseNotifyScales;
	bool mSectorLightingEditNotifyCacheValid = false;
	uint32_t mLastSectorLightingEditNotifyFrame = 0;
	std::vector<uint64_t> mSectorLightingEditNotifyHashes;
};
