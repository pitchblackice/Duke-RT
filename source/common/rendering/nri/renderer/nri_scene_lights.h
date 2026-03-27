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

class SceneLightSystem
{
public:
	struct AnalyticLightRegistry
	{
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

	bool HasRecords() const { return !mSurfaceRecords.empty(); }
	uint64_t GetFrameSerial() const { return mFrameSerial; }
	const std::vector<SurfaceRecord>& GetSurfaceRecords() const { return mSurfaceRecords; }

	const AnalyticLightRegistry& GetAnalyticLights() const { return mAnalyticLights; }
	const EmissiveSurfaceRegistry& GetEmissiveSurfaces() const { return mEmissiveSurfaces; }
	const SectorLightingRegistry& GetSectorLighting() const { return mSectorLighting; }
	const EnvironmentLightingState& GetEnvironmentLighting() const { return mEnvironmentLighting; }

private:
	void AppendSurfaceList(const std::vector<nri_scene::SurfaceRef>& surfaces, const nri_scene::MaterialBridgeData& materials, SceneLightRecordSource source, uint32_t& inOutMaterialIndex);

	AnalyticLightRegistry mAnalyticLights = {};
	EmissiveSurfaceRegistry mEmissiveSurfaces = {};
	SectorLightingRegistry mSectorLighting = {};
	EnvironmentLightingState mEnvironmentLighting = {};
	std::vector<SurfaceRecord> mSurfaceRecords;
	uint64_t mFrameSerial = 0;
};
