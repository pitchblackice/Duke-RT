#pragma once

#include <cstdint>

struct HWDrawInfo;
class HWPortal;

namespace nri_scene
{
	struct GeometryData;
	struct MaterialBridgeData;
	struct SceneView;
}

using NRIMirrorRebuildSceneViewStatsFn = void (*)(nri_scene::SceneView& sceneView);

struct NRIMirrorPortalSelectionRequest
{
	const HWDrawInfo* drawInfo = nullptr;
	int32_t preferredWallIndex = -1;
};

struct NRIMirrorPortalSelectionResult
{
	HWPortal* portal = nullptr;
	uint32_t candidateCount = 0;
	int32_t selectedWallIndex = -1;
};

struct NRIMirrorExtendedCaptureRequest
{
	HWDrawInfo* drawInfo = nullptr;
	HWPortal* mirrorPortal = nullptr;
	const nri_scene::SceneView* baseDynamicSceneView = nullptr;
	uint32_t frameIndex = 0;
	int32_t selectedMirrorWallIndex = -1;
	NRIMirrorRebuildSceneViewStatsFn rebuildSceneViewStats = nullptr;
};

struct NRIMirrorExtendedCaptureResult
{
	bool captured = false;
};

struct NRIMirrorPlayerCaptureStats
{
	int32_t viewpointActorIndex = -1;
	int32_t localPlayerActorIndex = -1;
	int32_t selectedMirrorWallIndex = -1;
	bool viewpointMatchesLocalPlayer = false;
	bool capturedScene = false;
	uint32_t mirrorPortalCandidates = 0;
	uint32_t rawFacingSprites = 0;
	uint32_t rawVoxelSprites = 0;
	uint32_t capturedSurfaceCount = 0;
	uint32_t capturedMatchingActorSurfaces = 0;
	uint32_t capturedOtherActorSurfaces = 0;
	uint32_t capturedActorlessSurfaces = 0;
	uint32_t filteredSurfaceCount = 0;
};

struct NRIMirrorPlayerCaptureRequest
{
	HWDrawInfo* drawInfo = nullptr;
	HWPortal* mirrorPortal = nullptr;
	uint32_t mirrorPortalCandidates = 0;
	int32_t selectedMirrorWallIndex = -1;
	NRIMirrorRebuildSceneViewStatsFn rebuildSceneViewStats = nullptr;
};

struct NRIMirrorPlayerCaptureResult
{
	bool captured = false;
	NRIMirrorPlayerCaptureStats stats = {};
};

struct NRIMirrorPlayerUploadStamp
{
	uint64_t vertexPayloadStamp = 0;
	uint64_t indexPayloadStamp = 0;
	uint64_t primitivePayloadStamp = 0;
	uint64_t primitiveProvenanceStamp = 0;
	uint64_t materialPayloadStamp = 0;
};

NRIMirrorPortalSelectionResult SelectNRIPrimaryMirrorPortal(const NRIMirrorPortalSelectionRequest& request);
NRIMirrorExtendedCaptureResult CaptureNRIMirrorExtendedDynamicScene(const NRIMirrorExtendedCaptureRequest& request, nri_scene::SceneView& outView);
NRIMirrorPlayerCaptureResult CaptureNRIMirrorPlayerDynamicScene(const NRIMirrorPlayerCaptureRequest& request, nri_scene::SceneView& outView);
bool IsNRIMirrorPlayerPreviewCaptureEnabled();
NRIMirrorPlayerUploadStamp BuildNRIMirrorPlayerUploadProducerStamp(
	const nri_scene::GeometryData& geometry,
	const nri_scene::MaterialBridgeData& materials,
	uint64_t frameIndex,
	uint64_t mapWorldBuildSerial);
