#pragma once

#include "nri_scene_frame_builder.h"
#include "nri_persistent_voxels.h"

struct NRISceneFrameOverlaySourceTelemetry
{
	double* totalMs = nullptr;
	double* geometryMs = nullptr;
	double* materialMs = nullptr;
	uint32_t* primitiveCount = nullptr;
	uint32_t* materialCount = nullptr;
	NRIRenderer::PerfShellTraceStats::OverlayAppendSourceTraceEntry* sourceTrace = nullptr;
};

struct NRISceneFrameOverlayBuildInputs
{
	bool collectTiming = false;
	uint64_t mapWorldBuildSerial = 0;
	uint64_t frameIndex = 0;
	NRIRenderer::PerfShellTraceStats* stats = nullptr;

	bool hasPersistentVoxelOverlay = false;
	const NRIPersistentVoxelOverlayStats* persistentVoxelOverlayStats = nullptr;

	bool hasRuntimeSpaceLinkOverlay = false;
	const nri_scene::GeometryData* runtimeSpaceLinkGeometry = nullptr;
	const nri_scene::MaterialBridgeData* runtimeSpaceLinkMaterials = nullptr;
	NRISceneFrameOverlaySourceTelemetry runtimeSpaceLinkTelemetry = {};

	bool hasRuntimeMutationOverlay = false;
	const nri_scene::GeometryData* runtimeMutationGeometry = nullptr;
	const nri_scene::MaterialBridgeData* runtimeMutationMaterials = nullptr;
	NRISceneFrameOverlaySourceTelemetry runtimeMutationTelemetry = {};

	bool hasActiveDynamicOverlay = false;
	const nri_scene::SceneView* activeDynamicSceneView = nullptr;
	const nri_scene::GeometryData* activeDynamicGeometry = nullptr;
	const nri_scene::MaterialBridgeData* activeDynamicMaterials = nullptr;
	NRISceneFrameOverlaySourceTelemetry activeDynamicTelemetry = {};

	bool hasMirrorExtendedDynamicOverlay = false;
	const nri_scene::SceneView* mirrorExtendedSceneView = nullptr;
	const nri_scene::GeometryData* mirrorExtendedGeometry = nullptr;
	const nri_scene::MaterialBridgeData* mirrorExtendedMaterials = nullptr;
	NRISceneFrameOverlaySourceTelemetry mirrorExtendedTelemetry = {};

	bool hasMirrorPlayerOverlay = false;
	const nri_scene::GeometryData* mirrorPlayerGeometry = nullptr;
	const nri_scene::MaterialBridgeData* mirrorPlayerMaterials = nullptr;
	NRISceneFrameOverlaySourceTelemetry mirrorPlayerTelemetry = {};

	bool hasRuntimeDebugSphereOverlay = false;
	const nri_scene::GeometryData* runtimeDebugSphereGeometry = nullptr;
	const nri_scene::MaterialBridgeData* runtimeDebugSphereMaterials = nullptr;
	NRISceneFrameOverlaySourceTelemetry runtimeDebugSphereTelemetry = {};

	bool hasSurfaceLightOverlay = false;
	const nri_scene::GeometryData* surfaceLightGeometry = nullptr;
	const nri_scene::MaterialBridgeData* surfaceLightMaterials = nullptr;
	NRISceneFrameOverlaySourceTelemetry surfaceLightTelemetry = {};
};

struct NRISceneFrameOverlayBuildOutputs
{
	nri_scene::GeometryData* overlayGeometry = nullptr;
	nri_scene::MaterialBridgeData* overlayMaterialBridge = nullptr;
	std::vector<NRIRenderer::SceneBufferUploadDomainSpan>* uploadSpans = nullptr;
};

void BuildNRISceneFrameOverlay(
	const NRISceneFrameOverlayBuildInputs& inputs,
	const NRISceneFrameOverlayBuildOutputs& outputs);
