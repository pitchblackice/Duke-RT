#pragma once

#include <cstdint>

struct HWDrawInfo;

namespace nri_scene
{
	struct GeometryData;
	struct MaterialBridgeData;
	struct SceneView;
}

using NRIMirrorRebuildSceneViewStatsFn = void (*)(nri_scene::SceneView& sceneView);

struct NRILocalPlayerReflectionCaptureStats
{
	int32_t viewpointActorIndex = -1;
	int32_t localPlayerActorIndex = -1;
	bool viewpointMatchesLocalPlayer = false;
	bool primaryVisible = false;
	bool capturedScene = false;
	uint32_t rawFacingSprites = 0;
	uint32_t rawVoxelSprites = 0;
	uint32_t capturedSurfaceCount = 0;
	uint32_t capturedMatchingActorSurfaces = 0;
	uint32_t capturedOtherActorSurfaces = 0;
	uint32_t capturedActorlessSurfaces = 0;
	uint32_t filteredSurfaceCount = 0;
	double drawInfoSetupMs = 0.0;
	double processSpritesMs = 0.0;
	double dispatchSpritesMs = 0.0;
	double cacheCaptureMs = 0.0;
	double drawInfoReleaseMs = 0.0;
};

struct NRILocalPlayerReflectionCaptureRequest
{
	HWDrawInfo* drawInfo = nullptr;
	NRIMirrorRebuildSceneViewStatsFn rebuildSceneViewStats = nullptr;
	bool residentVoxelReady = false;
	bool localPlayerPrimaryVisible = false;
};

struct NRILocalPlayerReflectionCaptureResult
{
	bool captured = false;
	bool currentVoxel = false;
	NRILocalPlayerReflectionCaptureStats stats = {};
};

struct NRILocalPlayerReflectionUploadStamp
{
	uint64_t vertexPayloadStamp = 0;
	uint64_t indexPayloadStamp = 0;
	uint64_t primitivePayloadStamp = 0;
	uint64_t primitiveProvenanceStamp = 0;
	uint64_t materialPayloadStamp = 0;
};

int32_t ResolveNRILocalPlayerActorIndex();
NRILocalPlayerReflectionCaptureResult CaptureNRILocalPlayerReflectionDynamicScene(const NRILocalPlayerReflectionCaptureRequest& request, nri_scene::SceneView& outView);
NRILocalPlayerReflectionUploadStamp BuildNRILocalPlayerReflectionUploadProducerStamp(
	const nri_scene::GeometryData& geometry,
	const nri_scene::MaterialBridgeData& materials,
	uint64_t frameIndex,
	uint64_t mapWorldBuildSerial);
