#pragma once

#include "nri_scene_frame_builder.h"

struct NRISceneFrameDynamicStateBuildRequest
{
	const nri_scene::SceneView* activeDynamicSceneView = nullptr;
	const nri_scene::GeometryData* activeDynamicGeometry = nullptr;
	const nri_scene::MaterialBridgeData* activeDynamicMaterials = nullptr;
	bool hasMirrorExtendedDynamicScene = false;
	const nri_scene::SceneView* mirrorExtendedSceneView = nullptr;
	const nri_scene::GeometryData* mirrorExtendedGeometry = nullptr;
	const nri_scene::MaterialBridgeData* mirrorExtendedMaterials = nullptr;
	bool hasMirrorPlayerScene = false;
	const nri_scene::SceneView* mirrorPlayerSceneView = nullptr;
	const nri_scene::GeometryData* mirrorPlayerGeometry = nullptr;
	const nri_scene::MaterialBridgeData* mirrorPlayerMaterials = nullptr;
	double* totalMs = nullptr;
	double* dynamicCoreMs = nullptr;
	double* mirrorExtendedMs = nullptr;
	double* mirrorPlayerMs = nullptr;
};

struct NRISceneFrameGenerationBuildRequest
{
	uint64_t staticMapBuildSerial = 0;
	uint64_t runtimeMutationGeneration = 0;
	uint64_t persistentVoxelGeneration = 0;
	uint64_t frameIndex = 0;
	uint64_t staticAccelerationBuildSerial = 0;
	uint32_t renderWidth = 0;
	uint32_t renderHeight = 0;
	const float* currentCameraPos = nullptr;
	const float* currentCameraForward = nullptr;
	const float* currentCameraRight = nullptr;
	const float* currentCameraUp = nullptr;
	float currentTanHalfFovX = 0.0f;
	float currentTanHalfFovY = 0.0f;
	bool selectedSceneHasDynamicOverlay = false;
	const nri_scene::SceneView* activeDynamicSceneView = nullptr;
	const nri_scene::GeometryData* activeDynamicGeometry = nullptr;
	const nri_scene::MaterialBridgeData* activeDynamicMaterials = nullptr;
	bool hasMirrorPlayerScene = false;
	const nri_scene::GeometryData* mirrorPlayerGeometry = nullptr;
	const nri_scene::MaterialBridgeData* mirrorPlayerMaterials = nullptr;
	const nri_scene::MaterialBridgeData* activeMaterialBridge = nullptr;
	const std::vector<nri_scene::MaterialData>* activeGpuMaterials = nullptr;
	uint32_t sceneTextureCacheCount = 0;
	uint32_t selectedTlasInstanceCount = 0;
	uint32_t selectedSceneInstanceCount = 0;
	uint32_t selectedStaticSceneInstanceCount = 0;
	uint32_t selectedDynamicSceneInstanceCount = 0;
	uint32_t selectedPersistentVoxelSceneInstanceCount = 0;
};

NRISceneFrameDynamicStateInputs MakeNRISceneFrameDynamicStateInputs(const NRISceneFrameDynamicStateBuildRequest& request);
NRISceneFrameGenerationInputs MakeNRISceneFrameGenerationInputs(const NRISceneFrameGenerationBuildRequest& request);
