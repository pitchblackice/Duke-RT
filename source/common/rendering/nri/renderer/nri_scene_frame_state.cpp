#include "nri_scene_frame_state.h"

NRISceneFrameDynamicStateInputs MakeNRISceneFrameDynamicStateInputs(const NRISceneFrameDynamicStateBuildRequest& request)
{
	NRISceneFrameDynamicStateInputs inputs = {};
	inputs.activeDynamicSceneView = request.activeDynamicSceneView;
	inputs.activeDynamicGeometry = request.activeDynamicGeometry;
	inputs.activeDynamicMaterials = request.activeDynamicMaterials;
	inputs.mirrorExtendedSceneView = request.hasMirrorExtendedDynamicScene ? request.mirrorExtendedSceneView : nullptr;
	inputs.mirrorExtendedGeometry = request.hasMirrorExtendedDynamicScene ? request.mirrorExtendedGeometry : nullptr;
	inputs.mirrorExtendedMaterials = request.hasMirrorExtendedDynamicScene ? request.mirrorExtendedMaterials : nullptr;
	inputs.mirrorPlayerSceneView = request.hasMirrorPlayerScene ? request.mirrorPlayerSceneView : nullptr;
	inputs.mirrorPlayerGeometry = request.hasMirrorPlayerScene ? request.mirrorPlayerGeometry : nullptr;
	inputs.mirrorPlayerMaterials = request.hasMirrorPlayerScene ? request.mirrorPlayerMaterials : nullptr;
	inputs.totalMs = request.totalMs;
	inputs.dynamicCoreMs = request.dynamicCoreMs;
	inputs.mirrorExtendedMs = request.mirrorExtendedMs;
	inputs.mirrorPlayerMs = request.mirrorPlayerMs;
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
	inputs.hasMirrorPlayerScene = request.hasMirrorPlayerScene;
	inputs.mirrorPlayerGeometry = request.mirrorPlayerGeometry;
	inputs.mirrorPlayerMaterials = request.mirrorPlayerMaterials;
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
