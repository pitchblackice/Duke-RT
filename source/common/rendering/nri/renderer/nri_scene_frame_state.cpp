#include "nri_scene_frame_state.h"

NRISceneFrameDynamicStateInputs MakeNRISceneFrameDynamicStateInputs(const NRISceneFrameDynamicStateBuildRequest& request)
{
	NRISceneFrameDynamicStateInputs inputs = {};
	inputs.activeDynamicSceneView = request.activeDynamicSceneView;
	inputs.activeDynamicGeometry = request.activeDynamicGeometry;
	inputs.activeDynamicMaterials = request.activeDynamicMaterials;
	inputs.localPlayerReflectionSceneView = request.hasLocalPlayerReflectionScene ? request.localPlayerReflectionSceneView : nullptr;
	inputs.localPlayerReflectionGeometry = request.hasLocalPlayerReflectionScene ? request.localPlayerReflectionGeometry : nullptr;
	inputs.localPlayerReflectionMaterials = request.hasLocalPlayerReflectionScene ? request.localPlayerReflectionMaterials : nullptr;
	inputs.totalMs = request.totalMs;
	inputs.dynamicCoreMs = request.dynamicCoreMs;
	inputs.localPlayerReflectionMs = request.localPlayerReflectionMs;
	return inputs;
}

NRISceneFrameGenerationInputs MakeNRISceneFrameGenerationInputs(const NRISceneFrameGenerationBuildRequest& request)
{
	NRISceneFrameGenerationInputs inputs = {};
	inputs.staticMapBuildSerial = request.staticMapBuildSerial;
	inputs.runtimeMutationGeneration = request.runtimeMutationGeneration;
	inputs.persistentVoxelGeneration = request.persistentVoxelGeneration;
	inputs.frameIndex = request.frameIndex;
	inputs.staticAccelerationBuildSerial = request.staticAccelerationBuildSerial;
	inputs.renderWidth = request.renderWidth;
	inputs.renderHeight = request.renderHeight;
	inputs.currentCameraPos = request.currentCameraPos;
	inputs.currentCameraForward = request.currentCameraForward;
	inputs.currentCameraRight = request.currentCameraRight;
	inputs.currentCameraUp = request.currentCameraUp;
	inputs.currentTanHalfFovX = request.currentTanHalfFovX;
	inputs.currentTanHalfFovY = request.currentTanHalfFovY;
	inputs.selectedSceneHasDynamicOverlay = request.selectedSceneHasDynamicOverlay;
	inputs.activeDynamicSceneView = request.activeDynamicSceneView;
	inputs.activeDynamicGeometry = request.activeDynamicGeometry;
	inputs.activeDynamicMaterials = request.activeDynamicMaterials;
	inputs.hasLocalPlayerReflectionScene = request.hasLocalPlayerReflectionScene;
	inputs.localPlayerReflectionGeometry = request.localPlayerReflectionGeometry;
	inputs.localPlayerReflectionMaterials = request.localPlayerReflectionMaterials;
	inputs.activeMaterialBridge = request.activeMaterialBridge;
	inputs.activeGpuMaterials = request.activeGpuMaterials;
	inputs.sceneTextureCacheCount = request.sceneTextureCacheCount;
	inputs.selectedTlasInstanceCount = request.selectedTlasInstanceCount;
	inputs.selectedSceneInstanceCount = request.selectedSceneInstanceCount;
	inputs.selectedStaticSceneInstanceCount = request.selectedStaticSceneInstanceCount;
	inputs.selectedDynamicSceneInstanceCount = request.selectedDynamicSceneInstanceCount;
	inputs.selectedPersistentVoxelSceneInstanceCount = request.selectedPersistentVoxelSceneInstanceCount;
	return inputs;
}
