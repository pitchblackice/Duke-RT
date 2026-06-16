#pragma once

#include "nri_scene_frame_builder.h"

struct NRISceneFrameDebugStatsBuildRequest
{
	const nri_scene::SceneDebugStats* staticMapStats = nullptr;
	bool deferOverlayThisFrame = false;
	const nri_scene::SceneView* deferredDynamicSceneView = nullptr;
	const nri_scene::SceneView* activeDynamicSceneView = nullptr;
	const nri_scene::SceneDebugStats* persistentVoxelStats = nullptr;
	bool hasMirrorExtendedDynamicScene = false;
	const nri_scene::SceneView* mirrorExtendedSceneView = nullptr;
	bool hasMirrorPlayerScene = false;
	const nri_scene::SceneView* mirrorPlayerSceneView = nullptr;
	double* totalMs = nullptr;
	double* baseMs = nullptr;
	double* persistentVoxelMs = nullptr;
	double* mirrorExtendedMs = nullptr;
	double* mirrorPlayerMs = nullptr;
	double* mergeMs = nullptr;
};

struct NRISceneSurfaceProbeFrameBuildRequest
{
	bool usesStaticMapScene = false;
	uint32_t activeStaticProbePrimitiveCount = 0;
	const nri_scene::GeometryData* runtimeSpaceLinkGeometry = nullptr;
	const nri_scene::GeometryData* runtimeMutationGeometry = nullptr;
	const nri_scene::GeometryData* overlayGeometry = nullptr;
	const nri_scene::GeometryData* activeDynamicGeometry = nullptr;
};

NRISceneFrameDebugStatsInputs MakeNRISceneFrameDebugStatsInputs(const NRISceneFrameDebugStatsBuildRequest& request);
NRISceneSurfaceProbeFrameInputs MakeNRISceneSurfaceProbeFrameInputs(const NRISceneSurfaceProbeFrameBuildRequest& request);
