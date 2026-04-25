#include "nri_portal_bridge.h"

#include "c_cvars.h"
#include "gamestruct.h"
#include "hw_portal.h"
#include "image.h"
#include "textures.h"
#include "v_video.h"

#include <algorithm>
#include <cstdint>
#define NOMINMAX
#include <windows.h>

CVAR(Int, nri_ptportaldepth, 6, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

namespace
{
	using namespace nri_scene;
	thread_local int gPortalCaptureDepth = 0;

	struct CaptureState
	{
		FRenderState* renderState = nullptr;
	};

	class ScopedPortalCaptureState
	{
	public:
		ScopedPortalCaptureState(DCoreActor* viewer, int type)
			: mViewer(viewer), mType(type)
		{
			if (gi != nullptr)
			{
				gi->EnterPortal(mViewer, mType);
				mEntered = true;
			}
		}

		~ScopedPortalCaptureState()
		{
			if (mEntered && gi != nullptr)
			{
				gi->LeavePortal(mViewer, mType);
			}
		}

	private:
		DCoreActor* mViewer = nullptr;
		int mType = -1;
		bool mEntered = false;
	};

	bool IsUsableGameTexturePointer(FGameTexture* texture)
	{
		const uintptr_t value = (uintptr_t)texture;
		if (value <= 0x10000 ||
			value == (uintptr_t)-1 ||
			(value & (sizeof(void*) - 1)) != 0)
		{
			return false;
		}

		MEMORY_BASIC_INFORMATION pointerInfo = {};
		if (VirtualQuery(texture, &pointerInfo, sizeof(pointerInfo)) != sizeof(pointerInfo) ||
			pointerInfo.State != MEM_COMMIT ||
			(pointerInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
		{
			return false;
		}

		void* vtable = nullptr;
		__try
		{
			vtable = *(void**)texture;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			vtable = nullptr;
		}

		if (vtable == nullptr)
		{
			return false;
		}

		MEMORY_BASIC_INFORMATION vtableInfo = {};
		return VirtualQuery(vtable, &vtableInfo, sizeof(vtableInfo)) == sizeof(vtableInfo) &&
			vtableInfo.State == MEM_COMMIT &&
			(vtableInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
	}

	void TranslateSurface(SurfaceRef& surface, const float delta[3])
	{
		for (CapturedVertex& vertex : surface.vertices)
		{
			for (int i = 0; i < 3; ++i)
			{
				vertex.position[i] += delta[i];
				vertex.prevPosition[i] += delta[i];
			}
		}
	}

	void AppendTranslatedSurfaces(const std::vector<SurfaceRef>& surfaces, const float delta[3], std::vector<SurfaceRef>& outSurfaces)
	{
		for (const SurfaceRef& source : surfaces)
		{
			SurfaceRef translated = source;
			TranslateSurface(translated, delta);
			outSurfaces.push_back(std::move(translated));
		}
	}

	void AppendTranslatedScene(const SceneView& source, const float delta[3], SceneView& outView)
	{
		AppendTranslatedSurfaces(source.opaqueWalls, delta, outView.opaqueWalls);
		AppendTranslatedSurfaces(source.opaqueFlats, delta, outView.opaqueFlats);
		AppendTranslatedSurfaces(source.opaqueSprites, delta, outView.opaqueSprites);
		outView.stats.portalViews += 1;
		outView.stats.materialRefs += source.stats.materialRefs;
		outView.stats.triangleEstimate += source.stats.triangleEstimate;
		outView.stats.mirrorSurfaces += source.stats.mirrorSurfaces;
		outView.stats.skySurfaces += source.stats.skySurfaces;
		outView.stats.modelDrawItems += source.stats.modelDrawItems;
		outView.stats.voxelProxyDrawItems += source.stats.voxelProxyDrawItems;
		outView.stats.unsupportedModelDrawItems += source.stats.unsupportedModelDrawItems;
		outView.stats.voxelStableCandidates += source.stats.voxelStableCandidates;
		outView.stats.voxelStableUncacheable += source.stats.voxelStableUncacheable;
		outView.stats.voxelStableSignatureHits += source.stats.voxelStableSignatureHits;
		outView.stats.voxelStableSignatureMisses += source.stats.voxelStableSignatureMisses;
		outView.stats.voxelStableSignatureChanges += source.stats.voxelStableSignatureChanges;
		outView.stats.voxelStableSplitStable += source.stats.voxelStableSplitStable;
		outView.stats.voxelStableSplitLive += source.stats.voxelStableSplitLive;
		outView.stats.voxelCacheEntries += source.stats.voxelCacheEntries;
		outView.stats.voxelCacheSurfaceHits += source.stats.voxelCacheSurfaceHits;
		outView.stats.voxelCacheSurfaceStores += source.stats.voxelCacheSurfaceStores;
		outView.stats.voxelCacheSurfaceRebuilds += source.stats.voxelCacheSurfaceRebuilds;
		outView.stats.voxelCacheSurfaceRemoves += source.stats.voxelCacheSurfaceRemoves;
		outView.stats.voxelCacheNotCaptured += source.stats.voxelCacheNotCaptured;
		outView.stats.voxelCachePrimitives += source.stats.voxelCachePrimitives;
		outView.stats.portalCapturesSkipped += source.stats.portalCapturesSkipped;
		if (source.sky.priority > outView.sky.priority)
		{
			outView.sky = source.sky;
			Copy3(source.skyColor, outView.skyColor);
		}
	}

	void MergeSkyFromPortals(HWDrawInfo& di, SceneView& outView)
	{
		for (HWPortal* portal : di.Portals)
		{
			auto* skyPortal = portal != nullptr && portal->IsSky() ? portal : nullptr;
			if (skyPortal == nullptr || skyPortal->lines.Size() == 0)
			{
				continue;
			}

			for (const HWWall& wall : skyPortal->lines)
			{
				if (wall.sky != nullptr)
				{
					FGameTexture* texture = nullptr;
					uint32_t fadeColor = 0;
					__try
					{
						texture = wall.sky->texture;
						fadeColor = wall.sky->fadecolor.d;
					}
					__except (EXCEPTION_EXECUTE_HANDLER)
					{
						texture = nullptr;
						fadeColor = 0;
					}
					UpdateSceneSky(outView, texture, fadeColor, PTSkySourceType::Portal);
					outView.stats.skySurfaces++;
					return;
				}
			}
		}
	}

	bool ShouldCapturePortal(HWPortal* portal)
	{
		if (portal == nullptr)
		{
			return false;
		}

		switch (portal->GetType())
		{
		case PORTAL_WALL_MIRROR:
		case PORTAL_WALL_VIEW:
		case PORTAL_WALL_TO_SPRITE:
		case PORTAL_SECTOR_FLOOR:
		case PORTAL_SECTOR_CEILING:
			return true;
		default:
			return false;
		}
	}

	unsigned int CountCapturablePortals(HWDrawInfo& di)
	{
		unsigned int count = 0;
		for (HWPortal* portal : di.Portals)
		{
			if (ShouldCapturePortal(portal))
			{
				count++;
			}
		}

		return count;
	}

	void CapturePortalsRecursive(HWDrawInfo& di, SceneView& outView, CaptureState& state)
	{
		const int maxDepth = std::max(0, std::min((int)nri_ptportaldepth, 8));
		if (state.renderState == nullptr)
		{
			return;
		}

		if (gPortalCaptureDepth >= maxDepth)
		{
			outView.stats.portalCapturesSkipped += CountCapturablePortals(di);
			return;
		}

		gPortalCaptureDepth++;
		MergeSkyFromPortals(di, outView);

		for (HWPortal* portal : di.Portals)
		{
			if (!ShouldCapturePortal(portal))
			{
				continue;
			}

			auto* scenePortal = static_cast<HWScenePortalBase*>(portal);
			HWDrawInfo* child = HWDrawInfo::StartDrawInfo(&di, di.Viewpoint, &di.VPUniforms);
			if (child == nullptr)
			{
				continue;
			}

			const bool setup = scenePortal->SetupForSceneCapture(child, *state.renderState);
			if (!setup)
			{
				child->EndDrawInfo();
				continue;
			}

			const int portalType = portal->GetType();
			const bool portalFlag = portalType == PORTAL_SECTOR_CEILING;
			{
				const ScopedPortalCaptureState portalCaptureState(child->Viewpoint.CameraActor, portalType);
				child->CreateScene(portalFlag);
			}

			SceneView childView;
			if (CaptureScene(*child, childView))
			{
				const float translation[3] = {
					(float)(di.Viewpoint.Pos.X - child->Viewpoint.Pos.X),
					(float)(di.Viewpoint.Pos.Z - child->Viewpoint.Pos.Z),
					(float)(di.Viewpoint.Pos.Y - child->Viewpoint.Pos.Y)
				};

				AppendTranslatedScene(childView, translation, outView);
			}

			scenePortal->ShutdownAfterSceneCapture(child, *state.renderState);
			child->EndDrawInfo();
		}

		gPortalCaptureDepth--;
	}
}

namespace nri_scene
{
void CapturePortalViews(HWDrawInfo& di, SceneView& outView)
{
	CaptureState state = {};
	state.renderState = screen != nullptr ? screen->RenderState() : nullptr;
	CapturePortalsRecursive(di, outView, state);
}
}
