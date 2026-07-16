#pragma once

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"

struct ResolvedLightOverlaySet;

struct NRISurfaceLightOverlayCacheStats
{
	uint64_t key = 0;
	bool hit = false;
	bool candidateHit = false;
	bool built = false;
	bool rejected = false;
	bool validationChecked = false;
	bool validationMismatch = false;
};

class NRISurfaceLightOverlayCache
{
public:
	bool Build(
		const ResolvedLightOverlaySet& resolved,
		bool allowReuse,
		bool validateReuse,
		nri_scene::SceneView& outSceneView,
		nri_scene::GeometryData& outGeometry,
		nri_scene::MaterialBridgeData& outMaterials,
		NRISurfaceLightOverlayCacheStats& outStats);
	void Reset();

private:
	nri_scene::SceneView mSceneView;
	nri_scene::GeometryData mGeometry;
	nri_scene::MaterialBridgeData mMaterials;
	uint64_t mKey = 0;
	bool mValid = false;
	bool mBuilt = false;
};

namespace nri_surface_light_overlay
{
bool BuildSurfaceLightOverlay(
	const ResolvedLightOverlaySet& resolved,
	nri_scene::SceneView& outSceneView,
	nri_scene::GeometryData& outGeometry,
	nri_scene::MaterialBridgeData& outMaterials);
}
