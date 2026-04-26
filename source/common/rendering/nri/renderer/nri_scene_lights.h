#pragma once

#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"

#include <cstdint>
#include <unordered_map>
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

enum SceneEmissiveSurfaceSourceFlags : uint32_t
{
	SceneEmissiveSurfaceSourceFlag_None = 0,
	SceneEmissiveSurfaceSourceFlag_AutoFullbright = 1u << 0,
	SceneEmissiveSurfaceSourceFlag_AutoTextureGlow = 1u << 1,
	SceneEmissiveSurfaceSourceFlag_AutoGlowmap = 1u << 2,
	SceneEmissiveSurfaceSourceFlag_ExplicitTextureRule = 1u << 3,
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
			SceneLightRecordSource source = SceneLightRecordSource::None;
			int32_t actorIndex = -1;
			uint32_t textureId = 0;
			uint32_t emissiveTextureIndex = UINT32_MAX;
			uint32_t materialIndex = UINT32_MAX;
			uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_None;
			float center[3] = {};
			float boundsRadius = 0.0f;
			float surfaceArea = 0.0f;
			float emissiveColor[3] = {};
			float emissiveIntensity = 0.0f;
			float powerEstimate = 0.0f;
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
		uint32_t truncatedSurfaceCount = 0;
		uint32_t nextRuleId = 1;
		bool topologyChanged = false;
		bool propertiesChanged = false;
		bool materialBindingChanged = false;
		bool materialPropertiesChanged = false;
		bool lastBuildTopologyChanged = false;
		bool lastBuildPropertiesChanged = false;
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
			float ambientColor[3] = {};
			float ambientIntensity = 0.0f;
			float hemisphereAmount = 0.0f;
			float fogAmount = 0.0f;
			float pulseScale = 1.0f;
		};

		std::vector<SectorLightRecord> sectors;
		std::vector<uint32_t> activeSectorIndices;
		std::vector<uint32_t> activeTopologyKeys;
		uint32_t sectorCount = 0;
		uint32_t eligibleSectorCount = 0;
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
	void RebuildAnalyticLights(
		uint32_t flickerTimeIndex,
		uint32_t renderFrameIndex,
		uint32_t maxActiveLights,
		const std::unordered_map<int32_t, std::vector<AnalyticLightRegistry::ActorOverlayRule>>* actorOverlayRules = nullptr,
		const std::vector<AnalyticLightRegistry::MapOverlayRule>* mapOverlayRules = nullptr);
	void RebuildEmissiveSurfaces(uint32_t maxActiveSurfaces);
	void RebuildSectorLighting(uint32_t frameIndex, uint32_t sectorCount);

	bool AddManualAnalyticLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius);
	bool RemoveManualAnalyticLight(uint32_t id);
	void ClearManualAnalyticLights();
	uint32_t GetManualAnalyticLightCount() const { return (uint32_t)mAnalyticLights.manualLights.size(); }
	void SetTransientAnalyticLights(const std::vector<SceneAnalyticLight>& lights);

	bool AddSpriteTileHeuristic(uint32_t textureId, const float color[3], float intensity, float radius, uint32_t flickerFrames, uint32_t& outRuleId);
	void ClearSpriteTileHeuristics();

	bool AddTextureEmissiveHeuristic(uint32_t textureId, uint32_t emissiveMode, float intensityScale, const float* emissiveColor, bool hasExplicitColor, uint32_t& outRuleId);
	void ClearTextureEmissiveHeuristics();
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
};
