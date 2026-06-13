#pragma once

#include "../scene/nri_material_bridge.h"
#include "../scene/nri_geometry_bridge.h"

#include <cstdint>
#include <vector>

struct NRIDebugOverlayBuildTelemetry
{
	double runtimeDebugSphereViewMs = 0.0;
	double runtimeDebugSphereGeoMs = 0.0;
	double runtimeDebugSphereMaterialMs = 0.0;
	double geometryBuildDebugSphereMs = 0.0;
	uint32_t runtimeDebugSphereCount = 0;
	uint32_t runtimeDebugSphereLongitudeSegments = 0;
	uint32_t runtimeDebugSphereLatitudeSegments = 0;
	uint32_t runtimeDebugSpherePrimitiveCount = 0;
	uint32_t runtimeDebugSphereMaterialCount = 0;
};

class NRIDebugOverlaySystem
{
public:
	bool AddRuntimeDebugSphere(const float center[3], float diameter, float metalness, float roughness, uint32_t& outId);
	bool RemoveRuntimeDebugSphere(uint32_t id);
	bool ClearRuntimeDebugSpheres();
	void PrintRuntimeDebugSpheres() const;
	uint32_t GetRuntimeDebugSphereCount() const { return (uint32_t)mRuntimeDebugSpheres.size(); }
	bool Empty() const { return mRuntimeDebugSpheres.empty(); }
	void ResetRuntimeDebugSphereIds() { mNextRuntimeDebugSphereId = 1; }
	void InvalidateRuntimeDebugSphereTessellation();

	bool BuildRuntimeDebugSphereOverlay(
		nri_scene::GeometryData& outGeometry,
		nri_scene::MaterialBridgeData& outMaterials,
		NRIDebugOverlayBuildTelemetry& outTelemetry,
		bool collectTiming);

private:
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

	bool EnsureRuntimeDebugSphereCache(RuntimeDebugSphere& sphere, bool collectTiming, NRIDebugOverlayBuildTelemetry& telemetry);
	void AppendRuntimeDebugSphereToSceneView(const RuntimeDebugSphere& sphere, nri_scene::SceneView& sceneView) const;

	std::vector<RuntimeDebugSphere> mRuntimeDebugSpheres;
	uint32_t mNextRuntimeDebugSphereId = 1;
};
