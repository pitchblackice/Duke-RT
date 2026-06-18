#include "nri_scene_frame_mirrors.h"
#include "nri_cvars.h"
#include "nri_upload_hash.h"
#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_hash.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"
#include "coreactor.h"
#include "coreplayer.h"
#include "gamecontrol.h"
#include "gamestruct.h"
#include "hw_voxels.h"
#include "printf.h"

#include <cstring>

namespace
{
	static uint32_t CountSceneViewSurfaces(const nri_scene::SceneView& sceneView)
	{
		return (uint32_t)(sceneView.opaqueWalls.size() + sceneView.opaqueFlats.size() + sceneView.opaqueSprites.size());
	}

	static bool localPlayerReflectionCaptureStatsDiffer(const NRILocalPlayerReflectionCaptureStats& a, const NRILocalPlayerReflectionCaptureStats& b)
	{
		return
			a.viewpointActorIndex != b.viewpointActorIndex ||
			a.localPlayerActorIndex != b.localPlayerActorIndex ||
			a.viewpointMatchesLocalPlayer != b.viewpointMatchesLocalPlayer ||
			a.capturedScene != b.capturedScene ||
			a.rawFacingSprites != b.rawFacingSprites ||
			a.rawVoxelSprites != b.rawVoxelSprites ||
			a.capturedSurfaceCount != b.capturedSurfaceCount ||
			a.capturedMatchingActorSurfaces != b.capturedMatchingActorSurfaces ||
			a.capturedOtherActorSurfaces != b.capturedOtherActorSurfaces ||
			a.capturedActorlessSurfaces != b.capturedActorlessSurfaces ||
			a.filteredSurfaceCount != b.filteredSurfaceCount;
	}

	static uint32_t CountDrawListActorSprites(const HWDrawList& drawList, int32_t actorIndex, bool requireVoxel)
	{
		uint32_t count = 0;
		for (auto* sprite : drawList.sprites)
		{
			if (sprite == nullptr || sprite->Sprite == nullptr || sprite->Sprite->ownerActor == nullptr)
			{
				continue;
			}

			if ((int32_t)sprite->Sprite->ownerActor->GetIndex() != actorIndex)
			{
				continue;
			}

			const bool isVoxelSprite =
				sprite->modelframe < 0 &&
				sprite->voxel != nullptr &&
				sprite->voxel->model != nullptr;
			if (requireVoxel ? isVoxelSprite : !isVoxelSprite)
			{
				count++;
			}
		}
		return count;
	}

	static void AccumulateSceneViewActorSurfaceStats(
		const nri_scene::SceneView& sceneView,
		int32_t actorIndex,
		uint32_t& outMatching,
		uint32_t& outOther,
		uint32_t& outActorless)
	{
		auto visit = [&](const auto& surfaces)
		{
			for (const auto& surface : surfaces)
			{
				if (surface.provenance.actorIndex < 0)
				{
					outActorless++;
				}
				else if (surface.provenance.actorIndex == actorIndex)
				{
					outMatching++;
				}
				else
				{
					outOther++;
				}
			}
		};

		visit(sceneView.opaqueWalls);
		visit(sceneView.opaqueFlats);
		visit(sceneView.opaqueSprites);
	}

	static void TraceLocalPlayerReflectionCaptureStats(const NRILocalPlayerReflectionCaptureStats& stats)
	{
		static bool hasPrevious = false;
		static NRILocalPlayerReflectionCaptureStats previous = {};
		if (!nri_ptscenestats)
		{
			hasPrevious = false;
			previous = {};
			return;
		}

		if (hasPrevious && !localPlayerReflectionCaptureStatsDiffer(previous, stats))
		{
			return;
		}

		Printf("NRI PT local player reflection capture: view_actor=%d local_actor=%d camera_match=%s raw_facing=%u raw_voxels=%u captured=%s surfaces=%u match=%u other=%u actorless=%u filtered=%u\n",
			stats.viewpointActorIndex,
			stats.localPlayerActorIndex,
			stats.viewpointMatchesLocalPlayer ? "yes" : "no",
			stats.rawFacingSprites,
			stats.rawVoxelSprites,
			stats.capturedScene ? "yes" : "no",
			stats.capturedSurfaceCount,
			stats.capturedMatchingActorSurfaces,
			stats.capturedOtherActorSurfaces,
			stats.capturedActorlessSurfaces,
			stats.filteredSurfaceCount);
		hasPrevious = true;
		previous = stats;
	}

	class ScopedLocalPlayerReflectionVisibilityCaptureOverride
	{
public:
		explicit ScopedLocalPlayerReflectionVisibilityCaptureOverride(bool enabled)
		{
			if (!enabled || gi == nullptr) return;
			mEnabled = true;
			mPrevious = gi->GetMirrorPlayerVisibilityCaptureOverride();
			gi->SetMirrorPlayerVisibilityCaptureOverride(true);
		}

		~ScopedLocalPlayerReflectionVisibilityCaptureOverride()
		{
			if (mEnabled && gi != nullptr)
			{
				gi->SetMirrorPlayerVisibilityCaptureOverride(mPrevious);
			}
		}

		ScopedLocalPlayerReflectionVisibilityCaptureOverride(const ScopedLocalPlayerReflectionVisibilityCaptureOverride&) = delete;
		ScopedLocalPlayerReflectionVisibilityCaptureOverride& operator=(const ScopedLocalPlayerReflectionVisibilityCaptureOverride&) = delete;

	private:
		bool mEnabled = false;
		bool mPrevious = false;
	};

	static bool AppendLocalPlayerReflectionSurfaces(const nri_scene::SceneView& sourceView, nri_scene::SceneView& outView)
	{
		for (const nri_scene::SurfaceRef& sourceSurface : sourceView.opaqueSprites)
		{
			outView.opaqueSprites.push_back(sourceSurface);
		}

		return !outView.opaqueSprites.empty();
	}

	static bool CaptureLocalPlayerReflectionDynamicScene(
		HWDrawInfo& di,
		nri_scene::SceneView& outView,
		NRIMirrorRebuildSceneViewStatsFn rebuildSceneViewStats,
		NRILocalPlayerReflectionCaptureStats* outStats = nullptr)
	{
		outView = {};
		NRILocalPlayerReflectionCaptureStats captureStats = {};
		captureStats.viewpointActorIndex = di.Viewpoint.CameraActor != nullptr ? (int32_t)di.Viewpoint.CameraActor->GetIndex() : -1;
		const auto publishStats = [&]()
		{
			if (outStats != nullptr)
			{
				*outStats = captureStats;
			}
			TraceLocalPlayerReflectionCaptureStats(captureStats);
		};
		if (gi == nullptr ||
			myconnectindex < 0 ||
			myconnectindex >= MAXPLAYERS)
		{
			publishStats();
			return false;
		}

		DCorePlayer* localPlayer = PlayerArray[myconnectindex];
		DCoreActor* localPlayerActor = localPlayer != nullptr ? localPlayer->GetActor() : nullptr;
		if (localPlayerActor == nullptr)
		{
			publishStats();
			return false;
		}

		const int32_t actorIndex = (int32_t)localPlayerActor->GetIndex();
		captureStats.localPlayerActorIndex = actorIndex;
		captureStats.viewpointMatchesLocalPlayer = di.Viewpoint.CameraActor == localPlayerActor;
		HWDrawInfo* captureDi = HWDrawInfo::StartDrawInfo(&di, di.Viewpoint, &di.VPUniforms);
		captureDi->visibility = di.visibility;
		captureDi->rellight = di.rellight;

		const ScopedLocalPlayerReflectionVisibilityCaptureOverride reflectionCaptureOverride(true);
		captureDi->CreateScene(false);
		captureStats.rawFacingSprites = CountDrawListActorSprites(captureDi->drawlists[GLDL_TRANSLUCENT], actorIndex, false);
		captureStats.rawVoxelSprites = CountDrawListActorSprites(captureDi->drawlists[GLDL_MODELS], actorIndex, true);

		nri_scene::SceneView capturedView;
		const bool hasCapture = nri_scene::CaptureActorSpriteScene(*captureDi, actorIndex, capturedView);
		captureDi->EndDrawInfo();
		if (!hasCapture || !AppendLocalPlayerReflectionSurfaces(capturedView, outView))
		{
			publishStats();
			outView = {};
			return false;
		}

		outView.drawInfo = &di;
		if (rebuildSceneViewStats != nullptr)
		{
			rebuildSceneViewStats(outView);
		}
		captureStats.capturedScene = true;
		captureStats.capturedSurfaceCount = CountSceneViewSurfaces(outView);
		AccumulateSceneViewActorSurfaceStats(
			outView,
			actorIndex,
			captureStats.capturedMatchingActorSurfaces,
			captureStats.capturedOtherActorSurfaces,
			captureStats.capturedActorlessSurfaces);

		outView.primitiveFlags = nri_scene::PrimitiveFlag_ReflectionOnly;
		captureStats.filteredSurfaceCount = captureStats.capturedSurfaceCount;
		publishStats();
		return true;
	}

	static uint64_t HashLocalPlayerReflectionMaterialPayloadData(const nri_scene::MaterialBridgeData& materialBridge)
	{
		const uint64_t materialSize = (uint64_t)materialBridge.materials.size() * sizeof(nri_scene::MaterialData);
		return NRIHashUploadPayloadBytes(
			materialBridge.materials.empty() ? nullptr : materialBridge.materials.data(),
			materialSize);
	}

	static uint64_t BuildConservativeLocalPlayerReflectionPayloadStamp(
		uint64_t kind,
		uint64_t frameIndex,
		const nri_scene::GeometryData& geometry,
		const nri_scene::MaterialBridgeData& materials,
		uint64_t mapWorldBuildSerial)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, kind);
		hash = nri_scene::HashCombine64(hash, frameIndex);
		hash = nri_scene::HashCombine64(hash, mapWorldBuildSerial);
		hash = nri_scene::HashCombine64(hash, (uint64_t)geometry.vertices.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)geometry.indices.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)geometry.primitives.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)geometry.primitiveProvenance.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)materials.materials.size());
		return hash != 0 ? hash : 1;
	}

	static NRILocalPlayerReflectionUploadStamp BuildLocalPlayerReflectionUploadProducerStamp(
		const nri_scene::GeometryData& geometry,
		const nri_scene::MaterialBridgeData& materials,
		uint64_t frameIndex,
		uint64_t mapWorldBuildSerial)
	{
		NRILocalPlayerReflectionUploadStamp stamp = {};
		stamp.vertexPayloadStamp = BuildConservativeLocalPlayerReflectionPayloadStamp(1u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.indexPayloadStamp = BuildConservativeLocalPlayerReflectionPayloadStamp(2u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.primitivePayloadStamp = BuildConservativeLocalPlayerReflectionPayloadStamp(3u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.primitiveProvenanceStamp = BuildConservativeLocalPlayerReflectionPayloadStamp(4u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.materialPayloadStamp = HashLocalPlayerReflectionMaterialPayloadData(materials);
		return stamp;
	}
}

NRILocalPlayerReflectionCaptureResult CaptureNRILocalPlayerReflectionDynamicScene(const NRILocalPlayerReflectionCaptureRequest& request, nri_scene::SceneView& outView)
{
	NRILocalPlayerReflectionCaptureResult result = {};
	if (request.drawInfo == nullptr)
	{
		outView = {};
		return result;
	}
	result.captured = CaptureLocalPlayerReflectionDynamicScene(
		*request.drawInfo,
		outView,
		request.rebuildSceneViewStats,
		&result.stats);
	return result;
}

NRILocalPlayerReflectionUploadStamp BuildNRILocalPlayerReflectionUploadProducerStamp(
	const nri_scene::GeometryData& geometry,
	const nri_scene::MaterialBridgeData& materials,
	uint64_t frameIndex,
	uint64_t mapWorldBuildSerial)
{
	return BuildLocalPlayerReflectionUploadProducerStamp(geometry, materials, frameIndex, mapWorldBuildSerial);
}
