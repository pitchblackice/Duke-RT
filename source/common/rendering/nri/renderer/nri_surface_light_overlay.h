#pragma once

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"

struct ResolvedLightOverlaySet;

namespace nri_surface_light_overlay
{
bool BuildSurfaceLightOverlay(
	const ResolvedLightOverlaySet& resolved,
	nri_scene::SceneView& outSceneView,
	nri_scene::GeometryData& outGeometry,
	nri_scene::MaterialBridgeData& outMaterials);
}
