#pragma once

#include "nri_scene_frame_builder.h"

struct NRISceneFramePathSelectionInputs
{
	bool allowStaticMapScene = false;
	bool staticMapSceneReady = false;
};

struct NRISceneFramePathSelectionResult
{
	bool hasStaticMapScene = false;
	bool useCapturedFallback = false;
};

struct NRISceneFrameOverlayDeferralInputs
{
	bool uploadedStaticMapSceneLastFrame = false;
	bool builtStaticMapSceneASLastFrame = false;
};

struct NRISceneFrameOverlayDeferralResult
{
	bool deferOverlayThisFrame = false;
};

struct NRISceneFrameOverlayEligibilityInputs
{
	bool deferOverlayThisFrame = false;

	bool runtimeSpaceLinkBuilt = false;
	bool runtimeMutationBuilt = false;

	bool hasPersistentVoxelBatch = false;
	bool persistentVoxelRenderable = false;

	const nri_scene::GeometryData* activeDynamicGeometry = nullptr;
	const nri_scene::MaterialBridgeData* activeDynamicMaterials = nullptr;

	bool hasLocalPlayerReflectionScene = false;
	const nri_scene::GeometryData* localPlayerReflectionGeometry = nullptr;
	const nri_scene::MaterialBridgeData* localPlayerReflectionMaterials = nullptr;

	bool runtimeDebugSphereBuilt = false;
	bool surfaceLightBuilt = false;
};

struct NRISceneFrameOverlayEligibilityResult
{
	bool hasRuntimeSpaceLinkOverlay = false;
	bool hasRuntimeMutationOverlay = false;
	bool hasPersistentVoxelOverlay = false;
	bool hasActiveDynamicOverlay = false;
	bool hasLocalPlayerReflectionOverlay = false;
	bool hasRuntimeDebugSphereOverlay = false;
	bool hasSurfaceLightOverlay = false;
	bool hasAnyOverlay = false;
};

NRISceneFramePathSelectionResult SelectNRISceneFramePath(const NRISceneFramePathSelectionInputs& inputs);
NRISceneFrameOverlayDeferralResult SelectNRISceneFrameOverlayDeferral(const NRISceneFrameOverlayDeferralInputs& inputs);
NRISceneFrameOverlayEligibilityResult SelectNRISceneFrameOverlayEligibility(const NRISceneFrameOverlayEligibilityInputs& inputs);
