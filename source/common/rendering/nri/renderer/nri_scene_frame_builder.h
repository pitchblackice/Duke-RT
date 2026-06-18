#pragma once

#include "nri_surface_probe.h"

#include "nri_renderer.h"

struct NRISceneContribution
{
	const nri_scene::GeometryData* geometry = nullptr;
	const NRIRenderer::SceneBufferUploadProducerStamp* producerStamp = nullptr;
	const nri_scene::MaterialBridgeData* materials = nullptr;
	NRIRenderer::SceneBufferUploadDomain uploadDomain = NRIRenderer::SceneBufferUploadDomain::StaticOverlay;
};

struct NRISceneContributionReserve
{
	size_t vertices = 0;
	size_t indices = 0;
	size_t primitives = 0;
	size_t primitiveProvenance = 0;
	size_t materials = 0;
	size_t lightMetadata = 0;
	size_t textures = 0;
	size_t paletteLookup = 0;
};

struct NRISceneContributionAppendStats
{
	double* totalMs = nullptr;
	double* geometryMs = nullptr;
	double* materialMs = nullptr;
	uint32_t* primitiveCount = nullptr;
	uint32_t* materialCount = nullptr;
	NRIRenderer::PerfShellTraceStats::OverlayAppendSourceTraceEntry* sourceTrace = nullptr;
};

struct NRISceneFrameGenerationInputs
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
	bool hasLocalPlayerReflectionScene = false;
	const nri_scene::GeometryData* localPlayerReflectionGeometry = nullptr;
	const nri_scene::MaterialBridgeData* localPlayerReflectionMaterials = nullptr;
	const nri_scene::MaterialBridgeData* activeMaterialBridge = nullptr;
	const std::vector<nri_scene::MaterialData>* activeGpuMaterials = nullptr;
	uint32_t sceneTextureCacheCount = 0;
	uint32_t selectedTlasInstanceCount = 0;
	uint32_t selectedSceneInstanceCount = 0;
	uint32_t selectedStaticSceneInstanceCount = 0;
	uint32_t selectedDynamicSceneInstanceCount = 0;
	uint32_t selectedPersistentVoxelSceneInstanceCount = 0;
};

struct NRISceneFrameGenerationResult
{
	NRIRenderer::StateCommitDomainGenerations current = {};
	uint32_t changedStaticMap = 0;
	uint32_t changedRuntimeMutation = 0;
	uint32_t changedDynamicActors = 0;
	uint32_t changedLocalPlayerReflection = 0;
	uint32_t changedPersistentVoxels = 0;
	uint32_t changedMaterialBridge = 0;
	uint32_t changedTextures = 0;
	uint32_t changedTlasInstances = 0;
	uint32_t changedSceneConstants = 0;
	uint32_t changedDomainCount = 0;
};

struct NRISceneFrameDebugStatsInputs
{
	const nri_scene::SceneDebugStats* staticMapStats = nullptr;
	const nri_scene::SceneView* deferredDynamicSceneView = nullptr;
	const nri_scene::SceneView* activeDynamicSceneView = nullptr;
	const nri_scene::SceneDebugStats* persistentVoxelStats = nullptr;
	const nri_scene::SceneView* localPlayerReflectionSceneView = nullptr;
	double* totalMs = nullptr;
	double* baseMs = nullptr;
	double* persistentVoxelMs = nullptr;
	double* localPlayerReflectionMs = nullptr;
	double* mergeMs = nullptr;
};

struct NRISceneFrameDynamicStateInputs
{
	const nri_scene::SceneView* activeDynamicSceneView = nullptr;
	const nri_scene::GeometryData* activeDynamicGeometry = nullptr;
	const nri_scene::MaterialBridgeData* activeDynamicMaterials = nullptr;
	const nri_scene::SceneView* localPlayerReflectionSceneView = nullptr;
	const nri_scene::GeometryData* localPlayerReflectionGeometry = nullptr;
	const nri_scene::MaterialBridgeData* localPlayerReflectionMaterials = nullptr;
	double* totalMs = nullptr;
	double* dynamicCoreMs = nullptr;
	double* localPlayerReflectionMs = nullptr;
};

struct NRISceneSurfaceProbeFrameInputs
{
	bool usesStaticMapScene = false;
	uint32_t activeStaticProbePrimitiveCount = 0;
	const nri_scene::GeometryData* runtimeSpaceLinkGeometry = nullptr;
	const nri_scene::GeometryData* runtimeMutationGeometry = nullptr;
	const nri_scene::GeometryData* overlayGeometry = nullptr;
	const nri_scene::GeometryData* activeDynamicGeometry = nullptr;
};

void AccumulateNRISceneContributionReserve(const NRISceneContribution& contribution, NRISceneContributionReserve& reserve);
void ReserveNRISceneContributionCapacity(
	const NRISceneContributionReserve& reserve,
	nri_scene::GeometryData& overlayGeometry,
	nri_scene::MaterialBridgeData& overlayMaterialBridge);
void AppendNRISceneContribution(
	const NRISceneContribution& contribution,
	NRISceneContributionAppendStats stats,
	nri_scene::GeometryData& overlayGeometry,
	nri_scene::MaterialBridgeData& overlayMaterialBridge,
	std::vector<NRIRenderer::SceneBufferUploadDomainSpan>& uploadSpans);
NRISceneFrameGenerationResult BuildNRISceneFrameGenerationResult(
	const NRISceneFrameGenerationInputs& inputs,
	const NRIRenderer::StateCommitDomainGenerations& previous,
	bool hasPrevious);
void WriteNRISceneFrameGenerationTraceStats(
	const NRISceneFrameGenerationResult& result,
	NRIRenderer::PerfShellTraceStats& stats);
nri_scene::SceneDebugStats BuildNRISceneFrameDebugStats(
	const NRISceneFrameDebugStatsInputs& inputs,
	NRIRenderer::PerfShellTraceStats& stats);
NRIRenderer::DynamicSceneFrameState BuildNRISceneFrameDynamicState(
	const NRISceneFrameDynamicStateInputs& inputs,
	const NRIRenderer::DynamicSceneFrameState& previous,
	NRIRenderer::PerfShellTraceStats& stats);
NRISurfaceProbeFrameState BuildNRISceneSurfaceProbeFrameState(
	const NRISceneSurfaceProbeFrameInputs& inputs);
