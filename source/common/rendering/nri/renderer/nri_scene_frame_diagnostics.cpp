#include "nri_scene_frame_diagnostics.h"

NRISceneFrameDebugStatsInputs MakeNRISceneFrameDebugStatsInputs(const NRISceneFrameDebugStatsBuildRequest& request)
{
	NRISceneFrameDebugStatsInputs inputs = {};
	inputs.staticMapStats = request.staticMapStats;
	inputs.deferredDynamicSceneView = !request.deferOverlayThisFrame ? request.deferredDynamicSceneView : nullptr;
	inputs.activeDynamicSceneView = request.activeDynamicSceneView;
	inputs.persistentVoxelStats = request.persistentVoxelStats;
	inputs.localPlayerReflectionSceneView = request.hasLocalPlayerReflectionScene ? request.localPlayerReflectionSceneView : nullptr;
	inputs.totalMs = request.totalMs;
	inputs.baseMs = request.baseMs;
	inputs.persistentVoxelMs = request.persistentVoxelMs;
	inputs.localPlayerReflectionMs = request.localPlayerReflectionMs;
	inputs.mergeMs = request.mergeMs;
	return inputs;
}

NRISceneSurfaceProbeFrameInputs MakeNRISceneSurfaceProbeFrameInputs(const NRISceneSurfaceProbeFrameBuildRequest& request)
{
	NRISceneSurfaceProbeFrameInputs inputs = {};
	inputs.usesStaticMapScene = request.usesStaticMapScene;
	inputs.activeStaticProbePrimitiveCount = request.activeStaticProbePrimitiveCount;
	inputs.runtimeSpaceLinkGeometry = request.runtimeSpaceLinkGeometry;
	inputs.runtimeMutationGeometry = request.runtimeMutationGeometry;
	inputs.overlayGeometry = request.overlayGeometry;
	inputs.activeDynamicGeometry = request.activeDynamicGeometry;
	return inputs;
}
