#include "nri_scene_frame_selection.h"

namespace
{
	bool HasPrimitives(const nri_scene::GeometryData* geometry)
	{
		return geometry != nullptr && !geometry->primitives.empty();
	}

	bool HasMaterials(const nri_scene::MaterialBridgeData* materials)
	{
		return materials != nullptr && !materials->materials.empty();
	}
}

NRISceneFramePathSelectionResult SelectNRISceneFramePath(const NRISceneFramePathSelectionInputs& inputs)
{
	NRISceneFramePathSelectionResult result = {};
	result.hasStaticMapScene = inputs.allowStaticMapScene && inputs.staticMapSceneReady;
	result.useCapturedFallback = !result.hasStaticMapScene;
	return result;
}

NRISceneFrameOverlayDeferralResult SelectNRISceneFrameOverlayDeferral(const NRISceneFrameOverlayDeferralInputs& inputs)
{
	NRISceneFrameOverlayDeferralResult result = {};
	result.deferOverlayThisFrame = inputs.uploadedStaticMapSceneLastFrame || inputs.builtStaticMapSceneASLastFrame;
	return result;
}

NRISceneFrameOverlayEligibilityResult SelectNRISceneFrameOverlayEligibility(const NRISceneFrameOverlayEligibilityInputs& inputs)
{
	NRISceneFrameOverlayEligibilityResult result = {};
	result.hasRuntimeSpaceLinkOverlay = inputs.runtimeSpaceLinkBuilt;
	result.hasRuntimeMutationOverlay = inputs.runtimeMutationBuilt;
	result.hasPersistentVoxelOverlay = inputs.hasPersistentVoxelBatch && inputs.persistentVoxelRenderable;
	result.hasActiveDynamicOverlay =
		HasPrimitives(inputs.activeDynamicGeometry) &&
		inputs.activeDynamicMaterials != nullptr;
	result.hasMirrorExtendedDynamicOverlay =
		inputs.hasMirrorExtendedDynamicScene &&
		HasPrimitives(inputs.mirrorExtendedGeometry) &&
		HasMaterials(inputs.mirrorExtendedMaterials);
	result.hasMirrorPlayerOverlay =
		inputs.hasMirrorPlayerScene &&
		HasPrimitives(inputs.mirrorPlayerGeometry) &&
		HasMaterials(inputs.mirrorPlayerMaterials);
	result.hasRuntimeDebugSphereOverlay = inputs.runtimeDebugSphereBuilt;
	result.hasSurfaceLightOverlay = inputs.surfaceLightBuilt;
	result.hasAnyOverlay =
		result.hasRuntimeSpaceLinkOverlay ||
		result.hasRuntimeMutationOverlay ||
		result.hasPersistentVoxelOverlay ||
		result.hasActiveDynamicOverlay ||
		result.hasMirrorExtendedDynamicOverlay ||
		result.hasMirrorPlayerOverlay ||
		result.hasRuntimeDebugSphereOverlay ||
		result.hasSurfaceLightOverlay;
	return result;
}
