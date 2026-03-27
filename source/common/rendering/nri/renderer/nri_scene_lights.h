#pragma once

#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"

#include <cstdint>
#include <vector>

enum class SceneLightRecordSource : uint32_t
{
	None = 0,
	CapturedScene,
	StaticMapScene,
	DynamicScene,
};

enum SceneAnalyticLightSourceFlags : uint32_t
{
	SceneAnalyticLightSourceFlag_None = 0,
	SceneAnalyticLightSourceFlag_Manual = 1u << 0,
	SceneAnalyticLightSourceFlag_SpriteTileHeuristic = 1u << 1,
};

class SceneLightSystem
{
public:
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
		std::vector<SceneAnalyticLight> manualLights;
		std::vector<AnalyticLightHeuristicRule> spriteTileRules;
		std::vector<SceneAnalyticLight> activeLights;
		std::vector<uint64_t> activeTopologyKeys;
		uint32_t matchedSurfaceCount = 0;
		uint32_t dedupedMatchCount = 0;
		uint32_t truncatedLightCount = 0;
		uint32_t nextRuleId = 1;
		bool topologyChanged = false;
	};

	struct EmissiveSurfaceRegistry
	{
	};

	struct SectorLightingRegistry
	{
	};

	struct EnvironmentLightingState
	{
	};

	struct SurfaceRecord
	{
		SceneLightRecordSource source = SceneLightRecordSource::None;
		uint32_t materialIndex = UINT32_MAX;
		float center[3] = {};
		float boundsRadius = 0.0f;
		nri_scene::SurfaceProvenance provenance = {};
		nri_scene::MaterialLightingMetadata material = {};
	};

	void Reset();
	void BeginFrame(uint64_t frameSerial);
	void AppendSceneView(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials, SceneLightRecordSource source);
	void RebuildAnalyticLights(uint32_t frameIndex, uint32_t maxActiveLights);

	bool AddManualAnalyticLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius);
	bool RemoveManualAnalyticLight(uint32_t id);
	void ClearManualAnalyticLights();
	uint32_t GetManualAnalyticLightCount() const { return (uint32_t)mAnalyticLights.manualLights.size(); }

	bool AddSpriteTileHeuristic(uint32_t textureId, const float color[3], float intensity, float radius, uint32_t flickerFrames, uint32_t& outRuleId);
	void ClearSpriteTileHeuristics();

	bool HasRecords() const { return !mSurfaceRecords.empty(); }
	uint64_t GetFrameSerial() const { return mFrameSerial; }
	const std::vector<SurfaceRecord>& GetSurfaceRecords() const { return mSurfaceRecords; }

	const AnalyticLightRegistry& GetAnalyticLights() const { return mAnalyticLights; }
	const EmissiveSurfaceRegistry& GetEmissiveSurfaces() const { return mEmissiveSurfaces; }
	const SectorLightingRegistry& GetSectorLighting() const { return mSectorLighting; }
	const EnvironmentLightingState& GetEnvironmentLighting() const { return mEnvironmentLighting; }

	bool ConsumeAnalyticLightTopologyChanged();

private:
	void AppendSurfaceList(const std::vector<nri_scene::SurfaceRef>& surfaces, const nri_scene::MaterialBridgeData& materials, SceneLightRecordSource source, uint32_t& inOutMaterialIndex);

	AnalyticLightRegistry mAnalyticLights = {};
	EmissiveSurfaceRegistry mEmissiveSurfaces = {};
	SectorLightingRegistry mSectorLighting = {};
	EnvironmentLightingState mEnvironmentLighting = {};
	std::vector<SurfaceRecord> mSurfaceRecords;
	uint64_t mFrameSerial = 0;
};
