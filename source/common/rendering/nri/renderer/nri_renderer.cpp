#include "nri_renderer.h"

#include "../framegen/nri_framegen.h"
#include "nri_renderstate.h"
#include "../scene/nri_map_builder.h"
#include "../system/nri_hwtexture.h"
#include "../system/nri_renderdevice.h"
#include "skyboxtexture.h"
#include "image.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "coreactor.h"
#include "coreplayer.h"
#include "hw_voxels.h"
#include "gamecontrol.h"
#include "lightoverlay.h"
#include "mapinfo.h"
#include "printf.h"
#include "gamestruct.h"
#include "hw_portal.h"
#include "texinfo.h"
#include "texturemanager.h"
#include "d_eventbase.h"
#include "v_video.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>

static constexpr int kPtDebugMenuModes[] = {
	0, 1, 2, 3, 4, 5,
	9, 10, 11, 12,
	16, 17, 18, 19,
	21, 22, 24, 25,
	26, 27, 28, 29,
	33, 34, 45
};

static int ClampPtDebugMenuIndex(int index)
{
	return std::clamp(index, 0, (int)std::size(kPtDebugMenuModes) - 1);
}

static int FindPtDebugMenuIndex(int debugMode)
{
	for (int i = 0; i < (int)std::size(kPtDebugMenuModes); ++i)
	{
		if (kPtDebugMenuModes[i] == debugMode)
		{
			return i;
		}
	}

	return -1;
}

static int ResolvePtDebugModeFromMenuIndex(int index)
{
	return kPtDebugMenuModes[ClampPtDebugMenuIndex(index)];
}

static int ResolvePtDebugMenuIndexFromMode(int debugMode)
{
	const int index = FindPtDebugMenuIndex(debugMode);
	return index >= 0 ? index : 0;
}

static bool gSyncingPtDebugMenu = false;

EXTERN_CVAR(Int, nri_ptdebugmenu)
CUSTOM_CVAR(Int, nri_ptdebug, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	const int resolvedMode = ResolvePtDebugModeFromMenuIndex(ResolvePtDebugMenuIndexFromMode(self));
	if (self != resolvedMode)
	{
		self = resolvedMode;
		return;
	}

	if (gSyncingPtDebugMenu)
	{
		return;
	}

	gSyncingPtDebugMenu = true;
	nri_ptdebugmenu = ResolvePtDebugMenuIndexFromMode(self);
	gSyncingPtDebugMenu = false;
}
CUSTOM_CVAR(Int, nri_ptdebugmenu, 0, CVAR_GLOBALCONFIG)
{
	const int clampedIndex = ClampPtDebugMenuIndex(self);
	if (self != clampedIndex)
	{
		self = clampedIndex;
		return;
	}

	if (gSyncingPtDebugMenu)
	{
		return;
	}

	gSyncingPtDebugMenu = true;
	nri_ptdebug = ResolvePtDebugModeFromMenuIndex(self);
	gSyncingPtDebugMenu = false;
}
CVAR(Bool, nri_denoise, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_nrddenoiser, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_upscaler, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_postsharpen, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_upscalermode, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_pttaa, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_renderscale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_sharpness, 0.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
EXTERN_CVAR(Bool, nri_ptscenestats)
EXTERN_CVAR(Float, nri_ptmirrordynamicdistance)
EXTERN_CVAR(Int, nri_pttraceframes)
EXTERN_CVAR(Int, perf_looptraceframes)
CUSTOM_CVAR(Int, nri_ptrebaselinecachechunksperframe, 16, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1)
	{
		self = 1;
	}
	else if (self > 1024)
	{
		self = 1024;
	}
}
CUSTOM_CVAR(Int, nri_ptrebaselineblasperframe, 8, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1)
	{
		self = 1;
	}
	else if (self > 1024)
	{
		self = 1024;
	}
}
CUSTOM_CVAR(Int, nri_ptactorspritetrace, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 2)
	{
		self = 2;
	}
}
CUSTOM_CVAR(Int, nri_ptoutputmode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 1)
	{
		self = 1;
	}
}
CUSTOM_CVAR(Int, nri_pttonemap, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 2)
	{
		self = 2;
	}
}
CUSTOM_CVAR(Float, nri_ptexposure, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.125f)
	{
		self = 0.125f;
	}
	else if (self > 8.0f)
	{
		self = 8.0f;
	}
}
CUSTOM_CVAR(Float, nri_ptcontrast, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.50f)
	{
		self = 0.50f;
	}
	else if (self > 1.50f)
	{
		self = 1.50f;
	}
}
CUSTOM_CVAR(Float, nri_ptsaturation, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.00f)
	{
		self = 0.00f;
	}
	else if (self > 2.00f)
	{
		self = 2.00f;
	}
}
CUSTOM_CVAR(Float, nri_ptshoulder, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.50f)
	{
		self = 0.50f;
	}
	else if (self > 1.50f)
	{
		self = 1.50f;
	}
}
CUSTOM_CVAR(Float, nri_pttoe, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.50f)
	{
		self = 0.50f;
	}
	else if (self > 1.50f)
	{
		self = 1.50f;
	}
}
CUSTOM_CVAR(Float, nri_ptpaperwhite, 200.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 80.0f)
	{
		self = 80.0f;
	}
	else if (self > 400.0f)
	{
		self = 400.0f;
	}
}
CUSTOM_CVAR(Int, nri_pthdrtonemap, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 2)
	{
		self = 2;
	}
}
CUSTOM_CVAR(Float, nri_pthdrexposure, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.125f)
	{
		self = 0.125f;
	}
	else if (self > 8.0f)
	{
		self = 8.0f;
	}
}
CUSTOM_CVAR(Float, nri_pthdrcontrast, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.50f)
	{
		self = 0.50f;
	}
	else if (self > 1.50f)
	{
		self = 1.50f;
	}
}
CUSTOM_CVAR(Float, nri_pthdrsaturation, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.00f)
	{
		self = 0.00f;
	}
	else if (self > 2.00f)
	{
		self = 2.00f;
	}
}
CUSTOM_CVAR(Float, nri_pthdrshoulder, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.50f)
	{
		self = 0.50f;
	}
	else if (self > 1.50f)
	{
		self = 1.50f;
	}
}
CUSTOM_CVAR(Float, nri_pthdrtoe, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.50f)
	{
		self = 0.50f;
	}
	else if (self > 1.50f)
	{
		self = 1.50f;
	}
}
CVAR(Bool, nri_ptnightvision, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Float, nri_ptnightvisionexposure, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.25f)
	{
		self = 0.25f;
	}
	else if (self > 4.0f)
	{
		self = 4.0f;
	}
}
CUSTOM_CVAR(Float, nri_ptnightvisioncontrast, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.25f)
	{
		self = 0.25f;
	}
	else if (self > 2.0f)
	{
		self = 2.0f;
	}
}
CUSTOM_CVAR(Float, nri_ptnightvisionsaturation, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 2.0f)
	{
		self = 2.0f;
	}
}
CUSTOM_CVAR(Float, nri_ptnightvisionred, 0.18f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 2.0f)
	{
		self = 2.0f;
	}
}
CUSTOM_CVAR(Float, nri_ptnightvisiongreen, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 2.0f)
	{
		self = 2.0f;
	}
}
CUSTOM_CVAR(Float, nri_ptnightvisionblue, 0.22f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 2.0f)
	{
		self = 2.0f;
	}
}
CVAR(Bool, nri_validation, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
EXTERN_CVAR(Bool, vid_vsync)
EXTERN_CVAR(Int, nri_ptspherelongs)
EXTERN_CVAR(Int, nri_ptspherelats)

namespace
{
	static constexpr uint32_t NriPtDebugSphereLimit = 64u;
	static constexpr uint32_t NriPtMuzzleFlashSlotCount = 8u;
	static constexpr double BuildTickSeconds = 1.0 / 120.0;

	static const char* GetNightVisionModeName(NRIPTNightVisionMode mode)
	{
		switch (mode)
		{
		case NRIPTNightVisionMode::Duke: return "duke";
		default: return "none";
		}
	}

	static uint32_t PackNightVisionControls(float contrast, float saturation)
	{
		const uint32_t contrastBits = (uint32_t)std::lround(std::clamp(contrast, 0.0f, 2.0f) * (65535.0f / 2.0f));
		const uint32_t saturationBits = (uint32_t)std::lround(std::clamp(saturation, 0.0f, 2.0f) * (65535.0f / 2.0f));
		return contrastBits | (saturationBits << 16);
	}

	static uint32_t PackNightVisionModeAndTint(NRIPTNightVisionMode mode, float red, float green, float blue)
	{
		const uint32_t redBits = (uint32_t)std::lround(std::clamp(red, 0.0f, 2.0f) * (255.0f / 2.0f));
		const uint32_t greenBits = (uint32_t)std::lround(std::clamp(green, 0.0f, 2.0f) * (255.0f / 2.0f));
		const uint32_t blueBits = (uint32_t)std::lround(std::clamp(blue, 0.0f, 2.0f) * (255.0f / 2.0f));
		return (uint32_t)mode | (redBits << 8) | (greenBits << 16) | (blueBits << 24);
	}

	static uint64_t HashCombineLightOverlay(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
	}

	static uint64_t QuantizeLightOverlayPositionKey(const float position[3])
	{
		const int64_t x = (int64_t)std::llround(position[0] * 16.0f);
		const int64_t y = (int64_t)std::llround(position[1] * 16.0f);
		const int64_t z = (int64_t)std::llround(position[2] * 16.0f);
		uint64_t key = 1469598103934665603ull;
		key = HashCombineLightOverlay(key, (uint64_t)x);
		key = HashCombineLightOverlay(key, (uint64_t)y);
		key = HashCombineLightOverlay(key, (uint64_t)z);
		return key;
	}

	static uint32_t GetGameplayLightTimeIndex()
	{
		return PlayClock > 0 ? (uint32_t)(PlayClock / 4) : 0u;
	}

	static void ComputeCapturedSurfaceCenter(const nri_scene::SurfaceRef& surface, float outCenter[3])
	{
		outCenter[0] = 0.0f;
		outCenter[1] = 0.0f;
		outCenter[2] = 0.0f;
		if (surface.vertices.empty())
		{
			return;
		}

		for (const nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			outCenter[0] += vertex.position[0];
			outCenter[1] += vertex.position[1];
			outCenter[2] += vertex.position[2];
		}

		const float invCount = 1.0f / (float)surface.vertices.size();
		outCenter[0] *= invCount;
		outCenter[1] *= invCount;
		outCenter[2] *= invCount;
	}

	static bool TryComputeCapturedSurfaceNormal(const nri_scene::SurfaceRef& surface, float outNormal[3])
	{
		if (surface.vertices.size() < 3)
		{
			return false;
		}

		const nri_scene::CapturedVertex& a = surface.vertices[0];
		const nri_scene::CapturedVertex& b = surface.vertices[1];
		const nri_scene::CapturedVertex& c = surface.vertices[2];
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float nx = aby * acz - abz * acy;
		const float ny = abz * acx - abx * acz;
		const float nz = abx * acy - aby * acx;
		const float lengthSq = nx * nx + ny * ny + nz * nz;
		if (lengthSq <= 1.0e-8f)
		{
			return false;
		}

		const float invLength = 1.0f / std::sqrt(lengthSq);
		outNormal[0] = nx * invLength;
		outNormal[1] = ny * invLength;
		outNormal[2] = nz * invLength;
		return true;
	}

	static void NudgeBlindSpotReplacementFlats(nri_scene::SceneView& sceneView)
	{
		static constexpr float kBlindSpotFlatDepthNudge = 0.01f;

		for (nri_scene::SurfaceRef& surface : sceneView.opaqueFlats)
		{
			if (surface.provenance.sourceType != nri_scene::SurfaceSourceType::MapFloorSection &&
				surface.provenance.sourceType != nri_scene::SurfaceSourceType::MapCeilingSection)
			{
				continue;
			}

			float normal[3] = {};
			if (!TryComputeCapturedSurfaceNormal(surface, normal))
			{
				continue;
			}

			for (nri_scene::CapturedVertex& vertex : surface.vertices)
			{
				vertex.position[0] += normal[0] * kBlindSpotFlatDepthNudge;
				vertex.position[1] += normal[1] * kBlindSpotFlatDepthNudge;
				vertex.position[2] += normal[2] * kBlindSpotFlatDepthNudge;
				vertex.prevPosition[0] += normal[0] * kBlindSpotFlatDepthNudge;
				vertex.prevPosition[1] += normal[1] * kBlindSpotFlatDepthNudge;
				vertex.prevPosition[2] += normal[2] * kBlindSpotFlatDepthNudge;
			}
		}
	}

	static void NudgeCapturedSurface(nri_scene::SurfaceRef& surface, float depthNudge)
	{
		float normal[3] = {};
		if (!TryComputeCapturedSurfaceNormal(surface, normal))
		{
			return;
		}

		for (nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			vertex.position[0] += normal[0] * depthNudge;
			vertex.position[1] += normal[1] * depthNudge;
			vertex.position[2] += normal[2] * depthNudge;
			vertex.prevPosition[0] += normal[0] * depthNudge;
			vertex.prevPosition[1] += normal[1] * depthNudge;
			vertex.prevPosition[2] += normal[2] * depthNudge;
		}
	}

	static bool IsMaterialOnlyChunkReplacement(uint32_t reasonMask)
	{
		const uint32_t materialOnlyReasonMask =
			nri_scene::PTMapChunkMutationReason_SectorMaterial |
			nri_scene::PTMapChunkMutationReason_WallMaterial;
		return
			(reasonMask & materialOnlyReasonMask) != 0 &&
			(reasonMask & ~materialOnlyReasonMask) == 0;
	}

	static uint32_t CountSceneViewSurfaces(const nri_scene::SceneView& sceneView)
	{
		return (uint32_t)(sceneView.opaqueWalls.size() + sceneView.opaqueFlats.size() + sceneView.opaqueSprites.size());
	}

	struct MirrorPlayerCaptureStats
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

	static bool MirrorPlayerCaptureStatsDiffer(const MirrorPlayerCaptureStats& a, const MirrorPlayerCaptureStats& b)
	{
		return
			a.viewpointActorIndex != b.viewpointActorIndex ||
			a.localPlayerActorIndex != b.localPlayerActorIndex ||
			a.selectedMirrorWallIndex != b.selectedMirrorWallIndex ||
			a.viewpointMatchesLocalPlayer != b.viewpointMatchesLocalPlayer ||
			a.capturedScene != b.capturedScene ||
			a.mirrorPortalCandidates != b.mirrorPortalCandidates ||
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

	static void TraceMirrorPlayerCaptureStats(const MirrorPlayerCaptureStats& stats)
	{
		static bool hasPrevious = false;
		static MirrorPlayerCaptureStats previous = {};
		if (!nri_ptscenestats)
		{
			hasPrevious = false;
			previous = {};
			return;
		}

		if (hasPrevious && !MirrorPlayerCaptureStatsDiffer(previous, stats))
		{
			return;
		}

		Printf("NRI PT mirror player capture: view_actor=%d local_actor=%d mirror_candidates=%u mirror_wall=%d camera_match=%s raw_facing=%u raw_voxels=%u captured=%s surfaces=%u match=%u other=%u actorless=%u filtered=%u\n",
			stats.viewpointActorIndex,
			stats.localPlayerActorIndex,
			stats.mirrorPortalCandidates,
			stats.selectedMirrorWallIndex,
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

	class ScopedMirrorPlayerVisibilityCaptureOverride
	{
public:
		explicit ScopedMirrorPlayerVisibilityCaptureOverride(bool enabled)
		{
			if (!enabled || gi == nullptr) return;
			mEnabled = true;
			mPrevious = gi->GetMirrorPlayerVisibilityCaptureOverride();
			gi->SetMirrorPlayerVisibilityCaptureOverride(true);
		}

		~ScopedMirrorPlayerVisibilityCaptureOverride()
		{
			if (mEnabled && gi != nullptr)
			{
				gi->SetMirrorPlayerVisibilityCaptureOverride(mPrevious);
			}
		}

		ScopedMirrorPlayerVisibilityCaptureOverride(const ScopedMirrorPlayerVisibilityCaptureOverride&) = delete;
		ScopedMirrorPlayerVisibilityCaptureOverride& operator=(const ScopedMirrorPlayerVisibilityCaptureOverride&) = delete;

	private:
		bool mEnabled = false;
		bool mPrevious = false;
	};

	static void RebuildSceneViewStats(nri_scene::SceneView& sceneView)
	{
		nri_scene::SceneDebugStats stats = {};
		stats.wallDrawItems = (uint32_t)sceneView.opaqueWalls.size();
		stats.flatDrawItems = (uint32_t)sceneView.opaqueFlats.size();
		stats.spriteDrawItems = (uint32_t)sceneView.opaqueSprites.size();

		for (const nri_scene::SurfaceRef& wall : sceneView.opaqueWalls)
		{
			stats.triangleEstimate += wall.vertices.size() >= 3 ? (uint32_t)wall.vertices.size() - 2u : 0u;
			stats.materialRefs++;
			if (wall.provenance.sourceType == nri_scene::SurfaceSourceType::MirrorWall)
			{
				stats.mirrorSurfaces++;
			}
		}

		for (const nri_scene::SurfaceRef& flat : sceneView.opaqueFlats)
		{
			stats.triangleEstimate += (uint32_t)(flat.vertices.size() / 3u);
			stats.materialRefs++;
		}

		for (const nri_scene::SurfaceRef& sprite : sceneView.opaqueSprites)
		{
			stats.triangleEstimate += sprite.vertices.size() >= 3 ? (uint32_t)sprite.vertices.size() - 2u : 0u;
			stats.materialRefs++;
			if (sprite.provenance.sourceType == nri_scene::SurfaceSourceType::VoxelProxySprite)
			{
				stats.modelDrawItems++;
				stats.voxelProxyDrawItems++;
			}
			else
			{
				stats.translucentDrawItems++;
			}
		}

		stats.totalDrawItems = stats.wallDrawItems + stats.flatDrawItems + stats.spriteDrawItems;
		sceneView.stats = stats;
	}

	static HWPortal* SelectPrimaryMirrorPortal(const HWDrawInfo& di, uint32_t& outCandidateCount, int32_t& outSelectedWallIndex, int32_t preferredWallIndex = -1)
	{
		outCandidateCount = 0;
		outSelectedWallIndex = -1;
		const DVector2 cameraPos(di.Viewpoint.Pos.X, -di.Viewpoint.Pos.Y);
		const DVector2 cameraPosRaw = di.Viewpoint.Pos.XY();
		const DVector2 cameraDir = di.Viewpoint.ViewVector;
		HWPortal* preferredPortal = nullptr;
		HWPortal* closestCenterHitPortal = nullptr;
		double closestCenterHitDistance = std::numeric_limits<double>::infinity();
		int32_t closestCenterHitWallIndex = -1;
		HWPortal* bestPortal = nullptr;
		double bestDistanceSquared = std::numeric_limits<double>::infinity();
		double bestFacing = -std::numeric_limits<double>::infinity();
		int32_t bestPortalWallIndex = -1;
		for (HWPortal* portal : di.Portals)
		{
			if (portal == nullptr || portal->GetType() != PORTAL_WALL_MIRROR)
			{
				continue;
			}

			outCandidateCount++;
			auto* mirrorLine = static_cast<walltype*>(portal->GetSource());
			if (mirrorLine == nullptr)
			{
				continue;
			}

			const int32_t mirrorWallIndex = wall.IndexOf(mirrorLine);
			if (preferredWallIndex >= 0 && mirrorWallIndex == preferredWallIndex)
			{
				preferredPortal = portal;
			}

			const walltype* next = mirrorLine->point2Wall();
			if (next == nullptr)
			{
				continue;
			}

			const DVector2 segmentStart(mirrorLine->pos.X, -mirrorLine->pos.Y);
			const DVector2 segmentDelta(next->pos.X - mirrorLine->pos.X, -next->pos.Y + mirrorLine->pos.Y);
			const double denominator = cameraDir.X * segmentDelta.Y - cameraDir.Y * segmentDelta.X;
			if (std::abs(denominator) > 1.0e-6)
			{
				const DVector2 fromCamera = segmentStart - cameraPos;
				const double rayDistance = (fromCamera.X * segmentDelta.Y - fromCamera.Y * segmentDelta.X) / denominator;
				const double segmentFraction = (fromCamera.X * cameraDir.Y - fromCamera.Y * cameraDir.X) / denominator;
				if (rayDistance >= 0.0 && segmentFraction >= 0.0 && segmentFraction <= 1.0 && rayDistance < closestCenterHitDistance)
				{
					closestCenterHitDistance = rayDistance;
					closestCenterHitPortal = portal;
					closestCenterHitWallIndex = mirrorWallIndex;
				}
			}

			double distanceSquared = std::numeric_limits<double>::infinity();
			DVector2 nearestVisiblePoint = cameraPosRaw;
			for (const HWWall& line : portal->lines)
			{
				const DVector2 lineStart(line.glseg.x1, line.glseg.y1);
				const DVector2 lineDelta(line.glseg.x2 - line.glseg.x1, line.glseg.y2 - line.glseg.y1);
				const double lineLengthSquared = lineDelta.LengthSquared();
				if (lineLengthSquared <= 1.0e-6)
				{
					continue;
				}

				const double t = clamp<double>(((cameraPosRaw - lineStart) | lineDelta) / lineLengthSquared, 0.0, 1.0);
				const DVector2 candidatePoint = lineStart + lineDelta * t;
				const double candidateDistanceSquared = (cameraPosRaw - candidatePoint).LengthSquared();
				if (candidateDistanceSquared < distanceSquared)
				{
					distanceSquared = candidateDistanceSquared;
					nearestVisiblePoint = candidatePoint;
				}
			}
			if (!std::isfinite(distanceSquared))
			{
				distanceSquared = SquareDistToWall(di.Viewpoint.Pos.X, di.Viewpoint.Pos.Y, mirrorLine);
			}
			if (distanceSquared <= 0.0001)
			{
				outSelectedWallIndex = mirrorWallIndex;
				return portal;
			}

			const DVector2 nearestVisiblePointFlipped(nearestVisiblePoint.X, -nearestVisiblePoint.Y);
			const DVector2 toMirror = nearestVisiblePointFlipped - cameraPos;
			const double visibleDistanceSquared = toMirror.LengthSquared();
			if (visibleDistanceSquared <= 0.0001)
			{
				outSelectedWallIndex = mirrorWallIndex;
				return portal;
			}

			const DVector2 toMirrorDir = toMirror / sqrt(visibleDistanceSquared);
			const double facing = cameraDir | toMirrorDir;
			if (distanceSquared < bestDistanceSquared - 0.0001 ||
				(std::abs(distanceSquared - bestDistanceSquared) <= 0.0001 && facing > bestFacing))
			{
				bestDistanceSquared = distanceSquared;
				bestFacing = facing;
				bestPortal = portal;
				bestPortalWallIndex = mirrorWallIndex;
			}
		}

		if (closestCenterHitPortal != nullptr)
		{
			outSelectedWallIndex = closestCenterHitWallIndex;
			return closestCenterHitPortal;
		}

		if (bestPortal != nullptr)
		{
			outSelectedWallIndex = bestPortalWallIndex;
			return bestPortal;
		}

		if (preferredPortal != nullptr)
		{
			outSelectedWallIndex = preferredWallIndex;
			return preferredPortal;
		}

		return nullptr;
	}

	struct MirrorBillboardLayout
	{
		nri_scene::CapturedVertex topLeft = {};
		nri_scene::CapturedVertex bottomLeft = {};
		nri_scene::CapturedVertex topRight = {};
		nri_scene::CapturedVertex bottomRight = {};
		float topCenter[3] = {};
		float bottomCenter[3] = {};
		float prevTopCenter[3] = {};
		float prevBottomCenter[3] = {};
		float halfWidth = 0.0f;
		float prevHalfWidth = 0.0f;
	};

	static void AverageCapturedVertexPair(const nri_scene::CapturedVertex& a, const nri_scene::CapturedVertex& b, float outPosition[3], float outPrevPosition[3])
	{
		for (int i = 0; i < 3; ++i)
		{
			outPosition[i] = (a.position[i] + b.position[i]) * 0.5f;
			outPrevPosition[i] = (a.prevPosition[i] + b.prevPosition[i]) * 0.5f;
		}
	}

	static float ComputeCapturedHorizontalDistance(const nri_scene::CapturedVertex& a, const float center[3], bool previous)
	{
		const float dx = (previous ? a.prevPosition[0] : a.position[0]) - center[0];
		const float dz = (previous ? a.prevPosition[2] : a.position[2]) - center[2];
		return sqrtf(dx * dx + dz * dz);
	}

	static bool ExtractMirrorBillboardLayout(const nri_scene::SurfaceRef& sourceSurface, MirrorBillboardLayout& outLayout)
	{
		if (sourceSurface.vertices.size() != 4 ||
			(sourceSurface.material.flags & nri_scene::MaterialFlag_FacingBillboard) == 0)
		{
			return false;
		}

		std::array<nri_scene::CapturedVertex, 4> vertices = {
			sourceSurface.vertices[0],
			sourceSurface.vertices[1],
			sourceSurface.vertices[2],
			sourceSurface.vertices[3]
		};

		std::sort(vertices.begin(), vertices.end(), [](const auto& a, const auto& b)
		{
			return a.position[1] > b.position[1];
		});

		std::array<nri_scene::CapturedVertex, 2> topPair = { vertices[0], vertices[1] };
		std::array<nri_scene::CapturedVertex, 2> bottomPair = { vertices[2], vertices[3] };

		const float axisX = topPair[1].position[0] - topPair[0].position[0];
		const float axisZ = topPair[1].position[2] - topPair[0].position[2];
		const float axisLength = sqrtf(axisX * axisX + axisZ * axisZ);
		if (axisLength <= 0.0001f)
		{
			return false;
		}

		const float invAxisLength = 1.0f / axisLength;
		const float normAxisX = axisX * invAxisLength;
		const float normAxisZ = axisZ * invAxisLength;

		auto horizontalProjection = [normAxisX, normAxisZ](const nri_scene::CapturedVertex& vertex)
		{
			return vertex.position[0] * normAxisX + vertex.position[2] * normAxisZ;
		};

		if (horizontalProjection(topPair[0]) > horizontalProjection(topPair[1]))
		{
			std::swap(topPair[0], topPair[1]);
		}
		if (horizontalProjection(bottomPair[0]) > horizontalProjection(bottomPair[1]))
		{
			std::swap(bottomPair[0], bottomPair[1]);
		}

		outLayout.topLeft = topPair[0];
		outLayout.topRight = topPair[1];
		outLayout.bottomLeft = bottomPair[0];
		outLayout.bottomRight = bottomPair[1];
		AverageCapturedVertexPair(outLayout.topLeft, outLayout.topRight, outLayout.topCenter, outLayout.prevTopCenter);
		AverageCapturedVertexPair(outLayout.bottomLeft, outLayout.bottomRight, outLayout.bottomCenter, outLayout.prevBottomCenter);
		outLayout.halfWidth =
			(ComputeCapturedHorizontalDistance(outLayout.topLeft, outLayout.topCenter, false) +
				ComputeCapturedHorizontalDistance(outLayout.bottomLeft, outLayout.bottomCenter, false)) * 0.5f;
		outLayout.prevHalfWidth =
			(ComputeCapturedHorizontalDistance(outLayout.topLeft, outLayout.prevTopCenter, true) +
				ComputeCapturedHorizontalDistance(outLayout.bottomLeft, outLayout.prevBottomCenter, true)) * 0.5f;
		return outLayout.halfWidth > 0.0001f;
	}

	static bool ComputeMirroredViewVector(const HWDrawInfo& di, const walltype& mirrorLine, float& outViewX, float& outViewY)
	{
		const walltype* next = mirrorLine.point2Wall();
		if (next == nullptr)
		{
			return false;
		}

		float lineX = (float)(next->pos.X - mirrorLine.pos.X);
		float lineY = (float)(-next->pos.Y + mirrorLine.pos.Y);
		const float lineLength = sqrtf(lineX * lineX + lineY * lineY);
		if (lineLength <= 0.0001f)
		{
			return false;
		}

		lineX /= lineLength;
		lineY /= lineLength;
		const float viewX = (float)di.Viewpoint.ViewVector.X;
		const float viewY = (float)di.Viewpoint.ViewVector.Y;
		const float projection = viewX * lineX + viewY * lineY;
		outViewX = lineX * (projection * 2.0f) - viewX;
		outViewY = lineY * (projection * 2.0f) - viewY;
		const float reflectedLength = sqrtf(outViewX * outViewX + outViewY * outViewY);
		if (reflectedLength <= 0.0001f)
		{
			return false;
		}

		outViewX /= reflectedLength;
		outViewY /= reflectedLength;
		return true;
	}

	static bool ApplyWallMirrorViewpoint(const walltype& mirrorLine, FRenderViewpoint& viewpoint)
	{
		const walltype* next = mirrorLine.point2Wall();
		if (next == nullptr)
		{
			return false;
		}

		const double x = mirrorLine.pos.X;
		const double y = mirrorLine.pos.Y;
		const double dx = next->pos.X - x;
		const double dy = next->pos.Y - y;
		const double lengthSq = dx * dx + dy * dy;
		if (lengthSq <= 0.0001)
		{
			return false;
		}

		const DVector2 viewPos = { viewpoint.Pos.X, -viewpoint.Pos.Y };
		const double projection = ((viewPos.X - x) * dx + (viewPos.Y - y) * dy) * 2.0;
		const double mirroredX = x * 2.0 + dx * projection / lengthSq - viewPos.X;
		const double mirroredY = y * 2.0 + dy * projection / lengthSq - viewPos.Y;

		const angle_t mirrorAngle = VecToAngle(dx, dy).BAMs();
		const angle_t mirroredRotAngle = mirrorAngle + mirrorAngle - viewpoint.RotAngle;

		viewpoint.Pos.X = mirroredX;
		viewpoint.Pos.Y = -mirroredY;
		viewpoint.RotAngle = mirroredRotAngle;
		viewpoint.SectNums = nullptr;
		viewpoint.SectCount = mirrorLine.sector;
		viewpoint.HWAngles.Yaw = FAngle::fromBam(-ANGLE_90 + mirroredRotAngle);

		const double focalTangent = tan(viewpoint.FieldOfView.Radians() / 2.0);
		const DAngle facingAngle = DAngle::fromDeg(270.0 - viewpoint.HWAngles.Yaw.Degrees());
		viewpoint.TanSin = focalTangent * facingAngle.Sin();
		viewpoint.TanCos = focalTangent * facingAngle.Cos();
		viewpoint.ViewVector = facingAngle.ToVector();
		return true;
	}

	static uint64_t BuildDynamicSurfaceMergeKey(const nri_scene::SurfaceRef& surface)
	{
		float center[3] = {};
		ComputeCapturedSurfaceCenter(surface, center);
		return SceneLightSystem::ComputeSurfaceIdentityKey(
			SceneLightRecordSource::DynamicScene,
			surface.provenance,
			center);
	}

	static float ComputeSurfaceDistanceSquaredToViewpoint(const FRenderViewpoint& viewpoint, const nri_scene::SurfaceRef& surface)
	{
		float center[3] = {};
		ComputeCapturedSurfaceCenter(surface, center);
		const float dx = center[0] - (float)viewpoint.Pos.X;
		const float dy = center[1] - (float)viewpoint.Pos.Z;
		const float dz = center[2] - (float)viewpoint.Pos.Y;
		return dx * dx + dy * dy + dz * dz;
	}

	static void SeedDynamicSurfaceMergeKeys(const nri_scene::SceneView& sceneView, std::unordered_set<uint64_t>& outKeys)
	{
		auto append = [&outKeys](const auto& surfaces)
		{
			for (const auto& surface : surfaces)
			{
				outKeys.insert(BuildDynamicSurfaceMergeKey(surface));
			}
		};

		append(sceneView.opaqueWalls);
		append(sceneView.opaqueFlats);
		append(sceneView.opaqueSprites);
	}

	static void AppendMirrorExtendedSurfaceList(
		const std::vector<nri_scene::SurfaceRef>& source,
		const FRenderViewpoint& viewpoint,
		float maxDistance,
		std::unordered_set<uint64_t>& existingKeys,
		std::vector<nri_scene::SurfaceRef>& destination)
	{
		const float maxDistanceSquared = maxDistance > 0.0f ? maxDistance * maxDistance : 0.0f;
		for (const nri_scene::SurfaceRef& surface : source)
		{
			if (maxDistanceSquared > 0.0f &&
				ComputeSurfaceDistanceSquaredToViewpoint(viewpoint, surface) > maxDistanceSquared)
			{
				continue;
			}

			const uint64_t key = BuildDynamicSurfaceMergeKey(surface);
			if (!existingKeys.insert(key).second)
			{
				continue;
			}

			destination.push_back(surface);
		}
	}

	static nri_scene::CapturedVertex MakeMirrorBillboardVertex(const nri_scene::CapturedVertex& source, const float center[3], float widthAxisX, float widthAxisY, float halfWidth, bool rightSide, bool previous)
	{
		nri_scene::CapturedVertex result = source;
		const float side = rightSide ? 1.0f : -1.0f;
		float* destination = previous ? result.prevPosition : result.position;
		destination[0] = center[0] + widthAxisX * halfWidth * side;
		destination[1] = center[1];
		destination[2] = center[2] + widthAxisY * halfWidth * side;
		return result;
	}

	static bool ReorientFacingBillboardForMirror(const HWDrawInfo& di, const walltype& mirrorLine, const nri_scene::SurfaceRef& sourceSurface, nri_scene::SurfaceRef& outSurface)
	{
		MirrorBillboardLayout layout = {};
		if (!ExtractMirrorBillboardLayout(sourceSurface, layout))
		{
			return false;
		}

		float mirroredViewX = 0.0f;
		float mirroredViewY = 0.0f;
		if (!ComputeMirroredViewVector(di, mirrorLine, mirroredViewX, mirroredViewY))
		{
			return false;
		}

		const float widthAxisX = -mirroredViewY;
		const float widthAxisY = mirroredViewX;
		const float prevHalfWidth = layout.prevHalfWidth > 0.0001f ? layout.prevHalfWidth : layout.halfWidth;

		outSurface = sourceSurface;
		outSurface.vertices.resize(4);
		outSurface.vertices[0] = MakeMirrorBillboardVertex(layout.topLeft, layout.topCenter, widthAxisX, widthAxisY, layout.halfWidth, false, false);
		outSurface.vertices[1] = MakeMirrorBillboardVertex(layout.topRight, layout.topCenter, widthAxisX, widthAxisY, layout.halfWidth, true, false);
		outSurface.vertices[2] = MakeMirrorBillboardVertex(layout.bottomLeft, layout.bottomCenter, widthAxisX, widthAxisY, layout.halfWidth, false, false);
		outSurface.vertices[3] = MakeMirrorBillboardVertex(layout.bottomRight, layout.bottomCenter, widthAxisX, widthAxisY, layout.halfWidth, true, false);
		outSurface.vertices[0] = MakeMirrorBillboardVertex(outSurface.vertices[0], layout.prevTopCenter, widthAxisX, widthAxisY, prevHalfWidth, false, true);
		outSurface.vertices[1] = MakeMirrorBillboardVertex(outSurface.vertices[1], layout.prevTopCenter, widthAxisX, widthAxisY, prevHalfWidth, true, true);
		outSurface.vertices[2] = MakeMirrorBillboardVertex(outSurface.vertices[2], layout.prevBottomCenter, widthAxisX, widthAxisY, prevHalfWidth, false, true);
		outSurface.vertices[3] = MakeMirrorBillboardVertex(outSurface.vertices[3], layout.prevBottomCenter, widthAxisX, widthAxisY, prevHalfWidth, true, true);
		return true;
	}

	static bool AppendMirrorPlayerSurfaces(const HWDrawInfo& di, const nri_scene::SceneView& sourceView, nri_scene::SceneView& outView)
	{
		for (const nri_scene::SurfaceRef& sourceSurface : sourceView.opaqueSprites)
		{
			outView.opaqueSprites.push_back(sourceSurface);
		}

		return !outView.opaqueSprites.empty();
	}

	static bool CaptureMirrorExtendedDynamicScene(
		HWDrawInfo& di,
		HWPortal* mirrorPortal,
		const nri_scene::SceneView* baseDynamicSceneView,
		nri_scene::SceneView& outView)
	{
		outView = {};
		if (mirrorPortal == nullptr || nri_ptmirrordynamicdistance <= 0.0f)
		{
			return false;
		}

		auto* mirrorLine = static_cast<walltype*>(mirrorPortal->GetSource());
		if (mirrorLine == nullptr)
		{
			return false;
		}

		HWDrawInfo* captureDi = HWDrawInfo::StartDrawInfo(&di, di.Viewpoint, &di.VPUniforms);
		captureDi->visibility = di.visibility;
		captureDi->rellight = di.rellight;
		if (!ApplyWallMirrorViewpoint(*mirrorLine, captureDi->Viewpoint))
		{
			captureDi->EndDrawInfo();
			return false;
		}

		captureDi->CreateScene(false);
		nri_scene::SceneView capturedView;
		const bool hasCapture = nri_scene::CaptureDynamicScene(*captureDi, capturedView);
		captureDi->EndDrawInfo();
		if (!hasCapture)
		{
			return false;
		}

		std::unordered_set<uint64_t> existingKeys;
		if (baseDynamicSceneView != nullptr)
		{
			SeedDynamicSurfaceMergeKeys(*baseDynamicSceneView, existingKeys);
		}

		outView.drawInfo = &di;
		AppendMirrorExtendedSurfaceList(
			capturedView.opaqueWalls,
			di.Viewpoint,
			nri_ptmirrordynamicdistance,
			existingKeys,
			outView.opaqueWalls);
		AppendMirrorExtendedSurfaceList(
			capturedView.opaqueFlats,
			di.Viewpoint,
			nri_ptmirrordynamicdistance,
			existingKeys,
			outView.opaqueFlats);
		AppendMirrorExtendedSurfaceList(
			capturedView.opaqueSprites,
			di.Viewpoint,
			nri_ptmirrordynamicdistance,
			existingKeys,
			outView.opaqueSprites);
		if (outView.opaqueWalls.empty() && outView.opaqueFlats.empty() && outView.opaqueSprites.empty())
		{
			outView = {};
			return false;
		}

		outView.primitiveFlags = nri_scene::PrimitiveFlag_ReflectionOnly;
		RebuildSceneViewStats(outView);
		return true;
	}

	static bool CaptureMirrorPlayerDynamicScene(HWDrawInfo& di, HWPortal* mirrorPortal, int32_t selectedMirrorWallIndex, uint32_t mirrorPortalCandidates, nri_scene::SceneView& outView)
	{
		outView = {};
		MirrorPlayerCaptureStats captureStats = {};
		captureStats.viewpointActorIndex = di.Viewpoint.CameraActor != nullptr ? (int32_t)di.Viewpoint.CameraActor->GetIndex() : -1;
		if (gi == nullptr ||
			myconnectindex < 0 ||
			myconnectindex >= MAXPLAYERS)
		{
			TraceMirrorPlayerCaptureStats(captureStats);
			return false;
		}

		DCorePlayer* localPlayer = PlayerArray[myconnectindex];
		DCoreActor* localPlayerActor = localPlayer != nullptr ? localPlayer->GetActor() : nullptr;
		if (localPlayerActor == nullptr)
		{
			TraceMirrorPlayerCaptureStats(captureStats);
			return false;
		}

		const int32_t actorIndex = (int32_t)localPlayerActor->GetIndex();
		captureStats.localPlayerActorIndex = actorIndex;
		captureStats.viewpointMatchesLocalPlayer = di.Viewpoint.CameraActor == localPlayerActor;
		captureStats.mirrorPortalCandidates = mirrorPortalCandidates;
		captureStats.selectedMirrorWallIndex = selectedMirrorWallIndex;
		HWDrawInfo* captureDi = HWDrawInfo::StartDrawInfo(&di, di.Viewpoint, &di.VPUniforms);
		captureDi->visibility = di.visibility;
		captureDi->rellight = di.rellight;
		if (mirrorPortal != nullptr)
		{
			auto* mirrorLine = static_cast<walltype*>(mirrorPortal->GetSource());
			if (mirrorLine != nullptr)
			{
				ApplyWallMirrorViewpoint(*mirrorLine, captureDi->Viewpoint);
			}
		}

		const ScopedMirrorPlayerVisibilityCaptureOverride mirrorCaptureOverride(true);
		captureDi->CreateScene(false);
		captureStats.rawFacingSprites = CountDrawListActorSprites(captureDi->drawlists[GLDL_TRANSLUCENT], actorIndex, false);
		captureStats.rawVoxelSprites = CountDrawListActorSprites(captureDi->drawlists[GLDL_MODELS], actorIndex, true);

		nri_scene::SceneView capturedView;
		const bool hasCapture = nri_scene::CaptureActorSpriteScene(*captureDi, actorIndex, capturedView);
		captureDi->EndDrawInfo();
		if (!hasCapture || !AppendMirrorPlayerSurfaces(di, capturedView, outView))
		{
			TraceMirrorPlayerCaptureStats(captureStats);
			outView = {};
			return false;
		}

		outView.drawInfo = &di;
		RebuildSceneViewStats(outView);
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
		TraceMirrorPlayerCaptureStats(captureStats);
		return true;
	}

	static bool IsMirrorPlayerPreviewCaptureEnabled()
	{
		// Phase 5 merges the captured local-player slice into the live PT dynamic
		// overlay and phase 4 marks it reflection-only, so the extra capture pass is
		// now consumed by the active mirror path instead of running as inert preview work.
		return true;
	}

	enum class ActorSpriteLiveMatchResult : uint32_t
	{
		Match = 0,
		NullLiveTexture,
		TextureMismatch,
		PaletteMismatch
	};

	struct ActorSpriteLiveMatchDetails
	{
		ActorSpriteLiveMatchResult result = ActorSpriteLiveMatchResult::Match;
		FGameTexture* liveTexture = nullptr;
		int32_t liveTextureId = -1;
		int32_t surfaceTextureId = -1;
		int32_t livePalette = 0;
		int32_t surfacePalette = 0;
	};

	static bool ShouldTraceActorSpriteVerbose()
	{
		return (int)nri_ptactorspritetrace == 1 && (int)nri_pttraceframes > 0;
	}

	static bool ShouldTraceActorSpriteMismatch()
	{
		return (int)nri_ptactorspritetrace >= 1 && (int)nri_pttraceframes > 0;
	}

	static bool ShouldTraceActorSpriteCoherency()
	{
		return (int)nri_ptactorspritetrace > 0 && (int)nri_pttraceframes > 0;
	}

	struct MaterialTextureAttributionCounts
	{
		uint32_t materialCount = 0;
		uint32_t actorMaterialCount = 0;
		uint32_t textureCount = 0;
		uint32_t baseTextureCount = 0;
		uint32_t glowTextureCount = 0;
		uint32_t normalTextureCount = 0;
		uint32_t metallicTextureCount = 0;
		uint32_t roughnessTextureCount = 0;
		uint32_t emissiveTextureCount = 0;
	};

	static bool ShouldTraceActorOverflow()
	{
		return (int)perf_looptraceframes > 0;
	}

	static constexpr uint32_t NRI_MAX_ACTOR_OVERFLOW_TRACE_LINES = 16;

	static MaterialTextureAttributionCounts GatherMaterialTextureAttribution(
		const std::vector<nri_scene::MaterialData>& materials,
		const std::vector<nri_scene::MaterialLightingMetadata>& lightMetadata,
		size_t textureCount)
	{
		MaterialTextureAttributionCounts counts = {};
		counts.materialCount = (uint32_t)materials.size();
		counts.textureCount = (uint32_t)textureCount;

		std::unordered_set<uint32_t> baseTextures;
		std::unordered_set<uint32_t> glowTextures;
		std::unordered_set<uint32_t> normalTextures;
		std::unordered_set<uint32_t> metallicTextures;
		std::unordered_set<uint32_t> roughnessTextures;
		std::unordered_set<uint32_t> emissiveTextures;
		baseTextures.reserve(materials.size());
		glowTextures.reserve(lightMetadata.size());
		normalTextures.reserve(materials.size());
		metallicTextures.reserve(materials.size());
		roughnessTextures.reserve(materials.size());
		emissiveTextures.reserve(materials.size());

		const auto addTextureIndex = [textureCount](std::unordered_set<uint32_t>& destination, uint32_t textureIndex)
		{
			if (textureIndex != UINT32_MAX && (size_t)textureIndex < textureCount)
			{
				destination.insert(textureIndex);
			}
		};

		for (uint32_t materialIndex = 0; materialIndex < (uint32_t)materials.size(); ++materialIndex)
		{
			const auto& material = materials[materialIndex];
			addTextureIndex(baseTextures, material.textureIndex);
			addTextureIndex(normalTextures, material.normalTextureIndex);
			addTextureIndex(metallicTextures, material.metallicTextureIndex);
			addTextureIndex(roughnessTextures, material.roughnessTextureIndex);
			addTextureIndex(emissiveTextures, material.emissiveTextureIndex);
			if (materialIndex < lightMetadata.size())
			{
				const auto& metadata = lightMetadata[materialIndex];
				addTextureIndex(glowTextures, metadata.glowmapTextureIndex);
				if (metadata.actorIndex >= 0)
				{
					counts.actorMaterialCount++;
				}
			}
		}

		counts.baseTextureCount = (uint32_t)baseTextures.size();
		counts.glowTextureCount = (uint32_t)glowTextures.size();
		counts.normalTextureCount = (uint32_t)normalTextures.size();
		counts.metallicTextureCount = (uint32_t)metallicTextures.size();
		counts.roughnessTextureCount = (uint32_t)roughnessTextures.size();
		counts.emissiveTextureCount = (uint32_t)emissiveTextures.size();
		return counts;
	}

	static void AccumulateMaterialTextureAttribution(NRIRenderer::MaterialBuildTraceEntry& entry, const MaterialTextureAttributionCounts& counts)
	{
		entry.materialCount += counts.materialCount;
		entry.actorMaterialCount += counts.actorMaterialCount;
		entry.textureCount += counts.textureCount;
		entry.baseTextureCount += counts.baseTextureCount;
		entry.glowTextureCount += counts.glowTextureCount;
		entry.normalTextureCount += counts.normalTextureCount;
		entry.metallicTextureCount += counts.metallicTextureCount;
		entry.roughnessTextureCount += counts.roughnessTextureCount;
		entry.emissiveTextureCount += counts.emissiveTextureCount;
	}

	static uint64_t CoherencyHashCombine64(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
	}

	static uint32_t CoherencyFloatBits(float value)
	{
		static_assert(sizeof(uint32_t) == sizeof(float), "unexpected float size");
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	static uint64_t HashDescriptorList(const nri::Descriptor* const* descriptors, size_t count)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = CoherencyHashCombine64(hash, (uint64_t)count);
		for (size_t i = 0; i < count; ++i)
		{
			hash = CoherencyHashCombine64(hash, (uint64_t)(uintptr_t)descriptors[i]);
		}
		return hash;
	}

	static uint64_t HashMaterialBridgeSummary(const nri_scene::MaterialBridgeData& materials)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = CoherencyHashCombine64(hash, (uint64_t)materials.materials.size());
		hash = CoherencyHashCombine64(hash, (uint64_t)materials.lightMetadata.size());
		hash = CoherencyHashCombine64(hash, (uint64_t)materials.textures.size());
		for (size_t i = 0; i < materials.materials.size(); ++i)
		{
			const auto& material = materials.materials[i];
			hash = CoherencyHashCombine64(hash, (uint64_t)material.textureIndex);
			hash = CoherencyHashCombine64(hash, (uint64_t)material.paletteIndex);
			hash = CoherencyHashCombine64(hash, (uint64_t)material.flags);
			hash = CoherencyHashCombine64(hash, (uint64_t)material.lightingFlags);
			hash = CoherencyHashCombine64(hash, (uint64_t)material.emissiveMode);
			hash = CoherencyHashCombine64(hash, (uint64_t)material.emissiveTextureIndex);
			hash = CoherencyHashCombine64(hash, (uint64_t)CoherencyFloatBits(material.alpha));
		}

		for (const auto& metadata : materials.lightMetadata)
		{
			hash = CoherencyHashCombine64(hash, metadata.materialKey);
			hash = CoherencyHashCombine64(hash, (uint64_t)metadata.textureId);
			hash = CoherencyHashCombine64(hash, (uint64_t)metadata.actorIndex);
			hash = CoherencyHashCombine64(hash, (uint64_t)metadata.textureIndex);
			hash = CoherencyHashCombine64(hash, (uint64_t)metadata.paletteIndex);
			hash = CoherencyHashCombine64(hash, (uint64_t)metadata.emissiveMode);
			hash = CoherencyHashCombine64(hash, (uint64_t)metadata.emissiveTextureIndex);
		}

		for (const auto& texture : materials.textures)
		{
			hash = CoherencyHashCombine64(hash, texture.key);
			hash = CoherencyHashCombine64(hash, (uint64_t)texture.width);
			hash = CoherencyHashCombine64(hash, (uint64_t)texture.height);
			hash = CoherencyHashCombine64(hash, texture.indexed ? 1ull : 0ull);
		}

		return hash;
	}

	static const char* GetActorSpriteTraceStageName(PathTracingActorSpriteTraceStage stage)
	{
		switch (stage)
		{
		case PathTracingActorSpriteTraceStage::Draw: return "draw";
		case PathTracingActorSpriteTraceStage::CaptureScene: return "capture_scene";
		case PathTracingActorSpriteTraceStage::CaptureActorScene: return "capture_actor_scene";
		default: return "unknown";
		}
	}

	static const char* GetActorSpriteLiveMatchResultName(ActorSpriteLiveMatchResult result)
	{
		switch (result)
		{
		case ActorSpriteLiveMatchResult::Match: return "match";
		case ActorSpriteLiveMatchResult::NullLiveTexture: return "null_live_texture";
		case ActorSpriteLiveMatchResult::TextureMismatch: return "texture_mismatch";
		case ActorSpriteLiveMatchResult::PaletteMismatch: return "palette_mismatch";
		default: return "unknown";
		}
	}

	static const char* GetSurfaceSourceTypeName(nri_scene::SurfaceSourceType sourceType);

	static FTextureID GetLiveActorDisplayTextureId(const DCoreActor& actor)
	{
		return actor.dispictex.isValid() ? actor.dispictex : actor.spr.spritetexture();
	}

	static FGameTexture* GetLiveActorSurfaceTexture(const DCoreActor& actor, nri_scene::SurfaceSourceType sourceType)
	{
		switch (sourceType)
		{
		case nri_scene::SurfaceSourceType::DrawListWall:
		case nri_scene::SurfaceSourceType::FacingSprite:
			return TexMan.GetGameTexture(GetLiveActorDisplayTextureId(actor));

		case nri_scene::SurfaceSourceType::VoxelProxySprite:
		{
			if (!r_voxels)
			{
				return nullptr;
			}

			const int voxelIndex = GetExtInfo(actor.spr.spritetexture()).tiletovox;
			if (voxelIndex < 0 || voxelIndex >= MAXVOXELS || voxmodels[voxelIndex] == nullptr || voxmodels[voxelIndex]->model == nullptr)
			{
				return nullptr;
			}

			return TexMan.GetGameTexture(voxmodels[voxelIndex]->model->GetPaletteTexture());
		}

		default:
			return nullptr;
		}
	}

	static bool SurfaceUsesLiveActorTextureValidation(const nri_scene::SurfaceRef& surface)
	{
		if (surface.provenance.actorIndex < 0)
		{
			return false;
		}

		switch (surface.provenance.sourceType)
		{
		case nri_scene::SurfaceSourceType::DrawListWall:
			return (surface.material.flags & nri_scene::MaterialFlag_Sprite) != 0;
		case nri_scene::SurfaceSourceType::FacingSprite:
		case nri_scene::SurfaceSourceType::VoxelProxySprite:
			return true;
		default:
			return false;
		}
	}

	static ActorSpriteLiveMatchDetails EvaluateCachedSurfaceMatchAgainstLiveActor(const nri_scene::SurfaceRef& surface, const DCoreActor& actor)
	{
		ActorSpriteLiveMatchDetails details = {};
		details.surfaceTextureId = surface.material.texture != nullptr ? surface.material.texture->GetID().GetIndex() : -1;
		details.surfacePalette = surface.material.palette;
		details.livePalette = actor.spr.pal;

		switch (surface.provenance.sourceType)
		{
		case nri_scene::SurfaceSourceType::DrawListWall:
		case nri_scene::SurfaceSourceType::FacingSprite:
		case nri_scene::SurfaceSourceType::VoxelProxySprite:
		{
			details.liveTexture = GetLiveActorSurfaceTexture(actor, surface.provenance.sourceType);
			details.liveTextureId = details.liveTexture != nullptr ? details.liveTexture->GetID().GetIndex() : -1;
			if (details.liveTexture == nullptr)
			{
				details.result = ActorSpriteLiveMatchResult::NullLiveTexture;
			}
			else if (surface.material.texture != details.liveTexture)
			{
				details.result = ActorSpriteLiveMatchResult::TextureMismatch;
			}
			else if (surface.material.palette != actor.spr.pal)
			{
				details.result = ActorSpriteLiveMatchResult::PaletteMismatch;
			}
			return details;
		}

		default:
			return details;
		}
	}

	static bool CachedSurfaceMatchesLiveActor(const nri_scene::SurfaceRef& surface, const DCoreActor& actor)
	{
		return EvaluateCachedSurfaceMatchAgainstLiveActor(surface, actor).result == ActorSpriteLiveMatchResult::Match;
	}

	static uint64_t HashPersistentSurfaceTaggedSignedValue(uint64_t hash, uint64_t tag, int32_t value)
	{
		hash = (hash ^ tag) * 1099511628211ull;
		hash = (hash ^ (uint64_t)(uint32_t)(value + 1)) * 1099511628211ull;
		return hash;
	}

	static uint64_t QuantizePersistentSurfaceCenter(const nri_scene::SurfaceRef& surface)
	{
		if (surface.vertices.empty())
		{
			return 0ull;
		}

		double center[3] = {};
		for (const auto& vertex : surface.vertices)
		{
			center[0] += vertex.position[0];
			center[1] += vertex.position[1];
			center[2] += vertex.position[2];
		}

		const double invVertexCount = 1.0 / (double)surface.vertices.size();
		const int64_t x = (int64_t)std::llround(center[0] * invVertexCount * 16.0);
		const int64_t y = (int64_t)std::llround(center[1] * invVertexCount * 16.0);
		const int64_t z = (int64_t)std::llround(center[2] * invVertexCount * 16.0);

		uint64_t key = 1469598103934665603ull;
		key = (key ^ (uint64_t)x) * 1099511628211ull;
		key = (key ^ (uint64_t)y) * 1099511628211ull;
		key = (key ^ (uint64_t)z) * 1099511628211ull;
		return key;
	}

	static uint64_t BuildPersistentEmissiveSurfaceIdentityKey(const nri_scene::SurfaceRef& surface)
	{
		uint64_t key = 1469598103934665603ull;
		key = (key ^ (uint64_t)(uint32_t)surface.provenance.sourceType) * 1099511628211ull;
		key = (key ^ (uint64_t)surface.provenance.drawListType) * 1099511628211ull;

		bool hasAuthoritativeOwnership = false;
		if (surface.provenance.actorIndex >= 0)
		{
			key = HashPersistentSurfaceTaggedSignedValue(key, 0xA11C700000000001ull, surface.provenance.actorIndex);
			hasAuthoritativeOwnership = true;
		}
		if (surface.provenance.sectorIndex >= 0)
		{
			key = HashPersistentSurfaceTaggedSignedValue(key, 0x5EC70B5E00000001ull, surface.provenance.sectorIndex);
			hasAuthoritativeOwnership = true;
		}
		if (surface.provenance.wallIndex >= 0)
		{
			key = HashPersistentSurfaceTaggedSignedValue(key, 0xAA11000000000001ull, surface.provenance.wallIndex);
			hasAuthoritativeOwnership = true;
		}
		if (surface.provenance.sectionIndex >= 0)
		{
			key = HashPersistentSurfaceTaggedSignedValue(key, 0x5EC7100000000001ull, surface.provenance.sectionIndex);
			hasAuthoritativeOwnership = true;
		}
		if (surface.provenance.mapChunkIndex >= 0)
		{
			key = HashPersistentSurfaceTaggedSignedValue(key, 0xC4C0000000000001ull, surface.provenance.mapChunkIndex);
			hasAuthoritativeOwnership = true;
		}
		if (surface.provenance.nextSectorIndex >= 0)
		{
			key = HashPersistentSurfaceTaggedSignedValue(key, 0x9E57000000000001ull, surface.provenance.nextSectorIndex);
			hasAuthoritativeOwnership = true;
		}
		if (!hasAuthoritativeOwnership)
		{
			key = (key ^ 0xCE173E0000000001ull) * 1099511628211ull;
			key = (key ^ QuantizePersistentSurfaceCenter(surface)) * 1099511628211ull;
		}

		key = (key ^ (uint64_t)(uintptr_t)surface.material.texture) * 1099511628211ull;
		key = (key ^ (uint64_t)(uint32_t)(surface.material.palette + 1)) * 1099511628211ull;
		key = (key ^ (uint64_t)surface.provenance.cstat) * 1099511628211ull;
		key = (key ^ (uint64_t)surface.provenance.materialFlags) * 1099511628211ull;
		return key;
	}

	template <typename SurfaceContainer>
	static void AppendUniquePersistentEmissiveSurfaces(
		const SurfaceContainer& source,
		SurfaceContainer& destination,
		std::unordered_set<uint64_t>& inOutSeenKeys)
	{
		for (const auto& surface : source)
		{
			const uint64_t identityKey = BuildPersistentEmissiveSurfaceIdentityKey(surface);
			if (!inOutSeenKeys.insert(identityKey).second)
			{
				continue;
			}

			destination.push_back(surface);
		}
	}

	static bool RequiresExclusiveMaterialOnlyChunkReplacement(uint32_t reasonMask)
	{
		// Material-only wall mutations leave the stale static wall traceable if
		// we only overlay the changed wall subset. Replacing the whole rebuilt
		// live chunk avoids that without dropping unrelated geometry.
		return (reasonMask & nri_scene::PTMapChunkMutationReason_WallMaterial) != 0;
	}

	static void FilterMaterialOnlyReplacementSceneView(nri_scene::SceneView& sceneView, uint32_t reasonMask)
	{
		static constexpr float kMaterialOnlyReplacementDepthNudge = 0.01f;
		const bool keepWalls = (reasonMask & nri_scene::PTMapChunkMutationReason_WallMaterial) != 0;
		const bool keepFlats = (reasonMask & nri_scene::PTMapChunkMutationReason_SectorMaterial) != 0;

		if (!keepWalls)
		{
			sceneView.opaqueWalls.clear();
		}

		if (!keepFlats)
		{
			sceneView.opaqueFlats.clear();
		}

		for (nri_scene::SurfaceRef& surface : sceneView.opaqueWalls)
		{
			NudgeCapturedSurface(surface, kMaterialOnlyReplacementDepthNudge);
		}

		for (nri_scene::SurfaceRef& surface : sceneView.opaqueFlats)
		{
			NudgeCapturedSurface(surface, kMaterialOnlyReplacementDepthNudge);
		}
	}

	static uint64_t HashLightOverlayText(uint64_t hash, const char* text)
	{
		if (text == nullptr)
		{
			return hash;
		}

		for (const unsigned char* cursor = (const unsigned char*)text; *cursor != '\0'; ++cursor)
		{
			hash ^= (uint64_t)(*cursor);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	static uint32_t BuildResolvedLightOverlayRuleId(const char* id, const char* classOrMapName, const LightOverlaySourceLocation& source)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashLightOverlayText(hash, id);
		hash = HashLightOverlayText(hash, classOrMapName);
		hash = HashLightOverlayText(hash, source.sourceName.GetChars());
		hash ^= (uint64_t)source.orderIndex + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		const uint32_t ruleId = (uint32_t)(hash ^ (hash >> 32));
		return ruleId != 0 ? ruleId : 1u;
	}

	static uint32_t BuildActorOverlayRuleId(const ResolvedLightOverlayActorRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.actorClassName.GetChars(), rule.source);
	}

	static bool IsSupportedActorOverlayRule(const ResolvedLightOverlayActorRule& rule)
	{
		return rule.lightType.IsEmpty() || rule.lightType.CompareNoCase("point") == 0;
	}

	static bool IsSupportedMapOverlayRule(const ResolvedLightOverlayMapLightRule& rule)
	{
		return rule.lightType.IsEmpty() || rule.lightType.CompareNoCase("point") == 0;
	}

	static bool IsUsableDirectionalVector(const float direction[3])
	{
		if (!std::isfinite(direction[0]) || !std::isfinite(direction[1]) || !std::isfinite(direction[2]))
		{
			return false;
		}

		const float lengthSq = direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2];
		return lengthSq > 0.000001f;
	}

	static float ClampDirectionalAngularSize(float angularSize)
	{
		if (!std::isfinite(angularSize))
		{
			return 0.03f;
		}

		return std::clamp(angularSize, 0.001f, 1.2f);
	}

	static uint64_t QuantizeDirectionalLightScalar(float value, float scale)
	{
		return (uint64_t)(int64_t)std::llround((double)value * (double)scale);
	}

	static uint32_t PackDirectionalLightColor24(const float color[3])
	{
		auto packChannel = [](float value) -> uint32_t
		{
			const float clamped = std::clamp(value, 0.0f, 8.0f);
			return (uint32_t)std::clamp((int)std::lround((double)(clamped * (255.0f / 8.0f))), 0, 255);
		};

		const uint32_t r = packChannel(color[0]);
		const uint32_t g = packChannel(color[1]);
		const uint32_t b = packChannel(color[2]);
		return r | (g << 8u) | (b << 16u);
	}

	static uint32_t PackDirectionalAngularSize16(float angularSize)
	{
		const float normalized = ClampDirectionalAngularSize(angularSize) / 1.2f;
		return (uint32_t)std::clamp((int)std::lround((double)(normalized * 65535.0f)), 0, 65535);
	}

	static uint64_t BuildDirectionalLightStateHash(const NRIDirectionalLightState& state)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombineLightOverlay(hash, state.enabled ? 1ull : 0ull);
		hash = HashCombineLightOverlay(hash, state.shadow ? 1ull : 0ull);
		hash = HashCombineLightOverlay(hash, state.fromOverlay ? 1ull : 0ull);
		hash = HashCombineLightOverlay(hash, (uint64_t)state.ruleId);
		hash = HashCombineLightOverlay(hash, QuantizeDirectionalLightScalar(state.direction[0], 4096.0f));
		hash = HashCombineLightOverlay(hash, QuantizeDirectionalLightScalar(state.direction[1], 4096.0f));
		hash = HashCombineLightOverlay(hash, QuantizeDirectionalLightScalar(state.direction[2], 4096.0f));
		hash = HashCombineLightOverlay(hash, QuantizeDirectionalLightScalar(state.color[0], 4096.0f));
		hash = HashCombineLightOverlay(hash, QuantizeDirectionalLightScalar(state.color[1], 4096.0f));
		hash = HashCombineLightOverlay(hash, QuantizeDirectionalLightScalar(state.color[2], 4096.0f));
		hash = HashCombineLightOverlay(hash, QuantizeDirectionalLightScalar(state.angularSize, 4096.0f));
		return hash;
	}

	static const char* GetDirectionalLightSourceName(const NRIDirectionalLightState& state)
	{
		if (!state.enabled)
		{
			return "off";
		}

		return state.fromOverlay ? "overlay" : "default";
	}

	static NRIDirectionalLightState BuildDirectionalLightState(const ResolvedLightOverlaySet& resolved, bool directionalLightEnabled)
	{
		NRIDirectionalLightState state = {};
		state.enabled = directionalLightEnabled;
		state.shadow = true;

		if (resolved.directionalRules.Size() > 0)
		{
			const ResolvedLightOverlayDirectionalRule& rule = resolved.directionalRules.Last();
			state.fromOverlay = true;
			state.ruleId = BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.mapName.GetChars(), rule.source);
			state.enabled = directionalLightEnabled;
			state.shadow = !rule.hasShadow || rule.shadow;

			if (rule.hasDirection && IsUsableDirectionalVector(rule.direction))
			{
				state.direction[0] = rule.direction[0];
				state.direction[1] = rule.direction[1];
				state.direction[2] = rule.direction[2];
				const float invLength = 1.0f / sqrtf(
					state.direction[0] * state.direction[0] +
					state.direction[1] * state.direction[1] +
					state.direction[2] * state.direction[2]);
				state.direction[0] *= invLength;
				state.direction[1] *= invLength;
				state.direction[2] *= invLength;
			}
			else
			{
				state.enabled = false;
				state.shadow = false;
			}

			if (rule.hasColor)
			{
				state.color[0] = std::max(rule.color[0], 0.0f);
				state.color[1] = std::max(rule.color[1], 0.0f);
				state.color[2] = std::max(rule.color[2], 0.0f);
			}

			const float intensity = rule.hasIntensity ? std::max(rule.intensity, 0.0f) : 1.0f;
			state.color[0] *= intensity;
			state.color[1] *= intensity;
			state.color[2] *= intensity;
			if (intensity <= 0.0f)
			{
				state.enabled = false;
				state.shadow = false;
			}

			if (rule.hasAngularSize)
			{
				state.angularSize = ClampDirectionalAngularSize(rule.angularSize);
			}
		}

		if (!state.enabled)
		{
			state.color[0] = 0.0f;
			state.color[1] = 0.0f;
			state.color[2] = 0.0f;
		}

		state.stateHash = BuildDirectionalLightStateHash(state);
		return state;
	}

	enum ActorMaterialOverrideBits : uint32_t
	{
		ActorMaterialOverride_None = 0,
		ActorMaterialOverride_NoShadowReceive = 1u << 0,
		ActorMaterialOverride_NoShadowCast = 1u << 1,
		ActorMaterialOverride_Fullbright = 1u << 2,
	};

	static void BuildActorMaterialOverrideMap(const ResolvedLightOverlaySet& resolved, std::unordered_map<int32_t, uint32_t>& outOverrides)
	{
		if (resolved.actorRules.Size() == 0 && resolved.actorOverrideRules.Size() == 0)
		{
			return;
		}

		TSpriteIterator<DCoreActor> it;
		while (auto actor = it.Next())
		{
			if (actor == nullptr || !actor->exists() || (actor->ObjectFlags & OF_EuthanizeMe) != 0)
			{
				continue;
			}

			PClass* actorClass = actor->GetClass();
			if (actorClass == nullptr)
			{
				continue;
			}

			uint32_t overrideBits = ActorMaterialOverride_None;
			bool touched = false;
			const uint32_t actorTextureId = (unsigned)actor->spr.picnum < MAXTILES ? (uint32_t)tileGetTextureID(actor->spr.picnum).GetIndex() : 0u;
			for (const auto& resolvedRule : resolved.actorRules)
			{
				if (!resolvedRule.actorClassResolved ||
					resolvedRule.actorClass == nullptr ||
					(actorClass != resolvedRule.actorClass && !actorClass->IsDescendantOf(resolvedRule.actorClass)))
				{
					continue;
				}

				if (resolvedRule.hasTileFilter && actorTextureId != (uint32_t)resolvedRule.tileFilter)
				{
					continue;
				}

				if (resolvedRule.hasShadowReceive)
				{
					touched = true;
					if (resolvedRule.shadowReceive)
					{
						overrideBits &= ~ActorMaterialOverride_NoShadowReceive;
					}
					else
					{
						overrideBits |= ActorMaterialOverride_NoShadowReceive;
					}
				}

				if (resolvedRule.hasShadowCast)
				{
					touched = true;
					if (resolvedRule.shadowCast)
					{
						overrideBits &= ~ActorMaterialOverride_NoShadowCast;
					}
					else
					{
						overrideBits |= ActorMaterialOverride_NoShadowCast;
					}
				}

				if (resolvedRule.hasFullbright)
				{
					touched = true;
					if (resolvedRule.fullbright)
					{
						overrideBits |= ActorMaterialOverride_Fullbright;
					}
					else
					{
						overrideBits &= ~ActorMaterialOverride_Fullbright;
					}
				}
			}

			for (const auto& resolvedRule : resolved.actorOverrideRules)
			{
				if (!resolvedRule.actorClassResolved ||
					resolvedRule.actorClass == nullptr ||
					(actorClass != resolvedRule.actorClass && !actorClass->IsDescendantOf(resolvedRule.actorClass)))
				{
					continue;
				}

				if (resolvedRule.hasShadowReceive)
				{
					touched = true;
					if (resolvedRule.shadowReceive)
					{
						overrideBits &= ~ActorMaterialOverride_NoShadowReceive;
					}
					else
					{
						overrideBits |= ActorMaterialOverride_NoShadowReceive;
					}
				}

				if (resolvedRule.hasShadowCast)
				{
					touched = true;
					if (resolvedRule.shadowCast)
					{
						overrideBits &= ~ActorMaterialOverride_NoShadowCast;
					}
					else
					{
						overrideBits |= ActorMaterialOverride_NoShadowCast;
					}
				}
			}

			if (touched && overrideBits != ActorMaterialOverride_None)
			{
				outOverrides[(int32_t)actor->GetIndex()] = overrideBits;
			}
		}
	}

	static bool HasActorFullbrightOverrides(const ResolvedLightOverlaySet& resolved)
	{
		for (const auto& rule : resolved.actorRules)
		{
			if (rule.hasFullbright)
			{
				return true;
			}
		}
		return false;
	}

	static uint32_t BuildMapOverlayRuleId(const ResolvedLightOverlayMapLightRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.mapName.GetChars(), rule.source);
	}

	static uint64_t BuildMapOverlayStableKey(uint32_t ruleId, const float position[3])
	{
		uint64_t key = 1469598103934665603ull;
		key = HashCombineLightOverlay(key, (uint64_t)ruleId);
		key = HashCombineLightOverlay(key, QuantizeLightOverlayPositionKey(position));
		return key;
	}

	static bool TryResolveSectorMapOverlayAnchorPosition(const nri_scene::PTMapWorld& mapWorld, int32_t sectorIndex, float outPosition[3])
	{
		const nri_scene::PTMapChunk* matchedChunk = nullptr;
		for (const auto& chunk : mapWorld.chunks)
		{
			if (chunk.sectorIndex == sectorIndex)
			{
				matchedChunk = &chunk;
				break;
			}
		}
		if (matchedChunk == nullptr)
		{
			return false;
		}

		float flatCenterSum[3] = {};
		int flatCenterCount = 0;
		float anyCenterSum[3] = {};
		int anyCenterCount = 0;
		const uint32_t endSurface = matchedChunk->firstSurface + matchedChunk->surfaceCount;
		for (uint32_t surfaceIndex = matchedChunk->firstSurface; surfaceIndex < endSurface && surfaceIndex < mapWorld.surfaces.size(); ++surfaceIndex)
		{
			const auto& surface = mapWorld.surfaces[surfaceIndex].surface;
			if (surface.provenance.sectorIndex != sectorIndex)
			{
				continue;
			}

			float center[3] = {};
			ComputeCapturedSurfaceCenter(surface, center);
			anyCenterSum[0] += center[0];
			anyCenterSum[1] += center[1];
			anyCenterSum[2] += center[2];
			anyCenterCount++;

			if (surface.provenance.sourceType == nri_scene::SurfaceSourceType::MapFloorSection ||
				surface.provenance.sourceType == nri_scene::SurfaceSourceType::MapCeilingSection)
			{
				flatCenterSum[0] += center[0];
				flatCenterSum[1] += center[1];
				flatCenterSum[2] += center[2];
				flatCenterCount++;
			}
		}

		const float* sum = flatCenterCount > 0 ? flatCenterSum : anyCenterSum;
		const int count = flatCenterCount > 0 ? flatCenterCount : anyCenterCount;
		if (count <= 0)
		{
			return false;
		}

		const float invCount = 1.0f / (float)count;
		outPosition[0] = sum[0] * invCount;
		outPosition[1] = sum[1] * invCount;
		outPosition[2] = sum[2] * invCount;
		return true;
	}

	static bool TryResolveWallMapOverlayAnchorPosition(const nri_scene::PTMapWorld& mapWorld, int32_t wallIndex, float outPosition[3])
	{
		float centerSum[3] = {};
		int centerCount = 0;
		for (const auto& mapSurface : mapWorld.surfaces)
		{
			if (mapSurface.surface.provenance.wallIndex != wallIndex)
			{
				continue;
			}

			float center[3] = {};
			ComputeCapturedSurfaceCenter(mapSurface.surface, center);
			centerSum[0] += center[0];
			centerSum[1] += center[1];
			centerSum[2] += center[2];
			centerCount++;
		}

		if (centerCount <= 0)
		{
			return false;
		}

		const float invCount = 1.0f / (float)centerCount;
		outPosition[0] = centerSum[0] * invCount;
		outPosition[1] = centerSum[1] * invCount;
		outPosition[2] = centerSum[2] * invCount;
		return true;
	}

	static bool TryResolveMapOverlayAnchorPosition(const nri_scene::PTMapWorld& mapWorld, const ResolvedLightOverlayMapLightRule& rule, float outPosition[3])
	{
		switch (rule.anchorType)
		{
		case LightOverlayAnchorType::Position:
			if (!rule.hasAnchorPosition)
			{
				return false;
			}
			outPosition[0] = rule.anchorPosition[0];
			outPosition[1] = rule.anchorPosition[1];
			outPosition[2] = rule.anchorPosition[2];
			return true;

		case LightOverlayAnchorType::Sector:
			return rule.anchorIndex >= 0 && TryResolveSectorMapOverlayAnchorPosition(mapWorld, rule.anchorIndex, outPosition);

		case LightOverlayAnchorType::Wall:
			return rule.anchorIndex >= 0 && TryResolveWallMapOverlayAnchorPosition(mapWorld, rule.anchorIndex, outPosition);

		default:
			return false;
		}
	}

	static void BuildActorAnalyticOverlayRules(
		const ResolvedLightOverlaySet& resolved,
		std::unordered_map<int32_t, std::vector<SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule>>& outRules)
	{
		if (resolved.actorRules.Size() == 0)
		{
			return;
		}

		TSpriteIterator<DCoreActor> it;
		while (auto actor = it.Next())
		{
			if (actor == nullptr ||
				!actor->exists() ||
				(actor->ObjectFlags & OF_EuthanizeMe) != 0)
			{
				continue;
			}

			PClass* actorClass = actor->GetClass();
			if (actorClass == nullptr)
			{
				continue;
			}

			auto& actorRules = outRules[(int32_t)actor->GetIndex()];
			for (const auto& resolvedRule : resolved.actorRules)
			{
				if (!resolvedRule.actorClassResolved ||
					resolvedRule.actorClass == nullptr ||
					!IsSupportedActorOverlayRule(resolvedRule) ||
					resolvedRule.intensity <= 0.0f ||
					resolvedRule.radius <= 0.0f ||
					(actorClass != resolvedRule.actorClass && !actorClass->IsDescendantOf(resolvedRule.actorClass)))
				{
					continue;
				}

				SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule actorRule = {};
				actorRule.ruleId = BuildActorOverlayRuleId(resolvedRule);
				actorRule.hasTileFilter = resolvedRule.hasTileFilter;
				actorRule.tileFilter = resolvedRule.hasTileFilter && resolvedRule.tileFilter >= 0 ? (uint32_t)resolvedRule.tileFilter : 0u;
				actorRule.color[0] = resolvedRule.color[0];
				actorRule.color[1] = resolvedRule.color[1];
				actorRule.color[2] = resolvedRule.color[2];
				actorRule.intensity = resolvedRule.intensity;
				actorRule.radius = resolvedRule.radius;
				actorRule.offset[0] = resolvedRule.offset[0];
				actorRule.offset[1] = resolvedRule.offset[1];
				actorRule.offset[2] = resolvedRule.offset[2];
				actorRule.hasNudgeFromSurface = resolvedRule.hasNudgeFromSurface && resolvedRule.nudgeFromSurfaceDistance > 0.0f;
				actorRule.nudgeFromSurfaceDistance = resolvedRule.nudgeFromSurfaceDistance;
				actorRule.flickerFrames = resolvedRule.flickerFrames;
				actorRule.hasRandomIntensity = resolvedRule.hasRandom;
				actorRule.randomIntensityRange[0] = resolvedRule.randomIntensityRange[0];
				actorRule.randomIntensityRange[1] = resolvedRule.randomIntensityRange[1];
				actorRules.push_back(actorRule);
			}

			if (actorRules.empty())
			{
				outRules.erase((int32_t)actor->GetIndex());
			}
		}
	}

	static void BuildStaticMapAnalyticOverlayRules(
		const ResolvedLightOverlaySet& resolved,
		const nri_scene::PTMapWorld& mapWorld,
		std::vector<SceneLightSystem::AnalyticLightRegistry::MapOverlayRule>& outRules)
	{
		for (const auto& resolvedRule : resolved.mapLightRules)
		{
			if (!IsSupportedMapOverlayRule(resolvedRule) ||
				resolvedRule.intensity <= 0.0f ||
				resolvedRule.radius <= 0.0f)
			{
				continue;
			}

			float anchorPosition[3] = {};
			if (!TryResolveMapOverlayAnchorPosition(mapWorld, resolvedRule, anchorPosition))
			{
				continue;
			}

			SceneLightSystem::AnalyticLightRegistry::MapOverlayRule overlayRule = {};
			overlayRule.ruleId = BuildMapOverlayRuleId(resolvedRule);
			overlayRule.source = SceneLightRecordSource::StaticMapScene;
			overlayRule.position[0] = anchorPosition[0] + resolvedRule.offset[0];
			overlayRule.position[1] = anchorPosition[1] + resolvedRule.offset[1];
			overlayRule.position[2] = anchorPosition[2] + resolvedRule.offset[2];
			overlayRule.stableKey = BuildMapOverlayStableKey(overlayRule.ruleId, overlayRule.position);
			overlayRule.color[0] = resolvedRule.color[0];
			overlayRule.color[1] = resolvedRule.color[1];
			overlayRule.color[2] = resolvedRule.color[2];
			overlayRule.intensity = resolvedRule.intensity;
			overlayRule.radius = resolvedRule.radius;
			overlayRule.flickerFrames = resolvedRule.flickerFrames;
			outRules.push_back(overlayRule);
		}
	}

	static bool ResolveSurfaceProbeTextureDebugInfo(uint32_t textureId, FString& outTextureName, int32_t& outLegacyTile)
	{
		outTextureName = "(none)";
		outLegacyTile = -1;
		if (textureId == 0)
		{
			return false;
		}

		auto texture = TexMan.GameByIndex((int)textureId);
		if (texture == nullptr)
		{
			return false;
		}

		outTextureName = texture->GetName();
		if (textureId >= (uint32_t)firstarttile && textureId <= (uint32_t)(firstarttile + maxarttile))
		{
			outLegacyTile = legacyTileNum(FSetTextureID((int)textureId));
		}
		return true;
	}

	static void RefreshActiveFrameGenerationSwapChain()
	{
		if (screen != nullptr && screen->Backend() == 4)
		{
			static_cast<NRIRenderDevice*>(screen)->SetVSync(vid_vsync);
		}
	}

	static void NotifyActiveGlowControlChange()
	{
		if (screen != nullptr && screen->Backend() == 4)
		{
			static_cast<NRIRenderDevice*>(screen)->NotifyPathTracingGlowControlChange();
		}
	}

	static void NotifyActiveMaterialLightingCalibrationChange()
	{
		if (screen != nullptr && screen->Backend() == 4)
		{
			static_cast<NRIRenderDevice*>(screen)->NotifyPathTracingMaterialLightingCalibrationChange();
		}
	}

	static void NotifyActiveDebugSphereTessellationChange()
	{
		if (screen != nullptr && screen->Backend() == 4)
		{
			static_cast<NRIRenderDevice*>(screen)->NotifyPathTracingDebugSphereTessellationChange();
		}
	}

	static uint32_t GetRuntimeDebugSphereLongitudeSegments()
	{
		return (uint32_t)clamp<int>(nri_ptspherelongs, 8, 256);
	}

	static uint32_t GetRuntimeDebugSphereLatitudeSegments()
	{
		return (uint32_t)clamp<int>(nri_ptspherelats, 4, 128);
	}

	static uint32_t GetRuntimeDebugSphereTriangleCount()
	{
		return GetRuntimeDebugSphereLongitudeSegments() * 2u * (GetRuntimeDebugSphereLatitudeSegments() - 1u);
	}

	static void PathTracingToWorldPosition(const float source[3], float destination[3])
	{
		destination[0] = source[0];
		destination[1] = -source[2];
		destination[2] = -source[1];
	}
}

CUSTOM_CVAR(Bool, nri_framegen, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	RefreshActiveFrameGenerationSwapChain();
}

CUSTOM_CVAR(Int, nri_ptspherelongs, 64, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 8)
	{
		self = 8;
		return;
	}
	else if (self > 256)
	{
		self = 256;
		return;
	}
	NotifyActiveDebugSphereTessellationChange();
}

CUSTOM_CVAR(Int, nri_ptspherelats, 32, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 4)
	{
		self = 4;
		return;
	}
	else if (self > 128)
	{
		self = 128;
		return;
	}
	NotifyActiveDebugSphereTessellationChange();
}

CUSTOM_CVAR(Int, nri_framegenprovider, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 1)
	{
		self = 1;
	}

	RefreshActiveFrameGenerationSwapChain();
}
CUSTOM_CVAR(Int, nri_framegenui, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 3)
	{
		self = 3;
	}
}
CVAR(Bool, nri_framegenasync, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Bool, nri_framegenlatency, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	RefreshActiveFrameGenerationSwapChain();
}
CVAR(Int, nri_nrdmaxframes, 31, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_nrdfastframes, 7, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_nrdstabilizationframes, 31, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_nrdantifirefly, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_nrdhitdistrecon, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_nrdsplit, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_nrdfasthistorysigma, 1.25f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_nrdprepassdiffuse, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_nrdprepassspecular, 4.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_nrdblurmin, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_nrdblurmax, 4.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_nrdsigmastabilization, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_nrdsigmaplanedistance, 0.01f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_apivalidation, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_dred, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptbootstrap, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptbootstrapmode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptdirectscene, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptdirectionallight, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptlightbounces, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptmirrorbounces, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Float, nri_ptmirrordynamicdistance, 2048.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}
CVAR(Int, nri_ptsurfaceprobe, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_pttemporaltrace, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptscenestats, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptmutationtracechunk, -1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptmutationtracesector, -1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptruntimelinktrace, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptrebaselinetrace, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptemissiveheuristics, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptemissiveautoonly, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptemissiveminpower, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptemissiveminsurface, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Float, nri_ptglowscale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	NotifyActiveGlowControlChange();
}
CUSTOM_CVAR(Float, nri_ptglowreach, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	NotifyActiveGlowControlChange();
}
CUSTOM_CVAR(Float, nri_ptglowfalloff, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.25f)
	{
		self = 0.25f;
	}
	else if (self > 4.0f)
	{
		self = 4.0f;
	}
	NotifyActiveGlowControlChange();
}
CUSTOM_CVAR(Float, nri_ptfullbrightboost, 2.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.50f)
	{
		self = 0.50f;
	}
	else if (self > 8.00f)
	{
		self = 8.00f;
	}
	NotifyActiveMaterialLightingCalibrationChange();
}
CVAR(Bool, nri_ptemissivetlas, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptemissivefastshadow, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptemissivesamples, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptsectorlighting, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Float, nri_ptsectorlightmultiplier, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}
CVAR(Float, nri_ptsectorambientscale, 0.20f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsectorhemiscale, 0.12f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsectorfogscale, 0.20f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsectorclamp, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsectorfilterpal, -1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsectorfilterminshade, -128, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsectorfiltermaxshade, 127, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsectorfilterlotag, -1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsectorpulseframes, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsectorpulseamount, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptvisiblechunkgate, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
EXTERN_CVAR(String, nri_api)
EXTERN_CVAR(Int, nri_ptportaldepth)
EXTERN_CVAR(Int, nri_pttraceframes)

namespace
{
	constexpr uint32_t NRI_MAX_SCENE_TEXTURES = 512;
	constexpr uint32_t NRI_SCENE_DESCRIPTOR_NUM = 2 + NRI_MAX_SCENE_TEXTURES;
	constexpr uint32_t NRI_SCENE_DATA_DESCRIPTOR_NUM = 21;
	constexpr uint32_t NRI_INPUT_DESCRIPTOR_NUM = 14;
	constexpr uint32_t NRI_OUTPUT_DESCRIPTOR_NUM = 15;
	constexpr uint32_t NRI_MAX_RUNTIME_POINT_LIGHTS = 64;
	constexpr uint32_t NRI_MAX_EMISSIVE_SURFACES = 4096;
	constexpr uint32_t NRI_MAX_EMISSIVE_PRIMITIVES = 16384;
	constexpr uint32_t NRI_RUNTIME_LIGHT_TILE_SIZE = 64;
	constexpr uint32_t NRI_PTDEBUG_ANALYTIC_DIRECT = 26;
	constexpr uint32_t NRI_PTDEBUG_EMISSIVE_TAGS = 27;
	constexpr uint32_t NRI_PTDEBUG_EMISSIVE_DIRECT = 28;
	constexpr uint32_t NRI_PTDEBUG_SECTOR_AMBIENT = 29;

	struct NriSceneTextureLimitValidation
	{
		uint32_t requiredSceneTextureCap = NRI_MAX_SCENE_TEXTURES;
		uint32_t requiredSceneTextureSetDescriptors = NRI_SCENE_DESCRIPTOR_NUM;
		uint32_t requiredStageTextureDescriptors = NRI_SCENE_DESCRIPTOR_NUM + NRI_INPUT_DESCRIPTOR_NUM;
		bool descriptorSetTextureLimitOk = false;
		bool descriptorSetUpdateAfterSetTextureLimitOk = false;
		bool shaderStageTextureLimitOk = false;
		bool shaderStageUpdateAfterSetTextureLimitOk = false;
	};

	static NriSceneTextureLimitValidation ValidateSceneTextureDescriptorLimits(const nri::DeviceDesc& deviceDesc)
	{
		NriSceneTextureLimitValidation validation = {};
		validation.descriptorSetTextureLimitOk =
			deviceDesc.descriptorSet.textureMaxNum >= validation.requiredSceneTextureSetDescriptors;
		validation.descriptorSetUpdateAfterSetTextureLimitOk =
			deviceDesc.descriptorSet.updateAfterSet.textureMaxNum >= validation.requiredSceneTextureSetDescriptors;
		validation.shaderStageTextureLimitOk =
			deviceDesc.shaderStage.descriptorTextureMaxNum >= validation.requiredStageTextureDescriptors;
		validation.shaderStageUpdateAfterSetTextureLimitOk =
			deviceDesc.shaderStage.updateAfterSet.descriptorTextureMaxNum >= validation.requiredStageTextureDescriptors;
		return validation;
	}

	static const char* GetSceneTextureDescriptorLimitFailureReason(const nri::DeviceDesc& deviceDesc)
	{
		const NriSceneTextureLimitValidation validation = ValidateSceneTextureDescriptorLimits(deviceDesc);
		if (!validation.descriptorSetTextureLimitOk)
		{
			return "descriptor-set texture limit is below the NRI PT 1024-scene-texture requirement";
		}
		if (!validation.descriptorSetUpdateAfterSetTextureLimitOk)
		{
			return "update-after-set texture limit is below the NRI PT 1024-scene-texture requirement";
		}
		if (!validation.shaderStageTextureLimitOk)
		{
			return "per-stage texture descriptor limit is below the NRI PT 1024-scene-texture requirement";
		}
		if (!validation.shaderStageUpdateAfterSetTextureLimitOk)
		{
			return "per-stage update-after-set texture descriptor limit is below the NRI PT 1024-scene-texture requirement";
		}
		return nullptr;
	}

	static void LogSceneTextureDescriptorLimits(const nri::DeviceDesc& deviceDesc)
	{
		const NriSceneTextureLimitValidation validation = ValidateSceneTextureDescriptorLimits(deviceDesc);
		Printf(
			"NRI PT scene texture cap: cap=%u scene_set=%u stage_textures=%u limits=set:%u set_uas:%u stage:%u stage_uas:%u supported=%s\n",
			validation.requiredSceneTextureCap,
			validation.requiredSceneTextureSetDescriptors,
			validation.requiredStageTextureDescriptors,
			deviceDesc.descriptorSet.textureMaxNum,
			deviceDesc.descriptorSet.updateAfterSet.textureMaxNum,
			deviceDesc.shaderStage.descriptorTextureMaxNum,
			deviceDesc.shaderStage.updateAfterSet.descriptorTextureMaxNum,
			GetSceneTextureDescriptorLimitFailureReason(deviceDesc) == nullptr ? "yes" : "no");
	}
	constexpr uint32_t NRI_PTDEBUG_EMISSIVE_SAMPLE_VISIBILITY = 33;
	constexpr uint32_t NRI_PTDEBUG_UPSCALER_TRACE_TRANSPARENT = 34;
	constexpr uint32_t NRI_PTDEBUG_TAA_PRE_EXPOSED_INPUT = 45;
	constexpr uint32_t NRI_SCENE_DATA_SOURCE_STATIC = 0;
	constexpr uint32_t NRI_SCENE_DATA_SOURCE_DYNAMIC = 1;
	constexpr uint32_t NRI_SURFACE_PROBE_OWNER_UNKNOWN = 0;
	constexpr uint32_t NRI_SURFACE_PROBE_OWNER_STATIC_MAP = 1;
	constexpr uint32_t NRI_SURFACE_PROBE_OWNER_CAPTURED_SCENE = 2;
	constexpr uint32_t NRI_SURFACE_PROBE_OWNER_RUNTIME_LINK = 3;
	constexpr uint32_t NRI_SURFACE_PROBE_OWNER_RUNTIME_MUTATION = 4;
	constexpr uint32_t NRI_SURFACE_PROBE_OWNER_DYNAMIC_OVERLAY = 5;
	constexpr uint32_t NRI_SAMPLER_DESCRIPTOR_NUM = 4;
	constexpr uint32_t NRI_FLAG_RESET_HISTORY = 0x1u;
	constexpr uint32_t NRI_FLAG_USE_UPSCALED = 0x2u;
	constexpr uint32_t NRI_FLAG_BOOTSTRAP_VIEW = 0x4u;
	constexpr uint32_t NRI_FLAG_PRESENT_RAW_TRACE = 0x8u;
	constexpr uint32_t NRI_FLAG_RAW_PRESENT_ADD_SECONDARY = 0x10u;
	constexpr uint32_t NRI_PRESENT_FLAG_SPLIT_SHADOW_DENOISER = 0x20u;
	constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_DISPLAY_INFO_AVAILABLE = 0x1u;
	constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_DISPLAY_HDR_SUPPORTED = 0x2u;
	constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_HDR_SWAPCHAIN_ACTIVE = 0x4u;
	constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_OFFSCREEN_HDR_TARGET = 0x8u;
	constexpr uint32_t NRI_FLAG_SPLIT_SHADOW_DENOISER = 0x20u;
	constexpr uint32_t NRI_FLAG_USE_JITTER = 0x40u;
	constexpr uint32_t NRI_FLAG_DIRECTIONAL_LIGHT = 0x80u;
	constexpr uint32_t NRI_FLAG_FAST_EMISSIVE_SHADOW = 0x100u;
	constexpr uint32_t NRI_FLAG_GATE_PRIMARY_VISIBLE_CHUNKS = 0x200u;
	constexpr uint32_t NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW = 0x400u;
	constexpr int NRI_TEMPORAL_TRACE_REARM_FRAME_COUNT = 8;
	constexpr uint32_t NRI_TAA_JITTER_PHASE_COUNT = 8;
	constexpr float NRI_TAA_EXPOSURE_RESET_THRESHOLD_STOPS = 0.5f;
	constexpr uint32_t NRI_PORTAL_FLAG_RUNTIME_BOUND = 0x1u;

	enum class NRIPresentRouteKind
	{
		BootstrapFinal,
		ResolvedBeauty,
		ComposedDebug,
		UpscalerTraceTransparentProbe,
		ValidationRaw,
		DenoisedRaw,
		ShadowFinal,
		FinalDebug,
		RawTraceDebug,
		FallbackFinal,
	};

	struct NRIPresentRouteInfo
	{
		NRIPresentRouteKind kind = NRIPresentRouteKind::FallbackFinal;
		const char* routeName = "fallback_final";
		const char* presenterName = "Final";
		const char* ownerName = "fallback";
	};
	constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_NONE = 0u;
	constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_REFLECTIVE = 1u;
	constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER = 2u;
	constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_RUNTIME_BOUND = 3u;
	constexpr uint32_t NRI_EMISSIVE_SAMPLING_FLAG_AUTO_ONLY = 0x1u;
	constexpr uint32_t NRI_SECTOR_LIGHTING_FLAG_ENABLED = 0x1u;

	struct RendererSkyPerfTraceStats
	{
		uint32_t ensureSceneTexturesCalls = 0;
		uint32_t ensureSceneTexturesPreserveTrueCalls = 0;
		uint32_t ensureSceneTexturesPreserveFalseCalls = 0;
		uint32_t ensureSkyCalls = 0;
		uint32_t preserveExistingHits = 0;
		uint32_t reuseActiveCubemapHits = 0;
		uint32_t probeAttempts = 0;
		uint32_t probeSuccesses = 0;
		uint32_t reuseActiveProbeHits = 0;
		uint32_t activateCachedCubemapHits = 0;
		uint32_t createCachedCubemapHits = 0;
		uint32_t keepLastCubemapHits = 0;
		uint32_t holdLevelCubemapHits = 0;
		uint32_t solidReuseHits = 0;
		uint32_t solidActivateHits = 0;
		uint32_t solidCreateHits = 0;
		uint32_t probeFaceCalls = 0;
		uint32_t buildCubemapUploadCalls = 0;
		uint32_t residentStaticSceneTextureBuilds = 0;
		uint32_t combinedOverlayTextureBuilds = 0;
		uint32_t lightingInvalidationRequests = 0;
		uint32_t lightingInvalidationsApplied = 0;
		uint32_t emissiveMaterialDirtyEvents = 0;
		uint64_t ensureSkyTimeUs = 0;
		uint64_t probeCubemapTimeUs = 0;
		uint64_t probeFaceTimeUs = 0;
		uint64_t buildCubemapUploadTimeUs = 0;
	};

	RendererSkyPerfTraceStats gRendererSkyPerfTraceStats = {};

	bool ShouldTraceSkyPerf()
	{
		return !!nri_pttemporaltrace && nri_pttraceframes > 0;
	}

	bool ShouldEmitTemporalTraceLogs()
	{
		return !!nri_pttemporaltrace && nri_pttraceframes > 0;
	}

	bool ShouldTraceRuntimeMutationRebaseline()
	{
		return !!nri_ptrebaselinetrace;
	}

	bool ShouldTracePtPerf()
	{
		return PerfLoopTraceActive() || ShouldEmitTemporalTraceLogs();
	}

	std::string FormatTopologyKeyList(const std::vector<uint64_t>& keys, size_t limit = 8)
	{
		if (keys.empty())
		{
			return "none";
		}

		std::string result;
		const size_t printCount = std::min(keys.size(), limit);
		char buffer[32] = {};
		for (size_t i = 0; i < printCount; ++i)
		{
			if (!result.empty())
			{
				result += ",";
			}

			std::snprintf(buffer, sizeof(buffer), "0x%016llx", (unsigned long long)keys[i]);
			result += buffer;
		}

		if (printCount < keys.size())
		{
			result += ",...";
		}

		return result;
	}

	std::string BuildNormalizedMuzzleFlashEventKey(const FString& eventId)
	{
		if (eventId.IsEmpty())
		{
			return {};
		}

		const FString normalizedId = eventId.MakeLower();
		return std::string(normalizedId.GetChars());
	}

	std::string FormatMuzzleFlashRuleIdList(const std::unordered_map<std::string, ResolvedLightOverlayMuzzleFlashRule>& lookup, size_t limit = 16)
	{
		if (lookup.empty())
		{
			return "none";
		}

		std::vector<std::string> ids;
		ids.reserve(lookup.size());
		for (const auto& entry : lookup)
		{
			ids.push_back(entry.second.id.GetChars());
		}

		std::sort(ids.begin(), ids.end());

		std::string result;
		const size_t printCount = std::min(ids.size(), limit);
		for (size_t i = 0; i < printCount; ++i)
		{
			if (!result.empty())
			{
				result += ",";
			}

			result += ids[i];
		}

		if (printCount < ids.size())
		{
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), ",...(+%u)", (unsigned)(ids.size() - printCount));
			result += buffer;
		}

		return result;
	}

	double GetCurrentGameplayTimeSeconds()
	{
		return PlayClock > 0 ? (double)PlayClock * BuildTickSeconds : 0.0;
	}

	void WorldToPathTracingPosition(const DVector3& worldPos, float out[3])
	{
		out[0] = (float)worldPos.X;
		out[1] = (float)-worldPos.Z;
		out[2] = (float)-worldPos.Y;
	}

	uint32_t BuildMuzzleFlashRuleId(const ResolvedLightOverlayMuzzleFlashRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), "", rule.source);
	}

	uint64_t BuildMuzzleFlashRandomSeed(const PathTracingWeaponLightEvent& event)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombineLightOverlay(hash, event.serial);
		hash = HashCombineLightOverlay(hash, (uint64_t)(uint32_t)(event.hasEmitterActorIndex ? event.emitterActorIndex + 1 : 0));
		hash = HashLightOverlayText(hash, BuildNormalizedMuzzleFlashEventKey(event.eventId).c_str());
		return hash;
	}

	uint64_t AdvanceMuzzleFlashRandomState(uint64_t& state)
	{
		state += 0x9e3779b97f4a7c15ull;
		uint64_t z = state;
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
		return z ^ (z >> 31);
	}

	float NextMuzzleFlashUnitRandom(uint64_t& state)
	{
		const uint64_t bits = AdvanceMuzzleFlashRandomState(state);
		return (float)((bits >> 40) & 0xFFFFFFu) * (1.0f / 16777215.0f);
	}

	float ResolveMuzzleFlashRandomRange(uint64_t& randomState, float minValue, float maxValue)
	{
		if (!std::isfinite(minValue) || !std::isfinite(maxValue))
		{
			return 0.0f;
		}

		if (minValue > maxValue)
		{
			std::swap(minValue, maxValue);
		}

		if (minValue == maxValue)
		{
			return minValue;
		}

		return minValue + (maxValue - minValue) * NextMuzzleFlashUnitRandom(randomState);
	}

	float EvaluateMuzzleFlashFadeOut(double currentTimeSeconds, bool occupied, float peakIntensity, float radius, double activationTimeSeconds, double endTimeSeconds)
	{
		if (!occupied ||
			peakIntensity <= 0.0f ||
			radius <= 0.0f ||
			currentTimeSeconds < activationTimeSeconds)
		{
			return 0.0f;
		}

		const double durationSeconds = endTimeSeconds - activationTimeSeconds;
		if (durationSeconds <= 0.0 || currentTimeSeconds >= endTimeSeconds)
		{
			return 0.0f;
		}

		const double progress = std::clamp((currentTimeSeconds - activationTimeSeconds) / durationSeconds, 0.0, 1.0);
		const double fade = progress >= 1.0 ? 0.0 : std::pow(2.0, -10.0 * progress);
		return (float)(peakIntensity * fade);
	}

	double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	}

	size_t GetMaterialBuildTraceSlotIndex(NRIRenderer::MaterialBuildTraceSlot slot)
	{
		return (size_t)slot;
	}

	constexpr uint32_t NRI_RUNTIME_MUTATION_REBASELINE_STABLE_FRAMES = 120u;
	constexpr uint32_t NRI_RUNTIME_MUTATION_REBASELINE_MIN_ACTIVE_CHUNKS = 32u;
	constexpr uint32_t NRI_RUNTIME_MUTATION_REBASELINE_MIN_STABLE_CHUNKS = 8u;
	constexpr uint32_t NRI_RUNTIME_MUTATION_REBASELINE_COOLDOWN_FRAMES = 600u;

	const char* GetMaterialBuildTraceSlotNameInternal(NRIRenderer::MaterialBuildTraceSlot slot)
	{
		switch (slot)
		{
		case NRIRenderer::MaterialBuildTraceSlot::DynamicLive: return "dynamic_live";
		case NRIRenderer::MaterialBuildTraceSlot::MirrorExtended: return "mirror_extended";
		case NRIRenderer::MaterialBuildTraceSlot::SceneLightMergedDynamic: return "scene_light_merged_dynamic";
		case NRIRenderer::MaterialBuildTraceSlot::MirrorPlayer: return "mirror_player";
		case NRIRenderer::MaterialBuildTraceSlot::DynamicWithPersistentEmissive: return "dynamic_with_persistent_emissive";
		case NRIRenderer::MaterialBuildTraceSlot::SceneLightMergedPersistent: return "scene_light_merged_persistent";
		case NRIRenderer::MaterialBuildTraceSlot::CapturedScene: return "captured_scene";
		case NRIRenderer::MaterialBuildTraceSlot::PersistentEmissiveCachePrune: return "persistent_emissive_cache_prune";
		case NRIRenderer::MaterialBuildTraceSlot::PersistentEmissiveCacheRebuild: return "persistent_emissive_cache_rebuild";
		case NRIRenderer::MaterialBuildTraceSlot::StaticMapAnimChunk: return "static_map_anim_chunk";
		case NRIRenderer::MaterialBuildTraceSlot::StaticMapChunk: return "static_map_chunk";
		case NRIRenderer::MaterialBuildTraceSlot::RuntimeMutationChunk: return "runtime_mutation_chunk";
		case NRIRenderer::MaterialBuildTraceSlot::RuntimeSpaceLinkChunk: return "runtime_space_link_chunk";
		case NRIRenderer::MaterialBuildTraceSlot::Unknown: return "unknown";
		case NRIRenderer::MaterialBuildTraceSlot::Count: break;
		}

		return "unknown";
	}

	const char* GetRuntimeMutationRebaselineStateNameInternal(NRIRenderer::RuntimeMutationRebaselineState state)
	{
		switch (state)
		{
		case NRIRenderer::RuntimeMutationRebaselineState::Idle: return "idle";
		case NRIRenderer::RuntimeMutationRebaselineState::Queued: return "queued";
		case NRIRenderer::RuntimeMutationRebaselineState::BuildingAuthoritativeWorld: return "building_authoritative_world";
		case NRIRenderer::RuntimeMutationRebaselineState::WorldReady: return "world_ready";
		case NRIRenderer::RuntimeMutationRebaselineState::BuildingStaticSceneCache: return "building_static_scene_cache";
		case NRIRenderer::RuntimeMutationRebaselineState::RealizingStaticSceneTextures: return "realizing_static_scene_textures";
		case NRIRenderer::RuntimeMutationRebaselineState::UploadingStaticSceneBuffers: return "uploading_static_scene_buffers";
		case NRIRenderer::RuntimeMutationRebaselineState::PreparingStaticSceneBlasResources: return "preparing_static_scene_blas_resources";
		case NRIRenderer::RuntimeMutationRebaselineState::BuildingStaticSceneBlas: return "building_static_scene_blas";
		case NRIRenderer::RuntimeMutationRebaselineState::BuildingStaticSceneTlas: return "building_static_scene_tlas";
		case NRIRenderer::RuntimeMutationRebaselineState::ReadyToSwap: return "ready_to_swap";
		}

		return "unknown";
	}

	class ScopedPtPerfTimer
	{
	public:
		explicit ScopedPtPerfTimer(double& targetMs)
			: mTarget(ShouldTracePtPerf() ? &targetMs : nullptr)
		{
			if (mTarget != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedPtPerfTimer()
		{
			if (mTarget != nullptr)
			{
				*mTarget += DurationMs(mStart, std::chrono::steady_clock::now());
			}
		}

	private:
		double* mTarget = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};

	class ScopedSkyPerfTimer
	{
	public:
		explicit ScopedSkyPerfTimer(uint64_t& targetUs)
			: mTarget(ShouldTraceSkyPerf() ? &targetUs : nullptr)
		{
			if (mTarget != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedSkyPerfTimer()
		{
			if (mTarget != nullptr)
			{
				const auto elapsed = std::chrono::steady_clock::now() - mStart;
				*mTarget += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
			}
		}

		ScopedSkyPerfTimer(const ScopedSkyPerfTimer&) = delete;
		ScopedSkyPerfTimer& operator=(const ScopedSkyPerfTimer&) = delete;

	private:
		uint64_t* mTarget = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};

	void ResetRendererSkyPerfTraceStats()
	{
		gRendererSkyPerfTraceStats = {};
	}

	const char* GetMaterialEmissiveModeName(uint32_t mode)
	{
		switch (mode)
		{
		case nri_scene::MaterialEmissiveMode_UseBaseTexture: return "base";
		case nri_scene::MaterialEmissiveMode_UseConstantColor: return "constant";
		case nri_scene::MaterialEmissiveMode_UseGlowmapTexture: return "glowmap";
		default: return "none";
		}
	}

	const char* GetSceneDataSourceName(uint32_t dataSource)
	{
		switch (dataSource)
		{
		case NRI_SCENE_DATA_SOURCE_STATIC: return "static";
		case NRI_SCENE_DATA_SOURCE_DYNAMIC: return "dynamic";
		default: return "unknown";
		}
	}

	const char* GetSurfaceProbeSceneOwnerName(uint32_t owner)
	{
		switch (owner)
		{
		case NRI_SURFACE_PROBE_OWNER_STATIC_MAP: return "static_map";
		case NRI_SURFACE_PROBE_OWNER_CAPTURED_SCENE: return "captured_scene";
		case NRI_SURFACE_PROBE_OWNER_RUNTIME_LINK: return "runtime_link_overlay";
		case NRI_SURFACE_PROBE_OWNER_RUNTIME_MUTATION: return "runtime_mutation_overlay";
		case NRI_SURFACE_PROBE_OWNER_DYNAMIC_OVERLAY: return "dynamic_overlay";
		default: return "unknown";
		}
	}

	uint32_t CountSurfaceTriangles(const nri_scene::SurfaceRef& surface)
	{
		return surface.vertices.size() >= 3 ? (uint32_t)surface.vertices.size() - 2 : 0u;
	}

	struct ChunkCompareSurfaceKey
	{
		uint32_t kind = UINT32_MAX;
		uint32_t sourceType = (uint32_t)nri_scene::SurfaceSourceType::Unknown;
		int32_t sectorIndex = -1;
		int32_t wallIndex = -1;
		int32_t sectionIndex = -1;
		int32_t nextSectorIndex = -1;
		int32_t actorIndex = -1;
		uint32_t cstat = 0;
		uint32_t materialFlags = 0;
		uint32_t primaryKey = UINT32_MAX;
		uint32_t secondaryKey = UINT32_MAX;

		bool operator==(const ChunkCompareSurfaceKey& other) const
		{
			return kind == other.kind &&
				sourceType == other.sourceType &&
				sectorIndex == other.sectorIndex &&
				wallIndex == other.wallIndex &&
				sectionIndex == other.sectionIndex &&
				nextSectorIndex == other.nextSectorIndex &&
				actorIndex == other.actorIndex &&
				cstat == other.cstat &&
				materialFlags == other.materialFlags &&
				primaryKey == other.primaryKey &&
				secondaryKey == other.secondaryKey;
		}
	};

	struct ChunkCompareSurfaceKeyHash
	{
		size_t operator()(const ChunkCompareSurfaceKey& key) const
		{
			size_t h = 1469598103934665603ull;
			const auto mix = [&h](uint64_t value)
			{
				h ^= (size_t)value;
				h *= 1099511628211ull;
			};
			mix(key.kind);
			mix(key.sourceType);
			mix((uint32_t)key.sectorIndex);
			mix((uint32_t)key.wallIndex);
			mix((uint32_t)key.sectionIndex);
			mix((uint32_t)key.nextSectorIndex);
			mix((uint32_t)key.actorIndex);
			mix(key.cstat);
			mix(key.materialFlags);
			mix(key.primaryKey);
			mix(key.secondaryKey);
			return h;
		}
	};

	struct ChunkCompareSurfaceMetrics
	{
		float centroid[3] = {};
		float normal[3] = {};
		float area = 0.0f;
		float aabbMin[3] = {};
		float aabbMax[3] = {};
		uint32_t vertexCount = 0;
		uint32_t triangleCount = 0;
		uint32_t textureId = 0;
		int palette = 0;
		int shade = 0;
		float alpha = 1.0f;
		uint32_t materialFlags = 0;
	};

	struct ChunkCompareMatchRecord
	{
		uint32_t staticSurfaceIndex = UINT32_MAX;
		uint32_t liveSurfaceIndex = UINT32_MAX;
		ChunkCompareSurfaceKey key = {};
		ChunkCompareSurfaceMetrics staticMetrics = {};
		ChunkCompareSurfaceMetrics liveMetrics = {};
		float delta[3] = {};
		float deltaDistance = 0.0f;
		float areaRatio = 1.0f;
		float normalDot = 1.0f;
		float materialScore = 0.0f;
		float deviationFromMean = 0.0f;
		float score = 0.0f;
	};

	static ChunkCompareSurfaceKey BuildChunkCompareSurfaceKey(const nri_scene::PTMapSurface& surface)
	{
		ChunkCompareSurfaceKey key = {};
		key.kind = (uint32_t)surface.kind;
		key.sourceType = (uint32_t)surface.surface.provenance.sourceType;
		key.sectorIndex = surface.surface.provenance.sectorIndex;
		key.wallIndex = surface.surface.provenance.wallIndex;
		key.sectionIndex = surface.surface.provenance.sectionIndex;
		key.nextSectorIndex = surface.surface.provenance.nextSectorIndex;
		key.actorIndex = surface.surface.provenance.actorIndex;
		key.cstat = surface.surface.provenance.cstat;
		key.materialFlags = surface.surface.provenance.materialFlags;
		key.primaryKey = surface.key.primary;
		key.secondaryKey = surface.key.secondary;
		return key;
	}

	static void BuildRuntimeMutationLightIdentityOverrides(
		const nri_scene::PTMapWorld& staticWorld,
		const nri_scene::PTMapChunk& staticChunk,
		const nri_scene::PTMapWorld& liveWorld,
		const nri_scene::PTMapChunk& liveChunk,
		SceneLightSystem::SurfaceIdentityOverrides& outOverrides)
	{
		outOverrides.Clear();
		if (!staticWorld.valid || !liveWorld.valid)
		{
			return;
		}

		std::vector<uint32_t> staticSurfaceIndices;
		std::vector<uint32_t> liveSurfaceIndices;
		staticSurfaceIndices.reserve(staticChunk.surfaceCount);
		liveSurfaceIndices.reserve(liveChunk.surfaceCount);

		for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < staticChunk.surfaceCount; ++localSurfaceIndex)
		{
			const uint32_t surfaceIndex = staticChunk.firstSurface + localSurfaceIndex;
			if (surfaceIndex >= staticWorld.surfaces.size())
			{
				break;
			}
			staticSurfaceIndices.push_back(surfaceIndex);
		}

		for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < liveChunk.surfaceCount; ++localSurfaceIndex)
		{
			const uint32_t surfaceIndex = liveChunk.firstSurface + localSurfaceIndex;
			if (surfaceIndex >= liveWorld.surfaces.size())
			{
				break;
			}
			liveSurfaceIndices.push_back(surfaceIndex);
		}

		std::unordered_map<ChunkCompareSurfaceKey, std::vector<uint32_t>, ChunkCompareSurfaceKeyHash> liveSurfaceLookup;
		liveSurfaceLookup.reserve(liveSurfaceIndices.size());
		for (uint32_t liveLocalIndex = 0; liveLocalIndex < (uint32_t)liveSurfaceIndices.size(); ++liveLocalIndex)
		{
			const auto& liveSurface = liveWorld.surfaces[liveSurfaceIndices[liveLocalIndex]];
			liveSurfaceLookup[BuildChunkCompareSurfaceKey(liveSurface)].push_back(liveLocalIndex);
		}

		std::vector<uint8_t> liveSurfaceUsed(liveSurfaceIndices.size(), 0u);
		std::vector<uint64_t> inheritedIdentityKeys(liveSurfaceIndices.size(), 0ull);
		float staticSurfaceCenter[3] = {};
		for (uint32_t staticSurfaceIndex : staticSurfaceIndices)
		{
			const auto& staticSurface = staticWorld.surfaces[staticSurfaceIndex];
			const ChunkCompareSurfaceKey key = BuildChunkCompareSurfaceKey(staticSurface);
			auto liveSurfaceIt = liveSurfaceLookup.find(key);
			if (liveSurfaceIt == liveSurfaceLookup.end())
			{
				continue;
			}

			uint32_t matchedLiveLocalIndex = UINT32_MAX;
			for (uint32_t candidate : liveSurfaceIt->second)
			{
				if (candidate < liveSurfaceUsed.size() && liveSurfaceUsed[candidate] == 0u)
				{
					matchedLiveLocalIndex = candidate;
					break;
				}
			}
			if (matchedLiveLocalIndex == UINT32_MAX)
			{
				continue;
			}

			liveSurfaceUsed[matchedLiveLocalIndex] = 1u;
			ComputeCapturedSurfaceCenter(staticSurface.surface, staticSurfaceCenter);
			inheritedIdentityKeys[matchedLiveLocalIndex] = SceneLightSystem::ComputeSurfaceIdentityKey(
				SceneLightRecordSource::StaticMapScene,
				staticSurface.surface.provenance,
				staticSurfaceCenter);
		}

		outOverrides.opaqueWalls.reserve(liveWorld.stats.wallSurfaceCount);
		outOverrides.opaqueFlats.reserve(liveWorld.stats.flatSurfaceCount);
		for (uint32_t liveLocalIndex = 0; liveLocalIndex < (uint32_t)liveSurfaceIndices.size(); ++liveLocalIndex)
		{
			const auto& liveSurface = liveWorld.surfaces[liveSurfaceIndices[liveLocalIndex]];
			if ((liveSurface.surface.material.flags & nri_scene::MaterialFlag_Sky) != 0 && liveSurface.surface.material.texture != nullptr)
			{
				continue;
			}

			switch (liveSurface.kind)
			{
			case nri_scene::PTMapSurfaceKind::Floor:
			case nri_scene::PTMapSurfaceKind::Ceiling:
				outOverrides.opaqueFlats.push_back(inheritedIdentityKeys[liveLocalIndex]);
				break;
			default:
				outOverrides.opaqueWalls.push_back(inheritedIdentityKeys[liveLocalIndex]);
				break;
			}
		}
	}

	static uint32_t GetSurfaceTextureId(const nri_scene::PTMapSurface& surface)
	{
		return
			surface.surface.material.texture != nullptr ?
			(uint32_t)surface.surface.material.texture->GetID().GetIndex() :
			0u;
	}

	static float Distance3(const float a[3], const float b[3])
	{
		const float dx = a[0] - b[0];
		const float dy = a[1] - b[1];
		const float dz = a[2] - b[2];
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	static float Dot3(const float a[3], const float b[3])
	{
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	}

	static float ComputeTriangleArea(const nri_scene::CapturedVertex& a, const nri_scene::CapturedVertex& b, const nri_scene::CapturedVertex& c)
	{
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float crossX = aby * acz - abz * acy;
		const float crossY = abz * acx - abx * acz;
		const float crossZ = abx * acy - aby * acx;
		return 0.5f * std::sqrt(crossX * crossX + crossY * crossY + crossZ * crossZ);
	}

	static void ComputeTriangleNormal(const nri_scene::CapturedVertex& a, const nri_scene::CapturedVertex& b, const nri_scene::CapturedVertex& c, float outNormal[3])
	{
		outNormal[0] = 0.0f;
		outNormal[1] = 0.0f;
		outNormal[2] = 0.0f;
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float crossX = aby * acz - abz * acy;
		const float crossY = abz * acx - abx * acz;
		const float crossZ = abx * acy - aby * acx;
		const float length = std::sqrt(crossX * crossX + crossY * crossY + crossZ * crossZ);
		if (length <= 0.0001f)
		{
			return;
		}

		outNormal[0] = crossX / length;
		outNormal[1] = crossY / length;
		outNormal[2] = crossZ / length;
	}

	static ChunkCompareSurfaceMetrics ComputeChunkCompareSurfaceMetrics(const nri_scene::PTMapSurface& surface)
	{
		ChunkCompareSurfaceMetrics metrics = {};
		const auto& vertices = surface.surface.vertices;
		metrics.vertexCount = (uint32_t)vertices.size();
		metrics.triangleCount = CountSurfaceTriangles(surface.surface);
		metrics.textureId = GetSurfaceTextureId(surface);
		metrics.palette = surface.surface.material.palette;
		metrics.shade = surface.surface.material.shade;
		metrics.alpha = surface.surface.material.alpha;
		metrics.materialFlags = surface.surface.material.flags;
		if (vertices.empty())
		{
			return metrics;
		}

		for (int axis = 0; axis < 3; ++axis)
		{
			metrics.aabbMin[axis] = vertices[0].position[axis];
			metrics.aabbMax[axis] = vertices[0].position[axis];
		}

		for (const auto& vertex : vertices)
		{
			for (int axis = 0; axis < 3; ++axis)
			{
				metrics.centroid[axis] += vertex.position[axis];
				metrics.aabbMin[axis] = std::min(metrics.aabbMin[axis], vertex.position[axis]);
				metrics.aabbMax[axis] = std::max(metrics.aabbMax[axis], vertex.position[axis]);
			}
		}

		const float invCount = 1.0f / (float)vertices.size();
		for (int axis = 0; axis < 3; ++axis)
		{
			metrics.centroid[axis] *= invCount;
		}

		if (vertices.size() >= 3)
		{
			if ((surface.surface.material.flags & nri_scene::MaterialFlag_Flat) != 0 &&
				(vertices.size() % 3u) == 0u)
			{
				for (size_t i = 0; i + 2 < vertices.size(); i += 3)
				{
					metrics.area += ComputeTriangleArea(vertices[i], vertices[i + 1], vertices[i + 2]);
					if (metrics.normal[0] == 0.0f && metrics.normal[1] == 0.0f && metrics.normal[2] == 0.0f)
					{
						ComputeTriangleNormal(vertices[i], vertices[i + 1], vertices[i + 2], metrics.normal);
					}
				}
			}
			else
			{
				const auto& root = vertices[0];
				for (size_t i = 1; i + 1 < vertices.size(); ++i)
				{
					metrics.area += ComputeTriangleArea(root, vertices[i], vertices[i + 1]);
					if (metrics.normal[0] == 0.0f && metrics.normal[1] == 0.0f && metrics.normal[2] == 0.0f)
					{
						ComputeTriangleNormal(root, vertices[i], vertices[i + 1], metrics.normal);
					}
				}
			}
		}

		return metrics;
	}

	const char* GetMapSurfaceKindName(nri_scene::PTMapSurfaceKind kind)
	{
		switch (kind)
		{
		case nri_scene::PTMapSurfaceKind::Floor: return "floor";
		case nri_scene::PTMapSurfaceKind::Ceiling: return "ceiling";
		case nri_scene::PTMapSurfaceKind::WallOneSided: return "wall_one_sided";
		case nri_scene::PTMapSurfaceKind::WallUpper: return "wall_upper";
		case nri_scene::PTMapSurfaceKind::WallMiddle: return "wall_middle";
		case nri_scene::PTMapSurfaceKind::WallLower: return "wall_lower";
		case nri_scene::PTMapSurfaceKind::Portal: return "portal";
		default: return "unknown";
		}
	}

	float ComputePrimitiveArea(const nri_scene::GeometryData& geometry, uint32_t primitiveIndex)
	{
		if (primitiveIndex >= geometry.primitives.size())
		{
			return 0.0f;
		}

		const auto& primitive = geometry.primitives[primitiveIndex];
		if (primitive.indices[0] >= geometry.vertices.size() ||
			primitive.indices[1] >= geometry.vertices.size() ||
			primitive.indices[2] >= geometry.vertices.size())
		{
			return 0.0f;
		}

		const auto& a = geometry.vertices[primitive.indices[0]];
		const auto& b = geometry.vertices[primitive.indices[1]];
		const auto& c = geometry.vertices[primitive.indices[2]];
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float crossX = aby * acz - abz * acy;
		const float crossY = abz * acx - abx * acz;
		const float crossZ = abx * acy - aby * acx;
		return 0.5f * std::sqrt(crossX * crossX + crossY * crossY + crossZ * crossZ);
	}

	void ComputePrimitiveCenter(const nri_scene::GeometryData& geometry, uint32_t primitiveIndex, float outCenter[3])
	{
		outCenter[0] = 0.0f;
		outCenter[1] = 0.0f;
		outCenter[2] = 0.0f;
		if (primitiveIndex >= geometry.primitives.size())
		{
			return;
		}

		const auto& primitive = geometry.primitives[primitiveIndex];
		if (primitive.indices[0] >= geometry.vertices.size() ||
			primitive.indices[1] >= geometry.vertices.size() ||
			primitive.indices[2] >= geometry.vertices.size())
		{
			return;
		}

		const auto& a = geometry.vertices[primitive.indices[0]];
		const auto& b = geometry.vertices[primitive.indices[1]];
		const auto& c = geometry.vertices[primitive.indices[2]];
		outCenter[0] = (a.position[0] + b.position[0] + c.position[0]) / 3.0f;
		outCenter[1] = (a.position[1] + b.position[1] + c.position[1]) / 3.0f;
		outCenter[2] = (a.position[2] + b.position[2] + c.position[2]) / 3.0f;
	}

	struct ScenePortalData
	{
		uint32_t traversalClass = 0;
		uint32_t kind = 0;
		uint32_t targetLocalSpaceIndex = UINT32_MAX;
		uint32_t flags = 0;
		float delta[3] = {};
		uint32_t reserved0 = 0;
	};

	template<typename T>
	static T NRIFlags(T a, T b)
	{
		return (T)((uint32_t)a | (uint32_t)b);
	}

	static nri::StageBits NRIComputeStage()
	{
		return nri::StageBits::COMPUTE_SHADER;
	}

	static nri::AccessStage NRIComputeShaderResourceAccess()
	{
		return { nri::AccessBits::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER };
	}

	static nri::AccessStage NRIAccelerationStructureBuildInputAccess()
	{
		return { nri::AccessBits::SHADER_RESOURCE, nri::StageBits::ALL_SHADERS };
	}

	static nri::AccessStage NRIAccelerationStructureWriteAccess()
	{
		return { nri::AccessBits::ACCELERATION_STRUCTURE_WRITE, nri::StageBits::ACCELERATION_STRUCTURE };
	}

	static nri::AccessStage NRIAccelerationStructureScratchAccess()
	{
		return { nri::AccessBits::SCRATCH_BUFFER, nri::StageBits::ACCELERATION_STRUCTURE };
	}

	static nri::AccessStage NRIAccelerationStructureReadAccess()
	{
		return { nri::AccessBits::ACCELERATION_STRUCTURE_READ, nri::StageBits::ACCELERATION_STRUCTURE };
	}

	static uint32_t ClampNrdHistoryFrameCount(int value)
	{
		return (uint32_t)std::clamp(value, 0, (int)nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
	}

	static uint32_t ClampNrdFastFrameCount(int value, uint32_t maxAccumulatedFrameNum)
	{
		return (uint32_t)std::clamp(value, 0, (int)maxAccumulatedFrameNum);
	}

	static uint32_t ClampNrdStabilizationFrameCount(int value, uint32_t maxAccumulatedFrameNum)
	{
		return (uint32_t)std::clamp(value, 0, (int)maxAccumulatedFrameNum);
	}

	static uint32_t ClampSigmaStabilizationFrameCount(int value)
	{
		return (uint32_t)std::clamp(value, 0, (int)nrd::SIGMA_MAX_HISTORY_FRAME_NUM);
	}

	static uint32_t GetNrdHitDistanceReconstructionMode()
	{
		return (uint32_t)std::clamp((int)nri_nrdhitdistrecon, 0, 2);
	}

	static const char* GetNrdHitDistanceReconstructionModeName(uint32_t mode)
	{
		switch (mode)
		{
		case 1: return "area_3x3";
		case 2: return "area_5x5";
		default: return "off";
		}
	}

	static uint32_t GetNrdInputSplitMode()
	{
		return (uint32_t)std::clamp((int)nri_nrdsplit, 0, 2);
	}

	static bool IsSupportedPtDebugMode(uint32_t debugMode)
	{
		return FindPtDebugMenuIndex((int)debugMode) >= 0;
	}

	static bool IsFinalShaderDebugMode(uint32_t debugMode)
	{
		return false;
	}

	static bool IsRawTraceDebugMode(uint32_t debugMode)
	{
		return
			(debugMode >= 1u && debugMode <= 5u) ||
			(debugMode >= 10u && debugMode <= 12u) ||
			debugMode == 18u ||
			debugMode == 19u ||
			(debugMode >= 21u && debugMode <= 22u) ||
			(debugMode >= 24u && debugMode <= 25u) ||
			(debugMode >= NRI_PTDEBUG_ANALYTIC_DIRECT && debugMode <= NRI_PTDEBUG_SECTOR_AMBIENT) ||
			debugMode == NRI_PTDEBUG_EMISSIVE_SAMPLE_VISIBILITY;
	}

	static uint32_t GetEffectivePtDebugMode()
	{
		if (nri_ptdebug < 0 || nri_ptdebug > (int)NRI_PTDEBUG_TAA_PRE_EXPOSED_INPUT)
		{
			return 0u;
		}

		const uint32_t debugMode = (uint32_t)nri_ptdebug;
		return IsSupportedPtDebugMode(debugMode) ? debugMode : 0u;
	}

	static float GetTemporalExposure(const NRIPTOutputPolicy& outputPolicy)
	{
		return std::max(outputPolicy.exposure, 0.125f);
	}

	static float GetFullbrightBoostScale()
	{
		return std::clamp((float)nri_ptfullbrightboost, 0.50f, 8.00f);
	}

	static bool IsGlowDrivenEmissiveForSampling(uint32_t sourceFlags, uint32_t emissiveMode)
	{
		if (emissiveMode == nri_scene::MaterialEmissiveMode_UseGlowmapTexture)
		{
			return true;
		}

		return (sourceFlags & (SceneEmissiveSurfaceSourceFlag_AutoTextureGlow | SceneEmissiveSurfaceSourceFlag_AutoGlowmap)) != 0;
	}

	static float ResolveGlowSamplingScale(uint32_t sourceFlags, uint32_t emissiveMode)
	{
		return IsGlowDrivenEmissiveForSampling(sourceFlags, emissiveMode) ? std::max((float)nri_ptglowreach, 0.0f) : 1.0f;
	}

	static float GetFullbrightRoughnessHint(uint32_t materialFlags)
	{
		if ((materialFlags & nri_scene::MaterialFlag_Sprite) != 0)
		{
			return 0.45f;
		}
		if ((materialFlags & nri_scene::MaterialFlag_Flat) != 0)
		{
			return 0.60f;
		}
		return 0.55f;
	}

	static void ApplyActorFullbrightOverridesToBuiltMaterials(
		const std::unordered_map<int32_t, uint32_t>& actorOverrides,
		nri_scene::MaterialBridgeData& materials)
	{
		const uint32_t count = std::min<uint32_t>((uint32_t)materials.materials.size(), (uint32_t)materials.lightMetadata.size());
		const float fullbrightBoost = GetFullbrightBoostScale();
		for (uint32_t materialIndex = 0; materialIndex < count; ++materialIndex)
		{
			nri_scene::MaterialLightingMetadata& metadata = materials.lightMetadata[materialIndex];
			if (metadata.actorIndex < 0)
			{
				continue;
			}

			auto it = actorOverrides.find(metadata.actorIndex);
			if (it == actorOverrides.end() || (it->second & ActorMaterialOverride_Fullbright) == 0)
			{
				continue;
			}

			nri_scene::MaterialData& material = materials.materials[materialIndex];
			material.flags |= nri_scene::MaterialFlag_Fullbright;
			material.lightLevel = 1.0f;
			material.roughnessHint = GetFullbrightRoughnessHint(material.flags);
			material.lightingFlags |= nri_scene::MaterialLightingFlag_MaterialFullbright;
			material.emissiveMode = nri_scene::MaterialEmissiveMode_UseBaseTexture;
			material.emissiveTextureIndex = material.textureIndex;
			material.emissiveIntensity = 1.0f;
			material.emissiveMaskScale = 1.0f;
			material.emissiveReserved = fullbrightBoost;
			material.emissiveColor[0] = 1.0f;
			material.emissiveColor[1] = 1.0f;
			material.emissiveColor[2] = 1.0f;

			metadata.materialFlags |= nri_scene::MaterialFlag_Fullbright;
			metadata.lightingFlags |= nri_scene::MaterialLightingFlag_MaterialFullbright;
			metadata.lightLevel = 1.0f;
			metadata.emissiveMode = nri_scene::MaterialEmissiveMode_UseBaseTexture;
			metadata.emissiveTextureIndex = material.textureIndex;
			metadata.emissiveIntensity = 1.0f;
			metadata.emissiveMaskScale = 1.0f;
			metadata.visibleFullbrightBoost = fullbrightBoost;
			metadata.emissiveColor[0] = 1.0f;
			metadata.emissiveColor[1] = 1.0f;
			metadata.emissiveColor[2] = 1.0f;
		}
	}

	static float GetExposureDeltaStops(float previousExposure, float currentExposure)
	{
		const float safePrevious = std::max(previousExposure, 0.125f);
		const float safeCurrent = std::max(currentExposure, 0.125f);
		return std::abs(std::log2(safeCurrent) - std::log2(safePrevious));
	}

	static uint32_t GetBootstrapMode();

	static NRIPresentRouteInfo ResolvePresentRouteInfo(uint32_t debugMode, bool bootstrap)
	{
		if (bootstrap)
		{
			const uint32_t bootstrapMode = GetBootstrapMode();
			if (bootstrapMode == 11u || bootstrapMode == 12u)
			{
				return { NRIPresentRouteKind::BootstrapFinal, "bootstrap_raw_trace", "Final", "bootstrap" };
			}

			return { NRIPresentRouteKind::FallbackFinal, "bootstrap_fallback", "Final", "bootstrap" };
		}

		if (debugMode == 0u)
		{
			return { NRIPresentRouteKind::ResolvedBeauty, "resolved_beauty", "FinalPresent", "beauty" };
		}
		if (debugMode == NRI_PTDEBUG_TAA_PRE_EXPOSED_INPUT)
		{
			return { NRIPresentRouteKind::ComposedDebug, "taa_pre_exposed_probe", "FinalPresent", "debug-temporal" };
		}
		if (debugMode == NRI_PTDEBUG_UPSCALER_TRACE_TRANSPARENT)
		{
			return { NRIPresentRouteKind::UpscalerTraceTransparentProbe, "upscaler_trace_transparent", "RawPresent", "debug-upscaler" };
		}
		if (debugMode == 9u)
		{
			return { NRIPresentRouteKind::ValidationRaw, "validation_raw", "RawPresent", "debug-nrd" };
		}
		if (debugMode == 16u || debugMode == 17u)
		{
			return { NRIPresentRouteKind::DenoisedRaw, "denoised_raw", "RawPresent", "debug-nrd" };
		}
		if (IsFinalShaderDebugMode(debugMode))
		{
			return { NRIPresentRouteKind::FinalDebug, "final_debug", "Final", "debug-final" };
		}
		if (IsRawTraceDebugMode(debugMode))
		{
			return { NRIPresentRouteKind::RawTraceDebug, "raw_trace_debug", "RawPresent", "debug-trace" };
		}

		return { NRIPresentRouteKind::FallbackFinal, "fallback_final", "Final", "fallback" };
	}

	static NRINrdDenoiserMode GetSelectedNrdDenoiserMode()
	{
		return (NRINrdDenoiserMode)std::clamp((int)nri_nrddenoiser, 0, 1);
	}

	static const char* GetNrdDenoiserModeName(NRINrdDenoiserMode mode)
	{
		switch (mode)
		{
		case NRINrdDenoiserMode::Relax: return "RELAX_DIFFUSE_SPECULAR";
		default: return "REBLUR_DIFFUSE_SPECULAR";
		}
	}

	static float ClampNrdFastHistorySigmaScale(float value)
	{
		return std::clamp(value, 1.0f, 3.0f);
	}

	static float ClampNrdPrepassBlurRadius(float value)
	{
		return std::clamp(value, 0.0f, 75.0f);
	}

	static float ClampNrdBlurRadius(float value)
	{
		return std::clamp(value, 0.0f, 60.0f);
	}

	static float ClampSigmaPlaneDistanceSensitivity(float value)
	{
		return std::clamp(value, 0.001f, 0.1f);
	}

	static const char* GetNrdInputSplitModeName(uint32_t mode)
	{
		switch (mode)
		{
		case 1: return "raw_left_denoised_right";
		case 2: return "denoised_left_raw_right";
		default: return "off";
		}
	}

	static bool SameRuntimeLinkDebugState(const RuntimeLinkDebugState& a, const RuntimeLinkDebugState& b)
	{
		return
			a.available == b.available &&
			a.specialWaterSector == b.specialWaterSector &&
			a.playerSectorIndex == b.playerSectorIndex &&
			a.playerSectorLotag == b.playerSectorLotag &&
			a.playerSectorHitag == b.playerSectorHitag &&
			a.effectiveSectorLotag == b.effectiveSectorLotag &&
			a.actorSectorIndex == b.actorSectorIndex &&
			a.actorSectorLotag == b.actorSectorLotag &&
			a.actorSectorHitag == b.actorSectorHitag &&
			a.onWarpingSector == b.onWarpingSector &&
			a.transporterHold == b.transporterHold &&
			a.rrGeoCount == b.rrGeoCount;
	}

	static bool SameRuntimeTaggedSectorDebugInfo(const RuntimeTaggedSectorDebugInfo& a, const RuntimeTaggedSectorDebugInfo& b)
	{
		if (a.available != b.available ||
			a.sectorIndex != b.sectorIndex ||
			a.lotag != b.lotag ||
			a.hitag != b.hitag ||
			a.effectorCount != b.effectorCount)
		{
			return false;
		}

		for (size_t i = 0; i < countof(a.effectorLotags); ++i)
		{
			if (a.effectorLotags[i] != b.effectorLotags[i] || a.effectorHitags[i] != b.effectorHitags[i])
			{
				return false;
			}
		}

		return true;
	}

	static bool ShouldStoreRuntimeSectorControlInfo(const RuntimeTaggedSectorDebugInfo& info)
	{
		return info.available && (info.lotag != 0 || info.hitag != 0 || info.effectorCount > 0);
	}

	static bool AppendRuntimeSectorControlInfo(std::array<RuntimeTaggedSectorDebugInfo, 12>& infos, uint32_t& infoCount, const RuntimeTaggedSectorDebugInfo& info)
	{
		if (!ShouldStoreRuntimeSectorControlInfo(info))
		{
			return false;
		}

		for (uint32_t i = 0; i < infoCount; ++i)
		{
			if (infos[i].sectorIndex == info.sectorIndex)
			{
				return false;
			}
		}

		if (infoCount >= infos.size())
		{
			return false;
		}

		infos[infoCount++] = info;
		return true;
	}

	static bool GetRuntimeSectorControlInfo(int sectorIndex, RuntimeTaggedSectorDebugInfo& info)
	{
		if (!validSectorIndex(sectorIndex))
		{
			return false;
		}

		info = {};
		if (gi != nullptr && gi->GetRuntimeLinkDebugTaggedSectorInfo(sectorIndex, &info))
		{
			return true;
		}

		const auto& sec = sector[(unsigned)sectorIndex];
		info.available = true;
		info.sectorIndex = sectorIndex;
		info.lotag = sec.lotag;
		info.hitag = sec.hitag;
		return true;
	}

	static const char* GetSkyModeName(nri_scene::PTSkyMode mode)
	{
		switch (mode)
		{
		case nri_scene::PTSkyMode::None:
			return "none";
		case nri_scene::PTSkyMode::SolidColor:
			return "solid";
		case nri_scene::PTSkyMode::Cubemap:
			return "cubemap";
		default:
			return "unknown";
		}
	}

	static const char* GetSkySourceTypeName(nri_scene::PTSkySourceType sourceType)
	{
		switch (sourceType)
		{
		case nri_scene::PTSkySourceType::None:
			return "none";
		case nri_scene::PTSkySourceType::Wall:
			return "wall";
		case nri_scene::PTSkySourceType::Flat:
			return "flat";
		case nri_scene::PTSkySourceType::Portal:
			return "portal";
		default:
			return "unknown";
		}
	}

	static nri::AccessStage NRIComputeAccelerationStructureReadAccess()
	{
		return { nri::AccessBits::ACCELERATION_STRUCTURE_READ, nri::StageBits::COMPUTE_SHADER };
	}

	static void AppendMutationReasonToken(std::string& text, const char* token)
	{
		if (!text.empty())
		{
			text += "|";
		}
		text += token;
	}

	static std::string GetRuntimeMapMutationReasonSummary(uint32_t reasonMask)
	{
		std::string text;
		if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectorGeometry) != 0)
		{
			AppendMutationReasonToken(text, "sector_geom");
		}
		if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectorMaterial) != 0)
		{
			AppendMutationReasonToken(text, "sector_mat");
		}
		if ((reasonMask & nri_scene::PTMapChunkMutationReason_WallGeometry) != 0)
		{
			AppendMutationReasonToken(text, "wall_geom");
		}
		if ((reasonMask & nri_scene::PTMapChunkMutationReason_WallMaterial) != 0)
		{
			AppendMutationReasonToken(text, "wall_mat");
		}
		if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectorDirty) != 0)
		{
			AppendMutationReasonToken(text, "sector_dirty");
		}
		if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectionDirty) != 0)
		{
			AppendMutationReasonToken(text, "section_dirty");
		}
		if ((reasonMask & nri_scene::PTMapChunkMutationReason_Dragged) != 0)
		{
			AppendMutationReasonToken(text, "dragged");
		}
		if (text.empty())
		{
			text = "none";
		}
		return text;
	}

	static uint32_t GetDispatchSize(uint32_t value)
	{
		return (value + 7u) / 8u;
	}

	static int32_t FindMapChunkIndexForSector(const nri_scene::PTMapWorld& mapWorld, int32_t sectorIndex)
	{
		if (!mapWorld.valid || sectorIndex < 0)
		{
			return -1;
		}

		for (const auto& chunk : mapWorld.chunks)
		{
			if (chunk.kind == nri_scene::PTMapChunkKind::Sector && chunk.sectorIndex == sectorIndex)
			{
				return (int32_t)chunk.chunkIndex;
			}
		}

		return -1;
	}

	static void MarkChunkVisible(std::vector<uint32_t>& visibleChunkWords, uint32_t chunkIndex)
	{
		const size_t wordIndex = (size_t)(chunkIndex >> 5u);
		if (wordIndex >= visibleChunkWords.size())
		{
			return;
		}

		visibleChunkWords[wordIndex] |= 1u << (chunkIndex & 31u);
	}

	static bool IsChunkMarkedVisible(const std::vector<uint32_t>& visibleChunkWords, uint32_t chunkIndex)
	{
		const size_t wordIndex = (size_t)(chunkIndex >> 5u);
		if (wordIndex >= visibleChunkWords.size())
		{
			return false;
		}

		return (visibleChunkWords[wordIndex] & (1u << (chunkIndex & 31u))) != 0u;
	}

	static uint32_t GetFlatPlaneVisibilityIndex(int32_t sectorIndex, bool ceiling)
	{
		return (uint32_t)sectorIndex * 2u + (ceiling ? 1u : 0u);
	}

	static void MarkFlatPlaneVisible(std::vector<uint32_t>& visibleFlatPlaneWords, int32_t sectorIndex, bool ceiling)
	{
		if (sectorIndex < 0)
		{
			return;
		}

		const uint32_t flatPlaneIndex = GetFlatPlaneVisibilityIndex(sectorIndex, ceiling);
		const size_t wordIndex = (size_t)(flatPlaneIndex >> 5u);
		if (wordIndex >= visibleFlatPlaneWords.size())
		{
			return;
		}

		visibleFlatPlaneWords[wordIndex] |= 1u << (flatPlaneIndex & 31u);
	}

	static bool IsFlatPlaneMarkedVisible(const std::vector<uint32_t>& visibleFlatPlaneWords, int32_t sectorIndex, bool ceiling)
	{
		if (sectorIndex < 0)
		{
			return false;
		}

		const uint32_t flatPlaneIndex = GetFlatPlaneVisibilityIndex(sectorIndex, ceiling);
		const size_t wordIndex = (size_t)(flatPlaneIndex >> 5u);
		if (wordIndex >= visibleFlatPlaneWords.size())
		{
			return false;
		}

		return (visibleFlatPlaneWords[wordIndex] & (1u << (flatPlaneIndex & 31u))) != 0u;
	}

	static void MarkVisibleChunkForSector(const nri_scene::PTMapWorld& mapWorld, int32_t sectorIndex, std::vector<uint32_t>& visibleChunkWords)
	{
		const int32_t chunkIndex = FindMapChunkIndexForSector(mapWorld, sectorIndex);
		if (chunkIndex >= 0)
		{
			MarkChunkVisible(visibleChunkWords, (uint32_t)chunkIndex);
		}
	}

	static void AccumulateVisibleChunksFromViewRoots(const HWDrawInfo& di, const nri_scene::PTMapWorld& mapWorld, std::vector<uint32_t>& visibleChunkWords)
	{
		if (di.Viewpoint.SectNums != nullptr)
		{
			for (int i = 0; i < di.Viewpoint.SectCount; ++i)
			{
				MarkVisibleChunkForSector(mapWorld, di.Viewpoint.SectNums[i], visibleChunkWords);
			}
		}
		else
		{
			MarkVisibleChunkForSector(mapWorld, di.Viewpoint.SectCount, visibleChunkWords);
		}
	}

	static void AccumulateVisibleChunksFromDrawLists(const HWDrawInfo& di, const nri_scene::PTMapWorld& mapWorld, std::vector<uint32_t>& visibleChunkWords)
	{
		for (int drawListType = 0; drawListType < GLDL_TYPES; ++drawListType)
		{
			const HWDrawList& drawList = di.drawlists[drawListType];

			for (const HWWall* wall : drawList.walls)
			{
				if (wall != nullptr && wall->seg != nullptr)
				{
					MarkVisibleChunkForSector(mapWorld, wall->seg->sector, visibleChunkWords);
				}
			}

			for (const HWFlat* flat : drawList.flats)
			{
				if (flat != nullptr && flat->sec != nullptr)
				{
					MarkVisibleChunkForSector(mapWorld, sector.IndexOf(flat->sec), visibleChunkWords);
				}
			}
		}
	}

	static void AccumulateVisibleFlatPlanesFromDrawLists(const HWDrawInfo& di, std::vector<uint32_t>& visibleFlatPlaneWords)
	{
		for (int drawListType = 0; drawListType < GLDL_TYPES; ++drawListType)
		{
			const HWDrawList& drawList = di.drawlists[drawListType];
			for (const HWFlat* flat : drawList.flats)
			{
				if (flat == nullptr || flat->sec == nullptr || flat->Sprite != nullptr)
				{
					continue;
				}

				MarkFlatPlaneVisible(
					visibleFlatPlaneWords,
					sector.IndexOf(flat->sec),
					flat->plane != 0);
			}
		}
	}

	static uint64_t GetGrownBufferSize(uint64_t currentCapacity, uint64_t requiredSize, uint32_t stride)
	{
		uint64_t newCapacity = std::max<uint64_t>(requiredSize, stride);
		if (currentCapacity >= newCapacity && currentCapacity != 0)
		{
			return currentCapacity;
		}

		if (currentCapacity != 0)
		{
			newCapacity = std::max(newCapacity, currentCapacity);
			while (newCapacity < requiredSize)
			{
				const uint64_t doubled = newCapacity <= std::numeric_limits<uint64_t>::max() / 2 ? newCapacity * 2 : std::numeric_limits<uint64_t>::max();
				if (doubled <= newCapacity)
				{
					newCapacity = requiredSize;
					break;
				}
				newCapacity = doubled;
			}
		}

		return std::max<uint64_t>(newCapacity, stride);
	}

	static float Clamp01(float value)
	{
		return std::max(0.0f, std::min(value, 1.0f));
	}

	static uint32_t ClampTraceBounceCount(int value, uint32_t maxValue)
	{
		return (uint32_t)std::max(0, std::min(value, (int)maxValue));
	}

	static uint32_t PackTraceBounceCounts(uint32_t lightBounceCount, uint32_t mirrorBounceCount, const float directionalColor[3])
	{
		return
			(lightBounceCount & 0xfu) |
			((mirrorBounceCount & 0xfu) << 4u) |
			(PackDirectionalLightColor24(directionalColor) << 8u);
	}

	static uint32_t PackTraceAux1(uint32_t denoiserMode, uint32_t emissiveSampleCount, float directionalAngularSize)
	{
		return
			(denoiserMode & 0xffu) |
			((emissiveSampleCount & 0xffu) << 8u) |
			(PackDirectionalAngularSize16(directionalAngularSize) << 16u);
	}

	static uint32_t PackDenoiserAux1(uint32_t denoiserMode, float directionalAngularSize)
	{
		return (denoiserMode & 0xffu) | (PackDirectionalAngularSize16(directionalAngularSize) << 16u);
	}

	static uint32_t PackUInt16Pair(uint32_t lo, uint32_t hi)
	{
		return (lo & 0xffffu) | ((hi & 0xffffu) << 16u);
	}

	static nri_scene::SceneDebugStats MergeSceneStats(const nri_scene::SceneDebugStats& a, const nri_scene::SceneDebugStats& b)
	{
		nri_scene::SceneDebugStats merged = {};
		merged.totalDrawItems = a.totalDrawItems + b.totalDrawItems;
		merged.wallDrawItems = a.wallDrawItems + b.wallDrawItems;
		merged.flatDrawItems = a.flatDrawItems + b.flatDrawItems;
		merged.spriteDrawItems = a.spriteDrawItems + b.spriteDrawItems;
		merged.translucentDrawItems = a.translucentDrawItems + b.translucentDrawItems;
		merged.triangleEstimate = a.triangleEstimate + b.triangleEstimate;
		merged.materialRefs = a.materialRefs + b.materialRefs;
		merged.mirrorSurfaces = a.mirrorSurfaces + b.mirrorSurfaces;
		merged.skySurfaces = a.skySurfaces + b.skySurfaces;
		merged.portalViews = a.portalViews + b.portalViews;
		merged.portalCapturesSkipped = a.portalCapturesSkipped + b.portalCapturesSkipped;
		merged.modelDrawItems = a.modelDrawItems + b.modelDrawItems;
		merged.voxelProxyDrawItems = a.voxelProxyDrawItems + b.voxelProxyDrawItems;
		merged.unsupportedModelDrawItems = a.unsupportedModelDrawItems + b.unsupportedModelDrawItems;
		return merged;
	}

	static uint32_t GetPortalTraversalClass(nri_scene::PTPortalKind kind)
	{
		switch (kind)
		{
		case nri_scene::PTPortalKind::WallMirror:
		case nri_scene::PTPortalKind::SectorFloorMirror:
		case nri_scene::PTPortalKind::SectorCeilingMirror:
			return NRI_PORTAL_TRAVERSAL_CLASS_REFLECTIVE;

		case nri_scene::PTPortalKind::WallView:
		case nri_scene::PTPortalKind::SectorFloorStack:
		case nri_scene::PTPortalKind::SectorCeilingStack:
			return NRI_PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER;

		case nri_scene::PTPortalKind::WallToSprite:
			return NRI_PORTAL_TRAVERSAL_CLASS_RUNTIME_BOUND;

		default:
			return NRI_PORTAL_TRAVERSAL_CLASS_NONE;
		}
	}

	static uint32_t CountPortalTraversalClass(const nri_scene::PTMapWorld& mapWorld, uint32_t traversalClass)
	{
		uint32_t count = 0;
		for (const auto& portal : mapWorld.portals)
		{
			if (GetPortalTraversalClass(portal.kind) == traversalClass)
			{
				count++;
			}
		}
		return count;
	}

	static uint32_t CountPendingPlanePortals(const nri_scene::PTMapWorld& mapWorld)
	{
		uint32_t count = 0;
		for (const auto& portal : mapWorld.portals)
		{
			switch (portal.kind)
			{
			case nri_scene::PTPortalKind::SectorFloorStack:
			case nri_scene::PTPortalKind::SectorCeilingStack:
			case nri_scene::PTPortalKind::SectorFloorMirror:
			case nri_scene::PTPortalKind::SectorCeilingMirror:
				if (portal.sourceSurfaceIndex == UINT32_MAX)
				{
					count++;
				}
				break;
			default:
				break;
			}
		}
		return count;
	}

	static uint32_t CountOrphanLocalSpaces(const nri_scene::PTMapWorld& mapWorld)
	{
		if (!mapWorld.valid || mapWorld.localSpaces.empty())
		{
			return 0;
		}

		std::vector<uint8_t> linked(mapWorld.localSpaces.size(), 0u);
		for (const auto& portal : mapWorld.portals)
		{
			if (portal.sourceLocalSpaceIndex < linked.size())
			{
				linked[portal.sourceLocalSpaceIndex] = 1u;
			}

			for (uint32_t i = 0; i < portal.targetCount; ++i)
			{
				const uint32_t targetIndex = portal.firstTarget + i;
				if (targetIndex >= mapWorld.portalTargets.size())
				{
					break;
				}

				const uint32_t localSpaceIndex = mapWorld.portalTargets[targetIndex].localSpaceIndex;
				if (localSpaceIndex < linked.size())
				{
					linked[localSpaceIndex] = 1u;
				}
			}
		}

		uint32_t orphanCount = 0;
		for (uint8_t value : linked)
		{
			if (value == 0u)
			{
				orphanCount++;
			}
		}

		return orphanCount;
	}

	static void TranslateGeometry(nri_scene::GeometryData& geometry, float dx, float dy, float dz, float prevDx, float prevDy, float prevDz)
	{
		for (auto& vertex : geometry.vertices)
		{
			vertex.position[0] += dx;
			vertex.position[1] += dy;
			vertex.position[2] += dz;
			vertex.prevPosition[0] += prevDx;
			vertex.prevPosition[1] += prevDy;
			vertex.prevPosition[2] += prevDz;
		}
	}

	static void AssignGeometryPortalIndices(const nri_scene::PTMapWorld& mapWorld, nri_scene::GeometryData& geometry)
	{
		const size_t count = std::min(geometry.primitives.size(), geometry.primitiveProvenance.size());
		for (size_t i = 0; i < count; ++i)
		{
			geometry.primitives[i].portalIndex = UINT32_MAX;
			const uint32_t flags = geometry.primitives[i].flags;
			if ((flags & (nri_scene::MaterialFlag_Mirror | nri_scene::MaterialFlag_Portal)) == 0)
			{
				continue;
			}

			const int32_t portalIndex = nri_scene::FindMapWorldPortalIndex(mapWorld, geometry.primitiveProvenance[i]);
			if (portalIndex >= 0)
			{
				geometry.primitives[i].portalIndex = (uint32_t)portalIndex;
			}
		}
	}

	static std::vector<ScenePortalData> BuildScenePortalData(const nri_scene::PTMapWorld& mapWorld)
	{
		std::vector<ScenePortalData> portals;
		portals.reserve(std::max<size_t>(mapWorld.portals.size(), 1u));

		for (const auto& portal : mapWorld.portals)
		{
			ScenePortalData data = {};
			data.traversalClass = GetPortalTraversalClass(portal.kind);
			data.kind = (uint32_t)portal.kind;
			data.flags = portal.runtimeBoundTarget ? NRI_PORTAL_FLAG_RUNTIME_BOUND : 0u;
			if (portal.targetCount > 0 && portal.firstTarget < mapWorld.portalTargets.size())
			{
				data.targetLocalSpaceIndex = mapWorld.portalTargets[portal.firstTarget].localSpaceIndex;
			}
			data.delta[0] = (float)portal.delta[0];
			data.delta[1] = (float)portal.delta[1];
			data.delta[2] = (float)portal.delta[2];
			portals.push_back(data);
		}

		if (portals.empty())
		{
			portals.push_back({});
		}

		return portals;
	}

	static void AppendGeometry(const nri_scene::GeometryData& source, uint32_t materialIndexOffset, nri_scene::GeometryData& destination)
	{
		const uint32_t vertexBase = (uint32_t)destination.vertices.size();
		destination.vertices.insert(destination.vertices.end(), source.vertices.begin(), source.vertices.end());

		destination.indices.reserve(destination.indices.size() + source.indices.size());
		for (uint32_t index : source.indices)
		{
			destination.indices.push_back(vertexBase + index);
		}

		destination.primitives.reserve(destination.primitives.size() + source.primitives.size());
		for (const auto& primitive : source.primitives)
		{
			nri_scene::PrimitiveData copy = primitive;
			copy.indices[0] += vertexBase;
			copy.indices[1] += vertexBase;
			copy.indices[2] += vertexBase;
			copy.materialIndex += materialIndexOffset;
			destination.primitives.push_back(copy);
		}

		destination.primitiveProvenance.insert(destination.primitiveProvenance.end(), source.primitiveProvenance.begin(), source.primitiveProvenance.end());
	}

	static void AppendGeometryChunk(
		const nri_scene::GeometryData& source,
		uint32_t sourceVertexOffset,
		uint32_t sourceVertexCount,
		uint32_t sourceIndexOffset,
		uint32_t sourceIndexCount,
		uint32_t sourcePrimitiveOffset,
		uint32_t sourcePrimitiveCount,
		nri_scene::GeometryData& destination)
	{
		if (sourceVertexOffset >= source.vertices.size() ||
			sourcePrimitiveOffset >= source.primitives.size() ||
			sourceVertexCount == 0 ||
			sourcePrimitiveCount == 0)
		{
			return;
		}

		sourceVertexCount = std::min(sourceVertexCount, (uint32_t)source.vertices.size() - sourceVertexOffset);
		if (sourceIndexOffset >= source.indices.size())
		{
			sourceIndexCount = 0;
		}
		else
		{
			sourceIndexCount = std::min(sourceIndexCount, (uint32_t)source.indices.size() - sourceIndexOffset);
		}
		sourcePrimitiveCount = std::min(sourcePrimitiveCount, (uint32_t)source.primitives.size() - sourcePrimitiveOffset);
		const uint32_t sourcePrimitiveProvenanceCount =
			sourcePrimitiveOffset < source.primitiveProvenance.size() ?
			std::min(sourcePrimitiveCount, (uint32_t)source.primitiveProvenance.size() - sourcePrimitiveOffset) :
			0u;

		const uint32_t vertexBase = (uint32_t)destination.vertices.size();
		destination.vertices.insert(
			destination.vertices.end(),
			source.vertices.begin() + sourceVertexOffset,
			source.vertices.begin() + sourceVertexOffset + sourceVertexCount);

		if (sourceIndexCount > 0)
		{
			destination.indices.reserve(destination.indices.size() + sourceIndexCount);
			for (uint32_t i = 0; i < sourceIndexCount; ++i)
			{
				destination.indices.push_back(vertexBase + source.indices[sourceIndexOffset + i] - sourceVertexOffset);
			}
		}

		destination.primitives.reserve(destination.primitives.size() + sourcePrimitiveCount);
		for (uint32_t i = 0; i < sourcePrimitiveCount; ++i)
		{
			nri_scene::PrimitiveData copy = source.primitives[sourcePrimitiveOffset + i];
			copy.indices[0] = vertexBase + copy.indices[0] - sourceVertexOffset;
			copy.indices[1] = vertexBase + copy.indices[1] - sourceVertexOffset;
			copy.indices[2] = vertexBase + copy.indices[2] - sourceVertexOffset;
			destination.primitives.push_back(copy);
		}

		if (sourcePrimitiveProvenanceCount > 0)
		{
			destination.primitiveProvenance.insert(
				destination.primitiveProvenance.end(),
				source.primitiveProvenance.begin() + sourcePrimitiveOffset,
				source.primitiveProvenance.begin() + sourcePrimitiveOffset + sourcePrimitiveProvenanceCount);
		}
	}

	static void AppendMaterialBridge(const nri_scene::MaterialBridgeData& source, nri_scene::MaterialBridgeData& destination)
	{
		std::unordered_map<uint64_t, uint32_t> textureLookup;
		textureLookup.reserve(destination.textures.size() + source.textures.size());
		for (uint32_t i = 0; i < (uint32_t)destination.textures.size(); ++i)
		{
			textureLookup.emplace(destination.textures[i].key, i);
		}

		auto remapTextureIndex = [&source, &destination, &textureLookup](uint32_t textureIndex) -> uint32_t
		{
			if (textureIndex == UINT32_MAX)
			{
				return UINT32_MAX;
			}
			if (textureIndex >= source.textures.size())
			{
				return textureIndex;
			}

			const auto& texture = source.textures[textureIndex];
			auto it = textureLookup.find(texture.key);
			if (it == textureLookup.end())
			{
				const uint32_t newIndex = (uint32_t)destination.textures.size();
				textureLookup.emplace(texture.key, newIndex);
				destination.textures.push_back(texture);
				return newIndex;
			}

			return it->second;
		};

		for (size_t materialIndex = 0; materialIndex < source.materials.size(); ++materialIndex)
		{
			const auto& material = source.materials[materialIndex];
			nri_scene::MaterialData copy = material;
			const bool hasLightMetadata = materialIndex < source.lightMetadata.size();
			copy.textureIndex = remapTextureIndex(material.textureIndex);
			copy.normalTextureIndex = remapTextureIndex(material.normalTextureIndex);
			copy.metallicTextureIndex = remapTextureIndex(material.metallicTextureIndex);
			copy.roughnessTextureIndex = remapTextureIndex(material.roughnessTextureIndex);
			copy.emissiveTextureIndex = remapTextureIndex(material.emissiveTextureIndex);

			destination.materials.push_back(copy);
			if (hasLightMetadata)
			{
				nri_scene::MaterialLightingMetadata metadata = source.lightMetadata[materialIndex];
				metadata.textureIndex = remapTextureIndex(metadata.textureIndex);
				metadata.glowmapTextureIndex = remapTextureIndex(metadata.glowmapTextureIndex);
				metadata.normalTextureIndex = remapTextureIndex(metadata.normalTextureIndex);
				metadata.metallicTextureIndex = remapTextureIndex(metadata.metallicTextureIndex);
				metadata.roughnessTextureIndex = remapTextureIndex(metadata.roughnessTextureIndex);
				metadata.emissiveTextureIndex = remapTextureIndex(metadata.emissiveTextureIndex);
				destination.lightMetadata.push_back(metadata);
			}
		}

		if (destination.paletteLookup.empty())
		{
			destination.paletteLookup = source.paletteLookup;
			destination.paletteWidth = source.paletteWidth;
			destination.paletteHeight = source.paletteHeight;
		}
	}

	static bool MaterialDataEqual(const nri_scene::MaterialData& a, const nri_scene::MaterialData& b)
	{
		return
			a.textureIndex == b.textureIndex &&
			a.paletteIndex == b.paletteIndex &&
			a.flags == b.flags &&
			a.materialClass == b.materialClass &&
			a.lightingFlags == b.lightingFlags &&
			a.normalTextureIndex == b.normalTextureIndex &&
			a.metallicTextureIndex == b.metallicTextureIndex &&
			a.roughnessTextureIndex == b.roughnessTextureIndex &&
			a.sectorIndex == b.sectorIndex &&
			a.emissiveTextureIndex == b.emissiveTextureIndex &&
			a.lightLevel == b.lightLevel &&
			a.alpha == b.alpha &&
			a.roughnessHint == b.roughnessHint &&
			a.metalnessHint == b.metalnessHint &&
			a.emissiveColor[0] == b.emissiveColor[0] &&
			a.emissiveColor[1] == b.emissiveColor[1] &&
			a.emissiveColor[2] == b.emissiveColor[2] &&
			a.emissiveIntensity == b.emissiveIntensity &&
			a.emissiveMaskScale == b.emissiveMaskScale &&
			a.emissiveMode == b.emissiveMode &&
			a.emissiveReserved == b.emissiveReserved;
	}

	static bool MaterialDataVectorEqual(const std::vector<nri_scene::MaterialData>& a, const std::vector<nri_scene::MaterialData>& b)
	{
		if (a.size() != b.size())
		{
			return false;
		}

		for (size_t i = 0; i < a.size(); ++i)
		{
			if (!MaterialDataEqual(a[i], b[i]))
			{
				return false;
			}
		}

		return true;
	}

	static float GetUpscalerRenderScale(nri::UpscalerMode mode)
	{
		switch (mode)
		{
		default:
		case nri::UpscalerMode::NATIVE: return 1.0f;
		case nri::UpscalerMode::ULTRA_QUALITY: return 1.0f / 1.3f;
		case nri::UpscalerMode::QUALITY: return 1.0f / 1.5f;
		case nri::UpscalerMode::BALANCED: return 1.0f / 1.7f;
		case nri::UpscalerMode::PERFORMANCE: return 0.5f;
		case nri::UpscalerMode::ULTRA_PERFORMANCE: return 1.0f / 3.0f;
		}
	}

	static uint32_t GetUpscalerJitterPhaseCount(nri::UpscalerMode mode)
	{
		switch (mode)
		{
		case nri::UpscalerMode::NATIVE: return 8u;
		case nri::UpscalerMode::ULTRA_QUALITY: return 14u;
		case nri::UpscalerMode::QUALITY: return 18u;
		case nri::UpscalerMode::BALANCED: return 23u;
		case nri::UpscalerMode::PERFORMANCE: return 32u;
		case nri::UpscalerMode::ULTRA_PERFORMANCE: return 72u;
		default: return 8u;
		}
	}

	static nri::UpscalerMode ResolveUpscalerModeForMain(NRIMainUpscalerKind kind, nri::UpscalerMode requestedMode)
	{
		switch (kind)
		{
		case NRIMainUpscalerKind::DLRR:
			return requestedMode;
		case NRIMainUpscalerKind::DLSR:
			return requestedMode;
		default:
			return requestedMode;
		}
	}

	static float ResolveRenderScaleForMain(NRIMainUpscalerKind kind, nri::UpscalerMode requestedMode, float manualRenderScale)
	{
		switch (kind)
		{
		case NRIMainUpscalerKind::DLSR:
			return GetUpscalerRenderScale(requestedMode);
		case NRIMainUpscalerKind::DLRR:
			return GetUpscalerRenderScale(requestedMode);
		default:
			return manualRenderScale;
		}
	}

	static const char* GetRenderResolutionPolicyName(NRIMainUpscalerKind kind)
	{
		switch (kind)
		{
		case NRIMainUpscalerKind::DLSR: return "sr-mode-scale";
		case NRIMainUpscalerKind::DLRR: return "rr-mode-scale";
		default: return "manual-scale";
		}
	}

	static void SyncLegacyUpscalerConfig(bool logMigration)
	{
		if ((int)nri_upscaler == 1)
		{
			nri_upscaler = 0;
			if ((int)nri_postsharpen == 0)
			{
				nri_postsharpen = 1;
			}

			static bool loggedLegacyNisMigration = false;
			if (logMigration && !loggedLegacyNisMigration)
			{
				Printf("NRI upscaler config: migrated legacy nri_upscaler=1 (NIS) to nri_upscaler=0 + nri_postsharpen=1\n");
				loggedLegacyNisMigration = true;
			}
		}

		const int clampedMainUpscaler =
			(int)nri_upscaler == 0 || (int)nri_upscaler == 2 || (int)nri_upscaler == 3
			? (int)nri_upscaler
			: 0;
		if ((int)nri_upscaler != clampedMainUpscaler)
		{
			const int invalidValue = (int)nri_upscaler;
			nri_upscaler = clampedMainUpscaler;

			static int lastLoggedInvalidMainUpscaler = std::numeric_limits<int>::min();
			if (logMigration && lastLoggedInvalidMainUpscaler != invalidValue)
			{
				Printf("NRI upscaler config: invalid main upscaler value %d, forcing off\n", invalidValue);
				lastLoggedInvalidMainUpscaler = invalidValue;
			}
		}

		const int clampedPostSharpen = (int)nri_postsharpen == 1 ? 1 : 0;
		if ((int)nri_postsharpen != clampedPostSharpen)
		{
			const int invalidValue = (int)nri_postsharpen;
			nri_postsharpen = clampedPostSharpen;

			static int lastLoggedInvalidPostSharpen = std::numeric_limits<int>::min();
			if (logMigration && lastLoggedInvalidPostSharpen != invalidValue)
			{
				Printf("NRI upscaler config: invalid post sharpen value %d, forcing off\n", invalidValue);
				lastLoggedInvalidPostSharpen = invalidValue;
			}
		}
	}

	static const char* GetMainUpscalerName(NRIMainUpscalerKind kind)
	{
		switch (kind)
		{
		case NRIMainUpscalerKind::DLSR: return "DLSS-SR";
		case NRIMainUpscalerKind::DLRR: return "DLRR";
		default: return "off";
		}
	}

	static const char* GetPostSharpenName(NRIPostSharpenKind kind)
	{
		switch (kind)
		{
		case NRIPostSharpenKind::NIS: return "NIS";
		default: return "off";
		}
	}

	static const char* GetUpscalerModeName(nri::UpscalerMode mode)
	{
		switch (mode)
		{
		case nri::UpscalerMode::ULTRA_QUALITY: return "ultra_quality";
		case nri::UpscalerMode::QUALITY: return "quality";
		case nri::UpscalerMode::BALANCED: return "balanced";
		case nri::UpscalerMode::PERFORMANCE: return "performance";
		case nri::UpscalerMode::ULTRA_PERFORMANCE: return "ultra_performance";
		default: return "native";
		}
	}

	static const char* GetUpscalerFamilyName(NRIMainUpscalerKind kind, bool runAppTaa)
	{
		switch (kind)
		{
		case NRIMainUpscalerKind::DLSR: return "vendor-sr";
		case NRIMainUpscalerKind::DLRR: return "vendor-rr";
		default: return runAppTaa ? "native-taa" : "native";
		}
	}

	static nri::UpscalerType ToMainUpscalerType(NRIMainUpscalerKind kind)
	{
		switch (kind)
		{
		case NRIMainUpscalerKind::DLSR: return nri::UpscalerType::DLSR;
		case NRIMainUpscalerKind::DLRR: return nri::UpscalerType::DLRR;
		default: return nri::UpscalerType::NIS;
		}
	}

	static nri::UpscalerType ToPostSharpenType(NRIPostSharpenKind kind)
	{
		switch (kind)
		{
		case NRIPostSharpenKind::NIS: return nri::UpscalerType::NIS;
		default: return nri::UpscalerType::NIS;
		}
	}

	struct NRITraceSceneConstants
	{
		float CameraPos[3] = {};
		uint32_t RenderWidth = 0;
		float CameraForward[3] = {};
		uint32_t RenderHeight = 0;
		float CameraRight[3] = {};
		float TanHalfFovX = 1.0f;
		float CameraUp[3] = {};
		float TanHalfFovY = 1.0f;
		float PrevCameraPos[3] = {};
		uint32_t DisplayWidth = 0;
		float PrevCameraForward[3] = {};
		uint32_t DisplayHeight = 0;
		float PrevCameraRight[3] = {};
		float PrevTanHalfFovX = 1.0f;
		float PrevCameraUp[3] = {};
		float PrevTanHalfFovY = 1.0f;
		float LightDirection[3] = { 0.3f, 0.85f, -0.4f };
		uint32_t SceneInstanceCount = 0;
		float SkyColor[3] = { 0.38f, 0.48f, 0.65f };
		uint32_t DebugMode = 0;
		float GroundColor[3] = { 0.08f, 0.08f, 0.08f };
		uint32_t StaticPrimitiveCount = 0;
		uint32_t FrameIndex = 0;
		uint32_t DynamicPrimitiveCount = 0;
		uint32_t Flags = 0;
		uint32_t StaticMaterialCount = 0;
		uint32_t BootstrapMode = 0;
		uint32_t DynamicMaterialCount = 0;
		uint32_t BounceCounts = 0;
		uint32_t PortalCount = 0;
		uint32_t RuntimeLightCount = 0;
		uint32_t PortalDepth = 0;
		uint32_t ReservedTrace0 = 0;
		uint32_t ReservedTrace1 = 0;
	};

	struct NRITemporalConstants
	{
		uint32_t RenderWidth = 0;
		uint32_t RenderHeight = 0;
		uint32_t FrameIndex = 0;
		uint32_t Flags = 0;
		float Exposure = 1.0f;
	};

	struct NRIPresentConstants
	{
		uint32_t InputWidth = 0;
		uint32_t InputHeight = 0;
		uint32_t DisplayWidth = 0;
		uint32_t DisplayHeight = 0;
		uint32_t PackedSceneOrigin = 0;
		uint32_t FrameIndex = 0;
		uint32_t DebugMode = 0;
		uint32_t Flags = 0;
		uint32_t DenoiserMode = 0;
		uint32_t OutputMode = 0;
		uint32_t TonemapMode = 0;
		uint32_t OutputFlags = 0;
		float Exposure = 1.0f;
		float Contrast = 1.0f;
		float Saturation = 1.0f;
		float Shoulder = 1.0f;
		float Toe = 1.0f;
		float PaperWhiteNits = 200.0f;
		float DisplayMaxLuminance = 80.0f;
		float DisplaySdrLuminance = 80.0f;
		uint32_t NightVisionPackedModeTint = 0;
		float NightVisionStrength = 0.0f;
		float NightVisionExposure = 1.0f;
		uint32_t NightVisionPackedControls = 0;
	};

	struct NRIReprojectionData
	{
		float currentViewToClip[16] = {};
		float previousViewToClip[16] = {};
		float currentWorldToView[16] = {};
		float previousWorldToView[16] = {};
	};

	static_assert(sizeof(NRITraceSceneConstants) <= 224, "NRITraceSceneConstants must stay within the validated shared root-constant budget.");
	static_assert(sizeof(NRITemporalConstants) <= 32, "NRITemporalConstants must stay compact.");
	static_assert(sizeof(NRIPresentConstants) <= 96, "NRIPresentConstants must stay compact.");

	static uint32_t PackPresentSceneOrigin(int sceneLeft, int sceneTop)
	{
		return (uint16_t)(int16_t)sceneLeft | ((uint32_t)(uint16_t)(int16_t)sceneTop << 16);
	}

	static void ApplyOutputPolicyToPresentConstants(const NRIPTOutputPolicy& policy, NRIPresentConstants& constants)
	{
		constants.OutputMode = (uint32_t)policy.resolvedMode;
		constants.TonemapMode = (uint32_t)policy.tonemapMode;
		constants.OutputFlags =
			(policy.displayInfoAvailable ? NRI_PRESENT_OUTPUT_FLAG_DISPLAY_INFO_AVAILABLE : 0u) |
			(policy.displayHdrSupported ? NRI_PRESENT_OUTPUT_FLAG_DISPLAY_HDR_SUPPORTED : 0u) |
			(policy.hdrSwapChainActive ? NRI_PRESENT_OUTPUT_FLAG_HDR_SWAPCHAIN_ACTIVE : 0u) |
			(policy.offscreenHdrTarget ? NRI_PRESENT_OUTPUT_FLAG_OFFSCREEN_HDR_TARGET : 0u);
		constants.Exposure = policy.exposure;
		constants.Contrast = policy.contrast;
		constants.Saturation = policy.saturation;
		constants.Shoulder = policy.shoulder;
		constants.Toe = policy.toe;
		constants.PaperWhiteNits = policy.paperWhiteNits;
		constants.DisplayMaxLuminance = policy.displayMaxLuminance;
		constants.DisplaySdrLuminance = policy.displaySdrLuminance;
	}

	static void ApplyNightVisionStateToPresentConstants(const NRIPTNightVisionState& state, NRIPresentConstants& constants)
	{
		constants.NightVisionPackedModeTint = PackNightVisionModeAndTint(
			state.mode,
			(float)nri_ptnightvisionred,
			(float)nri_ptnightvisiongreen,
			(float)nri_ptnightvisionblue);
		constants.NightVisionStrength = nri_ptnightvision ? state.strength01 : 0.0f;
		constants.NightVisionExposure = (float)nri_ptnightvisionexposure;
		constants.NightVisionPackedControls = PackNightVisionControls(
			(float)nri_ptnightvisioncontrast,
			(float)nri_ptnightvisionsaturation);
	}

	static bool IsAppTaaEligibleUpscaler(NRIMainUpscalerKind kind)
	{
		return kind == NRIMainUpscalerKind::Off;
	}

	static bool ShouldRunAppTaa(NRIMainUpscalerKind kind)
	{
		return IsAppTaaEligibleUpscaler(kind) && !!nri_pttaa;
	}

	static bool ShouldUseTemporalJitter(NRIMainUpscalerKind kind)
	{
		return ShouldRunAppTaa(kind) || kind == NRIMainUpscalerKind::DLSR || kind == NRIMainUpscalerKind::DLRR;
	}

	static const char* GetTemporalJitterModeName(NRIMainUpscalerKind kind, bool guiCaptureActive)
	{
		if (guiCaptureActive)
		{
			return "off-gui-capture";
		}

		if (kind == NRIMainUpscalerKind::DLSR || kind == NRIMainUpscalerKind::DLRR)
		{
			return "upscaler";
		}

		return ShouldRunAppTaa(kind) ? "taa" : "off";
	}

	static uint32_t GetTemporalJitterPhaseCount(NRIMainUpscalerKind kind, nri::UpscalerMode mode, bool guiCaptureActive)
	{
		if (guiCaptureActive)
		{
			return 0u;
		}

		if (kind == NRIMainUpscalerKind::DLSR || kind == NRIMainUpscalerKind::DLRR)
		{
			return GetUpscalerJitterPhaseCount(mode);
		}

		return NRI_TAA_JITTER_PHASE_COUNT;
	}

	static float GetHaltonSample(uint32_t index, uint32_t base)
	{
		float inverseBase = 1.0f / (float)base;
		float fraction = inverseBase;
		float result = 0.0f;

		while (index > 0)
		{
			result += fraction * (float)(index % base);
			index /= base;
			fraction *= inverseBase;
		}

		return result;
	}

	static void ComputeTemporalJitter(uint32_t frameIndex, float outJitter[2])
	{
		const uint32_t sampleIndex = (frameIndex % NRI_TAA_JITTER_PHASE_COUNT) + 1u;
		outJitter[0] = GetHaltonSample(sampleIndex, 2u) - 0.5f;
		outJitter[1] = GetHaltonSample(sampleIndex, 3u) - 0.5f;
	}

	static void Normalize3(float v[3])
	{
		const float length = std::max(0.0001f, sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}

	static void ApplyDirectionalLightStateToConstants(const NRIDirectionalLightState& state, NRITraceSceneConstants& constants)
	{
		constants.LightDirection[0] = state.direction[0];
		constants.LightDirection[1] = state.direction[1];
		constants.LightDirection[2] = state.direction[2];
		Normalize3(constants.LightDirection);
	}

	static void TransformPoint(const VSMatrix& matrix, float x, float y, float z, float out[4])
	{
		float point[4] = { x, y, z, 1.0f };
		VSMatrix copy = matrix;
		copy.multMatrixPoint(point, out);
	}

	static bool StatsDiffer(const nri_scene::SceneDebugStats& a, const nri_scene::SceneDebugStats& b)
	{
		return memcmp(&a, &b, sizeof(a)) != 0;
	}

	static void Copy3(const float* src, float* dst)
	{
		std::memcpy(dst, src, sizeof(float) * 3);
	}

	static void Copy2(const float* src, float* dst)
	{
		std::memcpy(dst, src, sizeof(float) * 2);
	}

	static const char* YesNo(bool value)
	{
		return value ? "yes" : "no";
	}

	static bool HasAutoEmissiveSourceFlags(uint32_t sourceFlags)
	{
		return (sourceFlags & (
			SceneEmissiveSurfaceSourceFlag_AutoFullbright |
			SceneEmissiveSurfaceSourceFlag_AutoTextureGlow |
			SceneEmissiveSurfaceSourceFlag_AutoGlowmap)) != 0;
	}

	struct SkyFaceUpload
	{
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> pixels;
	};

	struct SkyFaceProbe
	{
		FGameTexture* texture = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		uint64_t contentId = 0;
	};

	struct SkyProbe
	{
		uint64_t key = 0;
		uint32_t width = 1;
		uint32_t height = 1;
		std::array<SkyFaceProbe, 6> faces = {};
	};

	struct SkyUpload
	{
		uint64_t key = 0;
		bool cubemap = false;
		uint32_t width = 1;
		uint32_t height = 1;
		std::array<SkyFaceUpload, 6> faces = {};
	};

	static uint64_t HashBytes64(const uint8_t* data, size_t size)
	{
		uint64_t hash = 1469598103934665603ull;
		for (size_t i = 0; i < size; ++i)
		{
			hash ^= (uint64_t)data[i];
			hash *= 1099511628211ull;
		}
		return hash;
	}

	static uint64_t HashCombine64(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
	}

	static uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	static float GetSectorLightMultiplier()
	{
		return std::max(0.0f, (float)nri_ptsectorlightmultiplier);
	}

	static uint64_t HashGeometryForEmissiveSampling(const nri_scene::GeometryData* geometry)
	{
		uint64_t hash = 1469598103934665603ull;
		if (geometry == nullptr)
		{
			return HashCombine64(hash, 0ull);
		}

		hash = HashCombine64(hash, (uint64_t)geometry->vertices.size());
		hash = HashCombine64(hash, (uint64_t)geometry->primitives.size());
		for (const nri_scene::SceneVertex& vertex : geometry->vertices)
		{
			hash = HashCombine64(hash, (uint64_t)FloatBits(vertex.position[0]));
			hash = HashCombine64(hash, (uint64_t)FloatBits(vertex.position[1]));
			hash = HashCombine64(hash, (uint64_t)FloatBits(vertex.position[2]));
		}

		for (const nri_scene::PrimitiveData& primitive : geometry->primitives)
		{
			hash = HashCombine64(hash, (uint64_t)primitive.indices[0]);
			hash = HashCombine64(hash, (uint64_t)primitive.indices[1]);
			hash = HashCombine64(hash, (uint64_t)primitive.indices[2]);
			hash = HashCombine64(hash, (uint64_t)primitive.materialIndex);
		}

		return hash;
	}

	static uint32_t GetAnimatedTextureId(FGameTexture* texture)
	{
		return texture != nullptr ? (uint32_t)texture->GetID().GetIndex() : 0u;
	}

	static uint64_t HashAnimatedLayerTexture(FTexture* texture)
	{
		return texture != nullptr ? (uint64_t)(uintptr_t)texture : 0ull;
	}

	static uint64_t HashAnimatedTextureBindingSignature(FGameTexture* texture)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, (uint64_t)GetAnimatedTextureId(texture));
		if (texture == nullptr)
		{
			return hash;
		}

		hash = HashCombine64(hash, HashAnimatedLayerTexture(texture->GetGlowmap()));
		hash = HashCombine64(hash, HashAnimatedLayerTexture(texture->GetNormalmap()));
		hash = HashCombine64(hash, HashAnimatedLayerTexture(texture->GetMetallic()));
		hash = HashCombine64(hash, HashAnimatedLayerTexture(texture->GetRoughness()));
		return hash;
	}

	static uint64_t HashAnimatedTextureDisplaySignature(FGameTexture* texture)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, (uint64_t)GetAnimatedTextureId(texture));
		if (texture == nullptr)
		{
			return hash;
		}

		hash = HashCombine64(hash, (uint64_t)texture->GetDisplayWidth());
		hash = HashCombine64(hash, (uint64_t)texture->GetDisplayHeight());
		hash = HashCombine64(hash, (uint64_t)(uint32_t)texture->GetDisplayLeftOffset());
		hash = HashCombine64(hash, (uint64_t)(uint32_t)texture->GetDisplayTopOffset());
		return hash;
	}

	template <typename SurfaceContainer>
	static void HashAnimatedSurfaces(const SurfaceContainer& surfaces, uint64_t& hash, bool includeDisplaySignature)
	{
		hash = HashCombine64(hash, (uint64_t)surfaces.size());
		for (const auto& surface : surfaces)
		{
			hash = HashCombine64(hash, (uint64_t)(uint32_t)surface.provenance.sourceType);
			hash = HashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.sectorIndex + 1));
			hash = HashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.wallIndex + 1));
			hash = HashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.sectionIndex + 1));
			hash = HashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.actorIndex + 1));
			hash = HashCombine64(hash, (uint64_t)surface.provenance.cstat);
			hash = HashCombine64(hash, (uint64_t)surface.material.flags);
			hash = HashCombine64(hash, (uint64_t)(uint32_t)surface.material.palette);
			hash = HashCombine64(hash, (uint64_t)(uint32_t)surface.material.shade);
			hash = HashCombine64(hash, (uint64_t)FloatBits(surface.material.alpha));
			hash = HashCombine64(hash, HashAnimatedTextureBindingSignature(surface.material.texture));
			if (includeDisplaySignature)
			{
				hash = HashCombine64(hash, HashAnimatedTextureDisplaySignature(surface.material.texture));
			}
		}
	}

	template <typename SurfaceContainer>
	static void HashAnimatedGeometrySurfaces(const SurfaceContainer& surfaces, uint64_t& hash)
	{
		hash = HashCombine64(hash, (uint64_t)surfaces.size());
		for (const auto& surface : surfaces)
		{
			hash = HashCombine64(hash, (uint64_t)(uint32_t)surface.provenance.sourceType);
			hash = HashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.sectorIndex + 1));
			hash = HashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.wallIndex + 1));
			hash = HashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.sectionIndex + 1));
			hash = HashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.actorIndex + 1));
			hash = HashCombine64(hash, (uint64_t)surface.provenance.cstat);
			hash = HashCombine64(hash, HashAnimatedTextureDisplaySignature(surface.material.texture));
		}
	}

	static uint64_t ComputeAnimatedMaterialSignature(const nri_scene::SceneView& sceneView)
	{
		uint64_t hash = 1469598103934665603ull;
		HashAnimatedSurfaces(sceneView.opaqueWalls, hash, false);
		HashAnimatedSurfaces(sceneView.opaqueFlats, hash, false);
		HashAnimatedSurfaces(sceneView.opaqueSprites, hash, false);
		return hash;
	}

	static uint64_t ComputeAnimatedGeometrySignature(const nri_scene::SceneView& sceneView)
	{
		uint64_t hash = 1469598103934665603ull;
		HashAnimatedGeometrySurfaces(sceneView.opaqueWalls, hash);
		HashAnimatedGeometrySurfaces(sceneView.opaqueFlats, hash);
		HashAnimatedGeometrySurfaces(sceneView.opaqueSprites, hash);
		return hash;
	}

	template <typename SurfaceContainer>
	static bool SurfaceContainerUsesHardwareCanvasTexture(const SurfaceContainer& surfaces)
	{
		for (const auto& surface : surfaces)
		{
			if (surface.material.texture != nullptr &&
				surface.material.texture->isHardwareCanvas())
			{
				return true;
			}
		}

		return false;
	}

	static bool SceneViewUsesHardwareCanvasTexture(const nri_scene::SceneView& sceneView)
	{
		return
			SurfaceContainerUsesHardwareCanvasTexture(sceneView.opaqueWalls) ||
			SurfaceContainerUsesHardwareCanvasTexture(sceneView.opaqueFlats) ||
			SurfaceContainerUsesHardwareCanvasTexture(sceneView.opaqueSprites);
	}

	static FTextureID ResolveAuthoredTextureIdForStaticMapSurface(const nri_scene::PTMapSurface& surface)
	{
		switch (surface.kind)
		{
		case nri_scene::PTMapSurfaceKind::Floor:
		{
			const int32_t sectorIndex = surface.surface.provenance.sectorIndex;
			return sectorIndex >= 0 && (unsigned)sectorIndex < sector.Size() ? sector[(unsigned)sectorIndex].floortexture : FNullTextureID();
		}
		case nri_scene::PTMapSurfaceKind::Ceiling:
		{
			const int32_t sectorIndex = surface.surface.provenance.sectorIndex;
			return sectorIndex >= 0 && (unsigned)sectorIndex < sector.Size() ? sector[(unsigned)sectorIndex].ceilingtexture : FNullTextureID();
		}
		case nri_scene::PTMapSurfaceKind::WallOneSided:
		{
			const int32_t wallIndex = surface.surface.provenance.wallIndex;
			if (wallIndex < 0 || (unsigned)wallIndex >= wall.Size())
			{
				return FNullTextureID();
			}

			const walltype& wal = wall[(unsigned)wallIndex];
			return ((wal.cstat & CSTAT_WALL_1WAY) != 0 && wal.nextwall != -1) ? wal.overtexture : wal.walltexture;
		}
		case nri_scene::PTMapSurfaceKind::WallUpper:
		{
			const int32_t wallIndex = surface.surface.provenance.wallIndex;
			return wallIndex >= 0 && (unsigned)wallIndex < wall.Size() ? wall[(unsigned)wallIndex].walltexture : FNullTextureID();
		}
		case nri_scene::PTMapSurfaceKind::WallMiddle:
		{
			const int32_t wallIndex = surface.surface.provenance.wallIndex;
			return wallIndex >= 0 && (unsigned)wallIndex < wall.Size() ? wall[(unsigned)wallIndex].overtexture : FNullTextureID();
		}
		case nri_scene::PTMapSurfaceKind::WallLower:
		{
			const int32_t wallIndex = surface.surface.provenance.wallIndex;
			if (wallIndex < 0 || (unsigned)wallIndex >= wall.Size())
			{
				return FNullTextureID();
			}

			const walltype& wal = wall[(unsigned)wallIndex];
			if ((wal.cstat & CSTAT_WALL_BOTTOM_SWAP) != 0 && wal.nextwall >= 0 && (unsigned)wal.nextwall < wall.Size())
			{
				return wall[(unsigned)wal.nextwall].walltexture;
			}
			return wal.walltexture;
		}
		default:
			return FNullTextureID();
		}
	}

	static bool IsAnimatedStaticMapSurfaceCandidate(const nri_scene::PTMapSurface& surface)
	{
		const FTextureID textureId = ResolveAuthoredTextureIdForStaticMapSurface(surface);
		return textureId.isValid() && GetExtInfo(textureId).picanm.type() != 0;
	}

	static bool ChunkHasAnimatedStaticMapSurfaceCandidates(const nri_scene::PTMapWorld& mapWorld, const nri_scene::PTMapChunk& chunk)
	{
		const uint32_t endSurface = std::min<uint32_t>(chunk.firstSurface + chunk.surfaceCount, (uint32_t)mapWorld.surfaces.size());
		for (uint32_t surfaceIndex = chunk.firstSurface; surfaceIndex < endSurface; ++surfaceIndex)
		{
			if (IsAnimatedStaticMapSurfaceCandidate(mapWorld.surfaces[surfaceIndex]))
			{
				return true;
			}
		}

		return false;
	}

	static bool RefreshAnimatedBindingsForStaticMapChunk(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& chunk,
		nri_scene::SceneView& ioChunkView)
	{
		uint32_t wallSurfaceIndex = 0;
		uint32_t flatSurfaceIndex = 0;
		const uint32_t endSurface = std::min<uint32_t>(chunk.firstSurface + chunk.surfaceCount, (uint32_t)mapWorld.surfaces.size());
		for (uint32_t surfaceIndex = chunk.firstSurface; surfaceIndex < endSurface; ++surfaceIndex)
		{
			const auto& mapSurface = mapWorld.surfaces[surfaceIndex];
			if ((mapSurface.surface.material.flags & nri_scene::MaterialFlag_Sky) != 0 && mapSurface.surface.material.texture != nullptr)
			{
				continue;
			}

			nri_scene::SurfaceRef* targetSurface = nullptr;
			switch (mapSurface.kind)
			{
			case nri_scene::PTMapSurfaceKind::Floor:
			case nri_scene::PTMapSurfaceKind::Ceiling:
				if (flatSurfaceIndex >= ioChunkView.opaqueFlats.size())
				{
					return false;
				}
				targetSurface = &ioChunkView.opaqueFlats[flatSurfaceIndex++];
				break;
			default:
				if (wallSurfaceIndex >= ioChunkView.opaqueWalls.size())
				{
					return false;
				}
				targetSurface = &ioChunkView.opaqueWalls[wallSurfaceIndex++];
				break;
			}

			if (!IsAnimatedStaticMapSurfaceCandidate(mapSurface))
			{
				continue;
			}

			const FTextureID textureId = ResolveAuthoredTextureIdForStaticMapSurface(mapSurface);
			FGameTexture* liveTexture = textureId.isValid() ? TexMan.GetGameTexture(textureId, true) : nullptr;
			targetSurface->material.texture = liveTexture;
		}

		return wallSurfaceIndex == ioChunkView.opaqueWalls.size() && flatSurfaceIndex == ioChunkView.opaqueFlats.size();
	}

	static uint64_t BuildEmissiveTlasInstancePayloadHash(const std::vector<nri::TopLevelInstance>& instances)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, (uint64_t)instances.size());
		for (const nri::TopLevelInstance& instance : instances)
		{
			hash = HashCombine64(hash, (uint64_t)instance.instanceId);
			hash = HashCombine64(hash, (uint64_t)instance.mask);
			hash = HashCombine64(hash, (uint64_t)instance.shaderBindingTableLocalOffset);
			hash = HashCombine64(hash, (uint64_t)instance.flags);
			hash = HashCombine64(hash, instance.accelerationStructureHandle);
			for (uint32_t row = 0; row < 3; ++row)
			{
				hash = HashCombine64(hash, (uint64_t)FloatBits(instance.transform[row][0]));
				hash = HashCombine64(hash, (uint64_t)FloatBits(instance.transform[row][1]));
				hash = HashCombine64(hash, (uint64_t)FloatBits(instance.transform[row][2]));
				hash = HashCombine64(hash, (uint64_t)FloatBits(instance.transform[row][3]));
			}
		}

		return hash;
	}

	static bool IsUsableGameTexturePointer(FGameTexture* texture)
	{
		const intptr_t value = (intptr_t)texture;
		return value > 0x10000 && value != -1;
	}

	static FTexture* TryGetBaseTexture(FGameTexture* texture)
	{
		if (!IsUsableGameTexturePointer(texture))
		{
			return nullptr;
		}

		FTexture* baseTexture = nullptr;
		__try
		{
			baseTexture = texture->GetTexture();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			baseTexture = nullptr;
		}

		return baseTexture;
	}

	static FGameTexture* TryGetSkyFace(FSkyBox* skybox, int index)
	{
		if (skybox == nullptr || index < 0 || index >= 6)
		{
			return nullptr;
		}

		FGameTexture* face = nullptr;
		__try
		{
			face = skybox->GetSkyFace(index);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			face = nullptr;
		}

		return IsUsableGameTexturePointer(face) ? face : nullptr;
	}

	static uint64_t HashSkyColor(const float* color)
	{
		const uint8_t rgba[4] = {
			(uint8_t)std::clamp((int)std::lround(Clamp01(color[0]) * 255.0f), 0, 255),
			(uint8_t)std::clamp((int)std::lround(Clamp01(color[1]) * 255.0f), 0, 255),
			(uint8_t)std::clamp((int)std::lround(Clamp01(color[2]) * 255.0f), 0, 255),
			255
		};
		return HashBytes64(rgba, sizeof(rgba));
	}

	static void FlipImageHorizontal(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height)
	{
		for (uint32_t y = 0; y < height; ++y)
		{
			uint8_t* row = pixels.data() + (size_t)y * width * 4u;
			for (uint32_t x = 0; x < width / 2; ++x)
			{
				uint8_t* a = row + x * 4u;
				uint8_t* b = row + (width - 1 - x) * 4u;
				for (uint32_t c = 0; c < 4; ++c)
				{
					std::swap(a[c], b[c]);
				}
			}
		}
	}

	static void FlipImageVertical(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height)
	{
		const size_t rowSize = (size_t)width * 4u;
		std::vector<uint8_t> temp(rowSize);
		for (uint32_t y = 0; y < height / 2; ++y)
		{
			uint8_t* a = pixels.data() + (size_t)y * rowSize;
			uint8_t* b = pixels.data() + (size_t)(height - 1 - y) * rowSize;
			std::memcpy(temp.data(), a, rowSize);
			std::memcpy(a, b, rowSize);
			std::memcpy(b, temp.data(), rowSize);
		}
	}

	static bool CopyFacePixels(FGameTexture* texture, SkyFaceUpload& outFace)
	{
		FTexture* baseTexture = TryGetBaseTexture(texture);
		if (baseTexture == nullptr || baseTexture->GetImage() == nullptr)
		{
			return false;
		}

		FTextureBuffer texBuffer = baseTexture->CreateTexBuffer(0, CTF_ProcessData);
		if (texBuffer.mBuffer == nullptr || texBuffer.mWidth <= 0 || texBuffer.mHeight <= 0)
		{
			return false;
		}

		outFace.width = (uint32_t)texBuffer.mWidth;
		outFace.height = (uint32_t)texBuffer.mHeight;
		outFace.pixels.assign(texBuffer.mBuffer, texBuffer.mBuffer + (size_t)texBuffer.mWidth * (size_t)texBuffer.mHeight * 4u);
		return true;
	}

	static bool ProbeFace(FGameTexture* texture, SkyFaceProbe& outFace)
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.probeFaceCalls++;
		}
		ScopedSkyPerfTimer timer(gRendererSkyPerfTraceStats.probeFaceTimeUs);
		FTexture* baseTexture = TryGetBaseTexture(texture);
		if (baseTexture == nullptr || baseTexture->GetImage() == nullptr)
		{
			return false;
		}

		const int width = baseTexture->GetWidth();
		const int height = baseTexture->GetHeight();
		if (width <= 0 || height <= 0)
		{
			return false;
		}

		FContentIdBuilder contentId = {};
		contentId.imageID = baseTexture->GetImage()->GetId();
		contentId.translation = 0;
		contentId.expand = 0;
		contentId.scaler = 0;
		contentId.scalefactor = 0;

		outFace.texture = texture;
		outFace.width = (uint32_t)width;
		outFace.height = (uint32_t)height;
		outFace.contentId = contentId.id != 0 ? contentId.id : (uint64_t)(uintptr_t)texture;
		return true;
	}

	static bool ProbeCubemapSky(const nri_scene::SceneView& sceneView, SkyProbe& outProbe)
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.probeAttempts++;
		}
		ScopedSkyPerfTimer timer(gRendererSkyPerfTraceStats.probeCubemapTimeUs);
		if (sceneView.sky.mode != nri_scene::PTSkyMode::Cubemap || !IsUsableGameTexturePointer(sceneView.sky.texture))
		{
			return false;
		}

		auto* skybox = dynamic_cast<FSkyBox*>(TryGetBaseTexture(sceneView.sky.texture));
		if (skybox == nullptr)
		{
			return false;
		}

		struct FaceMapping
		{
			int sourceIndex;
			bool flipHorizontal;
			bool flipVertical;
		};

		// Build sky faces are ordered north, east, south, west, top, bottom.
		// The PT cubemap follows the conventional +X, -X, +Y, -Y, +Z, -Z order.
		// Top and bottom need explicit flips to match the ray-space basis used by the PT shaders.
		static const FaceMapping mappings[6] = {
			{ 3, false, false }, // +X = west
			{ 1, false, false }, // -X = east
			{ 4, true, false },  // +Y = top
			{ 5, true, true },   // -Y = bottom
			{ 2, false, false }, // +Z = south
			{ 0, false, false }  // -Z = north
		};

		uint64_t key = HashCombine64(1469598103934665603ull, (uint64_t)(uintptr_t)sceneView.sky.texture);
		key = HashCombine64(key, (uint64_t)sceneView.sky.faceMask);
		key = HashCombine64(key, sceneView.sky.flipTop ? 1ull : 0ull);
		for (uint32_t i = 0; i < 6; ++i)
		{
			if (!ProbeFace(TryGetSkyFace(skybox, mappings[i].sourceIndex), outProbe.faces[i]))
			{
				return false;
			}

			key = HashCombine64(key, (uint64_t)(uintptr_t)outProbe.faces[i].texture);
			key = HashCombine64(key, outProbe.faces[i].contentId);
			key = HashCombine64(key, ((uint64_t)outProbe.faces[i].width << 32) | outProbe.faces[i].height);
		}

		outProbe.width = outProbe.faces[0].width;
		outProbe.height = outProbe.faces[0].height;
		for (uint32_t i = 1; i < 6; ++i)
		{
			if (outProbe.faces[i].width != outProbe.width || outProbe.faces[i].height != outProbe.height)
			{
				return false;
			}
		}

		outProbe.key = key;
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.probeSuccesses++;
		}
		return true;
	}

	static bool BuildCubemapUpload(const nri_scene::SceneView& sceneView, const SkyProbe& probe, SkyUpload& outUpload)
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.buildCubemapUploadCalls++;
		}
		ScopedSkyPerfTimer timer(gRendererSkyPerfTraceStats.buildCubemapUploadTimeUs);
		struct FaceMapping
		{
			bool flipHorizontal;
			bool flipVertical;
		};

		// Build sky faces are ordered north, east, south, west, top, bottom.
		// The PT cubemap follows the conventional +X, -X, +Y, -Y, +Z, -Z order.
		// Top and bottom need explicit flips to match the ray-space basis used by the PT shaders.
		static const FaceMapping mappings[6] = {
			{ false, false }, // +X = west
			{ false, false }, // -X = east
			{ true, false },  // +Y = top
			{ true, true },   // -Y = bottom
			{ false, false }, // +Z = south
			{ false, false }  // -Z = north
		};

		for (uint32_t i = 0; i < 6; ++i)
		{
			if (!CopyFacePixels(probe.faces[i].texture, outUpload.faces[i]))
			{
				return false;
			}

			if (i == 2 && sceneView.sky.flipTop)
			{
				FlipImageVertical(outUpload.faces[i].pixels, outUpload.faces[i].width, outUpload.faces[i].height);
			}
			if (mappings[i].flipHorizontal)
			{
				FlipImageHorizontal(outUpload.faces[i].pixels, outUpload.faces[i].width, outUpload.faces[i].height);
			}
			if (mappings[i].flipVertical)
			{
				FlipImageVertical(outUpload.faces[i].pixels, outUpload.faces[i].width, outUpload.faces[i].height);
			}
		}

		outUpload.key = probe.key;
		outUpload.width = probe.width;
		outUpload.height = probe.height;
		outUpload.cubemap = true;
		return true;
	}

	static void BuildSolidSkyUpload(const float* skyColor, SkyUpload& outUpload)
	{
		outUpload = {};
		outUpload.key = HashSkyColor(skyColor) ^ 0x53594b59554c4c45ull;
		for (auto& face : outUpload.faces)
		{
			face.width = 1;
			face.height = 1;
			face.pixels = {
				(uint8_t)std::clamp((int)std::lround(Clamp01(skyColor[2]) * 255.0f), 0, 255),
				(uint8_t)std::clamp((int)std::lround(Clamp01(skyColor[1]) * 255.0f), 0, 255),
				(uint8_t)std::clamp((int)std::lround(Clamp01(skyColor[0]) * 255.0f), 0, 255),
				255
			};
		}
	}

	static void RemapToPTSpace(const float* src, float* dst)
	{
		dst[0] = src[0];
		dst[1] = src[2];
		dst[2] = src[1];
	}

	static uint32_t GetBootstrapMode()
	{
		return (uint32_t)std::max(0, std::min((int)nri_ptbootstrapmode, 13));
	}

	static bool IntersectProbeTriangle(const nri_scene::SceneVertex& v0, const nri_scene::SceneVertex& v1, const nri_scene::SceneVertex& v2, const float origin[3], const float direction[3], float& outT)
	{
		outT = 0.0f;
		const float edge1[3] = {
			v1.position[0] - v0.position[0],
			v1.position[1] - v0.position[1],
			v1.position[2] - v0.position[2]
		};
		const float edge2[3] = {
			v2.position[0] - v0.position[0],
			v2.position[1] - v0.position[1],
			v2.position[2] - v0.position[2]
		};
		const float p[3] = {
			direction[1] * edge2[2] - direction[2] * edge2[1],
			direction[2] * edge2[0] - direction[0] * edge2[2],
			direction[0] * edge2[1] - direction[1] * edge2[0]
		};
		const float det = edge1[0] * p[0] + edge1[1] * p[1] + edge1[2] * p[2];
		if (fabsf(det) < 1e-5f)
		{
			return false;
		}

		const float invDet = 1.0f / det;
		const float t[3] = {
			origin[0] - v0.position[0],
			origin[1] - v0.position[1],
			origin[2] - v0.position[2]
		};
		const float u = (t[0] * p[0] + t[1] * p[1] + t[2] * p[2]) * invDet;
		if (u < 0.0f || u > 1.0f)
		{
			return false;
		}

		const float q[3] = {
			t[1] * edge1[2] - t[2] * edge1[1],
			t[2] * edge1[0] - t[0] * edge1[2],
			t[0] * edge1[1] - t[1] * edge1[0]
		};
		const float v = (direction[0] * q[0] + direction[1] * q[1] + direction[2] * q[2]) * invDet;
		if (v < 0.0f || (u + v) > 1.0f)
		{
			return false;
		}

		const float hitT = (edge2[0] * q[0] + edge2[1] * q[1] + edge2[2] * q[2]) * invDet;
		if (hitT <= 0.001f)
		{
			return false;
		}

		outT = hitT;
		return true;
	}

	static const char* GetSurfaceSourceTypeName(nri_scene::SurfaceSourceType sourceType)
	{
		switch (sourceType)
		{
		case nri_scene::SurfaceSourceType::DrawListWall: return "draw_list_wall";
		case nri_scene::SurfaceSourceType::MirrorWall: return "mirror_wall";
		case nri_scene::SurfaceSourceType::FloorFlat: return "floor_flat";
		case nri_scene::SurfaceSourceType::CeilingFlat: return "ceiling_flat";
		case nri_scene::SurfaceSourceType::FacingSprite: return "facing_sprite";
		case nri_scene::SurfaceSourceType::VoxelProxySprite: return "voxel_proxy_sprite";
		case nri_scene::SurfaceSourceType::MapWallBand: return "map_wall_band";
		case nri_scene::SurfaceSourceType::MapFloorSection: return "map_floor_section";
		case nri_scene::SurfaceSourceType::MapCeilingSection: return "map_ceiling_section";
		case nri_scene::SurfaceSourceType::MapPortalSurface: return "map_portal_surface";
		case nri_scene::SurfaceSourceType::DebugSphere: return "debug_sphere";
		default: return "unknown";
		}
	}

	static const char* GetDrawListTypeName(uint32_t drawListType)
	{
		switch (drawListType)
		{
		case GLDL_PLAINWALLS: return "plain_walls";
		case GLDL_MASKEDWALLS: return "masked_walls";
		case GLDL_MASKEDWALLSS: return "masked_walls_split";
		case GLDL_MASKEDWALLSD: return "masked_walls_decal";
		case GLDL_MASKEDWALLSV: return "masked_walls_view";
		case GLDL_MASKEDWALLSH: return "masked_walls_horizon";
		case GLDL_TRANSLUCENTBORDER: return "translucent_border";
		case GLDL_PLAINFLATS: return "plain_flats";
		case GLDL_MASKEDFLATS: return "masked_flats";
		case GLDL_MASKEDSLOPEFLATS: return "masked_slope_flats";
		case GLDL_TRANSLUCENT: return "translucent";
		case GLDL_MODELS: return "models";
		case UINT32_MAX: return "none";
		default: return "unknown";
		}
	}

	static const char* GetSceneLightRecordSourceName(SceneLightRecordSource source)
	{
		switch (source)
		{
		case SceneLightRecordSource::CapturedScene: return "captured_scene";
		case SceneLightRecordSource::StaticMapScene: return "static_map_scene";
		case SceneLightRecordSource::RuntimeMutationScene: return "runtime_mutation_scene";
		case SceneLightRecordSource::DynamicScene: return "dynamic_scene";
		default: return "none";
		}
	}

}

NRIRenderer::NRIRenderer(NRIRenderDevice* frameBuffer)
	: mFrameBuffer(frameBuffer)
{
}

NRIRenderer::~NRIRenderer()
{
	Shutdown();
}

bool NRIRenderer::Initialize()
{
	Clocker clock(NriPTInitialize);

	if (mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return false;
	}

	if (!mSceneTextureLimitLogPrinted)
	{
		LogSceneTextureDescriptorLimits(mFrameBuffer->mCore.GetDeviceDesc(*mFrameBuffer->mDevice));
		mSceneTextureLimitLogPrinted = true;
	}

	if (!CheckPathTracingSupport())
	{
		return true;
	}

	if (mPipelineLayout != nullptr)
	{
		return true;
	}

	return CreatePipelineLayout() && CreateTaaPipelineLayout() && CreatePresentPipelineLayout() && AllocateDescriptorSets() && UpdateSamplerSet() && CreatePipelines();
}

void NRIRenderer::Shutdown()
{
	ResetMuzzleFlashOverlayState("renderer-shutdown");
	mLastResolvedLightOverlayGeneration = 0;

	if (mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return;
	}

	mNrd.Shutdown();
	mUpscaler.Shutdown(*mFrameBuffer);
	DestroyAccelerationStructures();
	ClearRuntimePointLights();
	DestroySceneBuffers();
	DestroyFrameTextures();
	mFrameBuffer->DestroyTextureResource(mPaletteTexture);
	DestroyCachedTextures();
	mFrameGenerationFrameId = 0;
	mHasFrameGenerationRealFrameTime = false;
	mHasPendingFrameGenerationRealFrameTime = false;
	mHasFrameGenerationTimestamp = false;
	mHasFrameGenerationConfigState = false;
	mLastFrameGenerationRealFrameTimeMs = 0.0f;
	mPendingFrameGenerationRealFrameTimeMs = 0.0f;
	mLastFrameGenerationTimestamp = {};
	mPendingFrameGenerationTimestamp = {};
	mSceneTextureLimitLogPrinted = false;
	mLastFrameGenerationRequestedEnabled = false;
	mLastFrameGenerationRequestedProvider = NRIFrameGenerationProvider::Off;
	mLastFrameGenerationResolvedUiMode = NRIFrameGenerationUiMode::Auto;

	for (nri::Pipeline*& pipeline : mPipelines)
	{
		if (pipeline != nullptr)
		{
			mFrameBuffer->mCore.DestroyPipeline(pipeline);
			pipeline = nullptr;
		}
	}

	if (mPipelineLayout != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipelineLayout(mPipelineLayout);
		mPipelineLayout = nullptr;
	}
	if (mTaaPipelineLayout != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipelineLayout(mTaaPipelineLayout);
		mTaaPipelineLayout = nullptr;
	}
	if (mPresentPipelineLayout != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipelineLayout(mPresentPipelineLayout);
		mPresentPipelineLayout = nullptr;
	}

	mSamplerSet = nullptr;
	mSceneTextureSets.clear();
	mSceneDataSets.clear();
	mFrameTextureSet = nullptr;
	mOutputSet = nullptr;
	mCompositionFrameTextureSet = nullptr;
	mCompositionOutputSet = nullptr;
	mUpscalerPrepassFrameTextureSet = nullptr;
	mUpscalerPrepassOutputSet = nullptr;
	mTaaFrameTextureSet = nullptr;
	mTaaOutputSet = nullptr;
	mRawPresentFrameTextureSet = nullptr;
	mRawPresentOutputSet = nullptr;
	mFinalPresentFrameTextureSet = nullptr;
	mFinalPresentOutputSet = nullptr;
	mSceneDataDescriptorsInitialized.clear();
}

void NRIRenderer::RefreshResolvedMuzzleFlashRuleLookup(const ResolvedLightOverlaySet& resolvedLightOverlays)
{
	mResolvedMuzzleFlashRuleLookup.clear();
	mResolvedMuzzleFlashRuleLookup.reserve((size_t)resolvedLightOverlays.muzzleFlashRules.Size());
	for (const auto& rule : resolvedLightOverlays.muzzleFlashRules)
	{
		const std::string key = BuildNormalizedMuzzleFlashEventKey(rule.id);
		if (key.empty())
		{
			continue;
		}

		mResolvedMuzzleFlashRuleLookup[key] = rule;
	}
}

void NRIRenderer::ResetMuzzleFlashOverlayState(const char* reason)
{
	mResolvedMuzzleFlashRuleLookup.clear();
	for (TransientMuzzleFlashSlot& slot : mTransientMuzzleFlashSlots)
	{
		slot.ruleId = 0;
		slot.sourceEventSerial = 0;
		slot.emitterActorIndex = -1;
		slot.renderPosition[0] = 0.0f;
		slot.renderPosition[1] = 0.0f;
		slot.renderPosition[2] = 0.0f;
		slot.color[0] = 1.0f;
		slot.color[1] = 1.0f;
		slot.color[2] = 1.0f;
		slot.peakIntensity = 0.0f;
		slot.radius = 0.0f;
		slot.activationTimeSeconds = 0.0;
		slot.endTimeSeconds = 0.0;
		slot.occupied = false;
	}
	mTransientMuzzleFlashLights.clear();

	uint32_t discardedEventCount = 0;
	if (mFrameBuffer != nullptr)
	{
		TArray<PathTracingWeaponLightEvent> discardedEvents;
		mFrameBuffer->ConsumePathTracingWeaponLightEvents(discardedEvents);
		discardedEventCount = (uint32_t)discardedEvents.Size();
	}

	if (discardedEventCount > 0 && nri_ptdebug > 0)
	{
		Printf("NRI PT muzzle-flash reset: reason=%s discarded_events=%u\n",
			reason != nullptr ? reason : "unknown",
			discardedEventCount);
	}
}

const ResolvedLightOverlayMuzzleFlashRule* NRIRenderer::FindResolvedMuzzleFlashRule(const FString& eventId) const
{
	const std::string key = BuildNormalizedMuzzleFlashEventKey(eventId);
	if (key.empty())
	{
		return nullptr;
	}

	const auto it = mResolvedMuzzleFlashRuleLookup.find(key);
	return it != mResolvedMuzzleFlashRuleLookup.end() ? &it->second : nullptr;
}

void NRIRenderer::RefreshTransientMuzzleFlashLights(double currentTimeSeconds)
{
	if (mTransientMuzzleFlashSlots.empty())
	{
		mTransientMuzzleFlashSlots.resize(NriPtMuzzleFlashSlotCount);
		for (uint32_t slotIndex = 0; slotIndex < (uint32_t)mTransientMuzzleFlashSlots.size(); ++slotIndex)
		{
			TransientMuzzleFlashSlot& slot = mTransientMuzzleFlashSlots[slotIndex];
			slot.stableKey = 0x4d555a5a4c450000ull | (uint64_t)slotIndex;
			slot.slotIndex = slotIndex;
			slot.color[0] = 1.0f;
			slot.color[1] = 1.0f;
			slot.color[2] = 1.0f;
		}
	}

	TArray<PathTracingWeaponLightEvent> pendingEvents;
	if (mFrameBuffer != nullptr)
	{
		mFrameBuffer->ConsumePathTracingWeaponLightEvents(pendingEvents);
	}

	for (const PathTracingWeaponLightEvent& event : pendingEvents)
	{
		const ResolvedLightOverlayMuzzleFlashRule* rule = FindResolvedMuzzleFlashRule(event.eventId);
		if (rule == nullptr)
		{
			if (nri_ptdebug > 0)
			{
				Printf("NRI PT muzzle-flash ignored: event=%s serial=%llu reason=no-rule\n",
					event.eventId.GetChars(),
					(unsigned long long)event.serial);
			}
			continue;
		}

		const float baseIntensity = rule->hasIntensity ? std::max(rule->intensity, 0.0f) : 0.0f;
		const float baseRadius = rule->hasRadius ? std::max(rule->radius, 0.0f) : 0.0f;
		const float baseDelaySeconds = rule->hasDelaySeconds ? std::max(rule->delaySeconds, 0.0f) : 0.0f;
		const float baseDurationSeconds = rule->hasDurationSeconds ? std::max(rule->durationSeconds, 0.0f) : 0.0f;
		if (baseIntensity <= 0.0f || baseRadius <= 0.0f || baseDurationSeconds <= 0.0f)
		{
			if (nri_ptdebug > 0)
			{
				Printf("NRI PT muzzle-flash ignored: event=%s serial=%llu reason=invalid-rule intensity=%.3f radius=%.3f duration=%.4f\n",
					event.eventId.GetChars(),
					(unsigned long long)event.serial,
					baseIntensity,
					baseRadius,
					baseDurationSeconds);
			}
			continue;
		}

		uint64_t randomState = BuildMuzzleFlashRandomSeed(event);
		const float intensityScale = rule->hasIntensityRandom ? ResolveMuzzleFlashRandomRange(randomState, rule->intensityRandomRange[0], rule->intensityRandomRange[1]) : 1.0f;
		const float radiusScale = rule->hasRadiusRandom ? ResolveMuzzleFlashRandomRange(randomState, rule->radiusRandomRange[0], rule->radiusRandomRange[1]) : 1.0f;
		const float delayRandomSeconds = rule->hasDelayRandomSeconds ? ResolveMuzzleFlashRandomRange(randomState, rule->delayRandomSecondsRange[0], rule->delayRandomSecondsRange[1]) : 0.0f;
		const float durationRandomSeconds = rule->hasDurationRandomSeconds ? ResolveMuzzleFlashRandomRange(randomState, rule->durationRandomSecondsRange[0], rule->durationRandomSecondsRange[1]) : 0.0f;
		const float resolvedPeakIntensity = std::max(baseIntensity * intensityScale, 0.0f);
		const float resolvedRadius = std::max(baseRadius * radiusScale, 0.0f);
		const float resolvedDelaySeconds = std::max(baseDelaySeconds + delayRandomSeconds, 0.0f);
		const float resolvedDurationSeconds = std::max(baseDurationSeconds + durationRandomSeconds, 0.001f);
		if (resolvedPeakIntensity <= 0.0f || resolvedRadius <= 0.0f)
		{
			continue;
		}

		DVector3 resolvedWorldPosition = event.worldPosition;
		if (rule->hasOffset && event.hasBasis)
		{
			resolvedWorldPosition +=
				event.basisRight * rule->offset[0] +
				event.basisForward * rule->offset[1] +
				event.basisUp * rule->offset[2];
		}

		float renderPosition[3] = {};
		WorldToPathTracingPosition(resolvedWorldPosition, renderPosition);

		size_t selectedSlotIndex = 0;
		bool foundReusableSlot = false;
		double oldestEndTimeSeconds = std::numeric_limits<double>::infinity();
		for (size_t slotIndex = 0; slotIndex < mTransientMuzzleFlashSlots.size(); ++slotIndex)
		{
			const TransientMuzzleFlashSlot& slot = mTransientMuzzleFlashSlots[slotIndex];
			if (!slot.occupied || slot.endTimeSeconds <= currentTimeSeconds)
			{
				selectedSlotIndex = slotIndex;
				foundReusableSlot = true;
				break;
			}

			if (slot.endTimeSeconds < oldestEndTimeSeconds)
			{
				oldestEndTimeSeconds = slot.endTimeSeconds;
				selectedSlotIndex = slotIndex;
			}
		}

		TransientMuzzleFlashSlot& slot = mTransientMuzzleFlashSlots[selectedSlotIndex];
		slot.ruleId = BuildMuzzleFlashRuleId(*rule);
		slot.sourceEventSerial = event.serial;
		slot.emitterActorIndex = event.hasEmitterActorIndex ? event.emitterActorIndex : -1;
		Copy3(renderPosition, slot.renderPosition);
		slot.color[0] = rule->hasColor ? std::max(rule->color[0], 0.0f) : 1.0f;
		slot.color[1] = rule->hasColor ? std::max(rule->color[1], 0.0f) : 1.0f;
		slot.color[2] = rule->hasColor ? std::max(rule->color[2], 0.0f) : 1.0f;
		slot.peakIntensity = resolvedPeakIntensity;
		slot.radius = resolvedRadius;
		slot.activationTimeSeconds = std::max(event.absoluteTimeSeconds, 0.0) + (double)resolvedDelaySeconds;
		slot.endTimeSeconds = slot.activationTimeSeconds + (double)resolvedDurationSeconds;
		slot.occupied = true;

		if (nri_ptdebug > 0)
		{
			Printf("NRI PT muzzle-flash spawn: slot=%u reused=%s event=%s serial=%llu rule=%u actor=%d delay=%.4f duration=%.4f peak=%.3f radius=%.3f render_pos=(%.3f, %.3f, %.3f)\n",
				slot.slotIndex,
				YesNo(foundReusableSlot),
				event.eventId.GetChars(),
				(unsigned long long)event.serial,
				slot.ruleId,
				slot.emitterActorIndex,
				resolvedDelaySeconds,
				resolvedDurationSeconds,
				slot.peakIntensity,
				slot.radius,
				slot.renderPosition[0],
				slot.renderPosition[1],
				slot.renderPosition[2]);
		}
	}

	mTransientMuzzleFlashLights.clear();
	mTransientMuzzleFlashLights.reserve(mTransientMuzzleFlashSlots.size());
	for (TransientMuzzleFlashSlot& slot : mTransientMuzzleFlashSlots)
	{
		SceneLightSystem::SceneAnalyticLight light = {};
		light.id = 0x8000u + slot.slotIndex;
		light.stableKey = slot.stableKey;
		light.sourceFlags = SceneAnalyticLightSourceFlag_MuzzleFlash;
		light.sourceRuleId = slot.ruleId;
		light.source = SceneLightRecordSource::None;
		light.actorIndex = slot.emitterActorIndex;
		Copy3(slot.renderPosition, light.position);
		Copy3(slot.color, light.color);
		light.intensity = EvaluateMuzzleFlashFadeOut(
			currentTimeSeconds,
			slot.occupied,
			slot.peakIntensity,
			slot.radius,
			slot.activationTimeSeconds,
			slot.endTimeSeconds);
		light.radius = light.intensity > 0.0f ? slot.radius : 0.0f;
		mTransientMuzzleFlashLights.push_back(light);

		if (slot.occupied && currentTimeSeconds >= slot.endTimeSeconds)
		{
			slot.ruleId = 0;
			slot.sourceEventSerial = 0;
			slot.emitterActorIndex = -1;
			slot.peakIntensity = 0.0f;
			slot.radius = 0.0f;
			slot.activationTimeSeconds = 0.0;
			slot.endTimeSeconds = 0.0;
			slot.occupied = false;
		}
	}

	mSceneLights.SetTransientAnalyticLights(mTransientMuzzleFlashLights);
}

void NRIRenderer::ResetPerfTraceStats()
{
	mLastPerfShellTraceStats = {};
	mLastPerfResourceTraceStats = {};
	UpdateRuntimeMutationRebaselinePerfStats();
}

void NRIRenderer::UpdateRuntimeMutationRebaselinePerfStats()
{
	const uint32_t candidateSceneChunkTotal = (uint32_t)mRuntimeMutationRebaselineCandidate.world.chunks.size();
	const uint32_t candidateBuiltChunkCount = mRuntimeMutationRebaselineCandidate.cacheBuildCount;
	const uint32_t candidatePreparedBlasCount = mRuntimeMutationRebaselineCandidate.blasPrepareCount;
	const uint32_t candidateBlasChunkCount = (uint32_t)mRuntimeMutationRebaselineCandidate.staticScene.chunks.size();
	mLastPerfShellTraceStats.runtimeMutationRebaselineQueued = mRuntimeMutationRebaselineState != RuntimeMutationRebaselineState::Idle;
	mLastPerfShellTraceStats.runtimeMutationRebaselineState = (uint32_t)mRuntimeMutationRebaselineState;
	mLastPerfShellTraceStats.runtimeMutationRebaselineQueueFrame = mRuntimeMutationRebaselineQueueFrame;
	mLastPerfShellTraceStats.runtimeMutationRebaselineActiveChunkCount = mPendingRuntimeMutationRebaselineActiveChunkCount;
	mLastPerfShellTraceStats.runtimeMutationRebaselineStableChunkCount = mPendingRuntimeMutationRebaselineStableChunkCount;
	mLastPerfShellTraceStats.runtimeMutationRebaselineFramesQueued =
		(mRuntimeMutationRebaselineState != RuntimeMutationRebaselineState::Idle && mFrameIndex >= mRuntimeMutationRebaselineQueueFrame)
		? (mFrameIndex - mRuntimeMutationRebaselineQueueFrame)
		: 0u;
	mLastPerfShellTraceStats.runtimeMutationRebaselineCandidateBuildSerial =
		mRuntimeMutationRebaselineCandidate.valid ? mRuntimeMutationRebaselineCandidate.world.buildSerial : 0ull;
	mLastPerfShellTraceStats.runtimeMutationRebaselineCandidateCacheBuilt = candidateBuiltChunkCount;
	mLastPerfShellTraceStats.runtimeMutationRebaselineCandidateCacheTotal = candidateSceneChunkTotal;
	mLastPerfShellTraceStats.runtimeMutationRebaselineCandidateBlasPrepared = candidatePreparedBlasCount;
	mLastPerfShellTraceStats.runtimeMutationRebaselineCandidateBlasPrepareTotal = candidateBlasChunkCount;
	mLastPerfShellTraceStats.runtimeMutationRebaselineCandidateBlasBuilt = mRuntimeMutationRebaselineCandidate.blasBuildCount;
	mLastPerfShellTraceStats.runtimeMutationRebaselineCandidateBlasTotal = candidateBlasChunkCount;
	mLastPerfShellTraceStats.runtimeMutationRebaselineCandidateSceneChunkCount = candidateBlasChunkCount;
	mLastPerfShellTraceStats.runtimeMutationRebaselineCandidateSceneSurfaceCount = mRuntimeMutationRebaselineCandidate.valid ? mRuntimeMutationRebaselineCandidate.world.stats.surfaceCount : 0u;
	mLastPerfShellTraceStats.runtimeMutationRebaselineCandidateSceneTriangleCount = mRuntimeMutationRebaselineCandidate.valid ? mRuntimeMutationRebaselineCandidate.world.stats.triangleCount : 0u;
	mLastPerfShellTraceStats.runtimeMutationRebaselineRetiredSceneCount = (uint32_t)mRetiredRuntimeMutationRebaselineStaticScenes.size();
	mLastPerfShellTraceStats.runtimeMutationRebaselineBuildWorldMs = mRuntimeMutationRebaselineBuildWorldMs;
	mLastPerfShellTraceStats.runtimeMutationRebaselineBuildStaticSceneCacheMs = mRuntimeMutationRebaselineBuildStaticSceneCacheMs;
	mLastPerfShellTraceStats.runtimeMutationRebaselineRealizeStaticSceneTexturesMs = mRuntimeMutationRebaselineRealizeStaticSceneTexturesMs;
	mLastPerfShellTraceStats.runtimeMutationRebaselineUploadStaticSceneBuffersMs = mRuntimeMutationRebaselineUploadStaticSceneBuffersMs;
	mLastPerfShellTraceStats.runtimeMutationRebaselinePrepareStaticSceneBlasMs = mRuntimeMutationRebaselinePrepareStaticSceneBlasMs;
	mLastPerfShellTraceStats.runtimeMutationRebaselineBuildStaticSceneBlasMs = mRuntimeMutationRebaselineBuildStaticSceneBlasMs;
	mLastPerfShellTraceStats.runtimeMutationRebaselineBuildStaticSceneTlasMs = mRuntimeMutationRebaselineBuildStaticSceneTlasMs;
	mLastPerfShellTraceStats.runtimeMutationRebaselineSwapMs = mRuntimeMutationRebaselineSwapMs;
	mLastPerfShellTraceStats.runtimeMutationRebaselineRetireMs = mRuntimeMutationRebaselineRetireMs;
}

void NRIRenderer::TraceRuntimeMutationRebaselineProgress(const char* eventLabel) const
{
	if (!ShouldTraceRuntimeMutationRebaseline())
	{
		return;
	}

	const auto& candidate = mRuntimeMutationRebaselineCandidate;
	const uint32_t totalCacheChunks = (uint32_t)candidate.world.chunks.size();
	const uint32_t totalBlasChunks = (uint32_t)candidate.staticScene.chunks.size();
	const uint32_t framesQueued =
		(mRuntimeMutationRebaselineState != RuntimeMutationRebaselineState::Idle && mFrameIndex >= mRuntimeMutationRebaselineQueueFrame)
		? (mFrameIndex - mRuntimeMutationRebaselineQueueFrame)
		: 0u;
	Printf(
		"NRI PT runtime mutation rebaseline trace: event=%s level=%s frame=%u state=%s queue_frame=%u frames_queued=%u active_chunks=%u stable_chunks=%u candidate_build_serial=%llu scene_chunks=%u scene_surfaces=%u scene_tris=%u cache=%u/%u blas_prepare=%u/%u blas=%u/%u cache_budget=%d blas_budget=%d retired=%u build_world_ms=%.3f build_static_scene_cache_ms=%.3f realize_static_scene_textures_ms=%.3f upload_static_scene_buffers_ms=%.3f prepare_static_scene_blas_ms=%.3f build_static_scene_blas_ms=%.3f build_static_scene_tlas_ms=%.3f swap_ms=%.3f retire_ms=%.3f\n",
		eventLabel != nullptr ? eventLabel : "progress",
		currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)",
		mFrameIndex,
		GetRuntimeMutationRebaselineStateName(mRuntimeMutationRebaselineState),
		mRuntimeMutationRebaselineQueueFrame,
		framesQueued,
		mPendingRuntimeMutationRebaselineActiveChunkCount,
		mPendingRuntimeMutationRebaselineStableChunkCount,
		(unsigned long long)(candidate.valid ? candidate.world.buildSerial : 0ull),
		totalBlasChunks,
		candidate.valid ? candidate.world.stats.surfaceCount : 0u,
		candidate.valid ? candidate.world.stats.triangleCount : 0u,
		candidate.cacheBuildCount,
		totalCacheChunks,
		candidate.blasPrepareCount,
		totalBlasChunks,
		candidate.blasBuildCount,
		totalBlasChunks,
		(int)nri_ptrebaselinecachechunksperframe,
		(int)nri_ptrebaselineblasperframe,
		(uint32_t)mRetiredRuntimeMutationRebaselineStaticScenes.size(),
		mRuntimeMutationRebaselineBuildWorldMs,
		mRuntimeMutationRebaselineBuildStaticSceneCacheMs,
		mRuntimeMutationRebaselineRealizeStaticSceneTexturesMs,
		mRuntimeMutationRebaselineUploadStaticSceneBuffersMs,
		mRuntimeMutationRebaselinePrepareStaticSceneBlasMs,
		mRuntimeMutationRebaselineBuildStaticSceneBlasMs,
		mRuntimeMutationRebaselineBuildStaticSceneTlasMs,
		mRuntimeMutationRebaselineSwapMs,
		mRuntimeMutationRebaselineRetireMs);
}

void NRIRenderer::WaitForCommandsTracked()
{
	if (mFrameBuffer == nullptr)
	{
		return;
	}

	const bool trace = ShouldTracePtPerf();
	const auto start = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	mFrameBuffer->WaitForCommands(true);
	if (trace)
	{
		mLastPerfResourceTraceStats.waitCalls++;
		mLastPerfResourceTraceStats.waitMs += DurationMs(start, std::chrono::steady_clock::now());
	}
}

void NRIRenderer::NotePerfBufferUpload(const SceneBufferDebugStats* stats, uint64_t size, bool growth)
{
	if (!ShouldTracePtPerf() || stats == nullptr)
	{
		return;
	}

	auto& perf = mLastPerfResourceTraceStats;
	if (growth)
	{
		perf.growEvents++;
	}
	else
	{
		perf.overwriteEvents++;
	}

	auto noteBytes = [&](uint32_t& callCount, uint64_t& byteCount)
	{
		callCount++;
		byteCount += size;
	};

	if (stats == &mVertexBufferStats || stats == &mIndexBufferStats || stats == &mPrimitiveBufferStats || stats == &mMaterialBufferStats)
	{
		noteBytes(perf.sceneUploadCalls, perf.sceneUploadBytes);
	}
	else if (stats == &mEmissivePrimitiveHeaderBufferStats || stats == &mEmissivePrimitiveBufferStats || stats == &mEmissivePrimitiveCdfBufferStats || stats == &mEmissiveTlasInstanceBufferStats)
	{
		noteBytes(perf.emissiveUploadCalls, perf.emissiveUploadBytes);
	}
	else
	{
		noteBytes(perf.sceneDataUploadCalls, perf.sceneDataUploadBytes);
	}
}

bool NRIRenderer::RenderScene(HWDrawInfo& di, int drawmode, bool portal)
{
	if ((drawmode != DM_MAINVIEW && drawmode != DM_OFFSCREEN) || portal || mFrameBuffer == nullptr ||
		mFrameBuffer->mCommandBuffer == nullptr || mFrameBuffer->mActiveTarget == nullptr)
	{
		return false;
	}

	if (!mPathTracingSupported)
	{
		LogFallback(GetAvailabilityReason());
		return false;
	}

	ResetPerfTraceStats();
	ScopedPtPerfTimer totalPerfTimer(mLastPerfShellTraceStats.totalMs);
	Clocker totalClock(NriPTAll);
	const uint32_t traceFrameIndex = mFrameIndex;

	const uint32_t bootstrapMode = GetBootstrapMode();
	const bool bootstrapSimpleView = nri_ptbootstrap && bootstrapMode <= 3u;
	const bool bootstrapCapturedView = nri_ptbootstrap && bootstrapMode >= 4u && bootstrapMode <= 12u;
	const bool bootstrapCapturedDiagnostics = nri_ptbootstrap && bootstrapMode >= 4u && bootstrapMode <= 10u;
	const bool bootstrapCapturedFlat = nri_ptbootstrap && bootstrapMode == 11u;
	const bool bootstrapCapturedBaseColor = nri_ptbootstrap && bootstrapMode == 12u;
	const bool rawTraceDirectScene = !nri_ptbootstrap && nri_ptdirectscene;
	const int debugMode = (int)nri_ptdebug;

	const bool preserveHistory = drawmode != DM_MAINVIEW;
	uint32_t savedFrameIndex = mFrameIndex;
	float savedCurrentCameraPos[3] = {};
	float savedCurrentCameraForward[3] = {};
	float savedCurrentCameraRight[3] = {};
	float savedCurrentCameraUp[3] = {};
	float savedPreviousCameraPos[3] = {};
	float savedPreviousCameraForward[3] = {};
	float savedPreviousCameraRight[3] = {};
	float savedPreviousCameraUp[3] = {};
	float savedCurrentJitter[2] = {};
	float savedPreviousJitter[2] = {};
	float savedCurrentViewToClip[16] = {};
	float savedPreviousViewToClip[16] = {};
	float savedCurrentWorldToView[16] = {};
	float savedPreviousWorldToView[16] = {};
	float savedCurrentTanHalfFovX = mCurrentTanHalfFovX;
	float savedCurrentTanHalfFovY = mCurrentTanHalfFovY;
	float savedPreviousTanHalfFovX = mPreviousTanHalfFovX;
	float savedPreviousTanHalfFovY = mPreviousTanHalfFovY;
	bool savedHasPreviousCameraState = mHasPreviousCameraState;
	bool savedResetHistory = mResetHistory;
	if (preserveHistory)
	{
		Copy3(mCurrentCameraPos, savedCurrentCameraPos);
		Copy3(mCurrentCameraForward, savedCurrentCameraForward);
		Copy3(mCurrentCameraRight, savedCurrentCameraRight);
		Copy3(mCurrentCameraUp, savedCurrentCameraUp);
		Copy3(mPreviousCameraPos, savedPreviousCameraPos);
		Copy3(mPreviousCameraForward, savedPreviousCameraForward);
		Copy3(mPreviousCameraRight, savedPreviousCameraRight);
		Copy3(mPreviousCameraUp, savedPreviousCameraUp);
		Copy2(mCurrentJitter, savedCurrentJitter);
		Copy2(mPreviousJitter, savedPreviousJitter);
		std::memcpy(savedCurrentViewToClip, mCurrentViewToClip, sizeof(savedCurrentViewToClip));
		std::memcpy(savedPreviousViewToClip, mPreviousViewToClip, sizeof(savedPreviousViewToClip));
		std::memcpy(savedCurrentWorldToView, mCurrentWorldToView, sizeof(savedCurrentWorldToView));
		std::memcpy(savedPreviousWorldToView, mPreviousWorldToView, sizeof(savedPreviousWorldToView));
	}

	auto restoreHistory = [this, &savedCurrentCameraPos, &savedCurrentCameraForward, &savedCurrentCameraRight, &savedCurrentCameraUp,
		&savedPreviousCameraPos, &savedPreviousCameraForward, &savedPreviousCameraRight, &savedPreviousCameraUp, &savedCurrentJitter, &savedPreviousJitter,
		&savedCurrentViewToClip, &savedPreviousViewToClip, &savedCurrentWorldToView, &savedPreviousWorldToView, savedFrameIndex, savedCurrentTanHalfFovX,
		savedCurrentTanHalfFovY, savedPreviousTanHalfFovX, savedPreviousTanHalfFovY, savedHasPreviousCameraState, savedResetHistory]()
	{
		mFrameIndex = savedFrameIndex;
		Copy3(savedCurrentCameraPos, mCurrentCameraPos);
		Copy3(savedCurrentCameraForward, mCurrentCameraForward);
		Copy3(savedCurrentCameraRight, mCurrentCameraRight);
		Copy3(savedCurrentCameraUp, mCurrentCameraUp);
		Copy3(savedPreviousCameraPos, mPreviousCameraPos);
		Copy3(savedPreviousCameraForward, mPreviousCameraForward);
		Copy3(savedPreviousCameraRight, mPreviousCameraRight);
		Copy3(savedPreviousCameraUp, mPreviousCameraUp);
		Copy2(savedCurrentJitter, mCurrentJitter);
		Copy2(savedPreviousJitter, mPreviousJitter);
		std::memcpy(mCurrentViewToClip, savedCurrentViewToClip, sizeof(mCurrentViewToClip));
		std::memcpy(mPreviousViewToClip, savedPreviousViewToClip, sizeof(mPreviousViewToClip));
		std::memcpy(mCurrentWorldToView, savedCurrentWorldToView, sizeof(mCurrentWorldToView));
		std::memcpy(mPreviousWorldToView, savedPreviousWorldToView, sizeof(mPreviousWorldToView));
		mCurrentTanHalfFovX = savedCurrentTanHalfFovX;
		mCurrentTanHalfFovY = savedCurrentTanHalfFovY;
		mPreviousTanHalfFovX = savedPreviousTanHalfFovX;
		mPreviousTanHalfFovY = savedPreviousTanHalfFovY;
		mHasPreviousCameraState = savedHasPreviousCameraState;
		mResetHistory = savedResetHistory;
	};

	bool ready = false;
	{
		ScopedPtPerfTimer initPerfTimer(mLastPerfShellTraceStats.initResourcesMs);
		ready =
			Initialize() &&
			EnsureFrameResources(
				std::max<uint32_t>((uint32_t)mFrameBuffer->mSceneViewport.width, 1u),
				std::max<uint32_t>((uint32_t)mFrameBuffer->mSceneViewport.height, 1u),
				mFrameBuffer->mActiveTarget->width,
				mFrameBuffer->mActiveTarget->height);
	}
	if (!ready)
	{
		LogFallback("PT frame resources or pipelines failed to initialize.");
		if (preserveHistory)
		{
			restoreHistory();
		}
		return false;
	}

	ResetSceneBufferFrameStats();
	ResetRendererSkyPerfTraceStats();
	nri_scene::ResetAverageTextureColorCache();
	nri_scene::ResetSkyPerfStats();
	mUsedStaticMapSceneLastFrame = false;
	mUsedDynamicSceneLastFrame = false;
	mHasVisibleMirrorPortalLastFrame = false;
	mUploadedStaticMapSceneLastFrame = false;
	mBuiltStaticMapSceneASLastFrame = false;
	mBuiltDynamicSceneASLastFrame = false;
	mDynamicSceneLastFrame = {};
	mRuntimeMapLastFrame = {};
	mRuntimeSpaceLinkLastFrame = {};
	if (!preserveHistory)
	{
		mPendingFrameGenerationTimestamp = std::chrono::steady_clock::now();
		mHasPendingFrameGenerationRealFrameTime = false;
		mPendingFrameGenerationRealFrameTimeMs = 0.0f;
		if (mHasFrameGenerationTimestamp)
		{
			const auto elapsed = mPendingFrameGenerationTimestamp - mLastFrameGenerationTimestamp;
			mPendingFrameGenerationRealFrameTimeMs = (float)std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(elapsed).count();
			mHasPendingFrameGenerationRealFrameTime = true;
			if (mPendingFrameGenerationRealFrameTimeMs > 250.0f)
			{
				RequestHistoryReset("cadence-break");
			}
		}
	}
	UpdateFrameGenerationHistoryPolicy(debugMode, mFrameBuffer->mFrameGeneration.GetPolicy(), preserveHistory);

	RefreshMapWorld();
	if (mPendingStartupMutationRebaseline)
	{
		RebuildStartupMutationBaseline();
	}
	if (mPendingRuntimeMutationRebaseline || mRuntimeMutationRebaselineState != RuntimeMutationRebaselineState::Idle)
	{
		AdvanceRuntimeMutationRebaseline();
	}
	if (mPendingStaticMapLightingInvalidation)
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.lightingInvalidationsApplied++;
		}
		InvalidateStaticMapSceneForMaterialLighting();
		mPendingStaticMapLightingInvalidation = false;
	}
	UpdatePerFrameState(di);
	if (preserveHistory)
	{
		mResetHistory = true;
	}

	if (bootstrapSimpleView)
	{
		mHistoryInputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPing : FrameTextureSlot::TaaHistoryPong;
		mHistoryOutputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPong : FrameTextureSlot::TaaHistoryPing;
		mUpscaledInputSlot = FrameTextureSlot::Composed;
		mUseUpscaledInFinal = false;
		if (!DispatchBootstrapView())
		{
			LogFallback("PT bootstrap view dispatch failed.");
			if (preserveHistory)
			{
				restoreHistory();
			}
			return false;
		}

		CopyFinalToActiveTarget();
		if (!preserveHistory)
		{
			NoteSuccessfulRealFrame();
			++mFrameIndex;
			mHasPreviousCameraState = true;
			mResetHistory = false;
		}
		else
		{
			restoreHistory();
		}
		return true;
	}

	const bool allowStaticMapScene = !bootstrapCapturedView && !rawTraceDirectScene && mMapWorld.valid;
	nri_scene::SceneView capturedSceneView;
	nri_scene::SceneView dynamicSceneView;
	nri_scene::GeometryData capturedGeometry;
	nri_scene::GeometryData runtimeMutationGeometry;
	nri_scene::GeometryData runtimeSpaceLinkGeometry;
	nri_scene::GeometryData dynamicGeometry;
	nri_scene::GeometryData mirrorExtendedDynamicGeometry;
	nri_scene::GeometryData mergedDynamicGeometry;
	nri_scene::GeometryData debugSphereGeometry;
	nri_scene::GeometryData overlayGeometry;
	nri_scene::GeometryData combinedGeometry;
	nri_scene::MaterialBridgeData materialBridge;
	nri_scene::MaterialBridgeData runtimeMutationMaterialBridge;
	nri_scene::MaterialBridgeData runtimeSpaceLinkMaterialBridge;
	nri_scene::MaterialBridgeData dynamicMaterialBridge;
	nri_scene::MaterialBridgeData mirrorExtendedDynamicMaterialBridge;
	nri_scene::MaterialBridgeData mirrorPlayerMaterialBridge;
	nri_scene::MaterialBridgeData sceneLightMergedDynamicMaterialBridge;
	nri_scene::MaterialBridgeData mergedDynamicMaterialBridge;
	nri_scene::MaterialBridgeData debugSphereMaterialBridge;
	nri_scene::MaterialBridgeData overlayMaterialBridge;
	nri_scene::MaterialBridgeData combinedMaterialBridge;
	std::vector<nri_scene::MaterialData> capturedGpuMaterials;
	std::vector<nri_scene::MaterialData> dynamicGpuMaterials;
	std::vector<nri_scene::MaterialData> combinedGpuMaterials;
	const nri_scene::SceneView* activeSceneView = nullptr;
	const nri_scene::GeometryData* activeGeometry = nullptr;
	const std::vector<nri_scene::MaterialData>* activeGpuMaterials = nullptr;
	const nri_scene::MaterialBridgeData* activeMaterialBridge = nullptr;
	const nri_scene::SceneView* sceneLightCapturedView = nullptr;
	const nri_scene::MaterialBridgeData* sceneLightCapturedMaterials = nullptr;
	const nri_scene::SceneView* sceneLightDynamicView = nullptr;
	const nri_scene::MaterialBridgeData* sceneLightDynamicMaterials = nullptr;
	nri_scene::SceneView mirrorExtendedDynamicSceneView;
	nri_scene::SceneView mirrorPlayerSceneView;
	nri_scene::SceneView sceneLightMergedDynamicSceneView;
	nri_scene::SceneView mergedDynamicSceneView;
	const nri_scene::SceneView* activeDynamicSceneView = nullptr;
	const nri_scene::GeometryData* activeDynamicGeometry = nullptr;
	const nri_scene::MaterialBridgeData* activeDynamicMaterials = nullptr;
	nri_scene::GeometryData mirrorPlayerGeometry;
	uint32_t activeStaticProbePrimitiveCount = 0;
	EmissiveSamplingBuildContext emissiveSamplingContext = {};
	bool sceneLightUsesStaticMapScene = false;
	nri_scene::SceneDebugStats activeStats = {};
	bool paletteReady = true;
	bool texturesReady = true;
	bool buffersReady = true;
	bool accelerationReady = true;
	bool usingPersistentDynamicEmissiveCache = false;
	bool liveDynamicHasEmissive = false;

	{
		ScopedPtPerfTimer sceneSelectTimer(mLastPerfShellTraceStats.sceneSelectMs);
		if (allowStaticMapScene && EnsureStaticMapScene())
		{
			sceneLightUsesStaticMapScene = true;
			emissiveSamplingContext.staticGeometry = &mStaticMapScene.geometry;
			mUsedStaticMapSceneLastFrame = true;
			activeSceneView = &mStaticMapScene.sceneView;
			activeGeometry = &mStaticMapScene.geometry;
			activeGpuMaterials = &mStaticMapScene.gpuMaterials;
			activeMaterialBridge = &mStaticMapScene.materialBridge;
			activeStaticProbePrimitiveCount = (uint32_t)mStaticMapScene.geometry.primitives.size();
			activeStats = mStaticMapScene.sceneView.stats;

		const bool deferOverlayThisFrame = mUploadedStaticMapSceneLastFrame || mBuiltStaticMapSceneASLastFrame;
		const bool hasRuntimeSpaceLinkOverlay = !deferOverlayThisFrame && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeSpaceLinkMs);
			return BuildRuntimeSpaceLinkOverlay(di, runtimeSpaceLinkGeometry, runtimeSpaceLinkMaterialBridge);
		}();
		mLastPerfShellTraceStats.runtimeSpaceLinkPrimitiveCount = (uint32_t)runtimeSpaceLinkGeometry.primitives.size();
		mLastPerfShellTraceStats.runtimeSpaceLinkMaterialCount = (uint32_t)runtimeSpaceLinkMaterialBridge.materials.size();
		const bool hasRuntimeMutationOverlay = !deferOverlayThisFrame && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationMs);
			return BuildRuntimeMapMutationOverlay(runtimeMutationGeometry, runtimeMutationMaterialBridge);
		}();
		const bool hasDynamicScene = !deferOverlayThisFrame && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.dynamicCaptureMs);
			return nri_scene::CaptureDynamicScene(di, dynamicSceneView);
		}();
		const int32_t preferredMirrorWallIndex =
			mLastSurfaceProbe.valid &&
			mLastSurfaceProbe.hit &&
			(mLastSurfaceProbe.primitiveFlags & nri_scene::MaterialFlag_Mirror) != 0 &&
			mLastSurfaceProbe.provenance.wallIndex >= 0 ?
				mLastSurfaceProbe.provenance.wallIndex :
				-1;
		uint32_t visibleMirrorPortalCandidates = 0;
		int32_t selectedVisibleMirrorWallIndex = -1;
		HWPortal* const visibleMirrorPortal = !deferOverlayThisFrame ?
			SelectPrimaryMirrorPortal(di, visibleMirrorPortalCandidates, selectedVisibleMirrorWallIndex, preferredMirrorWallIndex) :
			nullptr;
		mHasVisibleMirrorPortalLastFrame = visibleMirrorPortal != nullptr;
		const bool hasMirrorExtendedDynamicScene =
			!deferOverlayThisFrame &&
			CaptureMirrorExtendedDynamicScene(
				di,
				visibleMirrorPortal,
				hasDynamicScene ? &dynamicSceneView : nullptr,
				mirrorExtendedDynamicSceneView);
		const bool hasMirrorPlayerScene =
			!deferOverlayThisFrame &&
			IsMirrorPlayerPreviewCaptureEnabled() &&
			CaptureMirrorPlayerDynamicScene(
				di,
				visibleMirrorPortal,
				selectedVisibleMirrorWallIndex,
				visibleMirrorPortalCandidates,
				mirrorPlayerSceneView);
		if (hasDynamicScene)
		{
			{
				Clocker clock(NriPTGeometryBuild);
				nri_scene::BuildGeometry(dynamicSceneView, dynamicGeometry);
				AssignGeometryPortalIndices(mMapWorld, dynamicGeometry);
			}

			if (!dynamicGeometry.primitives.empty())
			{
				{
					Clocker clock(NriPTMaterialBuild);
					BuildMaterialsWithActorOverrides(dynamicSceneView, dynamicMaterialBridge, "dynamic_live");
				}
			}

			sceneLightDynamicView = &dynamicSceneView;
			sceneLightDynamicMaterials = &dynamicMaterialBridge;
			activeDynamicSceneView = &dynamicSceneView;
			activeDynamicGeometry = &dynamicGeometry;
			activeDynamicMaterials = &dynamicMaterialBridge;
			liveDynamicHasEmissive = [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.persistentDynamicMs);
				return RebuildPersistentDynamicEmissiveCache(dynamicSceneView, dynamicMaterialBridge);
			}();
		}
		if (hasMirrorExtendedDynamicScene)
		{
			{
				Clocker clock(NriPTGeometryBuild);
				nri_scene::BuildGeometry(mirrorExtendedDynamicSceneView, mirrorExtendedDynamicGeometry);
				AssignGeometryPortalIndices(mMapWorld, mirrorExtendedDynamicGeometry);
			}

			if (!mirrorExtendedDynamicGeometry.primitives.empty())
			{
				Clocker clock(NriPTMaterialBuild);
				BuildMaterialsWithActorOverrides(mirrorExtendedDynamicSceneView, mirrorExtendedDynamicMaterialBridge, "mirror_extended");
			}

			if (hasDynamicScene)
			{
				sceneLightMergedDynamicSceneView = dynamicSceneView;
				sceneLightMergedDynamicSceneView.opaqueWalls.insert(
					sceneLightMergedDynamicSceneView.opaqueWalls.end(),
					mirrorExtendedDynamicSceneView.opaqueWalls.begin(),
					mirrorExtendedDynamicSceneView.opaqueWalls.end());
				sceneLightMergedDynamicSceneView.opaqueFlats.insert(
					sceneLightMergedDynamicSceneView.opaqueFlats.end(),
					mirrorExtendedDynamicSceneView.opaqueFlats.begin(),
					mirrorExtendedDynamicSceneView.opaqueFlats.end());
				sceneLightMergedDynamicSceneView.opaqueSprites.insert(
					sceneLightMergedDynamicSceneView.opaqueSprites.end(),
					mirrorExtendedDynamicSceneView.opaqueSprites.begin(),
					mirrorExtendedDynamicSceneView.opaqueSprites.end());
				RebuildSceneViewStats(sceneLightMergedDynamicSceneView);
				BuildMaterialsWithActorOverrides(sceneLightMergedDynamicSceneView, sceneLightMergedDynamicMaterialBridge, "scene_light_merged_dynamic");
				sceneLightDynamicView = &sceneLightMergedDynamicSceneView;
				sceneLightDynamicMaterials = &sceneLightMergedDynamicMaterialBridge;
			}
			else
			{
				sceneLightDynamicView = &mirrorExtendedDynamicSceneView;
				sceneLightDynamicMaterials = &mirrorExtendedDynamicMaterialBridge;
			}
		}
		if (hasMirrorPlayerScene)
		{
			{
				Clocker clock(NriPTGeometryBuild);
				nri_scene::BuildGeometry(mirrorPlayerSceneView, mirrorPlayerGeometry);
				AssignGeometryPortalIndices(mMapWorld, mirrorPlayerGeometry);
			}

			if (!mirrorPlayerGeometry.primitives.empty())
			{
				Clocker clock(NriPTMaterialBuild);
				BuildMaterialsWithActorOverrides(mirrorPlayerSceneView, mirrorPlayerMaterialBridge, "mirror_player");
			}
		}

		PrunePersistentDynamicEmissiveCacheToLiveActors();
		const PersistentDynamicSurfaceStats persistentDynamicStats = GatherPersistentDynamicEmissiveSurfaceStats();
		mLastPerfShellTraceStats.persistentDynamicActorSurfaceCount = persistentDynamicStats.actorSurfaceCount;
		mLastPerfShellTraceStats.persistentDynamicNonActorSurfaceCount = persistentDynamicStats.nonActorSurfaceCount;
		mLastPerfShellTraceStats.persistentDynamicWallSurfaceCount = persistentDynamicStats.wallSurfaceCount;
		mLastPerfShellTraceStats.persistentDynamicFlatSurfaceCount = persistentDynamicStats.flatSurfaceCount;
		mLastPerfShellTraceStats.persistentDynamicSpriteSurfaceCount = persistentDynamicStats.spriteSurfaceCount;
		if (mPersistentDynamicEmissiveCache.valid)
		{
			mPersistentDynamicEmissiveHighWaterSurfaceCount = std::max(mPersistentDynamicEmissiveHighWaterSurfaceCount, mPersistentDynamicEmissiveCache.surfaceCount);
			mPersistentDynamicEmissiveHighWaterPrimitiveCount = std::max(mPersistentDynamicEmissiveHighWaterPrimitiveCount, mPersistentDynamicEmissiveCache.primitiveCount);
			mPersistentDynamicEmissiveHighWaterMaterialCount = std::max(mPersistentDynamicEmissiveHighWaterMaterialCount, mPersistentDynamicEmissiveCache.materialCount);
			mPersistentDynamicEmissiveHighWaterStats.actorSurfaceCount = std::max(mPersistentDynamicEmissiveHighWaterStats.actorSurfaceCount, persistentDynamicStats.actorSurfaceCount);
			mPersistentDynamicEmissiveHighWaterStats.nonActorSurfaceCount = std::max(mPersistentDynamicEmissiveHighWaterStats.nonActorSurfaceCount, persistentDynamicStats.nonActorSurfaceCount);
			mPersistentDynamicEmissiveHighWaterStats.wallSurfaceCount = std::max(mPersistentDynamicEmissiveHighWaterStats.wallSurfaceCount, persistentDynamicStats.wallSurfaceCount);
			mPersistentDynamicEmissiveHighWaterStats.flatSurfaceCount = std::max(mPersistentDynamicEmissiveHighWaterStats.flatSurfaceCount, persistentDynamicStats.flatSurfaceCount);
			mPersistentDynamicEmissiveHighWaterStats.spriteSurfaceCount = std::max(mPersistentDynamicEmissiveHighWaterStats.spriteSurfaceCount, persistentDynamicStats.spriteSurfaceCount);
			mPersistentDynamicEmissiveHighWaterStats.actorFacingSpriteCount = std::max(mPersistentDynamicEmissiveHighWaterStats.actorFacingSpriteCount, persistentDynamicStats.actorFacingSpriteCount);
			mPersistentDynamicEmissiveHighWaterStats.actorVoxelSpriteCount = std::max(mPersistentDynamicEmissiveHighWaterStats.actorVoxelSpriteCount, persistentDynamicStats.actorVoxelSpriteCount);
		}

		const bool shouldUsePersistentDynamicEmissive = mPersistentDynamicEmissiveCache.valid;
		if (shouldUsePersistentDynamicEmissive)
		{
			usingPersistentDynamicEmissiveCache = true;
			if (hasDynamicScene)
			{
				mergedDynamicSceneView = dynamicSceneView;
				std::unordered_set<uint64_t> seenSurfaceKeys;
				seenSurfaceKeys.reserve(
					mergedDynamicSceneView.opaqueWalls.size() +
					mergedDynamicSceneView.opaqueFlats.size() +
					mergedDynamicSceneView.opaqueSprites.size() +
					mPersistentDynamicEmissiveCache.sceneView.opaqueWalls.size() +
					mPersistentDynamicEmissiveCache.sceneView.opaqueFlats.size() +
					mPersistentDynamicEmissiveCache.sceneView.opaqueSprites.size());
				for (const auto& surface : mergedDynamicSceneView.opaqueWalls)
				{
					seenSurfaceKeys.insert(BuildPersistentEmissiveSurfaceIdentityKey(surface));
				}
				for (const auto& surface : mergedDynamicSceneView.opaqueFlats)
				{
					seenSurfaceKeys.insert(BuildPersistentEmissiveSurfaceIdentityKey(surface));
				}
				for (const auto& surface : mergedDynamicSceneView.opaqueSprites)
				{
					seenSurfaceKeys.insert(BuildPersistentEmissiveSurfaceIdentityKey(surface));
				}
				AppendUniquePersistentEmissiveSurfaces(
					mPersistentDynamicEmissiveCache.sceneView.opaqueWalls,
					mergedDynamicSceneView.opaqueWalls,
					seenSurfaceKeys);
				AppendUniquePersistentEmissiveSurfaces(
					mPersistentDynamicEmissiveCache.sceneView.opaqueFlats,
					mergedDynamicSceneView.opaqueFlats,
					seenSurfaceKeys);
				AppendUniquePersistentEmissiveSurfaces(
					mPersistentDynamicEmissiveCache.sceneView.opaqueSprites,
					mergedDynamicSceneView.opaqueSprites,
					seenSurfaceKeys);
				RebuildSceneViewStats(mergedDynamicSceneView);

				{
					Clocker clock(NriPTGeometryBuild);
					nri_scene::BuildGeometry(mergedDynamicSceneView, mergedDynamicGeometry);
					AssignGeometryPortalIndices(mMapWorld, mergedDynamicGeometry);
				}
				{
					Clocker clock(NriPTMaterialBuild);
					BuildMaterialsWithActorOverrides(mergedDynamicSceneView, mergedDynamicMaterialBridge, "dynamic_with_persistent_emissive");
				}

				if (!mergedDynamicGeometry.primitives.empty())
				{
					activeDynamicSceneView = &mergedDynamicSceneView;
					activeDynamicGeometry = &mergedDynamicGeometry;
					activeDynamicMaterials = &mergedDynamicMaterialBridge;
				}
			}
			else
			{
				activeDynamicSceneView = &mPersistentDynamicEmissiveCache.sceneView;
				activeDynamicGeometry = &mPersistentDynamicEmissiveCache.geometry;
				activeDynamicMaterials = &mPersistentDynamicEmissiveCache.materialBridge;
			}

			if (hasMirrorExtendedDynamicScene && activeDynamicSceneView != nullptr && activeDynamicMaterials != nullptr)
			{
				sceneLightMergedDynamicSceneView = *activeDynamicSceneView;
				sceneLightMergedDynamicSceneView.opaqueWalls.insert(
					sceneLightMergedDynamicSceneView.opaqueWalls.end(),
					mirrorExtendedDynamicSceneView.opaqueWalls.begin(),
					mirrorExtendedDynamicSceneView.opaqueWalls.end());
				sceneLightMergedDynamicSceneView.opaqueFlats.insert(
					sceneLightMergedDynamicSceneView.opaqueFlats.end(),
					mirrorExtendedDynamicSceneView.opaqueFlats.begin(),
					mirrorExtendedDynamicSceneView.opaqueFlats.end());
				sceneLightMergedDynamicSceneView.opaqueSprites.insert(
					sceneLightMergedDynamicSceneView.opaqueSprites.end(),
					mirrorExtendedDynamicSceneView.opaqueSprites.begin(),
					mirrorExtendedDynamicSceneView.opaqueSprites.end());
				RebuildSceneViewStats(sceneLightMergedDynamicSceneView);
				BuildMaterialsWithActorOverrides(sceneLightMergedDynamicSceneView, sceneLightMergedDynamicMaterialBridge, "scene_light_merged_persistent");
				sceneLightDynamicView = &sceneLightMergedDynamicSceneView;
				sceneLightDynamicMaterials = &sceneLightMergedDynamicMaterialBridge;
			}
			else if (activeDynamicSceneView != nullptr && activeDynamicMaterials != nullptr)
			{
				sceneLightDynamicView = activeDynamicSceneView;
				sceneLightDynamicMaterials = activeDynamicMaterials;
			}
			else if (hasMirrorExtendedDynamicScene)
			{
				sceneLightDynamicView = &mirrorExtendedDynamicSceneView;
				sceneLightDynamicMaterials = &mirrorExtendedDynamicMaterialBridge;
			}
		}

		const bool hasActiveDynamicOverlay =
			activeDynamicGeometry != nullptr &&
			!activeDynamicGeometry->primitives.empty() &&
			activeDynamicMaterials != nullptr;
		const bool hasMirrorExtendedDynamicOverlay =
			hasMirrorExtendedDynamicScene &&
			!mirrorExtendedDynamicGeometry.primitives.empty() &&
			!mirrorExtendedDynamicMaterialBridge.materials.empty();
		const bool hasMirrorPlayerOverlay =
			hasMirrorPlayerScene &&
			!mirrorPlayerGeometry.primitives.empty() &&
			!mirrorPlayerMaterialBridge.materials.empty();
		const bool hasRuntimeDebugSphereOverlay = !deferOverlayThisFrame && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeDebugSphereMs);
			return BuildRuntimeDebugSphereOverlay(debugSphereGeometry, debugSphereMaterialBridge);
		}();

		if (hasRuntimeSpaceLinkOverlay || hasRuntimeMutationOverlay || hasActiveDynamicOverlay || hasMirrorExtendedDynamicOverlay || hasMirrorPlayerOverlay || hasRuntimeDebugSphereOverlay)
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.overlayAssembleMs);
			overlayGeometry = {};
			overlayMaterialBridge = {};

			if (hasRuntimeSpaceLinkOverlay)
			{
				if (!runtimeSpaceLinkGeometry.primitives.empty())
				{
					AppendGeometry(runtimeSpaceLinkGeometry, (uint32_t)overlayMaterialBridge.materials.size(), overlayGeometry);
				}
				AppendMaterialBridge(runtimeSpaceLinkMaterialBridge, overlayMaterialBridge);
			}

			if (hasRuntimeMutationOverlay)
			{
				if (!runtimeMutationGeometry.primitives.empty())
				{
					AppendGeometry(runtimeMutationGeometry, (uint32_t)overlayMaterialBridge.materials.size(), overlayGeometry);
				}
				AppendMaterialBridge(runtimeMutationMaterialBridge, overlayMaterialBridge);
			}

			if (hasActiveDynamicOverlay)
			{
				AppendGeometry(*activeDynamicGeometry, (uint32_t)overlayMaterialBridge.materials.size(), overlayGeometry);
				AppendMaterialBridge(*activeDynamicMaterials, overlayMaterialBridge);
			}

			if (hasMirrorExtendedDynamicOverlay)
			{
				AppendGeometry(mirrorExtendedDynamicGeometry, (uint32_t)overlayMaterialBridge.materials.size(), overlayGeometry);
				AppendMaterialBridge(mirrorExtendedDynamicMaterialBridge, overlayMaterialBridge);
			}

			if (hasMirrorPlayerOverlay)
			{
				AppendGeometry(mirrorPlayerGeometry, (uint32_t)overlayMaterialBridge.materials.size(), overlayGeometry);
				AppendMaterialBridge(mirrorPlayerMaterialBridge, overlayMaterialBridge);
			}

			if (hasRuntimeDebugSphereOverlay)
			{
				AppendGeometry(debugSphereGeometry, (uint32_t)overlayMaterialBridge.materials.size(), overlayGeometry);
				AppendMaterialBridge(debugSphereMaterialBridge, overlayMaterialBridge);
			}

			mLastPerfShellTraceStats.overlayPrimitiveCount = (uint32_t)overlayGeometry.primitives.size();
			mLastPerfShellTraceStats.overlayMaterialCount = (uint32_t)overlayMaterialBridge.materials.size();

			std::vector<nri::TopLevelInstance> instances;
			std::vector<SceneInstanceData> sceneInstances;
			const std::vector<uint8_t>* replacedChunkMask = hasRuntimeMutationOverlay ? &mRuntimeMapMutations.replacedChunkMask : nullptr;
			BuildStaticMapInstances(instances, sceneInstances, replacedChunkMask);

			if (overlayGeometry.primitives.empty())
			{
				accelerationReady =
					BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static) &&
					UpdateSceneDataSet(
						mStaticVertexBuffer,
						mStaticIndexBuffer,
						mStaticPrimitiveBuffer,
						mStaticMaterialBuffer,
						mStaticVertexBuffer,
						mStaticIndexBuffer,
						mStaticPrimitiveBuffer,
						mStaticMaterialBuffer,
						sceneInstances,
						(uint32_t)mStaticMapScene.geometry.primitives.size(),
						0u,
						(uint32_t)mStaticMapScene.gpuMaterials.size(),
						0u,
						"static_only_scene");
				if (accelerationReady && hasRuntimeMutationOverlay)
				{
					mBuiltStaticMapSceneASLastFrame = false;
				}
			}
			else
			{
				combinedMaterialBridge = mStaticMapScene.materialBridge;
				AppendMaterialBridge(overlayMaterialBridge, combinedMaterialBridge);
				paletteReady = EnsurePaletteTexture(combinedMaterialBridge);
				if (ShouldTraceSkyPerf())
				{
					gRendererSkyPerfTraceStats.combinedOverlayTextureBuilds++;
				}
				texturesReady = paletteReady && EnsureSceneTextures(mStaticMapScene.sceneView, combinedMaterialBridge, combinedGpuMaterials, false, "static_map_overlay_combined");
				dynamicGpuMaterials.clear();
				if (texturesReady)
				{
					const size_t staticMaterialCount = mStaticMapScene.gpuMaterials.size();
					if (combinedGpuMaterials.size() < staticMaterialCount)
					{
						texturesReady = false;
					}
					else
					{
						dynamicGpuMaterials.assign(combinedGpuMaterials.begin() + staticMaterialCount, combinedGpuMaterials.end());
					}
				}
				buffersReady = texturesReady && UploadSceneBuffers(overlayGeometry, dynamicGpuMaterials);
				accelerationReady = false;
				if (buffersReady)
				{
					accelerationReady =
						BuildDynamicAccelerationStructure(overlayGeometry) &&
						mDynamicBottomLevelAS.accelerationStructure != nullptr;
				}
				emissiveSamplingContext.runtimeMutationGeometry = hasRuntimeMutationOverlay ? &runtimeMutationGeometry : nullptr;
				emissiveSamplingContext.runtimeMutationPrimitiveBaseOffset = (uint32_t)runtimeSpaceLinkGeometry.primitives.size();
				emissiveSamplingContext.dynamicGeometry = hasActiveDynamicOverlay ? activeDynamicGeometry : nullptr;
				emissiveSamplingContext.dynamicPrimitiveBaseOffset = (uint32_t)(runtimeSpaceLinkGeometry.primitives.size() + runtimeMutationGeometry.primitives.size());
				if (accelerationReady)
				{
					nri::TopLevelInstance dynamicInstance = {};
					dynamicInstance.transform[0][0] = 1.0f;
					dynamicInstance.transform[1][1] = 1.0f;
					dynamicInstance.transform[2][2] = 1.0f;
					dynamicInstance.instanceId = (uint32_t)sceneInstances.size();
					dynamicInstance.mask = 0xFF;
					dynamicInstance.shaderBindingTableLocalOffset = 0;
					dynamicInstance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
					dynamicInstance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*mDynamicBottomLevelAS.accelerationStructure);
					instances.push_back(dynamicInstance);
					sceneInstances.push_back({ 0u, NRI_SCENE_DATA_SOURCE_DYNAMIC, 0u, 0u });

					accelerationReady =
						BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static | SceneDataBufferMask_Dynamic) &&
						UpdateSceneDataSet(
							mStaticVertexBuffer,
							mStaticIndexBuffer,
							mStaticPrimitiveBuffer,
							mStaticMaterialBuffer,
							mVertexBuffer,
							mIndexBuffer,
							mPrimitiveBuffer,
							mMaterialBuffer,
							sceneInstances,
							(uint32_t)mStaticMapScene.geometry.primitives.size(),
							(uint32_t)overlayGeometry.primitives.size(),
							(uint32_t)mStaticMapScene.gpuMaterials.size(),
							(uint32_t)dynamicGpuMaterials.size(),
							"static_plus_overlay_scene");
				}
			}

			if (overlayGeometry.primitives.empty() || texturesReady)
			{
				PrepareSceneTextureInputsForCompute();
			}

			if (paletteReady && texturesReady && buffersReady && accelerationReady)
			{
				mUsedDynamicSceneLastFrame = hasActiveDynamicOverlay || hasMirrorExtendedDynamicOverlay || hasMirrorPlayerOverlay;
				mGpuSceneHasDynamicOverlay = true;
				if (activeDynamicSceneView != nullptr && activeDynamicGeometry != nullptr && activeDynamicMaterials != nullptr)
				{
					mDynamicSceneLastFrame.spriteSurfaceCount = (uint32_t)activeDynamicSceneView->opaqueSprites.size();
					mDynamicSceneLastFrame.primitiveCount = (uint32_t)activeDynamicGeometry->primitives.size();
					mDynamicSceneLastFrame.materialCount = (uint32_t)activeDynamicMaterials->materials.size();
					mDynamicSceneLastFrame.modelCount = activeDynamicSceneView->stats.modelDrawItems;
					mDynamicSceneLastFrame.unsupportedModelCount = activeDynamicSceneView->stats.unsupportedModelDrawItems;
				}
				if (hasMirrorExtendedDynamicScene)
				{
					mDynamicSceneLastFrame.mirrorExtendedSurfaceCount = CountSceneViewSurfaces(mirrorExtendedDynamicSceneView);
					mDynamicSceneLastFrame.mirrorExtendedPrimitiveCount = (uint32_t)mirrorExtendedDynamicGeometry.primitives.size();
					mDynamicSceneLastFrame.mirrorExtendedMaterialCount = (uint32_t)mirrorExtendedDynamicMaterialBridge.materials.size();
					mDynamicSceneLastFrame.mirrorExtendedModelCount = mirrorExtendedDynamicSceneView.stats.modelDrawItems;
					mDynamicSceneLastFrame.mirrorExtendedUnsupportedModelCount = mirrorExtendedDynamicSceneView.stats.unsupportedModelDrawItems;
				}
				if (hasMirrorPlayerScene)
				{
					mDynamicSceneLastFrame.mirrorPlayerSurfaceCount = CountSceneViewSurfaces(mirrorPlayerSceneView);
					mDynamicSceneLastFrame.mirrorPlayerPrimitiveCount = (uint32_t)mirrorPlayerGeometry.primitives.size();
					mDynamicSceneLastFrame.mirrorPlayerMaterialCount = (uint32_t)mirrorPlayerMaterialBridge.materials.size();
					mDynamicSceneLastFrame.mirrorPlayerModelCount = mirrorPlayerSceneView.stats.modelDrawItems;
					mDynamicSceneLastFrame.mirrorPlayerUnsupportedModelCount = mirrorPlayerSceneView.stats.unsupportedModelDrawItems;
				}
				if (!overlayGeometry.primitives.empty())
				{
					const bool useFilteredStaticProbeGeometry =
						hasRuntimeMutationOverlay &&
						!mRuntimeMapMutations.replacedChunkMask.empty();
					nri_scene::GeometryData filteredStaticGeometry;
					if (useFilteredStaticProbeGeometry)
					{
						BuildFilteredStaticMapGeometry(mRuntimeMapMutations.replacedChunkMask, filteredStaticGeometry);
						activeStaticProbePrimitiveCount = (uint32_t)filteredStaticGeometry.primitives.size();
						combinedGeometry = std::move(filteredStaticGeometry);
					}
					else
					{
						combinedGeometry = mStaticMapScene.geometry;
						activeStaticProbePrimitiveCount = (uint32_t)mStaticMapScene.geometry.primitives.size();
					}
					AppendGeometry(overlayGeometry, (uint32_t)mStaticMapScene.materialBridge.materials.size(), combinedGeometry);
					activeGeometry = &combinedGeometry;
					activeGpuMaterials = &combinedGpuMaterials;
					activeMaterialBridge = &combinedMaterialBridge;
				}
				else
				{
					activeGeometry = &mStaticMapScene.geometry;
					activeGpuMaterials = &mStaticMapScene.gpuMaterials;
					activeMaterialBridge = &mStaticMapScene.materialBridge;
				}

				nri_scene::SceneDebugStats dynamicOverlayStats =
					activeDynamicSceneView != nullptr ? activeDynamicSceneView->stats : nri_scene::SceneDebugStats{};
				if (hasMirrorExtendedDynamicScene)
				{
					dynamicOverlayStats = MergeSceneStats(dynamicOverlayStats, mirrorExtendedDynamicSceneView.stats);
				}
				if (hasMirrorPlayerScene)
				{
					dynamicOverlayStats = MergeSceneStats(dynamicOverlayStats, mirrorPlayerSceneView.stats);
				}
				activeStats = MergeSceneStats(mStaticMapScene.sceneView.stats, dynamicOverlayStats);
			}
			else
			{
				LogFallback("PT runtime/dynamic overlay update failed; tracing the resident static world only.");
				if (mGpuSceneHasDynamicOverlay)
				{
					RestoreStaticTopLevelScene();
				}
				paletteReady = true;
				texturesReady = true;
				buffersReady = true;
				accelerationReady = true;
			}
		}
		else if (deferOverlayThisFrame)
		{
			Printf("NRI PT dynamic scene deferred: skipping dynamic overlay on the same frame that rebuilt resident static map assets.\n");
		}
		else if (mGpuSceneHasDynamicOverlay)
		{
			DestroyAccelerationStructureResource(mDynamicBottomLevelAS);
			if (!RestoreStaticTopLevelScene())
			{
				LogFallback("PT static scene restore failed after dynamic overlay.");
				if (preserveHistory)
				{
					restoreHistory();
				}
				return false;
			}

			mGpuSceneHasDynamicOverlay = false;
			mUsedStaticMapSceneLastFrame = true;
			activeSceneView = &mStaticMapScene.sceneView;
			activeGeometry = &mStaticMapScene.geometry;
			activeGpuMaterials = &mStaticMapScene.gpuMaterials;
			activeMaterialBridge = &mStaticMapScene.materialBridge;
			activeStats = mStaticMapScene.sceneView.stats;
		}
		else
		{
			mGpuSceneHasDynamicOverlay = false;
		}
	}
	else
	{
		ResetPersistentDynamicEmissiveCache();
		Clocker clock(NriPTSceneCapture);
		if (!nri_scene::CaptureScene(di, capturedSceneView))
		{
			LogFallback("PT scene capture failed.");
			if (preserveHistory)
			{
				restoreHistory();
			}
			return false;
		}

		activeSceneView = &capturedSceneView;
		activeMaterialBridge = &materialBridge;
		sceneLightCapturedView = &capturedSceneView;
		activeStats = capturedSceneView.stats;

		{
			Clocker clock(NriPTGeometryBuild);
			nri_scene::BuildGeometry(capturedSceneView, capturedGeometry);
			AssignGeometryPortalIndices(mMapWorld, capturedGeometry);
		}

		{
			Clocker clock(NriPTMaterialBuild);
			BuildMaterialsWithActorOverrides(capturedSceneView, materialBridge, "captured_scene");
		}
		sceneLightCapturedMaterials = &materialBridge;

		const bool needsFallbackMaterials = bootstrapCapturedDiagnostics || bootstrapCapturedFlat;
		const bool needsRealTextures = !nri_ptbootstrap || bootstrapCapturedBaseColor || bootstrapMode >= 13u;
		paletteReady = needsRealTextures ? EnsurePaletteTexture(materialBridge) : true;
		texturesReady = needsFallbackMaterials ? UseFallbackSceneTextures(preserveHistory, "captured_scene_fallback") : (needsRealTextures ? (paletteReady && EnsureSceneTextures(capturedSceneView, materialBridge, capturedGpuMaterials, preserveHistory, "captured_scene")) : EnsureSkyTexture(capturedSceneView, preserveHistory));
		if (needsFallbackMaterials)
		{
			capturedGpuMaterials = materialBridge.materials;
			for (auto& material : capturedGpuMaterials)
			{
				material.textureIndex = 0;
				material.paletteIndex = 0;
				material.flags = 0;
				material.normalTextureIndex = UINT32_MAX;
				material.metallicTextureIndex = UINT32_MAX;
				material.roughnessTextureIndex = UINT32_MAX;
				material.emissiveTextureIndex = UINT32_MAX;
				material.lightLevel = 1.0f;
				material.alpha = 1.0f;
			}
		}
		else if (!needsRealTextures)
		{
			capturedGpuMaterials = materialBridge.materials;
		}

		buffersReady = texturesReady && UploadSceneBuffers(capturedGeometry, capturedGpuMaterials);
		std::vector<SceneInstanceData> sceneInstances;
		if (buffersReady)
		{
			sceneInstances.push_back({ 0u, NRI_SCENE_DATA_SOURCE_DYNAMIC, 0u, 0u });
			buffersReady = UpdateSceneDataSet(
				mVertexBuffer,
				mIndexBuffer,
				mPrimitiveBuffer,
				mMaterialBuffer,
				mVertexBuffer,
				mIndexBuffer,
				mPrimitiveBuffer,
				mMaterialBuffer,
				sceneInstances,
				0u,
				(uint32_t)capturedGeometry.primitives.size(),
				0u,
				(uint32_t)capturedGpuMaterials.size(),
				"captured_scene");
		}
		if (texturesReady)
		{
			PrepareSceneTextureInputsForCompute();
		}
		if (bootstrapCapturedView || rawTraceDirectScene)
		{
			accelerationReady = true;
		}
		else if (buffersReady)
		{
			accelerationReady =
				BuildDynamicAccelerationStructure(capturedGeometry) &&
				mDynamicBottomLevelAS.accelerationStructure != nullptr;
			if (accelerationReady)
			{
				nri::TopLevelInstance instance = {};
				instance.transform[0][0] = 1.0f;
				instance.transform[1][1] = 1.0f;
				instance.transform[2][2] = 1.0f;
				instance.instanceId = 0;
				instance.mask = 0xFF;
				instance.shaderBindingTableLocalOffset = 0;
				instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
				instance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*mDynamicBottomLevelAS.accelerationStructure);

				std::vector<nri::TopLevelInstance> instances = { instance };
				accelerationReady = BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Dynamic);
			}
		}
		else
		{
			accelerationReady = false;
		}
		activeGeometry = &capturedGeometry;
		activeGpuMaterials = &capturedGpuMaterials;
		emissiveSamplingContext.capturedGeometry = &capturedGeometry;
		}
	}

	if (activeSceneView == nullptr || activeGeometry == nullptr || activeGpuMaterials == nullptr || activeMaterialBridge == nullptr)
	{
		LogFallback("PT scene selection failed.");
		if (preserveHistory)
		{
			restoreHistory();
		}
		return false;
	}

	RefreshSceneLightSystem(
		sceneLightUsesStaticMapScene,
		sceneLightCapturedView,
		sceneLightCapturedMaterials,
		sceneLightDynamicView,
		sceneLightDynamicMaterials);

	if (mGpuSceneHasDynamicOverlay &&
		activeMaterialBridge == &combinedMaterialBridge &&
		!overlayGeometry.primitives.empty())
	{
		std::vector<nri_scene::MaterialData> refreshedCombinedGpuMaterials = combinedMaterialBridge.materials;
		ApplyEmissiveMaterialOverrides(combinedMaterialBridge, refreshedCombinedGpuMaterials);
		ApplyActorShadowMaterialOverrides(combinedMaterialBridge, refreshedCombinedGpuMaterials);
		if (!MaterialDataVectorEqual(refreshedCombinedGpuMaterials, combinedGpuMaterials))
		{
			const size_t staticMaterialCount = mStaticMapScene.gpuMaterials.size();
			if (refreshedCombinedGpuMaterials.size() < staticMaterialCount)
			{
				LogFallback("PT runtime overlay material refresh produced an invalid material slice.");
				if (preserveHistory)
				{
					restoreHistory();
				}
				return false;
			}

			combinedGpuMaterials = std::move(refreshedCombinedGpuMaterials);
			dynamicGpuMaterials.assign(combinedGpuMaterials.begin() + staticMaterialCount, combinedGpuMaterials.end());
			if (!UploadSceneBuffers(overlayGeometry, dynamicGpuMaterials) ||
				!UpdateSceneDataSet(
					mStaticVertexBuffer,
					mStaticIndexBuffer,
					mStaticPrimitiveBuffer,
					mStaticMaterialBuffer,
					mVertexBuffer,
					mIndexBuffer,
					mPrimitiveBuffer,
					mMaterialBuffer,
					mBoundSceneInstances,
					(uint32_t)mStaticMapScene.geometry.primitives.size(),
					(uint32_t)overlayGeometry.primitives.size(),
					(uint32_t)mStaticMapScene.gpuMaterials.size(),
					(uint32_t)dynamicGpuMaterials.size(),
					"resident_overlay_material_refresh"))
			{
				LogFallback("PT runtime overlay material refresh failed after scene-light rebuild.");
				if (preserveHistory)
				{
					restoreHistory();
				}
				return false;
			}

			activeGpuMaterials = &combinedGpuMaterials;
		}
	}

	if (sceneLightUsesStaticMapScene && !mGpuSceneHasDynamicOverlay)
	{
		const bool needsResidentStaticLightRefresh =
			!mSceneLights.GetAnalyticLights().activeLights.empty() ||
			mBoundRuntimeLightCount != 0 ||
			mSceneLights.GetSectorLighting().activeSectorCount > 0 ||
			mBoundSectorLightActiveCount != 0;
		if (needsResidentStaticLightRefresh)
		{
			if (!RefreshResidentStaticSceneDataSet())
			{
				LogFallback("PT static scene light refresh failed.");
				if (preserveHistory)
				{
					restoreHistory();
				}
				return false;
			}
		}
	}

	if (!UpdateEmissiveSamplingBuffers(emissiveSamplingContext))
	{
		LogFallback("PT emissive primitive update failed.");
		if (preserveHistory)
		{
			restoreHistory();
		}
		return false;
	}
	if (!BuildEmissiveTopLevelAccelerationStructure())
	{
		LogFallback("PT emissive TLAS update failed.");
		if (preserveHistory)
		{
			restoreHistory();
		}
		return false;
	}

	TraceRuntimeLinkEvents(di);
	LogBridgeStats(activeStats);
	if (activeStats.unsupportedModelDrawItems > 0)
	{
		LogFallback("generic GLDL_MODELS content is unsupported in the PT bridge; rendering the supported PT scene without those model draws.");
	}

	Copy3(activeSceneView->skyColor, mSkyColor);
	Copy3(activeSceneView->groundColor, mGroundColor);
	mSurfaceProbeFrame = {};
	mSurfaceProbeFrame.valid = true;
	mSurfaceProbeFrame.usesStaticMapScene = mUsedStaticMapSceneLastFrame;
	mSurfaceProbeFrame.staticTlasExcludesReplacedChunks = !runtimeMutationGeometry.primitives.empty();
	mSurfaceProbeFrame.staticProbeExcludesReplacedChunks =
		mUsedStaticMapSceneLastFrame &&
		activeGeometry != nullptr &&
		activeGeometry != &mStaticMapScene.geometry &&
		!runtimeMutationGeometry.primitives.empty();
	mSurfaceProbeFrame.staticPrimitiveCount = mUsedStaticMapSceneLastFrame ? activeStaticProbePrimitiveCount : 0u;
	mSurfaceProbeFrame.runtimeSpaceLinkPrimitiveCount = (uint32_t)runtimeSpaceLinkGeometry.primitives.size();
	mSurfaceProbeFrame.runtimeMutationPrimitiveCount = (uint32_t)runtimeMutationGeometry.primitives.size();
	mSurfaceProbeFrame.dynamicPrimitiveCount = activeDynamicGeometry != nullptr ? (uint32_t)activeDynamicGeometry->primitives.size() : 0u;

	if (!preserveHistory)
	{
		UpdateSurfaceProbe(*activeGeometry, activeMaterialBridge, true);
	}
	if (activeGeometry->primitives.empty())
	{
		LogFallback("PT scene path produced no supported opaque geometry.");
		if (preserveHistory)
		{
			restoreHistory();
		}
		return false;
	}

	if (mUsedStaticMapSceneLastFrame)
	{
		PrepareSceneTextureInputsForCompute();
	}

	bool dispatched = false;
	if (bootstrapCapturedView)
	{
		mHistoryInputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPing : FrameTextureSlot::TaaHistoryPong;
		mHistoryOutputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPong : FrameTextureSlot::TaaHistoryPing;
		mUpscaledInputSlot = FrameTextureSlot::Composed;
		mUseUpscaledInFinal = false;
		dispatched = buffersReady && DispatchBootstrapView();
	}
	else
	{
		dispatched = accelerationReady && DispatchFrameGraph(di, *activeGeometry, *activeGpuMaterials, drawmode);
	}
	const bool success = paletteReady && texturesReady && buffersReady && accelerationReady && dispatched;

	if (!paletteReady)
	{
		LogFallback("PT palette texture upload failed.");
	}
	else if (!texturesReady)
	{
		LogFallback("PT material texture upload failed.");
	}
	else if (!buffersReady)
	{
		LogFallback("PT scene buffer upload failed.");
	}
	else if (!accelerationReady)
	{
		LogFallback("PT acceleration structure build failed.");
	}
	else if (!dispatched)
	{
		LogFallback(bootstrapCapturedView ? "PT bootstrap captured-scene dispatch failed." : "PT frame graph dispatch failed.");
	}

	if (success)
	{
		mHasLoggedFallback = false;
		if (bootstrapCapturedView)
		{
			CopyFinalToActiveTarget();
		}

		if (!preserveHistory)
		{
			NoteSuccessfulRealFrame();
			mFrameIndex++;
			mHasPreviousCameraState = true;
			mResetHistory = false;
		}
		else
		{
			restoreHistory();
		}
	}
	else if (preserveHistory)
	{
		restoreHistory();
	}

	if (success)
	{
		mLastPerfShellTraceStats.activePrimitiveCount = (uint32_t)activeGeometry->primitives.size();
		mLastPerfShellTraceStats.dynamicPrimitiveCount = activeDynamicGeometry != nullptr ? (uint32_t)activeDynamicGeometry->primitives.size() : 0u;
		mLastPerfShellTraceStats.activeMaterialCount = (uint32_t)activeGpuMaterials->size();
		mLastPerfShellTraceStats.sceneInstanceCount = (uint32_t)mBoundSceneInstances.size();
		mLastPerfShellTraceStats.usedStaticMapScene = mUsedStaticMapSceneLastFrame;
		mLastPerfShellTraceStats.usedDynamicOverlay = mGpuSceneHasDynamicOverlay;
		mLastPerfShellTraceStats.usedPersistentDynamicEmissiveCache = usingPersistentDynamicEmissiveCache;
		const double accountedMs =
			mLastPerfShellTraceStats.initResourcesMs +
			mLastPerfShellTraceStats.mapWorldMs +
			mLastPerfShellTraceStats.updateStateMs +
			mLastPerfShellTraceStats.sceneSelectMs +
			mLastPerfShellTraceStats.sceneLightsMs +
			mLastPerfShellTraceStats.residentLightRefreshMs +
			mLastPerfShellTraceStats.emissiveUpdateMs +
			mLastPerfShellTraceStats.emissiveTlasMs +
			mLastPerfShellTraceStats.surfaceProbeMs +
			mLastPerfShellTraceStats.frameGraphMs;
		mLastPerfShellTraceStats.otherMs = std::max(0.0, mLastPerfShellTraceStats.totalMs - accountedMs);
	}

	if (ShouldEmitTemporalTraceLogs())
	{
		const auto& analyticLights = mSceneLights.GetAnalyticLights();
		const auto& emissiveSurfaces = mSceneLights.GetEmissiveSurfaces();
		Printf("NRI PT light trace: frame=%u analytic=%u topo=%s prop=%s added=%u removed=%u rebound=%u emissive=%u topo=%s prop=%s added=%u removed=%u rebound=%u reset=%s reason=%s\n",
			traceFrameIndex,
			(uint32_t)analyticLights.activeLights.size(),
			YesNo(analyticLights.lastBuildTopologyChanged),
			YesNo(analyticLights.lastBuildPropertiesChanged),
			(uint32_t)analyticLights.addedTopologyKeys.size(),
			(uint32_t)analyticLights.removedTopologyKeys.size(),
			(uint32_t)analyticLights.reboundTopologyKeys.size(),
			(uint32_t)emissiveSurfaces.activeSurfaces.size(),
			YesNo(emissiveSurfaces.lastBuildTopologyChanged),
			YesNo(emissiveSurfaces.lastBuildPropertiesChanged),
			(uint32_t)emissiveSurfaces.addedTopologyKeys.size(),
			(uint32_t)emissiveSurfaces.removedTopologyKeys.size(),
			(uint32_t)emissiveSurfaces.reboundTopologyKeys.size(),
			YesNo(mResetHistory),
			mResetHistory ? mLastHistoryResetReason.c_str() : "none");

		const nri_scene::SkyPerfStats sceneSkyPerf = nri_scene::ConsumeSkyPerfStats();
		Printf("NRI PT sky perf: frame=%u ensure_scene=%u preserve_scene=%u rebuild_scene=%u ensure_sky=%u preserve_hit=%u reuse_active=%u reuse_probe=%u probe=%u/%u face_probes=%u uploads=%u ensure_ms=%.3f probe_ms=%.3f face_ms=%.3f upload_ms=%.3f static_builds=%u overlay_builds=%u\n",
			traceFrameIndex,
			gRendererSkyPerfTraceStats.ensureSceneTexturesCalls,
			gRendererSkyPerfTraceStats.ensureSceneTexturesPreserveTrueCalls,
			gRendererSkyPerfTraceStats.ensureSceneTexturesPreserveFalseCalls,
			gRendererSkyPerfTraceStats.ensureSkyCalls,
			gRendererSkyPerfTraceStats.preserveExistingHits,
			gRendererSkyPerfTraceStats.reuseActiveCubemapHits + gRendererSkyPerfTraceStats.solidReuseHits,
			gRendererSkyPerfTraceStats.reuseActiveProbeHits,
			gRendererSkyPerfTraceStats.probeSuccesses,
			gRendererSkyPerfTraceStats.probeAttempts,
			gRendererSkyPerfTraceStats.probeFaceCalls,
			gRendererSkyPerfTraceStats.buildCubemapUploadCalls,
			(double)gRendererSkyPerfTraceStats.ensureSkyTimeUs / 1000.0,
			(double)gRendererSkyPerfTraceStats.probeCubemapTimeUs / 1000.0,
			(double)gRendererSkyPerfTraceStats.probeFaceTimeUs / 1000.0,
			(double)gRendererSkyPerfTraceStats.buildCubemapUploadTimeUs / 1000.0,
			gRendererSkyPerfTraceStats.residentStaticSceneTextureBuilds,
			gRendererSkyPerfTraceStats.combinedOverlayTextureBuilds);
		Printf("NRI PT sky scene: frame=%u updates=%u wall=%u flat=%u portal=%u inspects=%u cubemap_candidates=%u solid_candidates=%u inspect_faces=%u avg_base=%u avg_recursive=%u recursive_faces=%u avg_pixels=%llu update_ms=%.3f inspect_ms=%.3f avg_ms=%.3f\n",
			traceFrameIndex,
			sceneSkyPerf.updateCalls,
			sceneSkyPerf.wallUpdateCalls,
			sceneSkyPerf.flatUpdateCalls,
			sceneSkyPerf.portalUpdateCalls,
			sceneSkyPerf.inspectCalls,
			sceneSkyPerf.inspectCubemapCandidates,
			sceneSkyPerf.inspectSolidCandidates,
			sceneSkyPerf.inspectFaceWalks,
			sceneSkyPerf.averageColorBaseCalls,
			sceneSkyPerf.averageColorRecursiveCalls,
			sceneSkyPerf.recursiveSkyboxFaceSamples,
			(unsigned long long)sceneSkyPerf.averageColorPixels,
			(double)sceneSkyPerf.updateTimeUs / 1000.0,
			(double)sceneSkyPerf.inspectTimeUs / 1000.0,
			(double)sceneSkyPerf.averageColorTimeUs / 1000.0);
		Printf("NRI PT sky invalidation: frame=%u requests=%u applied=%u emissive_material_dirty=%u keep_last=%u hold_level=%u cached_cubemap=%u create_cubemap=%u cached_solid=%u create_solid=%u\n",
			traceFrameIndex,
			gRendererSkyPerfTraceStats.lightingInvalidationRequests,
			gRendererSkyPerfTraceStats.lightingInvalidationsApplied,
			gRendererSkyPerfTraceStats.emissiveMaterialDirtyEvents,
			gRendererSkyPerfTraceStats.keepLastCubemapHits,
			gRendererSkyPerfTraceStats.holdLevelCubemapHits,
			gRendererSkyPerfTraceStats.activateCachedCubemapHits,
			gRendererSkyPerfTraceStats.createCachedCubemapHits,
			gRendererSkyPerfTraceStats.solidActivateHits,
			gRendererSkyPerfTraceStats.solidCreateHits);
	}

	return success;
}

bool NRIRenderer::PreloadLevelScene(uint32_t outputWidth, uint32_t outputHeight, uint32_t targetWidth, uint32_t targetHeight)
{
	if (mFrameBuffer == nullptr || mFrameBuffer->mCommandBuffer == nullptr || mFrameBuffer->mActiveTarget == nullptr)
	{
		return false;
	}

	if (!RefreshPathTracingAvailability() || !mPathTracingSupported)
	{
		return true;
	}

	ResetPerfTraceStats();
	{
		ScopedPtPerfTimer initPerfTimer(mLastPerfShellTraceStats.initResourcesMs);
		if (!Initialize() || !EnsureFrameResources(outputWidth, outputHeight, targetWidth, targetHeight))
		{
			LogFallback("PT preload frame resources or pipelines failed to initialize.");
			return true;
		}
	}

	ResetSceneBufferFrameStats();
	ResetRendererSkyPerfTraceStats();
	nri_scene::ResetAverageTextureColorCache();
	nri_scene::ResetSkyPerfStats();

	RefreshMapWorld();
	if (mPendingStartupMutationRebaseline)
	{
		RebuildStartupMutationBaseline();
	}
	if (mPendingRuntimeMutationRebaseline || mRuntimeMutationRebaselineState != RuntimeMutationRebaselineState::Idle)
	{
		AdvanceRuntimeMutationRebaseline();
	}
	if (!mMapWorld.valid)
	{
		return true;
	}

	if (!EnsureStaticMapScene())
	{
		LogFallback("PT preload resident static scene build failed.");
		return true;
	}

	RefreshSceneLightSystem(true, nullptr, nullptr, nullptr, nullptr);
	if (!mGpuSceneHasDynamicOverlay)
	{
		const bool needsResidentStaticLightRefresh =
			!mSceneLights.GetAnalyticLights().activeLights.empty() ||
			mBoundRuntimeLightCount != 0 ||
			mSceneLights.GetSectorLighting().activeSectorCount > 0 ||
			mBoundSectorLightActiveCount != 0;
		if (needsResidentStaticLightRefresh && !RefreshResidentStaticSceneDataSet())
		{
			LogFallback("PT preload static scene light refresh failed.");
			return true;
		}
	}

	EmissiveSamplingBuildContext emissiveSamplingContext = {};
	emissiveSamplingContext.staticGeometry = &mStaticMapScene.geometry;
	if (!UpdateEmissiveSamplingBuffers(emissiveSamplingContext))
	{
		LogFallback("PT preload emissive primitive update failed.");
		return true;
	}
	if (!BuildEmissiveTopLevelAccelerationStructure())
	{
		LogFallback("PT preload emissive TLAS update failed.");
		return true;
	}

	PrepareSceneTextureInputsForCompute();
	Printf("NRI PT preload ready: level=%s build_serial=%llu chunks=%u tris=%u materials=%u\n",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)mMapWorld.buildSerial,
		(uint32_t)mStaticMapScene.chunks.size(),
		(uint32_t)mStaticMapScene.geometry.primitives.size(),
		(uint32_t)mStaticMapScene.gpuMaterials.size());
	return true;
}

void NRIRenderer::ResetHistory()
{
	RequestHistoryReset("history-reset", true, true);
}

void NRIRenderer::NotifyCameraCut(const char* reason)
{
	RequestHistoryReset((reason != nullptr && *reason != '\0') ? reason : "camera-cut", true, false);
}

void NRIRenderer::SetGuiCaptureState(bool active)
{
	if (mGuiCaptureActive == active)
	{
		return;
	}

	mGuiCaptureActive = active;
	if (nri_ptscenestats)
	{
		const NRIMainUpscalerKind resolvedMain = ResolveMainUpscalerKind(false);
		const nri::UpscalerMode resolvedUpscalerMode = ResolveUpscalerModeForMain(resolvedMain, GetSelectedUpscalerMode());
		Printf("NRI PT gui capture: frame=%u active=%s jitter=%s phases=%u\n",
			mFrameIndex,
			mGuiCaptureActive ? "yes" : "no",
			GetTemporalJitterModeName(resolvedMain, mGuiCaptureActive),
			GetTemporalJitterPhaseCount(resolvedMain, resolvedUpscalerMode, mGuiCaptureActive));
	}
}

void NRIRenderer::RequestHistoryReset(const char* reason, bool clearPreviousCameraState, bool clearRuntimeChunkTranslationHistory)
{
	ArmTemporalTraceBudget(reason);
	mResetHistory = true;
	mLastHistoryResetReason = (reason != nullptr && *reason != '\0') ? reason : "unspecified";
	if (clearPreviousCameraState)
	{
		mHasPreviousCameraState = false;
	}
	if (clearRuntimeChunkTranslationHistory)
	{
		mRuntimeChunkTranslationHistory.clear();
	}
}

void NRIRenderer::NoteLightHistoryChange(const char* reason)
{
	ArmTemporalTraceBudget(reason);
	if (ShouldEmitTemporalTraceLogs())
	{
		Printf("NRI PT light change: reason=%s frame=%u reset=no\n",
			(reason != nullptr && *reason != '\0') ? reason : "unspecified",
			mFrameIndex);
	}
}

bool NRIRenderer::AddRuntimePointLight(const float position[3], const float color[3], float intensity, float radius, uint32_t& outId)
{
	if (position == nullptr || color == nullptr || intensity <= 0.0f || radius <= 0.0f)
	{
		return false;
	}

	if (mSceneLights.GetManualAnalyticLightCount() >= NRI_MAX_RUNTIME_POINT_LIGHTS)
	{
		return false;
	}

	outId = mNextRuntimePointLightId++;
	if (!mSceneLights.AddManualAnalyticLight(outId, position, color, intensity, radius))
	{
		return false;
	}
	mBoundRuntimeLightCount = 0;
	NoteLightHistoryChange("runtime-light-change");
	return true;
}

bool NRIRenderer::RemoveRuntimePointLight(uint32_t id)
{
	if (!mSceneLights.RemoveManualAnalyticLight(id))
	{
		return false;
	}

	mBoundRuntimeLightCount = 0;
	NoteLightHistoryChange("runtime-light-change");
	return true;
}

void NRIRenderer::ClearRuntimePointLights()
{
	if (mSceneLights.GetManualAnalyticLightCount() == 0)
	{
		return;
	}

	mSceneLights.ClearManualAnalyticLights();
	mBoundRuntimeLightCount = 0;
	NoteLightHistoryChange("runtime-light-change");
}

void NRIRenderer::PrintRuntimePointLights() const
{
	const auto& analyticLights = mSceneLights.GetAnalyticLights();
	Printf("NRI PT analytic lights: active=%u manual=%u muzzle_slots=%u muzzle_active=%u rules=%u overlay_rules=%u map_rules=%u matched_surfaces=%u overlay_matches=%u deduped=%u truncated=%u topo_changed=%s prop_changed=%s added=%u removed=%u rebound=%u limit=%u\n",
		(uint32_t)analyticLights.activeLights.size(),
		(uint32_t)analyticLights.manualLights.size(),
		analyticLights.transientMuzzleSlotCount,
		analyticLights.transientMuzzleActiveCount,
		(uint32_t)analyticLights.spriteTileRules.size(),
		analyticLights.actorOverlayRuleCount,
		analyticLights.mapOverlayRuleCount,
		analyticLights.matchedSurfaceCount,
		analyticLights.actorOverlayMatchedSurfaceCount,
		analyticLights.dedupedMatchCount,
		analyticLights.truncatedLightCount,
		YesNo(analyticLights.lastBuildTopologyChanged),
		YesNo(analyticLights.lastBuildPropertiesChanged),
		(uint32_t)analyticLights.addedTopologyKeys.size(),
		(uint32_t)analyticLights.removedTopologyKeys.size(),
		(uint32_t)analyticLights.reboundTopologyKeys.size(),
		NRI_MAX_RUNTIME_POINT_LIGHTS);
	if (analyticLights.activeLights.empty())
	{
		return;
	}

	for (const SceneLightSystem::SceneAnalyticLight& light : analyticLights.activeLights)
	{
		const char* sourceBase =
			(light.sourceFlags & SceneAnalyticLightSourceFlag_Manual) != 0 ? "manual" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_MuzzleFlash) != 0 ? "transient" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_ActorOverlay) != 0 ? "overlay" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_MapOverlay) != 0 ? "overlay" :
			"heuristic";
		const char* sourceSuffix =
			(light.sourceFlags & SceneAnalyticLightSourceFlag_MuzzleFlash) != 0 ? ":muzzle" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_SpriteTileHeuristic) != 0 ? ":sprite_tile" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_ActorOverlay) != 0 ? ":actor" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_MapOverlay) != 0 ? ":map" :
			"";
		const auto diagnosticIt = analyticLights.activeDiagnosticFlags.find(light.stableKey);
		const uint32_t diagnosticFlags = diagnosticIt != analyticLights.activeDiagnosticFlags.end() ? diagnosticIt->second : SceneLightDiagnosticFlag_None;
		Printf("NRI PT analytic light %u: id=%u topology=0x%016llx prev_match=%s added=%s rebound=%s prop_changed=%s source=%s%s rule=%u actor=%d tile=%u render_pos=(%.3f, %.3f, %.3f) color=(%.3f, %.3f, %.3f) intensity=%.3f radius=%.3f\n",
			light.id,
			light.id,
			(unsigned long long)light.stableKey,
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_PreviousMatch) != 0),
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_Added) != 0),
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_Rebound) != 0),
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_PropertyChanged) != 0),
			sourceBase,
			sourceSuffix,
			light.sourceRuleId,
			light.actorIndex,
			light.textureId,
			light.position[0],
			light.position[1],
			light.position[2],
			light.color[0],
			light.color[1],
			light.color[2],
			light.intensity,
			light.radius);
	}
}

void NRIRenderer::PrintRuntimeLightClusterStatus() const
{
	const uint32_t tileCount = mBoundRuntimeLightTileCountX * mBoundRuntimeLightTileCountY;
	const uint32_t centerTileX = mBoundRuntimeLightTileCountX > 0 ? (mBoundRuntimeLightTileCountX - 1) / 2u : 0u;
	const uint32_t centerTileY = mBoundRuntimeLightTileCountY > 0 ? (mBoundRuntimeLightTileCountY - 1) / 2u : 0u;
	uint32_t centerTileCount = 0;
	if (mRuntimeLightTileHeaderBuffer.buffer != nullptr &&
		mBoundRuntimeLightTileCountX > 0 &&
		mBoundRuntimeLightTileCountY > 0)
	{
		void* mapped = mFrameBuffer->mCore.MapBuffer(*mRuntimeLightTileHeaderBuffer.buffer, 0, mRuntimeLightTileHeaderBuffer.usedSize);
		if (mapped != nullptr)
		{
			const auto* headers = reinterpret_cast<const RuntimeLightTileHeaderGpuData*>(mapped);
			const uint32_t centerIndex = centerTileY * mBoundRuntimeLightTileCountX + centerTileX;
			if ((uint64_t)(centerIndex + 1) * sizeof(RuntimeLightTileHeaderGpuData) <= mRuntimeLightTileHeaderBuffer.usedSize)
			{
				centerTileCount = headers[centerIndex].indexCount;
			}
			mFrameBuffer->mCore.UnmapBuffer(*mRuntimeLightTileHeaderBuffer.buffer);
		}
	}

	Printf("NRI PT light clusters: tile_size=%u grid=%ux%u tiles=%u active_lights=%u used_indices=%u max_occupancy=%u center_tile=(%u,%u) center_count=%u debug_mode=%u\n",
		mBoundRuntimeLightTileSize,
		mBoundRuntimeLightTileCountX,
		mBoundRuntimeLightTileCountY,
		tileCount,
		mBoundRuntimeLightCount,
		mBoundRuntimeLightTileIndexCount,
		mBoundRuntimeLightMaxTileOccupancy,
		centerTileX,
		centerTileY,
		centerTileCount,
		NRI_PTDEBUG_ANALYTIC_DIRECT);
}

uint32_t NRIRenderer::GetRuntimePointLightCount() const
{
	return mSceneLights.GetManualAnalyticLightCount();
}

bool NRIRenderer::AddRuntimeDebugSphere(const float center[3], float diameter, float metalness, float roughness, uint32_t& outId)
{
	if (center == nullptr || diameter <= 0.0f)
	{
		return false;
	}

	if (mRuntimeDebugSpheres.size() >= NriPtDebugSphereLimit)
	{
		return false;
	}

	RuntimeDebugSphere sphere = {};
	sphere.id = mNextRuntimeDebugSphereId++;
	nri_scene::Copy3(center, sphere.center);
	sphere.diameter = diameter;
	sphere.metalness = clamp(metalness, 0.0f, 1.0f);
	sphere.roughness = clamp(roughness, 0.0f, 1.0f);
	if (!EnsureRuntimeDebugSphereCache(sphere))
	{
		return false;
	}

	mRuntimeDebugSpheres.push_back(sphere);
	outId = sphere.id;
	RequestHistoryReset("runtime-debug-sphere-change");
	return true;
}

bool NRIRenderer::RemoveRuntimeDebugSphere(uint32_t id)
{
	const auto it = std::find_if(mRuntimeDebugSpheres.begin(), mRuntimeDebugSpheres.end(),
		[id](const RuntimeDebugSphere& sphere)
		{
			return sphere.id == id;
		});
	if (it == mRuntimeDebugSpheres.end())
	{
		return false;
	}

	mRuntimeDebugSpheres.erase(it);
	RequestHistoryReset("runtime-debug-sphere-change");
	return true;
}

void NRIRenderer::ClearRuntimeDebugSpheres()
{
	if (mRuntimeDebugSpheres.empty())
	{
		return;
	}

	mRuntimeDebugSpheres.clear();
	RequestHistoryReset("runtime-debug-sphere-change");
}

void NRIRenderer::PrintRuntimeDebugSpheres() const
{
	Printf("NRI PT debug spheres: active=%u limit=%u tessellation=%ux%u triangles_per_sphere=%u\n",
		(uint32_t)mRuntimeDebugSpheres.size(),
		NriPtDebugSphereLimit,
		GetRuntimeDebugSphereLongitudeSegments(),
		GetRuntimeDebugSphereLatitudeSegments(),
		GetRuntimeDebugSphereTriangleCount());
	for (const RuntimeDebugSphere& sphere : mRuntimeDebugSpheres)
	{
		float worldPosition[3] = {};
		PathTracingToWorldPosition(sphere.center, worldPosition);
		Printf("NRI PT debug sphere %u: id=%u render_pos=(%.3f, %.3f, %.3f) world_pos=(%.3f, %.3f, %.3f) diameter=%.3f metalness=%.3f roughness=%.3f\n",
			sphere.id,
			sphere.id,
			sphere.center[0],
			sphere.center[1],
			sphere.center[2],
			worldPosition[0],
			worldPosition[1],
			worldPosition[2],
			sphere.diameter,
			sphere.metalness,
			sphere.roughness);
	}
}

uint32_t NRIRenderer::GetRuntimeDebugSphereCount() const
{
	return (uint32_t)mRuntimeDebugSpheres.size();
}

bool NRIRenderer::AddSpriteTileLightHeuristic(uint32_t textureId, const float color[3], float intensity, float radius, uint32_t flickerFrames, uint32_t& outRuleId)
{
	if (!mSceneLights.AddSpriteTileHeuristic(textureId, color, intensity, radius, flickerFrames, outRuleId))
	{
		return false;
	}

	NoteLightHistoryChange("analytic-light-heuristic-change");
	return true;
}

void NRIRenderer::ClearSpriteTileLightHeuristics()
{
	if (mSceneLights.GetAnalyticLights().spriteTileRules.empty())
	{
		return;
	}

	mSceneLights.ClearSpriteTileHeuristics();
	NoteLightHistoryChange("analytic-light-heuristic-change");
}

void NRIRenderer::PrintSpriteTileLightHeuristics() const
{
	const auto& analyticLights = mSceneLights.GetAnalyticLights();
	Printf("NRI PT analytic sprite-tile heuristics: rules=%u matched_surfaces=%u deduped=%u truncated=%u\n",
		(uint32_t)analyticLights.spriteTileRules.size(),
		analyticLights.matchedSurfaceCount,
		analyticLights.dedupedMatchCount,
		analyticLights.truncatedLightCount);
	for (const auto& rule : analyticLights.spriteTileRules)
	{
		Printf("NRI PT analytic heuristic %u: tile=%u color=(%.3f, %.3f, %.3f) intensity=%.3f radius=%.3f flicker_frames=%u\n",
			rule.ruleId,
			rule.textureId,
			rule.color[0],
			rule.color[1],
			rule.color[2],
			rule.intensity,
			rule.radius,
			rule.flickerFrames);
	}
}

bool NRIRenderer::AddTextureEmissiveHeuristic(uint32_t textureId, uint32_t emissiveMode, float intensityScale, const float* emissiveColor, bool hasExplicitColor, uint32_t& outRuleId)
{
	if (!mSceneLights.AddTextureEmissiveHeuristic(textureId, emissiveMode, intensityScale, emissiveColor, hasExplicitColor, outRuleId))
	{
		return false;
	}

	QueueStaticMapSceneLightingInvalidation();
	mSceneLights.ConsumeEmissiveMaterialBindingChanged();
	mSceneLights.ConsumeEmissiveMaterialPropertiesChanged();
	NoteLightHistoryChange("emissive-heuristic-change");
	return true;
}

void NRIRenderer::ClearTextureEmissiveHeuristics()
{
	if (mSceneLights.GetEmissiveSurfaces().textureRules.empty())
	{
		return;
	}

	mSceneLights.ClearTextureEmissiveHeuristics();
	QueueStaticMapSceneLightingInvalidation();
	mSceneLights.ConsumeEmissiveMaterialBindingChanged();
	mSceneLights.ConsumeEmissiveMaterialPropertiesChanged();
	NoteLightHistoryChange("emissive-heuristic-change");
}

void NRIRenderer::NotifyGlowControlChange()
{
	QueueStaticMapSceneLightingInvalidation();
	ResetPersistentDynamicEmissiveCache();
	NoteLightHistoryChange("glow-control-change");
}

void NRIRenderer::NotifyMaterialLightingCalibrationChange()
{
	QueueStaticMapSceneLightingInvalidation();
	ResetPersistentDynamicEmissiveCache();
	NoteLightHistoryChange("material-lighting-calibration-change");
}

void NRIRenderer::NotifyDebugSphereTessellationChange()
{
	for (RuntimeDebugSphere& sphere : mRuntimeDebugSpheres)
	{
		sphere.cacheValid = false;
		sphere.cachedLongitudeSegments = 0;
		sphere.cachedLatitudeSegments = 0;
	}

	RequestHistoryReset("debug-sphere-tessellation-change");
}

void NRIRenderer::UpdateNightVisionState()
{
	mNightVisionState = {};

	if (gi == nullptr)
	{
		return;
	}

	RuntimeNightVisionState runtimeState = {};
	if (!gi->GetNightVisionState(&runtimeState) || !runtimeState.available)
	{
		return;
	}

	switch (runtimeState.mode)
	{
	case RuntimeNightVisionMode::Duke:
		mNightVisionState.mode = NRIPTNightVisionMode::Duke;
		break;
	default:
		mNightVisionState.mode = NRIPTNightVisionMode::None;
		break;
	}

	mNightVisionState.viewEligible = runtimeState.viewEligible;
	mNightVisionState.enabled = runtimeState.enabled;
	mNightVisionState.strength01 = runtimeState.strength01;
	mNightVisionState.remainingSeconds = runtimeState.remainingSeconds;
}

void NRIRenderer::PrintTextureEmissiveHeuristics() const
{
	const auto& emissive = mSceneLights.GetEmissiveSurfaces();
	Printf("NRI PT emissive heuristics: rules=%u auto_tagged=%u explicit_matches=%u active=%u total_power=%.3f glow_scale=%.3f glow_reach=%.3f glow_falloff=%.3f truncated=%u\n",
		(uint32_t)emissive.textureRules.size(),
		emissive.autoTaggedCount,
		emissive.explicitRuleMatchCount,
		(uint32_t)emissive.activeSurfaces.size(),
		emissive.totalPowerEstimate,
		(float)nri_ptglowscale,
		(float)nri_ptglowreach,
		(float)nri_ptglowfalloff,
		emissive.truncatedSurfaceCount);
	for (const auto& rule : emissive.textureRules)
	{
		Printf("NRI PT emissive heuristic %u: tile=%u mode=%s intensity_scale=%.3f explicit_color=%s color=(%.3f, %.3f, %.3f)\n",
			rule.ruleId,
			rule.textureId,
			GetMaterialEmissiveModeName(rule.emissiveMode),
			rule.intensityScale,
			rule.hasExplicitColor ? "yes" : "no",
			rule.emissiveColor[0],
			rule.emissiveColor[1],
			rule.emissiveColor[2]);
	}
}

void NRIRenderer::PrintEmissiveSurfaceDump(float radius, uint32_t limit) const
{
	if (mBoundEmissivePrimitiveRecords.empty())
	{
		Printf("NRI PT emissive primitives: no emissive primitive candidates are bound.\n");
		return;
	}

	struct Candidate
	{
		const EmissivePrimitiveDebugRecord* record = nullptr;
		float distanceSq = 0.0f;
	};

	std::vector<Candidate> candidates;
	candidates.reserve(mBoundEmissivePrimitiveRecords.size());
	const float radiusSq = radius > 0.0f ? radius * radius : -1.0f;
	for (const auto& record : mBoundEmissivePrimitiveRecords)
	{
		const float dx = record.center[0] - mCurrentCameraPos[0];
		const float dy = record.center[1] - mCurrentCameraPos[1];
		const float dz = record.center[2] - mCurrentCameraPos[2];
		const float distanceSq = dx * dx + dy * dy + dz * dz;
		if (radiusSq >= 0.0f && distanceSq > radiusSq)
		{
			continue;
		}
		candidates.push_back({ &record, distanceSq });
	}

	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
	{
		return a.distanceSq < b.distanceSq;
	});

	Printf("NRI PT emissive primitives: active=%u source_surfaces=%u auto=%u explicit=%u total_power=%.3f topo_changed=%s prop_changed=%s added=%u removed=%u rebound=%u min_surface=%.3f min_power=%.3f sampling_auto_only=%s\n",
		(uint32_t)mBoundEmissivePrimitiveRecords.size(),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().activeSurfaces.size(),
		mSceneLights.GetEmissiveSurfaces().autoTaggedCount,
		mSceneLights.GetEmissiveSurfaces().explicitRuleMatchCount,
		mBoundEmissiveTotalPower,
		YesNo(mSceneLights.GetEmissiveSurfaces().lastBuildTopologyChanged),
		YesNo(mSceneLights.GetEmissiveSurfaces().lastBuildPropertiesChanged),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().addedTopologyKeys.size(),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().removedTopologyKeys.size(),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().reboundTopologyKeys.size(),
		(float)nri_ptemissiveminsurface,
		(float)nri_ptemissiveminpower,
		nri_ptemissiveautoonly ? "on" : "off");

	const auto& emissiveSurfaces = mSceneLights.GetEmissiveSurfaces();
	const uint32_t printCount = std::min<uint32_t>((uint32_t)candidates.size(), limit);
	for (uint32_t i = 0; i < printCount; ++i)
	{
		const auto& record = *candidates[i].record;
		const auto diagnosticIt = emissiveSurfaces.activeDiagnosticFlags.find(record.surfaceStableKey);
		const uint32_t diagnosticFlags = diagnosticIt != emissiveSurfaces.activeDiagnosticFlags.end() ? diagnosticIt->second : SceneLightDiagnosticFlag_None;
		Printf("NRI PT emissive %u: primitive_key=0x%016llx surface_key=0x%016llx prev_match=%s added=%s rebound=%s prop_changed=%s source=%s primitive=%u material=%u flags=0x%x rule=%u actor=%d tile=%u mode=%s emissive_tex=%u area=%.2f power=%.3f sample_weight=%.3f pdf=%.6f center=(%.2f, %.2f, %.2f) color=(%.3f, %.3f, %.3f) intensity=%.3f\n",
			i,
			(unsigned long long)record.stableKey,
			(unsigned long long)record.surfaceStableKey,
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_PreviousMatch) != 0),
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_Added) != 0),
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_Rebound) != 0),
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_PropertyChanged) != 0),
			GetSceneDataSourceName(record.dataSource),
			record.primitiveIndex,
			record.materialIndex,
			record.sourceFlags,
			record.sourceRuleId,
			record.actorIndex,
			record.textureId,
			GetMaterialEmissiveModeName(record.emissiveMode),
			record.emissiveTextureIndex != UINT32_MAX ? record.emissiveTextureIndex : 0u,
			record.primitiveArea,
			record.powerEstimate,
			record.selectionWeight,
			record.selectionPdf,
			record.center[0],
			record.center[1],
			record.center[2],
			record.emissiveColor[0],
			record.emissiveColor[1],
			record.emissiveColor[2],
			record.emissiveIntensity);
	}
}

void NRIRenderer::PrintSectorLightDump(float radius, uint32_t limit) const
{
	const auto& registry = mSceneLights.GetSectorLighting();
	const float sectorLightMultiplier = GetSectorLightMultiplier();
	if (registry.activeSectorIndices.empty())
	{
		Printf("NRI PT sector lights: no active sector-light records are available.\n");
		return;
	}

	struct SectorCandidate
	{
		uint32_t sectorIndex = UINT32_MAX;
		float distanceSq = std::numeric_limits<float>::max();
		float center[3] = {};
	};

	std::vector<float> centerSums((size_t)registry.sectorCount * 3u, 0.0f);
	std::vector<uint32_t> centerCounts(registry.sectorCount, 0u);
	for (const auto& record : mSceneLights.GetSurfaceRecords())
	{
		if (record.provenance.sectorIndex < 0)
		{
			continue;
		}

		const uint32_t sectorIndex = (uint32_t)record.provenance.sectorIndex;
		if (sectorIndex >= registry.sectorCount)
		{
			continue;
		}

		centerSums[(size_t)sectorIndex * 3u + 0u] += record.center[0];
		centerSums[(size_t)sectorIndex * 3u + 1u] += record.center[1];
		centerSums[(size_t)sectorIndex * 3u + 2u] += record.center[2];
		centerCounts[sectorIndex]++;
	}

	std::vector<SectorCandidate> candidates;
	candidates.reserve(registry.activeSectorIndices.size());
	const float radiusSq = radius > 0.0f ? radius * radius : std::numeric_limits<float>::max();
	for (uint32_t sectorIndex : registry.activeSectorIndices)
	{
		if (sectorIndex >= registry.sectorCount || sectorIndex >= centerCounts.size() || centerCounts[sectorIndex] == 0u)
		{
			continue;
		}

		SectorCandidate candidate = {};
		candidate.sectorIndex = sectorIndex;
		const float invCount = 1.0f / (float)centerCounts[sectorIndex];
		candidate.center[0] = centerSums[(size_t)sectorIndex * 3u + 0u] * invCount;
		candidate.center[1] = centerSums[(size_t)sectorIndex * 3u + 1u] * invCount;
		candidate.center[2] = centerSums[(size_t)sectorIndex * 3u + 2u] * invCount;
		const float dx = candidate.center[0] - mCurrentCameraPos[0];
		const float dy = candidate.center[1] - mCurrentCameraPos[1];
		const float dz = candidate.center[2] - mCurrentCameraPos[2];
		candidate.distanceSq = dx * dx + dy * dy + dz * dz;
		if (candidate.distanceSq <= radiusSq)
		{
			candidates.push_back(candidate);
		}
	}

	std::sort(candidates.begin(), candidates.end(), [](const SectorCandidate& a, const SectorCandidate& b)
	{
		if (a.distanceSq != b.distanceSq)
		{
			return a.distanceSq < b.distanceSq;
		}
		return a.sectorIndex < b.sectorIndex;
	});

	Printf("NRI PT sector lights: active=%u eligible=%u fog=%u pulsing=%u radius=%.1f limit=%u multiplier=%.3f scales=(%.3f, %.3f, %.3f) clamp=%.3f filter=pal=%d shade=[%d,%d] lotag=%d pulse=%d/%.3f\n",
		registry.activeSectorCount,
		registry.eligibleSectorCount,
		registry.fogSectorCount,
		registry.pulsingSectorCount,
		radius,
		limit,
		sectorLightMultiplier,
		(float)nri_ptsectorambientscale,
		(float)nri_ptsectorhemiscale,
		(float)nri_ptsectorfogscale,
		(float)nri_ptsectorclamp,
		(int)nri_ptsectorfilterpal,
		(int)nri_ptsectorfilterminshade,
		(int)nri_ptsectorfiltermaxshade,
		(int)nri_ptsectorfilterlotag,
		(int)nri_ptsectorpulseframes,
		(float)nri_ptsectorpulseamount);

	const uint32_t printCount = std::min<uint32_t>((uint32_t)candidates.size(), limit);
	for (uint32_t i = 0; i < printCount; ++i)
	{
		const SectorCandidate& candidate = candidates[i];
		const auto& entry = registry.sectors[candidate.sectorIndex];
		Printf("NRI PT sector light %u: sector=%u dist=%.2f center=(%.2f, %.2f, %.2f) ambient=(%.3f, %.3f, %.3f)*%.3f hemi=%.3f fog=%.3f pulse=%.3f palette=%d shade=%d lotag=%d hitag=%d flags=0x%x\n",
			i,
			candidate.sectorIndex,
			std::sqrt(candidate.distanceSq),
			candidate.center[0],
			candidate.center[1],
			candidate.center[2],
			entry.ambientColor[0],
			entry.ambientColor[1],
			entry.ambientColor[2],
			entry.ambientIntensity * sectorLightMultiplier,
			entry.hemisphereAmount * sectorLightMultiplier,
			entry.fogAmount * sectorLightMultiplier,
			entry.pulseScale,
			entry.paletteIndex,
			entry.averageShade,
			entry.lotag,
			entry.hitag,
			entry.sourceFlags);
	}

	if (printCount == 0)
	{
		Printf("NRI PT sector lights: no active sector lights matched the requested radius.\n");
	}
}

void NRIRenderer::PrintStatus() const
{
	SyncLegacyUpscalerConfig(false);
	const NRIMainUpscalerKind requestedMain = GetSelectedMainUpscalerKind();
	const NRIMainUpscalerKind resolvedMain = GetResolvedMainUpscalerKindForStatus();
	const NRIPostSharpenKind requestedPost = GetSelectedPostSharpenKind();
	const NRIPostSharpenKind resolvedPost = GetResolvedPostSharpenKindForStatus();
	const nri::UpscalerMode requestedUpscalerMode = GetSelectedUpscalerMode();
	const nri::UpscalerMode resolvedUpscalerMode = ResolveUpscalerModeForMain(resolvedMain, requestedUpscalerMode);
	const bool runAppTaa = ShouldRunAppTaa(resolvedMain);
	const float requestedRenderScale = std::max(0.33f, std::min((float)nri_renderscale, 1.0f));
	const float resolvedRenderScale = ResolveRenderScaleForMain(resolvedMain, requestedUpscalerMode, requestedRenderScale);
	const uint32_t bootstrapMode = GetBootstrapMode();
	const uint32_t nrdMaxFrames = ClampNrdHistoryFrameCount((int)nri_nrdmaxframes);
	const uint32_t nrdFastFrames = ClampNrdFastFrameCount((int)nri_nrdfastframes, nrdMaxFrames);
	const uint32_t nrdStabilizationFrames = ClampNrdStabilizationFrameCount((int)nri_nrdstabilizationframes, nrdMaxFrames);
	const uint32_t nrdHitDistanceReconstruction = GetNrdHitDistanceReconstructionMode();
	const uint32_t nrdInputSplit = GetNrdInputSplitMode();
	const NRINrdDenoiserMode nrdDenoiserMode = GetSelectedNrdDenoiserMode();
	const float nrdFastHistorySigma = ClampNrdFastHistorySigmaScale((float)nri_nrdfasthistorysigma);
	const float nrdDiffusePrepass = ClampNrdPrepassBlurRadius((float)nri_nrdprepassdiffuse);
	const float nrdSpecularPrepass = ClampNrdPrepassBlurRadius((float)nri_nrdprepassspecular);
	const float nrdMinBlur = ClampNrdBlurRadius((float)nri_nrdblurmin);
	const float nrdMaxBlur = std::max(nrdMinBlur, ClampNrdBlurRadius((float)nri_nrdblurmax));
	const uint32_t sigmaStabilizationFrames = ClampSigmaStabilizationFrameCount((int)nri_nrdsigmastabilization);
	const float sigmaPlaneDistance = ClampSigmaPlaneDistanceSensitivity((float)nri_nrdsigmaplanedistance);
	const NRITextureResource& srInput = GetFrameTexture(FrameTextureSlot::SrInput);
	const NRITextureResource& rrInput = GetFrameTexture(FrameTextureSlot::RrInput);
	const NRITextureResource& upscalerDepth = GetFrameTexture(FrameTextureSlot::UpscalerDepth);
	const NRITextureResource& vendorOutput = GetFrameTexture(FrameTextureSlot::VendorOutput);
	const NRITextureResource& postSharpenOutput = GetFrameTexture(FrameTextureSlot::PostSharpenOutput);
	const NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	const auto& frameGenPolicy = mFrameBuffer->mFrameGeneration.GetPolicy();
	const auto& frameGenPresentContract = mFrameBuffer->mFrameGeneration.GetPresentContract();
	const NRIPTOutputPolicy outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	const NRIPresentRouteInfo presentRoute = ResolvePresentRouteInfo(GetEffectivePtDebugMode(), !!nri_ptbootstrap);
	const nri::Format expectedFinalFormat = ResolveFinalSceneFormat();
	const bool hasFrameGenDesc = mFrameBuffer->mFrameGeneration.HasFrameDesc();
	const auto& frameGenDesc = mFrameBuffer->mFrameGeneration.GetFrameDesc();
	const auto& frameGenAudit = mFrameBuffer->mFrameGeneration.GetInputAudit();
	const auto& frameGenProvider = mFrameBuffer->mFrameGeneration.GetProviderState();

	Printf("NRI PT status: support=%s", mPathTracingSupported ? "available" : "raster-fallback");
	if (!mPathTracingSupported)
	{
		Printf(" (%s)", GetAvailabilityReason());
	}
	Printf("\n");
	Printf("NRI PT frame: index=%u fg_frame_id=%llu render=%ux%u output=%ux%u prev_camera=%s reset_history=%s\n",
		mFrameIndex,
		(unsigned long long)mFrameGenerationFrameId,
		mRenderWidth,
		mRenderHeight,
		mOutputWidth,
		mOutputHeight,
		mHasPreviousCameraState ? "yes" : "no",
		mResetHistory ? "yes" : "no");
	Printf("NRI PT output: requested_mode=%s resolved_mode=%s control_block=%s tonemap=%s exposure=%.3f contrast=%.3f saturation=%.3f shoulder=%.3f toe=%.3f paper_white=%.1f offscreen_hdr=%s hdr_swapchain=%s display_info=%s display_hdr=%s display_sdr_nits=%.1f display_max_nits=%.1f\n",
		GetNRIPTOutputModeName(outputPolicy.requestedMode),
		GetNRIPTOutputModeName(outputPolicy.resolvedMode),
		GetNRIPTOutputControlBlockName(outputPolicy),
		GetNRIPTTonemapModeName(outputPolicy.tonemapMode),
		outputPolicy.exposure,
		outputPolicy.contrast,
		outputPolicy.saturation,
		outputPolicy.shoulder,
		outputPolicy.toe,
		outputPolicy.paperWhiteNits,
		outputPolicy.offscreenHdrTarget ? "yes" : "no",
		outputPolicy.hdrSwapChainActive ? "yes" : "no",
		outputPolicy.displayInfoAvailable ? "yes" : "no",
		outputPolicy.displayHdrSupported ? "yes" : "no",
		outputPolicy.displaySdrLuminance,
		outputPolicy.displayMaxLuminance);
	Printf("NRI PT nightvision: mode=%s view_eligible=%s active=%s presenter=%s strength=%.3f remaining_s=%.3f\n",
		GetNightVisionModeName(mNightVisionState.mode),
		YesNo(mNightVisionState.viewEligible),
		YesNo(mNightVisionState.enabled),
		nri_ptnightvision ? "on" : "off",
		mNightVisionState.strength01,
		mNightVisionState.remainingSeconds);
	Printf("NRI PT nightvision tuning: exposure=%.3f contrast=%.3f saturation=%.3f\n",
		(float)nri_ptnightvisionexposure,
		(float)nri_ptnightvisioncontrast,
		(float)nri_ptnightvisionsaturation);
	Printf("NRI PT nightvision tint: red=%.3f green=%.3f blue=%.3f\n",
		(float)nri_ptnightvisionred,
		(float)nri_ptnightvisiongreen,
		(float)nri_ptnightvisionblue);
	Printf("NRI PT material calibration: fullbright_boost=%.3f\n",
		(float)nri_ptfullbrightboost);
	if (outputPolicy.hdrSwapChainActive)
	{
		const float safeDisplaySdr = std::max(outputPolicy.displaySdrLuminance, 1.0f);
		const float safeDisplayMax = std::max(outputPolicy.displayMaxLuminance, safeDisplaySdr);
		const float safePaperWhite = std::clamp(std::max(outputPolicy.paperWhiteNits, safeDisplaySdr), safeDisplaySdr, safeDisplayMax);
		const float hdrPaperWhiteScale = safePaperWhite / 80.0f;
		const float hdrHeadroom = std::max(safeDisplayMax / safePaperWhite, 1.0f);
		const float hdrMaxScale = hdrPaperWhiteScale * hdrHeadroom;
		Printf("NRI PT output hdr: paper_scale=%.3f headroom=%.3f max_scale=%.3f active_linear16=%s\n",
			hdrPaperWhiteScale,
			hdrHeadroom,
			hdrMaxScale,
			outputPolicy.resolvedMode == NRIPTOutputMode::HDRLinear16 ? "yes" : "no");
	}
	Printf("NRI PT routing: debug=%u route=%s presenter=%s owner=%s root_bytes=scene:%u temporal:%u present:%u\n",
		GetEffectivePtDebugMode(),
		presentRoute.routeName,
		presentRoute.presenterName,
		presentRoute.ownerName,
		(unsigned)sizeof(NRITraceSceneConstants),
		(unsigned)sizeof(NRITemporalConstants),
		(unsigned)sizeof(NRIPresentConstants));
	Printf("NRI PT features: bootstrap=%s denoise=%s validation=%s api_validation=%s dred=%s main_upscaler=%s->%s post_sharpen=%s->%s requested_mode=%s resolved_mode=%s requested_render_scale=%.3f resolved_render_scale=%.3f sharpness=%.3f\n",
		nri_ptbootstrap ? "on" : "off",
		nri_denoise ? "on" : "off",
		nri_validation ? "on" : "off",
		nri_apivalidation ? "on" : "off",
		nri_dred ? "on" : "off",
		GetMainUpscalerName(requestedMain),
		GetMainUpscalerName(resolvedMain),
		GetPostSharpenName(requestedPost),
		GetPostSharpenName(resolvedPost),
		GetUpscalerModeName(requestedUpscalerMode),
		GetUpscalerModeName(resolvedUpscalerMode),
		requestedRenderScale,
		resolvedRenderScale,
		(float)nri_sharpness);
	Printf("NRI PT framegen policy: requested=%s provider=%s resolved=%s output=%s->%s contract=%s scope=%s api=%s shader_model=%u.%u window=%s low_latency=%s->%s(avail=%s iface=%s swapchain=%s) async=%s->%s(avail=%s) ui=%s->%s swapchain=%s native=device:%s queue:%s swapchain:%s waitable=%s runtime=%s frame_desc=%s reason=%s\n",
		frameGenPolicy.requestedEnabled ? "on" : "off",
		NRIFrameGenerationContext::GetProviderName(frameGenPolicy.requestedProvider),
		NRIFrameGenerationContext::GetProviderName(frameGenPolicy.resolvedProvider),
		GetNRIPTOutputModeName(frameGenPolicy.requestedOutputMode),
		GetNRIPTOutputModeName(frameGenPolicy.resolvedOutputMode),
		NRIFrameGenerationContext::GetOutputContractName(frameGenPolicy.resolvedOutputContract),
		frameGenPolicy.outputContractScope,
		frameGenPolicy.selectedApiName,
		frameGenPolicy.shaderModel / 10u,
		frameGenPolicy.shaderModel % 10u,
		NRIFrameGenerationContext::GetWindowModeName(frameGenPolicy.fullscreenActive),
		frameGenPolicy.requestedLowLatency ? "on" : "off",
		frameGenPolicy.resolvedLowLatency ? "on" : "off",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencyAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencyInterfaceAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencySwapChainEnabled),
		frameGenPolicy.requestedAsync ? "on" : "off",
		frameGenPolicy.resolvedAsync ? "on" : "off",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.asyncWorkloadAvailable),
		NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.requestedUiMode),
		NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.resolvedUiMode),
		frameGenPolicy.swapChainReady ? "ready" : "cold",
		frameGenPolicy.nativeDeviceAvailable ? "ok" : "missing",
		frameGenPolicy.nativeGraphicsQueueAvailable ? "ok" : "missing",
		frameGenPolicy.nativeSwapChainAvailable ? "ok" : "missing",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.waitableSwapChainAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.providerRuntimeSupported),
		hasFrameGenDesc ? "captured" : "empty",
		frameGenPolicy.resolvedReason);
	Printf("NRI PT framegen present contract: output=%s->%s proxy=%s hdr_swapchain=%s swapchain=%s texture=%s active=%s dxgi=%s active_dxgi=%s transfer=%s luminance=%.3f..%.3f hdr_scale=%.3f reason=%s\n",
		GetNRIPTOutputModeName(frameGenPresentContract.requestedOutputMode),
		GetNRIPTOutputModeName(frameGenPresentContract.resolvedOutputMode),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPresentContract.proxyAllowed),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPresentContract.usesHdrSwapChain),
		NRIFrameGenerationContext::GetSwapChainFormatName(frameGenPresentContract.createdSwapChainFormat),
		NRIFrameGenerationContext::GetNriFormatName(frameGenPresentContract.resolvedTextureFormat),
		NRIFrameGenerationContext::GetNriFormatName(frameGenPresentContract.activePresentTargetFormat),
		frameGenPresentContract.resolvedDxgiFormatValid ? NRIFrameGenerationContext::GetDxgiFormatName(frameGenPresentContract.resolvedDxgiFormat) : "unknown",
		frameGenPresentContract.activePresentTargetDxgiFormatValid ? NRIFrameGenerationContext::GetDxgiFormatName(frameGenPresentContract.activePresentTargetDxgiFormat) : "unknown",
		NRIFrameGenerationContext::GetPresentTransferFunctionName(frameGenPresentContract.transferFunction),
		frameGenPresentContract.minLuminance,
		frameGenPresentContract.maxLuminance,
		frameGenPresentContract.hdrPaperWhiteScale,
		frameGenPresentContract.resolvedReason);
	Printf("NRI PT final surface: expected=%s allocated=%s contract=%s active=%s size=%ux%u\n",
		NRIFrameGenerationContext::GetNriFormatName(expectedFinalFormat),
		NRIFrameGenerationContext::GetNriFormatName(final.format),
		NRIFrameGenerationContext::GetNriFormatName(frameGenPresentContract.resolvedTextureFormat),
		NRIFrameGenerationContext::GetNriFormatName(frameGenPresentContract.activePresentTargetFormat),
		final.width,
		final.height);
	Printf("NRI PT framegen provider: runtime=%s funcs=%s context=%s swapctx=%s bridge=%s debug=%s no_swapchain_notify=%s cfg=%s prepare=%s fg_dispatch=%s ui_reg=%s camera=%s lib=%s version=%s dims=render:%ux%u display:%ux%u counts=cfg:%llu prep:%llu fg:%llu frames=%llu/%llu query=%s/%s create=%s/%s config=%s/%s prepare=%s dispatch=%s vram=fg:%s:%llu/%llu sc:%s:%llu/%llu resets=%llu last_reset=%s present=%s/%s count=%llu reason=%s\n",
		frameGenProvider.runtimeLoaded ? "yes" : "no",
		frameGenProvider.runtimeFunctionsLoaded ? "yes" : "no",
		frameGenProvider.contextCreated ? "yes" : "no",
		frameGenProvider.swapChainContextCreated ? "yes" : "no",
		frameGenProvider.presentBridgeReady ? "yes" : "no",
		frameGenProvider.debugConfigured ? "yes" : "no",
		frameGenProvider.noSwapChainNotify ? "yes" : "no",
		frameGenProvider.configuredThisFrame ? "yes" : "no",
		frameGenProvider.prepareDispatchedThisFrame ? "yes" : "no",
		frameGenProvider.frameGenerationDispatchedThisFrame ? "yes" : "no",
		frameGenProvider.uiResourceRegisteredThisFrame ? "yes" : "no",
		frameGenProvider.prepareCameraInfoProvided ? "yes" : "no",
		frameGenProvider.runtimeLibrary,
		frameGenProvider.providerVersion,
		frameGenProvider.contextRenderWidth,
		frameGenProvider.contextRenderHeight,
		frameGenProvider.contextDisplayWidth,
		frameGenProvider.contextDisplayHeight,
		(unsigned long long)frameGenProvider.configureCount,
		(unsigned long long)frameGenProvider.prepareCount,
		(unsigned long long)frameGenProvider.dispatchCount,
		(unsigned long long)frameGenProvider.lastConfiguredFrameId,
		(unsigned long long)frameGenProvider.lastPreparedFrameId,
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastQueryResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastSwapChainQueryResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastCreateResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastSwapChainCreateResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastConfigureResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastSwapChainConfigureResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastPrepareResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastDispatchResult),
		frameGenProvider.memoryUsageValid ? "yes" : "no",
		(unsigned long long)frameGenProvider.totalUsageBytes,
		(unsigned long long)frameGenProvider.aliasableUsageBytes,
		frameGenProvider.swapChainMemoryUsageValid ? "yes" : "no",
		(unsigned long long)frameGenProvider.swapChainTotalUsageBytes,
		(unsigned long long)frameGenProvider.swapChainAliasableUsageBytes,
		(unsigned long long)frameGenProvider.resetCount,
		frameGenProvider.lastResetReason,
		frameGenProvider.lastPresentMode,
		NRIFrameGenerationContext::GetPresentResultName(frameGenProvider.lastPresentResult),
		(unsigned long long)frameGenProvider.presentCount,
		frameGenProvider.lastStatusReason);
	Printf("NRI PT framegen present: current=%s bridge_active=%s generated=%s fallback_pending=%s last=%s result=%s\n",
		frameGenProvider.frameGenerationDispatchedThisFrame ? "generated" :
			(frameGenProvider.presentUsedBridgeThisFrame ? "passthrough" : "native"),
		frameGenProvider.presentBridgeReady ? "yes" : "no",
		frameGenProvider.frameGenerationDispatchedThisFrame ? "yes" : "no",
		frameGenProvider.nativeFallbackRequested ? "yes" : "no",
		frameGenProvider.lastPresentMode,
		NRIFrameGenerationContext::GetPresentResultName(frameGenProvider.lastPresentResult));
	if (hasFrameGenDesc)
	{
		Printf("NRI PT framegen inputs: frame_id=%llu hudless=%s:%ux%u ui=%ux%u motion=%ux%u depth=%ux%u render_rect=%u,%u+%ux%u output_rect=%u,%u+%ux%u reset=%s prev_camera=%s frame_time=%s frame_time_ms=%.3f\n",
			(unsigned long long)frameGenDesc.frameId,
			NRIFrameGenerationContext::GetColorSourceName(frameGenDesc.hudlessColorSource),
			frameGenDesc.hudlessColor != nullptr ? frameGenDesc.hudlessColor->width : 0u,
			frameGenDesc.hudlessColor != nullptr ? frameGenDesc.hudlessColor->height : 0u,
			frameGenDesc.uiTexture != nullptr ? frameGenDesc.uiTexture->width : 0u,
			frameGenDesc.uiTexture != nullptr ? frameGenDesc.uiTexture->height : 0u,
			frameGenDesc.motionVectors != nullptr ? frameGenDesc.motionVectors->width : 0u,
			frameGenDesc.motionVectors != nullptr ? frameGenDesc.motionVectors->height : 0u,
			frameGenDesc.depth != nullptr ? frameGenDesc.depth->width : 0u,
			frameGenDesc.depth != nullptr ? frameGenDesc.depth->height : 0u,
			frameGenDesc.renderRect.left,
			frameGenDesc.renderRect.top,
			frameGenDesc.renderRect.width,
			frameGenDesc.renderRect.height,
			frameGenDesc.outputRect.left,
			frameGenDesc.outputRect.top,
			frameGenDesc.outputRect.width,
			frameGenDesc.outputRect.height,
			frameGenDesc.resetReason[0] != '\0' ? frameGenDesc.resetReason : "none",
			frameGenDesc.hasPreviousCamera ? "yes" : "no",
			frameGenDesc.hasRealFrameTimeMs ? "captured" : "pending",
			frameGenDesc.realFrameTimeMs);
		Printf("NRI PT framegen contract: motion=%s/%s scale=%.3f,%.3f depth=%s inverted=%s infinite=%s jitter=current(%.3f,%.3f) prev(%.3f,%.3f) fsr3=motion:%s depth:%s prepare:%s adapter:%s reason=%s\n",
			NRIFrameGenerationContext::GetMotionVectorSpaceName(frameGenDesc.motionVectorSpace),
			NRIFrameGenerationContext::GetMotionVectorDirectionName(frameGenDesc.motionVectorDirection),
			frameGenDesc.motionVectorScale[0],
			frameGenDesc.motionVectorScale[1],
			NRIFrameGenerationContext::GetDepthTypeName(frameGenDesc.depthType),
			frameGenDesc.depthInverted ? "yes" : "no",
			frameGenDesc.depthInfinite ? "yes" : "no",
			frameGenDesc.cameraJitter[0],
			frameGenDesc.cameraJitter[1],
			frameGenDesc.previousCameraJitter[0],
			frameGenDesc.previousCameraJitter[1],
			frameGenAudit.fsr3MotionCompatible ? "yes" : "no",
			frameGenAudit.fsr3DepthCompatible ? "yes" : "no",
			frameGenAudit.fsr3PrepareInputsRequired ? "yes" : "no",
			NRIFrameGenerationContext::GetAdapterRequirementName(frameGenAudit.adapterRequirement),
			frameGenAudit.statusReason);
	}
	Printf("NRI PT resolution policy: policy=%s render=%ux%u output=%ux%u jitter=%s phases=%u\n",
		GetRenderResolutionPolicyName(resolvedMain),
		mRenderWidth,
		mRenderHeight,
		mOutputWidth,
		mOutputHeight,
		GetTemporalJitterModeName(resolvedMain, mGuiCaptureActive),
		GetTemporalJitterPhaseCount(resolvedMain, resolvedUpscalerMode, mGuiCaptureActive));
	Printf("NRI PT output shell: family=%s sr_input=%ux%u rr_input=%ux%u guides=%ux%u vendor=%ux%u post_output=%ux%u post=%s active=%s last_reset_reason=%s\n",
		GetUpscalerFamilyName(resolvedMain, runAppTaa),
		srInput.width,
		srInput.height,
		rrInput.width,
		rrInput.height,
		upscalerDepth.width,
		upscalerDepth.height,
		vendorOutput.width,
		vendorOutput.height,
		postSharpenOutput.width,
		postSharpenOutput.height,
		GetPostSharpenName(resolvedPost),
		resolvedPost == NRIPostSharpenKind::Off ? "pre-post" : "post-sharpen-output",
		mLastHistoryResetReason.c_str());
	Printf("NRI PT tracing: direct_scene_fallback=%s light_bounces=%u mirror_bounces=%u portal_depth=%u surface_probe=%d\n",
		nri_ptdirectscene ? "on" : "off",
		ClampTraceBounceCount((int)nri_ptlightbounces, 4u),
		ClampTraceBounceCount((int)nri_ptmirrorbounces, 8u),
		ClampTraceBounceCount((int)nri_ptportaldepth, 8u),
		(int)nri_ptsurfaceprobe);
	Printf("NRI PT lighting shell: directional=%s sector=%s emissive_heuristics=%s\n",
		mDirectionalLightState.enabled ? "on" : "off",
		nri_ptsectorlighting ? "on" : "off",
		nri_ptemissiveheuristics ? "on" : "off");
	Printf("NRI PT directional light: source=%s shadow=%s rule=%u dir=(%.3f, %.3f, %.3f) color=(%.3f, %.3f, %.3f) angular=%.3f\n",
		GetDirectionalLightSourceName(mDirectionalLightState),
		mDirectionalLightState.enabled && mDirectionalLightState.shadow ? "on" : "off",
		mDirectionalLightState.ruleId,
		mDirectionalLightState.direction[0],
		mDirectionalLightState.direction[1],
		mDirectionalLightState.direction[2],
		mDirectionalLightState.color[0],
		mDirectionalLightState.color[1],
		mDirectionalLightState.color[2],
		mDirectionalLightState.angularSize);
	Printf("NRI PT transparent shell: trace_transparent=placeholder_noop\n");
	uint32_t emissiveBaseCount = 0;
	uint32_t emissiveConstantCount = 0;
	uint32_t emissiveGlowmapCount = 0;
	for (const auto& surface : mSceneLights.GetEmissiveSurfaces().activeSurfaces)
	{
		switch (surface.emissiveMode)
		{
		case nri_scene::MaterialEmissiveMode_UseBaseTexture: emissiveBaseCount++; break;
		case nri_scene::MaterialEmissiveMode_UseConstantColor: emissiveConstantCount++; break;
		case nri_scene::MaterialEmissiveMode_UseGlowmapTexture: emissiveGlowmapCount++; break;
		default: break;
		}
	}
	Printf("NRI PT NRD: integration=%s requested=%s validation_output=%s denoiser=%s motion=%s prev_position=%s extra_debugs=%s\n",
		mNrd.IsReady() ? "ready" : "cold",
		nri_denoise ? "on" : "off",
		nri_validation ? "expected" : "disabled",
		GetNrdDenoiserModeName(nrdDenoiserMode),
		"2.5D",
		"interpolated",
		"16=denoised_diff 17=denoised_spec 18=metalness 19=roughness 20=motion_z 21=live_raw_penumbra 22=live_raw_shadow 23=temporal_sigma_shadow 24=direct_lighting 25=direct_emission 26=analytic_direct 27=emissive_tags 28=emissive_direct 29=sector_ambient 30=emissive_uv 31=emissive_radiance 32=emissive_primitive 33=emissive_visibility 34=trace_transparent 35=sr_input 36=sr_depth 37=vendor_output 38=vendor_output_final 39=rr_input 40=rr_diffuse_albedo 41=rr_specular_albedo 42=rr_normal_roughness 43=rr_specular_hit_distance 44=post_sharpen_output 45=taa_pre_exposed_input");
	const char* shadowSplitMode =
		!mUseSplitShadowDenoiser ? "off" :
		(GetEffectivePtDebugMode() >= 21 && GetEffectivePtDebugMode() <= 23) ? "sigma-debug" :
		"sigma-beauty";
	Printf("NRI PT NRD settings: max_frames=%u fast_frames=%u stabilization_frames=%u anti_firefly=%s hit_recon=%s input_split=%s shadow_split=%s\n",
		nrdMaxFrames,
		nrdFastFrames,
		nrdStabilizationFrames,
		nri_nrdantifirefly ? "on" : "off",
		GetNrdHitDistanceReconstructionModeName(nrdHitDistanceReconstruction),
		GetNrdInputSplitModeName(nrdInputSplit),
		shadowSplitMode);
	Printf("NRI PT SIGMA tuning: stabilization_frames=%u plane_distance_sensitivity=%.3f\n",
		sigmaStabilizationFrames,
		sigmaPlaneDistance);
	if (nrdDenoiserMode == NRINrdDenoiserMode::Relax)
	{
		Printf("NRI PT NRD tuning: fast_history_sigma=%.2f prepass=%.2f/%.2f material_floor=1/2 blur_radius=n/a_relax\n",
			nrdFastHistorySigma,
			nrdDiffusePrepass,
			nrdSpecularPrepass);
	}
	else
	{
		Printf("NRI PT NRD tuning: fast_history_sigma=%.2f blur_radius=%.2f..%.2f prepass=%.2f/%.2f material_floor=1/2\n",
			nrdFastHistorySigma,
			nrdMinBlur,
			nrdMaxBlur,
			nrdDiffusePrepass,
			nrdSpecularPrepass);
	}
	Printf("NRI PT NRD guides: diffuse_signal=primary_demodulated_radiance specular_signal=primary_demodulated_radiance hit_distance=%s roughness=material_hint metalness=material_hint material_id=semantic_class\n",
		nrdDenoiserMode == NRINrdDenoiserMode::Relax ? "secondary_transport_linear_hitdist" : "secondary_transport_reblur_norm");
	Printf("NRI PT scene stats: %s\n", nri_ptscenestats ? "on" : "off");
	Printf("NRI PT mutation trace: chunk=%d sector=%d\n",
		(int)nri_ptmutationtracechunk,
		(int)nri_ptmutationtracesector);
	Printf("NRI PT runtime link trace: %s\n", nri_ptruntimelinktrace ? "on" : "off");
	Printf("NRI PT runtime mutation rebaseline trace: %s cache_per_frame=%d blas_per_frame=%d\n",
		nri_ptrebaselinetrace ? "on" : "off",
		(int)nri_ptrebaselinecachechunksperframe,
		(int)nri_ptrebaselineblasperframe);
	Printf("NRI PT analytic lights: active=%u manual=%u muzzle_slots=%u muzzle_active=%u rules=%u topo_changed=%s prop_changed=%s added=%u removed=%u rebound=%u limit=%u\n",
		(uint32_t)mSceneLights.GetAnalyticLights().activeLights.size(),
		(uint32_t)mSceneLights.GetAnalyticLights().manualLights.size(),
		mSceneLights.GetAnalyticLights().transientMuzzleSlotCount,
		mSceneLights.GetAnalyticLights().transientMuzzleActiveCount,
		(uint32_t)mSceneLights.GetAnalyticLights().spriteTileRules.size(),
		YesNo(mSceneLights.GetAnalyticLights().lastBuildTopologyChanged),
		YesNo(mSceneLights.GetAnalyticLights().lastBuildPropertiesChanged),
		(uint32_t)mSceneLights.GetAnalyticLights().addedTopologyKeys.size(),
		(uint32_t)mSceneLights.GetAnalyticLights().removedTopologyKeys.size(),
		(uint32_t)mSceneLights.GetAnalyticLights().reboundTopologyKeys.size(),
		NRI_MAX_RUNTIME_POINT_LIGHTS);
	Printf("NRI PT analytic clusters: tile=%u grid=%ux%u used_indices=%u max_occupancy=%u debug_mode=%u\n",
		mBoundRuntimeLightTileSize,
		mBoundRuntimeLightTileCountX,
		mBoundRuntimeLightTileCountY,
		mBoundRuntimeLightTileIndexCount,
		mBoundRuntimeLightMaxTileOccupancy,
		NRI_PTDEBUG_ANALYTIC_DIRECT);
	Printf("NRI PT emissive surfaces: active=%u rules=%u auto=%u explicit=%u total_power=%.3f topo_changed=%s prop_changed=%s added=%u removed=%u rebound=%u debug_mode=%u/%u thresholds=area>=%.3f power>=%.3f heuristics=%s sampling_auto_only=%s glow_scale=%.3f glow_reach=%.3f glow_falloff=%.3f\n",
		(uint32_t)mSceneLights.GetEmissiveSurfaces().activeSurfaces.size(),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().textureRules.size(),
		mSceneLights.GetEmissiveSurfaces().autoTaggedCount,
		mSceneLights.GetEmissiveSurfaces().explicitRuleMatchCount,
		mSceneLights.GetEmissiveSurfaces().totalPowerEstimate,
		YesNo(mSceneLights.GetEmissiveSurfaces().lastBuildTopologyChanged),
		YesNo(mSceneLights.GetEmissiveSurfaces().lastBuildPropertiesChanged),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().addedTopologyKeys.size(),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().removedTopologyKeys.size(),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().reboundTopologyKeys.size(),
		NRI_PTDEBUG_EMISSIVE_TAGS,
		NRI_PTDEBUG_EMISSIVE_DIRECT,
		(float)nri_ptemissiveminsurface,
		(float)nri_ptemissiveminpower,
		nri_ptemissiveheuristics ? "on" : "off",
		nri_ptemissiveautoonly ? "on" : "off",
		(float)nri_ptglowscale,
		(float)nri_ptglowreach,
		(float)nri_ptglowfalloff);
	const auto& appendStats = mSceneLights.GetFrameAppendStats();
	Printf("NRI PT scene-light ingest: records=%u static=%u mutation=%u captured=%u dynamic=%u append_ms=static:%.3f mutation:%.3f captured:%.3f dynamic:%.3f rebuild_ms=analytic:%.3f emissive:%.3f sector:%.3f\n",
		appendStats.totalRecordCount,
		appendStats.staticRecordCount,
		appendStats.runtimeMutationRecordCount,
		appendStats.capturedRecordCount,
		appendStats.dynamicRecordCount,
		mLastPerfShellTraceStats.sceneLightStaticAppendMs,
		mLastPerfShellTraceStats.sceneLightRuntimeMutationAppendMs,
		mLastPerfShellTraceStats.sceneLightCapturedAppendMs,
		mLastPerfShellTraceStats.sceneLightDynamicAppendMs,
		mLastPerfShellTraceStats.sceneLightAnalyticMs,
		mLastPerfShellTraceStats.sceneLightEmissiveMs,
		mLastPerfShellTraceStats.sceneLightSectorMs);
	Printf("NRI PT emissive sources: base=%u glowmap=%u constant=%u\n",
		emissiveBaseCount,
		emissiveGlowmapCount,
		emissiveConstantCount);
	Printf("NRI PT emissive sampling: primitives=%u total_power=%.3f samples=%u dominant_tile=%u dominant_primitive=%u dominant_source=%s dominant_power=%.3f dominant_flags=0x%x debug_mode=%u\n",
		mBoundEmissivePrimitiveCount,
		mBoundEmissiveTotalPower,
		std::max<uint32_t>(ClampTraceBounceCount((int)nri_ptemissivesamples, 4u), 1u),
		mBoundEmissiveDominantTile,
		mBoundEmissiveDominantPrimitive,
		GetSceneDataSourceName(mBoundEmissiveDominantDataSource),
		mBoundEmissiveDominantPower,
		mBoundEmissiveDominantFlags,
		NRI_PTDEBUG_EMISSIVE_SAMPLE_VISIBILITY);
	Printf("NRI PT emissive query: tlas=%s fast_shadow=%s instances=%u static=%u dynamic=%u builds=%u\n",
		nri_ptemissivetlas ? "on" : "off",
		nri_ptemissivefastshadow ? "on" : "off",
		mEmissiveTlasInstanceCount,
		mEmissiveTlasStaticInstanceCount,
		mEmissiveTlasDynamicInstanceCount,
		mEmissiveTlasBuildCount);
	Printf("NRI PT sector lighting: enabled=%s active=%u eligible=%u fog=%u pulsing=%u debug_mode=%u multiplier=%.3f scales=ambient=%.3f hemi=%.3f fog=%.3f clamp=%.3f filter=pal=%d shade=[%d,%d] lotag=%d pulse=%d/%.3f\n",
		nri_ptsectorlighting ? "on" : "off",
		mSceneLights.GetSectorLighting().activeSectorCount,
		mSceneLights.GetSectorLighting().eligibleSectorCount,
		mSceneLights.GetSectorLighting().fogSectorCount,
		mSceneLights.GetSectorLighting().pulsingSectorCount,
		NRI_PTDEBUG_SECTOR_AMBIENT,
		GetSectorLightMultiplier(),
		(float)nri_ptsectorambientscale,
		(float)nri_ptsectorhemiscale,
		(float)nri_ptsectorfogscale,
		(float)nri_ptsectorclamp,
		(int)nri_ptsectorfilterpal,
		(int)nri_ptsectorfilterminshade,
		(int)nri_ptsectorfiltermaxshade,
		(int)nri_ptsectorfilterlotag,
		(int)nri_ptsectorpulseframes,
		(float)nri_ptsectorpulseamount);
	Printf("NRI PT sector buffer: sectors=%u active=%u pulsing=%u dominant_sector=%u dominant_contribution=%.3f\n",
		mBoundSectorLightSectorCount,
		mBoundSectorLightActiveCount,
		mBoundSectorLightPulsingCount,
		mBoundSectorLightDominantSector != UINT32_MAX ? mBoundSectorLightDominantSector : 0u,
		mBoundSectorLightDominantContribution);
	if (nri_ptbootstrap)
	{
		Printf("NRI PT bootstrap mode: %u\n", bootstrapMode);
	}

	if (mHasLoggedStats)
	{
		const auto& stats = mLastStats;
		Printf("NRI PT last scene: walls=%u flats=%u sprites=%u translucent=%u models=%u voxel_proxies=%u unsupported_models=%u mirrors=%u skies=%u portal_views=%u portal_skips=%u approx_tris=%u materials=%u\n",
			stats.wallDrawItems,
			stats.flatDrawItems,
			stats.spriteDrawItems,
			stats.translucentDrawItems,
			stats.modelDrawItems,
			stats.voxelProxyDrawItems,
			stats.unsupportedModelDrawItems,
			stats.mirrorSurfaces,
			stats.skySurfaces,
			stats.portalViews,
			stats.portalCapturesSkipped,
			stats.triangleEstimate,
			stats.materialRefs);
	}
	else
	{
		Printf("NRI PT last scene: no translated PT scene has been captured yet.\n");
	}

	PrintMapWorldStatus();
	PrintPortalTraversalStatus();
	PrintStaticMapSceneStatus();
	PrintResidentMapChunkRegistryStatus();
	PrintDynamicSceneStatus();
	PrintTemporalStatus();
	PrintRuntimeMapMutationStatus();
	PrintRuntimeSpaceLinkStatus();
	PrintSceneBufferStatus();
	PrintSurfaceProbeStatus();
}

NRIRenderer::MemoryTelemetry NRIRenderer::GetMemoryTelemetry() const
{
	MemoryTelemetry telemetry = {};
	telemetry.renderWidth = mRenderWidth;
	telemetry.renderHeight = mRenderHeight;
	telemetry.outputWidth = mOutputWidth;
	telemetry.outputHeight = mOutputHeight;

	const auto accumulateTexture = [](const NRITextureResource& resource, uint64_t& total)
	{
		total += resource.memorySize;
	};
	const auto accumulateBuffer = [](const NRIBufferResource& resource, uint64_t& total)
	{
		total += resource.memorySize;
	};
	const auto accumulateAs = [](const NRIAccelerationStructureResource& resource, uint64_t& total)
	{
		total += resource.memorySize;
	};

	for (const NRITextureResource& texture : mFrameTextures)
	{
		accumulateTexture(texture, telemetry.frameTextureBytes);
	}

	accumulateTexture(mPaletteTexture, telemetry.sceneTextureBytes);
	for (const CachedTexture& texture : mTextureCache)
	{
		accumulateTexture(texture.resource, telemetry.sceneTextureBytes);
	}

	for (const CachedSkyTexture& texture : mSkyTextureCache)
	{
		accumulateTexture(texture.resource, telemetry.skyTextureBytes);
	}

	accumulateBuffer(mVertexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mIndexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mPrimitiveBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mMaterialBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mStaticVertexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mStaticIndexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mStaticPrimitiveBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mStaticMaterialBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mTlasInstanceBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mSceneInstanceBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mPortalBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mRuntimeLightBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mRuntimeLightTileHeaderBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mRuntimeLightTileIndexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissivePrimitiveHeaderBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissivePrimitiveBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissivePrimitiveCdfBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissiveTlasInstanceBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mSectorLightHeaderBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mSectorLightBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mReprojectionBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mVisibleChunkBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mVisibleFlatPlaneBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mScratchBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mTopLevelScratchBuffer, telemetry.sceneBufferBytes);

	accumulateAs(mDynamicBottomLevelAS, telemetry.accelerationStructureBytes);
	accumulateAs(mTopLevelAS, telemetry.accelerationStructureBytes);
	accumulateAs(mEmissiveTopLevelAS, telemetry.accelerationStructureBytes);
	for (const auto& chunk : mStaticMapScene.chunks)
	{
		accumulateAs(chunk.accelerationStructure, telemetry.accelerationStructureBytes);
	}

	telemetry.totalTrackedBytes =
		telemetry.frameTextureBytes +
		telemetry.sceneTextureBytes +
		telemetry.skyTextureBytes +
		telemetry.sceneBufferBytes +
		telemetry.accelerationStructureBytes;
	return telemetry;
}

const char* NRIRenderer::GetMaterialBuildTraceSlotName(MaterialBuildTraceSlot slot)
{
	return GetMaterialBuildTraceSlotNameInternal(slot);
}

const char* NRIRenderer::GetRuntimeMutationRebaselineStateName(RuntimeMutationRebaselineState state)
{
	return GetRuntimeMutationRebaselineStateNameInternal(state);
}

NRIRenderer::MaterialBuildTraceSlot NRIRenderer::ResolveMaterialBuildTraceSlot(const char* traceLabel)
{
	if (traceLabel == nullptr || traceLabel[0] == '\0')
	{
		return MaterialBuildTraceSlot::Unknown;
	}

	for (size_t index = 0; index < GetMaterialBuildTraceSlotIndex(MaterialBuildTraceSlot::Count); ++index)
	{
		const MaterialBuildTraceSlot slot = (MaterialBuildTraceSlot)index;
		if (std::strcmp(traceLabel, GetMaterialBuildTraceSlotNameInternal(slot)) == 0)
		{
			return slot;
		}
	}

	return MaterialBuildTraceSlot::Unknown;
}

const std::unordered_map<int32_t, uint32_t>& NRIRenderer::GetActorMaterialOverrideMapForFrame(MaterialBuildTraceSlot traceSlot)
{
	const ResolvedLightOverlaySet& resolvedLightOverlays = GetResolvedLightOverlaySet();
	const bool hasActorRules =
		resolvedLightOverlays.actorRules.Size() > 0 ||
		resolvedLightOverlays.actorOverrideRules.Size() > 0;
	const bool hasFullbrightOverrides = HasActorFullbrightOverrides(resolvedLightOverlays);
	if (mActorMaterialOverrideCache.valid &&
		mActorMaterialOverrideCache.frameIndex == mFrameIndex &&
		mActorMaterialOverrideCache.resolvedGeneration == resolvedLightOverlays.resolvedGeneration &&
		mActorMaterialOverrideCache.hasFullbrightOverrides == hasFullbrightOverrides)
	{
		return mActorMaterialOverrideCache.overrides;
	}

	mActorMaterialOverrideCache.valid = true;
	mActorMaterialOverrideCache.frameIndex = mFrameIndex;
	mActorMaterialOverrideCache.resolvedGeneration = resolvedLightOverlays.resolvedGeneration;
	mActorMaterialOverrideCache.hasFullbrightOverrides = hasFullbrightOverrides;
	mActorMaterialOverrideCache.overrides.clear();
	if (!hasActorRules)
	{
		return mActorMaterialOverrideCache.overrides;
	}

	mLastPerfShellTraceStats.actorOverrideMapBuildCalls++;
	auto& materialTraceEntry = mLastPerfShellTraceStats.materialBuildByLabel[GetMaterialBuildTraceSlotIndex(traceSlot)];
	materialTraceEntry.overrideBuildCalls++;
	if (ShouldTracePtPerf())
	{
		const auto start = std::chrono::steady_clock::now();
		BuildActorMaterialOverrideMap(resolvedLightOverlays, mActorMaterialOverrideCache.overrides);
		const double elapsedMs = DurationMs(start, std::chrono::steady_clock::now());
		mLastPerfShellTraceStats.actorOverrideMapBuildMs += elapsedMs;
		materialTraceEntry.overrideBuildMs += elapsedMs;
	}
	else
	{
		BuildActorMaterialOverrideMap(resolvedLightOverlays, mActorMaterialOverrideCache.overrides);
	}

	return mActorMaterialOverrideCache.overrides;
}

void NRIRenderer::PrintTemporalStatus() const
{
	SyncLegacyUpscalerConfig(false);
	const NRIPTOutputPolicy outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	const NRIMainUpscalerKind requestedMain = GetSelectedMainUpscalerKind();
	const NRIMainUpscalerKind resolvedMain = GetResolvedMainUpscalerKindForStatus();
	const NRIPostSharpenKind requestedPost = GetSelectedPostSharpenKind();
	const NRIPostSharpenKind resolvedPost = GetResolvedPostSharpenKindForStatus();
	const float exposure = GetTemporalExposure(outputPolicy);
	const float exposureStops = std::log2(std::max(exposure, 0.125f));
	const FrameTextureSlot presentSlot = mUseUpscaledInFinal ? mUpscaledInputSlot : mHistoryOutputSlot;
	const NRITextureResource& historyInput = GetFrameTexture(mHistoryInputSlot);
	const NRITextureResource& historyOutput = GetFrameTexture(mHistoryOutputSlot);
	Printf("NRI PT temporal: debug=%d requested_main=%s resolved_main=%s requested_post=%s resolved_post=%s taa=%s gui_capture=%s last_debug=%d last_main=%s last_post=%s reset=%s prev_camera=%s history_in=%s[%ux%u a=%u l=%u s=0x%x] history_out=%s[%ux%u a=%u l=%u s=0x%x] present=%s upscaled=%s use_upscaled=%s\n",
		(int)nri_ptdebug,
		GetMainUpscalerName(requestedMain),
		GetMainUpscalerName(resolvedMain),
		GetPostSharpenName(requestedPost),
		GetPostSharpenName(resolvedPost),
		nri_pttaa ? "on" : "off",
		mGuiCaptureActive ? "yes" : "no",
		mLastDebugMode,
		GetMainUpscalerName(mLastTemporalHistoryMainUpscaler),
		GetPostSharpenName(mLastTemporalPostSharpen),
		mResetHistory ? "yes" : "no",
		mHasPreviousCameraState ? "yes" : "no",
		GetFrameTextureSlotName(mHistoryInputSlot),
		historyInput.width,
		historyInput.height,
		(uint32_t)historyInput.state.access,
		(uint32_t)historyInput.state.layout,
		(uint32_t)historyInput.state.stages,
		GetFrameTextureSlotName(mHistoryOutputSlot),
		historyOutput.width,
		historyOutput.height,
		(uint32_t)historyOutput.state.access,
		(uint32_t)historyOutput.state.layout,
		(uint32_t)historyOutput.state.stages,
		GetFrameTextureSlotName(presentSlot),
		GetFrameTextureSlotName(mUpscaledInputSlot),
		mUseUpscaledInFinal ? "yes" : "no");
	Printf("NRI PT beauty path: nrd_and_composition -> pre_exposed_hdr_temporal -> final_display_mapping inspect_scene=15 inspect_pre_exposed=45 inspect_post_taa=13 inspect_post_upscale=14\n");
	Printf("NRI PT temporal domain: history=pre_exposed_hdr exposure=%.3f exposure_stops=%.3f reset_threshold_stops=%.3f\n",
		exposure,
		exposureStops,
		NRI_TAA_EXPOSURE_RESET_THRESHOLD_STOPS);
}

void NRIRenderer::ArmTemporalTraceBudget(const char* reason)
{
	if (!nri_pttemporaltrace)
	{
		return;
	}

	if ((int)nri_pttraceframes >= NRI_TEMPORAL_TRACE_REARM_FRAME_COUNT)
	{
		return;
	}

	nri_pttraceframes = NRI_TEMPORAL_TRACE_REARM_FRAME_COUNT;
	const NRIMainUpscalerKind resolvedMain = ResolveMainUpscalerKind(false);
	const NRIPostSharpenKind resolvedPost = ResolvePostSharpenKind(false);
	Printf("NRI PT temporal trace: armed=%d reason=%s frame=%u debug=%d resolved_main=%s resolved_post=%s\n",
		(int)nri_pttraceframes,
		reason != nullptr ? reason : "unspecified",
		mFrameIndex,
		(int)nri_ptdebug,
		GetMainUpscalerName(resolvedMain),
		GetPostSharpenName(resolvedPost));
}

void NRIRenderer::ResetResidentMapChunkRegistry()
{
	mResidentMapChunkRegistry = {};
}

void NRIRenderer::ResetStaticMapChunkAtlas(StaticMapChunkAtlas& atlas) const
{
	atlas = {};
}

uint32_t NRIRenderer::AllocateChunkAtlasSlice(uint32_t count, uint32_t alignment, uint32_t& cursor) const
{
	if (alignment == 0)
	{
		alignment = 1;
	}

	const uint32_t alignedCursor =
		cursor % alignment == 0 ?
		cursor :
		cursor + (alignment - cursor % alignment);
	cursor = alignedCursor + count;
	return alignedCursor;
}

bool NRIRenderer::BuildStaticMapChunkAtlasLayout(const StaticMapSceneCache& staticScene, StaticMapChunkAtlas& outAtlas) const
{
	ResetStaticMapChunkAtlas(outAtlas);
	if (staticScene.chunks.empty())
	{
		return false;
	}

	outAtlas.valid = true;
	outAtlas.buildSerial = staticScene.buildSerial;
	outAtlas.chunkCount = (uint32_t)staticScene.chunks.size();
	outAtlas.chunks.resize(staticScene.chunks.size());

	uint32_t vertexCursor = 0;
	uint32_t indexCursor = 0;
	uint32_t primitiveCursor = 0;
	uint32_t materialCursor = 0;
	for (uint32_t chunkListIndex = 0; chunkListIndex < staticScene.chunks.size(); ++chunkListIndex)
	{
		const auto& sourceChunk = staticScene.chunks[chunkListIndex];
		auto& atlasChunk = outAtlas.chunks[chunkListIndex];
		atlasChunk.valid = true;
		atlasChunk.chunkIndex = sourceChunk.chunkIndex;
		atlasChunk.staticSceneChunkListIndex = chunkListIndex;
		atlasChunk.vertexOffset = AllocateChunkAtlasSlice(sourceChunk.vertexCount, 1u, vertexCursor);
		atlasChunk.vertexCount = sourceChunk.vertexCount;
		atlasChunk.indexOffset = AllocateChunkAtlasSlice(sourceChunk.indexCount, 1u, indexCursor);
		atlasChunk.indexCount = sourceChunk.indexCount;
		atlasChunk.primitiveOffset = AllocateChunkAtlasSlice(sourceChunk.primitiveCount, 1u, primitiveCursor);
		atlasChunk.primitiveCount = sourceChunk.primitiveCount;
		atlasChunk.materialOffset = AllocateChunkAtlasSlice(sourceChunk.materialCount, 1u, materialCursor);
		atlasChunk.materialCount = sourceChunk.materialCount;
	}

	outAtlas.vertexCount = vertexCursor;
	outAtlas.indexCount = indexCursor;
	outAtlas.primitiveCount = primitiveCursor;
	outAtlas.materialCount = materialCursor;
	outAtlas.vertexCapacity = vertexCursor;
	outAtlas.indexCapacity = indexCursor;
	outAtlas.primitiveCapacity = primitiveCursor;
	outAtlas.materialCapacity = materialCursor;
	return true;
}

void NRIRenderer::UploadChunkGeometryToAtlas(
	const nri_scene::GeometryData& sourceGeometry,
	const StaticMapSceneCache::ChunkCache& sourceChunk,
	const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
	std::vector<nri_scene::SceneVertex>& outVertices,
	std::vector<uint32_t>& outIndices,
	std::vector<nri_scene::PrimitiveData>& outPrimitives) const
{
	if (!atlasChunk.valid)
	{
		return;
	}

	if (sourceChunk.vertexOffset + sourceChunk.vertexCount <= sourceGeometry.vertices.size() &&
		atlasChunk.vertexOffset + atlasChunk.vertexCount <= outVertices.size())
	{
		std::copy_n(
			sourceGeometry.vertices.data() + sourceChunk.vertexOffset,
			sourceChunk.vertexCount,
			outVertices.data() + atlasChunk.vertexOffset);
	}

	if (sourceChunk.indexOffset + sourceChunk.indexCount <= sourceGeometry.indices.size() &&
		atlasChunk.indexOffset + atlasChunk.indexCount <= outIndices.size())
	{
		for (uint32_t i = 0; i < sourceChunk.indexCount; ++i)
		{
			const uint32_t sourceIndex = sourceGeometry.indices[sourceChunk.indexOffset + i];
			outIndices[atlasChunk.indexOffset + i] = atlasChunk.vertexOffset + sourceIndex - sourceChunk.vertexOffset;
		}
	}

	if (sourceChunk.primitiveOffset + sourceChunk.primitiveCount <= sourceGeometry.primitives.size() &&
		atlasChunk.primitiveOffset + atlasChunk.primitiveCount <= outPrimitives.size())
	{
		for (uint32_t i = 0; i < sourceChunk.primitiveCount; ++i)
		{
			nri_scene::PrimitiveData primitive = sourceGeometry.primitives[sourceChunk.primitiveOffset + i];
			primitive.indices[0] = atlasChunk.vertexOffset + primitive.indices[0] - sourceChunk.vertexOffset;
			primitive.indices[1] = atlasChunk.vertexOffset + primitive.indices[1] - sourceChunk.vertexOffset;
			primitive.indices[2] = atlasChunk.vertexOffset + primitive.indices[2] - sourceChunk.vertexOffset;
			primitive.materialIndex = atlasChunk.materialOffset + primitive.materialIndex - sourceChunk.materialOffset;
			const uint32_t provenanceIndex = sourceChunk.primitiveOffset + i;
			primitive.reserved0 =
				provenanceIndex < sourceGeometry.primitiveProvenance.size() &&
				sourceGeometry.primitiveProvenance[provenanceIndex].mapChunkIndex >= 0 ?
				(uint32_t)sourceGeometry.primitiveProvenance[provenanceIndex].mapChunkIndex :
				UINT32_MAX;
			outPrimitives[atlasChunk.primitiveOffset + i] = primitive;
		}
	}
}

void NRIRenderer::UploadChunkMaterialsToAtlas(
	const std::vector<nri_scene::MaterialData>& sourceMaterials,
	const StaticMapSceneCache::ChunkCache& sourceChunk,
	const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
	std::vector<nri_scene::MaterialData>& outMaterials) const
{
	if (!atlasChunk.valid)
	{
		return;
	}

	if (sourceChunk.materialOffset + sourceChunk.materialCount <= sourceMaterials.size() &&
		atlasChunk.materialOffset + atlasChunk.materialCount <= outMaterials.size())
	{
		std::copy_n(
			sourceMaterials.data() + sourceChunk.materialOffset,
			sourceChunk.materialCount,
			outMaterials.data() + atlasChunk.materialOffset);
	}
}

bool NRIRenderer::UploadStaticMapChunkAtlas(
	NRIBufferResource& vertexBuffer,
	NRIBufferResource& indexBuffer,
	NRIBufferResource& primitiveBuffer,
	NRIBufferResource& materialBuffer,
	StaticMapChunkAtlas& atlas,
	const StaticMapSceneCache& staticScene,
	const std::vector<nri_scene::MaterialData>& gpuMaterials)
{
	if (!BuildStaticMapChunkAtlasLayout(staticScene, atlas))
	{
		return false;
	}

	std::vector<nri_scene::SceneVertex> atlasVertices(atlas.vertexCount);
	std::vector<uint32_t> atlasIndices(atlas.indexCount);
	std::vector<nri_scene::PrimitiveData> atlasPrimitives(atlas.primitiveCount);
	std::vector<nri_scene::MaterialData> atlasMaterials(atlas.materialCount);

	for (uint32_t chunkListIndex = 0; chunkListIndex < staticScene.chunks.size(); ++chunkListIndex)
	{
		const auto& sourceChunk = staticScene.chunks[chunkListIndex];
		const auto& atlasChunk = atlas.chunks[chunkListIndex];
		UploadChunkGeometryToAtlas(
			staticScene.geometry,
			sourceChunk,
			atlasChunk,
			atlasVertices,
			atlasIndices,
			atlasPrimitives);
		UploadChunkMaterialsToAtlas(
			gpuMaterials,
			sourceChunk,
			atlasChunk,
			atlasMaterials);
	}

	return
		EnsureStructuredBuffer(vertexBuffer, mVertexBufferStats, atlasVertices.data(), atlasVertices.size() * sizeof(nri_scene::SceneVertex), sizeof(nri_scene::SceneVertex), NRIFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), NRIAccelerationStructureBuildInputAccess()) &&
		EnsureStructuredBuffer(indexBuffer, mIndexBufferStats, atlasIndices.data(), atlasIndices.size() * sizeof(uint32_t), sizeof(uint32_t), NRIFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), NRIAccelerationStructureBuildInputAccess()) &&
		EnsureStructuredBuffer(primitiveBuffer, mPrimitiveBufferStats, atlasPrimitives.data(), atlasPrimitives.size() * sizeof(nri_scene::PrimitiveData), sizeof(nri_scene::PrimitiveData), nri::BufferUsageBits::SHADER_RESOURCE, NRIComputeShaderResourceAccess()) &&
		EnsureStructuredBuffer(materialBuffer, mMaterialBufferStats, atlasMaterials.data(), atlasMaterials.size() * sizeof(nri_scene::MaterialData), sizeof(nri_scene::MaterialData), nri::BufferUsageBits::SHADER_RESOURCE, NRIComputeShaderResourceAccess());
}

bool NRIRenderer::UploadStaticMapChunkMaterialAtlas(
	NRIBufferResource& materialBuffer,
	const StaticMapChunkAtlas& atlas,
	const StaticMapSceneCache& staticScene,
	const std::vector<nri_scene::MaterialData>& gpuMaterials)
{
	if (!atlas.valid || atlas.chunks.size() != staticScene.chunks.size())
	{
		return false;
	}

	std::vector<nri_scene::MaterialData> atlasMaterials(atlas.materialCount);
	for (uint32_t chunkListIndex = 0; chunkListIndex < staticScene.chunks.size(); ++chunkListIndex)
	{
		UploadChunkMaterialsToAtlas(
			gpuMaterials,
			staticScene.chunks[chunkListIndex],
			atlas.chunks[chunkListIndex],
			atlasMaterials);
	}

	return EnsureStructuredBuffer(
		materialBuffer,
		mMaterialBufferStats,
		atlasMaterials.data(),
		atlasMaterials.size() * sizeof(nri_scene::MaterialData),
		sizeof(nri_scene::MaterialData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess());
}

void NRIRenderer::SyncResidentMapChunkRegistryFromStaticScene()
{
	ResetResidentMapChunkRegistry();
	if (!mMapWorld.valid)
	{
		return;
	}

	auto& registry = mResidentMapChunkRegistry;
	registry.valid = true;
	registry.buildSerial = mMapWorld.buildSerial;
	registry.chunkCount = (uint32_t)mMapWorld.chunks.size();
	registry.entries.resize(mMapWorld.chunks.size());

	for (size_t chunkListIndex = 0; chunkListIndex < mMapWorld.chunks.size(); ++chunkListIndex)
	{
		const auto& mapChunk = mMapWorld.chunks[chunkListIndex];
		auto& entry = registry.entries[chunkListIndex];
		entry.valid = true;
		entry.chunkIndex = mapChunk.chunkIndex;
		if (chunkListIndex < mRuntimeMapMutations.chunks.size())
		{
			const auto& replacement = mRuntimeMapMutations.chunks[chunkListIndex];
			entry.appliedBaseline = replacement.baseline;
			entry.baselineSignature = replacement.baselineSignature;
			entry.liveSignature = replacement.liveSignature != 0 ? replacement.liveSignature : replacement.baselineSignature;
		}
	}

	const bool atlasMatchesStaticScene =
		mStaticMapChunkAtlas.valid &&
		mStaticMapChunkAtlas.buildSerial == mStaticMapScene.buildSerial &&
		mStaticMapChunkAtlas.chunks.size() == mStaticMapScene.chunks.size();
	for (size_t chunkListIndex = 0; chunkListIndex < mStaticMapScene.chunks.size(); ++chunkListIndex)
	{
		const auto& staticChunk = mStaticMapScene.chunks[chunkListIndex];
		if (staticChunk.chunkIndex >= registry.entries.size())
		{
			continue;
		}

		auto& entry = registry.entries[staticChunk.chunkIndex];
		entry.active = true;
		entry.mappedInStaticScene = true;
		entry.staticSceneChunkListIndex = (uint32_t)chunkListIndex;
		if (atlasMatchesStaticScene)
		{
			const auto& atlasChunk = mStaticMapChunkAtlas.chunks[chunkListIndex];
			entry.vertexOffset = atlasChunk.vertexOffset;
			entry.vertexCount = atlasChunk.vertexCount;
			entry.indexOffset = atlasChunk.indexOffset;
			entry.indexCount = atlasChunk.indexCount;
			entry.primitiveOffset = atlasChunk.primitiveOffset;
			entry.primitiveCount = atlasChunk.primitiveCount;
			entry.materialOffset = atlasChunk.materialOffset;
			entry.materialCount = atlasChunk.materialCount;
		}
		else
		{
			entry.vertexOffset = staticChunk.vertexOffset;
			entry.vertexCount = staticChunk.vertexCount;
			entry.indexOffset = staticChunk.indexOffset;
			entry.indexCount = staticChunk.indexCount;
			entry.primitiveOffset = staticChunk.primitiveOffset;
			entry.primitiveCount = staticChunk.primitiveCount;
			entry.materialOffset = staticChunk.materialOffset;
			entry.materialCount = staticChunk.materialCount;
		}
		entry.animatedMaterialSignature = staticChunk.animatedMaterialSignature;
		entry.animatedGeometrySignature = staticChunk.animatedGeometrySignature;
		entry.hasAnimatedTextureCandidates = staticChunk.hasAnimatedTextureCandidates;
		entry.animatedRefreshSuppressed = staticChunk.animatedRefreshSuppressed;
		entry.accelerationResident = staticChunk.accelerationStructure.accelerationStructure != nullptr;

		registry.activeChunkCount++;
		registry.mappedChunkCount++;
		if (entry.accelerationResident)
		{
			registry.accelerationResidentChunkCount++;
		}
		if (entry.hasAnimatedTextureCandidates)
		{
			registry.animatedCandidateChunkCount++;
		}
		if (entry.animatedRefreshSuppressed)
		{
			registry.animatedRefreshSuppressedChunkCount++;
		}
	}
}

void NRIRenderer::TraceTemporalState(const char* stage, NRIMainUpscalerKind resolvedMainUpscaler, NRIPostSharpenKind resolvedPostSharpen, bool runAppTaa, FrameTextureSlot primarySlot, FrameTextureSlot secondarySlot) const
{
	if (!ShouldEmitTemporalTraceLogs())
	{
		return;
	}

	const NRITextureResource& historyInput = GetFrameTexture(mHistoryInputSlot);
	const NRITextureResource& historyOutput = GetFrameTexture(mHistoryOutputSlot);
	const NRITextureResource& primary = GetFrameTexture(primarySlot);
	const NRITextureResource& secondary = secondarySlot == FrameTextureSlot::Count ? GetFrameTexture(mHistoryOutputSlot) : GetFrameTexture(secondarySlot);
	Printf("NRI PT temporal trace: stage=%s frame=%u debug=%d resolved_main=%s resolved_post=%s run_app_taa=%s gui_capture=%s domain=pre_exposed_hdr exposure=%.3f reset=%s reset_reason=%s prev_camera=%s history_in=%s[%ux%u a=%u l=%u s=0x%x] history_out=%s[%ux%u a=%u l=%u s=0x%x] primary=%s[%ux%u a=%u l=%u s=0x%x] secondary=%s[%ux%u a=%u l=%u s=0x%x] use_upscaled=%s\n",
		stage != nullptr ? stage : "unknown",
		mFrameIndex,
		(int)nri_ptdebug,
		GetMainUpscalerName(resolvedMainUpscaler),
		GetPostSharpenName(resolvedPostSharpen),
		runAppTaa ? "yes" : "no",
		mGuiCaptureActive ? "yes" : "no",
		GetTemporalExposure(mFrameBuffer->GetPathTracingOutputPolicy()),
		mResetHistory ? "yes" : "no",
		mLastHistoryResetReason.c_str(),
		mHasPreviousCameraState ? "yes" : "no",
		GetFrameTextureSlotName(mHistoryInputSlot),
		historyInput.width,
		historyInput.height,
		(uint32_t)historyInput.state.access,
		(uint32_t)historyInput.state.layout,
		(uint32_t)historyInput.state.stages,
		GetFrameTextureSlotName(mHistoryOutputSlot),
		historyOutput.width,
		historyOutput.height,
		(uint32_t)historyOutput.state.access,
		(uint32_t)historyOutput.state.layout,
		(uint32_t)historyOutput.state.stages,
		GetFrameTextureSlotName(primarySlot),
		primary.width,
		primary.height,
		(uint32_t)primary.state.access,
		(uint32_t)primary.state.layout,
		(uint32_t)primary.state.stages,
		GetFrameTextureSlotName(secondarySlot == FrameTextureSlot::Count ? mHistoryOutputSlot : secondarySlot),
		secondary.width,
		secondary.height,
		(uint32_t)secondary.state.access,
		(uint32_t)secondary.state.layout,
		(uint32_t)secondary.state.stages,
		mUseUpscaledInFinal ? "yes" : "no");
}

void NRIRenderer::PrintMapWorldStatus() const
{
	if (!mMapWorld.valid)
	{
		Printf("NRI PT map world: no authoritative map world has been built yet.\n");
		return;
	}

	const auto& stats = mMapWorld.stats;
	Printf("NRI PT map world: level=%s build_serial=%llu chunks=%u local_spaces=%u sectors=%u sections=%u surfaces=%u walls=%u flats=%u portal_surfaces=%u portal_graph=%u portal_targets=%u wall_portals=%u sector_portals=%u mirror_portals=%u runtime_portals=%u skies=%u tris=%u\n",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)mMapWorld.buildSerial,
		stats.chunkCount,
		stats.localSpaceCount,
		stats.sectorCount,
		stats.sectionCount,
		stats.surfaceCount,
		stats.wallSurfaceCount,
		stats.flatSurfaceCount,
		stats.portalSurfaceCount,
		stats.portalCount,
		stats.portalTargetCount,
		stats.wallPortalCount,
		stats.sectorPortalCount,
		stats.mirrorPortalCount,
		stats.runtimePortalCount,
		stats.skySurfaceCount,
		stats.triangleCount);
}

void NRIRenderer::PrintPortalTraversalStatus() const
{
	if (!mMapWorld.valid)
	{
		Printf("NRI PT portal traversal: no authoritative portal graph is available.\n");
		return;
	}

	Printf("NRI PT portal traversal: depth=%u reflective=%u transfer=%u runtime_bound=%u hittable_surfaces=%u plane_portals_pending=%u\n",
		ClampTraceBounceCount((int)nri_ptportaldepth, 8u),
		CountPortalTraversalClass(mMapWorld, NRI_PORTAL_TRAVERSAL_CLASS_REFLECTIVE),
		CountPortalTraversalClass(mMapWorld, NRI_PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER),
		CountPortalTraversalClass(mMapWorld, NRI_PORTAL_TRAVERSAL_CLASS_RUNTIME_BOUND),
		mMapWorld.stats.portalSurfaceCount,
		CountPendingPlanePortals(mMapWorld));
}

void NRIRenderer::PrintStaticMapSceneStatus() const
{
	const char* source = mUsedStaticMapSceneLastFrame ? "authoritative-map-world" : "captured-scene";
	Printf("NRI PT static scene: source=%s resident=%s build_serial=%llu scene_builds=%u uploads=%u as_builds=%u animated_candidate_chunks=%u animated_refreshes=%u animated_refresh_uploads=%u animated_geometry_fallbacks=%u animated_refresh_suppressed=%u reuses=%u last_frame_upload=%s last_frame_as_build=%s chunks=%u tlas_instances=%u tris=%u materials=%u\n",
		source,
		(mStaticMapScene.valid && mStaticMapScene.texturesResident && mStaticMapScene.buffersResident && mStaticMapScene.accelerationResident) ? "yes" : "no",
		(unsigned long long)mStaticMapScene.buildSerial,
		mStaticMapScene.sceneBuildCount,
		mStaticMapScene.gpuUploadCount,
		mStaticMapScene.accelerationBuildCount,
		mStaticMapScene.animatedCandidateChunkCount,
		mStaticMapScene.animatedRefreshCount,
		mStaticMapScene.animatedRefreshUploadCount,
		mStaticMapScene.animatedGeometryFallbackCount,
		mStaticMapScene.animatedRefreshSuppressedChunkCount,
		mStaticMapScene.reuseCount,
		mUploadedStaticMapSceneLastFrame ? "yes" : "no",
		mBuiltStaticMapSceneASLastFrame ? "yes" : "no",
		(uint32_t)mStaticMapScene.chunks.size(),
		mStaticMapScene.tlasInstanceCount,
		(uint32_t)mStaticMapScene.geometry.primitives.size(),
		(uint32_t)mStaticMapScene.gpuMaterials.size());
}

void NRIRenderer::PrintResidentMapChunkRegistryStatus() const
{
	if (!mResidentMapChunkRegistry.valid)
	{
		Printf("NRI PT resident chunk registry: unavailable.\n");
		return;
	}

	Printf("NRI PT resident chunk registry: build_serial=%llu chunks=%u active=%u mapped=%u acceleration_resident=%u animated_candidates=%u animated_refresh_suppressed=%u\n",
		(unsigned long long)mResidentMapChunkRegistry.buildSerial,
		mResidentMapChunkRegistry.chunkCount,
		mResidentMapChunkRegistry.activeChunkCount,
		mResidentMapChunkRegistry.mappedChunkCount,
		mResidentMapChunkRegistry.accelerationResidentChunkCount,
		mResidentMapChunkRegistry.animatedCandidateChunkCount,
		mResidentMapChunkRegistry.animatedRefreshSuppressedChunkCount);
}

NRIRenderer::PersistentDynamicSurfaceStats NRIRenderer::GatherPersistentDynamicEmissiveSurfaceStats() const
{
	PersistentDynamicSurfaceStats stats = {};
	if (!mPersistentDynamicEmissiveCache.valid)
	{
		return stats;
	}

	stats.wallSurfaceCount = (uint32_t)mPersistentDynamicEmissiveCache.sceneView.opaqueWalls.size();
	stats.flatSurfaceCount = (uint32_t)mPersistentDynamicEmissiveCache.sceneView.opaqueFlats.size();
	stats.spriteSurfaceCount = (uint32_t)mPersistentDynamicEmissiveCache.sceneView.opaqueSprites.size();

	auto accumulate = [&stats](const auto& surfaces)
	{
		for (const auto& surface : surfaces)
		{
			if (surface.provenance.actorIndex >= 0)
			{
				stats.actorSurfaceCount++;
			}
			else
			{
				stats.nonActorSurfaceCount++;
			}

			switch (surface.provenance.sourceType)
			{
			case nri_scene::SurfaceSourceType::FacingSprite:
				stats.actorFacingSpriteCount++;
				break;
			case nri_scene::SurfaceSourceType::VoxelProxySprite:
				stats.actorVoxelSpriteCount++;
				break;
			default:
				break;
			}
		}
	};

	accumulate(mPersistentDynamicEmissiveCache.sceneView.opaqueWalls);
	accumulate(mPersistentDynamicEmissiveCache.sceneView.opaqueFlats);
	accumulate(mPersistentDynamicEmissiveCache.sceneView.opaqueSprites);
	return stats;
}

NRIRenderer::RuntimeMutationCacheStats NRIRenderer::GatherRuntimeMutationCacheStats() const
{
	RuntimeMutationCacheStats stats = {};
	for (const auto& replacement : mRuntimeMapMutations.chunks)
	{
		if (!replacement.active)
		{
			continue;
		}

		stats.activeChunkCount++;
		if (!replacement.valid)
		{
			continue;
		}

		stats.validChunkCount++;
		if (replacement.excludeStaticChunk)
		{
			stats.excludedStaticChunkCount++;
		}

		stats.cachedSurfaceCount += replacement.surfaceCount;
		stats.cachedTriangleCount += replacement.triangleCount;
		stats.cachedMaterialCount += (uint32_t)replacement.materialBridge.materials.size();
	}

	return stats;
}

void NRIRenderer::PrintDynamicSceneStatus() const
{
	const PersistentDynamicSurfaceStats persistentStats = GatherPersistentDynamicEmissiveSurfaceStats();

	Printf("NRI PT dynamic scene: active=%s sprite_surfaces=%u tris=%u materials=%u models=%u unsupported_models=%u mirror_extended_surfaces=%u mirror_extended_tris=%u mirror_extended_materials=%u mirror_extended_models=%u mirror_extended_unsupported_models=%u mirror_player_surfaces=%u mirror_player_tris=%u mirror_player_materials=%u mirror_player_models=%u mirror_player_unsupported_models=%u mirror_distance=%.1f dynamic_as_builds=%u last_frame_as_build=%s active_tlas_instances=%u emissive_cache=%s cache_surfaces=%u cache_tris=%u cache_materials=%u\n",
		mUsedDynamicSceneLastFrame ? "yes" : "no",
		mDynamicSceneLastFrame.spriteSurfaceCount,
		mDynamicSceneLastFrame.primitiveCount,
		mDynamicSceneLastFrame.materialCount,
		mDynamicSceneLastFrame.modelCount,
		mDynamicSceneLastFrame.unsupportedModelCount,
		mDynamicSceneLastFrame.mirrorExtendedSurfaceCount,
		mDynamicSceneLastFrame.mirrorExtendedPrimitiveCount,
		mDynamicSceneLastFrame.mirrorExtendedMaterialCount,
		mDynamicSceneLastFrame.mirrorExtendedModelCount,
		mDynamicSceneLastFrame.mirrorExtendedUnsupportedModelCount,
		mDynamicSceneLastFrame.mirrorPlayerSurfaceCount,
		mDynamicSceneLastFrame.mirrorPlayerPrimitiveCount,
		mDynamicSceneLastFrame.mirrorPlayerMaterialCount,
		mDynamicSceneLastFrame.mirrorPlayerModelCount,
		mDynamicSceneLastFrame.mirrorPlayerUnsupportedModelCount,
		(double)nri_ptmirrordynamicdistance,
		mDynamicSceneLastFrame.asBuildCount,
		mBuiltDynamicSceneASLastFrame ? "yes" : "no",
		mActiveTlasInstanceCount,
		mPersistentDynamicEmissiveCache.valid ? "yes" : "no",
		mPersistentDynamicEmissiveCache.surfaceCount,
		mPersistentDynamicEmissiveCache.primitiveCount,
		mPersistentDynamicEmissiveCache.materialCount);
	Printf("NRI PT dynamic cache: actor_surfaces=%u non_actor_surfaces=%u walls=%u flats=%u sprites=%u actor_facing=%u actor_voxel=%u highwater=surfaces:%u tris:%u mats:%u actor:%u non_actor:%u walls:%u flats:%u sprites:%u actor_facing:%u actor_voxel:%u\n",
		persistentStats.actorSurfaceCount,
		persistentStats.nonActorSurfaceCount,
		persistentStats.wallSurfaceCount,
		persistentStats.flatSurfaceCount,
		persistentStats.spriteSurfaceCount,
		persistentStats.actorFacingSpriteCount,
		persistentStats.actorVoxelSpriteCount,
		mPersistentDynamicEmissiveHighWaterSurfaceCount,
		mPersistentDynamicEmissiveHighWaterPrimitiveCount,
		mPersistentDynamicEmissiveHighWaterMaterialCount,
		mPersistentDynamicEmissiveHighWaterStats.actorSurfaceCount,
		mPersistentDynamicEmissiveHighWaterStats.nonActorSurfaceCount,
		mPersistentDynamicEmissiveHighWaterStats.wallSurfaceCount,
		mPersistentDynamicEmissiveHighWaterStats.flatSurfaceCount,
		mPersistentDynamicEmissiveHighWaterStats.spriteSurfaceCount,
		mPersistentDynamicEmissiveHighWaterStats.actorFacingSpriteCount,
		mPersistentDynamicEmissiveHighWaterStats.actorVoxelSpriteCount);
	Printf("NRI PT actor sprite diag: trace=%d cache_actor_facing=%u cache_actor_voxel=%u prune_checks=%u prune_matches=%u drop_missing_actor=%u drop_missing_actor_index=%u drop_null_live_texture=%u drop_texture_mismatch=%u drop_palette_mismatch=%u\n",
		(int)nri_ptactorspritetrace,
		persistentStats.actorFacingSpriteCount,
		persistentStats.actorVoxelSpriteCount,
		mActorSpriteDebugStats.lastPruneChecks,
		mActorSpriteDebugStats.lastPruneMatches,
		mActorSpriteDebugStats.lastPruneDroppedMissingActor,
		mActorSpriteDebugStats.lastPruneDroppedMissingActorIndex,
		mActorSpriteDebugStats.lastPruneDroppedNullLiveTexture,
		mActorSpriteDebugStats.lastPruneDroppedTextureMismatch,
		mActorSpriteDebugStats.lastPruneDroppedPaletteMismatch);
	Printf("NRI PT scene texture overflow: textures=%u truncated=%u clamps=base:%u normal:%u metallic:%u roughness:%u emissive:%u builds=%llu warned=%s\n",
		mSceneTextureOverflowStats.textureCountLastBuild,
		mSceneTextureOverflowStats.truncatedTextureCountLastBuild,
		mSceneTextureOverflowStats.baseTextureClampCountLastBuild,
		mSceneTextureOverflowStats.normalTextureClampCountLastBuild,
		mSceneTextureOverflowStats.metallicTextureClampCountLastBuild,
		mSceneTextureOverflowStats.roughnessTextureClampCountLastBuild,
		mSceneTextureOverflowStats.emissiveTextureClampCountLastBuild,
		(unsigned long long)mSceneTextureOverflowStats.totalOverflowBuilds,
		mSceneTextureOverflowStats.warningLogged ? "yes" : "no");
	Printf("NRI PT scene texture attribution: reason=%s requested=%u actor_materials=%u base=%u glow=%u normal=%u metallic=%u roughness=%u emissive=%u\n",
		mLastPerfShellTraceStats.sceneTextureReason.empty() ? "none" : mLastPerfShellTraceStats.sceneTextureReason.c_str(),
		mLastPerfShellTraceStats.sceneTextureRequestedCount,
		mLastPerfShellTraceStats.sceneTextureReferencedActorMaterialCount,
		mLastPerfShellTraceStats.sceneTextureReferencedBaseCount,
		mLastPerfShellTraceStats.sceneTextureReferencedGlowCount,
		mLastPerfShellTraceStats.sceneTextureReferencedNormalCount,
		mLastPerfShellTraceStats.sceneTextureReferencedMetallicCount,
		mLastPerfShellTraceStats.sceneTextureReferencedRoughnessCount,
		mLastPerfShellTraceStats.sceneTextureReferencedEmissiveCount);
	Printf("NRI PT actor overflow: materials=%u clamps=base:%u normal:%u metallic:%u roughness:%u emissive:%u omitted=%u\n",
		mLastPerfShellTraceStats.actorOverflowMaterialCount,
		mLastPerfShellTraceStats.actorOverflowBaseClampCount,
		mLastPerfShellTraceStats.actorOverflowNormalClampCount,
		mLastPerfShellTraceStats.actorOverflowMetallicClampCount,
		mLastPerfShellTraceStats.actorOverflowRoughnessClampCount,
		mLastPerfShellTraceStats.actorOverflowEmissiveClampCount,
		mLastPerfShellTraceStats.actorOverflowTraceOmittedCount);
	Printf("NRI PT scene texture cache: entries=%u highwater=%u misses=%u inserts=%u transitions=%u lookup_ms=%.3f realize_ms=%.3f descriptor_ms=%.3f transition_ms=%.3f\n",
		mSceneTextureCacheDebugStats.cacheEntriesLastBuild,
		mSceneTextureCacheDebugStats.cacheEntriesHighWater,
		mSceneTextureCacheDebugStats.lookupMissesLastBuild,
		mSceneTextureCacheDebugStats.insertCountLastBuild,
		mSceneTextureCacheDebugStats.transitionCountLastFrame,
		mSceneTextureCacheDebugStats.lookupMsLastBuild,
		mSceneTextureCacheDebugStats.realizeMsLastBuild,
		mSceneTextureCacheDebugStats.descriptorMsLastBuild,
		mSceneTextureCacheDebugStats.transitionMsLastFrame);
	Printf("NRI PT binding diag: label=%s materials=%u textures=%u actor_surfaces=%u actor_count=%u bridge_hash=0x%llx actor_hash=0x%llx scene_tex_updates=%llu scene_tex_hash=0x%llx scene_tex_reason=%s qframe=%u outstanding=%u scene_data_updates=%llu scene_data_hash=0x%llx scene_data_reason=%s qframe=%u outstanding=%u\n",
		mDescriptorCoherencyDebugStats.lastMaterialBuildLabel.empty() ? "none" : mDescriptorCoherencyDebugStats.lastMaterialBuildLabel.c_str(),
		mDescriptorCoherencyDebugStats.lastMaterialCount,
		mDescriptorCoherencyDebugStats.lastTextureCount,
		mDescriptorCoherencyDebugStats.lastActorSpriteSurfaceCount,
		mDescriptorCoherencyDebugStats.lastActorSpriteActorCount,
		(unsigned long long)mDescriptorCoherencyDebugStats.lastMaterialBridgeHash,
		(unsigned long long)mDescriptorCoherencyDebugStats.lastActorSpriteMaterialHash,
		(unsigned long long)mDescriptorCoherencyDebugStats.sceneTextureSetUpdates,
		(unsigned long long)mDescriptorCoherencyDebugStats.lastSceneTextureDescriptorHash,
		mDescriptorCoherencyDebugStats.lastSceneTextureReason.empty() ? "none" : mDescriptorCoherencyDebugStats.lastSceneTextureReason.c_str(),
		mDescriptorCoherencyDebugStats.lastSceneTextureQueuedFrameIndex,
		mDescriptorCoherencyDebugStats.lastSceneTextureOutstandingQueuedFrames,
		(unsigned long long)mDescriptorCoherencyDebugStats.sceneDataSetUpdates,
		(unsigned long long)mDescriptorCoherencyDebugStats.lastSceneDataDescriptorHash,
		mDescriptorCoherencyDebugStats.lastSceneDataReason.empty() ? "none" : mDescriptorCoherencyDebugStats.lastSceneDataReason.c_str(),
		mDescriptorCoherencyDebugStats.lastSceneDataQueuedFrameIndex,
		mDescriptorCoherencyDebugStats.lastSceneDataOutstandingQueuedFrames);
	Printf("NRI PT material builds: calls=%u override_builds=%u override_ms=%.3f material_ms=%.3f\n",
		mLastPerfShellTraceStats.materialBuildCalls,
		mLastPerfShellTraceStats.actorOverrideMapBuildCalls,
		mLastPerfShellTraceStats.actorOverrideMapBuildMs,
		mLastPerfShellTraceStats.materialBuildMs);
	Printf("NRI PT mutation detail: structural_ms=%.3f material_refresh_ms=%.3f structural=%u material_refresh=%u refresh_delta=%u refresh_delta_mask=0x%x refresh_hwcanvas=%u refresh_animated=%u struct_delta=%u struct_delta_mask=0x%x struct_view=%u struct_static_anim_flip=%u struct_excl_static_flip=%u struct_force_topology=%u struct_invalid=%u hwcanvas_chunks=%u\n",
		mLastPerfShellTraceStats.runtimeMutationStructuralRebuildMs,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshMs,
		mLastPerfShellTraceStats.runtimeMutationStructuralRebuildChunks,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshChunks,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReplacementDeltaChunks,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReasonMaskOr,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshHardwareCanvasChunks,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshAnimatedChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaReasonMaskOr,
		mLastPerfShellTraceStats.runtimeMutationStructuralReplacementViewChangedChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralStaticAnimatedModeFlipChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralExcludeStaticFlipChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralForcedTopologyChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralInvalidChunks,
		mLastPerfShellTraceStats.runtimeMutationHardwareCanvasChunkCount);
	Printf("NRI PT runtime mutation rebaseline state: queued=%s state=%s queue_frame=%u frames_queued=%u active_chunks=%u stable_chunks=%u candidate_build_serial=%llu scene_chunks=%u scene_surfaces=%u scene_tris=%u cache=%u/%u blas_prepare=%u/%u blas=%u/%u retired=%u build_world_ms=%.3f build_static_scene_cache_ms=%.3f realize_static_scene_textures_ms=%.3f upload_static_scene_buffers_ms=%.3f prepare_static_scene_blas_ms=%.3f build_static_scene_blas_ms=%.3f build_static_scene_tlas_ms=%.3f swap_ms=%.3f retire_ms=%.3f\n",
		mRuntimeMutationRebaselineState != RuntimeMutationRebaselineState::Idle ? "yes" : "no",
		GetRuntimeMutationRebaselineStateName(mRuntimeMutationRebaselineState),
		mRuntimeMutationRebaselineQueueFrame,
		(mRuntimeMutationRebaselineState != RuntimeMutationRebaselineState::Idle && mFrameIndex >= mRuntimeMutationRebaselineQueueFrame) ? (mFrameIndex - mRuntimeMutationRebaselineQueueFrame) : 0u,
		mPendingRuntimeMutationRebaselineActiveChunkCount,
		mPendingRuntimeMutationRebaselineStableChunkCount,
		(unsigned long long)(mRuntimeMutationRebaselineCandidate.valid ? mRuntimeMutationRebaselineCandidate.world.buildSerial : 0ull),
		(uint32_t)mRuntimeMutationRebaselineCandidate.staticScene.chunks.size(),
		mRuntimeMutationRebaselineCandidate.valid ? mRuntimeMutationRebaselineCandidate.world.stats.surfaceCount : 0u,
		mRuntimeMutationRebaselineCandidate.valid ? mRuntimeMutationRebaselineCandidate.world.stats.triangleCount : 0u,
		mRuntimeMutationRebaselineCandidate.cacheBuildCount,
		(uint32_t)mRuntimeMutationRebaselineCandidate.world.chunks.size(),
		mRuntimeMutationRebaselineCandidate.blasPrepareCount,
		(uint32_t)mRuntimeMutationRebaselineCandidate.staticScene.chunks.size(),
		mRuntimeMutationRebaselineCandidate.blasBuildCount,
		(uint32_t)mRuntimeMutationRebaselineCandidate.staticScene.chunks.size(),
		(uint32_t)mRetiredRuntimeMutationRebaselineStaticScenes.size(),
		mRuntimeMutationRebaselineBuildWorldMs,
		mRuntimeMutationRebaselineBuildStaticSceneCacheMs,
		mRuntimeMutationRebaselineRealizeStaticSceneTexturesMs,
		mRuntimeMutationRebaselineUploadStaticSceneBuffersMs,
		mRuntimeMutationRebaselinePrepareStaticSceneBlasMs,
		mRuntimeMutationRebaselineBuildStaticSceneBlasMs,
		mRuntimeMutationRebaselineBuildStaticSceneTlasMs,
		mRuntimeMutationRebaselineSwapMs,
		mRuntimeMutationRebaselineRetireMs);
	for (size_t index = 0; index < NRIRenderer::MaterialBuildTraceSlotCount; ++index)
	{
		const auto& entry = mLastPerfShellTraceStats.materialBuildByLabel[index];
		if (entry.calls == 0 && entry.overrideBuildCalls == 0)
		{
			continue;
		}

		Printf("NRI PT material detail: label=%s calls=%u override_builds=%u override_ms=%.3f material_ms=%.3f\n",
			GetMaterialBuildTraceSlotName((MaterialBuildTraceSlot)index),
			entry.calls,
			entry.overrideBuildCalls,
			entry.overrideBuildMs,
			entry.materialBuildMs);
		Printf("NRI PT material textures: label=%s materials=%u actor_materials=%u textures=%u base=%u glow=%u normal=%u metallic=%u roughness=%u emissive=%u\n",
			GetMaterialBuildTraceSlotName((MaterialBuildTraceSlot)index),
			entry.materialCount,
			entry.actorMaterialCount,
			entry.textureCount,
			entry.baseTextureCount,
			entry.glowTextureCount,
			entry.normalTextureCount,
			entry.metallicTextureCount,
			entry.roughnessTextureCount,
			entry.emissiveTextureCount);
	}
}

void NRIRenderer::ResetPersistentDynamicEmissiveCache()
{
	mPersistentDynamicEmissiveCache = {};
	mActorSpriteDebugStats = {};
}

void NRIRenderer::PrunePersistentDynamicEmissiveCacheToLiveActors()
{
	mActorSpriteDebugStats = {};
	if (!mPersistentDynamicEmissiveCache.valid)
	{
		return;
	}

	std::unordered_map<int32_t, bool> liveActorIndices;
	std::unordered_map<int32_t, DCoreActor*> liveActorsByIndex;
	liveActorIndices.reserve(256);
	liveActorsByIndex.reserve(256);

	TSpriteIterator<DCoreActor> it;
	while (auto actor = it.Next())
	{
		if (actor == nullptr ||
			!actor->exists() ||
			(actor->ObjectFlags & OF_EuthanizeMe) != 0)
		{
			continue;
		}

		liveActorIndices[(int32_t)actor->GetIndex()] = true;
		liveActorsByIndex[(int32_t)actor->GetIndex()] = actor;
	}

	bool needsPrune = false;
	auto detectStaleActorOwnership = [this, &needsPrune, &liveActorIndices, &liveActorsByIndex](const auto& surfaces)
	{
		for (const auto& surface : surfaces)
		{
			if (SurfaceUsesLiveActorTextureValidation(surface))
			{
				mActorSpriteDebugStats.lastPruneChecks++;
				if (surface.provenance.actorIndex < 0)
				{
					mActorSpriteDebugStats.lastPruneDroppedMissingActorIndex++;
					if (ShouldTraceActorSpriteMismatch())
					{
						Printf("NRI PT actor-sprite cache: action=drop reason=missing_actor_index source=%s actor=%d surface_tex=%d surface_ptr=%p surface_pal=%d\n",
							GetSurfaceSourceTypeName(surface.provenance.sourceType),
							surface.provenance.actorIndex,
							surface.material.texture != nullptr ? surface.material.texture->GetID().GetIndex() : -1,
							surface.material.texture,
							surface.material.palette);
					}
					needsPrune = true;
					continue;
				}

				auto liveActorIt = liveActorsByIndex.find(surface.provenance.actorIndex);
				if (liveActorIt == liveActorsByIndex.end())
				{
					mActorSpriteDebugStats.lastPruneDroppedMissingActor++;
					if (ShouldTraceActorSpriteMismatch())
					{
						Printf("NRI PT actor-sprite cache: action=drop reason=missing_actor source=%s actor=%d surface_tex=%d surface_ptr=%p surface_pal=%d\n",
							GetSurfaceSourceTypeName(surface.provenance.sourceType),
							surface.provenance.actorIndex,
							surface.material.texture != nullptr ? surface.material.texture->GetID().GetIndex() : -1,
							surface.material.texture,
							surface.material.palette);
					}
					needsPrune = true;
					continue;
				}

				const ActorSpriteLiveMatchDetails match = EvaluateCachedSurfaceMatchAgainstLiveActor(surface, *liveActorIt->second);
				if (match.result == ActorSpriteLiveMatchResult::Match)
				{
					mActorSpriteDebugStats.lastPruneMatches++;
					if (ShouldTraceActorSpriteVerbose())
					{
						Printf("NRI PT actor-sprite cache: action=keep reason=%s source=%s actor=%d surface_tex=%d surface_ptr=%p live_tex=%d live_ptr=%p surface_pal=%d live_pal=%d\n",
							GetActorSpriteLiveMatchResultName(match.result),
							GetSurfaceSourceTypeName(surface.provenance.sourceType),
							surface.provenance.actorIndex,
							match.surfaceTextureId,
							surface.material.texture,
							match.liveTextureId,
							match.liveTexture,
							match.surfacePalette,
							match.livePalette);
					}
					continue;
				}

				switch (match.result)
				{
				case ActorSpriteLiveMatchResult::NullLiveTexture: mActorSpriteDebugStats.lastPruneDroppedNullLiveTexture++; break;
				case ActorSpriteLiveMatchResult::TextureMismatch: mActorSpriteDebugStats.lastPruneDroppedTextureMismatch++; break;
				case ActorSpriteLiveMatchResult::PaletteMismatch: mActorSpriteDebugStats.lastPruneDroppedPaletteMismatch++; break;
				default: break;
				}
				if (ShouldTraceActorSpriteMismatch())
				{
					Printf("NRI PT actor-sprite cache: action=drop reason=%s source=%s actor=%d surface_tex=%d surface_ptr=%p live_tex=%d live_ptr=%p surface_pal=%d live_pal=%d\n",
						GetActorSpriteLiveMatchResultName(match.result),
						GetSurfaceSourceTypeName(surface.provenance.sourceType),
						surface.provenance.actorIndex,
						match.surfaceTextureId,
						surface.material.texture,
						match.liveTextureId,
						match.liveTexture,
						match.surfacePalette,
						match.livePalette);
				}
				needsPrune = true;
			}
			else if (surface.provenance.actorIndex >= 0 &&
				liveActorIndices.find(surface.provenance.actorIndex) == liveActorIndices.end())
			{
				needsPrune = true;
			}
		}
	};

	detectStaleActorOwnership(mPersistentDynamicEmissiveCache.sceneView.opaqueWalls);
	detectStaleActorOwnership(mPersistentDynamicEmissiveCache.sceneView.opaqueFlats);
	detectStaleActorOwnership(mPersistentDynamicEmissiveCache.sceneView.opaqueSprites);
	if (!needsPrune)
	{
		return;
	}

	PersistentDynamicEmissiveCache next = {};
	next.sceneView.drawInfo = mPersistentDynamicEmissiveCache.sceneView.drawInfo;
	next.sceneView.sky = mPersistentDynamicEmissiveCache.sceneView.sky;
	Copy3(mPersistentDynamicEmissiveCache.sceneView.skyColor, next.sceneView.skyColor);
	Copy3(mPersistentDynamicEmissiveCache.sceneView.groundColor, next.sceneView.groundColor);

	auto appendLiveOwnedSurfaces = [&liveActorIndices, &liveActorsByIndex](const auto& source, auto& destination)
	{
		for (const auto& surface : source)
		{
			if (SurfaceUsesLiveActorTextureValidation(surface))
			{
				if (surface.provenance.actorIndex < 0)
				{
					continue;
				}

				auto liveActorIt = liveActorsByIndex.find(surface.provenance.actorIndex);
				if (liveActorIt == liveActorsByIndex.end() || !CachedSurfaceMatchesLiveActor(surface, *liveActorIt->second))
				{
					continue;
				}
			}
			else if (surface.provenance.actorIndex >= 0 &&
				liveActorIndices.find(surface.provenance.actorIndex) == liveActorIndices.end())
			{
				continue;
			}

			destination.push_back(surface);
		}
	};

	appendLiveOwnedSurfaces(mPersistentDynamicEmissiveCache.sceneView.opaqueWalls, next.sceneView.opaqueWalls);
	appendLiveOwnedSurfaces(mPersistentDynamicEmissiveCache.sceneView.opaqueFlats, next.sceneView.opaqueFlats);
	appendLiveOwnedSurfaces(mPersistentDynamicEmissiveCache.sceneView.opaqueSprites, next.sceneView.opaqueSprites);

	next.surfaceCount =
		(uint32_t)next.sceneView.opaqueWalls.size() +
		(uint32_t)next.sceneView.opaqueFlats.size() +
		(uint32_t)next.sceneView.opaqueSprites.size();
	if (next.surfaceCount == 0)
	{
		mPersistentDynamicEmissiveCache = {};
		return;
	}

	{
		Clocker clock(NriPTGeometryBuild);
		nri_scene::BuildGeometry(next.sceneView, next.geometry);
		AssignGeometryPortalIndices(mMapWorld, next.geometry);
	}
	{
		Clocker clock(NriPTMaterialBuild);
		BuildMaterialsWithActorOverrides(next.sceneView, next.materialBridge, "persistent_emissive_cache_prune");
	}

	next.primitiveCount = (uint32_t)next.geometry.primitives.size();
	next.materialCount = (uint32_t)next.materialBridge.materials.size();
	next.sceneView.stats.totalDrawItems = next.surfaceCount;
	next.sceneView.stats.wallDrawItems = (uint32_t)next.sceneView.opaqueWalls.size();
	next.sceneView.stats.flatDrawItems = (uint32_t)next.sceneView.opaqueFlats.size();
	next.sceneView.stats.spriteDrawItems = (uint32_t)next.sceneView.opaqueSprites.size();
	next.sceneView.stats.triangleEstimate = next.primitiveCount;
	next.sceneView.stats.materialRefs = next.materialCount;
	next.valid = next.primitiveCount > 0 && next.materialCount > 0;
	if (!next.valid)
	{
		mPersistentDynamicEmissiveCache = {};
		return;
	}

	mPersistentDynamicEmissiveCache = std::move(next);
}

bool NRIRenderer::RebuildPersistentDynamicEmissiveCache(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials)
{
	PersistentDynamicEmissiveCache next = {};
	next.sceneView.drawInfo = sceneView.drawInfo;
	next.sceneView.sky = sceneView.sky;
	Copy3(sceneView.skyColor, next.sceneView.skyColor);
	Copy3(sceneView.groundColor, next.sceneView.groundColor);

	bool liveSceneHasEmissive = false;
	std::unordered_set<uint64_t> seenSurfaceKeys;
	seenSurfaceKeys.reserve(
		sceneView.opaqueWalls.size() +
		sceneView.opaqueFlats.size() +
		sceneView.opaqueSprites.size() +
		mPersistentDynamicEmissiveCache.sceneView.opaqueWalls.size() +
		mPersistentDynamicEmissiveCache.sceneView.opaqueFlats.size() +
		mPersistentDynamicEmissiveCache.sceneView.opaqueSprites.size());

	uint32_t materialIndex = 0;
	auto appendLiveSurfaceList = [this, &materials, &materialIndex, &liveSceneHasEmissive, &seenSurfaceKeys](const auto& source, auto& destination)
	{
		for (const auto& surface : source)
		{
			const bool keepSurface =
				materialIndex < materials.lightMetadata.size() &&
				mSceneLights.MaterialWouldEmit(materials.lightMetadata[materialIndex]);
			const bool keepSpriteCacheSurface =
				surface.provenance.sourceType != nri_scene::SurfaceSourceType::FacingSprite &&
				surface.provenance.sourceType != nri_scene::SurfaceSourceType::VoxelProxySprite;
			if (keepSurface && (keepSpriteCacheSurface || surface.provenance.actorIndex >= 0))
			{
				liveSceneHasEmissive = true;
				const uint64_t identityKey = BuildPersistentEmissiveSurfaceIdentityKey(surface);
				if (seenSurfaceKeys.insert(identityKey).second)
				{
					destination.push_back(surface);
				}
			}
			materialIndex++;
		}
	};

	appendLiveSurfaceList(sceneView.opaqueWalls, next.sceneView.opaqueWalls);
	appendLiveSurfaceList(sceneView.opaqueFlats, next.sceneView.opaqueFlats);
	appendLiveSurfaceList(sceneView.opaqueSprites, next.sceneView.opaqueSprites);

	if (mPersistentDynamicEmissiveCache.valid)
	{
		AppendUniquePersistentEmissiveSurfaces(
			mPersistentDynamicEmissiveCache.sceneView.opaqueWalls,
			next.sceneView.opaqueWalls,
			seenSurfaceKeys);
		AppendUniquePersistentEmissiveSurfaces(
			mPersistentDynamicEmissiveCache.sceneView.opaqueFlats,
			next.sceneView.opaqueFlats,
			seenSurfaceKeys);
		AppendUniquePersistentEmissiveSurfaces(
			mPersistentDynamicEmissiveCache.sceneView.opaqueSprites,
			next.sceneView.opaqueSprites,
			seenSurfaceKeys);
	}

	next.surfaceCount =
		(uint32_t)next.sceneView.opaqueWalls.size() +
		(uint32_t)next.sceneView.opaqueFlats.size() +
		(uint32_t)next.sceneView.opaqueSprites.size();
	if (next.surfaceCount == 0)
	{
		mPersistentDynamicEmissiveCache = {};
		return liveSceneHasEmissive;
	}

	{
		Clocker clock(NriPTGeometryBuild);
		nri_scene::BuildGeometry(next.sceneView, next.geometry);
		AssignGeometryPortalIndices(mMapWorld, next.geometry);
	}
	{
		Clocker clock(NriPTMaterialBuild);
		BuildMaterialsWithActorOverrides(next.sceneView, next.materialBridge, "persistent_emissive_cache_rebuild");
	}

	next.primitiveCount = (uint32_t)next.geometry.primitives.size();
	next.materialCount = (uint32_t)next.materialBridge.materials.size();
	next.sceneView.stats.totalDrawItems = next.surfaceCount;
	next.sceneView.stats.wallDrawItems = (uint32_t)next.sceneView.opaqueWalls.size();
	next.sceneView.stats.flatDrawItems = (uint32_t)next.sceneView.opaqueFlats.size();
	next.sceneView.stats.spriteDrawItems = (uint32_t)next.sceneView.opaqueSprites.size();
	next.sceneView.stats.triangleEstimate = next.primitiveCount;
	next.sceneView.stats.materialRefs = next.materialCount;
	next.valid = next.primitiveCount > 0 && next.materialCount > 0;
	if (!next.valid)
	{
		mPersistentDynamicEmissiveCache = {};
		return liveSceneHasEmissive;
	}

	mPersistentDynamicEmissiveCache = std::move(next);
	return liveSceneHasEmissive;
}

void NRIRenderer::PrintRuntimeMapMutationStatus() const
{
	const RuntimeMutationCacheStats cacheStats = GatherRuntimeMutationCacheStats();

	Printf("NRI PT runtime map: active=%s dirty_chunks=%u replaced_chunks=%u rebuilt_chunks=%u held_chunks=%u animated_refreshes=%u blind_spots=%u sector_geom=%u sector_mat=%u wall_geom=%u wall_mat=%u sector_dirty=%u section_dirty=%u dragged=%u surfaces=%u tris=%u materials=%u\n",
		mRuntimeMapLastFrame.active ? "yes" : "no",
		mRuntimeMapLastFrame.dirtyChunkCount,
		mRuntimeMapLastFrame.replacedChunkCount,
		mRuntimeMapLastFrame.rebuiltChunkCount,
		mRuntimeMapLastFrame.heldChunkCount,
		mRuntimeMapLastFrame.animatedRefreshChunkCount,
		mRuntimeMapLastFrame.blindSpotChunkCount,
		mRuntimeMapLastFrame.sectorGeometryChunkCount,
		mRuntimeMapLastFrame.sectorMaterialChunkCount,
		mRuntimeMapLastFrame.wallGeometryChunkCount,
		mRuntimeMapLastFrame.wallMaterialChunkCount,
		mRuntimeMapLastFrame.sectorDirtyChunkCount,
		mRuntimeMapLastFrame.sectionDirtyChunkCount,
		mRuntimeMapLastFrame.draggedChunkCount,
		mRuntimeMapLastFrame.replacementSurfaceCount,
		mRuntimeMapLastFrame.replacementTriangleCount,
		mRuntimeMapLastFrame.materialCount);
	Printf("NRI PT runtime map cache: active_chunks=%u valid_chunks=%u exclude_static=%u cached_surfaces=%u cached_tris=%u cached_materials=%u highwater=active:%u valid:%u exclude_static:%u surfaces:%u tris:%u mats:%u\n",
		cacheStats.activeChunkCount,
		cacheStats.validChunkCount,
		cacheStats.excludedStaticChunkCount,
		cacheStats.cachedSurfaceCount,
		cacheStats.cachedTriangleCount,
		cacheStats.cachedMaterialCount,
		mRuntimeMutationCacheHighWaterStats.activeChunkCount,
		mRuntimeMutationCacheHighWaterStats.validChunkCount,
		mRuntimeMutationCacheHighWaterStats.excludedStaticChunkCount,
		mRuntimeMutationCacheHighWaterStats.cachedSurfaceCount,
		mRuntimeMutationCacheHighWaterStats.cachedTriangleCount,
		mRuntimeMutationCacheHighWaterStats.cachedMaterialCount);
}

void NRIRenderer::PrintRuntimeSpaceLinkStatus() const
{
	Printf("NRI PT runtime links: active=%s geo_effect=%s query_attempted=%s query_rejected=%s candidate_sector=%d candidate_lotag=%d source_sector=%d reported_geo_count=%d view_roots=%u visible_sectors=%u providers=%u geo_providers=%u provider_groups=%u local_space_matches=%u visible_matches=%u links=%u translated_chunks=%u orphan_local_spaces=%u unresolved_runtime_portals=%u surfaces=%u tris=%u materials=%u\n",
		mRuntimeSpaceLinkLastFrame.active ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.geoEffectActive ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.queryAttempted ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.queryRejected ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.candidateSectorIndex,
		mRuntimeSpaceLinkLastFrame.candidateSectorLotag,
		mRuntimeSpaceLinkLastFrame.sourceSectorIndex,
		mRuntimeSpaceLinkLastFrame.reportedGeoCount,
		mRuntimeSpaceLinkLastFrame.viewRootSectorCount,
		mRuntimeSpaceLinkLastFrame.visibleSectorCount,
		mRuntimeSpaceLinkLastFrame.providerSectorCount,
		mRuntimeSpaceLinkLastFrame.geoProviderCount,
		mRuntimeSpaceLinkLastFrame.providerGroupCount,
		mRuntimeSpaceLinkLastFrame.localSpaceMatchedProviderCount,
		mRuntimeSpaceLinkLastFrame.visibleMatchedProviderCount,
		mRuntimeSpaceLinkLastFrame.linkCount,
		mRuntimeSpaceLinkLastFrame.translatedChunkCount,
		mRuntimeSpaceLinkLastFrame.orphanLocalSpaceCount,
		mRuntimeSpaceLinkLastFrame.unresolvedRuntimePortalCount,
		mRuntimeSpaceLinkLastFrame.surfaceCount,
		mRuntimeSpaceLinkLastFrame.triangleCount,
		mRuntimeSpaceLinkLastFrame.materialCount);
	Printf("NRI PT runtime link motion: prev_chunk_offsets=%u topology_changed=%s special_material_history=%s\n",
		(uint32_t)mRuntimeChunkTranslationHistory.size(),
		mRuntimeSpaceLinkLastFrame.topologyChanged ? "yes" : "no",
		"portal_mirror_raw_fallback");
}

void NRIRenderer::TraceRuntimeLinkEvents(HWDrawInfo& di)
{
	if (!nri_ptruntimelinktrace)
	{
		mHasRuntimeLinkTraceState = false;
		mLastRuntimeLinkTraceState = {};
		return;
	}

	RuntimeLinkTraceState current = {};
	current.valid = true;
	current.candidateSectorIndex = mRuntimeSpaceLinkLastFrame.candidateSectorIndex;
	current.sourceSectorIndex = mRuntimeSpaceLinkLastFrame.sourceSectorIndex;
	current.geoEffectActive = mRuntimeSpaceLinkLastFrame.geoEffectActive;

	const BitArray& visibleSectors = di.GetVisibleSectors();
	for (unsigned sectorIndex = 0; sectorIndex < visibleSectors.Size(); ++sectorIndex)
	{
		if (!visibleSectors.Check(sectorIndex))
		{
			continue;
		}

		const auto& sec = sector[sectorIndex];
		if (sec.lotag != 0)
		{
			current.visibleTaggedSectorCount++;
			if (current.taggedVisibleSectorStoredCount < current.taggedVisibleSectors.size())
			{
				RuntimeTaggedSectorDebugInfo info = {};
				if (gi != nullptr && gi->GetRuntimeLinkDebugTaggedSectorInfo((int)sectorIndex, &info))
				{
					current.taggedVisibleSectors[current.taggedVisibleSectorStoredCount++] = info;
				}
				else
				{
					info.available = true;
					info.sectorIndex = (int32_t)sectorIndex;
					info.lotag = sec.lotag;
					info.hitag = sec.hitag;
					current.taggedVisibleSectors[current.taggedVisibleSectorStoredCount++] = info;
				}
			}
		}
		if (sec.lotag == 848)
		{
			current.visible848SectorCount++;
		}
		if (sec.lotag == 160 || sec.lotag == 161)
		{
			current.visibleTeleportSectorCount++;
		}
	}

	if (gi != nullptr)
	{
		gi->GetRuntimeLinkDebugState(&current.game);
	}

	std::array<int32_t, 4> controlRoots =
	{
		current.candidateSectorIndex,
		current.sourceSectorIndex,
		current.game.playerSectorIndex,
		current.game.actorSectorIndex
	};

	for (const int32_t rootSectorIndex : controlRoots)
	{
		if (!validSectorIndex(rootSectorIndex))
		{
			continue;
		}

		RuntimeTaggedSectorDebugInfo rootInfo = {};
		if (GetRuntimeSectorControlInfo(rootSectorIndex, rootInfo))
		{
			AppendRuntimeSectorControlInfo(current.nearbyControlSectors, current.nearbyControlSectorStoredCount, rootInfo);
		}

		const auto& rootSector = sector[(unsigned)rootSectorIndex];
		for (const auto& wal : rootSector.walls)
		{
			if (!wal.twoSided())
			{
				continue;
			}

			const int32_t adjacentSectorIndex = wal.nextsector;
			RuntimeTaggedSectorDebugInfo adjacentInfo = {};
			if (GetRuntimeSectorControlInfo(adjacentSectorIndex, adjacentInfo))
			{
				AppendRuntimeSectorControlInfo(current.nearbyControlSectors, current.nearbyControlSectorStoredCount, adjacentInfo);
			}
		}
	}

	const bool sameAsLast =
		mHasRuntimeLinkTraceState &&
		mLastRuntimeLinkTraceState.valid == current.valid &&
		mLastRuntimeLinkTraceState.candidateSectorIndex == current.candidateSectorIndex &&
		mLastRuntimeLinkTraceState.sourceSectorIndex == current.sourceSectorIndex &&
		mLastRuntimeLinkTraceState.geoEffectActive == current.geoEffectActive &&
		mLastRuntimeLinkTraceState.visibleTaggedSectorCount == current.visibleTaggedSectorCount &&
		mLastRuntimeLinkTraceState.visible848SectorCount == current.visible848SectorCount &&
		mLastRuntimeLinkTraceState.visibleTeleportSectorCount == current.visibleTeleportSectorCount &&
		mLastRuntimeLinkTraceState.taggedVisibleSectorStoredCount == current.taggedVisibleSectorStoredCount &&
		mLastRuntimeLinkTraceState.nearbyControlSectorStoredCount == current.nearbyControlSectorStoredCount &&
		SameRuntimeLinkDebugState(mLastRuntimeLinkTraceState.game, current.game);

	bool sameTaggedSectors = true;
	if (sameAsLast)
	{
		for (uint32_t i = 0; i < current.taggedVisibleSectorStoredCount; ++i)
		{
			if (!SameRuntimeTaggedSectorDebugInfo(mLastRuntimeLinkTraceState.taggedVisibleSectors[i], current.taggedVisibleSectors[i]))
			{
				sameTaggedSectors = false;
				break;
			}
		}
	}

	bool sameNearbyControlSectors = true;
	if (sameAsLast)
	{
		for (uint32_t i = 0; i < current.nearbyControlSectorStoredCount; ++i)
		{
			if (!SameRuntimeTaggedSectorDebugInfo(mLastRuntimeLinkTraceState.nearbyControlSectors[i], current.nearbyControlSectors[i]))
			{
				sameNearbyControlSectors = false;
				break;
			}
		}
	}

	if (sameAsLast && sameTaggedSectors && sameNearbyControlSectors)
	{
		return;
	}

	mLastRuntimeLinkTraceState = current;
	mHasRuntimeLinkTraceState = true;

	Printf("NRI PT runtime link event: geo_effect=%s candidate_sector=%d source_sector=%d player_sector=%d lotag=%d hitag=%d effective_lotag=%d actor_sector=%d actor_lotag=%d actor_hitag=%d on_warp=%d transporter_hold=%d rr_geo_count=%d special_water=%s visible_tagged=%u visible_848=%u visible_teleport=%u\n",
		current.geoEffectActive ? "yes" : "no",
		current.candidateSectorIndex,
		current.sourceSectorIndex,
		current.game.playerSectorIndex,
		current.game.playerSectorLotag,
		current.game.playerSectorHitag,
		current.game.effectiveSectorLotag,
		current.game.actorSectorIndex,
		current.game.actorSectorLotag,
		current.game.actorSectorHitag,
		current.game.onWarpingSector,
		current.game.transporterHold,
		current.game.rrGeoCount,
		current.game.specialWaterSector ? "yes" : "no",
		current.visibleTaggedSectorCount,
		current.visible848SectorCount,
		current.visibleTeleportSectorCount);

	if (current.taggedVisibleSectorStoredCount > 0)
	{
		std::string taggedLine = "NRI PT runtime tagged sectors:";
		for (uint32_t i = 0; i < current.taggedVisibleSectorStoredCount; ++i)
		{
			const auto& info = current.taggedVisibleSectors[i];
			taggedLine += " [sector=" + std::to_string(info.sectorIndex) +
				" lotag=" + std::to_string(info.lotag) +
				" hitag=" + std::to_string(info.hitag);
			if (info.effectorCount > 0)
			{
				taggedLine += " effectors=";
				const uint32_t storedEffectors = std::min<uint32_t>(info.effectorCount, (uint32_t)countof(info.effectorLotags));
				for (uint32_t effectorIndex = 0; effectorIndex < storedEffectors; ++effectorIndex)
				{
					if (effectorIndex > 0)
					{
						taggedLine += ",";
					}
					taggedLine += std::to_string(info.effectorLotags[effectorIndex]) +
						"/" + std::to_string(info.effectorHitags[effectorIndex]);
				}
				if (info.effectorCount > storedEffectors)
				{
					taggedLine += ",...";
				}
			}
			taggedLine += "]";
		}
		Printf("%s\n", taggedLine.c_str());
	}

	if (current.nearbyControlSectorStoredCount > 0)
	{
		std::string controlLine = "NRI PT runtime nearby controls:";
		for (uint32_t i = 0; i < current.nearbyControlSectorStoredCount; ++i)
		{
			const auto& info = current.nearbyControlSectors[i];
			controlLine += " [sector=" + std::to_string(info.sectorIndex) +
				" lotag=" + std::to_string(info.lotag) +
				" hitag=" + std::to_string(info.hitag);
			if (info.effectorCount > 0)
			{
				controlLine += " effectors=";
				const uint32_t storedEffectors = std::min<uint32_t>(info.effectorCount, (uint32_t)countof(info.effectorLotags));
				for (uint32_t effectorIndex = 0; effectorIndex < storedEffectors; ++effectorIndex)
				{
					if (effectorIndex > 0)
					{
						controlLine += ",";
					}
					controlLine += std::to_string(info.effectorLotags[effectorIndex]) +
						"/" + std::to_string(info.effectorHitags[effectorIndex]);
				}
				if (info.effectorCount > storedEffectors)
				{
					controlLine += ",...";
				}
			}
			controlLine += "]";
		}
		Printf("%s\n", controlLine.c_str());
	}
}

void NRIRenderer::TraceRuntimeMapMutationChunk(const nri_scene::PTMapChunk& mapChunk, RuntimeMapMutationCache::ChunkReplacement& replacement)
{
	if (!ShouldEmitTemporalTraceLogs())
	{
		return;
	}

	const bool filterByChunk = nri_ptmutationtracechunk >= 0;
	const bool filterBySector = nri_ptmutationtracesector >= 0;
	if (!filterByChunk && !filterBySector)
	{
		return;
	}

	if (filterByChunk && mapChunk.chunkIndex != (uint32_t)nri_ptmutationtracechunk)
	{
		return;
	}

	if (filterBySector && mapChunk.sectorIndex != nri_ptmutationtracesector)
	{
		return;
	}

	const bool changed =
		replacement.traceCount == 0 ||
		replacement.lastTraceSignature != replacement.liveSignature ||
		replacement.lastTraceAnimatedMaterialSignature != replacement.animatedMaterialSignature ||
		replacement.lastTraceReasonMask != replacement.reasonMask ||
		replacement.lastTraceActive != replacement.active ||
		replacement.lastTraceBlindSpot != replacement.blindSpot ||
		replacement.lastTraceAnimationOnlyRefreshed != replacement.animationOnlyRefreshed ||
		replacement.lastTraceStaticAnimatedReplacement != replacement.staticAnimatedReplacement;
	if (!changed)
	{
		return;
	}

	const std::string reasons = GetRuntimeMapMutationReasonSummary(replacement.reasonMask);
	Printf("NRI PT runtime map trace: chunk=%u sector=%d active=%s blind_spot=%s static_anim=%s signature_changed=%s anim_refresh=%s baseline_sig=0x%llx live_sig=0x%llx anim_sig=0x%llx reasons=%s section_dirty=%u sector_dirty=%s dragged=%s surfaces=%u tris=%u materials=%u\n",
		mapChunk.chunkIndex,
		mapChunk.sectorIndex,
		replacement.active ? "yes" : "no",
		replacement.blindSpot ? "yes" : "no",
		replacement.staticAnimatedReplacement ? "yes" : "no",
		replacement.liveSignature != replacement.baselineSignature ? "yes" : "no",
		replacement.animationOnlyRefreshed ? "yes" : "no",
		(unsigned long long)replacement.baselineSignature,
		(unsigned long long)replacement.liveSignature,
		(unsigned long long)replacement.animatedMaterialSignature,
		reasons.c_str(),
		replacement.sectionDirtyCount,
		replacement.sectorDirty ? "yes" : "no",
		replacement.dragged ? "yes" : "no",
		replacement.surfaceCount,
		replacement.triangleCount,
		(uint32_t)replacement.materialBridge.materials.size());

	replacement.lastTraceSignature = replacement.liveSignature;
	replacement.lastTraceAnimatedMaterialSignature = replacement.animatedMaterialSignature;
	replacement.lastTraceReasonMask = replacement.reasonMask;
	replacement.lastTraceActive = replacement.active;
	replacement.lastTraceBlindSpot = replacement.blindSpot;
	replacement.lastTraceAnimationOnlyRefreshed = replacement.animationOnlyRefreshed;
	replacement.lastTraceStaticAnimatedReplacement = replacement.staticAnimatedReplacement;
	replacement.traceCount++;
}

void NRIRenderer::PrintSceneBufferStatus() const
{
	const auto printBuffer = [](const NRIBufferResource& resource, const SceneBufferDebugStats& stats)
	{
		const uint64_t usedItems = resource.stride != 0 ? resource.usedSize / resource.stride : 0;
		const uint64_t capacityItems = resource.stride != 0 ? resource.size / resource.stride : 0;
		Printf("NRI PT %s buffer: used=%llu/%llu bytes items=%llu/%llu uploads=%u grows=%u overwrites=%u last_frame_bytes=%llu last_frame_grows=%u last_frame_overwrites=%u peak_used=%llu\n",
			stats.label,
			(unsigned long long)resource.usedSize,
			(unsigned long long)resource.size,
			(unsigned long long)usedItems,
			(unsigned long long)capacityItems,
			stats.uploadCount,
			stats.growthCount,
			stats.overwriteCount,
			(unsigned long long)stats.bytesUploadedLastFrame,
			stats.growEventsLastFrame,
			stats.overwriteEventsLastFrame,
			(unsigned long long)stats.peakUsedBytes);
	};

	const NRIBufferResource& activeVertexBuffer = GetActiveVertexBuffer();
	const NRIBufferResource& activeIndexBuffer = GetActiveIndexBuffer();
	const NRIBufferResource& activePrimitiveBuffer = GetActivePrimitiveBuffer();
	const NRIBufferResource& activeMaterialBuffer = GetActiveMaterialBuffer();
	const uint64_t totalUsed = activeVertexBuffer.usedSize + activeIndexBuffer.usedSize + activePrimitiveBuffer.usedSize + activeMaterialBuffer.usedSize;
	const uint64_t totalCapacity = activeVertexBuffer.size + activeIndexBuffer.size + activePrimitiveBuffer.size + activeMaterialBuffer.size;
	const uint64_t lastFrameUploadBytes =
		mVertexBufferStats.bytesUploadedLastFrame +
		mIndexBufferStats.bytesUploadedLastFrame +
		mPrimitiveBufferStats.bytesUploadedLastFrame +
		mMaterialBufferStats.bytesUploadedLastFrame;
	const uint32_t lastFrameGrowEvents =
		mVertexBufferStats.growEventsLastFrame +
		mIndexBufferStats.growEventsLastFrame +
		mPrimitiveBufferStats.growEventsLastFrame +
		mMaterialBufferStats.growEventsLastFrame;
	const uint32_t lastFrameOverwriteEvents =
		mVertexBufferStats.overwriteEventsLastFrame +
		mIndexBufferStats.overwriteEventsLastFrame +
		mPrimitiveBufferStats.overwriteEventsLastFrame +
		mMaterialBufferStats.overwriteEventsLastFrame;

	Printf("NRI PT scene buffers: used=%llu capacity=%llu last_frame_upload=%llu last_frame_grows=%u last_frame_overwrites=%u\n",
		(unsigned long long)totalUsed,
		(unsigned long long)totalCapacity,
		(unsigned long long)lastFrameUploadBytes,
		lastFrameGrowEvents,
		lastFrameOverwriteEvents);
	printBuffer(activeVertexBuffer, mVertexBufferStats);
	printBuffer(activeIndexBuffer, mIndexBufferStats);
	printBuffer(activePrimitiveBuffer, mPrimitiveBufferStats);
	printBuffer(activeMaterialBuffer, mMaterialBufferStats);
	printBuffer(mPortalBuffer, mPortalBufferStats);
	printBuffer(mRuntimeLightBuffer, mRuntimeLightBufferStats);
	printBuffer(mRuntimeLightTileHeaderBuffer, mRuntimeLightTileHeaderBufferStats);
	printBuffer(mRuntimeLightTileIndexBuffer, mRuntimeLightTileIndexBufferStats);
	printBuffer(mEmissivePrimitiveHeaderBuffer, mEmissivePrimitiveHeaderBufferStats);
	printBuffer(mEmissivePrimitiveBuffer, mEmissivePrimitiveBufferStats);
	printBuffer(mEmissivePrimitiveCdfBuffer, mEmissivePrimitiveCdfBufferStats);
	printBuffer(mSectorLightHeaderBuffer, mSectorLightHeaderBufferStats);
	printBuffer(mSectorLightBuffer, mSectorLightBufferStats);
}

void NRIRenderer::UpdateSurfaceProbe(const nri_scene::GeometryData& geometry, const nri_scene::MaterialBridgeData* materials, bool allowLogging)
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.surfaceProbeMs);
	const bool logSurfaceProbe = allowLogging && nri_ptsurfaceprobe > 0;
	if (geometry.primitives.empty())
	{
		mLastSurfaceProbe = {};
		mLastSurfaceProbe.valid = true;
		if (!logSurfaceProbe)
		{
			return;
		}

		const bool logOnChangeOnly = nri_ptsurfaceprobe >= 2;
		if (logOnChangeOnly && mLastLoggedSurfaceProbe.valid && !mLastLoggedSurfaceProbe.hit)
		{
			return;
		}

		Printf("NRI PT surface probe: miss\n");
		mLastLoggedSurfaceProbe = mLastSurfaceProbe;
		return;
	}

	SurfaceProbeResult result = {};
	result.valid = true;

	float direction[3] = { mCurrentCameraForward[0], mCurrentCameraForward[1], mCurrentCameraForward[2] };
	Normalize3(direction);

	float bestDistance = std::numeric_limits<float>::infinity();
	for (uint32_t primitiveIndex = 0; primitiveIndex < geometry.primitives.size(); ++primitiveIndex)
	{
		const auto& primitive = geometry.primitives[primitiveIndex];
		const auto& v0 = geometry.vertices[primitive.indices[0]];
		const auto& v1 = geometry.vertices[primitive.indices[1]];
		const auto& v2 = geometry.vertices[primitive.indices[2]];
		float hitT = 0.0f;
		if (!IntersectProbeTriangle(v0, v1, v2, mCurrentCameraPos, direction, hitT) || hitT >= bestDistance)
		{
			continue;
		}

		bestDistance = hitT;
		result.hit = true;
		result.primitiveIndex = primitiveIndex;
		result.materialIndex = primitive.materialIndex;
		result.primitiveFlags = primitive.flags;
		result.distance = hitT;
		result.position[0] = mCurrentCameraPos[0] + direction[0] * hitT;
		result.position[1] = mCurrentCameraPos[1] + direction[1] * hitT;
		result.position[2] = mCurrentCameraPos[2] + direction[2] * hitT;
		result.normal[0] = primitive.normal[0];
		result.normal[1] = primitive.normal[1];
		result.normal[2] = primitive.normal[2];
		if (primitiveIndex < geometry.primitiveProvenance.size())
		{
			result.provenance = geometry.primitiveProvenance[primitiveIndex];
		}
	}

	if (result.hit && materials != nullptr && result.materialIndex < materials->lightMetadata.size())
	{
		const auto& metadata = materials->lightMetadata[result.materialIndex];
		const auto& materialData = materials->materials[result.materialIndex];
		result.materialLightingFlags = metadata.lightingFlags;
		result.textureId = metadata.textureId;
		result.materialClass = metadata.materialClass;
		result.lightLevel = metadata.lightLevel;
		result.alpha = metadata.alpha;
		result.normalTextureIndex = metadata.normalTextureIndex;
		result.metallicTextureIndex = metadata.metallicTextureIndex;
		result.roughnessTextureIndex = metadata.roughnessTextureIndex;
		result.metalnessHint = materialData.metalnessHint;
		result.roughnessHint = materialData.roughnessHint;
		Copy3(metadata.averageColor, result.averageColor);
		Copy3(metadata.emissiveColor, result.emissiveColor);
		Copy3(metadata.glowColor, result.glowColor);

		nri_scene::MaterialData effectiveMaterial = {};
		effectiveMaterial.textureIndex = metadata.textureIndex;
		effectiveMaterial.paletteIndex = metadata.paletteIndex;
		effectiveMaterial.flags = metadata.materialFlags;
		effectiveMaterial.materialClass = metadata.materialClass;
		effectiveMaterial.lightLevel = metadata.lightLevel;
		effectiveMaterial.alpha = metadata.alpha;
		effectiveMaterial.normalTextureIndex = materialData.normalTextureIndex;
		effectiveMaterial.metallicTextureIndex = materialData.metallicTextureIndex;
		effectiveMaterial.roughnessTextureIndex = materialData.roughnessTextureIndex;
		effectiveMaterial.metalnessHint = materialData.metalnessHint;
		effectiveMaterial.roughnessHint = materialData.roughnessHint;
		effectiveMaterial.emissiveTextureIndex = metadata.emissiveTextureIndex;
		mSceneLights.ApplyEmissiveMaterialSettings(metadata, effectiveMaterial);
		result.emissiveMode = effectiveMaterial.emissiveMode;
		result.emissiveTextureIndex = effectiveMaterial.emissiveTextureIndex;
	}

	if (result.hit)
	{
		if (!mSurfaceProbeFrame.valid)
		{
			result.sceneDataSource = UINT32_MAX;
			result.sceneOwner = NRI_SURFACE_PROBE_OWNER_UNKNOWN;
		}
		else if (!mSurfaceProbeFrame.usesStaticMapScene)
		{
			result.sceneDataSource = NRI_SCENE_DATA_SOURCE_DYNAMIC;
			result.sceneOwner = NRI_SURFACE_PROBE_OWNER_CAPTURED_SCENE;
		}
		else if (result.primitiveIndex < mSurfaceProbeFrame.staticPrimitiveCount)
		{
			result.sceneDataSource = NRI_SCENE_DATA_SOURCE_STATIC;
			result.sceneOwner = NRI_SURFACE_PROBE_OWNER_STATIC_MAP;
		}
		else
		{
			uint32_t overlayPrimitiveIndex = result.primitiveIndex - mSurfaceProbeFrame.staticPrimitiveCount;
			result.sceneDataSource = NRI_SCENE_DATA_SOURCE_DYNAMIC;
			if (overlayPrimitiveIndex < mSurfaceProbeFrame.runtimeSpaceLinkPrimitiveCount)
			{
				result.sceneOwner = NRI_SURFACE_PROBE_OWNER_RUNTIME_LINK;
			}
			else
			{
				overlayPrimitiveIndex -= std::min(overlayPrimitiveIndex, mSurfaceProbeFrame.runtimeSpaceLinkPrimitiveCount);
				if (overlayPrimitiveIndex < mSurfaceProbeFrame.runtimeMutationPrimitiveCount)
				{
					result.sceneOwner = NRI_SURFACE_PROBE_OWNER_RUNTIME_MUTATION;
				}
				else
				{
					overlayPrimitiveIndex -= std::min(overlayPrimitiveIndex, mSurfaceProbeFrame.runtimeMutationPrimitiveCount);
					result.sceneOwner = overlayPrimitiveIndex < mSurfaceProbeFrame.dynamicPrimitiveCount ?
						NRI_SURFACE_PROBE_OWNER_DYNAMIC_OVERLAY :
						NRI_SURFACE_PROBE_OWNER_UNKNOWN;
				}
			}
		}
	}

	auto sameIdentity = [](const SurfaceProbeResult& a, const SurfaceProbeResult& b)
	{
		if (a.valid != b.valid || a.hit != b.hit)
		{
			return false;
		}
		if (!a.valid || !a.hit)
		{
			return true;
		}

		return
			a.provenance.sourceType == b.provenance.sourceType &&
			a.provenance.sectorIndex == b.provenance.sectorIndex &&
			a.provenance.wallIndex == b.provenance.wallIndex &&
			a.provenance.nextSectorIndex == b.provenance.nextSectorIndex &&
			a.provenance.actorIndex == b.provenance.actorIndex &&
			a.provenance.drawListType == b.provenance.drawListType &&
			a.provenance.cstat == b.provenance.cstat &&
			a.textureId == b.textureId &&
			a.materialLightingFlags == b.materialLightingFlags &&
			a.primitiveFlags == b.primitiveFlags &&
			a.sceneDataSource == b.sceneDataSource &&
			a.sceneOwner == b.sceneOwner &&
			a.materialIndex == b.materialIndex &&
			(a.provenance.sourceType != nri_scene::SurfaceSourceType::Unknown || a.primitiveIndex == b.primitiveIndex);
	};

	mLastSurfaceProbe = result;
	if (!logSurfaceProbe)
	{
		return;
	}

	const bool logOnChangeOnly = nri_ptsurfaceprobe >= 2;
	if (logOnChangeOnly && sameIdentity(mLastLoggedSurfaceProbe, result))
	{
		return;
	}

	if (!result.hit)
	{
		Printf("NRI PT surface probe: miss\n");
		mLastLoggedSurfaceProbe = result;
		return;
	}

	const SurfaceProbeEmissiveDiagnostics emissiveDiagnostics = BuildSurfaceProbeEmissiveDiagnostics(result);

	const uint32_t flags = result.primitiveFlags;
	const uint32_t lightingFlags = result.materialLightingFlags;
	const int32_t localSpaceIndex = result.provenance.mapChunkIndex >= 0 ? nri_scene::FindMapWorldLocalSpaceIndex(mMapWorld, (uint32_t)result.provenance.mapChunkIndex) : -1;
	const int32_t portalGraphIndex = nri_scene::FindMapWorldPortalIndex(mMapWorld, result.provenance);
	bool chunkResidentStatic = false;
	bool chunkStaticTlasInstanced = false;
	bool chunkStaticProbeIncluded = false;
	bool chunkVisibleGate = false;
	bool flatPlaneVisibilityRelevant = false;
	bool flatPlaneVisible = false;
	bool chunkReplaced = false;
	bool chunkSectorDirty = false;
	bool chunkDragged = false;
	bool chunkBlindSpot = false;
	uint32_t chunkReasonMask = 0;
	uint32_t chunkSectionDirtyCount = 0;
	uint32_t replacementSurfaceCount = 0;
	uint32_t replacementTriangleCount = 0;
	if (result.provenance.mapChunkIndex >= 0)
	{
		const uint32_t chunkIndex = (uint32_t)result.provenance.mapChunkIndex;
		chunkVisibleGate = IsChunkMarkedVisible(mCurrentVisibleChunkWords, chunkIndex);
		for (const auto& chunkCache : mStaticMapScene.chunks)
		{
			if (chunkCache.chunkIndex == chunkIndex)
			{
				chunkResidentStatic = true;
				chunkStaticTlasInstanced =
					!mSurfaceProbeFrame.staticTlasExcludesReplacedChunks ||
					chunkIndex >= mRuntimeMapMutations.replacedChunkMask.size() ||
					mRuntimeMapMutations.replacedChunkMask[chunkIndex] == 0;
				chunkStaticProbeIncluded =
					!mSurfaceProbeFrame.staticProbeExcludesReplacedChunks ||
					chunkIndex >= mRuntimeMapMutations.replacedChunkMask.size() ||
					mRuntimeMapMutations.replacedChunkMask[chunkIndex] == 0;
				break;
			}
		}
		if (chunkIndex < mRuntimeMapMutations.chunks.size())
		{
			const auto& replacement = mRuntimeMapMutations.chunks[chunkIndex];
			chunkReplaced = replacement.active;
			chunkSectorDirty = replacement.sectorDirty;
			chunkDragged = replacement.dragged;
			chunkBlindSpot = replacement.blindSpot;
			chunkReasonMask = replacement.reasonMask;
			chunkSectionDirtyCount = replacement.sectionDirtyCount;
			replacementSurfaceCount = replacement.surfaceCount;
			replacementTriangleCount = replacement.triangleCount;
		}
	}
	if ((flags & nri_scene::MaterialFlag_Flat) != 0 &&
		(flags & (nri_scene::MaterialFlag_Sprite | nri_scene::MaterialFlag_Mirror | nri_scene::MaterialFlag_Sky | nri_scene::MaterialFlag_Portal)) == 0 &&
		result.provenance.sectorIndex >= 0)
	{
		flatPlaneVisibilityRelevant = true;
		flatPlaneVisible = IsFlatPlaneMarkedVisible(mCurrentVisibleFlatPlaneWords, result.provenance.sectorIndex, result.normal[1] < 0.0f);
	}
	const std::string chunkReasons = GetRuntimeMapMutationReasonSummary(chunkReasonMask);
	FString textureName;
	int32_t legacyTile = -1;
	ResolveSurfaceProbeTextureDebugInfo(result.textureId, textureName, legacyTile);
	Printf("NRI PT surface probe: hit source=%s drawlist=%s owner=%s data_source=%s chunk=%d gate_visible=%s flat_drawlist_visible=%s static_resident=%s static_tlas_instanced=%s static_probe_included=%s chunk_replaced=%s chunk_reasons=%s section_dirty=%u sector_dirty=%s dragged=%s blind_spot=%s replacement_surfaces=%u replacement_tris=%u local_space=%d portal_graph=%d sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x primitive=%u material=%u texid=%u legacy_tile=%d texture_name=%s distance=%.2f pos=(%.2f, %.2f, %.2f) normal=(%.3f, %.3f, %.3f) flags=0x%x indexed=%s fullbright=%s flat=%s sprite=%s mirror=%s sky=%s portal=%s facing_billboard=%s tex_fullbright=%s glowing=%s auto_glow=%s glowmap=%s normalmap=%s metallic=%s roughness=%s normal_tex=%u metallic_tex=%u roughness_tex=%u metalness_hint=%.3f roughness_hint=%.3f material_class=%u emissive_mode=%s emissive_tex=%u light_surface=%s light_mat=%u emissive_surface=%s emissive_prims=%u light=%.3f alpha=%.3f avg=(%.2f, %.2f, %.2f) emissive=(%.2f, %.2f, %.2f) glow=(%.2f, %.2f, %.2f)\n",
		GetSurfaceSourceTypeName(result.provenance.sourceType),
		GetDrawListTypeName(result.provenance.drawListType),
		GetSurfaceProbeSceneOwnerName(result.sceneOwner),
		GetSceneDataSourceName(result.sceneDataSource),
		result.provenance.mapChunkIndex,
		YesNo(chunkVisibleGate),
		flatPlaneVisibilityRelevant ? YesNo(flatPlaneVisible) : "n/a",
		YesNo(chunkResidentStatic),
		YesNo(chunkStaticTlasInstanced),
		YesNo(chunkStaticProbeIncluded),
		YesNo(chunkReplaced),
		chunkReasons.c_str(),
		chunkSectionDirtyCount,
		YesNo(chunkSectorDirty),
		YesNo(chunkDragged),
		YesNo(chunkBlindSpot),
		replacementSurfaceCount,
		replacementTriangleCount,
		localSpaceIndex,
		portalGraphIndex,
		result.provenance.sectorIndex,
		result.provenance.wallIndex,
		result.provenance.nextSectorIndex,
		result.provenance.actorIndex,
		result.provenance.cstat,
		result.primitiveIndex,
		result.materialIndex,
		result.textureId,
		legacyTile,
		textureName.GetChars(),
		result.distance,
		result.position[0], result.position[1], result.position[2],
		result.normal[0], result.normal[1], result.normal[2],
		flags,
		YesNo((flags & nri_scene::MaterialFlag_Indexed) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Fullbright) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Flat) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Sprite) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Mirror) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Sky) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Portal) != 0),
		YesNo((flags & nri_scene::MaterialFlag_FacingBillboard) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureFullbright) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureGlowing) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureAutoGlowing) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_HasGlowmap) != 0),
		YesNo(result.normalTextureIndex != UINT32_MAX),
		YesNo(result.metallicTextureIndex != UINT32_MAX),
		YesNo(result.roughnessTextureIndex != UINT32_MAX),
		result.normalTextureIndex != UINT32_MAX ? result.normalTextureIndex : 0u,
		result.metallicTextureIndex != UINT32_MAX ? result.metallicTextureIndex : 0u,
		result.roughnessTextureIndex != UINT32_MAX ? result.roughnessTextureIndex : 0u,
		result.metalnessHint,
		result.roughnessHint,
		result.materialClass,
		GetMaterialEmissiveModeName(result.emissiveMode),
		result.emissiveTextureIndex != UINT32_MAX ? result.emissiveTextureIndex : 0u,
		YesNo(emissiveDiagnostics.sceneLightSurfaceMatch),
		emissiveDiagnostics.sceneLightMaterialIndex != UINT32_MAX ? emissiveDiagnostics.sceneLightMaterialIndex : 0u,
		YesNo(emissiveDiagnostics.activeEmissiveSurfaceMatch),
		emissiveDiagnostics.emissivePrimitiveMatchCount,
		result.lightLevel,
		result.alpha,
		result.averageColor[0], result.averageColor[1], result.averageColor[2],
		result.emissiveColor[0], result.emissiveColor[1], result.emissiveColor[2],
		result.glowColor[0], result.glowColor[1], result.glowColor[2]);
	mLastLoggedSurfaceProbe = result;
}

NRIRenderer::SurfaceProbeEmissiveDiagnostics NRIRenderer::BuildSurfaceProbeEmissiveDiagnostics(const SurfaceProbeResult& probe) const
{
	SurfaceProbeEmissiveDiagnostics diagnostics = {};
	if (!probe.valid || !probe.hit)
	{
		return diagnostics;
	}

	const auto provenanceMatches = [](const nri_scene::SurfaceProvenance& a, const nri_scene::SurfaceProvenance& b)
	{
		return
			a.sourceType == b.sourceType &&
			a.drawListType == b.drawListType &&
			a.mapChunkIndex == b.mapChunkIndex &&
			a.sectionIndex == b.sectionIndex &&
			a.sectorIndex == b.sectorIndex &&
			a.wallIndex == b.wallIndex &&
			a.nextSectorIndex == b.nextSectorIndex &&
			a.actorIndex == b.actorIndex &&
			a.cstat == b.cstat;
	};

	const auto mapOwnerToLightSource = [](uint32_t owner) -> SceneLightRecordSource
	{
		switch (owner)
		{
		case NRI_SURFACE_PROBE_OWNER_STATIC_MAP: return SceneLightRecordSource::StaticMapScene;
		case NRI_SURFACE_PROBE_OWNER_CAPTURED_SCENE: return SceneLightRecordSource::CapturedScene;
		case NRI_SURFACE_PROBE_OWNER_RUNTIME_MUTATION: return SceneLightRecordSource::RuntimeMutationScene;
		case NRI_SURFACE_PROBE_OWNER_DYNAMIC_OVERLAY: return SceneLightRecordSource::DynamicScene;
		default: return SceneLightRecordSource::CapturedScene;
		}
	};

	const SceneLightRecordSource expectedSource = mapOwnerToLightSource(probe.sceneOwner);
	const SceneLightSystem::SurfaceRecord* matchedSurface = nullptr;
	for (const auto& record : mSceneLights.GetSurfaceRecords())
	{
		if (!provenanceMatches(record.provenance, probe.provenance))
		{
			continue;
		}
		if (record.material.textureId != probe.textureId)
		{
			continue;
		}
		if (record.source == expectedSource)
		{
			matchedSurface = &record;
			break;
		}
		if (matchedSurface == nullptr)
		{
			matchedSurface = &record;
		}
	}

	if (matchedSurface == nullptr)
	{
		return diagnostics;
	}

	diagnostics.sceneLightSurfaceMatch = true;
	diagnostics.sceneLightMaterialIndex = matchedSurface->materialIndex;

	for (const auto& surface : mSceneLights.GetEmissiveSurfaces().activeSurfaces)
	{
		if (surface.source == matchedSurface->source &&
			surface.materialIndex == matchedSurface->materialIndex &&
			surface.textureId == matchedSurface->material.textureId &&
			surface.actorIndex == matchedSurface->provenance.actorIndex)
		{
			diagnostics.activeEmissiveSurfaceMatch = true;
			break;
		}
	}

	const uint32_t expectedDataSource =
		matchedSurface->source == SceneLightRecordSource::StaticMapScene ?
			NRI_SCENE_DATA_SOURCE_STATIC :
			NRI_SCENE_DATA_SOURCE_DYNAMIC;
	for (const auto& record : mBoundEmissivePrimitiveRecords)
	{
		if (record.dataSource == expectedDataSource &&
			record.materialIndex == matchedSurface->materialIndex &&
			record.textureId == matchedSurface->material.textureId &&
			record.actorIndex == matchedSurface->provenance.actorIndex)
		{
			diagnostics.emissivePrimitiveMatchCount++;
		}
	}

	return diagnostics;
}

void NRIRenderer::PrintSurfaceProbeStatus() const
{
	if (!mLastSurfaceProbe.valid)
	{
		Printf("NRI PT surface probe: no sampled center hit has been recorded yet.\n");
		return;
	}

	if (!mLastSurfaceProbe.hit)
	{
		Printf("NRI PT surface probe: last sampled center ray missed translated PT geometry.\n");
		return;
	}

	const SurfaceProbeEmissiveDiagnostics emissiveDiagnostics = BuildSurfaceProbeEmissiveDiagnostics(mLastSurfaceProbe);
	const uint32_t flags = mLastSurfaceProbe.primitiveFlags;
	const uint32_t lightingFlags = mLastSurfaceProbe.materialLightingFlags;
	const int32_t localSpaceIndex = mLastSurfaceProbe.provenance.mapChunkIndex >= 0 ? nri_scene::FindMapWorldLocalSpaceIndex(mMapWorld, (uint32_t)mLastSurfaceProbe.provenance.mapChunkIndex) : -1;
	const int32_t portalGraphIndex = nri_scene::FindMapWorldPortalIndex(mMapWorld, mLastSurfaceProbe.provenance);
	bool chunkResidentStatic = false;
	bool chunkStaticTlasInstanced = false;
	bool chunkStaticProbeIncluded = false;
	bool chunkVisibleGate = false;
	bool flatPlaneVisibilityRelevant = false;
	bool flatPlaneVisible = false;
	bool chunkReplaced = false;
	bool chunkSectorDirty = false;
	bool chunkDragged = false;
	bool chunkBlindSpot = false;
	uint32_t chunkReasonMask = 0;
	uint32_t chunkSectionDirtyCount = 0;
	uint32_t replacementSurfaceCount = 0;
	uint32_t replacementTriangleCount = 0;
	if (mLastSurfaceProbe.provenance.mapChunkIndex >= 0)
	{
		const uint32_t chunkIndex = (uint32_t)mLastSurfaceProbe.provenance.mapChunkIndex;
		chunkVisibleGate = IsChunkMarkedVisible(mCurrentVisibleChunkWords, chunkIndex);
		for (const auto& chunkCache : mStaticMapScene.chunks)
		{
			if (chunkCache.chunkIndex == chunkIndex)
			{
				chunkResidentStatic = true;
				chunkStaticTlasInstanced =
					!mSurfaceProbeFrame.staticTlasExcludesReplacedChunks ||
					chunkIndex >= mRuntimeMapMutations.replacedChunkMask.size() ||
					mRuntimeMapMutations.replacedChunkMask[chunkIndex] == 0;
				chunkStaticProbeIncluded =
					!mSurfaceProbeFrame.staticProbeExcludesReplacedChunks ||
					chunkIndex >= mRuntimeMapMutations.replacedChunkMask.size() ||
					mRuntimeMapMutations.replacedChunkMask[chunkIndex] == 0;
				break;
			}
		}
		if (chunkIndex < mRuntimeMapMutations.chunks.size())
		{
			const auto& replacement = mRuntimeMapMutations.chunks[chunkIndex];
			chunkReplaced = replacement.active;
			chunkSectorDirty = replacement.sectorDirty;
			chunkDragged = replacement.dragged;
			chunkBlindSpot = replacement.blindSpot;
			chunkReasonMask = replacement.reasonMask;
			chunkSectionDirtyCount = replacement.sectionDirtyCount;
			replacementSurfaceCount = replacement.surfaceCount;
			replacementTriangleCount = replacement.triangleCount;
		}
	}
	if ((flags & nri_scene::MaterialFlag_Flat) != 0 &&
		(flags & (nri_scene::MaterialFlag_Sprite | nri_scene::MaterialFlag_Mirror | nri_scene::MaterialFlag_Sky | nri_scene::MaterialFlag_Portal)) == 0 &&
		mLastSurfaceProbe.provenance.sectorIndex >= 0)
	{
		flatPlaneVisibilityRelevant = true;
		flatPlaneVisible = IsFlatPlaneMarkedVisible(mCurrentVisibleFlatPlaneWords, mLastSurfaceProbe.provenance.sectorIndex, mLastSurfaceProbe.normal[1] < 0.0f);
	}
	const std::string chunkReasons = GetRuntimeMapMutationReasonSummary(chunkReasonMask);
	Printf("NRI PT surface probe: source=%s drawlist=%s owner=%s data_source=%s chunk=%d gate_visible=%s flat_drawlist_visible=%s static_resident=%s static_tlas_instanced=%s static_probe_included=%s chunk_replaced=%s chunk_reasons=%s section_dirty=%u sector_dirty=%s dragged=%s blind_spot=%s replacement_surfaces=%u replacement_tris=%u local_space=%d portal_graph=%d sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x primitive=%u material=%u tile=%u distance=%.2f pos=(%.2f, %.2f, %.2f) flags=0x%x indexed=%s fullbright=%s flat=%s sprite=%s mirror=%s sky=%s portal=%s facing_billboard=%s tex_fullbright=%s glowing=%s auto_glow=%s glowmap=%s normalmap=%s metallic=%s roughness=%s normal_tex=%u metallic_tex=%u roughness_tex=%u metalness_hint=%.3f roughness_hint=%.3f material_class=%u emissive_mode=%s emissive_tex=%u light_surface=%s light_mat=%u emissive_surface=%s emissive_prims=%u light=%.3f alpha=%.3f avg=(%.2f, %.2f, %.2f) emissive=(%.2f, %.2f, %.2f) glow=(%.2f, %.2f, %.2f)\n",
		GetSurfaceSourceTypeName(mLastSurfaceProbe.provenance.sourceType),
		GetDrawListTypeName(mLastSurfaceProbe.provenance.drawListType),
		GetSurfaceProbeSceneOwnerName(mLastSurfaceProbe.sceneOwner),
		GetSceneDataSourceName(mLastSurfaceProbe.sceneDataSource),
		mLastSurfaceProbe.provenance.mapChunkIndex,
		YesNo(chunkVisibleGate),
		flatPlaneVisibilityRelevant ? YesNo(flatPlaneVisible) : "n/a",
		YesNo(chunkResidentStatic),
		YesNo(chunkStaticTlasInstanced),
		YesNo(chunkStaticProbeIncluded),
		YesNo(chunkReplaced),
		chunkReasons.c_str(),
		chunkSectionDirtyCount,
		YesNo(chunkSectorDirty),
		YesNo(chunkDragged),
		YesNo(chunkBlindSpot),
		replacementSurfaceCount,
		replacementTriangleCount,
		localSpaceIndex,
		portalGraphIndex,
		mLastSurfaceProbe.provenance.sectorIndex,
		mLastSurfaceProbe.provenance.wallIndex,
		mLastSurfaceProbe.provenance.nextSectorIndex,
		mLastSurfaceProbe.provenance.actorIndex,
		mLastSurfaceProbe.provenance.cstat,
		mLastSurfaceProbe.primitiveIndex,
		mLastSurfaceProbe.materialIndex,
		mLastSurfaceProbe.textureId,
		mLastSurfaceProbe.distance,
		mLastSurfaceProbe.position[0],
		mLastSurfaceProbe.position[1],
		mLastSurfaceProbe.position[2],
		flags,
		YesNo((flags & nri_scene::MaterialFlag_Indexed) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Fullbright) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Flat) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Sprite) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Mirror) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Sky) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Portal) != 0),
		YesNo((flags & nri_scene::MaterialFlag_FacingBillboard) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureFullbright) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureGlowing) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureAutoGlowing) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_HasGlowmap) != 0),
		YesNo(mLastSurfaceProbe.normalTextureIndex != UINT32_MAX),
		YesNo(mLastSurfaceProbe.metallicTextureIndex != UINT32_MAX),
		YesNo(mLastSurfaceProbe.roughnessTextureIndex != UINT32_MAX),
		mLastSurfaceProbe.normalTextureIndex != UINT32_MAX ? mLastSurfaceProbe.normalTextureIndex : 0u,
		mLastSurfaceProbe.metallicTextureIndex != UINT32_MAX ? mLastSurfaceProbe.metallicTextureIndex : 0u,
		mLastSurfaceProbe.roughnessTextureIndex != UINT32_MAX ? mLastSurfaceProbe.roughnessTextureIndex : 0u,
		mLastSurfaceProbe.metalnessHint,
		mLastSurfaceProbe.roughnessHint,
		mLastSurfaceProbe.materialClass,
		GetMaterialEmissiveModeName(mLastSurfaceProbe.emissiveMode),
		mLastSurfaceProbe.emissiveTextureIndex != UINT32_MAX ? mLastSurfaceProbe.emissiveTextureIndex : 0u,
		YesNo(emissiveDiagnostics.sceneLightSurfaceMatch),
		emissiveDiagnostics.sceneLightMaterialIndex != UINT32_MAX ? emissiveDiagnostics.sceneLightMaterialIndex : 0u,
		YesNo(emissiveDiagnostics.activeEmissiveSurfaceMatch),
		emissiveDiagnostics.emissivePrimitiveMatchCount,
		mLastSurfaceProbe.lightLevel,
		mLastSurfaceProbe.alpha,
		mLastSurfaceProbe.averageColor[0],
		mLastSurfaceProbe.averageColor[1],
		mLastSurfaceProbe.averageColor[2],
		mLastSurfaceProbe.emissiveColor[0],
		mLastSurfaceProbe.emissiveColor[1],
		mLastSurfaceProbe.emissiveColor[2],
		mLastSurfaceProbe.glowColor[0],
		mLastSurfaceProbe.glowColor[1],
		mLastSurfaceProbe.glowColor[2]);
}

void NRIRenderer::PrintMapChunkDump(int32_t chunkIndex) const
{
	if (!mMapWorld.valid)
	{
		Printf("NRI PT chunk dump: no authoritative map world has been built yet.\n");
		return;
	}

	if (chunkIndex < 0)
	{
		if (mLastSurfaceProbe.valid && mLastSurfaceProbe.hit && mLastSurfaceProbe.provenance.mapChunkIndex >= 0)
		{
			chunkIndex = mLastSurfaceProbe.provenance.mapChunkIndex;
		}
		else
		{
			Printf("NRI PT chunk dump: no chunk was specified and the last surface probe hit did not resolve to a map chunk.\n");
			return;
		}
	}

	if (chunkIndex < 0 || (unsigned)chunkIndex >= mMapWorld.chunks.size())
	{
		Printf("NRI PT chunk dump: chunk %d is out of range [0,%u).\n", chunkIndex, (uint32_t)mMapWorld.chunks.size());
		return;
	}

	const auto& chunk = mMapWorld.chunks[(unsigned)chunkIndex];
	const auto staticChunkIt = std::find_if(
		mStaticMapScene.chunks.begin(),
		mStaticMapScene.chunks.end(),
		[chunkIndex](const StaticMapSceneCache::ChunkCache& cache) { return cache.chunkIndex == (uint32_t)chunkIndex; });
	const bool residentStatic = staticChunkIt != mStaticMapScene.chunks.end();
	const bool staticTlasInstanced =
		residentStatic &&
		(!mSurfaceProbeFrame.staticTlasExcludesReplacedChunks ||
		 (unsigned)chunkIndex >= mRuntimeMapMutations.replacedChunkMask.size() ||
		 mRuntimeMapMutations.replacedChunkMask[(unsigned)chunkIndex] == 0);
	const bool staticProbeIncluded =
		residentStatic &&
		(!mSurfaceProbeFrame.staticProbeExcludesReplacedChunks ||
		 (unsigned)chunkIndex >= mRuntimeMapMutations.replacedChunkMask.size() ||
		 mRuntimeMapMutations.replacedChunkMask[(unsigned)chunkIndex] == 0);
	const auto* replacement =
		(unsigned)chunkIndex < mRuntimeMapMutations.chunks.size() ?
		&mRuntimeMapMutations.chunks[(unsigned)chunkIndex] :
		nullptr;

	uint32_t portalSurfaceCount = 0;
	uint32_t skySurfaceCount = 0;
	uint32_t surfaceTriangleCount = 0;
	for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < chunk.surfaceCount; ++localSurfaceIndex)
	{
		const uint32_t surfaceIndex = chunk.firstSurface + localSurfaceIndex;
		if (surfaceIndex >= mMapWorld.surfaces.size())
		{
			break;
		}

		const auto& surface = mMapWorld.surfaces[surfaceIndex];
		surfaceTriangleCount += CountSurfaceTriangles(surface.surface);
		if ((surface.surface.material.flags & (nri_scene::MaterialFlag_Portal | nri_scene::MaterialFlag_Mirror)) != 0)
		{
			portalSurfaceCount++;
		}
		if ((surface.surface.material.flags & nri_scene::MaterialFlag_Sky) != 0)
		{
			skySurfaceCount++;
		}
	}

	uint32_t sourcePortalCount = 0;
	for (const auto& portal : mMapWorld.portals)
	{
		if (portal.sourceChunkIndex == (uint32_t)chunkIndex)
		{
			sourcePortalCount++;
		}
	}

	Printf("NRI PT chunk dump: chunk=%d sector=%d local_space=%u surfaces=%u tris=%u portal_surfaces=%u sky_surfaces=%u source_portals=%u resident_static=%s static_tlas_instanced=%s static_probe_included=%s runtime_replaced=%s replacement_reasons=%s section_dirty=%u sector_dirty=%s dragged=%s blind_spot=%s replacement_surfaces=%u replacement_tris=%u\n",
		chunkIndex,
		chunk.sectorIndex,
		chunk.localSpaceIndex,
		chunk.surfaceCount,
		surfaceTriangleCount,
		portalSurfaceCount,
		skySurfaceCount,
		sourcePortalCount,
		YesNo(residentStatic),
		YesNo(staticTlasInstanced),
		YesNo(staticProbeIncluded),
		YesNo(replacement != nullptr && replacement->active),
		replacement != nullptr ? GetRuntimeMapMutationReasonSummary(replacement->reasonMask).c_str() : "none",
		replacement != nullptr ? replacement->sectionDirtyCount : 0u,
		YesNo(replacement != nullptr && replacement->sectorDirty),
		YesNo(replacement != nullptr && replacement->dragged),
		YesNo(replacement != nullptr && replacement->blindSpot),
		replacement != nullptr ? replacement->surfaceCount : 0u,
		replacement != nullptr ? replacement->triangleCount : 0u);

	if (residentStatic)
	{
		Printf("NRI PT chunk dump static: primitive_offset=%u primitive_count=%u material_offset=%u material_count=%u as_ready=%s\n",
			staticChunkIt->primitiveOffset,
			staticChunkIt->primitiveCount,
			staticChunkIt->materialOffset,
			staticChunkIt->materialCount,
			YesNo(staticChunkIt->accelerationStructure.accelerationStructure != nullptr));
	}

	for (const auto& portal : mMapWorld.portals)
	{
		if (portal.sourceChunkIndex != (uint32_t)chunkIndex)
		{
			continue;
		}

		Printf("NRI PT chunk portal: portal=%u source_surface=%u source_sector=%d source_wall=%d source_plane=%d target_count=%u runtime_bound=%s delta=(%.2f, %.2f, %.2f)\n",
			portal.portalIndex,
			portal.sourceSurfaceIndex,
			portal.sourceSectorIndex,
			portal.sourceWallIndex,
			portal.sourcePlane,
			portal.targetCount,
			YesNo(portal.runtimeBoundTarget),
			(float)portal.delta[0],
			(float)portal.delta[1],
			(float)portal.delta[2]);
	}

	for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < chunk.surfaceCount; ++localSurfaceIndex)
	{
		const uint32_t surfaceIndex = chunk.firstSurface + localSurfaceIndex;
		if (surfaceIndex >= mMapWorld.surfaces.size())
		{
			break;
		}

		const auto& surface = mMapWorld.surfaces[surfaceIndex];
		const uint32_t flags = surface.surface.material.flags;
		const uint32_t textureId =
			surface.surface.material.texture != nullptr ?
			(uint32_t)surface.surface.material.texture->GetID().GetIndex() :
			0u;
		Printf("NRI PT chunk surface %u: kind=%s source=%s section=%d sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x flags=0x%x flat=%s sprite=%s mirror=%s sky=%s portal=%s one_way=%s facing_billboard=%s tile=%u pal=%d shade=%d alpha=%.3f verts=%u tris=%u\n",
			surfaceIndex,
			GetMapSurfaceKindName(surface.kind),
			GetSurfaceSourceTypeName(surface.surface.provenance.sourceType),
			surface.surface.provenance.sectionIndex,
			surface.surface.provenance.sectorIndex,
			surface.surface.provenance.wallIndex,
			surface.surface.provenance.nextSectorIndex,
			surface.surface.provenance.actorIndex,
			surface.surface.provenance.cstat,
			flags,
			YesNo((flags & nri_scene::MaterialFlag_Flat) != 0),
			YesNo((flags & nri_scene::MaterialFlag_Sprite) != 0),
			YesNo((flags & nri_scene::MaterialFlag_Mirror) != 0),
			YesNo((flags & nri_scene::MaterialFlag_Sky) != 0),
			YesNo((flags & nri_scene::MaterialFlag_Portal) != 0),
			YesNo((flags & nri_scene::MaterialFlag_OneWay) != 0),
			YesNo((flags & nri_scene::MaterialFlag_FacingBillboard) != 0),
			textureId,
			surface.surface.material.palette,
			surface.surface.material.shade,
			surface.surface.material.alpha,
			(uint32_t)surface.surface.vertices.size(),
			CountSurfaceTriangles(surface.surface));
	}
}

void NRIRenderer::PrintMapChunkCompare(int32_t chunkIndex) const
{
	if (!mMapWorld.valid)
	{
		Printf("NRI PT chunk compare: no authoritative map world has been built yet.\n");
		return;
	}

	if (chunkIndex < 0)
	{
		if (mLastSurfaceProbe.valid && mLastSurfaceProbe.hit && mLastSurfaceProbe.provenance.mapChunkIndex >= 0)
		{
			chunkIndex = mLastSurfaceProbe.provenance.mapChunkIndex;
		}
		else
		{
			Printf("NRI PT chunk compare: no chunk was specified and the last surface probe hit did not resolve to a map chunk.\n");
			return;
		}
	}

	if (chunkIndex < 0 || (unsigned)chunkIndex >= mMapWorld.chunks.size())
	{
		Printf("NRI PT chunk compare: chunk %d is out of range [0,%u).\n", chunkIndex, (uint32_t)mMapWorld.chunks.size());
		return;
	}

	const auto& staticChunk = mMapWorld.chunks[(unsigned)chunkIndex];
	nri_scene::PTMapWorld liveWorld = {};
	nri_scene::PTMapWorldStats liveStats = {};
	if (!nri_scene::BuildLiveMapChunkWorld(staticChunk, liveWorld, &liveStats) ||
		liveWorld.chunks.empty())
	{
		Printf("NRI PT chunk compare: failed to build live runtime chunk %d.\n", chunkIndex);
		return;
	}

	const auto& liveChunk = liveWorld.chunks[0];
	const auto* replacement =
		(unsigned)chunkIndex < mRuntimeMapMutations.chunks.size() ?
		&mRuntimeMapMutations.chunks[(unsigned)chunkIndex] :
		nullptr;

	std::vector<uint32_t> staticSurfaceIndices;
	std::vector<uint32_t> liveSurfaceIndices;
	staticSurfaceIndices.reserve(staticChunk.surfaceCount);
	liveSurfaceIndices.reserve(liveChunk.surfaceCount);

	for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < staticChunk.surfaceCount; ++localSurfaceIndex)
	{
		const uint32_t surfaceIndex = staticChunk.firstSurface + localSurfaceIndex;
		if (surfaceIndex >= mMapWorld.surfaces.size())
		{
			break;
		}
		staticSurfaceIndices.push_back(surfaceIndex);
	}

	for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < liveChunk.surfaceCount; ++localSurfaceIndex)
	{
		const uint32_t surfaceIndex = liveChunk.firstSurface + localSurfaceIndex;
		if (surfaceIndex >= liveWorld.surfaces.size())
		{
			break;
		}
		liveSurfaceIndices.push_back(surfaceIndex);
	}

	std::unordered_map<ChunkCompareSurfaceKey, std::vector<uint32_t>, ChunkCompareSurfaceKeyHash> liveSurfaceLookup;
	liveSurfaceLookup.reserve(liveSurfaceIndices.size());
	for (uint32_t liveLocalIndex = 0; liveLocalIndex < (uint32_t)liveSurfaceIndices.size(); ++liveLocalIndex)
	{
		const auto& liveSurface = liveWorld.surfaces[liveSurfaceIndices[liveLocalIndex]];
		liveSurfaceLookup[BuildChunkCompareSurfaceKey(liveSurface)].push_back(liveLocalIndex);
	}

	std::vector<uint8_t> liveSurfaceUsed(liveSurfaceIndices.size(), 0u);
	std::vector<ChunkCompareMatchRecord> matches;
	std::vector<uint32_t> unmatchedStaticSurfaceIndices;
	std::vector<uint32_t> unmatchedLiveSurfaceIndices;
	matches.reserve(std::min(staticSurfaceIndices.size(), liveSurfaceIndices.size()));
	unmatchedStaticSurfaceIndices.reserve(staticSurfaceIndices.size());
	unmatchedLiveSurfaceIndices.reserve(liveSurfaceIndices.size());

	for (uint32_t staticSurfaceIndex : staticSurfaceIndices)
	{
		const auto& staticSurface = mMapWorld.surfaces[staticSurfaceIndex];
		const ChunkCompareSurfaceKey key = BuildChunkCompareSurfaceKey(staticSurface);
		auto it = liveSurfaceLookup.find(key);
		if (it == liveSurfaceLookup.end())
		{
			unmatchedStaticSurfaceIndices.push_back(staticSurfaceIndex);
			continue;
		}

		uint32_t matchedLiveLocalIndex = UINT32_MAX;
		for (uint32_t candidate : it->second)
		{
			if (candidate < liveSurfaceUsed.size() && liveSurfaceUsed[candidate] == 0u)
			{
				matchedLiveLocalIndex = candidate;
				break;
			}
		}
		if (matchedLiveLocalIndex == UINT32_MAX)
		{
			unmatchedStaticSurfaceIndices.push_back(staticSurfaceIndex);
			continue;
		}

		liveSurfaceUsed[matchedLiveLocalIndex] = 1u;
		const uint32_t liveSurfaceIndex = liveSurfaceIndices[matchedLiveLocalIndex];
		const auto& liveSurface = liveWorld.surfaces[liveSurfaceIndex];

		ChunkCompareMatchRecord match = {};
		match.staticSurfaceIndex = staticSurfaceIndex;
		match.liveSurfaceIndex = liveSurfaceIndex;
		match.key = key;
		match.staticMetrics = ComputeChunkCompareSurfaceMetrics(staticSurface);
		match.liveMetrics = ComputeChunkCompareSurfaceMetrics(liveSurface);
		for (int axis = 0; axis < 3; ++axis)
		{
			match.delta[axis] = match.liveMetrics.centroid[axis] - match.staticMetrics.centroid[axis];
		}
		match.deltaDistance = Distance3(match.liveMetrics.centroid, match.staticMetrics.centroid);
		if (match.staticMetrics.area > 0.0001f)
		{
			match.areaRatio = match.liveMetrics.area / match.staticMetrics.area;
		}
		else
		{
			match.areaRatio = match.liveMetrics.area > 0.0001f ? 9999.0f : 1.0f;
		}

		const float staticNormalLength = std::sqrt(Dot3(match.staticMetrics.normal, match.staticMetrics.normal));
		const float liveNormalLength = std::sqrt(Dot3(match.liveMetrics.normal, match.liveMetrics.normal));
		if (staticNormalLength > 0.0001f && liveNormalLength > 0.0001f)
		{
			match.normalDot = std::max(-1.0f, std::min(1.0f, Dot3(match.staticMetrics.normal, match.liveMetrics.normal)));
		}
		else
		{
			match.normalDot = staticNormalLength <= 0.0001f && liveNormalLength <= 0.0001f ? 1.0f : 0.0f;
		}

		match.materialScore =
			(match.staticMetrics.textureId == match.liveMetrics.textureId ? 0.0f : 1.0f) +
			(match.staticMetrics.palette == match.liveMetrics.palette ? 0.0f : 1.0f) +
			(match.staticMetrics.shade == match.liveMetrics.shade ? 0.0f : 1.0f) +
			(match.staticMetrics.materialFlags == match.liveMetrics.materialFlags ? 0.0f : 1.0f) +
			(std::fabs(match.staticMetrics.alpha - match.liveMetrics.alpha) > 0.001f ? 1.0f : 0.0f);
		matches.push_back(match);
	}

	for (uint32_t liveLocalIndex = 0; liveLocalIndex < (uint32_t)liveSurfaceIndices.size(); ++liveLocalIndex)
	{
		if (liveSurfaceUsed[liveLocalIndex] == 0u)
		{
			unmatchedLiveSurfaceIndices.push_back(liveSurfaceIndices[liveLocalIndex]);
		}
	}

	float meanDelta[3] = {};
	for (const auto& match : matches)
	{
		meanDelta[0] += match.delta[0];
		meanDelta[1] += match.delta[1];
		meanDelta[2] += match.delta[2];
	}
	if (!matches.empty())
	{
		const float invMatchCount = 1.0f / (float)matches.size();
		meanDelta[0] *= invMatchCount;
		meanDelta[1] *= invMatchCount;
		meanDelta[2] *= invMatchCount;
	}

	std::unordered_map<int32_t, uint32_t> sectorChunkLookup;
	sectorChunkLookup.reserve(mMapWorld.chunks.size());
	for (const auto& mapChunk : mMapWorld.chunks)
	{
		if (mapChunk.sectorIndex >= 0)
		{
			sectorChunkLookup.emplace(mapChunk.sectorIndex, mapChunk.chunkIndex);
		}
	}

	uint32_t within1 = 0;
	uint32_t within4 = 0;
	uint32_t areaOutlierCount = 0;
	uint32_t normalOutlierCount = 0;
	uint32_t materialDiffCount = 0;
	uint32_t seamSurfaceCount = 0;
	uint32_t seamOutlierCount = 0;
	uint32_t seamAgainstStaticCount = 0;
	uint32_t seamAgainstReplacedCount = 0;
	for (auto& match : matches)
	{
		const float meanDeltaPoint[3] = { meanDelta[0], meanDelta[1], meanDelta[2] };
		match.deviationFromMean = Distance3(match.delta, meanDeltaPoint);
		const float areaDelta = std::fabs(match.areaRatio - 1.0f);
		if (match.deviationFromMean <= 1.0f)
		{
			within1++;
		}
		if (match.deviationFromMean <= 4.0f)
		{
			within4++;
		}
		if (areaDelta > 0.05f)
		{
			areaOutlierCount++;
		}
		if (match.normalDot < 0.98f)
		{
			normalOutlierCount++;
		}
		if (match.materialScore > 0.0f)
		{
			materialDiffCount++;
		}
		match.score = match.deviationFromMean + areaDelta * 10.0f + (1.0f - match.normalDot) * 10.0f + match.materialScore;

		const auto& staticSurface = mMapWorld.surfaces[match.staticSurfaceIndex];
		if (staticSurface.surface.provenance.nextSectorIndex >= 0 &&
			staticSurface.kind != nri_scene::PTMapSurfaceKind::Floor &&
			staticSurface.kind != nri_scene::PTMapSurfaceKind::Ceiling &&
			staticSurface.kind != nri_scene::PTMapSurfaceKind::Portal)
		{
			seamSurfaceCount++;
			auto adjacentChunkIt = sectorChunkLookup.find(staticSurface.surface.provenance.nextSectorIndex);
			const bool adjacentReplaced =
				adjacentChunkIt != sectorChunkLookup.end() &&
				adjacentChunkIt->second < mRuntimeMapMutations.chunks.size() &&
				mRuntimeMapMutations.chunks[adjacentChunkIt->second].active;
			if (adjacentReplaced)
			{
				seamAgainstReplacedCount++;
			}
			else
			{
				seamAgainstStaticCount++;
			}

			if (match.deviationFromMean > 0.5f)
			{
				seamOutlierCount++;
			}
		}
	}

	std::sort(matches.begin(), matches.end(), [](const ChunkCompareMatchRecord& a, const ChunkCompareMatchRecord& b)
	{
		return a.score > b.score;
	});

	const bool likelyCoherent =
		!matches.empty() &&
		unmatchedStaticSurfaceIndices.empty() &&
		unmatchedLiveSurfaceIndices.empty() &&
		within4 + std::max<uint32_t>(1u, (uint32_t)matches.size() / 10u) >= (uint32_t)matches.size() &&
		areaOutlierCount == 0 &&
		normalOutlierCount == 0;

	Printf("NRI PT chunk compare: chunk=%d sector=%d static_surfaces=%u live_surfaces=%u matched=%u unmatched_static=%u unmatched_live=%u reasons=%s dragged=%s replacement_active=%s mean_delta=(%.2f, %.2f, %.2f) within_1=%u within_4=%u area_outliers=%u normal_outliers=%u material_diffs=%u likely_coherent=%s live_tris=%u\n",
		chunkIndex,
		staticChunk.sectorIndex,
		(uint32_t)staticSurfaceIndices.size(),
		(uint32_t)liveSurfaceIndices.size(),
		(uint32_t)matches.size(),
		(uint32_t)unmatchedStaticSurfaceIndices.size(),
		(uint32_t)unmatchedLiveSurfaceIndices.size(),
		replacement != nullptr ? GetRuntimeMapMutationReasonSummary(replacement->reasonMask).c_str() : "none",
		YesNo(replacement != nullptr && replacement->dragged),
		YesNo(replacement != nullptr && replacement->active),
		meanDelta[0],
		meanDelta[1],
		meanDelta[2],
		within1,
		within4,
		areaOutlierCount,
		normalOutlierCount,
		materialDiffCount,
		YesNo(likelyCoherent),
		liveChunk.triangleCount);
	Printf("NRI PT chunk seam compare: chunk=%d border_surfaces=%u seam_outliers=%u adjacent_static=%u adjacent_replaced=%u\n",
		chunkIndex,
		seamSurfaceCount,
		seamOutlierCount,
		seamAgainstStaticCount,
		seamAgainstReplacedCount);

	const size_t outlierCount = std::min<size_t>(matches.size(), 8u);
	for (size_t i = 0; i < outlierCount; ++i)
	{
		const auto& match = matches[i];
		if (match.score <= 0.01f && likelyCoherent)
		{
			break;
		}

		const auto& staticSurface = mMapWorld.surfaces[match.staticSurfaceIndex];
		const auto& liveSurface = liveWorld.surfaces[match.liveSurfaceIndex];
		Printf("NRI PT chunk compare match: static_surface=%u live_surface=%u kind=%s source=%s sector=%d wall=%d section=%d nextsector=%d cstat=0x%x delta=(%.2f, %.2f, %.2f) dev=%.2f area_ratio=%.3f normal_dot=%.3f tile_static=%u tile_live=%u flags_static=0x%x flags_live=0x%x\n",
			match.staticSurfaceIndex,
			match.liveSurfaceIndex,
			GetMapSurfaceKindName(staticSurface.kind),
			GetSurfaceSourceTypeName(staticSurface.surface.provenance.sourceType),
			staticSurface.surface.provenance.sectorIndex,
			staticSurface.surface.provenance.wallIndex,
			staticSurface.surface.provenance.sectionIndex,
			staticSurface.surface.provenance.nextSectorIndex,
			staticSurface.surface.provenance.cstat,
			match.delta[0],
			match.delta[1],
			match.delta[2],
			match.deviationFromMean,
			match.areaRatio,
			match.normalDot,
			match.staticMetrics.textureId,
			match.liveMetrics.textureId,
			staticSurface.surface.material.flags,
			liveSurface.surface.material.flags);
	}

	size_t seamPrinted = 0;
	for (const auto& match : matches)
	{
		if (seamPrinted >= 8u)
		{
			break;
		}

		const auto& staticSurface = mMapWorld.surfaces[match.staticSurfaceIndex];
		if (staticSurface.surface.provenance.nextSectorIndex < 0 ||
			staticSurface.kind == nri_scene::PTMapSurfaceKind::Floor ||
			staticSurface.kind == nri_scene::PTMapSurfaceKind::Ceiling ||
			staticSurface.kind == nri_scene::PTMapSurfaceKind::Portal)
		{
			continue;
		}

		auto adjacentChunkIt = sectorChunkLookup.find(staticSurface.surface.provenance.nextSectorIndex);
		const int32_t adjacentChunkIndex = adjacentChunkIt != sectorChunkLookup.end() ? (int32_t)adjacentChunkIt->second : -1;
		const bool adjacentReplaced =
			adjacentChunkIndex >= 0 &&
			(unsigned)adjacentChunkIndex < mRuntimeMapMutations.chunks.size() &&
			mRuntimeMapMutations.chunks[(unsigned)adjacentChunkIndex].active;
		const bool seamOutlier = match.deviationFromMean > 0.5f;
		if (!seamOutlier && seamPrinted >= 4u)
		{
			continue;
		}

		Printf("NRI PT chunk seam match: static_surface=%u live_surface=%u kind=%s wall=%d nextsector=%d adjacent_chunk=%d adjacent_replaced=%s delta=(%.2f, %.2f, %.2f) dev=%.2f area_ratio=%.3f normal_dot=%.3f seam_outlier=%s\n",
			match.staticSurfaceIndex,
			match.liveSurfaceIndex,
			GetMapSurfaceKindName(staticSurface.kind),
			staticSurface.surface.provenance.wallIndex,
			staticSurface.surface.provenance.nextSectorIndex,
			adjacentChunkIndex,
			YesNo(adjacentReplaced),
			match.delta[0],
			match.delta[1],
			match.delta[2],
			match.deviationFromMean,
			match.areaRatio,
			match.normalDot,
			YesNo(seamOutlier));
		seamPrinted++;
	}

	const size_t unmatchedStaticCount = std::min<size_t>(unmatchedStaticSurfaceIndices.size(), 8u);
	for (size_t i = 0; i < unmatchedStaticCount; ++i)
	{
		const auto& surface = mMapWorld.surfaces[unmatchedStaticSurfaceIndices[i]];
		Printf("NRI PT chunk compare unmatched_static: surface=%u kind=%s source=%s sector=%d wall=%d section=%d nextsector=%d cstat=0x%x tile=%u flags=0x%x verts=%u tris=%u\n",
			unmatchedStaticSurfaceIndices[i],
			GetMapSurfaceKindName(surface.kind),
			GetSurfaceSourceTypeName(surface.surface.provenance.sourceType),
			surface.surface.provenance.sectorIndex,
			surface.surface.provenance.wallIndex,
			surface.surface.provenance.sectionIndex,
			surface.surface.provenance.nextSectorIndex,
			surface.surface.provenance.cstat,
			GetSurfaceTextureId(surface),
			surface.surface.material.flags,
			(uint32_t)surface.surface.vertices.size(),
			CountSurfaceTriangles(surface.surface));
	}

	const size_t unmatchedLiveCount = std::min<size_t>(unmatchedLiveSurfaceIndices.size(), 8u);
	for (size_t i = 0; i < unmatchedLiveCount; ++i)
	{
		const auto& surface = liveWorld.surfaces[unmatchedLiveSurfaceIndices[i]];
		Printf("NRI PT chunk compare unmatched_live: surface=%u kind=%s source=%s sector=%d wall=%d section=%d nextsector=%d cstat=0x%x tile=%u flags=0x%x verts=%u tris=%u\n",
			unmatchedLiveSurfaceIndices[i],
			GetMapSurfaceKindName(surface.kind),
			GetSurfaceSourceTypeName(surface.surface.provenance.sourceType),
			surface.surface.provenance.sectorIndex,
			surface.surface.provenance.wallIndex,
			surface.surface.provenance.sectionIndex,
			surface.surface.provenance.nextSectorIndex,
			surface.surface.provenance.cstat,
			GetSurfaceTextureId(surface),
			surface.surface.material.flags,
			(uint32_t)surface.surface.vertices.size(),
			CountSurfaceTriangles(surface.surface));
	}
}

void NRIRenderer::RefreshSceneLightSystem(
	bool usedStaticMapScene,
	const nri_scene::SceneView* capturedSceneView,
	const nri_scene::MaterialBridgeData* capturedMaterials,
	const nri_scene::SceneView* dynamicSceneView,
	const nri_scene::MaterialBridgeData* dynamicMaterials)
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneLightsMs);
	mSceneLights.BeginFrame(mFrameIndex);

	if (usedStaticMapScene && mStaticMapScene.valid)
	{
		const size_t chunkCount = std::min(mStaticMapScene.lightChunkViews.size(), mStaticMapScene.chunks.size());
		{
			ScopedPtPerfTimer appendTimer(mLastPerfShellTraceStats.sceneLightStaticAppendMs);
			for (size_t chunkListIndex = 0; chunkListIndex < chunkCount; ++chunkListIndex)
			{
				const auto& staticChunk = mStaticMapScene.chunks[chunkListIndex];
				const uint32_t mapChunkIndex = staticChunk.chunkIndex;
				const bool useRuntimeMutationReplacement =
					mapChunkIndex < mRuntimeMapMutations.chunks.size() &&
					mRuntimeMapMutations.chunks[mapChunkIndex].active &&
					mRuntimeMapMutations.chunks[mapChunkIndex].valid;
				if (useRuntimeMutationReplacement)
				{
					continue;
				}

				mSceneLights.AppendSceneView(
					mStaticMapScene.lightChunkViews[chunkListIndex],
					mStaticMapScene.materialBridge,
					SceneLightRecordSource::StaticMapScene,
					staticChunk.materialOffset,
					staticChunk.materialOffset);
			}
		}

		// Runtime mutation emissive records must follow the same full map chunk
		// order used by BuildRuntimeMapMutationOverlay so material indices line
		// up with the uploaded replacement geometry/material buffers.
		uint32_t runtimeMutationMaterialOffset = 0;
		{
			ScopedPtPerfTimer appendTimer(mLastPerfShellTraceStats.sceneLightRuntimeMutationAppendMs);
			for (const auto& replacement : mRuntimeMapMutations.chunks)
			{
				if (!replacement.active || !replacement.valid)
				{
					continue;
				}

				mSceneLights.AppendSceneView(
					replacement.sceneView,
					replacement.materialBridge,
					SceneLightRecordSource::RuntimeMutationScene,
					runtimeMutationMaterialOffset,
					0u,
					&replacement.lightIdentityOverrides);
				runtimeMutationMaterialOffset += (uint32_t)replacement.materialBridge.materials.size();
			}
		}
	}
	else if (capturedSceneView != nullptr && capturedMaterials != nullptr)
	{
		ScopedPtPerfTimer appendTimer(mLastPerfShellTraceStats.sceneLightCapturedAppendMs);
		mSceneLights.AppendSceneView(*capturedSceneView, *capturedMaterials, SceneLightRecordSource::CapturedScene);
	}

	if (dynamicSceneView != nullptr && dynamicMaterials != nullptr)
	{
		ScopedPtPerfTimer appendTimer(mLastPerfShellTraceStats.sceneLightDynamicAppendMs);
		mSceneLights.AppendSceneView(*dynamicSceneView, *dynamicMaterials, SceneLightRecordSource::DynamicScene);
	}

	const ResolvedLightOverlaySet& resolvedLightOverlays = GetResolvedLightOverlaySet();
	const bool resolvedGenerationChanged =
		resolvedLightOverlays.resolvedGeneration != mLastResolvedLightOverlayGeneration;
	const bool muzzleFlashLookupNeedsRefresh =
		resolvedGenerationChanged ||
		mResolvedMuzzleFlashRuleLookup.size() != (size_t)resolvedLightOverlays.muzzleFlashRules.Size();
	if (muzzleFlashLookupNeedsRefresh)
	{
		const bool hadPreviousGeneration = mLastResolvedLightOverlayGeneration != 0;
		if (resolvedGenerationChanged && hadPreviousGeneration)
		{
			ResetMuzzleFlashOverlayState("lightoverlay-resolve");
		}
		else if (resolvedLightOverlays.resolvedGeneration == 0)
		{
			ResetMuzzleFlashOverlayState("lightoverlay-empty");
		}

		RefreshResolvedMuzzleFlashRuleLookup(resolvedLightOverlays);
		mLastResolvedLightOverlayGeneration = resolvedLightOverlays.resolvedGeneration;
		Printf("NRI PT muzzle-flash rules: generation=%u count=%u ids=%s\n",
			resolvedLightOverlays.resolvedGeneration,
			(unsigned)mResolvedMuzzleFlashRuleLookup.size(),
			FormatMuzzleFlashRuleIdList(mResolvedMuzzleFlashRuleLookup).c_str());

		if (resolvedGenerationChanged && hadPreviousGeneration)
		{
			NoteLightHistoryChange("lightoverlay-resolve");
		}
	}

	const NRIDirectionalLightState nextDirectionalLightState = BuildDirectionalLightState(resolvedLightOverlays, nri_ptdirectionallight);
	const bool directionalLightStateChanged =
		!mHasDirectionalLightState ||
		nextDirectionalLightState.stateHash != mDirectionalLightState.stateHash;
	const bool hadDirectionalLightState = mHasDirectionalLightState;
	mDirectionalLightState = nextDirectionalLightState;
	mHasDirectionalLightState = true;
	std::unordered_map<int32_t, std::vector<SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule>> actorOverlayRules;
	std::vector<SceneLightSystem::AnalyticLightRegistry::MapOverlayRule> mapOverlayRules;
	BuildActorAnalyticOverlayRules(resolvedLightOverlays, actorOverlayRules);
	if (mMapWorld.valid)
	{
		BuildStaticMapAnalyticOverlayRules(resolvedLightOverlays, mMapWorld, mapOverlayRules);
	}
	const uint32_t gameplayLightTimeIndex = GetGameplayLightTimeIndex();
	const double currentTimeSeconds = GetCurrentGameplayTimeSeconds();
	RefreshTransientMuzzleFlashLights(currentTimeSeconds);

	{
		ScopedPtPerfTimer rebuildTimer(mLastPerfShellTraceStats.sceneLightAnalyticMs);
		mSceneLights.RebuildAnalyticLights(
			gameplayLightTimeIndex,
			mFrameIndex,
			NRI_MAX_RUNTIME_POINT_LIGHTS,
			actorOverlayRules.empty() ? nullptr : &actorOverlayRules,
			mapOverlayRules.empty() ? nullptr : &mapOverlayRules);
	}
	{
		ScopedPtPerfTimer rebuildTimer(mLastPerfShellTraceStats.sceneLightEmissiveMs);
		mSceneLights.RebuildEmissiveSurfaces(NRI_MAX_EMISSIVE_SURFACES);
	}
	{
		ScopedPtPerfTimer rebuildTimer(mLastPerfShellTraceStats.sceneLightSectorMs);
		mSceneLights.RebuildSectorLighting(gameplayLightTimeIndex, (uint32_t)sector.Size());
	}
	const auto& frameAppendStats = mSceneLights.GetFrameAppendStats();
	mLastPerfShellTraceStats.sceneLightSurfaceRecordCount = frameAppendStats.totalRecordCount;
	mLastPerfShellTraceStats.sceneLightStaticRecordCount = frameAppendStats.staticRecordCount;
	mLastPerfShellTraceStats.sceneLightRuntimeMutationRecordCount = frameAppendStats.runtimeMutationRecordCount;
	mLastPerfShellTraceStats.sceneLightDynamicRecordCount = frameAppendStats.dynamicRecordCount;
	mLastPerfShellTraceStats.sceneLightCapturedRecordCount = frameAppendStats.capturedRecordCount;
	if (hadDirectionalLightState && directionalLightStateChanged)
	{
		NoteLightHistoryChange("directional-light-change");
	}
	const bool analyticLightTopologyChanged = mSceneLights.ConsumeAnalyticLightTopologyChanged();
	const bool analyticLightPropertiesChanged = mSceneLights.ConsumeAnalyticLightPropertiesChanged();
	const bool emissiveSurfaceTopologyChanged = mSceneLights.ConsumeEmissiveSurfaceTopologyChanged();
	const bool emissiveSurfacePropertiesChanged = mSceneLights.ConsumeEmissiveSurfacePropertiesChanged();
	const bool emissiveMaterialBindingChanged = mSceneLights.ConsumeEmissiveMaterialBindingChanged();
	const bool emissiveMaterialPropertiesChanged = mSceneLights.ConsumeEmissiveMaterialPropertiesChanged();
	const bool sectorLightingTopologyChanged = mSceneLights.ConsumeSectorLightingTopologyChanged();

	if (analyticLightTopologyChanged)
	{
		mBoundRuntimeLightCount = 0;
		if (ShouldTraceSkyPerf())
		{
			const auto& analyticLights = mSceneLights.GetAnalyticLights();
			Printf("NRI PT light topology analytic: frame=%u added=%s removed=%s rebound=%s\n",
				mFrameIndex,
				FormatTopologyKeyList(analyticLights.addedTopologyKeys).c_str(),
				FormatTopologyKeyList(analyticLights.removedTopologyKeys).c_str(),
				FormatTopologyKeyList(analyticLights.reboundTopologyKeys).c_str());
		}
		NoteLightHistoryChange("analytic-light-topology");
	}
	if (emissiveSurfaceTopologyChanged)
	{
		if (ShouldTraceSkyPerf())
		{
			const auto& emissiveSurfaces = mSceneLights.GetEmissiveSurfaces();
			Printf("NRI PT light topology emissive: frame=%u added=%s removed=%s rebound=%s\n",
				mFrameIndex,
				FormatTopologyKeyList(emissiveSurfaces.addedTopologyKeys).c_str(),
				FormatTopologyKeyList(emissiveSurfaces.removedTopologyKeys).c_str(),
				FormatTopologyKeyList(emissiveSurfaces.reboundTopologyKeys).c_str());
		}
		NoteLightHistoryChange("emissive-surface-topology");
	}
	if (emissiveMaterialBindingChanged)
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.emissiveMaterialDirtyEvents++;
		}

		// Binding changes still require resident static materials to be refreshed,
		// but they no longer need to tear down the whole static scene cache.
		QueueStaticMapSceneLightingInvalidation();
	}
	if (sectorLightingTopologyChanged)
	{
		NoteLightHistoryChange("sector-light-topology");
	}

	(void)analyticLightPropertiesChanged;
	(void)emissiveSurfacePropertiesChanged;
	(void)emissiveMaterialPropertiesChanged;
}

void NRIRenderer::QueueStaticMapSceneLightingInvalidation()
{
	if (ShouldTraceSkyPerf())
	{
		gRendererSkyPerfTraceStats.lightingInvalidationRequests++;
	}
	mPendingStaticMapLightingInvalidation = true;
}

void NRIRenderer::ApplyEmissiveMaterialOverrides(const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& inOutGpuMaterials) const
{
	const uint32_t count = std::min<uint32_t>((uint32_t)inOutGpuMaterials.size(), (uint32_t)materials.lightMetadata.size());
	for (uint32_t materialIndex = 0; materialIndex < count; ++materialIndex)
	{
		mSceneLights.ApplyEmissiveMaterialSettings(materials.lightMetadata[materialIndex], inOutGpuMaterials[materialIndex]);
	}
}

void NRIRenderer::BuildMaterialsWithActorOverrides(nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& outMaterials, const char* traceLabel)
{
	mLastPerfShellTraceStats.materialBuildCalls++;
	const bool tracePerf = ShouldTracePtPerf();
	const MaterialBuildTraceSlot materialTraceSlot = ResolveMaterialBuildTraceSlot(traceLabel);
	auto& materialTraceEntry = mLastPerfShellTraceStats.materialBuildByLabel[GetMaterialBuildTraceSlotIndex(materialTraceSlot)];
	materialTraceEntry.calls++;
	const ResolvedLightOverlaySet& resolvedLightOverlays = GetResolvedLightOverlaySet();
	if (HasActorFullbrightOverrides(resolvedLightOverlays))
	{
		const auto& actorOverrides = GetActorMaterialOverrideMapForFrame(materialTraceSlot);
		if (!actorOverrides.empty())
		{
			struct SavedMaterialFlags
			{
				uint32_t* flags = nullptr;
				uint32_t value = 0;
			};

			std::vector<SavedMaterialFlags> savedFlags;
			savedFlags.reserve(sceneView.opaqueWalls.size() + sceneView.opaqueFlats.size() + sceneView.opaqueSprites.size());

			auto applyOverrides = [&actorOverrides, &savedFlags](auto& surfaces)
			{
				for (auto& surface : surfaces)
				{
					if ((surface.material.flags & nri_scene::MaterialFlag_Sprite) == 0)
					{
						continue;
					}

					auto it = actorOverrides.find(surface.provenance.actorIndex);
					if (it == actorOverrides.end())
					{
						continue;
					}

					if ((it->second & ActorMaterialOverride_Fullbright) != 0)
					{
						savedFlags.push_back({ &surface.material.flags, surface.material.flags });
						surface.material.flags |= nri_scene::MaterialFlag_Fullbright;
					}
				}
			};

			applyOverrides(sceneView.opaqueWalls);
			applyOverrides(sceneView.opaqueFlats);
			applyOverrides(sceneView.opaqueSprites);

			if (tracePerf)
			{
				const auto start = std::chrono::steady_clock::now();
				nri_scene::BuildMaterials(sceneView, outMaterials);
				const double elapsedMs = DurationMs(start, std::chrono::steady_clock::now());
				mLastPerfShellTraceStats.materialBuildMs += elapsedMs;
				materialTraceEntry.materialBuildMs += elapsedMs;
			}
			else
			{
				nri_scene::BuildMaterials(sceneView, outMaterials);
			}
			ApplyActorFullbrightOverridesToBuiltMaterials(actorOverrides, outMaterials);
			for (const SavedMaterialFlags& saved : savedFlags)
			{
				*saved.flags = saved.value;
			}
			AccumulateMaterialTextureAttribution(
				materialTraceEntry,
				GatherMaterialTextureAttribution(outMaterials.materials, outMaterials.lightMetadata, outMaterials.textures.size()));
			TraceActorSpriteMaterialAssignments(sceneView, outMaterials, traceLabel);
			return;
		}
	}

	if (tracePerf)
	{
		const auto start = std::chrono::steady_clock::now();
		nri_scene::BuildMaterials(sceneView, outMaterials);
		const double elapsedMs = DurationMs(start, std::chrono::steady_clock::now());
		mLastPerfShellTraceStats.materialBuildMs += elapsedMs;
		materialTraceEntry.materialBuildMs += elapsedMs;
	}
	else
	{
		nri_scene::BuildMaterials(sceneView, outMaterials);
	}
	AccumulateMaterialTextureAttribution(
		materialTraceEntry,
		GatherMaterialTextureAttribution(outMaterials.materials, outMaterials.lightMetadata, outMaterials.textures.size()));
	TraceActorSpriteMaterialAssignments(sceneView, outMaterials, traceLabel);
}

void NRIRenderer::ApplyActorShadowMaterialOverrides(const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& inOutGpuMaterials)
{
	const ResolvedLightOverlaySet& resolvedLightOverlays = GetResolvedLightOverlaySet();
	if (resolvedLightOverlays.actorRules.Size() == 0 && resolvedLightOverlays.actorOverrideRules.Size() == 0)
	{
		return;
	}

	const auto& actorOverrides = GetActorMaterialOverrideMapForFrame();
	if (actorOverrides.empty())
	{
		return;
	}

	const uint32_t count = std::min<uint32_t>((uint32_t)inOutGpuMaterials.size(), (uint32_t)materials.lightMetadata.size());
	const float fullbrightBoost = GetFullbrightBoostScale();
	for (uint32_t materialIndex = 0; materialIndex < count; ++materialIndex)
	{
		const nri_scene::MaterialLightingMetadata& metadata = materials.lightMetadata[materialIndex];
		if (metadata.actorIndex < 0)
		{
			continue;
		}

		auto it = actorOverrides.find(metadata.actorIndex);
		if (it == actorOverrides.end())
		{
			continue;
		}

		if ((it->second & ActorMaterialOverride_NoShadowReceive) != 0)
		{
			inOutGpuMaterials[materialIndex].lightingFlags |= nri_scene::MaterialLightingFlag_NoShadowReceive;
		}
		if ((it->second & ActorMaterialOverride_NoShadowCast) != 0)
		{
			inOutGpuMaterials[materialIndex].lightingFlags |= nri_scene::MaterialLightingFlag_NoShadowCast;
		}
		if ((it->second & ActorMaterialOverride_Fullbright) != 0)
		{
			nri_scene::MaterialData& material = inOutGpuMaterials[materialIndex];
			material.flags |= nri_scene::MaterialFlag_Fullbright;
			material.lightLevel = 1.0f;
			material.roughnessHint = GetFullbrightRoughnessHint(material.flags);
			material.lightingFlags |= nri_scene::MaterialLightingFlag_MaterialFullbright;
			material.emissiveMode = nri_scene::MaterialEmissiveMode_UseBaseTexture;
			material.emissiveTextureIndex = material.textureIndex;
			material.emissiveIntensity = 1.0f;
			material.emissiveMaskScale = 1.0f;
			material.emissiveReserved = fullbrightBoost;
			material.emissiveColor[0] = 1.0f;
			material.emissiveColor[1] = 1.0f;
			material.emissiveColor[2] = 1.0f;
		}
	}
}

void NRIRenderer::InvalidateStaticMapSceneForMaterialLighting()
{
	if (!mStaticMapScene.valid)
	{
		return;
	}

	if (!EnsurePaletteTexture(mStaticMapScene.materialBridge) ||
		!EnsureSceneTextures(mStaticMapScene.sceneView, mStaticMapScene.materialBridge, mStaticMapScene.gpuMaterials, false, "static_map_scene") ||
		!EnsureStructuredBuffer(
			mStaticMaterialBuffer,
			mMaterialBufferStats,
			mStaticMapScene.gpuMaterials.data(),
			mStaticMapScene.gpuMaterials.size() * sizeof(nri_scene::MaterialData),
			sizeof(nri_scene::MaterialData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess()))
	{
		// Fall back to a full resident-scene rebuild on the next frame if the
		// targeted material refresh path fails for any reason.
		DestroyStaticMapSceneCache();
		mStaticMapScene = {};
		mStaticAccelerationBuildSerial = 0;
		return;
	}

	mStaticMapScene.texturesResident = true;
	mStaticMapScene.buffersResident = true;
	mStaticMapScene.gpuUploadCount++;
}

void NRIRenderer::PrintSceneLightDump(float radius, uint32_t limit) const
{
	if (!mSceneLights.HasRecords())
	{
		Printf("NRI PT scene lights: no cached scene-light identity is available yet.\n");
		return;
	}

	struct Candidate
	{
		const SceneLightSystem::SurfaceRecord* record = nullptr;
		float distanceSq = 0.0f;
	};

	std::vector<Candidate> candidates;
	candidates.reserve(mSceneLights.GetSurfaceRecords().size());
	const float radiusSq = radius > 0.0f ? radius * radius : -1.0f;

	for (const SceneLightSystem::SurfaceRecord& record : mSceneLights.GetSurfaceRecords())
	{
		const float dx = record.center[0] - mCurrentCameraPos[0];
		const float dy = record.center[1] - mCurrentCameraPos[1];
		const float dz = record.center[2] - mCurrentCameraPos[2];
		const float distanceSq = dx * dx + dy * dy + dz * dz;
		if (radiusSq >= 0.0f && distanceSq > radiusSq)
		{
			continue;
		}

		candidates.push_back({ &record, distanceSq });
	}

	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
	{
		if (a.distanceSq != b.distanceSq)
		{
			return a.distanceSq < b.distanceSq;
		}
		return a.record->materialIndex < b.record->materialIndex;
	});

	const uint32_t requestedLimit = limit == 0 ? 32u : limit;
	const uint32_t printCount = (uint32_t)std::min<size_t>(candidates.size(), requestedLimit);
	Printf("NRI PT scene lights: cached_surface_identities=%u near_camera=%u radius=%.2f frame=%u\n",
		(uint32_t)mSceneLights.GetSurfaceRecords().size(),
		(uint32_t)candidates.size(),
		radius,
		mFrameIndex);

	for (uint32_t i = 0; i < printCount; ++i)
	{
		const SceneLightSystem::SurfaceRecord& record = *candidates[i].record;
		const uint32_t lightingFlags = record.material.lightingFlags;
		const int32_t localSpaceIndex = record.provenance.mapChunkIndex >= 0 ? nri_scene::FindMapWorldLocalSpaceIndex(mMapWorld, (uint32_t)record.provenance.mapChunkIndex) : -1;
		const int32_t portalGraphIndex = nri_scene::FindMapWorldPortalIndex(mMapWorld, record.provenance);
		const char* textureName = record.material.texture != nullptr ? record.material.texture->GetName().GetChars() : "(null)";
		Printf("NRI PT scene light %u: source=%s drawlist=%s dist=%.2f center=(%.2f, %.2f, %.2f) radius=%.2f material=%u material_key=0x%016llx texture_key=0x%016llx glowmap_key=0x%016llx tile=%u texture=%s sector=%d wall=%d chunk=%d local_space=%d portal_graph=%d actor=%d palette=%u shade=%d alpha=%.3f light=%.3f flags=0x%x fullbright=%s tex_fullbright=%s glowing=%s auto_glow=%s glowmap=%s emissive_mode=%s emissive_tex=%u avg=(%.2f, %.2f, %.2f) glow=(%.2f, %.2f, %.2f)\n",
			i,
			GetSceneLightRecordSourceName(record.source),
			GetDrawListTypeName(record.provenance.drawListType),
			std::sqrt(candidates[i].distanceSq),
			record.center[0],
			record.center[1],
			record.center[2],
			record.boundsRadius,
			record.materialIndex,
			(unsigned long long)record.material.materialKey,
			(unsigned long long)record.material.textureContentKey,
			(unsigned long long)record.material.glowmapContentKey,
			record.material.textureId,
			textureName,
			record.provenance.sectorIndex,
			record.provenance.wallIndex,
			record.provenance.mapChunkIndex,
			localSpaceIndex,
			portalGraphIndex,
			record.provenance.actorIndex,
			record.material.paletteIndex,
			record.material.shade,
			record.material.alpha,
			record.material.lightLevel,
			record.material.materialFlags,
			(lightingFlags & nri_scene::MaterialLightingFlag_MaterialFullbright) != 0 ? "yes" : "no",
			(lightingFlags & nri_scene::MaterialLightingFlag_TextureFullbright) != 0 ? "yes" : "no",
			(lightingFlags & nri_scene::MaterialLightingFlag_TextureGlowing) != 0 ? "yes" : "no",
			(lightingFlags & nri_scene::MaterialLightingFlag_TextureAutoGlowing) != 0 ? "yes" : "no",
			(lightingFlags & nri_scene::MaterialLightingFlag_HasGlowmap) != 0 ? "yes" : "no",
			GetMaterialEmissiveModeName(record.material.emissiveMode),
			record.material.emissiveTextureIndex != UINT32_MAX ? record.material.emissiveTextureIndex : 0u,
			record.material.averageColor[0],
			record.material.averageColor[1],
			record.material.averageColor[2],
			record.material.glowColor[0],
			record.material.glowColor[1],
			record.material.glowColor[2]);
	}

	if (printCount == 0)
	{
		Printf("NRI PT scene lights: no cached surfaces matched the requested radius.\n");
	}
}

const char* NRIRenderer::GetAvailabilityReason() const
{
	if (mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return "renderer device is not initialized";
	}

	const nri::DeviceDesc& deviceDesc = mFrameBuffer->mCore.GetDeviceDesc(*mFrameBuffer->mDevice);
	if (deviceDesc.tiers.rayTracing == 0)
	{
		return "required ray tracing capability is unavailable on this device/API";
	}

	const size_t requiredRootConstantSize = std::max({ sizeof(NRITraceSceneConstants), sizeof(NRITemporalConstants), sizeof(NRIPresentConstants) });
	if (deviceDesc.pipelineLayout.rootConstantMaxSize < requiredRootConstantSize ||
		deviceDesc.pipelineLayout.rootDescriptorMaxNum < 1 ||
		deviceDesc.pipelineLayout.descriptorSetMaxNum < 5)
	{
		return "device pipeline layout limits are below the NRI PT backend requirements";
	}

	if (const char* sceneTextureLimitReason = GetSceneTextureDescriptorLimitFailureReason(deviceDesc))
	{
		return sceneTextureLimitReason;
	}

	return "path tracing is unavailable";
}

bool NRIRenderer::RefreshPathTracingAvailability()
{
	return CheckPathTracingSupport();
}

bool NRIRenderer::CheckPathTracingSupport()
{
	mPathTracingSupported = mFrameBuffer != nullptr && mFrameBuffer->mDevice != nullptr;
	if (!mPathTracingSupported)
	{
		return false;
	}

	const nri::DeviceDesc& deviceDesc = mFrameBuffer->mCore.GetDeviceDesc(*mFrameBuffer->mDevice);
	const size_t requiredRootConstantSize = std::max({ sizeof(NRITraceSceneConstants), sizeof(NRITemporalConstants), sizeof(NRIPresentConstants) });
	if (deviceDesc.tiers.rayTracing == 0 ||
		deviceDesc.pipelineLayout.rootConstantMaxSize < requiredRootConstantSize ||
		deviceDesc.pipelineLayout.rootDescriptorMaxNum < 1 ||
		deviceDesc.pipelineLayout.descriptorSetMaxNum < 5)
	{
		mPathTracingSupported = false;
		LogFallback(GetAvailabilityReason());
	}
	else if (const char* sceneTextureLimitReason = GetSceneTextureDescriptorLimitFailureReason(deviceDesc))
	{
		mPathTracingSupported = false;
		LogFallback(sceneTextureLimitReason);
	}

	return mPathTracingSupported;
}

void NRIRenderer::LogFallback(const char* reason)
{
	if (mHasLoggedFallback)
	{
		return;
	}

	Printf(TEXTCOLOR_ORANGE "NRI PT fallback: %s\n", reason != nullptr ? reason : "unknown reason");
	mHasLoggedFallback = true;
}

void NRIRenderer::ResetRuntimeMutationRebaselineState(bool destroyCandidateResources)
{
	mPendingRuntimeMutationRebaseline = false;
	mPendingRuntimeMutationRebaselineActiveChunkCount = 0;
	mPendingRuntimeMutationRebaselineStableChunkCount = 0;
	mRuntimeMutationRebaselineState = RuntimeMutationRebaselineState::Idle;
	mRuntimeMutationRebaselineQueueFrame = 0;
	mRuntimeMutationRebaselineLastAdvanceFrame = UINT32_MAX;
	mRuntimeMutationRebaselineExpectedGeometryBuildSerial = 0;
	mRuntimeMutationRebaselineBuildWorldMs = 0.0;
	mRuntimeMutationRebaselineBuildStaticSceneCacheMs = 0.0;
	mRuntimeMutationRebaselineRealizeStaticSceneTexturesMs = 0.0;
	mRuntimeMutationRebaselineUploadStaticSceneBuffersMs = 0.0;
	mRuntimeMutationRebaselinePrepareStaticSceneBlasMs = 0.0;
	mRuntimeMutationRebaselineBuildStaticSceneBlasMs = 0.0;
	mRuntimeMutationRebaselineBuildStaticSceneTlasMs = 0.0;
	mRuntimeMutationRebaselineSwapMs = 0.0;
	mRuntimeMutationRebaselineRetireMs = 0.0;
	mRuntimeMutationRebaselineCandidate.valid = false;
	mRuntimeMutationRebaselineCandidate.world = {};
	mRuntimeMutationRebaselineCandidate.pendingGeometryBuildSerial = 0;
	mRuntimeMutationRebaselineCandidate.cacheBuildCursor = 0;
	mRuntimeMutationRebaselineCandidate.cacheBuildCount = 0;
	mRuntimeMutationRebaselineCandidate.blasPrepareCursor = 0;
	mRuntimeMutationRebaselineCandidate.blasPrepareCount = 0;
	mRuntimeMutationRebaselineCandidate.blasBuildCursor = 0;
	mRuntimeMutationRebaselineCandidate.blasBuildCount = 0;
	mRuntimeMutationRebaselineCandidate.blasScratchSize = 0;

	if (destroyCandidateResources)
	{
		DestroyStaticMapSceneResources(
			mRuntimeMutationRebaselineCandidate.staticScene,
			mRuntimeMutationRebaselineCandidate.staticResources,
			true);
		mRuntimeMutationRebaselineCandidate.runtimeMutations = {};
		for (auto& retired : mRetiredRuntimeMutationRebaselineStaticScenes)
		{
			DestroyStaticMapSceneResources(
				retired.staticScene,
				retired.staticResources,
				true);
		}
		mRetiredRuntimeMutationRebaselineStaticScenes.clear();
	}

	UpdateRuntimeMutationRebaselinePerfStats();
}

void NRIRenderer::RefreshMapWorld()
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.mapWorldMs);
	const uint64_t pendingBuildSerial = nri_scene::GetPendingLevelGeometryBuildSerial();
	const bool levelChanged = mMapWorld.level != currentLevel;
	if (levelChanged)
	{
		RequestHistoryReset("map-load", true, true);
		mSceneTextureCacheDebugStats = {};
		mPersistentDynamicEmissiveHighWaterStats = {};
		mPersistentDynamicEmissiveHighWaterSurfaceCount = 0;
		mPersistentDynamicEmissiveHighWaterPrimitiveCount = 0;
		mPersistentDynamicEmissiveHighWaterMaterialCount = 0;
		mRuntimeMutationCacheHighWaterStats = {};
		ResetRuntimeMutationRebaselineState(true);
		mHasRuntimeMutationRebaseline = false;
		mLastRuntimeMutationRebaselineFrame = 0;
	}
	if (levelChanged && mSceneLights.GetManualAnalyticLightCount() > 0)
	{
		const uint32_t clearedCount = mSceneLights.GetManualAnalyticLightCount();
		ClearRuntimePointLights();
		Printf("NRI PT test lights cleared: count=%u reason=level-change\n", clearedCount);
	}
	if (levelChanged && !mRuntimeDebugSpheres.empty())
	{
		const uint32_t clearedCount = (uint32_t)mRuntimeDebugSpheres.size();
		ClearRuntimeDebugSpheres();
		Printf("NRI PT debug spheres cleared: count=%u reason=level-change\n", clearedCount);
	}
	const bool needsBuild = !mMapWorld.valid || levelChanged || pendingBuildSerial != mObservedMapWorldBuildSerial;
	if (!needsBuild)
	{
		if (mAllowStartupMutationRebaseline && mFrameIndex > mStartupMutationRebaselineDeadlineFrame)
		{
			mAllowStartupMutationRebaseline = false;
			mPendingStartupMutationRebaseline = false;
		}
		return;
	}

	if (!levelChanged && pendingBuildSerial != mObservedMapWorldBuildSerial)
	{
		ResetRuntimeMutationRebaselineState(true);
	}

	ResetPersistentDynamicEmissiveCache();
	mAllowStartupMutationRebaseline = true;
	mPendingStartupMutationRebaseline = false;
	mStartupMutationRebaselineDeadlineFrame = mFrameIndex + 4u;

	nri_scene::PTMapWorld world;
	if (!nri_scene::BuildMapWorld(world))
	{
		if (pendingBuildSerial != mObservedMapWorldBuildSerial || levelChanged)
		{
			Printf(TEXTCOLOR_RED "NRI PT map world: authoritative level-load build failed for %s.\n",
				currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)");
		}
		mMapWorld.Reset();
		mMapWorld.level = currentLevel;
		mObservedMapWorldBuildSerial = pendingBuildSerial;
		mAllowStartupMutationRebaseline = false;
		mPendingStartupMutationRebaseline = false;
		return;
	}

	mMapWorld = std::move(world);
	mObservedMapWorldBuildSerial = pendingBuildSerial;
	const auto& stats = mMapWorld.stats;
	Printf("NRI PT map world built: level=%s build_serial=%llu chunks=%u surfaces=%u walls=%u flats=%u portals=%u skies=%u tris=%u\n",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)mMapWorld.buildSerial,
		stats.chunkCount,
		stats.surfaceCount,
		stats.wallSurfaceCount,
		stats.flatSurfaceCount,
		stats.portalSurfaceCount,
		stats.skySurfaceCount,
		stats.triangleCount);
}

void NRIRenderer::RebuildStartupMutationBaseline()
{
	if (!mPendingStartupMutationRebaseline)
	{
		return;
	}

	mPendingStartupMutationRebaseline = false;
	mAllowStartupMutationRebaseline = false;
	ResetRuntimeMutationRebaselineState(true);
	mHasRuntimeMutationRebaseline = false;
	mLastRuntimeMutationRebaselineFrame = 0;

	nri_scene::PTMapWorld world;
	if (!nri_scene::BuildMapWorld(world))
	{
		Printf(TEXTCOLOR_RED "NRI PT startup mutation rebaseline: authoritative rebuild failed for %s.\n",
			currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)");
		return;
	}

	DestroyStaticMapSceneCache();
	mStaticMapScene = {};
	mStaticAccelerationBuildSerial = 0;
	mPreservedStaticMapSky = {};
	mMapWorld = std::move(world);
	mObservedMapWorldBuildSerial = nri_scene::GetPendingLevelGeometryBuildSerial();
	RequestHistoryReset("startup-mutation-rebaseline");

	const auto& stats = mMapWorld.stats;
	Printf("NRI PT startup mutation rebaseline: level=%s build_serial=%llu chunks=%u surfaces=%u walls=%u flats=%u portals=%u skies=%u tris=%u\n",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)mMapWorld.buildSerial,
		stats.chunkCount,
		stats.surfaceCount,
		stats.wallSurfaceCount,
		stats.flatSurfaceCount,
		stats.portalSurfaceCount,
		stats.skySurfaceCount,
		stats.triangleCount);
}

void NRIRenderer::RebuildRuntimeMutationBaseline()
{
	AdvanceRuntimeMutationRebaseline();
}

bool NRIRenderer::BuildRuntimeMutationRebaselineStaticSceneCache()
{
	auto& candidate = mRuntimeMutationRebaselineCandidate;
	if (!candidate.valid || !candidate.world.valid)
	{
		return false;
	}

	if (candidate.cacheBuildCursor == 0)
	{
		DestroyStaticMapSceneResources(candidate.staticScene, candidate.staticResources, false);
		candidate.runtimeMutations = {};
		candidate.staticScene = {};
		candidate.cacheBuildCount = 0;
		candidate.blasPrepareCursor = 0;
		candidate.blasPrepareCount = 0;
		candidate.blasBuildCursor = 0;
		candidate.blasBuildCount = 0;
		candidate.blasScratchSize = 0;
		InitializeStaticMapSceneCacheBuild(candidate.world, nullptr, candidate.staticScene, candidate.runtimeMutations);
	}

	const uint32_t totalChunkCount = (uint32_t)candidate.world.chunks.size();
	const uint32_t batchChunkCount = std::max(1, (int)nri_ptrebaselinecachechunksperframe);
	const uint32_t buildEnd = std::min(totalChunkCount, candidate.cacheBuildCursor + batchChunkCount);
	for (uint32_t chunkIndex = candidate.cacheBuildCursor; chunkIndex < buildEnd; ++chunkIndex)
	{
		AppendStaticMapSceneCacheChunk(
			candidate.world,
			candidate.world.chunks[chunkIndex],
			nullptr,
			candidate.staticScene,
			candidate.runtimeMutations);
	}

	candidate.cacheBuildCursor = buildEnd;
	candidate.cacheBuildCount = buildEnd;
	return true;
}

bool NRIRenderer::RealizeRuntimeMutationRebaselineStaticSceneTextures()
{
	auto& candidate = mRuntimeMutationRebaselineCandidate;
	if (!candidate.valid || !candidate.world.valid || candidate.staticScene.geometry.primitives.empty())
	{
		return false;
	}

	if (!EnsurePaletteTexture(candidate.staticScene.materialBridge) ||
		!EnsureSceneTextures(candidate.staticScene.sceneView, candidate.staticScene.materialBridge, candidate.staticScene.gpuMaterials, false, "runtime_mutation_rebaseline_candidate"))
	{
		return false;
	}

	candidate.staticScene.texturesResident = true;
	return true;
}

bool NRIRenderer::UploadRuntimeMutationRebaselineStaticSceneBuffers()
{
	auto& candidate = mRuntimeMutationRebaselineCandidate;
	if (!candidate.valid || !candidate.world.valid || !candidate.staticScene.texturesResident)
	{
		return false;
	}

	if (!UploadStaticMapChunkAtlas(
		candidate.staticResources.vertexBuffer,
		candidate.staticResources.indexBuffer,
		candidate.staticResources.primitiveBuffer,
		candidate.staticResources.materialBuffer,
		candidate.staticResources.chunkAtlas,
		candidate.staticScene,
		candidate.staticScene.gpuMaterials))
	{
		return false;
	}

	candidate.staticScene.buffersResident = true;
	return true;
}

bool NRIRenderer::PrepareRuntimeMutationRebaselineStaticSceneBlas()
{
	auto& candidate = mRuntimeMutationRebaselineCandidate;
	if (!candidate.valid || !candidate.world.valid || !candidate.staticScene.buffersResident || candidate.staticScene.chunks.empty())
	{
		return false;
	}

	auto& staticScene = candidate.staticScene;
	auto& staticResources = candidate.staticResources;
	if (!staticResources.chunkAtlas.valid || staticResources.chunkAtlas.chunks.size() != staticScene.chunks.size())
	{
		return false;
	}

	if (candidate.blasPrepareCursor == 0)
	{
		const bool needsWait =
			staticResources.topLevelAS.accelerationStructure != nullptr ||
			staticResources.tlasInstanceBuffer.buffer != nullptr ||
			staticResources.scratchBuffer.buffer != nullptr ||
			staticResources.topLevelScratchBuffer.buffer != nullptr;
		if (needsWait)
		{
			WaitForCommandsTracked();
		}

		DestroyBufferResource(staticResources.tlasInstanceBuffer);
		DestroyBufferResource(staticResources.scratchBuffer);
		DestroyBufferResource(staticResources.topLevelScratchBuffer);
		DestroyAccelerationStructureResource(staticResources.topLevelAS);
		for (auto& chunk : staticScene.chunks)
		{
			DestroyAccelerationStructureResource(chunk.accelerationStructure);
		}

		staticResources.sceneInstances.clear();
		staticResources.tlasInstanceCount = 0;
		staticResources.accelerationBuildSerial = staticScene.buildSerial;
		candidate.blasScratchSize = 0;
		candidate.blasPrepareCount = 0;
		candidate.blasBuildCursor = 0;
		candidate.blasBuildCount = 0;
	}

	const uint32_t totalChunkCount = (uint32_t)staticScene.chunks.size();
	const uint32_t batchChunkCount = std::max(1, (int)nri_ptrebaselineblasperframe);
	const uint32_t prepareEnd = std::min(totalChunkCount, candidate.blasPrepareCursor + batchChunkCount);
	for (uint32_t chunkIndex = candidate.blasPrepareCursor; chunkIndex < prepareEnd; ++chunkIndex)
	{
		auto& chunk = staticScene.chunks[chunkIndex];
		const auto& atlasChunk = staticResources.chunkAtlas.chunks[chunkIndex];
		nri::BottomLevelGeometryDesc geometryDesc = {};
		geometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
		geometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
		geometryDesc.triangles.vertexBuffer = staticResources.vertexBuffer.buffer;
		geometryDesc.triangles.vertexOffset = 0;
		geometryDesc.triangles.vertexNum = staticResources.chunkAtlas.vertexCount;
		geometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
		geometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
		geometryDesc.triangles.indexBuffer = staticResources.indexBuffer.buffer;
		geometryDesc.triangles.indexOffset = (uint64_t)atlasChunk.indexOffset * sizeof(uint32_t);
		geometryDesc.triangles.indexNum = atlasChunk.indexCount;
		geometryDesc.triangles.indexType = nri::IndexType::UINT32;

		nri::AccelerationStructureDesc blasDesc = {};
		blasDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
		blasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
		blasDesc.geometryOrInstanceNum = 1;
		blasDesc.geometries = &geometryDesc;
		if (mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, blasDesc, chunk.accelerationStructure.accelerationStructure) != nri::Result::SUCCESS)
		{
			return false;
		}

		nri::MemoryDesc memoryDesc = {};
		mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(*chunk.accelerationStructure.accelerationStructure, nri::MemoryLocation::DEVICE, memoryDesc);
		chunk.accelerationStructure.memorySize = memoryDesc.size;
		chunk.accelerationStructure.memoryLocation = nri::MemoryLocation::DEVICE;

		candidate.blasScratchSize = std::max(candidate.blasScratchSize, mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*chunk.accelerationStructure.accelerationStructure));
	}

	candidate.blasPrepareCursor = prepareEnd;
	candidate.blasPrepareCount = prepareEnd;

	if (candidate.blasPrepareCursor < totalChunkCount)
	{
		return true;
	}

	if (staticResources.scratchBuffer.buffer == nullptr &&
		!CreateBufferWithoutView(staticResources.scratchBuffer, candidate.blasScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
	{
		return false;
	}

	return true;
}

bool NRIRenderer::AdvanceRuntimeMutationRebaselineStaticSceneBlas()
{
	auto& candidate = mRuntimeMutationRebaselineCandidate;
	auto& staticScene = candidate.staticScene;
	auto& staticResources = candidate.staticResources;
	if (!candidate.valid ||
		!candidate.world.valid ||
		staticResources.scratchBuffer.buffer == nullptr ||
		staticScene.chunks.empty())
	{
		return false;
	}
	if (!staticResources.chunkAtlas.valid || staticResources.chunkAtlas.chunks.size() != staticScene.chunks.size())
	{
		return false;
	}

	const uint32_t totalChunkCount = (uint32_t)staticScene.chunks.size();
	if (candidate.blasBuildCursor >= totalChunkCount)
	{
		candidate.blasBuildCount = totalChunkCount;
		return true;
	}

	const uint32_t batchChunkCount = std::max(1, (int)nri_ptrebaselineblasperframe);
	const uint32_t buildEnd = std::min(totalChunkCount, candidate.blasBuildCursor + batchChunkCount);
	std::vector<nri::BufferBarrierDesc> blasBarriers;
	blasBarriers.reserve(buildEnd - candidate.blasBuildCursor);

	for (uint32_t chunkIndex = candidate.blasBuildCursor; chunkIndex < buildEnd; ++chunkIndex)
	{
		auto& chunk = staticScene.chunks[chunkIndex];
		const auto& atlasChunk = staticResources.chunkAtlas.chunks[chunkIndex];

		nri::BottomLevelGeometryDesc geometryDesc = {};
		geometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
		geometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
		geometryDesc.triangles.vertexBuffer = staticResources.vertexBuffer.buffer;
		geometryDesc.triangles.vertexOffset = 0;
		geometryDesc.triangles.vertexNum = staticResources.chunkAtlas.vertexCount;
		geometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
		geometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
		geometryDesc.triangles.indexBuffer = staticResources.indexBuffer.buffer;
		geometryDesc.triangles.indexOffset = (uint64_t)atlasChunk.indexOffset * sizeof(uint32_t);
		geometryDesc.triangles.indexNum = atlasChunk.indexCount;
		geometryDesc.triangles.indexType = nri::IndexType::UINT32;

		nri::BuildBottomLevelAccelerationStructureDesc build = {};
		build.dst = chunk.accelerationStructure.accelerationStructure;
		build.geometries = &geometryDesc;
		build.geometryNum = 1;
		build.scratchBuffer = staticResources.scratchBuffer.buffer;
		build.scratchOffset = 0;
		mFrameBuffer->mRayTracing.CmdBuildBottomLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &build, 1);

		if (chunkIndex + 1 < buildEnd)
		{
			nri::BufferBarrierDesc scratchBarrier = {};
			scratchBarrier.buffer = staticResources.scratchBuffer.buffer;
			scratchBarrier.before = NRIAccelerationStructureScratchAccess();
			scratchBarrier.after = NRIAccelerationStructureScratchAccess();

			nri::BarrierDesc scratchBarrierDesc = {};
			scratchBarrierDesc.buffers = &scratchBarrier;
			scratchBarrierDesc.bufferNum = 1;
			mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, scratchBarrierDesc);
		}

		nri::BufferBarrierDesc barrier = {};
		barrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*chunk.accelerationStructure.accelerationStructure);
		barrier.before = NRIAccelerationStructureWriteAccess();
		barrier.after = NRIAccelerationStructureReadAccess();
		blasBarriers.push_back(barrier);
	}

	if (!blasBarriers.empty())
	{
		nri::BarrierDesc blasBarrierDesc = {};
		blasBarrierDesc.buffers = blasBarriers.data();
		blasBarrierDesc.bufferNum = (uint32_t)blasBarriers.size();
		mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, blasBarrierDesc);
	}

	candidate.blasBuildCursor = buildEnd;
	candidate.blasBuildCount = buildEnd;
	return true;
}

bool NRIRenderer::BuildRuntimeMutationRebaselineStaticSceneTlas()
{
	auto& candidate = mRuntimeMutationRebaselineCandidate;
	auto& staticScene = candidate.staticScene;
	auto& staticResources = candidate.staticResources;
	if (!candidate.valid ||
		!candidate.world.valid ||
		candidate.blasBuildCount < staticScene.chunks.size())
	{
		return false;
	}

	std::vector<nri::TopLevelInstance> instances;
	std::vector<SceneInstanceData> sceneInstances;
	BuildStaticMapInstances(staticScene, staticResources.chunkAtlas, instances, sceneInstances);
	staticResources.sceneInstances = sceneInstances;
	staticResources.accelerationBuildSerial = staticScene.buildSerial;
	if (!BuildTopLevelAccelerationStructure(
		instances,
		SceneDataBufferMask_Static,
		staticResources.topLevelAS,
		staticResources.tlasInstanceBuffer,
		staticResources.topLevelScratchBuffer,
		&staticResources.vertexBuffer,
		&staticResources.indexBuffer,
		&staticResources.tlasInstanceCount,
		false))
	{
		return false;
	}

	staticScene.valid = true;
	staticScene.texturesResident = true;
	staticScene.buffersResident = true;
	staticScene.accelerationResident = true;
	staticScene.buildSerial = candidate.world.buildSerial;
	staticScene.tlasInstanceCount = staticResources.tlasInstanceCount;
	staticScene.sceneBuildCount++;
	staticScene.gpuUploadCount++;
	staticScene.accelerationBuildCount++;
	return true;
}

bool NRIRenderer::SwapRuntimeMutationRebaselineCandidate()
{
	auto& candidate = mRuntimeMutationRebaselineCandidate;
	if (!candidate.valid ||
		!candidate.world.valid ||
		!candidate.staticScene.valid ||
		candidate.staticResources.topLevelAS.accelerationStructure == nullptr)
	{
		return false;
	}

	RuntimeMutationRebaselineRetiredStaticScene retiredScene = {};
	retiredScene.valid = true;
	retiredScene.retireAfterFrame = mFrameIndex + std::max<uint32_t>(CountPotentialOutstandingQueuedFrames(), mFrameBuffer != nullptr ? (uint32_t)mFrameBuffer->mQueuedFrames.size() : 0u) + 1u;
	retiredScene.staticScene = std::move(mStaticMapScene);
	retiredScene.staticResources.vertexBuffer = std::move(mStaticVertexBuffer);
	retiredScene.staticResources.indexBuffer = std::move(mStaticIndexBuffer);
	retiredScene.staticResources.primitiveBuffer = std::move(mStaticPrimitiveBuffer);
	retiredScene.staticResources.materialBuffer = std::move(mStaticMaterialBuffer);
	retiredScene.staticResources.chunkAtlas = std::move(mStaticMapChunkAtlas);
	retiredScene.staticResources.tlasInstanceBuffer = std::move(mTlasInstanceBuffer);
	retiredScene.staticResources.scratchBuffer = std::move(mScratchBuffer);
	retiredScene.staticResources.topLevelScratchBuffer = std::move(mTopLevelScratchBuffer);
	retiredScene.staticResources.topLevelAS = std::move(mTopLevelAS);
	retiredScene.staticResources.accelerationBuildSerial = mStaticAccelerationBuildSerial;
	retiredScene.staticResources.tlasInstanceCount = mActiveTlasInstanceCount;
	mRetiredRuntimeMutationRebaselineStaticScenes.push_back(std::move(retiredScene));

	mStaticMapScene = std::move(candidate.staticScene);
	mStaticVertexBuffer = std::move(candidate.staticResources.vertexBuffer);
	mStaticIndexBuffer = std::move(candidate.staticResources.indexBuffer);
	mStaticPrimitiveBuffer = std::move(candidate.staticResources.primitiveBuffer);
	mStaticMaterialBuffer = std::move(candidate.staticResources.materialBuffer);
	mStaticMapChunkAtlas = std::move(candidate.staticResources.chunkAtlas);
	mTlasInstanceBuffer = std::move(candidate.staticResources.tlasInstanceBuffer);
	mScratchBuffer = std::move(candidate.staticResources.scratchBuffer);
	mTopLevelScratchBuffer = std::move(candidate.staticResources.topLevelScratchBuffer);
	mTopLevelAS = std::move(candidate.staticResources.topLevelAS);
	mStaticAccelerationBuildSerial = candidate.staticResources.accelerationBuildSerial;
	mActiveTlasInstanceCount = candidate.staticResources.tlasInstanceCount;
	mMapWorld = std::move(candidate.world);
	mRuntimeMapMutations = std::move(candidate.runtimeMutations);
	mObservedMapWorldBuildSerial = candidate.pendingGeometryBuildSerial;
	SyncResidentMapChunkRegistryFromStaticScene();

	if (!UpdateSceneDataSet(
		mStaticVertexBuffer,
		mStaticIndexBuffer,
		mStaticPrimitiveBuffer,
		mStaticMaterialBuffer,
		mStaticVertexBuffer,
		mStaticIndexBuffer,
		mStaticPrimitiveBuffer,
		mStaticMaterialBuffer,
		candidate.staticResources.sceneInstances,
		(uint32_t)mStaticMapScene.geometry.primitives.size(),
		0u,
		(uint32_t)mStaticMapScene.gpuMaterials.size(),
		0u,
		"runtime_mutation_rebaseline_swap"))
	{
		return false;
	}

	mPreservedStaticMapSky = {};
	mHasRuntimeMutationRebaseline = true;
	mLastRuntimeMutationRebaselineFrame = mFrameIndex;
	mAllowStartupMutationRebaseline = false;
	RequestHistoryReset("runtime-mutation-rebaseline");

	candidate = {};
	return true;
}

void NRIRenderer::DrainRetiredRuntimeMutationRebaselineStaticScenes()
{
	if (mRetiredRuntimeMutationRebaselineStaticScenes.empty())
	{
		return;
	}

	auto& retired = mRetiredRuntimeMutationRebaselineStaticScenes.front();
	if (!retired.valid || mFrameIndex < retired.retireAfterFrame)
	{
		return;
	}

	const auto start = std::chrono::steady_clock::now();
	const uint32_t totalChunkCount = (uint32_t)retired.staticScene.chunks.size();
	const uint32_t batchChunkCount = std::max(1, (int)nri_ptrebaselinecachechunksperframe);
	const uint32_t destroyEnd = std::min(totalChunkCount, retired.destroyChunkCursor + batchChunkCount);
	for (uint32_t chunkIndex = retired.destroyChunkCursor; chunkIndex < destroyEnd; ++chunkIndex)
	{
		DestroyAccelerationStructureResource(retired.staticScene.chunks[chunkIndex].accelerationStructure);
	}

	retired.destroyChunkCursor = destroyEnd;
	if (retired.destroyChunkCursor >= totalChunkCount)
	{
		DestroyAccelerationStructureResource(retired.staticResources.topLevelAS);
		DestroyBufferResource(retired.staticResources.vertexBuffer);
		DestroyBufferResource(retired.staticResources.indexBuffer);
		DestroyBufferResource(retired.staticResources.primitiveBuffer);
		DestroyBufferResource(retired.staticResources.materialBuffer);
		DestroyBufferResource(retired.staticResources.tlasInstanceBuffer);
		DestroyBufferResource(retired.staticResources.scratchBuffer);
		DestroyBufferResource(retired.staticResources.topLevelScratchBuffer);
		retired.staticScene = {};
		retired.staticResources = {};
		retired.valid = false;
		mRetiredRuntimeMutationRebaselineStaticScenes.erase(mRetiredRuntimeMutationRebaselineStaticScenes.begin());
	}

	mRuntimeMutationRebaselineRetireMs += DurationMs(start, std::chrono::steady_clock::now());
}

void NRIRenderer::AdvanceRuntimeMutationRebaseline()
{
	if (mRuntimeMutationRebaselineLastAdvanceFrame == mFrameIndex)
	{
		return;
	}
	mRuntimeMutationRebaselineLastAdvanceFrame = mFrameIndex;
	DrainRetiredRuntimeMutationRebaselineStaticScenes();

	if (mPendingRuntimeMutationRebaseline && mRuntimeMutationRebaselineState == RuntimeMutationRebaselineState::Idle)
	{
		mRuntimeMutationRebaselineState = RuntimeMutationRebaselineState::Queued;
		mRuntimeMutationRebaselineQueueFrame = mFrameIndex;
		mRuntimeMutationRebaselineExpectedGeometryBuildSerial = nri_scene::GetPendingLevelGeometryBuildSerial();
		mRuntimeMutationRebaselineBuildWorldMs = 0.0;
		mRuntimeMutationRebaselineBuildStaticSceneCacheMs = 0.0;
		mRuntimeMutationRebaselineRealizeStaticSceneTexturesMs = 0.0;
		mRuntimeMutationRebaselineUploadStaticSceneBuffersMs = 0.0;
		mRuntimeMutationRebaselinePrepareStaticSceneBlasMs = 0.0;
		mRuntimeMutationRebaselineBuildStaticSceneBlasMs = 0.0;
		mRuntimeMutationRebaselineBuildStaticSceneTlasMs = 0.0;
		mRuntimeMutationRebaselineSwapMs = 0.0;
		mRuntimeMutationRebaselineRetireMs = 0.0;
		UpdateRuntimeMutationRebaselinePerfStats();
		TraceRuntimeMutationRebaselineProgress("queued");
		return;
	}

	switch (mRuntimeMutationRebaselineState)
	{
	case RuntimeMutationRebaselineState::Idle:
		break;
	case RuntimeMutationRebaselineState::Queued:
		mRuntimeMutationRebaselineState = RuntimeMutationRebaselineState::BuildingAuthoritativeWorld;
		TraceRuntimeMutationRebaselineProgress("enter_building_authoritative_world");
		break;
	case RuntimeMutationRebaselineState::BuildingAuthoritativeWorld:
	{
		auto start = std::chrono::steady_clock::now();
		auto& candidate = mRuntimeMutationRebaselineCandidate;
		DestroyStaticMapSceneResources(candidate.staticScene, candidate.staticResources, false);
		candidate = {};
		nri_scene::PTMapWorld world;
		if (!nri_scene::BuildMapWorld(world))
		{
			Printf(TEXTCOLOR_RED "NRI PT runtime mutation rebaseline: authoritative rebuild failed for %s active_chunks=%u stable_chunks=%u.\n",
				currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)",
				mPendingRuntimeMutationRebaselineActiveChunkCount,
				mPendingRuntimeMutationRebaselineStableChunkCount);
			ResetRuntimeMutationRebaselineState(true);
			return;
		}

		candidate.valid = true;
		candidate.world = std::move(world);
		candidate.pendingGeometryBuildSerial = mRuntimeMutationRebaselineExpectedGeometryBuildSerial;
		candidate.cacheBuildCursor = 0;
		candidate.cacheBuildCount = 0;
		candidate.blasPrepareCursor = 0;
		candidate.blasPrepareCount = 0;
		candidate.blasBuildCursor = 0;
		candidate.blasBuildCount = 0;
		candidate.blasScratchSize = 0;
		mRuntimeMutationRebaselineBuildWorldMs = DurationMs(start, std::chrono::steady_clock::now());
		mRuntimeMutationRebaselineState = RuntimeMutationRebaselineState::WorldReady;
		TraceRuntimeMutationRebaselineProgress("world_ready");
		break;
	}
	case RuntimeMutationRebaselineState::WorldReady:
		mRuntimeMutationRebaselineState = RuntimeMutationRebaselineState::BuildingStaticSceneCache;
		TraceRuntimeMutationRebaselineProgress("enter_building_static_scene_cache");
		break;
	case RuntimeMutationRebaselineState::BuildingStaticSceneCache:
	{
		auto start = std::chrono::steady_clock::now();
		if (!BuildRuntimeMutationRebaselineStaticSceneCache())
		{
			Printf(TEXTCOLOR_RED "NRI PT runtime mutation rebaseline: static-scene cache build failed for %s active_chunks=%u stable_chunks=%u.\n",
				currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)",
				mPendingRuntimeMutationRebaselineActiveChunkCount,
				mPendingRuntimeMutationRebaselineStableChunkCount);
			ResetRuntimeMutationRebaselineState(true);
			return;
		}

		mRuntimeMutationRebaselineBuildStaticSceneCacheMs += DurationMs(start, std::chrono::steady_clock::now());
		if (mRuntimeMutationRebaselineCandidate.cacheBuildCount >= mRuntimeMutationRebaselineCandidate.world.chunks.size())
		{
			mRuntimeMutationRebaselineState = RuntimeMutationRebaselineState::RealizingStaticSceneTextures;
			TraceRuntimeMutationRebaselineProgress("static_scene_cache_ready");
		}
		else
		{
			TraceRuntimeMutationRebaselineProgress("static_scene_cache_progress");
		}
		break;
	}
	case RuntimeMutationRebaselineState::RealizingStaticSceneTextures:
	{
		auto start = std::chrono::steady_clock::now();
		if (!RealizeRuntimeMutationRebaselineStaticSceneTextures())
		{
			Printf(TEXTCOLOR_RED "NRI PT runtime mutation rebaseline: static-scene texture realization failed for %s active_chunks=%u stable_chunks=%u.\n",
				currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)",
				mPendingRuntimeMutationRebaselineActiveChunkCount,
				mPendingRuntimeMutationRebaselineStableChunkCount);
			ResetRuntimeMutationRebaselineState(true);
			return;
		}

		mRuntimeMutationRebaselineRealizeStaticSceneTexturesMs = DurationMs(start, std::chrono::steady_clock::now());
		mRuntimeMutationRebaselineState = RuntimeMutationRebaselineState::UploadingStaticSceneBuffers;
		TraceRuntimeMutationRebaselineProgress("static_scene_textures_ready");
		break;
	}
	case RuntimeMutationRebaselineState::UploadingStaticSceneBuffers:
	{
		auto start = std::chrono::steady_clock::now();
		if (!UploadRuntimeMutationRebaselineStaticSceneBuffers())
		{
			Printf(TEXTCOLOR_RED "NRI PT runtime mutation rebaseline: static-scene buffer upload failed for %s active_chunks=%u stable_chunks=%u.\n",
				currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)",
				mPendingRuntimeMutationRebaselineActiveChunkCount,
				mPendingRuntimeMutationRebaselineStableChunkCount);
			ResetRuntimeMutationRebaselineState(true);
			return;
		}

		mRuntimeMutationRebaselineUploadStaticSceneBuffersMs = DurationMs(start, std::chrono::steady_clock::now());
		mRuntimeMutationRebaselineState = RuntimeMutationRebaselineState::PreparingStaticSceneBlasResources;
		TraceRuntimeMutationRebaselineProgress("static_scene_buffers_ready");
		break;
	}
	case RuntimeMutationRebaselineState::PreparingStaticSceneBlasResources:
	{
		auto start = std::chrono::steady_clock::now();
		if (!PrepareRuntimeMutationRebaselineStaticSceneBlas())
		{
			Printf(TEXTCOLOR_RED "NRI PT runtime mutation rebaseline: static-scene BLAS preparation failed for %s active_chunks=%u stable_chunks=%u.\n",
				currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)",
				mPendingRuntimeMutationRebaselineActiveChunkCount,
				mPendingRuntimeMutationRebaselineStableChunkCount);
			ResetRuntimeMutationRebaselineState(true);
			return;
		}

		mRuntimeMutationRebaselinePrepareStaticSceneBlasMs += DurationMs(start, std::chrono::steady_clock::now());
		if (mRuntimeMutationRebaselineCandidate.blasPrepareCount >= mRuntimeMutationRebaselineCandidate.staticScene.chunks.size() &&
			mRuntimeMutationRebaselineCandidate.staticResources.scratchBuffer.buffer != nullptr)
		{
			mRuntimeMutationRebaselineState = RuntimeMutationRebaselineState::BuildingStaticSceneBlas;
			TraceRuntimeMutationRebaselineProgress("static_scene_blas_resources_ready");
		}
		else
		{
			TraceRuntimeMutationRebaselineProgress("static_scene_blas_resources_progress");
		}
		break;
	}
	case RuntimeMutationRebaselineState::BuildingStaticSceneBlas:
	{
		auto start = std::chrono::steady_clock::now();
		if (!AdvanceRuntimeMutationRebaselineStaticSceneBlas())
		{
			Printf(TEXTCOLOR_RED "NRI PT runtime mutation rebaseline: static-scene BLAS build failed for %s active_chunks=%u stable_chunks=%u.\n",
				currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)",
				mPendingRuntimeMutationRebaselineActiveChunkCount,
				mPendingRuntimeMutationRebaselineStableChunkCount);
			ResetRuntimeMutationRebaselineState(true);
			return;
		}

		mRuntimeMutationRebaselineBuildStaticSceneBlasMs += DurationMs(start, std::chrono::steady_clock::now());
		if (mRuntimeMutationRebaselineCandidate.blasBuildCount >= mRuntimeMutationRebaselineCandidate.staticScene.chunks.size() &&
			mRuntimeMutationRebaselineCandidate.staticResources.scratchBuffer.buffer != nullptr)
		{
			mRuntimeMutationRebaselineState = RuntimeMutationRebaselineState::BuildingStaticSceneTlas;
			TraceRuntimeMutationRebaselineProgress("static_scene_blas_complete");
		}
		else
		{
			TraceRuntimeMutationRebaselineProgress("static_scene_blas_progress");
		}
		break;
	}
	case RuntimeMutationRebaselineState::BuildingStaticSceneTlas:
	{
		auto start = std::chrono::steady_clock::now();
		if (!BuildRuntimeMutationRebaselineStaticSceneTlas())
		{
			Printf(TEXTCOLOR_RED "NRI PT runtime mutation rebaseline: static-scene TLAS build failed for %s active_chunks=%u stable_chunks=%u.\n",
				currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)",
				mPendingRuntimeMutationRebaselineActiveChunkCount,
				mPendingRuntimeMutationRebaselineStableChunkCount);
			ResetRuntimeMutationRebaselineState(true);
			return;
		}

		mRuntimeMutationRebaselineBuildStaticSceneTlasMs = DurationMs(start, std::chrono::steady_clock::now());
		mRuntimeMutationRebaselineState = RuntimeMutationRebaselineState::ReadyToSwap;
		TraceRuntimeMutationRebaselineProgress("static_scene_tlas_ready");
		break;
	}
	case RuntimeMutationRebaselineState::ReadyToSwap:
	{
		auto start = std::chrono::steady_clock::now();
		if (!SwapRuntimeMutationRebaselineCandidate())
		{
			Printf(TEXTCOLOR_RED "NRI PT runtime mutation rebaseline: swap failed for %s active_chunks=%u stable_chunks=%u.\n",
				currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)",
				mPendingRuntimeMutationRebaselineActiveChunkCount,
				mPendingRuntimeMutationRebaselineStableChunkCount);
			ResetRuntimeMutationRebaselineState(true);
			return;
		}

		mRuntimeMutationRebaselineSwapMs = DurationMs(start, std::chrono::steady_clock::now());
		TraceRuntimeMutationRebaselineProgress("complete");
		ResetRuntimeMutationRebaselineState(false);
		break;
	}
	}

	UpdateRuntimeMutationRebaselinePerfStats();
}

bool NRIRenderer::CreatePipelineLayout()
{
	nri::DescriptorRangeDesc samplerRange = {};
	samplerRange.baseRegisterIndex = 0;
	samplerRange.descriptorNum = NRI_SAMPLER_DESCRIPTOR_NUM;
	samplerRange.descriptorType = nri::DescriptorType::SAMPLER;
	samplerRange.shaderStages = NRIComputeStage();

	nri::DescriptorRangeDesc sceneTextureRange = {};
	sceneTextureRange.baseRegisterIndex = 0;
	sceneTextureRange.descriptorNum = NRI_SCENE_DESCRIPTOR_NUM;
	sceneTextureRange.descriptorType = nri::DescriptorType::TEXTURE;
	sceneTextureRange.shaderStages = NRIComputeStage();
	sceneTextureRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc inputRange = {};
	inputRange.baseRegisterIndex = 0;
	inputRange.descriptorNum = NRI_INPUT_DESCRIPTOR_NUM;
	inputRange.descriptorType = nri::DescriptorType::TEXTURE;
	inputRange.shaderStages = NRIComputeStage();
	inputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc sceneDataRange = {};
	sceneDataRange.baseRegisterIndex = 0;
	sceneDataRange.descriptorNum = NRI_SCENE_DATA_DESCRIPTOR_NUM;
	sceneDataRange.descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	sceneDataRange.shaderStages = NRIComputeStage();
	sceneDataRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc outputRange = {};
	outputRange.baseRegisterIndex = 0;
	outputRange.descriptorNum = NRI_OUTPUT_DESCRIPTOR_NUM;
	outputRange.descriptorType = nri::DescriptorType::STORAGE_TEXTURE;
	outputRange.shaderStages = NRIComputeStage();
	outputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorSetDesc descriptorSets[5] = {};
	descriptorSets[0].registerSpace = 0;
	descriptorSets[0].ranges = &samplerRange;
	descriptorSets[0].rangeNum = 1;
	descriptorSets[1].registerSpace = 1;
	descriptorSets[1].ranges = &sceneTextureRange;
	descriptorSets[1].rangeNum = 1;
	descriptorSets[1].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[2].registerSpace = 2;
	descriptorSets[2].ranges = &sceneDataRange;
	descriptorSets[2].rangeNum = 1;
	descriptorSets[2].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[3].registerSpace = 3;
	descriptorSets[3].ranges = &inputRange;
	descriptorSets[3].rangeNum = 1;
	descriptorSets[3].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[4].registerSpace = 4;
	descriptorSets[4].ranges = &outputRange;
	descriptorSets[4].rangeNum = 1;
	descriptorSets[4].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;

	nri::RootConstantDesc rootConstant = {};
	rootConstant.registerIndex = 0;
	rootConstant.size = sizeof(NRITraceSceneConstants);
	rootConstant.shaderStages = NRIComputeStage();

	nri::RootDescriptorDesc rootDescriptors[1] = {};
	rootDescriptors[0].registerIndex = 0;
	rootDescriptors[0].shaderStages = NRIComputeStage();
	rootDescriptors[0].descriptorType = nri::DescriptorType::ACCELERATION_STRUCTURE;

	nri::PipelineLayoutDesc desc = {};
	desc.rootRegisterSpace = 5;
	desc.rootConstants = &rootConstant;
	desc.rootConstantNum = 1;
	desc.rootDescriptors = rootDescriptors;
	desc.rootDescriptorNum = (uint32_t)std::size(rootDescriptors);
	desc.descriptorSets = descriptorSets;
	desc.descriptorSetNum = (uint32_t)std::size(descriptorSets);
	desc.shaderStages = NRIComputeStage();

	return mFrameBuffer->mCore.CreatePipelineLayout(*mFrameBuffer->mDevice, desc, mPipelineLayout) == nri::Result::SUCCESS;
}

bool NRIRenderer::CreateTaaPipelineLayout()
{
	nri::DescriptorRangeDesc inputRange = {};
	inputRange.baseRegisterIndex = 0;
	inputRange.descriptorNum = 3;
	inputRange.descriptorType = nri::DescriptorType::TEXTURE;
	inputRange.shaderStages = NRIComputeStage();
	inputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc outputRange = {};
	outputRange.baseRegisterIndex = 0;
	outputRange.descriptorNum = 1;
	outputRange.descriptorType = nri::DescriptorType::STORAGE_TEXTURE;
	outputRange.shaderStages = NRIComputeStage();
	outputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorSetDesc descriptorSets[2] = {};
	descriptorSets[0].registerSpace = 0;
	descriptorSets[0].ranges = &inputRange;
	descriptorSets[0].rangeNum = 1;
	descriptorSets[0].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[1].registerSpace = 1;
	descriptorSets[1].ranges = &outputRange;
	descriptorSets[1].rangeNum = 1;
	descriptorSets[1].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;

	nri::RootConstantDesc rootConstant = {};
	rootConstant.registerIndex = 0;
	rootConstant.size = sizeof(NRITemporalConstants);
	rootConstant.shaderStages = NRIComputeStage();

	nri::PipelineLayoutDesc desc = {};
	desc.rootRegisterSpace = 2;
	desc.rootConstants = &rootConstant;
	desc.rootConstantNum = 1;
	desc.descriptorSets = descriptorSets;
	desc.descriptorSetNum = (uint32_t)std::size(descriptorSets);
	desc.shaderStages = NRIComputeStage();

	return mFrameBuffer->mCore.CreatePipelineLayout(*mFrameBuffer->mDevice, desc, mTaaPipelineLayout) == nri::Result::SUCCESS;
}

bool NRIRenderer::CreatePresentPipelineLayout()
{
	nri::DescriptorRangeDesc inputRange = {};
	inputRange.baseRegisterIndex = 0;
	inputRange.descriptorNum = 3;
	inputRange.descriptorType = nri::DescriptorType::TEXTURE;
	inputRange.shaderStages = NRIComputeStage();
	inputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc outputRange = {};
	outputRange.baseRegisterIndex = 0;
	outputRange.descriptorNum = 1;
	outputRange.descriptorType = nri::DescriptorType::STORAGE_TEXTURE;
	outputRange.shaderStages = NRIComputeStage();
	outputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorSetDesc descriptorSets[2] = {};
	descriptorSets[0].registerSpace = 0;
	descriptorSets[0].ranges = &inputRange;
	descriptorSets[0].rangeNum = 1;
	descriptorSets[0].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[1].registerSpace = 1;
	descriptorSets[1].ranges = &outputRange;
	descriptorSets[1].rangeNum = 1;
	descriptorSets[1].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;

	nri::RootConstantDesc rootConstant = {};
	rootConstant.registerIndex = 0;
	rootConstant.size = sizeof(NRIPresentConstants);
	rootConstant.shaderStages = NRIComputeStage();

	nri::PipelineLayoutDesc desc = {};
	desc.rootRegisterSpace = 2;
	desc.rootConstants = &rootConstant;
	desc.rootConstantNum = 1;
	desc.descriptorSets = descriptorSets;
	desc.descriptorSetNum = (uint32_t)std::size(descriptorSets);
	desc.shaderStages = NRIComputeStage();

	return mFrameBuffer->mCore.CreatePipelineLayout(*mFrameBuffer->mDevice, desc, mPresentPipelineLayout) == nri::Result::SUCCESS;
}

bool NRIRenderer::CreatePipelines()
{
	auto createPipeline = [this](const char* fileName, PipelineSlot slot, nri::PipelineLayout* layout)
	{
		std::vector<uint8_t> shaderBlob;
		if (!mFrameBuffer->LoadShaderBlob(fileName, shaderBlob))
		{
			return false;
		}

		nri::ShaderDesc shader = {};
		shader.stage = nri::StageBits::COMPUTE_SHADER;
		shader.bytecode = shaderBlob.data();
		shader.size = shaderBlob.size();
		shader.entryPointName = "main";

		nri::ComputePipelineDesc pipelineDesc = {};
		pipelineDesc.pipelineLayout = layout;
		pipelineDesc.shader = shader;
		return mFrameBuffer->mCore.CreateComputePipeline(*mFrameBuffer->mDevice, pipelineDesc, mPipelines[(size_t)slot]) == nri::Result::SUCCESS;
	};

	const bool d3d12 = mFrameBuffer->GetSelectedAPI() == nri::GraphicsAPI::D3D12;
	const char* suffix = d3d12 ? "dxil" : "spirv";

	FString trace = FStringf("TraceOpaque.cs.%s", suffix);
	FString composition = FStringf("Composition.cs.%s", suffix);
	FString traceTransparent = FStringf("TraceTransparent.cs.%s", suffix);
	FString taa = FStringf("Taa.cs.%s", suffix);
	FString rawPresent = FStringf("RawPresent.cs.%s", suffix);
	FString finalPresent = FStringf("FinalPresent.cs.%s", suffix);
	FString dlssSrBefore = FStringf("DlssSrBefore.cs.%s", suffix);
	FString dlssBefore = FStringf("DlssBefore.cs.%s", suffix);
	FString dlssAfter = FStringf("DlssAfter.cs.%s", suffix);
	FString final = FStringf("Final.cs.%s", suffix);

	return
		createPipeline(trace.GetChars(), PipelineSlot::TraceOpaque, mPipelineLayout) &&
		createPipeline(composition.GetChars(), PipelineSlot::Composition, mPipelineLayout) &&
		createPipeline(traceTransparent.GetChars(), PipelineSlot::TraceTransparent, mPipelineLayout) &&
		createPipeline(taa.GetChars(), PipelineSlot::Taa, mTaaPipelineLayout) &&
		createPipeline(rawPresent.GetChars(), PipelineSlot::RawPresent, mPresentPipelineLayout) &&
		createPipeline(finalPresent.GetChars(), PipelineSlot::FinalPresent, mPresentPipelineLayout) &&
		createPipeline(dlssSrBefore.GetChars(), PipelineSlot::DlssSrBefore, mPipelineLayout) &&
		createPipeline(dlssBefore.GetChars(), PipelineSlot::DlssBefore, mPipelineLayout) &&
		createPipeline(dlssAfter.GetChars(), PipelineSlot::DlssAfter, mPipelineLayout) &&
		createPipeline(final.GetChars(), PipelineSlot::Final, mPipelineLayout);
}

bool NRIRenderer::AllocateDescriptorSets()
{
	const uint32_t queuedFrameCount = mFrameBuffer != nullptr ? std::max(1u, (uint32_t)mFrameBuffer->mQueuedFrames.size()) : 1u;
	mSceneTextureSets.assign(queuedFrameCount, nullptr);
	mSceneDataSets.assign(queuedFrameCount, nullptr);
	mSceneDataDescriptorsInitialized.assign(queuedFrameCount, 0u);

	auto allocateQueuedSets = [&](nri::PipelineLayout* layout, uint32_t setIndex, std::vector<nri::DescriptorSet*>& sets) -> bool
	{
		for (nri::DescriptorSet*& set : sets)
		{
			if (mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *layout, setIndex, &set, 1, 0) != nri::Result::SUCCESS)
			{
				return false;
			}
		}

		return true;
	};

	return
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 0, &mSamplerSet, 1, 0) == nri::Result::SUCCESS &&
		allocateQueuedSets(mPipelineLayout, 1, mSceneTextureSets) &&
		allocateQueuedSets(mPipelineLayout, 2, mSceneDataSets) &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 3, &mFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 4, &mOutputSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 3, &mCompositionFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 4, &mCompositionOutputSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 3, &mUpscalerPrepassFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 4, &mUpscalerPrepassOutputSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mTaaPipelineLayout, 0, &mTaaFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mTaaPipelineLayout, 1, &mTaaOutputSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPresentPipelineLayout, 0, &mRawPresentFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPresentPipelineLayout, 1, &mRawPresentOutputSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPresentPipelineLayout, 0, &mFinalPresentFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPresentPipelineLayout, 1, &mFinalPresentOutputSet, 1, 0) == nri::Result::SUCCESS;
}

bool NRIRenderer::UpdateSamplerSet()
{
	const nri::Descriptor* descriptors[NRI_SAMPLER_DESCRIPTOR_NUM] = {
		mFrameBuffer->mSamplers[(size_t)NRISamplerMode::WrapLinear],
		mFrameBuffer->mSamplers[(size_t)NRISamplerMode::ClampLinear],
		mFrameBuffer->mSamplers[(size_t)NRISamplerMode::WrapPoint],
		mFrameBuffer->mSamplers[(size_t)NRISamplerMode::ClampPoint]
	};
	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = mSamplerSet;
	update.rangeIndex = 0;
	update.descriptors = descriptors;
	update.descriptorNum = NRI_SAMPLER_DESCRIPTOR_NUM;
	mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	return true;
}

bool NRIRenderer::UpdateSceneTextureSet(const std::vector<nri::Descriptor*>& descriptors, const char* reason)
{
	nri::DescriptorSet* sceneTextureSet = GetCurrentSceneTextureSet();
	if (sceneTextureSet == nullptr)
	{
		return false;
	}

	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = sceneTextureSet;
	update.rangeIndex = 0;
	update.descriptors = reinterpret_cast<const nri::Descriptor* const*>(descriptors.data());
	update.descriptorNum = (uint32_t)descriptors.size();
	mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	TraceSharedDescriptorRewrite(
		"scene_textures",
		reason != nullptr ? reason : "unlabeled",
		HashDescriptorList(reinterpret_cast<const nri::Descriptor* const*>(descriptors.data()), descriptors.size()),
		(uint32_t)descriptors.size(),
		true);
	return true;
}

void NRIRenderer::BuildRuntimePointLightUpload(std::vector<RuntimePointLightGpuData>& outLights) const
{
	const auto& activeLights = mSceneLights.GetAnalyticLights().activeLights;
	outLights.clear();
	outLights.reserve(activeLights.size());
	for (const SceneLightSystem::SceneAnalyticLight& light : activeLights)
	{
		RuntimePointLightGpuData gpuLight = {};
		Copy3(light.position, gpuLight.position);
		gpuLight.radius = light.radius;
		Copy3(light.color, gpuLight.color);
		gpuLight.intensity = light.intensity;
		outLights.push_back(gpuLight);
	}
}

uint64_t NRIRenderer::BuildRuntimeLightPayloadHash() const
{
	const auto& activeLights = mSceneLights.GetAnalyticLights().activeLights;
	uint64_t hash = 1469598103934665603ull;
	hash = HashCombine64(hash, (uint64_t)activeLights.size());
	for (const SceneLightSystem::SceneAnalyticLight& light : activeLights)
	{
		hash = HashCombine64(hash, (uint64_t)FloatBits(light.position[0]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(light.position[1]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(light.position[2]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(light.color[0]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(light.color[1]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(light.color[2]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(light.intensity));
		hash = HashCombine64(hash, (uint64_t)FloatBits(light.radius));
	}

	return hash;
}

uint64_t NRIRenderer::BuildRuntimeLightClusterCameraHash() const
{
	uint64_t hash = 1469598103934665603ull;
	hash = HashCombine64(hash, (uint64_t)mRenderWidth);
	hash = HashCombine64(hash, (uint64_t)mRenderHeight);

	if (mSceneLights.GetAnalyticLights().activeLights.empty())
	{
		return hash;
	}

	hash = HashCombine64(hash, (uint64_t)FloatBits(mCurrentCameraPos[0]));
	hash = HashCombine64(hash, (uint64_t)FloatBits(mCurrentCameraPos[1]));
	hash = HashCombine64(hash, (uint64_t)FloatBits(mCurrentCameraPos[2]));
	hash = HashCombine64(hash, (uint64_t)FloatBits(mCurrentCameraForward[0]));
	hash = HashCombine64(hash, (uint64_t)FloatBits(mCurrentCameraForward[1]));
	hash = HashCombine64(hash, (uint64_t)FloatBits(mCurrentCameraForward[2]));
	hash = HashCombine64(hash, (uint64_t)FloatBits(mCurrentCameraRight[0]));
	hash = HashCombine64(hash, (uint64_t)FloatBits(mCurrentCameraRight[1]));
	hash = HashCombine64(hash, (uint64_t)FloatBits(mCurrentCameraRight[2]));
	hash = HashCombine64(hash, (uint64_t)FloatBits(mCurrentCameraUp[0]));
	hash = HashCombine64(hash, (uint64_t)FloatBits(mCurrentCameraUp[1]));
	hash = HashCombine64(hash, (uint64_t)FloatBits(mCurrentCameraUp[2]));
	hash = HashCombine64(hash, (uint64_t)FloatBits(mCurrentTanHalfFovX));
	hash = HashCombine64(hash, (uint64_t)FloatBits(mCurrentTanHalfFovY));
	return hash;
}

void NRIRenderer::BuildEmissiveSamplingUpload(
	const EmissiveSamplingBuildContext& context,
	EmissivePrimitiveHeaderGpuData& outHeader,
	std::vector<EmissivePrimitiveGpuData>& outPrimitives,
	std::vector<float>& outCdf,
	std::vector<EmissivePrimitiveDebugRecord>& outDebugRecords) const
{
	outHeader = {};
	outHeader.dominantIndex = UINT32_MAX;
	outHeader.flags = nri_ptemissiveautoonly ? NRI_EMISSIVE_SAMPLING_FLAG_AUTO_ONLY : 0u;
	outPrimitives.clear();
	outCdf.clear();
	outDebugRecords.clear();

	struct MaterialPrimitiveRange
	{
		uint32_t first = UINT32_MAX;
		uint32_t count = 0;
	};

	struct BuiltCandidate
	{
		EmissivePrimitiveGpuData gpu = {};
		EmissivePrimitiveDebugRecord debug = {};
	};

	auto buildRanges = [](const nri_scene::GeometryData* geometry, std::vector<MaterialPrimitiveRange>& outRanges)
	{
		outRanges.clear();
		if (geometry == nullptr)
		{
			return;
		}

		uint32_t maxMaterialIndex = 0;
		for (const auto& primitive : geometry->primitives)
		{
			maxMaterialIndex = std::max(maxMaterialIndex, primitive.materialIndex);
		}

		outRanges.assign((size_t)maxMaterialIndex + 1u, {});
		for (uint32_t primitiveIndex = 0; primitiveIndex < geometry->primitives.size(); ++primitiveIndex)
		{
			const uint32_t materialIndex = geometry->primitives[primitiveIndex].materialIndex;
			auto& range = outRanges[materialIndex];
			if (range.count == 0)
			{
				range.first = primitiveIndex;
			}
			range.count++;
		}
	};

	std::vector<MaterialPrimitiveRange> staticRanges;
	std::vector<MaterialPrimitiveRange> capturedRanges;
	std::vector<MaterialPrimitiveRange> runtimeMutationRanges;
	std::vector<MaterialPrimitiveRange> dynamicRanges;
	buildRanges(context.staticGeometry, staticRanges);
	buildRanges(context.capturedGeometry, capturedRanges);
	buildRanges(context.runtimeMutationGeometry, runtimeMutationRanges);
	buildRanges(context.dynamicGeometry, dynamicRanges);

	std::vector<BuiltCandidate> candidates;
	const auto& activeSurfaces = mSceneLights.GetEmissiveSurfaces().activeSurfaces;
	candidates.reserve(activeSurfaces.size());

	auto appendSurfacePrimitives = [&](const SceneLightSystem::EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface, const nri_scene::GeometryData* geometry, const std::vector<MaterialPrimitiveRange>& ranges, uint32_t dataSource, uint32_t primitiveBase)
	{
		if (geometry == nullptr || surface.materialIndex == UINT32_MAX || surface.materialIndex >= ranges.size())
		{
			return;
		}

		const auto& range = ranges[surface.materialIndex];
		if (range.count == 0 || range.first == UINT32_MAX)
		{
			return;
		}

		float representativeLuminance = 0.0f;
		if (surface.surfaceArea > 0.0f && surface.emissiveIntensity > 0.0f)
		{
			representativeLuminance = std::max(surface.powerEstimate / (surface.surfaceArea * surface.emissiveIntensity), 0.0f);
		}
		const float samplingScale = ResolveGlowSamplingScale(surface.sourceFlags, surface.emissiveMode);

		for (uint32_t localOffset = 0; localOffset < range.count; ++localOffset)
		{
			const uint32_t localPrimitiveIndex = range.first + localOffset;
			const uint32_t primitiveIndex = primitiveBase + localPrimitiveIndex;
			const float primitiveArea = ComputePrimitiveArea(*geometry, localPrimitiveIndex);
			if (primitiveArea <= 0.0f)
			{
				continue;
			}

			BuiltCandidate candidate = {};
			candidate.gpu.dataSource = dataSource;
			candidate.gpu.primitiveIndex = primitiveIndex;
			candidate.gpu.sourceFlags = surface.sourceFlags;
			candidate.gpu.textureId = surface.textureId;
			candidate.gpu.primitiveArea = primitiveArea;
			candidate.gpu.powerEstimate = std::max(primitiveArea * representativeLuminance * surface.emissiveIntensity, 0.0f);
			candidate.gpu.selectionWeight = candidate.gpu.powerEstimate * samplingScale;

			candidate.debug.stableKey = HashCombine64(surface.stableKey, ((uint64_t)dataSource << 32u) | primitiveIndex);
			candidate.debug.surfaceStableKey = surface.stableKey;
			candidate.debug.dataSource = dataSource;
			candidate.debug.primitiveIndex = primitiveIndex;
			candidate.debug.materialIndex = surface.materialIndex;
			candidate.debug.sourceFlags = surface.sourceFlags;
			candidate.debug.sourceRuleId = surface.sourceRuleId;
			candidate.debug.textureId = surface.textureId;
			candidate.debug.emissiveMode = surface.emissiveMode;
			candidate.debug.emissiveTextureIndex = surface.emissiveTextureIndex;
			candidate.debug.actorIndex = surface.actorIndex;
			candidate.debug.primitiveArea = primitiveArea;
			candidate.debug.powerEstimate = candidate.gpu.powerEstimate;
			candidate.debug.selectionWeight = candidate.gpu.selectionWeight;
			candidate.debug.selectionPdf = 0.0f;
			candidate.debug.emissiveIntensity = surface.emissiveIntensity;
			Copy3(surface.emissiveColor, candidate.debug.emissiveColor);
			ComputePrimitiveCenter(*geometry, localPrimitiveIndex, candidate.debug.center);

			candidate.gpu.stableKeyLo = (uint32_t)(candidate.debug.stableKey & 0xffffffffu);
			candidate.gpu.stableKeyHi = (uint32_t)(candidate.debug.stableKey >> 32u);
			candidates.push_back(candidate);
		}
	};

	for (const auto& surface : activeSurfaces)
	{
		if (nri_ptemissiveautoonly && !HasAutoEmissiveSourceFlags(surface.sourceFlags))
		{
			continue;
		}

		switch (surface.source)
		{
		case SceneLightRecordSource::StaticMapScene:
			appendSurfacePrimitives(surface, context.staticGeometry, staticRanges, NRI_SCENE_DATA_SOURCE_STATIC, 0u);
			break;
		case SceneLightRecordSource::CapturedScene:
			appendSurfacePrimitives(surface, context.capturedGeometry, capturedRanges, NRI_SCENE_DATA_SOURCE_DYNAMIC, 0u);
			break;
		case SceneLightRecordSource::RuntimeMutationScene:
			appendSurfacePrimitives(surface, context.runtimeMutationGeometry, runtimeMutationRanges, NRI_SCENE_DATA_SOURCE_DYNAMIC, context.runtimeMutationPrimitiveBaseOffset);
			break;
		case SceneLightRecordSource::DynamicScene:
			appendSurfacePrimitives(surface, context.dynamicGeometry, dynamicRanges, NRI_SCENE_DATA_SOURCE_DYNAMIC, context.dynamicPrimitiveBaseOffset);
			break;
		default:
			break;
		}
	}

	if (candidates.size() > NRI_MAX_EMISSIVE_PRIMITIVES)
	{
		std::stable_sort(candidates.begin(), candidates.end(), [](const BuiltCandidate& a, const BuiltCandidate& b)
		{
			if (a.gpu.selectionWeight != b.gpu.selectionWeight)
			{
				return a.gpu.selectionWeight > b.gpu.selectionWeight;
			}

			return a.debug.stableKey < b.debug.stableKey;
		});
		candidates.resize(NRI_MAX_EMISSIVE_PRIMITIVES);
	}

	outPrimitives.reserve(candidates.size());
	outDebugRecords.reserve(candidates.size());

	float totalPower = 0.0f;
	float totalSelectionWeight = 0.0f;
	float dominantPower = -1.0f;
	uint32_t dominantTile = 0;
	uint32_t dominantFlags = 0;
	uint32_t dominantPrimitive = UINT32_MAX;
	uint32_t dominantDataSource = 0;

	for (size_t i = 0; i < candidates.size(); ++i)
	{
		outPrimitives.push_back(candidates[i].gpu);
		outDebugRecords.push_back(candidates[i].debug);
		totalPower += candidates[i].gpu.powerEstimate;
		totalSelectionWeight += candidates[i].gpu.selectionWeight;
		if (candidates[i].gpu.powerEstimate > dominantPower)
		{
			dominantPower = candidates[i].gpu.powerEstimate;
			outHeader.dominantIndex = (uint32_t)i;
			dominantTile = candidates[i].gpu.textureId;
			dominantFlags = candidates[i].gpu.sourceFlags;
			dominantPrimitive = candidates[i].gpu.primitiveIndex;
			dominantDataSource = candidates[i].gpu.dataSource;
		}
	}

	outHeader.activeCount = (uint32_t)outPrimitives.size();
	outHeader.totalPower = totalPower;

	if (outPrimitives.empty())
	{
		outCdf.resize(1, 1.0f);
		return;
	}

	float runningCdf = 0.0f;
	const float invTotalSelectionWeight = totalSelectionWeight > 0.0f ? (1.0f / totalSelectionWeight) : 0.0f;
	for (size_t i = 0; i < outPrimitives.size(); ++i)
	{
		float pdf = 0.0f;
		if (totalSelectionWeight > 0.0f)
		{
			pdf = outPrimitives[i].selectionWeight * invTotalSelectionWeight;
		}
		else
		{
			pdf = 1.0f / (float)outPrimitives.size();
		}

		outPrimitives[i].selectionPdf = pdf;
		outDebugRecords[i].selectionPdf = pdf;
		runningCdf += pdf;
		outCdf.push_back(i + 1 == outPrimitives.size() ? 1.0f : std::min(runningCdf, 1.0f));
	}
}

void NRIRenderer::BuildSectorLightingUpload(
	SectorLightHeaderGpuData& outHeader,
	std::vector<SectorLightGpuData>& outSectors)
{
	const auto& registry = mSceneLights.GetSectorLighting();
	const float sectorLightMultiplier = GetSectorLightMultiplier();
	UpdateBoundSectorLightingState();
	outHeader = {};
	outHeader.sectorCount = registry.sectorCount;
	outHeader.activeCount = registry.activeSectorCount;
	outHeader.pulsingCount = registry.pulsingSectorCount;
	outHeader.flags = nri_ptsectorlighting ? NRI_SECTOR_LIGHTING_FLAG_ENABLED : 0u;
	outSectors.assign(registry.sectorCount, {});

	for (uint32_t sectorIndex : registry.activeSectorIndices)
	{
		if (sectorIndex >= registry.sectors.size() || sectorIndex >= outSectors.size())
		{
			continue;
		}

		const auto& source = registry.sectors[sectorIndex];
		auto& target = outSectors[sectorIndex];
		Copy3(source.ambientColor, target.ambientColor);
		Copy3(source.ambientColor, target.hemisphereColor);
		target.ambientIntensity = source.ambientIntensity * sectorLightMultiplier;
		target.hemisphereAmount = source.hemisphereAmount * sectorLightMultiplier;
		target.fogAmount = source.fogAmount * sectorLightMultiplier;
		target.pulseScale = source.pulseScale;
		target.sourceFlags = source.sourceFlags;
		target.paletteIndex = source.paletteIndex;
		target.lotag = source.lotag;
		target.hitag = source.hitag;
	}
}

uint64_t NRIRenderer::BuildEmissiveSamplingPayloadHash(const EmissiveSamplingBuildContext& context) const
{
	uint64_t hash = 1469598103934665603ull;
	hash = HashCombine64(hash, nri_ptemissiveautoonly ? 1ull : 0ull);
	hash = HashCombine64(hash, HashGeometryForEmissiveSampling(context.staticGeometry));
	hash = HashCombine64(hash, HashGeometryForEmissiveSampling(context.capturedGeometry));
	hash = HashCombine64(hash, HashGeometryForEmissiveSampling(context.runtimeMutationGeometry));
	hash = HashCombine64(hash, (uint64_t)context.runtimeMutationPrimitiveBaseOffset);
	hash = HashCombine64(hash, HashGeometryForEmissiveSampling(context.dynamicGeometry));
	hash = HashCombine64(hash, (uint64_t)context.dynamicPrimitiveBaseOffset);

	const auto& emissiveRegistry = mSceneLights.GetEmissiveSurfaces();
	hash = HashCombine64(hash, (uint64_t)emissiveRegistry.activeSurfaces.size());
	for (const auto& surface : emissiveRegistry.activeSurfaces)
	{
		hash = HashCombine64(hash, surface.stableKey);

		const auto propertyIt = emissiveRegistry.activePropertyHashes.find(surface.stableKey);
		hash = HashCombine64(hash, propertyIt != emissiveRegistry.activePropertyHashes.end() ? propertyIt->second : 0ull);

		const auto bindingIt = emissiveRegistry.activeBindingHashes.find(surface.stableKey);
		hash = HashCombine64(hash, bindingIt != emissiveRegistry.activeBindingHashes.end() ? bindingIt->second : 0ull);
	}

	return hash;
}

uint64_t NRIRenderer::BuildSectorLightingPayloadHash() const
{
	const auto& registry = mSceneLights.GetSectorLighting();
	uint64_t hash = 1469598103934665603ull;
	hash = HashCombine64(hash, nri_ptsectorlighting ? 1ull : 0ull);
	hash = HashCombine64(hash, (uint64_t)FloatBits(GetSectorLightMultiplier()));
	hash = HashCombine64(hash, (uint64_t)registry.sectorCount);
	hash = HashCombine64(hash, (uint64_t)registry.activeSectorCount);
	hash = HashCombine64(hash, (uint64_t)registry.pulsingSectorCount);
	for (uint32_t sectorIndex : registry.activeSectorIndices)
	{
		hash = HashCombine64(hash, (uint64_t)sectorIndex);
		if (sectorIndex >= registry.sectors.size())
		{
			continue;
		}

		const auto& sector = registry.sectors[sectorIndex];
		hash = HashCombine64(hash, (uint64_t)sector.sourceFlags);
		hash = HashCombine64(hash, (uint64_t)(int64_t)sector.paletteIndex);
		hash = HashCombine64(hash, (uint64_t)(int64_t)sector.lotag);
		hash = HashCombine64(hash, (uint64_t)(int64_t)sector.hitag);
		hash = HashCombine64(hash, (uint64_t)(int64_t)sector.averageShade);
		hash = HashCombine64(hash, (uint64_t)FloatBits(sector.ambientColor[0]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(sector.ambientColor[1]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(sector.ambientColor[2]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(sector.ambientIntensity));
		hash = HashCombine64(hash, (uint64_t)FloatBits(sector.hemisphereAmount));
		hash = HashCombine64(hash, (uint64_t)FloatBits(sector.fogAmount));
		hash = HashCombine64(hash, (uint64_t)FloatBits(sector.pulseScale));
	}

	return hash;
}

void NRIRenderer::UpdateBoundSectorLightingState()
{
	const auto& registry = mSceneLights.GetSectorLighting();
	const float sectorLightMultiplier = GetSectorLightMultiplier();
	mBoundSectorLightSectorCount = registry.sectorCount;
	mBoundSectorLightActiveCount = registry.activeSectorCount;
	mBoundSectorLightPulsingCount = registry.pulsingSectorCount;
	mBoundSectorLightDominantSector = UINT32_MAX;
	mBoundSectorLightDominantContribution = 0.0f;

	for (uint32_t sectorIndex : registry.activeSectorIndices)
	{
		if (sectorIndex >= registry.sectors.size())
		{
			continue;
		}

		const auto& sector = registry.sectors[sectorIndex];
		const float contribution = sectorLightMultiplier * (sector.ambientIntensity + std::abs(sector.hemisphereAmount) + sector.fogAmount);
		if (contribution > mBoundSectorLightDominantContribution)
		{
			mBoundSectorLightDominantContribution = contribution;
			mBoundSectorLightDominantSector = sectorIndex;
		}
	}
}

bool NRIRenderer::UpdateEmissiveSamplingBuffers(const EmissiveSamplingBuildContext& context)
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.emissiveUpdateMs);
	const uint64_t payloadHash = BuildEmissiveSamplingPayloadHash(context);
	if (mEmissiveSamplingPayloadCacheValid &&
		mEmissiveSamplingPayloadHash == payloadHash &&
		mEmissivePrimitiveHeaderBuffer.shaderView != nullptr &&
		mEmissivePrimitiveBuffer.shaderView != nullptr &&
		mEmissivePrimitiveCdfBuffer.shaderView != nullptr)
	{
		return true;
	}

	EmissivePrimitiveHeaderGpuData emissiveHeader = {};
	std::vector<EmissivePrimitiveGpuData> emissivePrimitives;
	std::vector<float> emissiveCdf;
	std::vector<EmissivePrimitiveDebugRecord> emissiveDebugRecords;
	BuildEmissiveSamplingUpload(context, emissiveHeader, emissivePrimitives, emissiveCdf, emissiveDebugRecords);

	if (!EnsureStructuredBuffer(
		mEmissivePrimitiveHeaderBuffer,
		mEmissivePrimitiveHeaderBufferStats,
		&emissiveHeader,
		sizeof(emissiveHeader),
		sizeof(EmissivePrimitiveHeaderGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!EnsureStructuredBuffer(
		mEmissivePrimitiveBuffer,
		mEmissivePrimitiveBufferStats,
		emissivePrimitives.empty() ? nullptr : emissivePrimitives.data(),
		emissivePrimitives.empty() ? 0u : emissivePrimitives.size() * sizeof(EmissivePrimitiveGpuData),
		sizeof(EmissivePrimitiveGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!EnsureStructuredBuffer(
		mEmissivePrimitiveCdfBuffer,
		mEmissivePrimitiveCdfBufferStats,
		emissiveCdf.data(),
		emissiveCdf.size() * sizeof(float),
		sizeof(float),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	mBoundEmissivePrimitiveCount = emissiveHeader.activeCount;
	mBoundEmissiveTotalPower = emissiveHeader.totalPower;
	mBoundEmissiveDominantPrimitive = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissiveDebugRecords.size() ? emissiveDebugRecords[emissiveHeader.dominantIndex].primitiveIndex : UINT32_MAX;
	mBoundEmissiveDominantTile = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].textureId : 0u;
	mBoundEmissiveDominantFlags = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].sourceFlags : 0u;
	mBoundEmissiveDominantDataSource = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].dataSource : 0u;
	mBoundEmissiveDominantPower = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].powerEstimate : 0.0f;
	mBoundEmissivePrimitiveRecords = std::move(emissiveDebugRecords);

	mSceneDataDescriptors[13] = mEmissivePrimitiveHeaderBuffer.shaderView;
	mSceneDataDescriptors[14] = mEmissivePrimitiveBuffer.shaderView;
	mSceneDataDescriptors[15] = mEmissivePrimitiveCdfBuffer.shaderView;

	bool descriptorsReady = IsCurrentSceneDataDescriptorsInitialized() && GetCurrentSceneDataSet() != nullptr;
	if (descriptorsReady)
	{
		for (const nri::Descriptor* descriptor : mSceneDataDescriptors)
		{
			if (descriptor == nullptr)
			{
				descriptorsReady = false;
				break;
			}
		}
	}

	if (descriptorsReady)
	{
		CommitSceneDataDescriptors("emissive_sampling_refresh");
	}
	mEmissiveSamplingPayloadCacheValid = true;
	mEmissiveSamplingPayloadHash = payloadHash;
	return true;
}

bool NRIRenderer::UpdateReprojectionBuffer()
{
	NRIReprojectionData data = {};
	std::memcpy(data.currentViewToClip, mCurrentViewToClip, sizeof(data.currentViewToClip));
	std::memcpy(data.previousViewToClip, mPreviousViewToClip, sizeof(data.previousViewToClip));
	std::memcpy(data.currentWorldToView, mCurrentWorldToView, sizeof(data.currentWorldToView));
	std::memcpy(data.previousWorldToView, mPreviousWorldToView, sizeof(data.previousWorldToView));
	if (!EnsureStructuredBuffer(
		mReprojectionBuffer,
		mReprojectionBufferStats,
		&data,
		sizeof(data),
		sizeof(data),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	if (mSceneDataDescriptors[18] != mReprojectionBuffer.shaderView)
	{
		mSceneDataDescriptors[18] = mReprojectionBuffer.shaderView;
		bool descriptorsReady = IsCurrentSceneDataDescriptorsInitialized() && GetCurrentSceneDataSet() != nullptr;
		for (const nri::Descriptor* descriptor : mSceneDataDescriptors)
		{
			if (descriptor == nullptr)
			{
				descriptorsReady = false;
				break;
			}
		}

		if (descriptorsReady)
		{
			CommitSceneDataDescriptors("reprojection_refresh");
		}
	}

	return true;
}

bool NRIRenderer::UpdateVisibleChunkBuffer()
{
	const uint32_t defaultVisibleChunkWord = 0u;
	const void* visibleChunkData = mCurrentVisibleChunkWords.empty() ? (const void*)&defaultVisibleChunkWord : mCurrentVisibleChunkWords.data();
	const size_t visibleChunkSize = mCurrentVisibleChunkWords.empty() ? sizeof(uint32_t) : mCurrentVisibleChunkWords.size() * sizeof(uint32_t);
	if (!EnsureStructuredBuffer(
		mVisibleChunkBuffer,
		mVisibleChunkBufferStats,
		visibleChunkData,
		visibleChunkSize,
		sizeof(uint32_t),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	if (mSceneDataDescriptors[19] != mVisibleChunkBuffer.shaderView)
	{
		mSceneDataDescriptors[19] = mVisibleChunkBuffer.shaderView;
		bool descriptorsReady = IsCurrentSceneDataDescriptorsInitialized() && GetCurrentSceneDataSet() != nullptr;
		for (const nri::Descriptor* descriptor : mSceneDataDescriptors)
		{
			if (descriptor == nullptr)
			{
				descriptorsReady = false;
				break;
			}
		}

		if (descriptorsReady)
		{
			CommitSceneDataDescriptors("visible_chunk_refresh");
		}
	}

	return true;
}

bool NRIRenderer::UpdateVisibleFlatPlaneBuffer()
{
	const uint32_t defaultVisibleFlatPlaneWord = 0u;
	const void* visibleFlatPlaneData = mCurrentVisibleFlatPlaneWords.empty() ? (const void*)&defaultVisibleFlatPlaneWord : mCurrentVisibleFlatPlaneWords.data();
	const size_t visibleFlatPlaneSize = mCurrentVisibleFlatPlaneWords.empty() ? sizeof(uint32_t) : mCurrentVisibleFlatPlaneWords.size() * sizeof(uint32_t);
	if (!EnsureStructuredBuffer(
		mVisibleFlatPlaneBuffer,
		mVisibleFlatPlaneBufferStats,
		visibleFlatPlaneData,
		visibleFlatPlaneSize,
		sizeof(uint32_t),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	if (mSceneDataDescriptors[20] != mVisibleFlatPlaneBuffer.shaderView)
	{
		mSceneDataDescriptors[20] = mVisibleFlatPlaneBuffer.shaderView;
		bool descriptorsReady = IsCurrentSceneDataDescriptorsInitialized() && GetCurrentSceneDataSet() != nullptr;
		for (const nri::Descriptor* descriptor : mSceneDataDescriptors)
		{
			if (descriptor == nullptr)
			{
				descriptorsReady = false;
				break;
			}
		}

		if (descriptorsReady)
		{
			CommitSceneDataDescriptors("visible_flat_refresh");
		}
	}

	return true;
}

void NRIRenderer::BuildRuntimeLightClusterUpload(
	std::vector<RuntimeLightTileHeaderGpuData>& outHeaders,
	std::vector<uint32_t>& outIndices,
	uint32_t& outTileCountX,
	uint32_t& outTileCountY,
	uint32_t& outTileIndexCount,
	uint32_t& outMaxTileOccupancy) const
{
	const auto& activeLights = mSceneLights.GetAnalyticLights().activeLights;
	const uint32_t activeLightCount = (uint32_t)activeLights.size();
	outTileCountX = std::max(1u, (mRenderWidth + NRI_RUNTIME_LIGHT_TILE_SIZE - 1u) / NRI_RUNTIME_LIGHT_TILE_SIZE);
	outTileCountY = std::max(1u, (mRenderHeight + NRI_RUNTIME_LIGHT_TILE_SIZE - 1u) / NRI_RUNTIME_LIGHT_TILE_SIZE);
	const uint32_t tileCount = outTileCountX * outTileCountY;
	const uint32_t maxIndexCapacity = tileCount * NRI_MAX_RUNTIME_POINT_LIGHTS;
	outTileIndexCount = 0;
	outMaxTileOccupancy = 0;
	outHeaders.assign(tileCount, {});
	outIndices.assign(maxIndexCapacity, 0u);

	if (tileCount == 0 || activeLightCount == 0 || mRenderWidth == 0 || mRenderHeight == 0)
	{
		return;
	}

	auto dot3 = [](const float* a, const float* b) -> float
	{
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	};

	std::vector<std::vector<uint32_t>> tileLights(tileCount);
	const bool mirrorExtendedLightCoverage =
		mHasVisibleMirrorPortalLastFrame &&
		nri_ptmirrordynamicdistance > 0.0f;
	const float mirrorExtendedLightDistanceSq =
		mirrorExtendedLightCoverage ? nri_ptmirrordynamicdistance * nri_ptmirrordynamicdistance : 0.0f;
	for (uint32_t lightIndex = 0; lightIndex < activeLightCount; ++lightIndex)
	{
		const SceneLightSystem::SceneAnalyticLight& light = activeLights[lightIndex];
		if (light.intensity <= 0.0f || light.radius <= 0.0f)
		{
			continue;
		}

		const float toLight[3] = {
			light.position[0] - mCurrentCameraPos[0],
			light.position[1] - mCurrentCameraPos[1],
			light.position[2] - mCurrentCameraPos[2]
		};
		const float viewX = dot3(toLight, mCurrentCameraRight);
		const float viewY = dot3(toLight, mCurrentCameraUp);
		const float viewZ = dot3(toLight, mCurrentCameraForward);
		const float lightDistanceSq =
			toLight[0] * toLight[0] +
			toLight[1] * toLight[1] +
			toLight[2] * toLight[2];
		const bool forceMirrorFullscreen =
			mirrorExtendedLightCoverage &&
			lightDistanceSq <= mirrorExtendedLightDistanceSq;
		if (!forceMirrorFullscreen && viewZ <= -light.radius)
		{
			continue;
		}

		int32_t minTileX = 0;
		int32_t minTileY = 0;
		int32_t maxTileX = (int32_t)outTileCountX - 1;
		int32_t maxTileY = (int32_t)outTileCountY - 1;

		if (!forceMirrorFullscreen &&
			viewZ > light.radius &&
			mCurrentTanHalfFovX > 0.0f &&
			mCurrentTanHalfFovY > 0.0f)
		{
			const float conservativeDepth = std::max(viewZ - light.radius, 1.0f);
			const float centerNdcX = viewX / (viewZ * mCurrentTanHalfFovX);
			const float centerNdcY = viewY / (viewZ * mCurrentTanHalfFovY);
			const float radiusNdcX = light.radius / (conservativeDepth * mCurrentTanHalfFovX);
			const float radiusNdcY = light.radius / (conservativeDepth * mCurrentTanHalfFovY);
			const float minPixelX = ((centerNdcX - radiusNdcX) * 0.5f + 0.5f) * (float)mRenderWidth;
			const float maxPixelX = ((centerNdcX + radiusNdcX) * 0.5f + 0.5f) * (float)mRenderWidth;
			const float minPixelY = (0.5f - (centerNdcY + radiusNdcY) * 0.5f) * (float)mRenderHeight;
			const float maxPixelY = (0.5f - (centerNdcY - radiusNdcY) * 0.5f) * (float)mRenderHeight;
			if (maxPixelX < 0.0f || minPixelX >= (float)mRenderWidth || maxPixelY < 0.0f || minPixelY >= (float)mRenderHeight)
			{
				continue;
			}

			minTileX = std::max(0, (int32_t)std::floor(minPixelX / (float)NRI_RUNTIME_LIGHT_TILE_SIZE));
			minTileY = std::max(0, (int32_t)std::floor(minPixelY / (float)NRI_RUNTIME_LIGHT_TILE_SIZE));
			maxTileX = std::min((int32_t)outTileCountX - 1, (int32_t)std::floor(std::max(maxPixelX - 1.0f, 0.0f) / (float)NRI_RUNTIME_LIGHT_TILE_SIZE));
			maxTileY = std::min((int32_t)outTileCountY - 1, (int32_t)std::floor(std::max(maxPixelY - 1.0f, 0.0f) / (float)NRI_RUNTIME_LIGHT_TILE_SIZE));
		}

		if (minTileX > maxTileX || minTileY > maxTileY)
		{
			continue;
		}

		for (int32_t tileY = minTileY; tileY <= maxTileY; ++tileY)
		{
			for (int32_t tileX = minTileX; tileX <= maxTileX; ++tileX)
			{
				tileLights[(size_t)tileY * outTileCountX + (size_t)tileX].push_back(lightIndex);
			}
		}
	}

	uint32_t indexCursor = 0;
	for (uint32_t tileIndex = 0; tileIndex < tileCount; ++tileIndex)
	{
		RuntimeLightTileHeaderGpuData& header = outHeaders[tileIndex];
		const std::vector<uint32_t>& tileLightList = tileLights[tileIndex];
		header.indexOffset = indexCursor;
		header.indexCount = (uint32_t)tileLightList.size();
		outMaxTileOccupancy = std::max(outMaxTileOccupancy, header.indexCount);
		for (uint32_t lightIndex : tileLightList)
		{
			if (indexCursor < outIndices.size())
			{
				outIndices[indexCursor] = lightIndex;
				indexCursor++;
			}
		}
	}

	outTileIndexCount = indexCursor;
}

bool NRIRenderer::UpdateSceneDataSet(
	const NRIBufferResource& staticVertexBuffer,
	const NRIBufferResource& staticIndexBuffer,
	const NRIBufferResource& staticPrimitiveBuffer,
	const NRIBufferResource& staticMaterialBuffer,
	const NRIBufferResource& dynamicVertexBuffer,
	const NRIBufferResource& dynamicIndexBuffer,
	const NRIBufferResource& dynamicPrimitiveBuffer,
	const NRIBufferResource& dynamicMaterialBuffer,
	const std::vector<SceneInstanceData>& sceneInstances,
	uint32_t staticPrimitiveCount,
	uint32_t dynamicPrimitiveCount,
	uint32_t staticMaterialCount,
	uint32_t dynamicMaterialCount,
	const char* reason)
{
	SetCurrentSceneDataDescriptorsInitialized(false);

	if (!UpdateReprojectionBuffer())
	{
		return false;
	}

	if (!UpdateVisibleFlatPlaneBuffer())
	{
		return false;
	}

	if (!UpdateVisibleChunkBuffer())
	{
		return false;
	}

	if (sceneInstances.empty())
	{
		return false;
	}

	mBoundRuntimeLightCount = 0;

	if (!EnsureStructuredBuffer(
		mSceneInstanceBuffer,
		mSceneInstanceBufferStats,
		sceneInstances.data(),
		sceneInstances.size() * sizeof(SceneInstanceData),
		sizeof(SceneInstanceData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}
	mBoundSceneInstances = sceneInstances;

	const std::vector<ScenePortalData> scenePortals = BuildScenePortalData(mMapWorld);
	if (!EnsureStructuredBuffer(
		mPortalBuffer,
		mPortalBufferStats,
		scenePortals.data(),
		scenePortals.size() * sizeof(ScenePortalData),
		sizeof(ScenePortalData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	const uint64_t runtimeLightPayloadHash = BuildRuntimeLightPayloadHash();
	const uint32_t activeRuntimeLightCount = (uint32_t)mSceneLights.GetAnalyticLights().activeLights.size();
	if (!mRuntimeLightPayloadCacheValid ||
		mRuntimeLightPayloadHash != runtimeLightPayloadHash ||
		mRuntimeLightBuffer.shaderView == nullptr)
	{
		std::vector<RuntimePointLightGpuData> runtimeLights;
		BuildRuntimePointLightUpload(runtimeLights);
		if (!EnsureStructuredBuffer(
			mRuntimeLightBuffer,
			mRuntimeLightBufferStats,
			runtimeLights.empty() ? nullptr : runtimeLights.data(),
			runtimeLights.size() * sizeof(RuntimePointLightGpuData),
			sizeof(RuntimePointLightGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess()))
		{
			return false;
		}

		mRuntimeLightPayloadCacheValid = true;
		mRuntimeLightPayloadHash = runtimeLightPayloadHash;
	}

	uint32_t runtimeLightTileCountX = 0;
	uint32_t runtimeLightTileCountY = 0;
	uint32_t runtimeLightTileIndexCount = 0;
	uint32_t runtimeLightMaxTileOccupancy = 0;
	const uint64_t runtimeLightClusterCameraHash = BuildRuntimeLightClusterCameraHash();
	const uint64_t runtimeLightClusterPayloadHash =
		HashCombine64(runtimeLightPayloadHash, runtimeLightClusterCameraHash);
	if (!mRuntimeLightClusterCacheValid ||
		mRuntimeLightClusterPayloadHash != runtimeLightClusterPayloadHash ||
		mRuntimeLightTileHeaderBuffer.shaderView == nullptr ||
		mRuntimeLightTileIndexBuffer.shaderView == nullptr)
	{
		std::vector<RuntimeLightTileHeaderGpuData> runtimeLightTileHeaders;
		std::vector<uint32_t> runtimeLightTileIndices;
		BuildRuntimeLightClusterUpload(
			runtimeLightTileHeaders,
			runtimeLightTileIndices,
			runtimeLightTileCountX,
			runtimeLightTileCountY,
			runtimeLightTileIndexCount,
			runtimeLightMaxTileOccupancy);
		if (!EnsureStructuredBuffer(
			mRuntimeLightTileHeaderBuffer,
			mRuntimeLightTileHeaderBufferStats,
			runtimeLightTileHeaders.data(),
			runtimeLightTileHeaders.size() * sizeof(RuntimeLightTileHeaderGpuData),
			sizeof(RuntimeLightTileHeaderGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess()))
		{
			return false;
		}

		if (!EnsureStructuredBuffer(
			mRuntimeLightTileIndexBuffer,
			mRuntimeLightTileIndexBufferStats,
			runtimeLightTileIndices.data(),
			runtimeLightTileIndices.size() * sizeof(uint32_t),
			sizeof(uint32_t),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess()))
		{
			return false;
		}

		mRuntimeLightClusterCacheValid = true;
		mRuntimeLightClusterPayloadHash = runtimeLightClusterPayloadHash;
		mRuntimeLightClusterCameraHash = runtimeLightClusterCameraHash;
	}
	else
	{
		runtimeLightTileCountX = mBoundRuntimeLightTileCountX;
		runtimeLightTileCountY = mBoundRuntimeLightTileCountY;
		runtimeLightTileIndexCount = mBoundRuntimeLightTileIndexCount;
		runtimeLightMaxTileOccupancy = mBoundRuntimeLightMaxTileOccupancy;
	}

	if (!mEmissiveSamplingPayloadCacheValid ||
		mEmissivePrimitiveHeaderBuffer.shaderView == nullptr ||
		mEmissivePrimitiveBuffer.shaderView == nullptr ||
		mEmissivePrimitiveCdfBuffer.shaderView == nullptr)
	{
		EmissivePrimitiveHeaderGpuData emissiveHeader = {};
		std::vector<EmissivePrimitiveGpuData> emissivePrimitives;
		std::vector<float> emissiveCdf;
		std::vector<EmissivePrimitiveDebugRecord> ignoredEmissiveDebugRecords;
		BuildEmissiveSamplingUpload({}, emissiveHeader, emissivePrimitives, emissiveCdf, ignoredEmissiveDebugRecords);
		if (!EnsureStructuredBuffer(
			mEmissivePrimitiveHeaderBuffer,
			mEmissivePrimitiveHeaderBufferStats,
			&emissiveHeader,
			sizeof(emissiveHeader),
			sizeof(EmissivePrimitiveHeaderGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess()))
		{
			return false;
		}

		if (!EnsureStructuredBuffer(
			mEmissivePrimitiveBuffer,
			mEmissivePrimitiveBufferStats,
			emissivePrimitives.empty() ? nullptr : emissivePrimitives.data(),
			emissivePrimitives.empty() ? 0u : emissivePrimitives.size() * sizeof(EmissivePrimitiveGpuData),
			sizeof(EmissivePrimitiveGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess()))
		{
			return false;
		}

		if (!EnsureStructuredBuffer(
			mEmissivePrimitiveCdfBuffer,
			mEmissivePrimitiveCdfBufferStats,
			emissiveCdf.data(),
			emissiveCdf.size() * sizeof(float),
			sizeof(float),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess()))
		{
			return false;
		}
	}

	UpdateBoundSectorLightingState();
	const uint64_t sectorLightingPayloadHash = BuildSectorLightingPayloadHash();
	if (!mSectorLightingPayloadCacheValid ||
		mSectorLightingPayloadHash != sectorLightingPayloadHash ||
		mSectorLightHeaderBuffer.shaderView == nullptr ||
		mSectorLightBuffer.shaderView == nullptr)
	{
		SectorLightHeaderGpuData sectorLightHeader = {};
		std::vector<SectorLightGpuData> sectorLights;
		BuildSectorLightingUpload(sectorLightHeader, sectorLights);
		if (!EnsureStructuredBuffer(
			mSectorLightHeaderBuffer,
			mSectorLightHeaderBufferStats,
			&sectorLightHeader,
			sizeof(sectorLightHeader),
			sizeof(SectorLightHeaderGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess()))
		{
			return false;
		}

		if (!EnsureStructuredBuffer(
			mSectorLightBuffer,
			mSectorLightBufferStats,
			sectorLights.empty() ? nullptr : sectorLights.data(),
			sectorLights.empty() ? 0u : sectorLights.size() * sizeof(SectorLightGpuData),
			sizeof(SectorLightGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess()))
		{
			return false;
		}

		mSectorLightingPayloadCacheValid = true;
		mSectorLightingPayloadHash = sectorLightingPayloadHash;
	}

	auto selectView = [](const NRIBufferResource& primary, const NRIBufferResource& fallback) -> nri::Descriptor*
	{
		return primary.shaderView != nullptr ? primary.shaderView : fallback.shaderView;
	};

	mSceneDataDescriptors = {
		selectView(staticVertexBuffer, dynamicVertexBuffer),
		selectView(staticIndexBuffer, dynamicIndexBuffer),
		selectView(staticPrimitiveBuffer, dynamicPrimitiveBuffer),
		selectView(staticMaterialBuffer, dynamicMaterialBuffer),
		selectView(dynamicVertexBuffer, staticVertexBuffer),
		selectView(dynamicIndexBuffer, staticIndexBuffer),
		selectView(dynamicPrimitiveBuffer, staticPrimitiveBuffer),
		selectView(dynamicMaterialBuffer, staticMaterialBuffer),
		mSceneInstanceBuffer.shaderView,
		mPortalBuffer.shaderView,
		mRuntimeLightBuffer.shaderView,
		mRuntimeLightTileHeaderBuffer.shaderView,
		mRuntimeLightTileIndexBuffer.shaderView,
		mEmissivePrimitiveHeaderBuffer.shaderView,
		mEmissivePrimitiveBuffer.shaderView,
		mEmissivePrimitiveCdfBuffer.shaderView,
		mSectorLightHeaderBuffer.shaderView,
		mSectorLightBuffer.shaderView,
		mReprojectionBuffer.shaderView,
		mVisibleChunkBuffer.shaderView,
		mVisibleFlatPlaneBuffer.shaderView,
	};

	for (const nri::Descriptor* descriptor : mSceneDataDescriptors)
	{
		if (descriptor == nullptr)
		{
			return false;
		}
	}

	if (!CommitSceneDataDescriptors(reason != nullptr ? reason : "scene_data_full_rebuild"))
	{
		return false;
	}

	mBoundStaticPrimitiveCount = staticPrimitiveCount;
	mBoundDynamicPrimitiveCount = dynamicPrimitiveCount;
	mBoundStaticMaterialCount = staticMaterialCount;
	mBoundDynamicMaterialCount = dynamicMaterialCount;
	mBoundPortalCount = mMapWorld.valid ? (uint32_t)mMapWorld.portals.size() : 0u;
	mBoundRuntimeLightCount = activeRuntimeLightCount;
	mBoundRuntimeLightTileCountX = runtimeLightTileCountX;
	mBoundRuntimeLightTileCountY = runtimeLightTileCountY;
	mBoundRuntimeLightTileSize = NRI_RUNTIME_LIGHT_TILE_SIZE;
	mBoundRuntimeLightTileIndexCount = runtimeLightTileIndexCount;
	mBoundRuntimeLightMaxTileOccupancy = runtimeLightMaxTileOccupancy;
	return true;
}

bool NRIRenderer::CommitSceneDataDescriptors(const char* reason)
{
	for (const nri::Descriptor* descriptor : mSceneDataDescriptors)
	{
		if (descriptor == nullptr)
		{
			return false;
		}
	}

	nri::DescriptorSet* sceneDataSet = GetCurrentSceneDataSet();
	if (sceneDataSet == nullptr)
	{
		return false;
	}

	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = sceneDataSet;
	update.rangeIndex = 0;
	update.descriptors = reinterpret_cast<const nri::Descriptor* const*>(mSceneDataDescriptors.data());
	update.descriptorNum = NRI_SCENE_DATA_DESCRIPTOR_NUM;
	mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	SetCurrentSceneDataDescriptorsInitialized(true);
	TraceSharedDescriptorRewrite(
		"scene_data",
		reason != nullptr ? reason : "unlabeled",
		HashDescriptorList(reinterpret_cast<const nri::Descriptor* const*>(mSceneDataDescriptors.data()), mSceneDataDescriptors.size()),
		NRI_SCENE_DATA_DESCRIPTOR_NUM,
		false);
	return true;
}

bool NRIRenderer::UpdateFrameTextureSet()
{
	return UpdateFrameTextureSet(mFrameTextureSet, mFrameInputDescriptors);
}

bool NRIRenderer::UpdateFrameTextureSet(nri::DescriptorSet* set, const std::array<nri::Descriptor*, 14>& descriptors)
{
	const nri::Descriptor* rawDescriptors[NRI_INPUT_DESCRIPTOR_NUM] = {};
	for (size_t i = 0; i < NRI_INPUT_DESCRIPTOR_NUM; ++i)
	{
		rawDescriptors[i] = descriptors[i];
	}

	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = set;
	update.rangeIndex = 0;
	update.descriptors = rawDescriptors;
	update.descriptorNum = NRI_INPUT_DESCRIPTOR_NUM;
	mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	return true;
}

bool NRIRenderer::UpdateOutputSet()
{
	return UpdateOutputSet(mOutputSet, mOutputDescriptors);
}

bool NRIRenderer::UpdateOutputSet(nri::DescriptorSet* set, const std::array<nri::Descriptor*, 15>& descriptors)
{
	const nri::Descriptor* rawDescriptors[NRI_OUTPUT_DESCRIPTOR_NUM] = {};
	for (size_t i = 0; i < NRI_OUTPUT_DESCRIPTOR_NUM; ++i)
	{
		rawDescriptors[i] = descriptors[i];
	}

	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = set;
	update.rangeIndex = 0;
	update.descriptors = rawDescriptors;
	update.descriptorNum = NRI_OUTPUT_DESCRIPTOR_NUM;
	mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	return true;
}

bool NRIRenderer::CreateFrameTexture(FrameTextureSlot slot, uint32_t width, uint32_t height, nri::Format format)
{
	return mFrameBuffer->CreateOwnedTexture(GetFrameTexture(slot), width, height, format, NRIFlags(nri::TextureUsageBits::SHADER_RESOURCE, nri::TextureUsageBits::SHADER_RESOURCE_STORAGE));
}

void NRIRenderer::PrepareSceneTextureInputsForCompute()
{
	if (mFrameBuffer == nullptr)
	{
		return;
	}

	const bool tracePerf = ShouldTracePtPerf();
	const auto transitionStart = tracePerf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	uint32_t transitionCount = 0;

	if (mPaletteTexture.texture != nullptr)
	{
		mFrameBuffer->TransitionTexture(mPaletteTexture, NRIComputeShaderResourceState());
	}

	if (mFrameBuffer->mWhiteTexture != nullptr)
	{
		mFrameBuffer->TransitionTexture(mFrameBuffer->mWhiteTexture->GetResource(), NRIComputeShaderResourceState());
	}

	for (auto& entry : mTextureCache)
	{
		if (entry.resource.texture != nullptr)
		{
			transitionCount++;
			mFrameBuffer->TransitionTexture(entry.resource, NRIComputeShaderResourceState());
		}
	}

	for (NRITextureResource* resource : mLiveSceneTextureResources)
	{
		if (resource != nullptr && resource->texture != nullptr)
		{
			transitionCount++;
			mFrameBuffer->TransitionTexture(*resource, NRIComputeShaderResourceState());
		}
	}

	const double transitionMs = tracePerf ? DurationMs(transitionStart, std::chrono::steady_clock::now()) : 0.0;
	mSceneTextureCacheDebugStats.transitionCountLastFrame = transitionCount;
	mSceneTextureCacheDebugStats.transitionMsLastFrame = transitionMs;
	mLastPerfShellTraceStats.sceneTextureCacheCount = (uint32_t)mTextureCache.size();
	mLastPerfShellTraceStats.sceneTextureTransitionCount = transitionCount;
	mLastPerfShellTraceStats.sceneTextureTransitionMs = transitionMs;
}

void NRIRenderer::TrackLiveSceneTextureResource(NRITextureResource& resource)
{
	if (resource.texture == nullptr)
	{
		return;
	}

	for (NRITextureResource* existing : mLiveSceneTextureResources)
	{
		if (existing == &resource)
		{
			return;
		}
	}

	mLiveSceneTextureResources.push_back(&resource);
}

nri::Format NRIRenderer::ResolveFinalSceneFormat() const
{
	if (mFrameBuffer == nullptr)
	{
		return nri::Format::BGRA8_UNORM;
	}

	const NRIFrameGenerationPresentContract& presentContract = mFrameBuffer->mFrameGeneration.GetPresentContract();
	if (presentContract.resolvedTextureFormat != nri::Format::UNKNOWN)
	{
		return presentContract.resolvedTextureFormat;
	}

	if (mFrameBuffer->mResolvedSwapChainTextureFormat != nri::Format::UNKNOWN)
	{
		return mFrameBuffer->mResolvedSwapChainTextureFormat;
	}

	return nri::Format::BGRA8_UNORM;
}

bool NRIRenderer::EnsureFrameResources(uint32_t outputWidth, uint32_t outputHeight, uint32_t targetWidth, uint32_t targetHeight)
{
	Clocker clock(NriPTFrameResources);

	if (outputWidth == 0 || outputHeight == 0 || targetWidth == 0 || targetHeight == 0)
	{
		return false;
	}

	const int32_t sceneLeft = mFrameBuffer->mSceneViewport.left;
	// Preserve the oversized hardware viewport and crop it during present instead of shrinking it to the visible target.
	const int32_t sceneBottom = mFrameBuffer->mSceneViewport.top;
	const int32_t sceneTop = (int32_t)targetHeight - sceneBottom - (int32_t)outputHeight;

	const NRIMainUpscalerKind mainUpscalerKind = ResolveMainUpscalerKind(false);
	const nri::UpscalerMode requestedUpscalerMode = GetSelectedUpscalerMode();
	const nri::UpscalerMode resolvedUpscalerMode = ResolveUpscalerModeForMain(mainUpscalerKind, requestedUpscalerMode);
	const float requestedRenderScale = std::max(0.33f, std::min((float)nri_renderscale, 1.0f));
	const float renderScale = ResolveRenderScaleForMain(mainUpscalerKind, requestedUpscalerMode, requestedRenderScale);
	const NRIFrameGenerationPresentContract& presentContract = mFrameBuffer->mFrameGeneration.GetPresentContract();

	const uint32_t renderWidth = std::max(1u, (uint32_t)std::lround((double)outputWidth * renderScale));
	const uint32_t renderHeight = std::max(1u, (uint32_t)std::lround((double)outputHeight * renderScale));
	const nri::Format finalFormat = ResolveFinalSceneFormat();
	const nri::Format activeTargetFormat =
		(mFrameBuffer->mActiveTarget != nullptr && mFrameBuffer->mActiveTarget->format != nri::Format::UNKNOWN)
		? mFrameBuffer->mActiveTarget->format
		: nri::Format::UNKNOWN;

	const bool upToDate =
		mRenderWidth == renderWidth &&
		mRenderHeight == renderHeight &&
		mOutputWidth == outputWidth &&
		mOutputHeight == outputHeight &&
		mTargetWidth == targetWidth &&
		mTargetHeight == targetHeight &&
		mSceneLeft == sceneLeft &&
		mSceneTop == sceneTop &&
		mFinalSceneFormat == finalFormat &&
		GetFrameTexture(FrameTextureSlot::Final).texture != nullptr;

	if (upToDate)
	{
		return true;
	}

	// Frame-resource rebuilds on resize/upscaler mode changes can retire textures that the current
	// command allocator still references. Drain GPU work before destroying frame-sized resources.
	const bool dimensionsChanged =
		mRenderWidth != renderWidth ||
		mRenderHeight != renderHeight ||
		mOutputWidth != outputWidth ||
		mOutputHeight != outputHeight ||
		mTargetWidth != targetWidth ||
		mTargetHeight != targetHeight;
	WaitForCommandsTracked();
	mNrd.Shutdown();
	DestroyFrameTextures();
	mRenderWidth = renderWidth;
	mRenderHeight = renderHeight;
	mOutputWidth = outputWidth;
	mOutputHeight = outputHeight;
	mTargetWidth = targetWidth;
	mTargetHeight = targetHeight;
	mSceneLeft = sceneLeft;
	mSceneTop = sceneTop;
	mFinalSceneFormat = finalFormat;
	RequestHistoryReset(dimensionsChanged ? "resize" : "frame-resources");
	Printf("NRI PT frame resources: main=%s policy=%s requested_mode=%s resolved_mode=%s requested_render_scale=%.3f resolved_render_scale=%.3f render=%ux%u output=%ux%u final=%s contract=%s active=%s jitter=%s phases=%u\n",
		GetMainUpscalerName(mainUpscalerKind),
		GetRenderResolutionPolicyName(mainUpscalerKind),
		GetUpscalerModeName(requestedUpscalerMode),
		GetUpscalerModeName(resolvedUpscalerMode),
		requestedRenderScale,
		renderScale,
		renderWidth,
		renderHeight,
		outputWidth,
		outputHeight,
		NRIFrameGenerationContext::GetNriFormatName(finalFormat),
		NRIFrameGenerationContext::GetNriFormatName(presentContract.resolvedTextureFormat),
		NRIFrameGenerationContext::GetNriFormatName(activeTargetFormat),
		GetTemporalJitterModeName(mainUpscalerKind, mGuiCaptureActive),
		GetTemporalJitterPhaseCount(mainUpscalerKind, resolvedUpscalerMode, mGuiCaptureActive));

	const nri::Format colorFormat = nri::Format::RGBA16_SFLOAT;
	const nri::Format normalRoughnessFormat = nri::Format::R10_G10_B10_A2_UNORM;
	const nri::Format upscalerDepthFormat = nri::Format::R32_SFLOAT;
	const nri::Format rrGuideAlbedoFormat = nri::Format::R10_G10_B10_A2_UNORM;
	const nri::Format rrGuideSpecHitDistanceFormat = nri::Format::R16_SFLOAT;
	const nri::Format rrGuideNormalRoughnessFormat = nri::Format::RGBA16_SFLOAT;

	return
		CreateFrameTexture(FrameTextureSlot::ViewZ, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::Motion, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::NormalRoughness, renderWidth, renderHeight, normalRoughnessFormat) &&
		CreateFrameTexture(FrameTextureSlot::BaseColorMetalness, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::UnfilteredDiffuse, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::UnfilteredSpecular, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::UnfilteredPenumbra, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DenoisedDiffuse, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DenoisedSpecular, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DenoisedShadow, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::Composed, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::TraceTransparentOutput, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DirectLighting, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DirectEmission, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::TaaHistoryPing, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::TaaHistoryPong, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::Validation, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::SrInput, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::RrInput, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::UpscalerDepth, renderWidth, renderHeight, upscalerDepthFormat) &&
		CreateFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo, renderWidth, renderHeight, rrGuideAlbedoFormat) &&
		CreateFrameTexture(FrameTextureSlot::RrGuideSpecularAlbedo, renderWidth, renderHeight, rrGuideAlbedoFormat) &&
		CreateFrameTexture(FrameTextureSlot::RrGuideSpecularHitDistance, renderWidth, renderHeight, rrGuideSpecHitDistanceFormat) &&
		CreateFrameTexture(FrameTextureSlot::RrGuideNormalRoughness, renderWidth, renderHeight, rrGuideNormalRoughnessFormat) &&
		CreateFrameTexture(FrameTextureSlot::VendorOutput, outputWidth, outputHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::PostSharpenOutput, outputWidth, outputHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::Final, targetWidth, targetHeight, finalFormat);
}

bool NRIRenderer::EnsurePaletteTexture(const nri_scene::MaterialBridgeData& materials)
{
	Clocker clock(NriPTPaletteUpload);

	if (mPaletteTexture.texture != nullptr &&
		mPaletteTexture.width == materials.paletteWidth &&
		mPaletteTexture.height == materials.paletteHeight)
	{
		return true;
	}

	mFrameBuffer->DestroyTextureResource(mPaletteTexture);
	if (!mFrameBuffer->CreateOwnedTexture(mPaletteTexture, materials.paletteWidth, materials.paletteHeight, nri::Format::BGRA8_UNORM, nri::TextureUsageBits::SHADER_RESOURCE))
	{
		return false;
	}

	return mFrameBuffer->UploadTextureData(mPaletteTexture, materials.paletteLookup.data(), materials.paletteWidth, materials.paletteHeight, materials.paletteWidth * 4u);
}

bool NRIRenderer::DispatchBootstrapView()
{
	Clocker clock(NriPTBootstrapDispatch);

	if (!UpdateReprojectionBuffer())
	{
		return false;
	}

	const uint32_t bootstrapMode = GetBootstrapMode();
	NRITraceSceneConstants constants = {};
	Copy3(mCurrentCameraPos, constants.CameraPos);
	Copy3(mCurrentCameraForward, constants.CameraForward);
	Copy3(mCurrentCameraRight, constants.CameraRight);
	Copy3(mCurrentCameraUp, constants.CameraUp);
	Copy3(mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = mRenderWidth;
	constants.RenderHeight = mRenderHeight;
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.TanHalfFovX = mCurrentTanHalfFovX;
	constants.TanHalfFovY = mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = mPreviousTanHalfFovY;
	constants.SceneInstanceCount = mSceneInstanceBuffer.stride != 0 ? (uint32_t)(mSceneInstanceBuffer.usedSize / mSceneInstanceBuffer.stride) : 0u;
	constants.StaticPrimitiveCount = mBoundStaticPrimitiveCount;
	constants.DynamicPrimitiveCount = mBoundDynamicPrimitiveCount;
	constants.FrameIndex = mFrameIndex;
	constants.Flags =
		NRI_FLAG_BOOTSTRAP_VIEW |
		(mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(mDirectionalLightState.enabled && mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u);
	constants.StaticMaterialCount = mBoundStaticMaterialCount;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = mBoundDynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(0u, 0u, mDirectionalLightState.color);
	constants.ReservedTrace0 = (uint16_t)(int16_t)mSceneLeft | ((uint32_t)(uint16_t)(int16_t)mSceneTop << 16);
	Copy3(mSkyColor, constants.SkyColor);
	Copy3(mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(mDirectionalLightState, constants);

	NRITextureResource& history = GetFrameTexture(mHistoryOutputSlot);
	NRITextureResource& upscaled = GetFrameTexture(FrameTextureSlot::Composed);
	NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	mFrameBuffer->TransitionTexture(history, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Validation), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredSpecular), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(upscaled, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	mFrameInputDescriptors.fill(GetFrameTexture(FrameTextureSlot::Composed).shaderView);
	mFrameInputDescriptors[0] = history.shaderView;
	mFrameInputDescriptors[1] = GetFrameTexture(FrameTextureSlot::Motion).shaderView;
	mFrameInputDescriptors[2] = GetFrameTexture(FrameTextureSlot::ViewZ).shaderView;
	mFrameInputDescriptors[3] = GetFrameTexture(FrameTextureSlot::NormalRoughness).shaderView;
	mFrameInputDescriptors[4] = GetFrameTexture(FrameTextureSlot::BaseColorMetalness).shaderView;
	mFrameInputDescriptors[5] = GetFrameTexture(FrameTextureSlot::Composed).shaderView;
	mFrameInputDescriptors[6] = upscaled.shaderView;
	mFrameInputDescriptors[7] = GetFrameTexture(FrameTextureSlot::Validation).shaderView;
	mFrameInputDescriptors[8] = GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse).shaderView;
	mFrameInputDescriptors[9] = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular).shaderView;
	mFrameInputDescriptors[10] = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular).shaderView;
	UpdateFrameTextureSet(mUpscalerPrepassFrameTextureSet, mFrameInputDescriptors);

	mOutputDescriptors.fill(GetFrameTexture(FrameTextureSlot::VendorOutput).storageView);
	mOutputDescriptors[2] = final.storageView;
	UpdateOutputSet(mUpscalerPrepassOutputSet, mOutputDescriptors);

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	BindSceneRootDescriptors();
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mSamplerSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 2, GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 3, mFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 4, mOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::Final));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mTargetWidth), GetDispatchSize(mTargetHeight), 1 });
	return true;
}

void NRIRenderer::ResetSceneBufferFrameStats()
{
	mVertexBufferStats.bytesUploadedLastFrame = 0;
	mVertexBufferStats.growEventsLastFrame = 0;
	mVertexBufferStats.overwriteEventsLastFrame = 0;
	mIndexBufferStats.bytesUploadedLastFrame = 0;
	mIndexBufferStats.growEventsLastFrame = 0;
	mIndexBufferStats.overwriteEventsLastFrame = 0;
	mPrimitiveBufferStats.bytesUploadedLastFrame = 0;
	mPrimitiveBufferStats.growEventsLastFrame = 0;
	mPrimitiveBufferStats.overwriteEventsLastFrame = 0;
	mMaterialBufferStats.bytesUploadedLastFrame = 0;
	mMaterialBufferStats.growEventsLastFrame = 0;
	mMaterialBufferStats.overwriteEventsLastFrame = 0;
	mPortalBufferStats.bytesUploadedLastFrame = 0;
	mPortalBufferStats.growEventsLastFrame = 0;
	mPortalBufferStats.overwriteEventsLastFrame = 0;
}

const NRIBufferResource& NRIRenderer::GetActiveVertexBuffer() const
{
	return mBoundDynamicPrimitiveCount > 0 ? mVertexBuffer : mStaticVertexBuffer;
}

const NRIBufferResource& NRIRenderer::GetActiveIndexBuffer() const
{
	return mBoundDynamicPrimitiveCount > 0 ? mIndexBuffer : mStaticIndexBuffer;
}

const NRIBufferResource& NRIRenderer::GetActivePrimitiveBuffer() const
{
	return mBoundDynamicPrimitiveCount > 0 ? mPrimitiveBuffer : mStaticPrimitiveBuffer;
}

const NRIBufferResource& NRIRenderer::GetActiveMaterialBuffer() const
{
	return mBoundDynamicMaterialCount > 0 ? mMaterialBuffer : mStaticMaterialBuffer;
}

void NRIRenderer::BindSceneRootDescriptors()
{
	if (mTopLevelAS.descriptor != nullptr)
	{
		mFrameBuffer->mCore.CmdSetRootDescriptor(*mFrameBuffer->mCommandBuffer, { 0, mTopLevelAS.descriptor, 0, nri::BindPoint::COMPUTE });
	}
}

bool NRIRenderer::RefreshStaticMapAnimatedMaterials()
{
	if (!mStaticMapScene.valid ||
		!mStaticMapScene.texturesResident ||
		!mStaticMapScene.buffersResident ||
		!mStaticMapScene.accelerationResident ||
		mStaticMapScene.buildSerial != mMapWorld.buildSerial)
	{
		return true;
	}

	const nri_scene::SceneView* preservedSkyView =
		(mPreservedStaticMapSky.valid && mPreservedStaticMapSky.buildSerial == mMapWorld.buildSerial)
		? &mPreservedStaticMapSky.sceneView
		: nullptr;
	bool refreshedAnyChunk = false;
	uint32_t refreshedChunkCount = 0;
	const auto suppressAnimatedChunkRefresh = [&](StaticMapSceneCache::ChunkCache& targetChunk, const char* reason)
	{
		if (targetChunk.animatedRefreshSuppressed)
		{
			return;
		}

		targetChunk.animatedRefreshSuppressed = true;
		mStaticMapScene.animatedRefreshSuppressedChunkCount++;
		Printf("NRI PT static scene anim: suppressing chunk=%u resident animated refresh (%s).\n",
			targetChunk.chunkIndex,
			reason != nullptr ? reason : "unknown");
	};

	for (size_t chunkListIndex = 0; chunkListIndex < mStaticMapScene.chunks.size(); ++chunkListIndex)
	{
		auto& chunkCache = mStaticMapScene.chunks[chunkListIndex];
		if (chunkListIndex >= mStaticMapScene.lightChunkViews.size() || chunkCache.chunkIndex >= mMapWorld.chunks.size())
		{
			DestroyStaticMapSceneCache();
			mStaticMapScene = {};
			mStaticAccelerationBuildSerial = 0;
			mPreservedStaticMapSky = {};
			return EnsureStaticMapScene();
		}
		if (!chunkCache.hasAnimatedTextureCandidates ||
			chunkCache.animatedRefreshSuppressed ||
			!IsChunkMarkedVisible(mCurrentVisibleChunkWords, chunkCache.chunkIndex))
		{
			continue;
		}

		nri_scene::SceneView liveChunkView = mStaticMapScene.lightChunkViews[chunkListIndex];
		if (!RefreshAnimatedBindingsForStaticMapChunk(mMapWorld, mMapWorld.chunks[chunkCache.chunkIndex], liveChunkView))
		{
			suppressAnimatedChunkRefresh(chunkCache, "surface-mapping-mismatch");
			continue;
		}
		const uint64_t liveAnimatedMaterialSignature = ComputeAnimatedMaterialSignature(liveChunkView);
		if (liveAnimatedMaterialSignature == chunkCache.animatedMaterialSignature)
		{
			continue;
		}

		const uint64_t liveAnimatedGeometrySignature = ComputeAnimatedGeometrySignature(liveChunkView);
		if (liveAnimatedGeometrySignature != chunkCache.animatedGeometrySignature)
		{
			mStaticMapScene.animatedGeometryFallbackCount++;
			suppressAnimatedChunkRefresh(chunkCache, "display-metric-mismatch");
			continue;
		}

		nri_scene::MaterialBridgeData liveChunkMaterials;
		{
			Clocker clock(NriPTMaterialBuild);
			BuildMaterialsWithActorOverrides(liveChunkView, liveChunkMaterials, "static_map_anim_chunk");
		}
		if ((uint32_t)liveChunkMaterials.materials.size() != chunkCache.materialCount)
		{
			mStaticMapScene.animatedGeometryFallbackCount++;
			suppressAnimatedChunkRefresh(chunkCache, "material-slice-mismatch");
			continue;
		}

		mStaticMapScene.lightChunkViews[chunkListIndex] = std::move(liveChunkView);
		chunkCache.materialBridge = std::move(liveChunkMaterials);
		chunkCache.animatedMaterialSignature = liveAnimatedMaterialSignature;
		refreshedAnyChunk = true;
		refreshedChunkCount++;
	}

	if (!refreshedAnyChunk)
	{
		return true;
	}

	nri_scene::BuildMapSceneView(mMapWorld, mStaticMapScene.sceneView, preservedSkyView);
	mStaticMapScene.materialBridge = {};
	for (auto& chunkCache : mStaticMapScene.chunks)
	{
		const uint32_t nextMaterialOffset = (uint32_t)mStaticMapScene.materialBridge.materials.size();
		if (nextMaterialOffset != chunkCache.materialOffset)
		{
			mStaticMapScene.animatedGeometryFallbackCount++;
			DestroyStaticMapSceneCache();
			mStaticMapScene = {};
			mStaticAccelerationBuildSerial = 0;
			mPreservedStaticMapSky = {};
			return EnsureStaticMapScene();
		}

		chunkCache.materialOffset = nextMaterialOffset;
		chunkCache.materialCount = (uint32_t)chunkCache.materialBridge.materials.size();
		AppendMaterialBridge(chunkCache.materialBridge, mStaticMapScene.materialBridge);
	}

	if (!EnsurePaletteTexture(mStaticMapScene.materialBridge) ||
		!EnsureSceneTextures(mStaticMapScene.sceneView, mStaticMapScene.materialBridge, mStaticMapScene.gpuMaterials, false, "static_map_scene_anim") ||
		!UploadStaticMapChunkMaterialAtlas(
			mStaticMaterialBuffer,
			mStaticMapChunkAtlas,
			mStaticMapScene,
			mStaticMapScene.gpuMaterials))
	{
		DestroyStaticMapSceneCache();
		mStaticMapScene = {};
		mStaticAccelerationBuildSerial = 0;
		mPreservedStaticMapSky = {};
		return EnsureStaticMapScene();
	}

	mStaticMapScene.texturesResident = true;
	mStaticMapScene.buffersResident = true;
	mStaticMapScene.gpuUploadCount++;
	mStaticMapScene.animatedRefreshCount += refreshedChunkCount;
	mStaticMapScene.animatedRefreshUploadCount++;
	SyncResidentMapChunkRegistryFromStaticScene();
	mUploadedStaticMapSceneLastFrame = true;
	return true;
}

bool NRIRenderer::EnsureStaticMapScene()
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.staticSceneMs);
	if (!mMapWorld.valid)
	{
		return false;
	}

	if (mStaticMapScene.buildSerial != mMapWorld.buildSerial)
	{
		DestroyStaticMapSceneCache();
		mStaticMapScene = {};
		mStaticAccelerationBuildSerial = 0;
		mPreservedStaticMapSky = {};
	}

	if (mStaticMapScene.valid &&
		mStaticMapScene.texturesResident &&
		mStaticMapScene.buffersResident &&
		mStaticMapScene.accelerationResident &&
		mStaticMapScene.buildSerial == mMapWorld.buildSerial)
	{
		if (!RefreshStaticMapAnimatedMaterials())
		{
			return false;
		}
		mStaticMapScene.reuseCount++;
		return true;
	}

	if (!BuildStaticMapSceneCache(
		mMapWorld,
		(mPreservedStaticMapSky.valid && mPreservedStaticMapSky.buildSerial == mMapWorld.buildSerial) ? &mPreservedStaticMapSky : nullptr,
		mStaticMapScene,
		mRuntimeMapMutations))
	{
		return false;
	}

	if (ShouldTraceSkyPerf())
	{
		gRendererSkyPerfTraceStats.residentStaticSceneTextureBuilds++;
	}

	if (mStaticMapScene.geometry.primitives.empty() ||
		!EnsurePaletteTexture(mStaticMapScene.materialBridge) ||
		!EnsureSceneTextures(mStaticMapScene.sceneView, mStaticMapScene.materialBridge, mStaticMapScene.gpuMaterials, false, "static_map_scene") ||
		!UploadStaticMapChunkAtlas(
			mStaticVertexBuffer,
			mStaticIndexBuffer,
			mStaticPrimitiveBuffer,
			mStaticMaterialBuffer,
			mStaticMapChunkAtlas,
			mStaticMapScene,
			mStaticMapScene.gpuMaterials) ||
		!BuildStaticMapAccelerationStructures())
	{
		return false;
	}

	mStaticMapScene.valid = true;
	mStaticMapScene.texturesResident = true;
	mStaticMapScene.buffersResident = true;
	mStaticMapScene.accelerationResident = true;
	mStaticMapScene.buildSerial = mMapWorld.buildSerial;
	mStaticMapScene.tlasInstanceCount = (uint32_t)mStaticMapScene.chunks.size();
	mStaticMapScene.sceneBuildCount++;
	mStaticMapScene.gpuUploadCount++;
	mStaticMapScene.accelerationBuildCount++;
	SyncResidentMapChunkRegistryFromStaticScene();
	mUploadedStaticMapSceneLastFrame = true;
	mBuiltStaticMapSceneASLastFrame = true;
	mPreservedStaticMapSky = {};

	Printf("NRI PT static scene resident: level=%s build_serial=%llu chunks=%u tris=%u materials=%u uploads=%u as_builds=%u\n",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)mStaticMapScene.buildSerial,
		(uint32_t)mStaticMapScene.chunks.size(),
		(uint32_t)mStaticMapScene.geometry.primitives.size(),
		(uint32_t)mStaticMapScene.gpuMaterials.size(),
		mStaticMapScene.gpuUploadCount,
		mStaticMapScene.accelerationBuildCount);
	return true;
}

void NRIRenderer::InitializeStaticMapSceneCacheBuild(
	const nri_scene::PTMapWorld& mapWorld,
	const PreservedStaticMapSkyState* preservedSkyState,
	StaticMapSceneCache& outStaticScene,
	RuntimeMapMutationCache& outRuntimeMutations)
{
	outStaticScene.valid = false;
	outStaticScene.texturesResident = false;
	outStaticScene.buffersResident = false;
	outStaticScene.accelerationResident = false;
	outStaticScene.buildSerial = mapWorld.buildSerial;
	outStaticScene.sceneBuildCount = 0;
	outStaticScene.gpuUploadCount = 0;
	outStaticScene.accelerationBuildCount = 0;
	outStaticScene.animatedCandidateChunkCount = 0;
	outStaticScene.animatedRefreshCount = 0;
	outStaticScene.animatedRefreshUploadCount = 0;
	outStaticScene.animatedGeometryFallbackCount = 0;
	outStaticScene.animatedRefreshSuppressedChunkCount = 0;
	outStaticScene.reuseCount = 0;
	outStaticScene.sceneView = {};
	outStaticScene.lightChunkViews.clear();
	outStaticScene.geometry = {};
	outStaticScene.materialBridge = {};
	outStaticScene.gpuMaterials.clear();
	outStaticScene.chunks.clear();
	outStaticScene.tlasInstanceCount = 0;
	outStaticScene.lightChunkViews.reserve(mapWorld.chunks.size());
	outStaticScene.chunks.reserve(mapWorld.chunks.size());

	outRuntimeMutations.chunks.clear();
	outRuntimeMutations.chunks.resize(mapWorld.chunks.size());
	outRuntimeMutations.replacedChunkMask.assign(mapWorld.chunks.size(), 0u);

	const nri_scene::SceneView* preservedSkyView = preservedSkyState != nullptr ? &preservedSkyState->sceneView : nullptr;
	nri_scene::BuildMapSceneView(mapWorld, outStaticScene.sceneView, preservedSkyView);
}

void NRIRenderer::AppendStaticMapSceneCacheChunk(
	const nri_scene::PTMapWorld& mapWorld,
	const nri_scene::PTMapChunk& chunk,
	const nri_scene::SceneView* preservedSkyView,
	StaticMapSceneCache& outStaticScene,
	RuntimeMapMutationCache& outRuntimeMutations)
{
	if (chunk.chunkIndex < outRuntimeMutations.chunks.size())
	{
		auto& replacement = outRuntimeMutations.chunks[chunk.chunkIndex];
		nri_scene::CaptureMapChunkMutationBaseline(chunk, replacement.baseline);
		replacement.replacementBaseline = replacement.baseline;
		replacement.baselineSignature = replacement.baseline.signature;
		replacement.liveSignature = replacement.baselineSignature;
		replacement.animatedMaterialSignature = 0;
		replacement.reasonMask = 0;
		replacement.sectionDirtyCount = 0;
		replacement.stableMutationFrameCount = 0;
		replacement.sectorDirty = false;
		replacement.dragged = false;
		replacement.blindSpot = false;
		replacement.excludeStaticChunk = false;
		replacement.staticAnimatedReplacement = false;
		replacement.lastTraceSignature = UINT64_MAX;
		replacement.lastTraceAnimatedMaterialSignature = UINT64_MAX;
		replacement.lastTraceReasonMask = UINT32_MAX;
		replacement.lastTraceActive = false;
		replacement.lastTraceBlindSpot = false;
		replacement.animationOnlyRefreshed = false;
		replacement.lastTraceAnimationOnlyRefreshed = false;
		replacement.lastTraceStaticAnimatedReplacement = false;
		replacement.traceCount = 0;
		replacement.surfaceCount = 0;
		replacement.triangleCount = 0;
		replacement.sceneView = {};
		replacement.geometry = {};
		replacement.materialBridge = {};
		replacement.lightIdentityOverrides = {};
	}

	nri_scene::SceneView chunkSceneView;
	nri_scene::GeometryData chunkGeometry;
	nri_scene::MaterialBridgeData chunkMaterials;
	nri_scene::BuildMapChunkSceneView(mapWorld, chunk, chunkSceneView, preservedSkyView);
	{
		Clocker clock(NriPTGeometryBuild);
		nri_scene::BuildGeometry(chunkSceneView, chunkGeometry);
		AssignGeometryPortalIndices(mapWorld, chunkGeometry);
	}
	{
		Clocker clock(NriPTMaterialBuild);
		BuildMaterialsWithActorOverrides(chunkSceneView, chunkMaterials, "static_map_chunk");
	}
	if (chunkGeometry.primitives.empty())
	{
		return;
	}

	StaticMapSceneCache::ChunkCache chunkCache = {};
	chunkCache.chunkIndex = chunk.chunkIndex;
	chunkCache.vertexOffset = (uint32_t)outStaticScene.geometry.vertices.size();
	chunkCache.vertexCount = (uint32_t)chunkGeometry.vertices.size();
	chunkCache.indexOffset = (uint32_t)outStaticScene.geometry.indices.size();
	chunkCache.indexCount = (uint32_t)chunkGeometry.indices.size();
	chunkCache.primitiveOffset = (uint32_t)outStaticScene.geometry.primitives.size();
	chunkCache.primitiveCount = (uint32_t)chunkGeometry.primitives.size();
	chunkCache.materialOffset = (uint32_t)outStaticScene.materialBridge.materials.size();
	chunkCache.materialCount = (uint32_t)chunkMaterials.materials.size();
	chunkCache.animatedMaterialSignature = ComputeAnimatedMaterialSignature(chunkSceneView);
	chunkCache.animatedGeometrySignature = ComputeAnimatedGeometrySignature(chunkSceneView);
	chunkCache.hasAnimatedTextureCandidates = ChunkHasAnimatedStaticMapSurfaceCandidates(mapWorld, chunk);
	chunkCache.animatedRefreshSuppressed = false;

	AppendGeometry(chunkGeometry, chunkCache.materialOffset, outStaticScene.geometry);
	AppendMaterialBridge(chunkMaterials, outStaticScene.materialBridge);
	chunkCache.materialBridge = std::move(chunkMaterials);
	if (chunkCache.hasAnimatedTextureCandidates)
	{
		outStaticScene.animatedCandidateChunkCount++;
	}
	outStaticScene.lightChunkViews.push_back(std::move(chunkSceneView));
	outStaticScene.chunks.push_back(std::move(chunkCache));
}

bool NRIRenderer::BuildStaticMapSceneCache(
	const nri_scene::PTMapWorld& mapWorld,
	const PreservedStaticMapSkyState* preservedSkyState,
	StaticMapSceneCache& outStaticScene,
	RuntimeMapMutationCache& outRuntimeMutations)
{
	if (!mapWorld.valid)
	{
		return false;
	}

	InitializeStaticMapSceneCacheBuild(mapWorld, preservedSkyState, outStaticScene, outRuntimeMutations);
	const nri_scene::SceneView* preservedSkyView = preservedSkyState != nullptr ? &preservedSkyState->sceneView : nullptr;
	for (const nri_scene::PTMapChunk& chunk : mapWorld.chunks)
	{
		AppendStaticMapSceneCacheChunk(mapWorld, chunk, preservedSkyView, outStaticScene, outRuntimeMutations);
	}

	return !outStaticScene.geometry.primitives.empty();
}

bool NRIRenderer::EnsureSkyTexture(const nri_scene::SceneView& sceneView, bool preserveExistingSky)
{
	if (ShouldTraceSkyPerf())
	{
		gRendererSkyPerfTraceStats.ensureSkyCalls++;
	}
	ScopedSkyPerfTimer timer(gRendererSkyPerfTraceStats.ensureSkyTimeUs);
	if (mSkyLevel != currentLevel)
	{
		mActiveSkyTextureIndex = UINT32_MAX;
		mSkyTextureKey = 0;
		mSkyState = {};
		mSkyLevel = currentLevel;
	}

	auto findCachedSkyTexture = [this](uint64_t key, uint32_t width, uint32_t height) -> uint32_t
	{
		for (uint32_t i = 0; i < (uint32_t)mSkyTextureCache.size(); ++i)
		{
			const CachedSkyTexture& cached = mSkyTextureCache[i];
			if (cached.key == key &&
				cached.resource.width == width &&
				cached.resource.height == height)
			{
				return i;
			}
		}

		return UINT32_MAX;
	};

	auto activateCachedSky = [this](uint32_t index, uint64_t key, const nri_scene::SceneView& sourceView, nri_scene::PTSkyMode mode)
	{
		mActiveSkyTextureIndex = index;
		mSkyTextureKey = key;
		mSkyState.mode = mode;
		mSkyState.sourceType = sourceView.sky.sourceType;
		mSkyState.texture = sourceView.sky.texture;
		mSkyState.faceMask = sourceView.sky.faceMask;
		mSkyState.flipTop = sourceView.sky.flipTop;
	};

	auto createCachedSky = [this, &findCachedSkyTexture](const SkyUpload& upload, nri_scene::PTSkyMode mode) -> uint32_t
	{
		const uint32_t existing = findCachedSkyTexture(upload.key, upload.width, upload.height);
		if (existing != UINT32_MAX)
		{
			return existing;
		}

		CachedSkyTexture cacheEntry = {};
		cacheEntry.key = upload.key;
		cacheEntry.mode = mode;
		if (!mFrameBuffer->CreateOwnedTexture(cacheEntry.resource, upload.width, upload.height, nri::Format::BGRA8_UNORM, nri::TextureUsageBits::SHADER_RESOURCE, nri::TextureType::TEXTURE_2D, 6, nri::TextureView::TEXTURE_CUBE))
		{
			return UINT32_MAX;
		}

		std::array<nri::TextureSubresourceUploadDesc, 6> subresources = {};
		for (uint32_t i = 0; i < 6; ++i)
		{
			subresources[i].slices = upload.faces[i].pixels.data();
			subresources[i].sliceNum = 1;
			subresources[i].rowPitch = upload.faces[i].width * 4u;
			subresources[i].slicePitch = upload.faces[i].width * upload.faces[i].height * 4u;
		}

		if (!mFrameBuffer->UploadTextureSubresources(cacheEntry.resource, subresources.data(), (uint32_t)subresources.size(), upload.width, upload.height))
		{
			mFrameBuffer->DestroyTextureResource(cacheEntry.resource);
			return UINT32_MAX;
		}

		mSkyTextureCache.push_back(std::move(cacheEntry));
		return (uint32_t)mSkyTextureCache.size() - 1;
	};

	const NRITextureResource* activeSkyTexture = GetActiveSkyTexture();
	if (preserveExistingSky && activeSkyTexture != nullptr)
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.preserveExistingHits++;
		}
		TraceSkyState(sceneView, "preserve-existing", mSkyTextureKey);
		return true;
	}

	if (sceneView.sky.mode == nri_scene::PTSkyMode::Cubemap &&
		activeSkyTexture != nullptr &&
		mSkyState.mode == nri_scene::PTSkyMode::Cubemap &&
		mSkyState.texture == sceneView.sky.texture &&
		mSkyState.faceMask == sceneView.sky.faceMask &&
		mSkyState.flipTop == sceneView.sky.flipTop)
	{
		mSkyLevel = currentLevel;
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.reuseActiveCubemapHits++;
		}
		TraceSkyState(sceneView, "reuse-active-cubemap", mSkyTextureKey);
		return true;
	}

	SkyProbe probe = {};
	if (ProbeCubemapSky(sceneView, probe))
	{
		if (activeSkyTexture != nullptr &&
			mSkyTextureKey == probe.key &&
			activeSkyTexture->width == probe.width &&
			activeSkyTexture->height == probe.height)
		{
			mSkyLevel = currentLevel;
			if (ShouldTraceSkyPerf())
			{
				gRendererSkyPerfTraceStats.reuseActiveProbeHits++;
			}
			TraceSkyState(sceneView, "reuse-active-probe", probe.key);
			return true;
		}

		const uint32_t cachedIndex = findCachedSkyTexture(probe.key, probe.width, probe.height);
		if (cachedIndex != UINT32_MAX)
		{
			activateCachedSky(cachedIndex, probe.key, sceneView, nri_scene::PTSkyMode::Cubemap);
			mSkyLevel = currentLevel;
			if (ShouldTraceSkyPerf())
			{
				gRendererSkyPerfTraceStats.activateCachedCubemapHits++;
			}
			TraceSkyState(sceneView, "activate-cached-cubemap", probe.key);
			return true;
		}

		SkyUpload upload = {};
		if (!BuildCubemapUpload(sceneView, probe, upload))
		{
			return false;
		}

		const uint32_t createdIndex = createCachedSky(upload, nri_scene::PTSkyMode::Cubemap);
		if (createdIndex == UINT32_MAX)
		{
			return false;
		}

		activateCachedSky(createdIndex, upload.key, sceneView, nri_scene::PTSkyMode::Cubemap);
		mSkyLevel = currentLevel;
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.createCachedCubemapHits++;
		}
		TraceSkyState(sceneView, "create-cached-cubemap", upload.key);
		return true;
	}

	const bool shouldKeepLastCubemap =
		activeSkyTexture != nullptr &&
		mSkyState.mode == nri_scene::PTSkyMode::Cubemap &&
		(sceneView.sky.mode == nri_scene::PTSkyMode::None ||
			sceneView.sky.texture == mSkyState.texture ||
			(sceneView.sky.texture == nullptr && sceneView.stats.skySurfaces > 0) ||
			(mSkyLevel == currentLevel &&
				sceneView.sky.mode == nri_scene::PTSkyMode::SolidColor &&
				sceneView.sky.sourceType != nri_scene::PTSkySourceType::Portal &&
				sceneView.stats.skySurfaces > 0));
	if (shouldKeepLastCubemap)
	{
		if (sceneView.sky.mode == nri_scene::PTSkyMode::SolidColor &&
			sceneView.sky.sourceType != nri_scene::PTSkySourceType::Portal)
		{
			if (ShouldTraceSkyPerf())
			{
				gRendererSkyPerfTraceStats.holdLevelCubemapHits++;
			}
			TraceSkyState(sceneView, "hold-level-cubemap", mSkyTextureKey);
			return true;
		}

		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.keepLastCubemapHits++;
		}
		TraceSkyState(sceneView, "keep-last-cubemap", mSkyTextureKey);
		return true;
	}

	SkyUpload upload = {};
	BuildSolidSkyUpload(sceneView.skyColor, upload);
	if (activeSkyTexture != nullptr &&
		mSkyTextureKey == upload.key &&
		activeSkyTexture->width == upload.width &&
		activeSkyTexture->height == upload.height)
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.solidReuseHits++;
		}
		TraceSkyState(sceneView, "reuse-active-solid", upload.key);
		return true;
	}

	const uint32_t cachedIndex = findCachedSkyTexture(upload.key, upload.width, upload.height);
	if (cachedIndex != UINT32_MAX)
	{
		activateCachedSky(cachedIndex, upload.key, sceneView, nri_scene::PTSkyMode::SolidColor);
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.solidActivateHits++;
		}
		TraceSkyState(sceneView, "activate-cached-solid", upload.key);
		return true;
	}

	const uint32_t createdIndex = createCachedSky(upload, nri_scene::PTSkyMode::SolidColor);
	if (createdIndex == UINT32_MAX)
	{
		return false;
	}

	activateCachedSky(createdIndex, upload.key, sceneView, nri_scene::PTSkyMode::SolidColor);
	if (ShouldTraceSkyPerf())
	{
		gRendererSkyPerfTraceStats.solidCreateHits++;
	}
	TraceSkyState(sceneView, "create-cached-solid", upload.key);
	return true;
}

bool NRIRenderer::EnsureSceneTextures(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& outGpuMaterials, bool preserveExistingSky, const char* reason)
{
	Clocker clock(NriPTSceneTextures);
	static bool sLoggedActiveCanvasTextureReuse = false;
	const bool tracePerf = ShouldTracePtPerf();
	uint32_t lookupMisses = 0;
	uint32_t insertCount = 0;
	double lookupMs = 0.0;
	double realizeMs = 0.0;
	double descriptorMs = 0.0;
	mSceneTextureOverflowStats.textureCountLastBuild = (uint32_t)materials.textures.size();
	mSceneTextureOverflowStats.truncatedTextureCountLastBuild =
		mSceneTextureOverflowStats.textureCountLastBuild > NRI_MAX_SCENE_TEXTURES ?
		mSceneTextureOverflowStats.textureCountLastBuild - NRI_MAX_SCENE_TEXTURES : 0;
	mSceneTextureOverflowStats.baseTextureClampCountLastBuild = 0;
	mSceneTextureOverflowStats.normalTextureClampCountLastBuild = 0;
	mSceneTextureOverflowStats.metallicTextureClampCountLastBuild = 0;
	mSceneTextureOverflowStats.roughnessTextureClampCountLastBuild = 0;
	mSceneTextureOverflowStats.emissiveTextureClampCountLastBuild = 0;
	mSceneTextureCacheDebugStats.cacheEntriesLastBuild = (uint32_t)mTextureCache.size();
	mSceneTextureCacheDebugStats.lookupMissesLastBuild = 0;
	mSceneTextureCacheDebugStats.insertCountLastBuild = 0;
	mSceneTextureCacheDebugStats.lookupMsLastBuild = 0.0;
	mSceneTextureCacheDebugStats.realizeMsLastBuild = 0.0;
	mSceneTextureCacheDebugStats.descriptorMsLastBuild = 0.0;
	mLiveSceneTextureResources.clear();
	mLastPerfShellTraceStats.sceneTextureCacheCount = (uint32_t)mTextureCache.size();
	mLastPerfShellTraceStats.sceneTextureCacheMisses = 0;
	mLastPerfShellTraceStats.sceneTextureCacheInserts = 0;
	mLastPerfShellTraceStats.sceneTextureLookupMs = 0.0;
	mLastPerfShellTraceStats.sceneTextureRealizeMs = 0.0;
	mLastPerfShellTraceStats.sceneTextureDescriptorMs = 0.0;
	mLastPerfShellTraceStats.sceneTextureReason = reason != nullptr ? reason : "none";
	mLastPerfShellTraceStats.sceneTextureRequestedCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedActorMaterialCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedBaseCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedGlowCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedNormalCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedMetallicCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedRoughnessCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedEmissiveCount = 0;
	mLastPerfShellTraceStats.actorOverflowMaterialCount = 0;
	mLastPerfShellTraceStats.actorOverflowBaseClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowNormalClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowMetallicClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowRoughnessClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowEmissiveClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowTraceOmittedCount = 0;
	if (ShouldTraceSkyPerf())
	{
		gRendererSkyPerfTraceStats.ensureSceneTexturesCalls++;
		if (preserveExistingSky)
		{
			gRendererSkyPerfTraceStats.ensureSceneTexturesPreserveTrueCalls++;
		}
		else
		{
			gRendererSkyPerfTraceStats.ensureSceneTexturesPreserveFalseCalls++;
		}
	}

	outGpuMaterials = materials.materials;
	ApplyEmissiveMaterialOverrides(materials, outGpuMaterials);
	ApplyActorShadowMaterialOverrides(materials, outGpuMaterials);
	const MaterialTextureAttributionCounts sceneTextureAttribution =
		GatherMaterialTextureAttribution(outGpuMaterials, materials.lightMetadata, materials.textures.size());
	mLastPerfShellTraceStats.sceneTextureRequestedCount = sceneTextureAttribution.textureCount;
	mLastPerfShellTraceStats.sceneTextureReferencedActorMaterialCount = sceneTextureAttribution.actorMaterialCount;
	mLastPerfShellTraceStats.sceneTextureReferencedBaseCount = sceneTextureAttribution.baseTextureCount;
	mLastPerfShellTraceStats.sceneTextureReferencedGlowCount = sceneTextureAttribution.glowTextureCount;
	mLastPerfShellTraceStats.sceneTextureReferencedNormalCount = sceneTextureAttribution.normalTextureCount;
	mLastPerfShellTraceStats.sceneTextureReferencedMetallicCount = sceneTextureAttribution.metallicTextureCount;
	mLastPerfShellTraceStats.sceneTextureReferencedRoughnessCount = sceneTextureAttribution.roughnessTextureCount;
	mLastPerfShellTraceStats.sceneTextureReferencedEmissiveCount = sceneTextureAttribution.emissiveTextureCount;
	if (!EnsureSkyTexture(sceneView, preserveExistingSky))
	{
		return false;
	}

	std::vector<nri::Descriptor*> descriptors(NRI_SCENE_DESCRIPTOR_NUM, mFrameBuffer->mWhiteTexture->GetResource().shaderView);
	descriptors[0] = mPaletteTexture.shaderView;
	descriptors[1] = GetActiveSkyTexture() != nullptr ? GetActiveSkyTexture()->shaderView : mFrameBuffer->mWhiteTexture->GetResource().shaderView;

	for (uint32_t i = 0; i < std::min<uint32_t>((uint32_t)materials.textures.size(), NRI_MAX_SCENE_TEXTURES); ++i)
	{
		const auto& upload = materials.textures[i];
		if (mFrameBuffer->mActiveCanvasSourceTexture != nullptr &&
			upload.sourceTexture == mFrameBuffer->mActiveCanvasSourceTexture)
		{
			if (!sLoggedActiveCanvasTextureReuse || nri_ptdebug > 0)
			{
				Printf(TEXTCOLOR_ORANGE "NRI PT textures: using a fallback descriptor for the canvas currently being rendered to avoid self-referential camera-texture uploads.\n");
				sLoggedActiveCanvasTextureReuse = true;
			}
			continue;
		}

		if (upload.sourceTexture != nullptr && upload.sourceTexture->isHardwareCanvas())
		{
			auto* hardwareTexture = static_cast<NRIHardwareTexture*>(upload.sourceTexture->GetHardwareTexture(0, 0));
			if (hardwareTexture != nullptr)
			{
				hardwareTexture->EnsureCanvas(upload.sourceTexture);
				if (hardwareTexture->GetResource().shaderView != nullptr)
				{
					descriptors[2 + i] = hardwareTexture->GetResource().shaderView;
					TrackLiveSceneTextureResource(hardwareTexture->GetResource());
					continue;
				}
			}
		}

		if (upload.width == 0 || upload.height == 0)
		{
			continue;
		}

		auto it = mTextureCache.end();
		if (tracePerf)
		{
			const auto start = std::chrono::steady_clock::now();
			it = std::find_if(mTextureCache.begin(), mTextureCache.end(), [&upload](const CachedTexture& entry) { return entry.key == upload.key; });
			lookupMs += DurationMs(start, std::chrono::steady_clock::now());
		}
		else
		{
			it = std::find_if(mTextureCache.begin(), mTextureCache.end(), [&upload](const CachedTexture& entry) { return entry.key == upload.key; });
		}
		if (it == mTextureCache.end())
		{
			lookupMisses++;
		}
		if (it == mTextureCache.end())
		{
			const auto realizeStart = tracePerf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
			std::vector<uint8_t> realizedPixels;
			uint32_t realizedWidth = upload.width;
			uint32_t realizedHeight = upload.height;
			const uint8_t* pixelData = upload.pixels.data();
			if (upload.pixels.empty())
			{
				if (!nri_scene::RealizeTextureUploadPayload(upload, realizedPixels, realizedWidth, realizedHeight))
				{
					continue;
				}
				pixelData = realizedPixels.data();
			}
			else
			{
				realizedWidth = upload.width;
				realizedHeight = upload.height;
			}

			if (pixelData == nullptr || realizedWidth == 0 || realizedHeight == 0)
			{
				continue;
			}

			CachedTexture cacheEntry = {};
			cacheEntry.key = upload.key;
			const nri::Format format = upload.indexed ? nri::Format::R8_UNORM : nri::Format::BGRA8_UNORM;
			const uint32_t rowPitch = upload.indexed ? realizedWidth : realizedWidth * 4u;
			if (!mFrameBuffer->CreateOwnedTexture(cacheEntry.resource, realizedWidth, realizedHeight, format, nri::TextureUsageBits::SHADER_RESOURCE) ||
				!mFrameBuffer->UploadTextureData(cacheEntry.resource, pixelData, realizedWidth, realizedHeight, rowPitch))
			{
				return false;
			}

			mTextureCache.push_back(cacheEntry);
			insertCount++;
			if (tracePerf)
			{
				realizeMs += DurationMs(realizeStart, std::chrono::steady_clock::now());
			}
			it = mTextureCache.end() - 1;
		}

		descriptors[2 + i] = it->resource.shaderView;
	}

	uint32_t actorOverflowTraceLines = 0;
	for (uint32_t materialIndex = 0; materialIndex < (uint32_t)outGpuMaterials.size(); ++materialIndex)
	{
		auto& material = outGpuMaterials[materialIndex];
		const uint32_t originalTextureIndex = material.textureIndex;
		const uint32_t originalNormalTextureIndex = material.normalTextureIndex;
		const uint32_t originalMetallicTextureIndex = material.metallicTextureIndex;
		const uint32_t originalRoughnessTextureIndex = material.roughnessTextureIndex;
		const uint32_t originalEmissiveTextureIndex = material.emissiveTextureIndex;
		bool baseClamped = false;
		bool normalClamped = false;
		bool metallicClamped = false;
		bool roughnessClamped = false;
		bool emissiveClamped = false;
		if (material.textureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			mSceneTextureOverflowStats.baseTextureClampCountLastBuild++;
			material.textureIndex = 0;
			baseClamped = true;
		}
		if (material.normalTextureIndex != UINT32_MAX && material.normalTextureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			mSceneTextureOverflowStats.normalTextureClampCountLastBuild++;
			material.normalTextureIndex = UINT32_MAX;
			normalClamped = true;
		}
		if (material.metallicTextureIndex != UINT32_MAX && material.metallicTextureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			mSceneTextureOverflowStats.metallicTextureClampCountLastBuild++;
			material.metallicTextureIndex = UINT32_MAX;
			metallicClamped = true;
		}
		if (material.roughnessTextureIndex != UINT32_MAX && material.roughnessTextureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			mSceneTextureOverflowStats.roughnessTextureClampCountLastBuild++;
			material.roughnessTextureIndex = UINT32_MAX;
			roughnessClamped = true;
		}
		if (material.emissiveTextureIndex != UINT32_MAX && material.emissiveTextureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			mSceneTextureOverflowStats.emissiveTextureClampCountLastBuild++;
			material.emissiveTextureIndex = 0;
			emissiveClamped = true;
		}

		if (!(baseClamped || normalClamped || metallicClamped || roughnessClamped || emissiveClamped))
		{
			continue;
		}

		const nri_scene::MaterialLightingMetadata* metadata =
			materialIndex < materials.lightMetadata.size() ? &materials.lightMetadata[materialIndex] : nullptr;
		if (metadata == nullptr || metadata->actorIndex < 0)
		{
			continue;
		}

		mLastPerfShellTraceStats.actorOverflowMaterialCount++;
		if (baseClamped)
		{
			mLastPerfShellTraceStats.actorOverflowBaseClampCount++;
		}
		if (normalClamped)
		{
			mLastPerfShellTraceStats.actorOverflowNormalClampCount++;
		}
		if (metallicClamped)
		{
			mLastPerfShellTraceStats.actorOverflowMetallicClampCount++;
		}
		if (roughnessClamped)
		{
			mLastPerfShellTraceStats.actorOverflowRoughnessClampCount++;
		}
		if (emissiveClamped)
		{
			mLastPerfShellTraceStats.actorOverflowEmissiveClampCount++;
		}

		if (!ShouldTraceActorOverflow())
		{
			continue;
		}

		if (actorOverflowTraceLines < NRI_MAX_ACTOR_OVERFLOW_TRACE_LINES)
		{
			Printf(
				"PERF pt actor overflow NRI: frame=%llu reason=%s actor=%d source=%s material=%u texture_id=%u base=%u->%u normal=%u->%u metallic=%u->%u roughness=%u->%u emissive=%u->%u\n",
				(unsigned long long)mFrameIndex,
				mLastPerfShellTraceStats.sceneTextureReason.empty() ? "none" : mLastPerfShellTraceStats.sceneTextureReason.c_str(),
				metadata->actorIndex,
				GetSurfaceSourceTypeName(metadata->sourceType),
				materialIndex,
				metadata->textureId,
				originalTextureIndex,
				material.textureIndex,
				originalNormalTextureIndex,
				material.normalTextureIndex,
				originalMetallicTextureIndex,
				material.metallicTextureIndex,
				originalRoughnessTextureIndex,
				material.roughnessTextureIndex,
				originalEmissiveTextureIndex,
				material.emissiveTextureIndex);
			actorOverflowTraceLines++;
		}
		else
		{
			mLastPerfShellTraceStats.actorOverflowTraceOmittedCount++;
		}
	}
	if (mLastPerfShellTraceStats.actorOverflowTraceOmittedCount > 0 && ShouldTraceActorOverflow())
	{
		Printf(
			"PERF pt actor overflow NRI: frame=%llu reason=%s omitted=%u limit=%u\n",
			(unsigned long long)mFrameIndex,
			mLastPerfShellTraceStats.sceneTextureReason.empty() ? "none" : mLastPerfShellTraceStats.sceneTextureReason.c_str(),
			mLastPerfShellTraceStats.actorOverflowTraceOmittedCount,
			NRI_MAX_ACTOR_OVERFLOW_TRACE_LINES);
	}

	const bool sceneTextureOverflow =
		mSceneTextureOverflowStats.truncatedTextureCountLastBuild > 0 ||
		mSceneTextureOverflowStats.baseTextureClampCountLastBuild > 0 ||
		mSceneTextureOverflowStats.normalTextureClampCountLastBuild > 0 ||
		mSceneTextureOverflowStats.metallicTextureClampCountLastBuild > 0 ||
		mSceneTextureOverflowStats.roughnessTextureClampCountLastBuild > 0 ||
		mSceneTextureOverflowStats.emissiveTextureClampCountLastBuild > 0;
	if (sceneTextureOverflow)
	{
		mSceneTextureOverflowStats.totalOverflowBuilds++;
		if (!mSceneTextureOverflowStats.warningLogged || (int)nri_pttraceframes > 0 || (int)nri_ptactorspritetrace > 0 || nri_ptdebug > 0)
		{
			Printf(TEXTCOLOR_ORANGE "NRI PT scene textures: requested=%u cap=%u truncated=%u clamps=base:%u normal:%u metallic:%u roughness:%u emissive:%u\n",
				mSceneTextureOverflowStats.textureCountLastBuild,
				NRI_MAX_SCENE_TEXTURES,
				mSceneTextureOverflowStats.truncatedTextureCountLastBuild,
				mSceneTextureOverflowStats.baseTextureClampCountLastBuild,
				mSceneTextureOverflowStats.normalTextureClampCountLastBuild,
				mSceneTextureOverflowStats.metallicTextureClampCountLastBuild,
				mSceneTextureOverflowStats.roughnessTextureClampCountLastBuild,
				mSceneTextureOverflowStats.emissiveTextureClampCountLastBuild);
			mSceneTextureOverflowStats.warningLogged = true;
		}
	}

	mSceneTextureCacheDebugStats.cacheEntriesLastBuild = (uint32_t)mTextureCache.size();
	mSceneTextureCacheDebugStats.cacheEntriesHighWater = std::max(mSceneTextureCacheDebugStats.cacheEntriesHighWater, (uint32_t)mTextureCache.size());
	mSceneTextureCacheDebugStats.lookupMissesLastBuild = lookupMisses;
	mSceneTextureCacheDebugStats.insertCountLastBuild = insertCount;
	mSceneTextureCacheDebugStats.lookupMsLastBuild = lookupMs;
	mSceneTextureCacheDebugStats.realizeMsLastBuild = realizeMs;
	mLastPerfShellTraceStats.sceneTextureCacheCount = (uint32_t)mTextureCache.size();
	mLastPerfShellTraceStats.sceneTextureCacheMisses = lookupMisses;
	mLastPerfShellTraceStats.sceneTextureCacheInserts = insertCount;
	mLastPerfShellTraceStats.sceneTextureLookupMs = lookupMs;
	mLastPerfShellTraceStats.sceneTextureRealizeMs = realizeMs;
	bool updated = false;
	if (tracePerf)
	{
		const auto descriptorStart = std::chrono::steady_clock::now();
		updated = UpdateSceneTextureSet(descriptors, reason);
		descriptorMs = DurationMs(descriptorStart, std::chrono::steady_clock::now());
	}
	else
	{
		updated = UpdateSceneTextureSet(descriptors, reason);
	}
	mSceneTextureCacheDebugStats.descriptorMsLastBuild = descriptorMs;
	mLastPerfShellTraceStats.sceneTextureDescriptorMs = descriptorMs;
	return updated;
}

bool NRIRenderer::UseFallbackSceneTextures(bool preserveExistingSky, const char* reason)
{
	mLiveSceneTextureResources.clear();
	mLastPerfShellTraceStats.sceneTextureReason = reason != nullptr ? reason : "fallback";
	mLastPerfShellTraceStats.sceneTextureRequestedCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedActorMaterialCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedBaseCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedGlowCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedNormalCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedMetallicCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedRoughnessCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedEmissiveCount = 0;
	mLastPerfShellTraceStats.actorOverflowMaterialCount = 0;
	mLastPerfShellTraceStats.actorOverflowBaseClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowNormalClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowMetallicClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowRoughnessClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowEmissiveClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowTraceOmittedCount = 0;
	if (!preserveExistingSky || GetActiveSkyTexture() == nullptr)
	{
		EnsureSkyTexture(nri_scene::SceneView{}, false);
	}
	std::vector<nri::Descriptor*> descriptors(NRI_SCENE_DESCRIPTOR_NUM, mFrameBuffer->mWhiteTexture->GetResource().shaderView);
	descriptors[0] = mFrameBuffer->mWhiteTexture->GetResource().shaderView;
	descriptors[1] = GetActiveSkyTexture() != nullptr && GetActiveSkyTexture()->shaderView != nullptr ? GetActiveSkyTexture()->shaderView : mFrameBuffer->mWhiteTexture->GetResource().shaderView;
	return UpdateSceneTextureSet(descriptors, reason != nullptr ? reason : "fallback");
}

uint32_t NRIRenderer::CountPotentialOutstandingQueuedFrames() const
{
	if (mFrameBuffer == nullptr)
	{
		return 0;
	}

	uint32_t count = 0;
	for (uint32_t i = 0; i < (uint32_t)mFrameBuffer->mQueuedFrames.size(); ++i)
	{
		if (i == mFrameBuffer->mCurrentQueuedFrameIndex)
		{
			continue;
		}

		const auto& queuedFrame = mFrameBuffer->mQueuedFrames[i];
		if (queuedFrame.hasSubmittedWork && queuedFrame.lastSubmittedFenceValue != 0)
		{
			count++;
		}
	}

	return count;
}

uint32_t NRIRenderer::GetCurrentQueuedFrameIndex() const
{
	if (mFrameBuffer == nullptr || mFrameBuffer->mQueuedFrames.empty())
	{
		return 0;
	}

	return std::min<uint32_t>(mFrameBuffer->mCurrentQueuedFrameIndex, (uint32_t)mFrameBuffer->mQueuedFrames.size() - 1u);
}

nri::DescriptorSet* NRIRenderer::GetCurrentSceneTextureSet() const
{
	const uint32_t queuedFrameIndex = GetCurrentQueuedFrameIndex();
	return queuedFrameIndex < mSceneTextureSets.size() ? mSceneTextureSets[queuedFrameIndex] : nullptr;
}

nri::DescriptorSet* NRIRenderer::GetCurrentSceneDataSet() const
{
	const uint32_t queuedFrameIndex = GetCurrentQueuedFrameIndex();
	return queuedFrameIndex < mSceneDataSets.size() ? mSceneDataSets[queuedFrameIndex] : nullptr;
}

bool NRIRenderer::IsCurrentSceneDataDescriptorsInitialized() const
{
	const uint32_t queuedFrameIndex = GetCurrentQueuedFrameIndex();
	return queuedFrameIndex < mSceneDataDescriptorsInitialized.size() && mSceneDataDescriptorsInitialized[queuedFrameIndex] != 0;
}

void NRIRenderer::SetCurrentSceneDataDescriptorsInitialized(bool value)
{
	const uint32_t queuedFrameIndex = GetCurrentQueuedFrameIndex();
	if (queuedFrameIndex >= mSceneDataDescriptorsInitialized.size())
	{
		return;
	}

	mSceneDataDescriptorsInitialized[queuedFrameIndex] = value ? 1u : 0u;
}

void NRIRenderer::TraceSharedDescriptorRewrite(const char* setName, const char* reason, uint64_t descriptorHash, uint32_t descriptorCount, bool sceneTextureSet)
{
	if (sceneTextureSet)
	{
		mDescriptorCoherencyDebugStats.sceneTextureSetUpdates++;
		mDescriptorCoherencyDebugStats.lastSceneTextureDescriptorHash = descriptorHash;
		mDescriptorCoherencyDebugStats.lastSceneTextureDescriptorCount = descriptorCount;
		mDescriptorCoherencyDebugStats.lastSceneTextureReason = reason != nullptr ? reason : "unlabeled";
	}
	else
	{
		mDescriptorCoherencyDebugStats.sceneDataSetUpdates++;
		mDescriptorCoherencyDebugStats.lastSceneDataDescriptorHash = descriptorHash;
		mDescriptorCoherencyDebugStats.lastSceneDataDescriptorCount = descriptorCount;
		mDescriptorCoherencyDebugStats.lastSceneDataReason = reason != nullptr ? reason : "unlabeled";
	}

	uint32_t queuedFrameIndex = 0;
	uint64_t queuedFrameFence = 0;
	uint64_t submittedFence = 0;
	if (mFrameBuffer != nullptr)
	{
		queuedFrameIndex = mFrameBuffer->mCurrentQueuedFrameIndex;
		submittedFence = mFrameBuffer->mSubmittedFenceValue;
		if (queuedFrameIndex < mFrameBuffer->mQueuedFrames.size())
		{
			queuedFrameFence = mFrameBuffer->mQueuedFrames[queuedFrameIndex].lastSubmittedFenceValue;
		}
	}

	const uint32_t outstandingQueuedFrames = CountPotentialOutstandingQueuedFrames();
	if (sceneTextureSet)
	{
		mDescriptorCoherencyDebugStats.lastSceneTextureQueuedFrameIndex = queuedFrameIndex;
		mDescriptorCoherencyDebugStats.lastSceneTextureQueuedFrameFence = queuedFrameFence;
		mDescriptorCoherencyDebugStats.lastSceneTextureSubmittedFence = submittedFence;
		mDescriptorCoherencyDebugStats.lastSceneTextureOutstandingQueuedFrames = outstandingQueuedFrames;
	}
	else
	{
		mDescriptorCoherencyDebugStats.lastSceneDataQueuedFrameIndex = queuedFrameIndex;
		mDescriptorCoherencyDebugStats.lastSceneDataQueuedFrameFence = queuedFrameFence;
		mDescriptorCoherencyDebugStats.lastSceneDataSubmittedFence = submittedFence;
		mDescriptorCoherencyDebugStats.lastSceneDataOutstandingQueuedFrames = outstandingQueuedFrames;
	}

	if (!ShouldTraceActorSpriteCoherency())
	{
		return;
	}

	Printf("NRI PT descriptor rewrite: frame=%u set=%s reason=%s hash=0x%llx descriptors=%u qframe=%u slot_fence=%llu submitted_fence=%llu outstanding_slots=%u\n",
		mFrameIndex,
		setName != nullptr ? setName : "unknown",
		reason != nullptr ? reason : "unlabeled",
		(unsigned long long)descriptorHash,
		descriptorCount,
		queuedFrameIndex,
		(unsigned long long)queuedFrameFence,
		(unsigned long long)submittedFence,
		outstandingQueuedFrames);
}

void NRIRenderer::TraceActorSpriteMaterialAssignments(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& outMaterials, const char* traceLabel)
{
	mDescriptorCoherencyDebugStats.actorMaterialBuilds++;
	mDescriptorCoherencyDebugStats.lastMaterialBuildLabel = traceLabel != nullptr ? traceLabel : "unlabeled";
	mDescriptorCoherencyDebugStats.lastMaterialCount = (uint32_t)outMaterials.materials.size();
	mDescriptorCoherencyDebugStats.lastTextureCount = (uint32_t)outMaterials.textures.size();
	mDescriptorCoherencyDebugStats.lastMaterialBridgeHash = HashMaterialBridgeSummary(outMaterials);

	const uint32_t spriteMaterialBase = (uint32_t)(sceneView.opaqueWalls.size() + sceneView.opaqueFlats.size());
	uint32_t actorSurfaceCount = 0;
	uint64_t actorHash = 1469598103934665603ull;
	std::unordered_set<int32_t> actorIndices;
	actorIndices.reserve(sceneView.opaqueSprites.size());
	uint32_t printed = 0;

	for (uint32_t spriteIndex = 0; spriteIndex < (uint32_t)sceneView.opaqueSprites.size(); ++spriteIndex)
	{
		const auto& surface = sceneView.opaqueSprites[spriteIndex];
		if (surface.provenance.actorIndex < 0)
		{
			continue;
		}

		const uint32_t materialIndex = spriteMaterialBase + spriteIndex;
		if (materialIndex >= outMaterials.materials.size() || materialIndex >= outMaterials.lightMetadata.size())
		{
			continue;
		}

		const auto& material = outMaterials.materials[materialIndex];
		const auto& metadata = outMaterials.lightMetadata[materialIndex];
		actorSurfaceCount++;
		actorIndices.insert(surface.provenance.actorIndex);
		actorHash = CoherencyHashCombine64(actorHash, (uint64_t)surface.provenance.actorIndex);
		actorHash = CoherencyHashCombine64(actorHash, (uint64_t)(uint32_t)surface.provenance.sourceType);
		actorHash = CoherencyHashCombine64(actorHash, (uint64_t)materialIndex);
		actorHash = CoherencyHashCombine64(actorHash, (uint64_t)metadata.textureId);
		actorHash = CoherencyHashCombine64(actorHash, (uint64_t)material.textureIndex);
		actorHash = CoherencyHashCombine64(actorHash, (uint64_t)material.paletteIndex);
		actorHash = CoherencyHashCombine64(actorHash, (uint64_t)material.emissiveMode);
		actorHash = CoherencyHashCombine64(actorHash, (uint64_t)material.emissiveTextureIndex);
		actorHash = CoherencyHashCombine64(actorHash, metadata.materialKey);

		if (ShouldTraceActorSpriteVerbose() && printed < 32)
		{
			Printf("NRI PT actor-sprite material: frame=%u label=%s actor=%d source=%s material=%u tex_id=%u tex_index=%u emissive_mode=%u emissive_tex=%u palette=%u flags=0x%x light_flags=0x%x material_key=0x%llx tex_ptr=%p\n",
				mFrameIndex,
				traceLabel != nullptr ? traceLabel : "unlabeled",
				surface.provenance.actorIndex,
				GetSurfaceSourceTypeName(surface.provenance.sourceType),
				materialIndex,
				metadata.textureId,
				material.textureIndex,
				material.emissiveMode,
				material.emissiveTextureIndex,
				material.paletteIndex,
				material.flags,
				material.lightingFlags,
				(unsigned long long)metadata.materialKey,
				metadata.texture);
			printed++;
		}
	}

	mDescriptorCoherencyDebugStats.lastActorSpriteSurfaceCount = actorSurfaceCount;
	mDescriptorCoherencyDebugStats.lastActorSpriteActorCount = (uint32_t)actorIndices.size();
	mDescriptorCoherencyDebugStats.lastActorSpriteMaterialHash = actorHash;

	if (actorSurfaceCount == 0 || !ShouldTraceActorSpriteCoherency())
	{
		return;
	}

	Printf("NRI PT actor-sprite materials: frame=%u label=%s materials=%u textures=%u actor_surfaces=%u actor_count=%u bridge_hash=0x%llx actor_hash=0x%llx qframe=%u outstanding_slots=%u\n",
		mFrameIndex,
		traceLabel != nullptr ? traceLabel : "unlabeled",
		(uint32_t)outMaterials.materials.size(),
		(uint32_t)outMaterials.textures.size(),
		actorSurfaceCount,
		(uint32_t)actorIndices.size(),
		(unsigned long long)mDescriptorCoherencyDebugStats.lastMaterialBridgeHash,
		(unsigned long long)actorHash,
		mFrameBuffer != nullptr ? mFrameBuffer->mCurrentQueuedFrameIndex : 0u,
		CountPotentialOutstandingQueuedFrames());
}

void NRIRenderer::TraceActorSpriteEvent(const PathTracingActorSpriteTraceEvent& event)
{
	if (!ShouldTraceActorSpriteVerbose())
	{
		return;
	}

	Printf("NRI PT actor-sprite %s: actor=%d stat=%d pic=%d base_tex=%d resolved_tex=%d pal=%d shade=%d cstat=0x%x cstat2=0x%x noanimate=%s fullbright=%s drawlist=%u tex_ptr=%p\n",
		GetActorSpriteTraceStageName(event.stage),
		event.actorIndex,
		event.spriteStatnum,
		event.spritePicnum,
		event.baseTextureId,
		event.resolvedTextureId,
		event.palette,
		event.shade,
		event.cstat,
		event.cstat2,
		event.noAnimate ? "yes" : "no",
		event.fullbright ? "yes" : "no",
		event.drawListType,
		event.resolvedGameTexture);
}

bool NRIRenderer::CreateStructuredBuffer(NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after)
{
	if (resource.buffer != nullptr || resource.shaderView != nullptr)
	{
		WaitForCommandsTracked();
	}

	DestroyBufferResource(resource);

	nri::BufferDesc desc = {};
	desc.size = std::max<uint64_t>(size, stride);
	desc.structureStride = stride;
	desc.usage = usage;

	if (mFrameBuffer->mCore.CreateCommittedBuffer(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE_UPLOAD, 0.0f, desc, resource.buffer) != nri::Result::SUCCESS)
	{
		return false;
	}

	nri::MemoryDesc memoryDesc = {};
	mFrameBuffer->mCore.GetBufferMemoryDesc(*resource.buffer, nri::MemoryLocation::DEVICE_UPLOAD, memoryDesc);
	resource.size = desc.size;
	resource.memorySize = memoryDesc.size;
	resource.memoryLocation = nri::MemoryLocation::DEVICE_UPLOAD;
	resource.usedSize = size;
	resource.stride = stride;

	nri::BufferViewDesc viewDesc = {};
	viewDesc.buffer = resource.buffer;
	viewDesc.type = nri::BufferView::STRUCTURED_BUFFER;
	viewDesc.offset = 0;
	viewDesc.size = nri::WHOLE_SIZE;
	viewDesc.structureStride = stride;
	if (mFrameBuffer->mCore.CreateBufferView(viewDesc, resource.shaderView) != nri::Result::SUCCESS)
	{
		return false;
	}

	if (data != nullptr && size != 0)
	{
		void* mapped = mFrameBuffer->mCore.MapBuffer(*resource.buffer, 0, desc.size);
		if (mapped == nullptr)
		{
			return false;
		}

		std::memcpy(mapped, data, (size_t)size);
		if (desc.size > size)
		{
			std::memset(static_cast<uint8_t*>(mapped) + size, 0, (size_t)(desc.size - size));
		}
		mFrameBuffer->mCore.UnmapBuffer(*resource.buffer);
	}

	if (mFrameBuffer->mCommandBuffer != nullptr && after.access != nri::AccessBits::NONE)
	{
		nri::BufferBarrierDesc barrier = {};
		barrier.buffer = resource.buffer;
		barrier.before = {};
		barrier.after = after;

		nri::BarrierDesc barrierDesc = {};
		barrierDesc.buffers = &barrier;
		barrierDesc.bufferNum = 1;
		mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrierDesc);
	}

	return true;
}

bool NRIRenderer::EnsureStructuredBuffer(NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after)
{
	const uint64_t requiredSize = std::max<uint64_t>(size, stride);
	const bool needsGrowth =
		resource.buffer == nullptr ||
		resource.shaderView == nullptr ||
		resource.stride != stride ||
		resource.size < requiredSize;

	stats.bytesUploadedLastFrame = size;
	stats.growEventsLastFrame = 0;
	stats.overwriteEventsLastFrame = 0;
	stats.uploadCount++;
	stats.peakUsedBytes = std::max(stats.peakUsedBytes, size);
	NotePerfBufferUpload(&stats, size, needsGrowth);

	if (needsGrowth)
	{
		const uint64_t grownSize = GetGrownBufferSize(resource.size, requiredSize, stride);
		if (resource.buffer != nullptr || resource.shaderView != nullptr)
		{
			WaitForCommandsTracked();
		}
		DestroyBufferResource(resource);

		nri::BufferDesc desc = {};
		desc.size = std::max<uint64_t>(grownSize, stride);
		desc.structureStride = stride;
		desc.usage = usage;

		if (mFrameBuffer->mCore.CreateCommittedBuffer(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE_UPLOAD, 0.0f, desc, resource.buffer) != nri::Result::SUCCESS)
		{
			return false;
		}

		nri::MemoryDesc memoryDesc = {};
		mFrameBuffer->mCore.GetBufferMemoryDesc(*resource.buffer, nri::MemoryLocation::DEVICE_UPLOAD, memoryDesc);
		resource.size = desc.size;
		resource.memorySize = memoryDesc.size;
		resource.memoryLocation = nri::MemoryLocation::DEVICE_UPLOAD;
		resource.usedSize = size;
		resource.stride = stride;

		nri::BufferViewDesc viewDesc = {};
		viewDesc.buffer = resource.buffer;
		viewDesc.type = nri::BufferView::STRUCTURED_BUFFER;
		viewDesc.offset = 0;
		viewDesc.size = nri::WHOLE_SIZE;
		viewDesc.structureStride = stride;
		if (mFrameBuffer->mCore.CreateBufferView(viewDesc, resource.shaderView) != nri::Result::SUCCESS)
		{
			return false;
		}

		stats.growthCount++;
		stats.growEventsLastFrame = 1;
	}
	else
	{
		resource.usedSize = size;
		stats.overwriteCount++;
		stats.overwriteEventsLastFrame = 1;
	}

	if (data != nullptr && size != 0)
	{
		if (!needsGrowth)
		{
			// Scene buffers are reused persistent DEVICE_UPLOAD allocations. Fence before
			// overwriting them so prior queued frames cannot read partially updated data.
			WaitForCommandsTracked();
		}

		void* mapped = mFrameBuffer->mCore.MapBuffer(*resource.buffer, 0, resource.size);
		if (mapped == nullptr)
		{
			return false;
		}

		std::memcpy(mapped, data, (size_t)size);
		mFrameBuffer->mCore.UnmapBuffer(*resource.buffer);
	}

	if (mFrameBuffer->mCommandBuffer != nullptr && after.access != nri::AccessBits::NONE)
	{
		nri::BufferBarrierDesc barrier = {};
		barrier.buffer = resource.buffer;
		barrier.before = {};
		barrier.after = after;

		nri::BarrierDesc barrierDesc = {};
		barrierDesc.buffers = &barrier;
		barrierDesc.bufferNum = 1;
		mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrierDesc);
	}

	return true;
}

bool NRIRenderer::CreateBufferWithoutView(NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage)
{
	if (resource.buffer != nullptr)
	{
		WaitForCommandsTracked();
	}

	DestroyBufferResource(resource);

	nri::BufferDesc desc = {};
	desc.size = std::max<uint64_t>(size, stride);
	desc.structureStride = stride;
	desc.usage = usage;
	if (mFrameBuffer->mCore.CreateCommittedBuffer(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, desc, resource.buffer) != nri::Result::SUCCESS)
	{
		return false;
	}

	nri::MemoryDesc memoryDesc = {};
	mFrameBuffer->mCore.GetBufferMemoryDesc(*resource.buffer, nri::MemoryLocation::DEVICE, memoryDesc);
	resource.size = desc.size;
	resource.memorySize = memoryDesc.size;
	resource.memoryLocation = nri::MemoryLocation::DEVICE;
	resource.usedSize = size;
	resource.stride = stride;
	return true;
}

bool NRIRenderer::UploadSceneBuffers(const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials)
{
	return UploadSceneBuffers(mVertexBuffer, mIndexBuffer, mPrimitiveBuffer, mMaterialBuffer, geometry, materials);
}

bool NRIRenderer::UploadSceneBuffers(
	NRIBufferResource& vertexBuffer,
	NRIBufferResource& indexBuffer,
	NRIBufferResource& primitiveBuffer,
	NRIBufferResource& materialBuffer,
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& materials)
{
	Clocker clock(NriPTSceneBuffers);
	mVertexBufferStats.bytesUploadedLastFrame = 0;
	mVertexBufferStats.growEventsLastFrame = 0;
	mVertexBufferStats.overwriteEventsLastFrame = 0;
	mIndexBufferStats.bytesUploadedLastFrame = 0;
	mIndexBufferStats.growEventsLastFrame = 0;
	mIndexBufferStats.overwriteEventsLastFrame = 0;
	mPrimitiveBufferStats.bytesUploadedLastFrame = 0;
	mPrimitiveBufferStats.growEventsLastFrame = 0;
	mPrimitiveBufferStats.overwriteEventsLastFrame = 0;
	mMaterialBufferStats.bytesUploadedLastFrame = 0;
	mMaterialBufferStats.growEventsLastFrame = 0;
	mMaterialBufferStats.overwriteEventsLastFrame = 0;
	std::vector<nri_scene::PrimitiveData> gpuPrimitives = geometry.primitives;
	const size_t primitiveCount = std::min(gpuPrimitives.size(), geometry.primitiveProvenance.size());
	for (size_t primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
	{
		const int32_t chunkIndex = geometry.primitiveProvenance[primitiveIndex].mapChunkIndex;
		gpuPrimitives[primitiveIndex].reserved0 = chunkIndex >= 0 ? (uint32_t)chunkIndex : UINT32_MAX;
	}
	for (size_t primitiveIndex = primitiveCount; primitiveIndex < gpuPrimitives.size(); ++primitiveIndex)
	{
		gpuPrimitives[primitiveIndex].reserved0 = UINT32_MAX;
	}

	return
		EnsureStructuredBuffer(vertexBuffer, mVertexBufferStats, geometry.vertices.data(), geometry.vertices.size() * sizeof(nri_scene::SceneVertex), sizeof(nri_scene::SceneVertex), NRIFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), NRIAccelerationStructureBuildInputAccess()) &&
		EnsureStructuredBuffer(indexBuffer, mIndexBufferStats, geometry.indices.data(), geometry.indices.size() * sizeof(uint32_t), sizeof(uint32_t), NRIFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), NRIAccelerationStructureBuildInputAccess()) &&
		EnsureStructuredBuffer(primitiveBuffer, mPrimitiveBufferStats, gpuPrimitives.data(), gpuPrimitives.size() * sizeof(nri_scene::PrimitiveData), sizeof(nri_scene::PrimitiveData), nri::BufferUsageBits::SHADER_RESOURCE, NRIComputeShaderResourceAccess()) &&
		EnsureStructuredBuffer(materialBuffer, mMaterialBufferStats, materials.data(), materials.size() * sizeof(nri_scene::MaterialData), sizeof(nri_scene::MaterialData), nri::BufferUsageBits::SHADER_RESOURCE, NRIComputeShaderResourceAccess());
}

bool NRIRenderer::BuildStaticMapAccelerationStructures()
{
	Clocker clock(NriPTAcceleration);

	if (mStaticMapScene.chunks.empty() ||
		!mStaticMapChunkAtlas.valid ||
		mStaticMapChunkAtlas.chunks.size() != mStaticMapScene.chunks.size())
	{
		return false;
	}

	const bool needsWait =
		mTopLevelAS.accelerationStructure != nullptr ||
		mEmissiveTopLevelAS.accelerationStructure != nullptr ||
		mDynamicBottomLevelAS.accelerationStructure != nullptr ||
		mTlasInstanceBuffer.buffer != nullptr ||
		mEmissiveTlasInstanceBuffer.buffer != nullptr ||
		mSceneInstanceBuffer.buffer != nullptr ||
		mScratchBuffer.buffer != nullptr ||
		mTopLevelScratchBuffer.buffer != nullptr;
	if (needsWait)
	{
		WaitForCommandsTracked();
	}

	DestroyBufferResource(mTlasInstanceBuffer);
	DestroyBufferResource(mEmissiveTlasInstanceBuffer);
	DestroyBufferResource(mSceneInstanceBuffer);
	DestroyBufferResource(mScratchBuffer);
	DestroyBufferResource(mTopLevelScratchBuffer);
	DestroyAccelerationStructureResource(mDynamicBottomLevelAS);
	DestroyAccelerationStructureResource(mTopLevelAS);
	DestroyAccelerationStructureResource(mEmissiveTopLevelAS);

	for (auto& chunk : mStaticMapScene.chunks)
	{
		DestroyAccelerationStructureResource(chunk.accelerationStructure);
	}

	uint64_t maxScratchSize = 0;
	for (size_t chunkIndex = 0; chunkIndex < mStaticMapScene.chunks.size(); ++chunkIndex)
	{
		auto& chunk = mStaticMapScene.chunks[chunkIndex];
		const auto& atlasChunk = mStaticMapChunkAtlas.chunks[chunkIndex];
		nri::BottomLevelGeometryDesc geometryDesc = {};
		geometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
		geometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
		geometryDesc.triangles.vertexBuffer = mStaticVertexBuffer.buffer;
		geometryDesc.triangles.vertexOffset = 0;
		geometryDesc.triangles.vertexNum = mStaticMapChunkAtlas.vertexCount;
		geometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
		geometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
		geometryDesc.triangles.indexBuffer = mStaticIndexBuffer.buffer;
		geometryDesc.triangles.indexOffset = (uint64_t)atlasChunk.indexOffset * sizeof(uint32_t);
		geometryDesc.triangles.indexNum = atlasChunk.indexCount;
		geometryDesc.triangles.indexType = nri::IndexType::UINT32;

		nri::AccelerationStructureDesc blasDesc = {};
		blasDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
		blasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
		blasDesc.geometryOrInstanceNum = 1;
		blasDesc.geometries = &geometryDesc;
		if (mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, blasDesc, chunk.accelerationStructure.accelerationStructure) != nri::Result::SUCCESS)
		{
			return false;
		}

		nri::MemoryDesc memoryDesc = {};
		mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(*chunk.accelerationStructure.accelerationStructure, nri::MemoryLocation::DEVICE, memoryDesc);
		chunk.accelerationStructure.memorySize = memoryDesc.size;
		chunk.accelerationStructure.memoryLocation = nri::MemoryLocation::DEVICE;

		maxScratchSize = std::max(maxScratchSize, mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*chunk.accelerationStructure.accelerationStructure));
	}

	if (!CreateBufferWithoutView(mScratchBuffer, maxScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
	{
		return false;
	}

	std::vector<nri::BufferBarrierDesc> blasBarriers;
	blasBarriers.reserve(mStaticMapScene.chunks.size());
	for (size_t chunkIndex = 0; chunkIndex < mStaticMapScene.chunks.size(); ++chunkIndex)
	{
		auto& chunk = mStaticMapScene.chunks[chunkIndex];
		const auto& atlasChunk = mStaticMapChunkAtlas.chunks[chunkIndex];
		nri::BottomLevelGeometryDesc geometryDesc = {};
		geometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
		geometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
		geometryDesc.triangles.vertexBuffer = mStaticVertexBuffer.buffer;
		geometryDesc.triangles.vertexOffset = 0;
		geometryDesc.triangles.vertexNum = mStaticMapChunkAtlas.vertexCount;
		geometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
		geometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
		geometryDesc.triangles.indexBuffer = mStaticIndexBuffer.buffer;
		geometryDesc.triangles.indexOffset = (uint64_t)atlasChunk.indexOffset * sizeof(uint32_t);
		geometryDesc.triangles.indexNum = atlasChunk.indexCount;
		geometryDesc.triangles.indexType = nri::IndexType::UINT32;

		nri::BuildBottomLevelAccelerationStructureDesc build = {};
		build.dst = chunk.accelerationStructure.accelerationStructure;
		build.geometries = &geometryDesc;
		build.geometryNum = 1;
		build.scratchBuffer = mScratchBuffer.buffer;
		build.scratchOffset = 0;
		mFrameBuffer->mRayTracing.CmdBuildBottomLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &build, 1);

		if (chunkIndex + 1 < mStaticMapScene.chunks.size())
		{
			nri::BufferBarrierDesc scratchBarrier = {};
			scratchBarrier.buffer = mScratchBuffer.buffer;
			scratchBarrier.before = NRIAccelerationStructureScratchAccess();
			scratchBarrier.after = NRIAccelerationStructureScratchAccess();

			nri::BarrierDesc scratchBarrierDesc = {};
			scratchBarrierDesc.buffers = &scratchBarrier;
			scratchBarrierDesc.bufferNum = 1;
			mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, scratchBarrierDesc);
		}

		nri::BufferBarrierDesc barrier = {};
		barrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*chunk.accelerationStructure.accelerationStructure);
		barrier.before = NRIAccelerationStructureWriteAccess();
		barrier.after = NRIAccelerationStructureReadAccess();
		blasBarriers.push_back(barrier);
	}

	if (!blasBarriers.empty())
	{
		nri::BarrierDesc blasBarrierDesc = {};
		blasBarrierDesc.buffers = blasBarriers.data();
		blasBarrierDesc.bufferNum = (uint32_t)blasBarriers.size();
		mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, blasBarrierDesc);
	}

	std::vector<nri::TopLevelInstance> instances;
	std::vector<SceneInstanceData> sceneInstances;
	BuildStaticMapInstances(instances, sceneInstances);
	mStaticAccelerationBuildSerial = mStaticMapScene.buildSerial;
	return
		BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static) &&
		UpdateSceneDataSet(
			mStaticVertexBuffer,
			mStaticIndexBuffer,
			mStaticPrimitiveBuffer,
			mStaticMaterialBuffer,
			mStaticVertexBuffer,
			mStaticIndexBuffer,
			mStaticPrimitiveBuffer,
			mStaticMaterialBuffer,
			sceneInstances,
			(uint32_t)mStaticMapScene.geometry.primitives.size(),
			0u,
			(uint32_t)mStaticMapScene.gpuMaterials.size(),
			0u,
			"build_static_map_scene");
}

bool NRIRenderer::BuildStaticMapAccelerationStructures(
	StaticMapSceneCache& staticScene,
	StaticMapSceneResources& staticResources,
	bool updateLiveState)
{
	Clocker clock(NriPTAcceleration);

	if (staticScene.chunks.empty() ||
		!staticResources.chunkAtlas.valid ||
		staticResources.chunkAtlas.chunks.size() != staticScene.chunks.size())
	{
		return false;
	}

	const bool needsWait =
		staticResources.topLevelAS.accelerationStructure != nullptr ||
		staticResources.tlasInstanceBuffer.buffer != nullptr ||
		staticResources.scratchBuffer.buffer != nullptr ||
		staticResources.topLevelScratchBuffer.buffer != nullptr;
	if (needsWait)
	{
		WaitForCommandsTracked();
	}

	DestroyBufferResource(staticResources.tlasInstanceBuffer);
	DestroyBufferResource(staticResources.scratchBuffer);
	DestroyBufferResource(staticResources.topLevelScratchBuffer);
	DestroyAccelerationStructureResource(staticResources.topLevelAS);

	for (auto& chunk : staticScene.chunks)
	{
		DestroyAccelerationStructureResource(chunk.accelerationStructure);
	}

	uint64_t maxScratchSize = 0;
	for (size_t chunkIndex = 0; chunkIndex < staticScene.chunks.size(); ++chunkIndex)
	{
		auto& chunk = staticScene.chunks[chunkIndex];
		const auto& atlasChunk = staticResources.chunkAtlas.chunks[chunkIndex];
		nri::BottomLevelGeometryDesc geometryDesc = {};
		geometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
		geometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
		geometryDesc.triangles.vertexBuffer = staticResources.vertexBuffer.buffer;
		geometryDesc.triangles.vertexOffset = 0;
		geometryDesc.triangles.vertexNum = staticResources.chunkAtlas.vertexCount;
		geometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
		geometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
		geometryDesc.triangles.indexBuffer = staticResources.indexBuffer.buffer;
		geometryDesc.triangles.indexOffset = (uint64_t)atlasChunk.indexOffset * sizeof(uint32_t);
		geometryDesc.triangles.indexNum = atlasChunk.indexCount;
		geometryDesc.triangles.indexType = nri::IndexType::UINT32;

		nri::AccelerationStructureDesc blasDesc = {};
		blasDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
		blasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
		blasDesc.geometryOrInstanceNum = 1;
		blasDesc.geometries = &geometryDesc;
		if (mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, blasDesc, chunk.accelerationStructure.accelerationStructure) != nri::Result::SUCCESS)
		{
			return false;
		}

		nri::MemoryDesc memoryDesc = {};
		mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(*chunk.accelerationStructure.accelerationStructure, nri::MemoryLocation::DEVICE, memoryDesc);
		chunk.accelerationStructure.memorySize = memoryDesc.size;
		chunk.accelerationStructure.memoryLocation = nri::MemoryLocation::DEVICE;

		maxScratchSize = std::max(maxScratchSize, mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*chunk.accelerationStructure.accelerationStructure));
	}

	if (!CreateBufferWithoutView(staticResources.scratchBuffer, maxScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
	{
		return false;
	}

	std::vector<nri::BufferBarrierDesc> blasBarriers;
	blasBarriers.reserve(staticScene.chunks.size());
	for (size_t chunkIndex = 0; chunkIndex < staticScene.chunks.size(); ++chunkIndex)
	{
		auto& chunk = staticScene.chunks[chunkIndex];
		const auto& atlasChunk = staticResources.chunkAtlas.chunks[chunkIndex];
		nri::BottomLevelGeometryDesc geometryDesc = {};
		geometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
		geometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
		geometryDesc.triangles.vertexBuffer = staticResources.vertexBuffer.buffer;
		geometryDesc.triangles.vertexOffset = 0;
		geometryDesc.triangles.vertexNum = staticResources.chunkAtlas.vertexCount;
		geometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
		geometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
		geometryDesc.triangles.indexBuffer = staticResources.indexBuffer.buffer;
		geometryDesc.triangles.indexOffset = (uint64_t)atlasChunk.indexOffset * sizeof(uint32_t);
		geometryDesc.triangles.indexNum = atlasChunk.indexCount;
		geometryDesc.triangles.indexType = nri::IndexType::UINT32;

		nri::BuildBottomLevelAccelerationStructureDesc build = {};
		build.dst = chunk.accelerationStructure.accelerationStructure;
		build.geometries = &geometryDesc;
		build.geometryNum = 1;
		build.scratchBuffer = staticResources.scratchBuffer.buffer;
		build.scratchOffset = 0;
		mFrameBuffer->mRayTracing.CmdBuildBottomLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &build, 1);

		if (chunkIndex + 1 < staticScene.chunks.size())
		{
			// The static chunk path deliberately reuses one scratch buffer across many BLAS builds.
			// Serialize reuse explicitly so later builds do not stomp scratch data that the GPU is still consuming.
			nri::BufferBarrierDesc scratchBarrier = {};
			scratchBarrier.buffer = staticResources.scratchBuffer.buffer;
			scratchBarrier.before = NRIAccelerationStructureScratchAccess();
			scratchBarrier.after = NRIAccelerationStructureScratchAccess();

			nri::BarrierDesc scratchBarrierDesc = {};
			scratchBarrierDesc.buffers = &scratchBarrier;
			scratchBarrierDesc.bufferNum = 1;
			mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, scratchBarrierDesc);
		}

		nri::BufferBarrierDesc barrier = {};
		barrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*chunk.accelerationStructure.accelerationStructure);
		barrier.before = NRIAccelerationStructureWriteAccess();
		barrier.after = NRIAccelerationStructureReadAccess();
		blasBarriers.push_back(barrier);
	}

	if (!blasBarriers.empty())
	{
		nri::BarrierDesc blasBarrierDesc = {};
		blasBarrierDesc.buffers = blasBarriers.data();
		blasBarrierDesc.bufferNum = (uint32_t)blasBarriers.size();
		mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, blasBarrierDesc);
	}

	std::vector<nri::TopLevelInstance> instances;
	std::vector<SceneInstanceData> sceneInstances;
	BuildStaticMapInstances(staticScene, staticResources.chunkAtlas, instances, sceneInstances);
	staticResources.sceneInstances = sceneInstances;
	staticResources.accelerationBuildSerial = staticScene.buildSerial;
	return
		BuildTopLevelAccelerationStructure(
			instances,
			SceneDataBufferMask_Static,
			staticResources.topLevelAS,
			staticResources.tlasInstanceBuffer,
			staticResources.topLevelScratchBuffer,
			&staticResources.vertexBuffer,
			&staticResources.indexBuffer,
			&staticResources.tlasInstanceCount,
			updateLiveState);
}

void NRIRenderer::BuildStaticMapInstances(std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances, const std::vector<uint8_t>* replacedChunkMask) const
{
	if (!mResidentMapChunkRegistry.valid ||
		mResidentMapChunkRegistry.buildSerial != mStaticMapScene.buildSerial ||
		mResidentMapChunkRegistry.entries.empty())
	{
		if (mStaticMapChunkAtlas.valid &&
			mStaticMapChunkAtlas.buildSerial == mStaticMapScene.buildSerial &&
			mStaticMapChunkAtlas.chunks.size() == mStaticMapScene.chunks.size())
		{
			BuildStaticMapInstances(mStaticMapScene, mStaticMapChunkAtlas, outTlasInstances, outSceneInstances, replacedChunkMask);
			return;
		}

		BuildStaticMapInstances(mStaticMapScene, outTlasInstances, outSceneInstances, replacedChunkMask);
		return;
	}

	outTlasInstances.clear();
	outSceneInstances.clear();
	outTlasInstances.reserve(mResidentMapChunkRegistry.activeChunkCount);
	outSceneInstances.reserve(mResidentMapChunkRegistry.activeChunkCount);

	for (const auto& entry : mResidentMapChunkRegistry.entries)
	{
		if (!entry.valid ||
			!entry.active ||
			!entry.mappedInStaticScene ||
			entry.staticSceneChunkListIndex >= mStaticMapScene.chunks.size())
		{
			continue;
		}

		if (replacedChunkMask != nullptr &&
			entry.chunkIndex < replacedChunkMask->size() &&
			(*replacedChunkMask)[entry.chunkIndex] != 0)
		{
			continue;
		}

		const auto& chunk = mStaticMapScene.chunks[entry.staticSceneChunkListIndex];
		if (chunk.accelerationStructure.accelerationStructure == nullptr)
		{
			continue;
		}

		nri::TopLevelInstance instance = {};
		instance.transform[0][0] = 1.0f;
		instance.transform[1][1] = 1.0f;
		instance.transform[2][2] = 1.0f;
		instance.instanceId = (uint32_t)outSceneInstances.size();
		instance.mask = 0xFF;
		instance.shaderBindingTableLocalOffset = 0;
		instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
		instance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*chunk.accelerationStructure.accelerationStructure);
		outTlasInstances.push_back(instance);
		outSceneInstances.push_back({ entry.primitiveOffset, NRI_SCENE_DATA_SOURCE_STATIC, 0u, 0u });
	}
}

void NRIRenderer::BuildStaticMapInstances(const StaticMapSceneCache& staticScene, std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances, const std::vector<uint8_t>* replacedChunkMask) const
{
	outTlasInstances.clear();
	outSceneInstances.clear();
	outTlasInstances.reserve(staticScene.chunks.size());
	outSceneInstances.reserve(staticScene.chunks.size());

	for (uint32_t chunkIndex = 0; chunkIndex < (uint32_t)staticScene.chunks.size(); ++chunkIndex)
	{
		const auto& chunk = staticScene.chunks[chunkIndex];
		if (replacedChunkMask != nullptr &&
			chunk.chunkIndex < replacedChunkMask->size() &&
			(*replacedChunkMask)[chunk.chunkIndex] != 0)
		{
			continue;
		}

		if (chunk.accelerationStructure.accelerationStructure == nullptr)
		{
			continue;
		}

		nri::TopLevelInstance instance = {};
		instance.transform[0][0] = 1.0f;
		instance.transform[1][1] = 1.0f;
		instance.transform[2][2] = 1.0f;
		instance.instanceId = (uint32_t)outSceneInstances.size();
		instance.mask = 0xFF;
		instance.shaderBindingTableLocalOffset = 0;
		instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
		instance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*chunk.accelerationStructure.accelerationStructure);
		outTlasInstances.push_back(instance);
		outSceneInstances.push_back({ chunk.primitiveOffset, NRI_SCENE_DATA_SOURCE_STATIC, 0u, 0u });
	}
}

void NRIRenderer::BuildStaticMapInstances(const StaticMapSceneCache& staticScene, const StaticMapChunkAtlas& atlas, std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances, const std::vector<uint8_t>* replacedChunkMask) const
{
	if (!atlas.valid || atlas.chunks.size() != staticScene.chunks.size())
	{
		BuildStaticMapInstances(staticScene, outTlasInstances, outSceneInstances, replacedChunkMask);
		return;
	}

	outTlasInstances.clear();
	outSceneInstances.clear();
	outTlasInstances.reserve(staticScene.chunks.size());
	outSceneInstances.reserve(staticScene.chunks.size());

	for (uint32_t chunkListIndex = 0; chunkListIndex < staticScene.chunks.size(); ++chunkListIndex)
	{
		const auto& chunk = staticScene.chunks[chunkListIndex];
		const auto& atlasChunk = atlas.chunks[chunkListIndex];
		if (replacedChunkMask != nullptr &&
			chunk.chunkIndex < replacedChunkMask->size() &&
			(*replacedChunkMask)[chunk.chunkIndex] != 0)
		{
			continue;
		}

		if (chunk.accelerationStructure.accelerationStructure == nullptr)
		{
			continue;
		}

		nri::TopLevelInstance instance = {};
		instance.transform[0][0] = 1.0f;
		instance.transform[1][1] = 1.0f;
		instance.transform[2][2] = 1.0f;
		instance.instanceId = (uint32_t)outSceneInstances.size();
		instance.mask = 0xFF;
		instance.shaderBindingTableLocalOffset = 0;
		instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
		instance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*chunk.accelerationStructure.accelerationStructure);
		outTlasInstances.push_back(instance);
		outSceneInstances.push_back({ atlasChunk.primitiveOffset, NRI_SCENE_DATA_SOURCE_STATIC, 0u, 0u });
	}
}

void NRIRenderer::BuildFilteredStaticMapGeometry(const std::vector<uint8_t>& replacedChunkMask, nri_scene::GeometryData& outGeometry) const
{
	outGeometry = {};
	outGeometry.vertices.reserve(mStaticMapScene.geometry.vertices.size());
	outGeometry.indices.reserve(mStaticMapScene.geometry.indices.size());
	outGeometry.primitives.reserve(mStaticMapScene.geometry.primitives.size());
	outGeometry.primitiveProvenance.reserve(mStaticMapScene.geometry.primitiveProvenance.size());

	for (const auto& chunk : mStaticMapScene.chunks)
	{
		if (chunk.chunkIndex < replacedChunkMask.size() &&
			replacedChunkMask[chunk.chunkIndex] != 0)
		{
			continue;
		}

		AppendGeometryChunk(
			mStaticMapScene.geometry,
			chunk.vertexOffset,
			chunk.vertexCount,
			chunk.indexOffset,
			chunk.indexCount,
			chunk.primitiveOffset,
			chunk.primitiveCount,
			outGeometry);
	}
}

bool NRIRenderer::BuildRuntimeDebugSphereOverlay(nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials)
{
	outGeometry = {};
	outMaterials = {};
	mLastPerfShellTraceStats.runtimeDebugSphereCount = (uint32_t)mRuntimeDebugSpheres.size();
	mLastPerfShellTraceStats.runtimeDebugSphereLongitudeSegments = GetRuntimeDebugSphereLongitudeSegments();
	mLastPerfShellTraceStats.runtimeDebugSphereLatitudeSegments = GetRuntimeDebugSphereLatitudeSegments();
	mLastPerfShellTraceStats.runtimeDebugSpherePrimitiveCount = 0;
	mLastPerfShellTraceStats.runtimeDebugSphereMaterialCount = 0;

	if (mRuntimeDebugSpheres.empty())
	{
		return false;
	}

	size_t totalVertexCount = 0;
	size_t totalIndexCount = 0;
	size_t totalPrimitiveCount = 0;
	size_t totalProvenanceCount = 0;
	size_t totalMaterialCount = 0;
	size_t totalLightMetadataCount = 0;
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeDebugSphereGeoMs);
		for (RuntimeDebugSphere& sphere : mRuntimeDebugSpheres)
		{
			if (!EnsureRuntimeDebugSphereCache(sphere))
			{
				continue;
			}

			totalVertexCount += sphere.geometry.vertices.size();
			totalIndexCount += sphere.geometry.indices.size();
			totalPrimitiveCount += sphere.geometry.primitives.size();
			totalProvenanceCount += sphere.geometry.primitiveProvenance.size();
			totalMaterialCount += sphere.materialBridge.materials.size();
			totalLightMetadataCount += sphere.materialBridge.lightMetadata.size();
		}
	}

	outGeometry.vertices.reserve(totalVertexCount);
	outGeometry.indices.reserve(totalIndexCount);
	outGeometry.primitives.reserve(totalPrimitiveCount);
	outGeometry.primitiveProvenance.reserve(totalProvenanceCount);
	outMaterials.materials.reserve(totalMaterialCount);
	outMaterials.lightMetadata.reserve(totalLightMetadataCount);

	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeDebugSphereMaterialMs);
		for (RuntimeDebugSphere& sphere : mRuntimeDebugSpheres)
		{
			if (!sphere.cacheValid)
			{
				continue;
			}

			AppendGeometry(sphere.geometry, (uint32_t)outMaterials.materials.size(), outGeometry);
			AppendMaterialBridge(sphere.materialBridge, outMaterials);
		}
	}

	mLastPerfShellTraceStats.runtimeDebugSpherePrimitiveCount = (uint32_t)outGeometry.primitives.size();
	mLastPerfShellTraceStats.runtimeDebugSphereMaterialCount = (uint32_t)outMaterials.materials.size();

	return !outGeometry.primitives.empty() && !outMaterials.materials.empty();
}

bool NRIRenderer::EnsureRuntimeDebugSphereCache(RuntimeDebugSphere& sphere)
{
	const uint32_t longitudeSegments = GetRuntimeDebugSphereLongitudeSegments();
	const uint32_t latitudeSegments = GetRuntimeDebugSphereLatitudeSegments();
	if (sphere.cacheValid &&
		sphere.cachedLongitudeSegments == longitudeSegments &&
		sphere.cachedLatitudeSegments == latitudeSegments &&
		!sphere.geometry.primitives.empty() &&
		!sphere.materialBridge.materials.empty())
	{
		return true;
	}

	nri_scene::SceneView sphereView = {};
	sphere.geometry = {};
	sphere.materialBridge = {};
	sphereView.opaqueFlats.reserve(1u);
	sphereView.stats.totalDrawItems = 1;
	sphereView.stats.flatDrawItems = 1;
	sphereView.stats.materialRefs = 1;
	sphereView.stats.triangleEstimate = (unsigned int)GetRuntimeDebugSphereTriangleCount();
	AppendRuntimeDebugSphereToSceneView(sphere, sphereView);
	nri_scene::BuildGeometry(sphereView, sphere.geometry);
	nri_scene::BuildMaterials(sphereView, sphere.materialBridge);

	const size_t materialCount = sphere.materialBridge.materials.size();
	if (sphere.geometry.primitives.empty() || materialCount == 0)
	{
		sphere.cacheValid = false;
		sphere.cachedLongitudeSegments = 0;
		sphere.cachedLatitudeSegments = 0;
		return false;
	}

	for (size_t i = 0; i < materialCount; ++i)
	{
		nri_scene::MaterialData& material = sphere.materialBridge.materials[i];
		material.lightLevel = 1.0f;
		material.alpha = 1.0f;
		material.metalnessHint = sphere.metalness;
		material.roughnessHint = sphere.roughness;
		material.materialClass = 0;

		if (i < sphere.materialBridge.lightMetadata.size())
		{
			nri_scene::MaterialLightingMetadata& metadata = sphere.materialBridge.lightMetadata[i];
			metadata.texture = nullptr;
			metadata.textureId = 0;
			metadata.materialFlags = material.flags;
			metadata.materialClass = material.materialClass;
			metadata.alpha = material.alpha;
			metadata.lightLevel = material.lightLevel;
			metadata.averageColor[0] = 1.0f;
			metadata.averageColor[1] = 1.0f;
			metadata.averageColor[2] = 1.0f;

			uint32_t diameterBits = 0;
			uint32_t metalnessBits = 0;
			uint32_t roughnessBits = 0;
			std::memcpy(&diameterBits, &sphere.diameter, sizeof(diameterBits));
			std::memcpy(&metalnessBits, &sphere.metalness, sizeof(metalnessBits));
			std::memcpy(&roughnessBits, &sphere.roughness, sizeof(roughnessBits));
			metadata.materialKey = HashCombine64(metadata.materialKey, sphere.id);
			metadata.materialKey = HashCombine64(metadata.materialKey, ((uint64_t)diameterBits << 32u) | (uint64_t)metalnessBits);
			metadata.materialKey = HashCombine64(metadata.materialKey, (uint64_t)roughnessBits);
		}
	}

	sphere.cachedLongitudeSegments = longitudeSegments;
	sphere.cachedLatitudeSegments = latitudeSegments;
	sphere.cacheValid = true;
	return true;
}

void NRIRenderer::AppendRuntimeDebugSphereToSceneView(const RuntimeDebugSphere& sphere, nri_scene::SceneView& sceneView) const
{
	const uint32_t longitudeSegments = GetRuntimeDebugSphereLongitudeSegments();
	const uint32_t latitudeSegments = GetRuntimeDebugSphereLatitudeSegments();
	constexpr float Pi = 3.14159265358979323846f;
	auto makeVertex = [Pi](const RuntimeDebugSphere& sphere, float u, float v) -> nri_scene::CapturedVertex
	{
		const float theta = u * 2.0f * Pi;
		const float phi = v * Pi;
		const float radius = sphere.diameter * 0.5f;
		const float sinPhi = sinf(phi);
		nri_scene::CapturedVertex vertex = {};
		vertex.position[0] = sphere.center[0] + radius * sinPhi * cosf(theta);
		vertex.position[1] = sphere.center[1] + radius * cosf(phi);
		vertex.position[2] = sphere.center[2] + radius * sinPhi * sinf(theta);
		vertex.prevPosition[0] = vertex.position[0];
		vertex.prevPosition[1] = vertex.position[1];
		vertex.prevPosition[2] = vertex.position[2];
		vertex.uv[0] = u;
		vertex.uv[1] = v;
		return vertex;
	};
	auto appendTriangle = [](nri_scene::SurfaceRef& surface, const RuntimeDebugSphere& sphere, const nri_scene::CapturedVertex& a, const nri_scene::CapturedVertex& b, const nri_scene::CapturedVertex& c)
	{
		nri_scene::CapturedVertex v0 = a;
		nri_scene::CapturedVertex v1 = b;
		nri_scene::CapturedVertex v2 = c;

		const float abx = v1.position[0] - v0.position[0];
		const float aby = v1.position[1] - v0.position[1];
		const float abz = v1.position[2] - v0.position[2];
		const float acx = v2.position[0] - v0.position[0];
		const float acy = v2.position[1] - v0.position[1];
		const float acz = v2.position[2] - v0.position[2];
		const float nx = aby * acz - abz * acy;
		const float ny = abz * acx - abx * acz;
		const float nz = abx * acy - aby * acx;
		const float centroidX = (v0.position[0] + v1.position[0] + v2.position[0]) / 3.0f;
		const float centroidY = (v0.position[1] + v1.position[1] + v2.position[1]) / 3.0f;
		const float centroidZ = (v0.position[2] + v1.position[2] + v2.position[2]) / 3.0f;
		const float radialX = centroidX - sphere.center[0];
		const float radialY = centroidY - sphere.center[1];
		const float radialZ = centroidZ - sphere.center[2];
		if (nx * radialX + ny * radialY + nz * radialZ < 0.0f)
		{
			std::swap(v1, v2);
		}

		surface.vertices.push_back(v0);
		surface.vertices.push_back(v1);
		surface.vertices.push_back(v2);
	};

	nri_scene::SurfaceRef surface = {};
	surface.material.texture = nullptr;
	surface.material.palette = 0;
	surface.material.shade = 0;
	surface.material.alpha = 1.0f;
	surface.material.flags = nri_scene::MaterialFlag_None;
	surface.provenance.sourceType = nri_scene::SurfaceSourceType::DebugSphere;

	for (uint32_t lat = 0; lat < latitudeSegments; ++lat)
	{
		const float v0 = (float)lat / (float)latitudeSegments;
		const float v1 = (float)(lat + 1u) / (float)latitudeSegments;
		for (uint32_t lon = 0; lon < longitudeSegments; ++lon)
		{
			const float u0 = (float)lon / (float)longitudeSegments;
			const float u1 = (float)(lon + 1u) / (float)longitudeSegments;
			const auto p00 = makeVertex(sphere, u0, v0);
			const auto p01 = makeVertex(sphere, u1, v0);
			const auto p10 = makeVertex(sphere, u0, v1);
			const auto p11 = makeVertex(sphere, u1, v1);

			if (lat == 0u)
			{
				appendTriangle(surface, sphere, p00, p10, p11);
			}
			else if (lat + 1u == latitudeSegments)
			{
				appendTriangle(surface, sphere, p00, p10, p01);
			}
			else
			{
				appendTriangle(surface, sphere, p00, p10, p11);
				appendTriangle(surface, sphere, p00, p11, p01);
			}
		}
	}

	sceneView.opaqueFlats.push_back(std::move(surface));
}

bool NRIRenderer::BuildRuntimeMapMutationOverlay(nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials)
{
	outGeometry = {};
	outMaterials = {};
	mRuntimeMapLastFrame = {};
	bool startupMaterialOnlyMutationDetected = false;
	uint32_t stableRetireEligibleChunkCount = 0;
	uint32_t maxStableMutationFrames = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationDirtyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationRebuiltChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationHeldChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationReplacedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshAnimatedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReplacementDeltaChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshHardwareCanvasChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralReplacementViewChangedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralStaticAnimatedModeFlipChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralExcludeStaticFlipChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralForcedTopologyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralInvalidChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationHardwareCanvasChunkCount = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaReasonMaskOr = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReasonMaskOr = 0;
	mLastPerfShellTraceStats.runtimeMutationActiveChunkCount = 0;
	mLastPerfShellTraceStats.runtimeMutationValidChunkCount = 0;
	mLastPerfShellTraceStats.runtimeMutationExcludedStaticChunkCount = 0;
	mLastPerfShellTraceStats.runtimeMutationCachedSurfaceCount = 0;
	mLastPerfShellTraceStats.runtimeMutationCachedTriangleCount = 0;
	mLastPerfShellTraceStats.runtimeMutationCachedMaterialCount = 0;
	mLastPerfShellTraceStats.runtimeMutationPrimitiveCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialCount = 0;

	if (!mStaticMapScene.valid ||
		mRuntimeMapMutations.chunks.size() != mMapWorld.chunks.size() ||
		mRuntimeMapMutations.replacedChunkMask.size() != mMapWorld.chunks.size())
	{
		return false;
	}

	std::fill(mRuntimeMapMutations.replacedChunkMask.begin(), mRuntimeMapMutations.replacedChunkMask.end(), 0u);
	const auto isVisibleSuppressedStaticAnimatedChunk = [&](uint32_t mapChunkIndex) -> bool
	{
		if (!IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunkIndex))
		{
			return false;
		}

		for (const auto& staticChunk : mStaticMapScene.chunks)
		{
			if (staticChunk.chunkIndex != mapChunkIndex)
			{
				continue;
			}

			return
				staticChunk.hasAnimatedTextureCandidates &&
				staticChunk.animatedRefreshSuppressed;
		}

		return false;
	};

	for (size_t chunkIndex = 0; chunkIndex < mMapWorld.chunks.size(); ++chunkIndex)
	{
		const auto& mapChunk = mMapWorld.chunks[chunkIndex];
		auto& replacement = mRuntimeMapMutations.chunks[chunkIndex];
		const uint64_t cachedSignature = replacement.liveSignature;
		const uint32_t previousReasonMask = replacement.reasonMask;
		nri_scene::PTMapChunkMutationAnalysis analysis = {};
		const bool analyzed = [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationAnalyzeMs);
			return nri_scene::AnalyzeMapChunkMutation(mapChunk, replacement.baseline, analysis);
		}();
		if (!analyzed)
		{
			replacement.active = false;
			replacement.reasonMask = nri_scene::PTMapChunkMutationReason_None;
			replacement.sectionDirtyCount = 0;
			replacement.stableMutationFrameCount = 0;
			replacement.sectorDirty = false;
			replacement.dragged = false;
			replacement.blindSpot = false;
			replacement.excludeStaticChunk = false;
			replacement.staticAnimatedReplacement = false;
			replacement.animationOnlyRefreshed = false;
			replacement.animatedMaterialSignature = 0;
			replacement.lightIdentityOverrides.Clear();
			replacement.sceneView = {};
			TraceRuntimeMapMutationChunk(mapChunk, replacement);
			continue;
		}

		replacement.liveSignature = analysis.signature;
		replacement.reasonMask = analysis.reasonMask;
		replacement.sectionDirtyCount = analysis.sectionDirtyCount;
		replacement.sectorDirty = analysis.sectorDirty;
		replacement.dragged = analysis.dragged;
		replacement.staticAnimatedReplacement = false;
		replacement.blindSpot = analysis.reasonMask != nri_scene::PTMapChunkMutationReason_None && !analysis.signatureChanged;
		// Section dirty alone is too broad for PT runtime replacement because
		// the raster path can mark transient warped sections dirty during draw
		// prep without producing a stable gameplay map mutation. Keep explicit
		// forced invalidation for sector-dirty, but do not let the sticky
		// dragged-sector ownership bit force perpetual rebuilds once PT already
		// has a valid replacement baseline for the chunk.
		const bool forceTopologyInvalidation =
			(analysis.reasonMask & nri_scene::PTMapChunkMutationReason_SectorDirty) != 0;
		const uint32_t materialOnlyReasonMask =
			nri_scene::PTMapChunkMutationReason_SectorMaterial |
			nri_scene::PTMapChunkMutationReason_WallMaterial;
		if (mAllowStartupMutationRebaseline &&
			mFrameIndex <= mStartupMutationRebaselineDeadlineFrame &&
			(analysis.reasonMask & materialOnlyReasonMask) != 0 &&
			(analysis.reasonMask & ~materialOnlyReasonMask) == 0)
		{
			startupMaterialOnlyMutationDetected = true;
		}

		if ((analysis.reasonMask & nri_scene::PTMapChunkMutationReason_SectorGeometry) != 0)
		{
			mRuntimeMapLastFrame.sectorGeometryChunkCount++;
		}
		if ((analysis.reasonMask & nri_scene::PTMapChunkMutationReason_SectorMaterial) != 0)
		{
			mRuntimeMapLastFrame.sectorMaterialChunkCount++;
		}
		if ((analysis.reasonMask & nri_scene::PTMapChunkMutationReason_WallGeometry) != 0)
		{
			mRuntimeMapLastFrame.wallGeometryChunkCount++;
		}
		if ((analysis.reasonMask & nri_scene::PTMapChunkMutationReason_WallMaterial) != 0)
		{
			mRuntimeMapLastFrame.wallMaterialChunkCount++;
		}
		if ((analysis.reasonMask & nri_scene::PTMapChunkMutationReason_SectorDirty) != 0)
		{
			mRuntimeMapLastFrame.sectorDirtyChunkCount++;
		}
		if ((analysis.reasonMask & nri_scene::PTMapChunkMutationReason_SectionDirty) != 0)
		{
			mRuntimeMapLastFrame.sectionDirtyChunkCount++;
		}
		if ((analysis.reasonMask & nri_scene::PTMapChunkMutationReason_Dragged) != 0)
		{
			mRuntimeMapLastFrame.draggedChunkCount++;
		}

		const bool useStaticAnimatedReplacement =
			analysis.reasonMask == nri_scene::PTMapChunkMutationReason_None &&
			isVisibleSuppressedStaticAnimatedChunk(mapChunk.chunkIndex);
		if (analysis.reasonMask == nri_scene::PTMapChunkMutationReason_None && !useStaticAnimatedReplacement)
		{
			replacement.active = false;
			replacement.excludeStaticChunk = false;
			replacement.staticAnimatedReplacement = false;
			replacement.animationOnlyRefreshed = false;
			replacement.animatedMaterialSignature = 0;
			replacement.stableMutationFrameCount = 0;
			replacement.lightIdentityOverrides.Clear();
			replacement.sceneView = {};
			TraceRuntimeMapMutationChunk(mapChunk, replacement);
			continue;
		}

		if (!useStaticAnimatedReplacement)
		{
			mRuntimeMapLastFrame.dirtyChunkCount++;
			mLastPerfShellTraceStats.runtimeMutationDirtyChunks++;
			if (replacement.blindSpot)
			{
				mRuntimeMapLastFrame.blindSpotChunkCount++;
			}
		}

		const bool materialOnlyReplacement = IsMaterialOnlyChunkReplacement(analysis.reasonMask);
		nri_scene::PTMapChunkMutationAnalysis replacementDelta = {};
		const bool analyzedReplacementDelta =
			replacement.valid &&
			nri_scene::AnalyzeMapChunkMutation(mapChunk, replacement.replacementBaseline, replacementDelta);
		const uint32_t structuralReasonMask =
			nri_scene::PTMapChunkMutationReason_SectorGeometry |
			nri_scene::PTMapChunkMutationReason_WallGeometry |
			nri_scene::PTMapChunkMutationReason_SectorDirty |
			nri_scene::PTMapChunkMutationReason_SectionDirty;
		const uint32_t replacementViewReasonMask =
			nri_scene::PTMapChunkMutationReason_SectorGeometry |
			nri_scene::PTMapChunkMutationReason_SectorMaterial |
			nri_scene::PTMapChunkMutationReason_WallGeometry |
			nri_scene::PTMapChunkMutationReason_WallMaterial |
			nri_scene::PTMapChunkMutationReason_SectorDirty |
			nri_scene::PTMapChunkMutationReason_SectionDirty |
			nri_scene::PTMapChunkMutationReason_Dragged;
		const uint32_t replacementRefreshReasonMask =
			replacementDelta.reasonMask & ~nri_scene::PTMapChunkMutationReason_Dragged;
		const bool replacementViewChanged =
			(previousReasonMask & replacementViewReasonMask) !=
			(analysis.reasonMask & replacementViewReasonMask);
		replacement.animationOnlyRefreshed = false;
		nri_scene::PTMapWorld liveWorld = {};
		nri_scene::SceneView liveChunkView;
		nri_scene::PTMapWorldStats liveStats = {};
		bool havePreparedLiveChunkView = false;
		const auto prepareLiveChunkView = [&]() -> bool
		{
			if (havePreparedLiveChunkView)
			{
				return true;
			}

			if (!nri_scene::BuildLiveMapChunkWorld(mapChunk, liveWorld, &liveStats))
			{
				return false;
			}

			nri_scene::BuildMapChunkSceneView(liveWorld, liveWorld.chunks[0], liveChunkView);
			const bool exclusiveMaterialOnlyReplacement =
				materialOnlyReplacement &&
				RequiresExclusiveMaterialOnlyChunkReplacement(analysis.reasonMask);
			if (replacement.blindSpot && replacement.dragged)
			{
				NudgeBlindSpotReplacementFlats(liveChunkView);
			}
			if (materialOnlyReplacement && !exclusiveMaterialOnlyReplacement)
			{
				FilterMaterialOnlyReplacementSceneView(liveChunkView, analysis.reasonMask);
			}

			havePreparedLiveChunkView = true;
			return true;
		};
		const auto rebuildReplacementFromPreparedLiveChunk = [&](bool countAsStructuralRebuild) -> bool
		{
			const bool exclusiveMaterialOnlyReplacement =
				materialOnlyReplacement &&
				RequiresExclusiveMaterialOnlyChunkReplacement(analysis.reasonMask);
			const bool excludeStaticChunk =
				useStaticAnimatedReplacement ||
				!materialOnlyReplacement ||
				exclusiveMaterialOnlyReplacement;
			BuildRuntimeMutationLightIdentityOverrides(
				mMapWorld,
				mapChunk,
				liveWorld,
				liveWorld.chunks[0],
				replacement.lightIdentityOverrides);

			nri_scene::GeometryData liveGeometry;
			nri_scene::MaterialBridgeData liveMaterials;
			{
				Clocker clock(NriPTGeometryBuild);
				nri_scene::BuildGeometry(liveChunkView, liveGeometry);
				AssignGeometryPortalIndices(mMapWorld, liveGeometry);
			}
			{
				Clocker clock(NriPTMaterialBuild);
				BuildMaterialsWithActorOverrides(liveChunkView, liveMaterials, "runtime_mutation_chunk");
			}
			nri_scene::PTMapChunkMutationBaseline liveBaseline;
			if (!nri_scene::CaptureMapChunkMutationBaseline(mapChunk, liveBaseline))
			{
				return false;
			}

			replacement.sceneView = liveChunkView;
			replacement.geometry = std::move(liveGeometry);
			replacement.materialBridge = std::move(liveMaterials);
			replacement.replacementBaseline = std::move(liveBaseline);
			replacement.surfaceCount = CountSceneViewSurfaces(replacement.sceneView);
			replacement.triangleCount = (uint32_t)replacement.geometry.primitives.size();
			replacement.animatedMaterialSignature = ComputeAnimatedMaterialSignature(replacement.sceneView);
			replacement.valid = true;
			replacement.active = true;
			replacement.excludeStaticChunk = excludeStaticChunk;
			replacement.staticAnimatedReplacement = useStaticAnimatedReplacement;
			if (countAsStructuralRebuild)
			{
				mRuntimeMapLastFrame.rebuiltChunkCount++;
				mLastPerfShellTraceStats.runtimeMutationRebuiltChunks++;
			}
			return true;
		};
		const auto refreshReplacementMaterialsFromPreparedLiveChunk = [&]() -> bool
		{
			const bool exclusiveMaterialOnlyReplacement =
				materialOnlyReplacement &&
				RequiresExclusiveMaterialOnlyChunkReplacement(analysis.reasonMask);
			const bool excludeStaticChunk =
				useStaticAnimatedReplacement ||
				!materialOnlyReplacement ||
				exclusiveMaterialOnlyReplacement;
			nri_scene::MaterialBridgeData liveMaterials;
			{
				Clocker clock(NriPTMaterialBuild);
				BuildMaterialsWithActorOverrides(liveChunkView, liveMaterials, "runtime_mutation_chunk");
			}
			nri_scene::PTMapChunkMutationBaseline liveBaseline;
			if (!nri_scene::CaptureMapChunkMutationBaseline(mapChunk, liveBaseline))
			{
				return false;
			}

			replacement.sceneView = liveChunkView;
			replacement.materialBridge = std::move(liveMaterials);
			replacement.replacementBaseline = std::move(liveBaseline);
			replacement.surfaceCount = CountSceneViewSurfaces(replacement.sceneView);
			replacement.animatedMaterialSignature = ComputeAnimatedMaterialSignature(replacement.sceneView);
			replacement.valid = true;
			replacement.active = true;
			replacement.excludeStaticChunk = excludeStaticChunk;
			replacement.staticAnimatedReplacement = useStaticAnimatedReplacement;
			return true;
		};
		const bool exclusiveMaterialOnlyReplacement =
			materialOnlyReplacement &&
			RequiresExclusiveMaterialOnlyChunkReplacement(analysis.reasonMask);
		const bool desiredExcludeStaticChunk =
			useStaticAnimatedReplacement ||
			!materialOnlyReplacement ||
			exclusiveMaterialOnlyReplacement;
		const bool structuralInvalid = !replacement.valid;
		const bool structuralReplacementDelta =
			!analyzedReplacementDelta ||
			(replacementDelta.reasonMask & structuralReasonMask) != 0;
		const bool structuralStaticAnimatedModeFlip =
			replacement.staticAnimatedReplacement != useStaticAnimatedReplacement;
		const bool structuralExcludeStaticFlip =
			replacement.excludeStaticChunk != desiredExcludeStaticChunk;
		const bool needsStructuralRebuild =
			structuralInvalid ||
			forceTopologyInvalidation ||
			structuralReplacementDelta ||
			replacementViewChanged ||
			structuralExcludeStaticFlip ||
			structuralStaticAnimatedModeFlip;

		if (needsStructuralRebuild)
		{
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildChunks++;
			if (structuralReplacementDelta)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaChunks++;
				if (analyzedReplacementDelta)
				{
					mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaReasonMaskOr |= replacementDelta.reasonMask;
				}
			}
			if (replacementViewChanged)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralReplacementViewChangedChunks++;
			}
			if (structuralStaticAnimatedModeFlip)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralStaticAnimatedModeFlipChunks++;
			}
			if (structuralExcludeStaticFlip)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralExcludeStaticFlipChunks++;
			}
			if (forceTopologyInvalidation)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralForcedTopologyChunks++;
			}
			if (structuralInvalid)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralInvalidChunks++;
			}
			const bool builtChunk = [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationRebuildMs);
				ScopedPtPerfTimer structuralPerfTimer(mLastPerfShellTraceStats.runtimeMutationStructuralRebuildMs);
				if (!prepareLiveChunkView())
				{
					return false;
				}
				return rebuildReplacementFromPreparedLiveChunk(true);
			}();
			if (!builtChunk && replacement.valid)
			{
				replacement.active = true;
				mRuntimeMapLastFrame.heldChunkCount++;
				mLastPerfShellTraceStats.runtimeMutationHeldChunks++;
			}
			else if (!builtChunk)
			{
				replacement.active = false;
				replacement.excludeStaticChunk = false;
				replacement.staticAnimatedReplacement = false;
				replacement.animationOnlyRefreshed = false;
				replacement.animatedMaterialSignature = 0;
				replacement.stableMutationFrameCount = 0;
				replacement.lightIdentityOverrides.Clear();
				replacement.sceneView = {};
				TraceRuntimeMapMutationChunk(mapChunk, replacement);
				continue;
			}
		}
		else
		{
			replacement.active = true;
			bool activeHardwareCanvasChunk = false;
			// Build tile animation can change the resolved PT texture binding
			// without mutating the authored wall/sector fields tracked above.
			const bool refreshedAnimatedChunk = [&]()
			{
				const bool forceReplacementMaterialRefresh =
					analyzedReplacementDelta &&
					replacementRefreshReasonMask != nri_scene::PTMapChunkMutationReason_None;
				if (!prepareLiveChunkView())
				{
					return false;
				}

				activeHardwareCanvasChunk = SceneViewUsesHardwareCanvasTexture(liveChunkView);
				const bool forceHardwareCanvasRefresh =
					activeHardwareCanvasChunk &&
					IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunk.chunkIndex);
				const uint64_t liveAnimatedMaterialSignature =
					ComputeAnimatedMaterialSignature(liveChunkView);
				if (!forceReplacementMaterialRefresh &&
					!forceHardwareCanvasRefresh &&
					liveAnimatedMaterialSignature == replacement.animatedMaterialSignature)
				{
					return true;
				}

				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationRebuildMs);
				ScopedPtPerfTimer materialRefreshPerfTimer(mLastPerfShellTraceStats.runtimeMutationMaterialRefreshMs);
				if (!refreshReplacementMaterialsFromPreparedLiveChunk())
				{
					return false;
				}

				mLastPerfShellTraceStats.runtimeMutationMaterialRefreshChunks++;
				if (forceReplacementMaterialRefresh)
				{
					mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReplacementDeltaChunks++;
					mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReasonMaskOr |= replacementRefreshReasonMask;
				}
				if (forceHardwareCanvasRefresh)
				{
					mLastPerfShellTraceStats.runtimeMutationMaterialRefreshHardwareCanvasChunks++;
				}
				if (!forceReplacementMaterialRefresh && !forceHardwareCanvasRefresh)
				{
					mLastPerfShellTraceStats.runtimeMutationMaterialRefreshAnimatedChunks++;
					replacement.animationOnlyRefreshed = true;
					mRuntimeMapLastFrame.animatedRefreshChunkCount++;
				}
				return true;
			}();
			if (!refreshedAnimatedChunk)
			{
				replacement.active = true;
				mRuntimeMapLastFrame.heldChunkCount++;
				mLastPerfShellTraceStats.runtimeMutationHeldChunks++;
			}
		}

		if (replacement.active &&
			SceneViewUsesHardwareCanvasTexture(replacement.sceneView))
		{
			mLastPerfShellTraceStats.runtimeMutationHardwareCanvasChunkCount++;
		}

		const uint32_t transientReasonMask =
			nri_scene::PTMapChunkMutationReason_SectorDirty |
			nri_scene::PTMapChunkMutationReason_SectionDirty |
			nri_scene::PTMapChunkMutationReason_Dragged;
		const bool stableRetireCandidate =
			replacement.active &&
			replacement.valid &&
			!replacement.staticAnimatedReplacement &&
			!replacement.animationOnlyRefreshed &&
			!needsStructuralRebuild &&
			analysis.reasonMask != nri_scene::PTMapChunkMutationReason_None &&
			(analysis.reasonMask & transientReasonMask) == 0 &&
			cachedSignature == replacement.liveSignature &&
			previousReasonMask == replacement.reasonMask;
		if (stableRetireCandidate)
		{
			replacement.stableMutationFrameCount++;
		}
		else
		{
			replacement.stableMutationFrameCount = 0;
		}
		maxStableMutationFrames = std::max(maxStableMutationFrames, replacement.stableMutationFrameCount);
		if (replacement.stableMutationFrameCount >= NRI_RUNTIME_MUTATION_REBASELINE_STABLE_FRAMES)
		{
			stableRetireEligibleChunkCount++;
		}

		mRuntimeMapMutations.replacedChunkMask[chunkIndex] = replacement.excludeStaticChunk ? 1u : 0u;
		mRuntimeMapLastFrame.replacedChunkCount++;
		mLastPerfShellTraceStats.runtimeMutationReplacedChunks++;
		mRuntimeMapLastFrame.replacementSurfaceCount += replacement.surfaceCount;
		mRuntimeMapLastFrame.replacementTriangleCount += replacement.triangleCount;
		mRuntimeMapLastFrame.materialCount += (uint32_t)replacement.materialBridge.materials.size();

		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationAppendMs);
			if (!replacement.geometry.primitives.empty())
			{
				AppendGeometry(replacement.geometry, (uint32_t)outMaterials.materials.size(), outGeometry);
			}
			AppendMaterialBridge(replacement.materialBridge, outMaterials);
		}
		TraceRuntimeMapMutationChunk(mapChunk, replacement);
	}

	mRuntimeMapLastFrame.active = mRuntimeMapLastFrame.replacedChunkCount > 0;
	if (startupMaterialOnlyMutationDetected && !mPendingStartupMutationRebaseline)
	{
		mPendingStartupMutationRebaseline = true;
		Printf("NRI PT startup mutation rebaseline queued: level=%s frame=%u dirty_material_chunks=%u replaced_chunks=%u\n",
			currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)",
			mFrameIndex,
			mRuntimeMapLastFrame.sectorMaterialChunkCount + mRuntimeMapLastFrame.wallMaterialChunkCount,
			mRuntimeMapLastFrame.replacedChunkCount);
	}
	if (mAllowStartupMutationRebaseline && mFrameIndex > mStartupMutationRebaselineDeadlineFrame)
	{
		mAllowStartupMutationRebaseline = false;
	}
	const RuntimeMutationCacheStats cacheStats = GatherRuntimeMutationCacheStats();
	mLastPerfShellTraceStats.runtimeMutationActiveChunkCount = cacheStats.activeChunkCount;
	mLastPerfShellTraceStats.runtimeMutationValidChunkCount = cacheStats.validChunkCount;
	mLastPerfShellTraceStats.runtimeMutationExcludedStaticChunkCount = cacheStats.excludedStaticChunkCount;
	mLastPerfShellTraceStats.runtimeMutationCachedSurfaceCount = cacheStats.cachedSurfaceCount;
	mLastPerfShellTraceStats.runtimeMutationCachedTriangleCount = cacheStats.cachedTriangleCount;
	mLastPerfShellTraceStats.runtimeMutationCachedMaterialCount = cacheStats.cachedMaterialCount;
	mRuntimeMutationCacheHighWaterStats.activeChunkCount = std::max(mRuntimeMutationCacheHighWaterStats.activeChunkCount, cacheStats.activeChunkCount);
	mRuntimeMutationCacheHighWaterStats.validChunkCount = std::max(mRuntimeMutationCacheHighWaterStats.validChunkCount, cacheStats.validChunkCount);
	mRuntimeMutationCacheHighWaterStats.excludedStaticChunkCount = std::max(mRuntimeMutationCacheHighWaterStats.excludedStaticChunkCount, cacheStats.excludedStaticChunkCount);
	mRuntimeMutationCacheHighWaterStats.cachedSurfaceCount = std::max(mRuntimeMutationCacheHighWaterStats.cachedSurfaceCount, cacheStats.cachedSurfaceCount);
	mRuntimeMutationCacheHighWaterStats.cachedTriangleCount = std::max(mRuntimeMutationCacheHighWaterStats.cachedTriangleCount, cacheStats.cachedTriangleCount);
	mRuntimeMutationCacheHighWaterStats.cachedMaterialCount = std::max(mRuntimeMutationCacheHighWaterStats.cachedMaterialCount, cacheStats.cachedMaterialCount);
	if (!mPendingRuntimeMutationRebaseline &&
		!mPendingStartupMutationRebaseline &&
		!mAllowStartupMutationRebaseline &&
		cacheStats.activeChunkCount >= NRI_RUNTIME_MUTATION_REBASELINE_MIN_ACTIVE_CHUNKS &&
		stableRetireEligibleChunkCount >= NRI_RUNTIME_MUTATION_REBASELINE_MIN_STABLE_CHUNKS &&
		(!mHasRuntimeMutationRebaseline ||
			mFrameIndex >= mLastRuntimeMutationRebaselineFrame + NRI_RUNTIME_MUTATION_REBASELINE_COOLDOWN_FRAMES))
	{
		mPendingRuntimeMutationRebaseline = true;
		mPendingRuntimeMutationRebaselineActiveChunkCount = cacheStats.activeChunkCount;
		mPendingRuntimeMutationRebaselineStableChunkCount = stableRetireEligibleChunkCount;
		if (ShouldTraceRuntimeMutationRebaseline())
		{
			Printf("NRI PT runtime mutation rebaseline trigger: level=%s frame=%u active_chunks=%u stable_chunks=%u max_stable_frames=%u cached_surfaces=%u cached_tris=%u cached_materials=%u\n",
				currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)",
				mFrameIndex,
				cacheStats.activeChunkCount,
				stableRetireEligibleChunkCount,
				maxStableMutationFrames,
				cacheStats.cachedSurfaceCount,
				cacheStats.cachedTriangleCount,
				cacheStats.cachedMaterialCount);
		}
	}
	mLastPerfShellTraceStats.runtimeMutationPrimitiveCount = (uint32_t)outGeometry.primitives.size();
	mLastPerfShellTraceStats.runtimeMutationMaterialCount = (uint32_t)outMaterials.materials.size();
	return mRuntimeMapLastFrame.active;
}

bool NRIRenderer::BuildRuntimeSpaceLinkOverlay(HWDrawInfo& di, nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials)
{
	outGeometry = {};
	outMaterials = {};
	mRuntimeSpaceLinkLastFrame = {};
	mRuntimeSpaceLinkLastFrame.orphanLocalSpaceCount = CountOrphanLocalSpaces(mMapWorld);
	mRuntimeSpaceLinkLastFrame.unresolvedRuntimePortalCount = mMapWorld.stats.runtimePortalCount;

	const auto deactivateRuntimeLinkHistory = [&]()
	{
		if (!mRuntimeChunkTranslationHistory.empty())
		{
			mRuntimeSpaceLinkLastFrame.topologyChanged = true;
			RequestHistoryReset("runtime-link-deactivated", false, true);
		}
	};

	if (!mMapWorld.valid)
	{
		deactivateRuntimeLinkHistory();
		return false;
	}

	int effectSectorIndex = -1;
	if (di.Viewpoint.SectNums != nullptr)
	{
		if (di.Viewpoint.SectCount > 0)
		{
			effectSectorIndex = di.Viewpoint.SectNums[0];
			mRuntimeSpaceLinkLastFrame.viewRootSectorCount = (uint32_t)di.Viewpoint.SectCount;
		}
	}
	else
	{
		effectSectorIndex = di.Viewpoint.SectCount;
		mRuntimeSpaceLinkLastFrame.viewRootSectorCount = 1;
	}

	if (effectSectorIndex < 0 || (unsigned)effectSectorIndex >= sector.Size())
	{
		deactivateRuntimeLinkHistory();
		return false;
	}

	const BitArray& visibleSectors = di.GetVisibleSectors();
	for (unsigned sectorIndex = 0; sectorIndex < visibleSectors.Size(); ++sectorIndex)
	{
		if (visibleSectors.Check(sectorIndex))
		{
			mRuntimeSpaceLinkLastFrame.visibleSectorCount++;
		}
	}

	mRuntimeSpaceLinkLastFrame.candidateSectorIndex = effectSectorIndex;
	mRuntimeSpaceLinkLastFrame.candidateSectorLotag = sector[(unsigned)effectSectorIndex].lotag;
	mRuntimeSpaceLinkLastFrame.queryAttempted = true;

	GeoEffect effect = {};
	int providerSectorIndex = -1;
	if (gi != nullptr && gi->GetGeoEffect(&effect, &sector[effectSectorIndex]))
	{
		providerSectorIndex = effectSectorIndex;
	}
	else
	{
		mRuntimeSpaceLinkLastFrame.queryRejected = true;
	}

	const auto getLocalSpaceIndex = [&](int sectorIndex) -> uint32_t
	{
		if (sectorIndex < 0 || (unsigned)sectorIndex >= mMapWorld.chunks.size())
		{
			return UINT32_MAX;
		}

		return mMapWorld.chunks[(unsigned)sectorIndex].localSpaceIndex;
	};

	const uint32_t candidateLocalSpaceIndex = getLocalSpaceIndex(effectSectorIndex);
	const auto sectorMatchesVisibleSet = [&](int sectorIndex) -> bool
	{
		return sectorIndex >= 0 &&
			(unsigned)sectorIndex < visibleSectors.Size() &&
			visibleSectors.Check((unsigned)sectorIndex);
	};
	auto groupMatchesCandidate = [&](const GeoEffect& candidateEffect, int groupIndex) -> bool
	{
		auto matchesSector = [&](sectortype* sect) -> bool
		{
			if (sect == nullptr)
			{
				return false;
			}

			const int sectorIndex = sector.IndexOf(sect);
			if (sectorIndex < 0)
			{
				return false;
			}

			if (sectorIndex == effectSectorIndex)
			{
				return true;
			}

			if (candidateLocalSpaceIndex == UINT32_MAX)
			{
				return false;
			}

			return getLocalSpaceIndex(sectorIndex) == candidateLocalSpaceIndex;
		};

		return
			matchesSector(candidateEffect.geosector != nullptr ? candidateEffect.geosector[groupIndex] : nullptr) ||
			matchesSector(candidateEffect.geosectorwarp != nullptr ? candidateEffect.geosectorwarp[groupIndex] : nullptr) ||
			matchesSector(candidateEffect.geosectorwarp2 != nullptr ? candidateEffect.geosectorwarp2[groupIndex] : nullptr);
	};
	auto groupMatchesVisibleSectors = [&](const GeoEffect& candidateEffect, int groupIndex) -> bool
	{
		auto matchesVisible = [&](sectortype* sect) -> bool
		{
			if (sect == nullptr)
			{
				return false;
			}

			const int sectorIndex = sector.IndexOf(sect);
			return sectorMatchesVisibleSet(sectorIndex);
		};

		return
			matchesVisible(candidateEffect.geosector != nullptr ? candidateEffect.geosector[groupIndex] : nullptr) ||
			matchesVisible(candidateEffect.geosectorwarp != nullptr ? candidateEffect.geosectorwarp[groupIndex] : nullptr) ||
			matchesVisible(candidateEffect.geosectorwarp2 != nullptr ? candidateEffect.geosectorwarp2[groupIndex] : nullptr);
	};

	if (gi != nullptr)
	{
		for (unsigned sectorIndex = 0; sectorIndex < sector.Size(); ++sectorIndex)
		{
			if (sector[sectorIndex].lotag != 848)
			{
				continue;
			}

			mRuntimeSpaceLinkLastFrame.providerSectorCount++;

			GeoEffect candidateEffect = {};
			if (!gi->GetGeoEffect(&candidateEffect, &sector[sectorIndex]) || candidateEffect.geocnt <= 0)
			{
				continue;
			}

			mRuntimeSpaceLinkLastFrame.geoProviderCount++;
			mRuntimeSpaceLinkLastFrame.providerGroupCount += (uint32_t)candidateEffect.geocnt;

			bool matched = false;
			bool visibleMatched = false;
			for (int i = 0; i < candidateEffect.geocnt; ++i)
			{
				if (groupMatchesCandidate(candidateEffect, i))
				{
					matched = true;
				}
				if (groupMatchesVisibleSectors(candidateEffect, i))
				{
					visibleMatched = true;
				}
			}

			if (matched)
			{
				mRuntimeSpaceLinkLastFrame.localSpaceMatchedProviderCount++;
			}
			if (visibleMatched)
			{
				mRuntimeSpaceLinkLastFrame.visibleMatchedProviderCount++;
			}

			if (providerSectorIndex >= 0 || !matched)
			{
				continue;
			}

			effect = candidateEffect;
			providerSectorIndex = (int)sectorIndex;
			break;
		}
	}

	if (providerSectorIndex < 0)
	{
		mRuntimeSpaceLinkLastFrame.queryRejected = true;
		deactivateRuntimeLinkHistory();
		return false;
	}

	mRuntimeSpaceLinkLastFrame.sourceSectorIndex = providerSectorIndex;
	mRuntimeSpaceLinkLastFrame.reportedGeoCount = effect.geocnt;
	if (effect.geocnt <= 0)
	{
		mRuntimeSpaceLinkLastFrame.queryRejected = true;
		deactivateRuntimeLinkHistory();
		return false;
	}

	struct RuntimeGeoLink
	{
		uint32_t chunkIndex = UINT32_MAX;
		float dx = 0.0f;
		float dz = 0.0f;
		float prevDx = 0.0f;
		float prevDz = 0.0f;
	};

	std::vector<RuntimeGeoLink> links;
	links.reserve((size_t)effect.geocnt * 2u);

	auto appendLink = [&](sectortype* warpedSector, double mapDx, double mapDy)
	{
		if (warpedSector == nullptr)
		{
			return;
		}

		const int32_t sectorIndex = sector.IndexOf(warpedSector);
		if (sectorIndex < 0 || (unsigned)sectorIndex >= mMapWorld.chunks.size())
		{
			return;
		}

		RuntimeGeoLink link = {};
		link.chunkIndex = (uint32_t)sectorIndex;
		link.dx = (float)mapDx;
		link.dz = (float)-mapDy;
		for (const RuntimeGeoLink& existing : links)
		{
			if (existing.chunkIndex == link.chunkIndex &&
				fabs(existing.dx - link.dx) < 0.001f &&
				fabs(existing.dz - link.dz) < 0.001f)
			{
				return;
			}
		}

		links.push_back(link);
	};

	for (int i = 0; i < effect.geocnt; ++i)
	{
		if (!groupMatchesCandidate(effect, i))
		{
			continue;
		}

		appendLink(effect.geosectorwarp != nullptr ? effect.geosectorwarp[i] : nullptr,
			effect.geox != nullptr ? effect.geox[i] : 0.0,
			effect.geoy != nullptr ? effect.geoy[i] : 0.0);
		appendLink(effect.geosectorwarp2 != nullptr ? effect.geosectorwarp2[i] : nullptr,
			effect.geox2 != nullptr ? effect.geox2[i] : 0.0,
			effect.geoy2 != nullptr ? effect.geoy2[i] : 0.0);
	}

	if (links.empty())
	{
		deactivateRuntimeLinkHistory();
		return false;
	}

	const auto findPreviousTranslation = [&](uint32_t chunkIndex, float& outPrevDx, float& outPrevDz) -> bool
	{
		for (const RuntimeChunkTranslationState& previous : mRuntimeChunkTranslationHistory)
		{
			if (previous.chunkIndex == chunkIndex)
			{
				outPrevDx = previous.dx;
				outPrevDz = previous.dz;
				return true;
			}
		}

		return false;
	};

	for (RuntimeGeoLink& link : links)
	{
		findPreviousTranslation(link.chunkIndex, link.prevDx, link.prevDz);
	}

	const auto runtimeLinkTopologyChanged = [&]() -> bool
	{
		if (links.size() != mRuntimeChunkTranslationHistory.size())
		{
			return true;
		}

		for (const RuntimeGeoLink& link : links)
		{
			bool found = false;
			for (const RuntimeChunkTranslationState& previous : mRuntimeChunkTranslationHistory)
			{
				if (previous.chunkIndex == link.chunkIndex)
				{
					found = true;
					break;
				}
			}

			if (!found)
			{
				return true;
			}
		}

		return false;
	};

	mRuntimeSpaceLinkLastFrame.geoEffectActive = true;
	mRuntimeSpaceLinkLastFrame.linkCount = (uint32_t)links.size();
	mRuntimeSpaceLinkLastFrame.topologyChanged = runtimeLinkTopologyChanged();
	if (mRuntimeSpaceLinkLastFrame.topologyChanged)
	{
		RequestHistoryReset("runtime-link-topology");
	}

	std::vector<RuntimeChunkTranslationState> nextRuntimeChunkTranslationHistory;
	nextRuntimeChunkTranslationHistory.reserve(links.size());

	for (const RuntimeGeoLink& link : links)
	{
		if (link.chunkIndex >= mMapWorld.chunks.size())
		{
			continue;
		}

		nri_scene::SceneView liveChunkView;
		nri_scene::PTMapWorldStats liveStats = {};
		if (!nri_scene::BuildLiveMapChunkSceneView(mMapWorld.chunks[link.chunkIndex], liveChunkView, &liveStats))
		{
			continue;
		}

		nri_scene::GeometryData chunkGeometry;
		nri_scene::MaterialBridgeData chunkMaterials;
		{
			Clocker clock(NriPTGeometryBuild);
			nri_scene::BuildGeometry(liveChunkView, chunkGeometry);
			AssignGeometryPortalIndices(mMapWorld, chunkGeometry);
			TranslateGeometry(chunkGeometry, link.dx, 0.0f, link.dz, link.prevDx, 0.0f, link.prevDz);
		}
		{
			Clocker clock(NriPTMaterialBuild);
			BuildMaterialsWithActorOverrides(liveChunkView, chunkMaterials, "runtime_space_link_chunk");
		}

		if (!chunkGeometry.primitives.empty())
		{
			AppendGeometry(chunkGeometry, (uint32_t)outMaterials.materials.size(), outGeometry);
		}
		AppendMaterialBridge(chunkMaterials, outMaterials);

		mRuntimeSpaceLinkLastFrame.translatedChunkCount++;
		mRuntimeSpaceLinkLastFrame.surfaceCount += liveStats.surfaceCount;
		mRuntimeSpaceLinkLastFrame.triangleCount += liveStats.triangleCount;
		mRuntimeSpaceLinkLastFrame.materialCount += (uint32_t)chunkMaterials.materials.size();
		nextRuntimeChunkTranslationHistory.push_back({ link.chunkIndex, link.dx, link.dz });
	}

	mRuntimeChunkTranslationHistory = std::move(nextRuntimeChunkTranslationHistory);
	mRuntimeSpaceLinkLastFrame.active = !outGeometry.primitives.empty();
	return mRuntimeSpaceLinkLastFrame.active;
}

bool NRIRenderer::RestoreStaticTopLevelScene()
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.restoreStaticSceneMs);
	std::vector<nri::TopLevelInstance> instances;
	std::vector<SceneInstanceData> sceneInstances;
	BuildStaticMapInstances(instances, sceneInstances);
	return
		BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static) &&
		UpdateSceneDataSet(
			mStaticVertexBuffer,
			mStaticIndexBuffer,
			mStaticPrimitiveBuffer,
			mStaticMaterialBuffer,
			mVertexBuffer,
			mIndexBuffer,
			mPrimitiveBuffer,
			mMaterialBuffer,
			sceneInstances,
			(uint32_t)mStaticMapScene.geometry.primitives.size(),
			0u,
			(uint32_t)mStaticMapScene.gpuMaterials.size(),
			0u,
			"restore_static_scene");
}

bool NRIRenderer::RefreshResidentStaticSceneDataSet()
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.residentLightRefreshMs);
	std::vector<SceneInstanceData> sceneInstances;
	std::vector<nri::TopLevelInstance> ignoredInstances;
	BuildStaticMapInstances(ignoredInstances, sceneInstances);
	return UpdateSceneDataSet(
		mStaticVertexBuffer,
		mStaticIndexBuffer,
		mStaticPrimitiveBuffer,
		mStaticMaterialBuffer,
		mVertexBuffer,
		mIndexBuffer,
		mPrimitiveBuffer,
		mMaterialBuffer,
		sceneInstances,
		(uint32_t)mStaticMapScene.geometry.primitives.size(),
		0u,
		(uint32_t)mStaticMapScene.gpuMaterials.size(),
		0u,
		"refresh_resident_static_scene");
}

bool NRIRenderer::BuildDynamicAccelerationStructure(const nri_scene::GeometryData& geometry)
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.dynamicAsMs);
	mLastPerfShellTraceStats.dynamicAsPrimitiveCount = (uint32_t)geometry.primitives.size();
	mLastPerfShellTraceStats.dynamicAsVertexCount = (uint32_t)geometry.vertices.size();
	mLastPerfShellTraceStats.dynamicAsIndexCount = (uint32_t)geometry.indices.size();
	if (geometry.primitives.empty() || geometry.vertices.empty() || geometry.indices.empty())
	{
		return false;
	}

	nri::BottomLevelGeometryDesc dynamicGeometryDesc = {};
	dynamicGeometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
	dynamicGeometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
	dynamicGeometryDesc.triangles.vertexBuffer = mVertexBuffer.buffer;
	dynamicGeometryDesc.triangles.vertexOffset = 0;
	dynamicGeometryDesc.triangles.vertexNum = (uint32_t)geometry.vertices.size();
	dynamicGeometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
	dynamicGeometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
	dynamicGeometryDesc.triangles.indexBuffer = mIndexBuffer.buffer;
	dynamicGeometryDesc.triangles.indexOffset = 0;
	dynamicGeometryDesc.triangles.indexNum = (uint32_t)geometry.indices.size();
	dynamicGeometryDesc.triangles.indexType = nri::IndexType::UINT32;

	DestroyAccelerationStructureResource(mDynamicBottomLevelAS);

	nri::AccelerationStructureDesc blasDesc = {};
	blasDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
	blasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_BUILD;
	blasDesc.geometryOrInstanceNum = 1;
	blasDesc.geometries = &dynamicGeometryDesc;
	const bool createdAs = [&]()
	{
		ScopedPtPerfTimer phaseTimer(mLastPerfShellTraceStats.dynamicAsCreateMs);
		return mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, blasDesc, mDynamicBottomLevelAS.accelerationStructure) == nri::Result::SUCCESS;
	}();
	if (!createdAs)
	{
		return false;
	}

	{
		nri::MemoryDesc memoryDesc = {};
		mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(*mDynamicBottomLevelAS.accelerationStructure, nri::MemoryLocation::DEVICE, memoryDesc);
		mDynamicBottomLevelAS.memorySize = memoryDesc.size;
		mDynamicBottomLevelAS.memoryLocation = nri::MemoryLocation::DEVICE;
	}

	uint64_t requiredScratchSize = 0;
	{
		ScopedPtPerfTimer phaseTimer(mLastPerfShellTraceStats.dynamicAsScratchMs);
		requiredScratchSize = mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*mDynamicBottomLevelAS.accelerationStructure);
	}
	if (mScratchBuffer.buffer == nullptr || mScratchBuffer.size < requiredScratchSize)
	{
		DestroyBufferResource(mScratchBuffer);
		{
			ScopedPtPerfTimer phaseTimer(mLastPerfShellTraceStats.dynamicAsScratchMs);
			if (!CreateBufferWithoutView(mScratchBuffer, requiredScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
			{
				return false;
			}
		}
	}

	nri::BuildBottomLevelAccelerationStructureDesc dynamicBuild = {};
	dynamicBuild.dst = mDynamicBottomLevelAS.accelerationStructure;
	dynamicBuild.geometries = &dynamicGeometryDesc;
	dynamicBuild.geometryNum = 1;
	dynamicBuild.scratchBuffer = mScratchBuffer.buffer;
	dynamicBuild.scratchOffset = 0;
	{
		ScopedPtPerfTimer phaseTimer(mLastPerfShellTraceStats.dynamicAsBuildMs);
		mFrameBuffer->mRayTracing.CmdBuildBottomLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &dynamicBuild, 1);
	}

	nri::BufferBarrierDesc barrier = {};
	barrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*mDynamicBottomLevelAS.accelerationStructure);
	barrier.before = NRIAccelerationStructureWriteAccess();
	barrier.after = NRIAccelerationStructureReadAccess();

	nri::BarrierDesc barrierDesc = {};
	barrierDesc.buffers = &barrier;
	barrierDesc.bufferNum = 1;
	{
		ScopedPtPerfTimer phaseTimer(mLastPerfShellTraceStats.dynamicAsBarrierMs);
		mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrierDesc);
	}
	mBuiltDynamicSceneASLastFrame = true;
	mDynamicSceneLastFrame.asBuildCount++;
	return true;
}

bool NRIRenderer::BuildEmissiveTopLevelAccelerationStructure()
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.emissiveTlasMs);
	mEmissiveTlasInstanceCount = 0;
	mEmissiveTlasStaticInstanceCount = 0;
	mEmissiveTlasDynamicInstanceCount = 0;

	if (!nri_ptemissivetlas ||
		mBoundEmissivePrimitiveRecords.empty() ||
		mBoundSceneInstances.empty())
	{
		DestroyBufferResource(mEmissiveTlasInstanceBuffer);
		DestroyAccelerationStructureResource(mEmissiveTopLevelAS);
		mEmissiveTlasInstancePayloadCacheValid = false;
		mEmissiveTlasInstancePayloadHash = 0;
		return true;
	}

	std::unordered_map<uint32_t, uint32_t> staticSceneInstanceByPrimitiveOffset;
	staticSceneInstanceByPrimitiveOffset.reserve(mBoundSceneInstances.size());
	uint32_t dynamicSceneInstanceIndex = UINT32_MAX;
	for (uint32_t sceneInstanceIndex = 0; sceneInstanceIndex < (uint32_t)mBoundSceneInstances.size(); ++sceneInstanceIndex)
	{
		const SceneInstanceData& sceneInstance = mBoundSceneInstances[sceneInstanceIndex];
		if (sceneInstance.dataSource == NRI_SCENE_DATA_SOURCE_STATIC)
		{
			staticSceneInstanceByPrimitiveOffset.emplace(sceneInstance.primitiveOffset, sceneInstanceIndex);
		}
		else if (sceneInstance.dataSource == NRI_SCENE_DATA_SOURCE_DYNAMIC && dynamicSceneInstanceIndex == UINT32_MAX)
		{
			dynamicSceneInstanceIndex = sceneInstanceIndex;
		}
	}

	std::vector<uint8_t> emissiveStaticChunks(mStaticMapScene.chunks.size(), 0u);
	bool includeDynamicInstance = false;
	const auto findStaticChunkIndexForPrimitive = [&](uint32_t primitiveIndex) -> int32_t
	{
		uint32_t low = 0;
		uint32_t high = (uint32_t)mStaticMapScene.chunks.size();
		while (low < high)
		{
			const uint32_t mid = (low + high) >> 1u;
			const auto& chunk = mStaticMapScene.chunks[mid];
			const uint32_t chunkBegin = chunk.primitiveOffset;
			const uint32_t chunkEnd = chunkBegin + chunk.primitiveCount;
			if (primitiveIndex < chunkBegin)
			{
				high = mid;
			}
			else if (primitiveIndex >= chunkEnd)
			{
				low = mid + 1u;
			}
			else
			{
				return (int32_t)mid;
			}
		}

		return -1;
	};

	for (const EmissivePrimitiveDebugRecord& record : mBoundEmissivePrimitiveRecords)
	{
		if (record.dataSource == NRI_SCENE_DATA_SOURCE_STATIC)
		{
			const int32_t chunkIndex = findStaticChunkIndexForPrimitive(record.primitiveIndex);
			if (chunkIndex >= 0)
			{
				emissiveStaticChunks[(size_t)chunkIndex] = 1u;
			}
		}
		else if (record.dataSource == NRI_SCENE_DATA_SOURCE_DYNAMIC &&
			dynamicSceneInstanceIndex != UINT32_MAX &&
			mDynamicBottomLevelAS.accelerationStructure != nullptr)
		{
			includeDynamicInstance = true;
		}
	}

	std::vector<nri::TopLevelInstance> instances;
	instances.reserve(mStaticMapScene.chunks.size() + (includeDynamicInstance ? 1u : 0u));
	for (size_t chunkIndex = 0; chunkIndex < mStaticMapScene.chunks.size(); ++chunkIndex)
	{
		if (emissiveStaticChunks[chunkIndex] == 0u)
		{
			continue;
		}

		const auto& chunk = mStaticMapScene.chunks[chunkIndex];
		if (chunk.accelerationStructure.accelerationStructure == nullptr)
		{
			continue;
		}

		const auto sceneInstanceIt = staticSceneInstanceByPrimitiveOffset.find(chunk.primitiveOffset);
		if (sceneInstanceIt == staticSceneInstanceByPrimitiveOffset.end())
		{
			continue;
		}

		nri::TopLevelInstance instance = {};
		instance.transform[0][0] = 1.0f;
		instance.transform[1][1] = 1.0f;
		instance.transform[2][2] = 1.0f;
		instance.instanceId = sceneInstanceIt->second;
		instance.mask = 0xFF;
		instance.shaderBindingTableLocalOffset = 0;
		instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
		instance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*chunk.accelerationStructure.accelerationStructure);
		instances.push_back(instance);
		mEmissiveTlasStaticInstanceCount++;
	}

	if (includeDynamicInstance)
	{
		nri::TopLevelInstance instance = {};
		instance.transform[0][0] = 1.0f;
		instance.transform[1][1] = 1.0f;
		instance.transform[2][2] = 1.0f;
		instance.instanceId = dynamicSceneInstanceIndex;
		instance.mask = 0xFF;
		instance.shaderBindingTableLocalOffset = 0;
		instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
		instance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*mDynamicBottomLevelAS.accelerationStructure);
		instances.push_back(instance);
		mEmissiveTlasDynamicInstanceCount = 1;
	}

	if (instances.empty())
	{
		DestroyBufferResource(mEmissiveTlasInstanceBuffer);
		DestroyAccelerationStructureResource(mEmissiveTopLevelAS);
		mEmissiveTlasInstancePayloadCacheValid = false;
		mEmissiveTlasInstancePayloadHash = 0;
		return true;
	}

	const uint64_t payloadHash = BuildEmissiveTlasInstancePayloadHash(instances);
	if (mEmissiveTlasInstancePayloadCacheValid &&
		mEmissiveTlasInstancePayloadHash == payloadHash &&
		mEmissiveTlasInstanceBuffer.buffer != nullptr &&
		mEmissiveTopLevelAS.accelerationStructure != nullptr)
	{
		mEmissiveTlasInstanceCount = (uint32_t)instances.size();
		return true;
	}

	DestroyAccelerationStructureResource(mEmissiveTopLevelAS);
	if (!EnsureStructuredBuffer(
		mEmissiveTlasInstanceBuffer,
		mEmissiveTlasInstanceBufferStats,
		instances.data(),
		instances.size() * sizeof(nri::TopLevelInstance),
		sizeof(nri::TopLevelInstance),
		nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
		NRIAccelerationStructureBuildInputAccess()))
	{
		return false;
	}

	nri::AccelerationStructureDesc tlasDesc = {};
	tlasDesc.type = nri::AccelerationStructureType::TOP_LEVEL;
	tlasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
	tlasDesc.geometryOrInstanceNum = (uint32_t)instances.size();
	if (mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, tlasDesc, mEmissiveTopLevelAS.accelerationStructure) != nri::Result::SUCCESS)
	{
		return false;
	}

	{
		nri::MemoryDesc memoryDesc = {};
		mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(*mEmissiveTopLevelAS.accelerationStructure, nri::MemoryLocation::DEVICE, memoryDesc);
		mEmissiveTopLevelAS.memorySize = memoryDesc.size;
		mEmissiveTopLevelAS.memoryLocation = nri::MemoryLocation::DEVICE;
	}

	const uint64_t requiredScratchSize = mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*mEmissiveTopLevelAS.accelerationStructure);
	if (mTopLevelScratchBuffer.buffer == nullptr || mTopLevelScratchBuffer.size < requiredScratchSize)
	{
		DestroyBufferResource(mTopLevelScratchBuffer);
		if (!CreateBufferWithoutView(mTopLevelScratchBuffer, requiredScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
		{
			return false;
		}
	}

	if (mFrameBuffer->mRayTracing.CreateAccelerationStructureDescriptor(*mEmissiveTopLevelAS.accelerationStructure, mEmissiveTopLevelAS.descriptor) != nri::Result::SUCCESS)
	{
		return false;
	}

	nri::BuildTopLevelAccelerationStructureDesc tlasBuild = {};
	tlasBuild.dst = mEmissiveTopLevelAS.accelerationStructure;
	tlasBuild.instanceNum = (uint32_t)instances.size();
	tlasBuild.instanceBuffer = mEmissiveTlasInstanceBuffer.buffer;
	tlasBuild.instanceOffset = 0;
	tlasBuild.scratchBuffer = mTopLevelScratchBuffer.buffer;
	tlasBuild.scratchOffset = 0;
	mFrameBuffer->mRayTracing.CmdBuildTopLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &tlasBuild, 1);

	nri::BufferBarrierDesc tlasBarrier = {};
	tlasBarrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*mEmissiveTopLevelAS.accelerationStructure);
	tlasBarrier.before = NRIAccelerationStructureWriteAccess();
	tlasBarrier.after = NRIComputeAccelerationStructureReadAccess();

	nri::BarrierDesc barrierDesc = {};
	barrierDesc.buffers = &tlasBarrier;
	barrierDesc.bufferNum = 1;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrierDesc);

	mEmissiveTlasInstanceCount = (uint32_t)instances.size();
	mEmissiveTlasBuildCount++;
	mEmissiveTlasInstancePayloadCacheValid = true;
	mEmissiveTlasInstancePayloadHash = payloadHash;
	return true;
}

bool NRIRenderer::BuildTopLevelAccelerationStructure(const std::vector<nri::TopLevelInstance>& instances, uint32_t sceneBufferMask)
{
	return BuildTopLevelAccelerationStructure(
		instances,
		sceneBufferMask,
		mTopLevelAS,
		mTlasInstanceBuffer,
		mTopLevelScratchBuffer,
		&mStaticVertexBuffer,
		&mStaticIndexBuffer,
		&mActiveTlasInstanceCount,
		true);
}

bool NRIRenderer::BuildTopLevelAccelerationStructure(
	const std::vector<nri::TopLevelInstance>& instances,
	uint32_t sceneBufferMask,
	NRIAccelerationStructureResource& topLevelAS,
	NRIBufferResource& tlasInstanceBuffer,
	NRIBufferResource& topLevelScratchBuffer,
	const NRIBufferResource* staticVertexBuffer,
	const NRIBufferResource* staticIndexBuffer,
	uint32_t* outTlasInstanceCount,
	bool updateLiveState)
{
	if (instances.empty())
	{
		return false;
	}

	DestroyAccelerationStructureResource(topLevelAS);

	static SceneBufferDebugStats sTlasInstanceStats = { "TLASInstance" };
	if (!EnsureStructuredBuffer(
		tlasInstanceBuffer,
		sTlasInstanceStats,
		instances.data(),
		instances.size() * sizeof(nri::TopLevelInstance),
		sizeof(nri::TopLevelInstance),
		nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
		NRIAccelerationStructureBuildInputAccess()))
	{
		return false;
	}

	nri::AccelerationStructureDesc tlasDesc = {};
	tlasDesc.type = nri::AccelerationStructureType::TOP_LEVEL;
	tlasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
	tlasDesc.geometryOrInstanceNum = (uint32_t)instances.size();
	if (mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, tlasDesc, topLevelAS.accelerationStructure) != nri::Result::SUCCESS)
	{
		return false;
	}

	{
		nri::MemoryDesc memoryDesc = {};
		mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(*topLevelAS.accelerationStructure, nri::MemoryLocation::DEVICE, memoryDesc);
		topLevelAS.memorySize = memoryDesc.size;
		topLevelAS.memoryLocation = nri::MemoryLocation::DEVICE;
	}

	const uint64_t requiredScratchSize = mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*topLevelAS.accelerationStructure);
	if (topLevelScratchBuffer.buffer == nullptr || topLevelScratchBuffer.size < requiredScratchSize)
	{
		DestroyBufferResource(topLevelScratchBuffer);
		if (!CreateBufferWithoutView(topLevelScratchBuffer, requiredScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
		{
			return false;
		}
	}

	if (mFrameBuffer->mRayTracing.CreateAccelerationStructureDescriptor(*topLevelAS.accelerationStructure, topLevelAS.descriptor) != nri::Result::SUCCESS)
	{
		return false;
	}

	nri::BuildTopLevelAccelerationStructureDesc tlasBuild = {};
	tlasBuild.dst = topLevelAS.accelerationStructure;
	tlasBuild.instanceNum = (uint32_t)instances.size();
	tlasBuild.instanceBuffer = tlasInstanceBuffer.buffer;
	tlasBuild.instanceOffset = 0;
	tlasBuild.scratchBuffer = topLevelScratchBuffer.buffer;
	tlasBuild.scratchOffset = 0;
	mFrameBuffer->mRayTracing.CmdBuildTopLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &tlasBuild, 1);

	nri::BufferBarrierDesc tlasBarrier = {};
	tlasBarrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*topLevelAS.accelerationStructure);
	tlasBarrier.before = NRIAccelerationStructureWriteAccess();
	tlasBarrier.after = NRIComputeAccelerationStructureReadAccess();

	std::vector<nri::BufferBarrierDesc> barriers;
	barriers.reserve(5);
	barriers.push_back(tlasBarrier);
	if ((sceneBufferMask & SceneDataBufferMask_Static) != 0 && staticVertexBuffer != nullptr && staticIndexBuffer != nullptr)
	{
		nri::BufferBarrierDesc vertexBarrier = {};
		vertexBarrier.buffer = staticVertexBuffer->buffer;
		vertexBarrier.before = NRIAccelerationStructureBuildInputAccess();
		vertexBarrier.after = NRIComputeShaderResourceAccess();
		barriers.push_back(vertexBarrier);

		nri::BufferBarrierDesc indexBarrier = {};
		indexBarrier.buffer = staticIndexBuffer->buffer;
		indexBarrier.before = NRIAccelerationStructureBuildInputAccess();
		indexBarrier.after = NRIComputeShaderResourceAccess();
		barriers.push_back(indexBarrier);
	}
	if ((sceneBufferMask & SceneDataBufferMask_Dynamic) != 0)
	{
		nri::BufferBarrierDesc vertexBarrier = {};
		vertexBarrier.buffer = mVertexBuffer.buffer;
		vertexBarrier.before = NRIAccelerationStructureBuildInputAccess();
		vertexBarrier.after = NRIComputeShaderResourceAccess();
		barriers.push_back(vertexBarrier);

		nri::BufferBarrierDesc indexBarrier = {};
		indexBarrier.buffer = mIndexBuffer.buffer;
		indexBarrier.before = NRIAccelerationStructureBuildInputAccess();
		indexBarrier.after = NRIComputeShaderResourceAccess();
		barriers.push_back(indexBarrier);
	}

	nri::BarrierDesc barrierDesc = {};
	barrierDesc.buffers = barriers.data();
	barrierDesc.bufferNum = (uint32_t)barriers.size();
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrierDesc);

	if (outTlasInstanceCount != nullptr)
	{
		*outTlasInstanceCount = (uint32_t)instances.size();
	}

	if (updateLiveState)
	{
		mActiveTlasInstanceCount = (uint32_t)instances.size();
		if ((sceneBufferMask & SceneDataBufferMask_Static) != 0 &&
			(sceneBufferMask & SceneDataBufferMask_Dynamic) == 0)
		{
			mStaticMapScene.tlasInstanceCount = (uint32_t)instances.size();
			mStaticMapScene.accelerationResident = true;
			mBuiltStaticMapSceneASLastFrame = true;
		}
	}
	return true;
}

bool NRIRenderer::DispatchFrameGraph(HWDrawInfo& di, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials, int)
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.frameGraphMs);
	Clocker clock(NriPTFrameGraph);

	static bool sLoggedPhaseBCompositionPath = false;
	static bool sLoggedPhaseGResolvedPresentPath = false;
	static bool sLoggedPhaseFDenoiserPath = false;
	static bool sLoggedPhaseFDenoiserFallback = false;
	static bool sLoggedPhaseFTraceTransparentPath = false;
	static bool sLoggedTraceTransparentProbePath = false;
	static bool sLoggedRawTraceBypass = false;
	static bool sLoggedPhaseHRrInputPath = false;
	const int ptDebugMode = (int)GetEffectivePtDebugMode();
	const NRIPresentRouteInfo presentRoute = ResolvePresentRouteInfo((uint32_t)ptDebugMode, !!nri_ptbootstrap);
	const bool bootstrapRawTracePresent = presentRoute.kind == NRIPresentRouteKind::BootstrapFinal;
	const bool useResolvedPresent = presentRoute.kind == NRIPresentRouteKind::ResolvedBeauty;
	const bool useComposedDebugPresent = presentRoute.kind == NRIPresentRouteKind::ComposedDebug;
	const bool useUpscalerTraceTransparentProbe = presentRoute.kind == NRIPresentRouteKind::UpscalerTraceTransparentProbe;
	const bool useCompositionPath = useResolvedPresent || useComposedDebugPresent || useUpscalerTraceTransparentProbe;
	const bool useValidationPresent = presentRoute.kind == NRIPresentRouteKind::ValidationRaw;
	const bool useDenoisedDebugPresent = presentRoute.kind == NRIPresentRouteKind::DenoisedRaw;
	const bool useShadowDebugPresent = presentRoute.kind == NRIPresentRouteKind::ShadowFinal;
	const bool useFinalDebugPresent = presentRoute.kind == NRIPresentRouteKind::FinalDebug || useShadowDebugPresent;
	const bool rawTraceDirectPresent = presentRoute.kind == NRIPresentRouteKind::RawTraceDebug;
	const bool useSplitShadowDebugProbe = rawTraceDirectPresent && ptDebugMode >= 21 && ptDebugMode <= 22;
	mHistoryInputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPing : FrameTextureSlot::TaaHistoryPong;
	mHistoryOutputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPong : FrameTextureSlot::TaaHistoryPing;
	mUpscaledInputSlot = FrameTextureSlot::PostSharpenOutput;
	mUseUpscaledInFinal = false;
	mUseDenoisedCompositionInputs = false;
	const bool directionalLightShadowEnabled = mDirectionalLightState.enabled && mDirectionalLightState.shadow;
	mUseSplitShadowDenoiser = directionalLightShadowEnabled && (useShadowDebugPresent || useSplitShadowDebugProbe || (useCompositionPath && nri_denoise));

	if (!DispatchTraceOpaque(di, geometry, materials))
	{
		return false;
	}

	if (bootstrapRawTracePresent)
	{
		if (!DispatchFinal())
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	if (useValidationPresent)
	{
		if (!DispatchDenoiser())
		{
			return false;
		}

		if (!DispatchRawPresent(FrameTextureSlot::Validation))
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	if (useDenoisedDebugPresent)
	{
		if (!DispatchDenoiser())
		{
			return false;
		}

		const FrameTextureSlot denoisedSlot = ptDebugMode == 16 ? FrameTextureSlot::DenoisedDiffuse : FrameTextureSlot::DenoisedSpecular;
		if (!DispatchRawPresent(denoisedSlot))
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	if (useShadowDebugPresent)
	{
		if (nri_denoise && !DispatchDenoiser())
		{
			return false;
		}

		mUseUpscaledInFinal = false;
		if (!DispatchFinal())
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	auto dispatchCompositionPath = [&]() -> bool
	{
		const NRIMainUpscalerKind resolvedMainKind = ResolveMainUpscalerKind(false);
		const bool buildRrInput = resolvedMainKind == NRIMainUpscalerKind::DLRR;
		const bool needStandardComposition =
			!buildRrInput || useComposedDebugPresent || useUpscalerTraceTransparentProbe;

		mUseDenoisedCompositionInputs = false;

		if (buildRrInput)
		{
			if (!sLoggedPhaseHRrInputPath)
			{
				Printf("NRI Phase H: DLRR now builds a separate noisy RrInput before NRD and bypasses opaque denoising for the vendor RR branch.\n");
				sLoggedPhaseHRrInputPath = true;
			}

			mUseSplitShadowDenoiser = false;
			if (!DispatchComposition(FrameTextureSlot::RrInput))
			{
				return false;
			}
		}

		if (!needStandardComposition)
		{
			return true;
		}

		if (!buildRrInput && nri_denoise)
		{
			if (!sLoggedPhaseFDenoiserPath)
			{
				Printf("NRI Phase F: the Composition-backed PT paths now route through NRD before Composition when nri_denoise is enabled.\n");
				sLoggedPhaseFDenoiserPath = true;
			}

			if (!DispatchDenoiser())
			{
				if (!sLoggedPhaseFDenoiserFallback)
				{
					Printf(TEXTCOLOR_ORANGE "NRI Phase F: NRD dispatch failed in the composition path; falling back to raw trace inputs for this frame.\n");
					sLoggedPhaseFDenoiserFallback = true;
				}
			}
			else
			{
				mUseDenoisedCompositionInputs = true;
				mUseSplitShadowDenoiser = directionalLightShadowEnabled;
			}
		}

		if (!DispatchComposition(FrameTextureSlot::Composed))
		{
			return false;
		}

		if (!sLoggedPhaseFTraceTransparentPath)
		{
			Printf("NRI Phase F.5: Composition-backed PT paths now pass through placeholder TraceTransparent before output-resolution dispatch.\n");
			sLoggedPhaseFTraceTransparentPath = true;
		}

		if (!DispatchTraceTransparent())
		{
			return false;
		}

		return true;
	};

	if (useResolvedPresent)
	{
		if (!sLoggedPhaseGResolvedPresentPath)
		{
			Printf("NRI Phase G: ptdebug 0 now routes through Composition, placeholder TraceTransparent, DispatchUpscaleChain, and the minimal FinalPresent presenter.\n");
			sLoggedPhaseGResolvedPresentPath = true;
		}

		if (!dispatchCompositionPath())
		{
			return false;
		}

		if (!DispatchUpscaleChain())
		{
			return false;
		}

		const FrameTextureSlot resolvedPresentSlot = mUseUpscaledInFinal ? mUpscaledInputSlot : mHistoryOutputSlot;
		TraceTemporalState("resolved-present", ResolveMainUpscalerKind(false), ResolvePostSharpenKind(false), false, resolvedPresentSlot, mHistoryOutputSlot);
		if (!DispatchFinalPresent(resolvedPresentSlot))
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	if (useComposedDebugPresent)
	{
		if (!sLoggedPhaseBCompositionPath)
		{
			Printf("NRI Phase B: ptdebug 45 now routes through Composition, placeholder TraceTransparent, and the minimal FinalPresent presenter.\n");
			sLoggedPhaseBCompositionPath = true;
		}

		if (!dispatchCompositionPath())
		{
			return false;
		}

		if (!DispatchFinalPresent(FrameTextureSlot::TraceTransparentOutput))
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	if (useUpscalerTraceTransparentProbe)
	{
		if (!sLoggedTraceTransparentProbePath)
		{
			Printf("NRI Phase I instrumentation: ptdebug 34 now exposes TraceTransparentOutput before the upscaler chain.\n");
			sLoggedTraceTransparentProbePath = true;
		}

		if (!dispatchCompositionPath())
		{
			return false;
		}

		if (!DispatchRawPresent(FrameTextureSlot::TraceTransparentOutput))
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	if (useFinalDebugPresent)
	{
		mUseUpscaledInFinal = false;
		if (!DispatchFinal())
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	if (rawTraceDirectPresent)
	{
		if (!sLoggedRawTraceBypass)
		{
			Printf("NRI frame-graph bypass: presenting raw TraceOpaque output through the direct present path for non-composition debug views.\n");
			sLoggedRawTraceBypass = true;
		}

		FrameTextureSlot rawPresentSlot = FrameTextureSlot::UnfilteredDiffuse;
		FrameTextureSlot rawPresentSecondarySlot = FrameTextureSlot::Count;
		FrameTextureSlot rawPresentTertiarySlot = FrameTextureSlot::Count;
		if (ptDebugMode == 11 || ptDebugMode == 12)
		{
			rawPresentSlot = FrameTextureSlot::UnfilteredSpecular;
		}
		else if (ptDebugMode == 18)
		{
			rawPresentSlot = FrameTextureSlot::BaseColorMetalness;
		}
		else if (ptDebugMode == 19)
		{
			rawPresentSlot = FrameTextureSlot::NormalRoughness;
		}
		else if (ptDebugMode == 21 || ptDebugMode == 22)
		{
			rawPresentSlot = FrameTextureSlot::UnfilteredPenumbra;
		}
		else if (ptDebugMode == 24)
		{
			rawPresentSlot = FrameTextureSlot::DirectLighting;
		}
		else if (ptDebugMode == 25)
		{
			rawPresentSlot = FrameTextureSlot::DirectEmission;
		}

		if (ptDebugMode == 12)
		{
			rawPresentSecondarySlot = FrameTextureSlot::ViewZ;
			rawPresentTertiarySlot = FrameTextureSlot::NormalRoughness;
		}

		if (!DispatchRawPresent(rawPresentSlot, rawPresentSecondarySlot, rawPresentTertiarySlot))
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	if (!sLoggedRawTraceBypass)
	{
		Printf("NRI frame-graph bypass: presenting raw TraceOpaque output until composition integration is stabilized.\n");
		sLoggedRawTraceBypass = true;
	}

	mUseUpscaledInFinal = false;
	if (!DispatchFinal())
	{
		return false;
	}

	CopyFinalToActiveTarget();
	return true;
}

bool NRIRenderer::DispatchTraceOpaque(HWDrawInfo&, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials)
{
	Clocker clock(NriPTTraceOpaque);

	if (!UpdateReprojectionBuffer())
	{
		return false;
	}

	NRITraceSceneConstants constants = {};
	const uint32_t bootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	const bool directSceneTrace = (!nri_ptbootstrap && nri_ptdirectscene) || bootstrapMode == 11u || bootstrapMode == 12u;
	const bool useTemporalJitter =
		!nri_ptbootstrap &&
		!mGuiCaptureActive &&
		ShouldUseTemporalJitter(ResolveMainUpscalerKind(false));
	Copy3(mCurrentCameraPos, constants.CameraPos);
	Copy3(mCurrentCameraForward, constants.CameraForward);
	Copy3(mCurrentCameraRight, constants.CameraRight);
	Copy3(mCurrentCameraUp, constants.CameraUp);
	Copy3(mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = mRenderWidth;
	constants.RenderHeight = mRenderHeight;
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.TanHalfFovX = mCurrentTanHalfFovX;
	constants.TanHalfFovY = mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = mPreviousTanHalfFovY;
	constants.SceneInstanceCount = mSceneInstanceBuffer.stride != 0 ? (uint32_t)(mSceneInstanceBuffer.usedSize / mSceneInstanceBuffer.stride) : 0u;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.StaticPrimitiveCount = mBoundStaticPrimitiveCount;
	constants.FrameIndex = mFrameIndex;
	constants.DynamicPrimitiveCount = mBoundDynamicPrimitiveCount;
	constants.Flags =
		(mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(directSceneTrace ? NRI_FLAG_PRESENT_RAW_TRACE : 0u) |
		(mUseSplitShadowDenoiser && !directSceneTrace ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(mDirectionalLightState.enabled && mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u) |
		(nri_ptemissivefastshadow ? NRI_FLAG_FAST_EMISSIVE_SHADOW : 0u) |
		(nri_ptvisiblechunkgate ? NRI_FLAG_GATE_PRIMARY_VISIBLE_CHUNKS : 0u) |
		(useTemporalJitter ? NRI_FLAG_USE_JITTER : 0u);
	constants.StaticMaterialCount = mBoundStaticMaterialCount;
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = mBoundDynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(
		ClampTraceBounceCount((int)nri_ptlightbounces, 4u),
		ClampTraceBounceCount((int)nri_ptmirrorbounces, 8u),
		mDirectionalLightState.color);
	constants.PortalCount = mBoundPortalCount;
	constants.RuntimeLightCount = mBoundRuntimeLightCount;
	constants.PortalDepth = ClampTraceBounceCount((int)nri_ptportaldepth, 8u);
	constants.ReservedTrace0 = (mBoundRuntimeLightTileCountX & 0xffffu) | ((mBoundRuntimeLightTileCountY & 0xffffu) << 16u);
	constants.ReservedTrace1 = PackTraceAux1(
		(uint32_t)GetSelectedNrdDenoiserMode(),
		std::max<uint32_t>(ClampTraceBounceCount((int)nri_ptemissivesamples, 4u), 1u),
		mDirectionalLightState.angularSize);
	Copy3(mSkyColor, constants.SkyColor);
	Copy3(mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(mDirectionalLightState, constants);

	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredSpecular), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DirectLighting), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DirectEmission), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::ViewZ), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::NormalRoughness), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::BaseColorMetalness), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::SrInput), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::RrGuideSpecularHitDistance), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Validation), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::VendorOutput), NRIComputeStorageState());

	const nri::Descriptor* defaultInput = GetFrameTexture(FrameTextureSlot::Composed).shaderView;
	mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	UpdateFrameTextureSet();

	const nri::Descriptor* defaultOutput = GetFrameTexture(FrameTextureSlot::Validation).storageView;
	mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	mOutputDescriptors[0] = GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse).storageView;
	mOutputDescriptors[3] = GetFrameTexture(FrameTextureSlot::Motion).storageView;
	mOutputDescriptors[4] = GetFrameTexture(FrameTextureSlot::ViewZ).storageView;
	mOutputDescriptors[5] = GetFrameTexture(FrameTextureSlot::NormalRoughness).storageView;
	mOutputDescriptors[6] = GetFrameTexture(FrameTextureSlot::BaseColorMetalness).storageView;
	mOutputDescriptors[9] = GetFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo).storageView;
	mOutputDescriptors[10] = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular).storageView;
	mOutputDescriptors[11] = GetFrameTexture(FrameTextureSlot::RrGuideSpecularHitDistance).storageView;
	mOutputDescriptors[12] = GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra).storageView;
	mOutputDescriptors[13] = GetFrameTexture(FrameTextureSlot::DirectLighting).storageView;
	mOutputDescriptors[14] = GetFrameTexture(FrameTextureSlot::DirectEmission).storageView;
	UpdateOutputSet();

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	BindSceneRootDescriptors();
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mSamplerSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 2, GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 3, mFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 4, mOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::TraceOpaque));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mRenderWidth), GetDispatchSize(mRenderHeight), 1 });
	return true;
}

bool NRIRenderer::DispatchDenoiser()
{
	Clocker clock(NriPTDenoiser);
	const uint32_t nrdMaxFrames = ClampNrdHistoryFrameCount((int)nri_nrdmaxframes);

	if (!mNrd.EnsureReady(*mFrameBuffer->mDevice, mRenderWidth, mRenderHeight, 1))
	{
		return false;
	}

	mNrd.NewFrame();

	NRINrdDispatchDesc desc = {};
	desc.commandBuffer = mFrameBuffer->mCommandBuffer;
	desc.motion = &GetFrameTexture(FrameTextureSlot::Motion);
	desc.viewZ = &GetFrameTexture(FrameTextureSlot::ViewZ);
	desc.normalRoughness = &GetFrameTexture(FrameTextureSlot::NormalRoughness);
	desc.baseColorMetalness = &GetFrameTexture(FrameTextureSlot::BaseColorMetalness);
	desc.unfilteredDiffuse = &GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse);
	desc.unfilteredSpecular = &GetFrameTexture(FrameTextureSlot::UnfilteredSpecular);
	desc.unfilteredPenumbra = &GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra);
	desc.diffuse = &GetFrameTexture(FrameTextureSlot::DenoisedDiffuse);
	desc.specular = &GetFrameTexture(FrameTextureSlot::DenoisedSpecular);
	desc.shadow = &GetFrameTexture(FrameTextureSlot::DenoisedShadow);
	desc.validation = &GetFrameTexture(FrameTextureSlot::Validation);
	desc.resourceWidth = mRenderWidth;
	desc.resourceHeight = mRenderHeight;
	desc.frameIndex = mFrameIndex;
	Copy2(mCurrentJitter, desc.cameraJitter);
	Copy2(mPreviousJitter, desc.cameraJitterPrev);
	std::memcpy(desc.viewToClipMatrix, mCurrentViewToClip, sizeof(desc.viewToClipMatrix));
	std::memcpy(desc.viewToClipMatrixPrev, mPreviousViewToClip, sizeof(desc.viewToClipMatrixPrev));
	std::memcpy(desc.worldToViewMatrix, mCurrentWorldToView, sizeof(desc.worldToViewMatrix));
	std::memcpy(desc.worldToViewMatrixPrev, mPreviousWorldToView, sizeof(desc.worldToViewMatrixPrev));
	desc.lightDirection[0] = mDirectionalLightState.direction[0];
	desc.lightDirection[1] = mDirectionalLightState.direction[1];
	desc.lightDirection[2] = mDirectionalLightState.direction[2];
	Normalize3(desc.lightDirection);
	desc.denoiserMode = GetSelectedNrdDenoiserMode();
	desc.maxAccumulatedFrameNum = nrdMaxFrames;
	desc.maxFastAccumulatedFrameNum = ClampNrdFastFrameCount((int)nri_nrdfastframes, nrdMaxFrames);
	desc.maxStabilizedFrameNum = ClampNrdStabilizationFrameCount((int)nri_nrdstabilizationframes, nrdMaxFrames);
	desc.hitDistanceReconstructionMode = GetNrdHitDistanceReconstructionMode();
	desc.fastHistoryClampingSigmaScale = ClampNrdFastHistorySigmaScale((float)nri_nrdfasthistorysigma);
	desc.diffusePrepassBlurRadius = ClampNrdPrepassBlurRadius((float)nri_nrdprepassdiffuse);
	desc.specularPrepassBlurRadius = ClampNrdPrepassBlurRadius((float)nri_nrdprepassspecular);
	desc.minBlurRadius = ClampNrdBlurRadius((float)nri_nrdblurmin);
	desc.maxBlurRadius = std::max(desc.minBlurRadius, ClampNrdBlurRadius((float)nri_nrdblurmax));
	desc.sigmaMaxStabilizedFrameNum = ClampSigmaStabilizationFrameCount((int)nri_nrdsigmastabilization);
	desc.sigmaPlaneDistanceSensitivity = ClampSigmaPlaneDistanceSensitivity((float)nri_nrdsigmaplanedistance);
	desc.resetHistory = mResetHistory;
	desc.enableAntiFirefly = nri_nrdantifirefly;
	desc.enableValidation = nri_validation;
	desc.enableSigmaShadow = mUseSplitShadowDenoiser;
	return mNrd.Denoise(desc);
}

bool NRIRenderer::DispatchComposition(FrameTextureSlot outputSlot)
{
	Clocker clock(NriPTComposition);

	NRITraceSceneConstants constants = {};
	Copy3(mCurrentCameraPos, constants.CameraPos);
	Copy3(mCurrentCameraForward, constants.CameraForward);
	Copy3(mCurrentCameraRight, constants.CameraRight);
	Copy3(mCurrentCameraUp, constants.CameraUp);
	Copy3(mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = mRenderWidth;
	constants.RenderHeight = mRenderHeight;
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.TanHalfFovX = mCurrentTanHalfFovX;
	constants.TanHalfFovY = mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = mPreviousTanHalfFovY;
	constants.FrameIndex = mFrameIndex;
	constants.Flags =
		(mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(mUseSplitShadowDenoiser ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(mDirectionalLightState.enabled && mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u);
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.BootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	constants.BounceCounts = PackTraceBounceCounts(0u, 0u, mDirectionalLightState.color);
	constants.RuntimeLightCount = mBoundRuntimeLightCount;
	constants.ReservedTrace0 = GetNrdInputSplitMode();
	constants.ReservedTrace1 = PackDenoiserAux1((uint32_t)GetSelectedNrdDenoiserMode(), mDirectionalLightState.angularSize);
	Copy3(mSkyColor, constants.SkyColor);
	Copy3(mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(mDirectionalLightState, constants);

	NRITextureResource& diffuse = GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse);
	NRITextureResource& specular = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular);
	NRITextureResource& viewZ = GetFrameTexture(FrameTextureSlot::ViewZ);
	NRITextureResource& normalRoughness = GetFrameTexture(FrameTextureSlot::NormalRoughness);
	NRITextureResource& baseColorMetalness = GetFrameTexture(FrameTextureSlot::BaseColorMetalness);
	NRITextureResource& rawShadow = GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra);
	NRITextureResource& directLighting = GetFrameTexture(FrameTextureSlot::DirectLighting);
	NRITextureResource& directEmission = GetFrameTexture(FrameTextureSlot::DirectEmission);
	const FrameTextureSlot filteredDiffuseSlot = mUseDenoisedCompositionInputs ? FrameTextureSlot::DenoisedDiffuse : FrameTextureSlot::UnfilteredDiffuse;
	const FrameTextureSlot filteredSpecularSlot = mUseDenoisedCompositionInputs ? FrameTextureSlot::DenoisedSpecular : FrameTextureSlot::UnfilteredSpecular;
	const FrameTextureSlot filteredShadowSlot = mUseDenoisedCompositionInputs ? FrameTextureSlot::DenoisedShadow : FrameTextureSlot::UnfilteredPenumbra;
	NRITextureResource& filteredDiffuse = GetFrameTexture(filteredDiffuseSlot);
	NRITextureResource& filteredSpecular = GetFrameTexture(filteredSpecularSlot);
	NRITextureResource& filteredShadow = GetFrameTexture(filteredShadowSlot);
	NRITextureResource& composed = GetFrameTexture(outputSlot);

	mFrameBuffer->TransitionTexture(diffuse, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(specular, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(viewZ, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(normalRoughness, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(baseColorMetalness, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(rawShadow, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(directLighting, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(directEmission, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(filteredDiffuse, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(filteredSpecular, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(filteredShadow, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(composed, NRIComputeStorageState());

	const nri::Descriptor* defaultInput = diffuse.shaderView;
	mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	mFrameInputDescriptors[2] = viewZ.shaderView;
	mFrameInputDescriptors[3] = normalRoughness.shaderView;
	mFrameInputDescriptors[4] = baseColorMetalness.shaderView;
	mFrameInputDescriptors[5] = diffuse.shaderView;
	mFrameInputDescriptors[6] = specular.shaderView;
	mFrameInputDescriptors[8] = filteredDiffuse.shaderView;
	mFrameInputDescriptors[9] = filteredSpecular.shaderView;
	mFrameInputDescriptors[10] = rawShadow.shaderView;
	mFrameInputDescriptors[11] = filteredShadow.shaderView;
	mFrameInputDescriptors[12] = directLighting.shaderView;
	mFrameInputDescriptors[13] = directEmission.shaderView;
	UpdateFrameTextureSet(mCompositionFrameTextureSet, mFrameInputDescriptors);

	const nri::Descriptor* defaultOutput = composed.storageView;
	mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	mOutputDescriptors[1] = composed.storageView;
	UpdateOutputSet(mCompositionOutputSet, mOutputDescriptors);

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	BindSceneRootDescriptors();
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mSamplerSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 2, GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 3, mCompositionFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 4, mCompositionOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::Composition));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mRenderWidth), GetDispatchSize(mRenderHeight), 1 });
	return true;
}

bool NRIRenderer::DispatchTraceTransparent()
{
	Clocker clock(NriPTComposition);

	NRITextureResource& composed = GetFrameTexture(FrameTextureSlot::Composed);
	NRITextureResource& transparentOutput = GetFrameTexture(FrameTextureSlot::TraceTransparentOutput);
	CopyTexture(composed, transparentOutput);
	return true;
}

bool NRIRenderer::DispatchUpscalerPrepass(NRIMainUpscalerKind mainKind)
{
	if (mainKind == NRIMainUpscalerKind::Off)
	{
		return false;
	}

	const FrameTextureSlot vendorInputSlot =
		mainKind == NRIMainUpscalerKind::DLSR ? FrameTextureSlot::SrInput :
		FrameTextureSlot::RrInput;
	NRITextureResource& vendorInput = GetFrameTexture(vendorInputSlot);
	NRITextureResource& upscalerDepth = GetFrameTexture(FrameTextureSlot::UpscalerDepth);
	NRITextureResource& rrGuideDiffuseAlbedo = GetFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo);
	NRITextureResource& rrGuideSpecularAlbedo = GetFrameTexture(FrameTextureSlot::RrGuideSpecularAlbedo);
	NRITextureResource& rrGuideSpecularHitDistance = GetFrameTexture(FrameTextureSlot::RrGuideSpecularHitDistance);
	NRITextureResource& rrGuideNormalRoughness = GetFrameTexture(FrameTextureSlot::RrGuideNormalRoughness);
	const bool useSrPrepass = mainKind == NRIMainUpscalerKind::DLSR;

	// SR consumes the post-transparent composed signal, while RR now arrives with an
	// explicitly prepared noisy RrInput from the frame-graph path above.
	if (useSrPrepass)
	{
		CopyTexture(GetFrameTexture(FrameTextureSlot::TraceTransparentOutput), vendorInput);
	}
	mFrameBuffer->TransitionTexture(vendorInput, NRIComputeShaderResourceState());

	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(upscalerDepth, NRIComputeStorageState());
	if (!useSrPrepass)
	{
		mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredSpecular), NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(rrGuideDiffuseAlbedo, NRIComputeStorageState());
		mFrameBuffer->TransitionTexture(rrGuideSpecularAlbedo, NRIComputeStorageState());
		mFrameBuffer->TransitionTexture(rrGuideSpecularHitDistance, NRIComputeStorageState());
		mFrameBuffer->TransitionTexture(rrGuideNormalRoughness, NRIComputeStorageState());
	}

	const nri::Descriptor* defaultInput = GetFrameTexture(FrameTextureSlot::ViewZ).shaderView;
	mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	mFrameInputDescriptors[2] = GetFrameTexture(FrameTextureSlot::ViewZ).shaderView;
	if (!useSrPrepass)
	{
		mFrameInputDescriptors[3] = GetFrameTexture(FrameTextureSlot::NormalRoughness).shaderView;
		mFrameInputDescriptors[4] = GetFrameTexture(FrameTextureSlot::BaseColorMetalness).shaderView;
		mFrameInputDescriptors[6] = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular).shaderView;
	}
	if (!UpdateFrameTextureSet(mUpscalerPrepassFrameTextureSet, mFrameInputDescriptors))
	{
		return false;
	}

	const nri::Descriptor* defaultOutput = upscalerDepth.storageView;
	mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	mOutputDescriptors[12] = upscalerDepth.storageView;
	if (!useSrPrepass)
	{
		mOutputDescriptors[5] = rrGuideNormalRoughness.storageView;
		mOutputDescriptors[9] = rrGuideDiffuseAlbedo.storageView;
		mOutputDescriptors[10] = rrGuideSpecularAlbedo.storageView;
		mOutputDescriptors[11] = rrGuideSpecularHitDistance.storageView;
	}
	if (!UpdateOutputSet(mUpscalerPrepassOutputSet, mOutputDescriptors))
	{
		return false;
	}

	NRITraceSceneConstants constants = {};
	constants.RenderWidth = mRenderWidth;
	constants.RenderHeight = mRenderHeight;
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.FrameIndex = mFrameIndex;
	constants.ReservedTrace0 =
		mainKind == NRIMainUpscalerKind::DLSR ? 1u :
		mainKind == NRIMainUpscalerKind::DLRR ? 2u :
		0u;
	constants.ReservedTrace1 = (uint32_t)GetSelectedNrdDenoiserMode();
	constants.Flags = mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u;
	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	BindSceneRootDescriptors();
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mSamplerSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 2, GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 3, mUpscalerPrepassFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 4, mUpscalerPrepassOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(useSrPrepass ? PipelineSlot::DlssSrBefore : PipelineSlot::DlssBefore));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mRenderWidth), GetDispatchSize(mRenderHeight), 1 });
	return true;
}

bool NRIRenderer::DispatchRawPresent(FrameTextureSlot inputSlot, FrameTextureSlot secondarySlot, FrameTextureSlot tertiarySlot)
{
	Clocker clock(NriPTRawPresent);

	NRIPresentConstants constants = {};
	ApplyOutputPolicyToPresentConstants(mFrameBuffer->GetPathTracingOutputPolicy(), constants);
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.FrameIndex = mFrameIndex;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.PackedSceneOrigin = PackPresentSceneOrigin(mSceneLeft, mSceneTop);
	constants.DenoiserMode = (uint32_t)GetSelectedNrdDenoiserMode();

	NRITextureResource& input = GetFrameTexture(inputSlot);
	constants.InputWidth = input.width;
	constants.InputHeight = input.height;
	const bool addSecondary = secondarySlot != FrameTextureSlot::Count;
	NRITextureResource& secondary = GetFrameTexture(addSecondary ? secondarySlot : inputSlot);
	const bool hasTertiary = tertiarySlot != FrameTextureSlot::Count;
	NRITextureResource& tertiary = GetFrameTexture(hasTertiary ? tertiarySlot : inputSlot);
	NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	if (addSecondary)
	{
		constants.Flags |= NRI_FLAG_RAW_PRESENT_ADD_SECONDARY;
	}
	if (mUseSplitShadowDenoiser)
	{
		constants.Flags |= NRI_PRESENT_FLAG_SPLIT_SHADOW_DENOISER;
	}

	mFrameBuffer->TransitionTexture(input, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(secondary, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(tertiary, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	const nri::Descriptor* inputs[3] = {
		input.shaderView,
		secondary.shaderView,
		tertiary.shaderView
	};
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = mRawPresentFrameTextureSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputs;
	inputUpdate.descriptorNum = (uint32_t)std::size(inputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&inputUpdate, 1);

	const nri::Descriptor* outputs[1] = { final.storageView };
	nri::UpdateDescriptorRangeDesc outputUpdate = {};
	outputUpdate.descriptorSet = mRawPresentOutputSet;
	outputUpdate.rangeIndex = 0;
	outputUpdate.descriptors = outputs;
	outputUpdate.descriptorNum = (uint32_t)std::size(outputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&outputUpdate, 1);

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPresentPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mRawPresentFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mRawPresentOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::RawPresent));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mTargetWidth), GetDispatchSize(mTargetHeight), 1 });
	return true;
}

bool NRIRenderer::DispatchFinalPresent(FrameTextureSlot inputSlot)
{
	Clocker clock(NriPTFinalPresent);

	NRIPresentConstants constants = {};
	ApplyOutputPolicyToPresentConstants(mFrameBuffer->GetPathTracingOutputPolicy(), constants);
	ApplyNightVisionStateToPresentConstants(mNightVisionState, constants);
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.FrameIndex = mFrameIndex;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.PackedSceneOrigin = PackPresentSceneOrigin(mSceneLeft, mSceneTop);

	NRITextureResource& input = GetFrameTexture(inputSlot);
	NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	constants.InputWidth = input.width;
	constants.InputHeight = input.height;

	mFrameBuffer->TransitionTexture(input, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	const nri::Descriptor* inputs[3] = {
		input.shaderView,
		input.shaderView,
		input.shaderView
	};
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = mFinalPresentFrameTextureSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputs;
	inputUpdate.descriptorNum = (uint32_t)std::size(inputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&inputUpdate, 1);

	const nri::Descriptor* outputs[1] = { final.storageView };
	nri::UpdateDescriptorRangeDesc outputUpdate = {};
	outputUpdate.descriptorSet = mFinalPresentOutputSet;
	outputUpdate.rangeIndex = 0;
	outputUpdate.descriptors = outputs;
	outputUpdate.descriptorNum = (uint32_t)std::size(outputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&outputUpdate, 1);

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPresentPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mFinalPresentFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mFinalPresentOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::FinalPresent));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mTargetWidth), GetDispatchSize(mTargetHeight), 1 });
	return true;
}

bool NRIRenderer::DispatchUpscaleChain()
{
	Clocker clock(NriPTUpscale);

	const NRIMainUpscalerKind mainKind = ResolveMainUpscalerKind(true);
	const NRIPostSharpenKind postSharpenKind = ResolvePostSharpenKind(true);
	const bool runAppTaa = ShouldRunAppTaa(mainKind);
	const bool useAppTaaJitter = runAppTaa && !mGuiCaptureActive;
	NRITextureResource& composed = GetFrameTexture(FrameTextureSlot::TraceTransparentOutput);
	const FrameTextureSlot vendorSourceSlot =
		mainKind == NRIMainUpscalerKind::DLRR ? FrameTextureSlot::RrInput :
		FrameTextureSlot::TraceTransparentOutput;
	NRITextureResource& historyInput = GetFrameTexture(mHistoryInputSlot);
	NRITextureResource& historyOutput = GetFrameTexture(mHistoryOutputSlot);
	TraceTemporalState("upscale-entry", mainKind, postSharpenKind, runAppTaa, mHistoryOutputSlot, vendorSourceSlot);

	if (runAppTaa)
	{
		NRITemporalConstants constants = {};
		constants.RenderWidth = mRenderWidth;
		constants.RenderHeight = mRenderHeight;
		constants.FrameIndex = mFrameIndex;
		constants.Flags =
			(mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
			(useAppTaaJitter ? NRI_FLAG_USE_JITTER : 0u);
		constants.Exposure = GetTemporalExposure(mFrameBuffer->GetPathTracingOutputPolicy());

		mFrameBuffer->TransitionTexture(composed, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(historyInput, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(historyOutput, NRIComputeStorageState());

		const nri::Descriptor* taaInputs[3] = {
			historyInput.shaderView,
			GetFrameTexture(FrameTextureSlot::Motion).shaderView,
			composed.shaderView
		};
		nri::UpdateDescriptorRangeDesc taaInputUpdate = {};
		taaInputUpdate.descriptorSet = mTaaFrameTextureSet;
		taaInputUpdate.rangeIndex = 0;
		taaInputUpdate.descriptors = taaInputs;
		taaInputUpdate.descriptorNum = (uint32_t)std::size(taaInputs);
		mFrameBuffer->mCore.UpdateDescriptorRanges(&taaInputUpdate, 1);

		const nri::Descriptor* taaOutputs[1] = { historyOutput.storageView };
		nri::UpdateDescriptorRangeDesc taaOutputUpdate = {};
		taaOutputUpdate.descriptorSet = mTaaOutputSet;
		taaOutputUpdate.rangeIndex = 0;
		taaOutputUpdate.descriptors = taaOutputs;
		taaOutputUpdate.descriptorNum = (uint32_t)std::size(taaOutputs);
		mFrameBuffer->mCore.UpdateDescriptorRanges(&taaOutputUpdate, 1);

		mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mTaaPipelineLayout);
		mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mTaaFrameTextureSet, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mTaaOutputSet, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::Taa));
		mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mRenderWidth), GetDispatchSize(mRenderHeight), 1 });
	}
	else if (mainKind == NRIMainUpscalerKind::Off)
	{
		CopyTexture(composed, historyOutput);
	}
	else if (mainKind == NRIMainUpscalerKind::DLSR)
	{
		// Keep ptdebug 13 meaningful even when app-TAA is intentionally bypassed for vendor SR.
		CopyTexture(composed, historyOutput);
	}
	else if (mainKind == NRIMainUpscalerKind::DLRR)
	{
		// Keep ptdebug 13 meaningful for RR as well by exposing the explicit noisy RR input.
		CopyTexture(GetFrameTexture(FrameTextureSlot::RrInput), historyOutput);
	}

	FrameTextureSlot resolvedInputSlot = mHistoryOutputSlot;

	if (mainKind != NRIMainUpscalerKind::Off)
	{
		const FrameTextureSlot vendorInputSlot =
			mainKind == NRIMainUpscalerKind::DLSR ? FrameTextureSlot::SrInput :
			FrameTextureSlot::RrInput;
		NRITextureResource& vendorInput = GetFrameTexture(vendorInputSlot);
		NRITextureResource& upscalerDepth = GetFrameTexture(FrameTextureSlot::UpscalerDepth);
		NRITextureResource& rrGuideDiffuseAlbedo = GetFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo);
		NRITextureResource& rrGuideSpecularAlbedo = GetFrameTexture(FrameTextureSlot::RrGuideSpecularAlbedo);
		NRITextureResource& rrGuideSpecularHitDistance = GetFrameTexture(FrameTextureSlot::RrGuideSpecularHitDistance);
		NRITextureResource& rrGuideNormalRoughness = GetFrameTexture(FrameTextureSlot::RrGuideNormalRoughness);
		NRITextureResource& vendorOutput = GetFrameTexture(FrameTextureSlot::VendorOutput);

		if (!DispatchUpscalerPrepass(mainKind))
		{
			return false;
		}

		mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(vendorInput, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(upscalerDepth, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(rrGuideDiffuseAlbedo, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(rrGuideSpecularAlbedo, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(rrGuideSpecularHitDistance, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(rrGuideNormalRoughness, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(vendorOutput, NRIComputeStorageState());

		const nri::UpscalerMode resolvedUpscalerMode = ResolveUpscalerModeForMain(mainKind, GetSelectedUpscalerMode());
		if (!mUpscaler.EnsureMainUpscaler(*mFrameBuffer, mainKind, resolvedUpscalerMode, mOutputWidth, mOutputHeight))
		{
			return false;
		}

		NRIUpscalerDispatchDesc upscalerDesc = {};
		upscalerDesc.commandBuffer = mFrameBuffer->mCommandBuffer;
		upscalerDesc.input = &vendorInput;
		upscalerDesc.output = &vendorOutput;
		upscalerDesc.motion = &GetFrameTexture(FrameTextureSlot::Motion);
		upscalerDesc.depth = &upscalerDepth;
		upscalerDesc.normalRoughness = &rrGuideNormalRoughness;
		upscalerDesc.diffuseAlbedo = &rrGuideDiffuseAlbedo;
		upscalerDesc.specularAlbedo = &rrGuideSpecularAlbedo;
		upscalerDesc.specularHitDistance = &rrGuideSpecularHitDistance;
		upscalerDesc.currentWidth = mRenderWidth;
		upscalerDesc.currentHeight = mRenderHeight;
		Copy2(mCurrentJitter, upscalerDesc.cameraJitter);
		std::memcpy(upscalerDesc.viewToClipMatrix, mCurrentViewToClip, sizeof(upscalerDesc.viewToClipMatrix));
		std::memcpy(upscalerDesc.worldToViewMatrix, mCurrentWorldToView, sizeof(upscalerDesc.worldToViewMatrix));
		upscalerDesc.sharpness = Clamp01((float)nri_sharpness);
		upscalerDesc.resetHistory = mResetHistory;
		if (!mUpscaler.DispatchMainUpscaler(*mFrameBuffer, mainKind, upscalerDesc))
		{
			return false;
		}

		mUseUpscaledInFinal = true;
		mUpscaledInputSlot = FrameTextureSlot::VendorOutput;
		resolvedInputSlot = FrameTextureSlot::VendorOutput;
		TraceTemporalState("upscale-vendor", mainKind, postSharpenKind, runAppTaa, mUpscaledInputSlot, vendorSourceSlot);
	}
	else
	{
		mUseUpscaledInFinal = false;
		mUpscaledInputSlot = mHistoryOutputSlot;
		resolvedInputSlot = mHistoryOutputSlot;
		TraceTemporalState("upscale-native", mainKind, postSharpenKind, runAppTaa, resolvedInputSlot, mHistoryOutputSlot);
	}

	if (postSharpenKind == NRIPostSharpenKind::Off)
	{
		return true;
	}

	NRITextureResource& postInput = GetFrameTexture(resolvedInputSlot);
	NRITextureResource& postOutput = GetFrameTexture(FrameTextureSlot::PostSharpenOutput);
	mFrameBuffer->TransitionTexture(postInput, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(postOutput, NRIComputeStorageState());
	if (!mUpscaler.EnsurePostSharpen(*mFrameBuffer, postSharpenKind, mOutputWidth, mOutputHeight))
	{
		return false;
	}

	NRIUpscalerDispatchDesc postDesc = {};
	postDesc.commandBuffer = mFrameBuffer->mCommandBuffer;
	postDesc.input = &postInput;
	postDesc.output = &postOutput;
	postDesc.currentWidth = postInput.width;
	postDesc.currentHeight = postInput.height;
	Copy2(mCurrentJitter, postDesc.cameraJitter);
	postDesc.sharpness = Clamp01((float)nri_sharpness);
	postDesc.resetHistory = mResetHistory;
	if (!mUpscaler.DispatchPostSharpen(*mFrameBuffer, postSharpenKind, postDesc))
	{
		return false;
	}

	mUseUpscaledInFinal = true;
	mUpscaledInputSlot = FrameTextureSlot::PostSharpenOutput;
	TraceTemporalState("upscale-post-sharpen", mainKind, postSharpenKind, runAppTaa, mUpscaledInputSlot, resolvedInputSlot);
	return true;
}

bool NRIRenderer::DispatchFinal()
{
	Clocker clock(NriPTFinal);

	NRITraceSceneConstants constants = {};
	const uint32_t bootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	const bool presentRawTrace = (!nri_ptbootstrap && !mUseUpscaledInFinal) || bootstrapMode >= 13u;
	Copy3(mCurrentCameraPos, constants.CameraPos);
	Copy3(mCurrentCameraForward, constants.CameraForward);
	Copy3(mCurrentCameraRight, constants.CameraRight);
	Copy3(mCurrentCameraUp, constants.CameraUp);
	Copy3(mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = mRenderWidth;
	constants.RenderHeight = mRenderHeight;
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.TanHalfFovX = mCurrentTanHalfFovX;
	constants.TanHalfFovY = mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = mPreviousTanHalfFovY;
	constants.SceneInstanceCount = mSceneInstanceBuffer.stride != 0 ? (uint32_t)(mSceneInstanceBuffer.usedSize / mSceneInstanceBuffer.stride) : 0u;
	constants.StaticPrimitiveCount = mBoundStaticPrimitiveCount;
	constants.DynamicPrimitiveCount = mBoundDynamicPrimitiveCount;
	constants.FrameIndex = mFrameIndex;
	constants.Flags =
		(mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(mUseUpscaledInFinal ? NRI_FLAG_USE_UPSCALED : 0u) |
		(presentRawTrace ? NRI_FLAG_PRESENT_RAW_TRACE : 0u) |
		(mUseSplitShadowDenoiser ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(mDirectionalLightState.enabled && mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u);
	constants.StaticMaterialCount = mBoundStaticMaterialCount;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = mBoundDynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(0u, 0u, mDirectionalLightState.color);
	constants.RuntimeLightCount = mBoundRuntimeLightCount;
	constants.ReservedTrace0 = (uint16_t)(int16_t)mSceneLeft | ((uint32_t)(uint16_t)(int16_t)mSceneTop << 16);
	constants.ReservedTrace1 = PackDenoiserAux1(0u, mDirectionalLightState.angularSize);
	Copy3(mSkyColor, constants.SkyColor);
	Copy3(mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(mDirectionalLightState, constants);

	NRITextureResource& history = GetFrameTexture(mHistoryOutputSlot);
	NRITextureResource& upscaled = GetFrameTexture(mUpscaledInputSlot);
	NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredSpecular), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DenoisedShadow), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DirectLighting), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DirectEmission), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Validation), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::RrGuideSpecularAlbedo), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::RrGuideSpecularHitDistance), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(history, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(upscaled, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	mFrameInputDescriptors.fill(GetFrameTexture(FrameTextureSlot::Composed).shaderView);
	mFrameInputDescriptors[0] = history.shaderView;
	mFrameInputDescriptors[1] = GetFrameTexture(FrameTextureSlot::Motion).shaderView;
	mFrameInputDescriptors[2] = GetFrameTexture(FrameTextureSlot::ViewZ).shaderView;
	mFrameInputDescriptors[3] = GetFrameTexture(FrameTextureSlot::NormalRoughness).shaderView;
	mFrameInputDescriptors[4] = GetFrameTexture(FrameTextureSlot::BaseColorMetalness).shaderView;
	mFrameInputDescriptors[5] = presentRawTrace ? (mUseUpscaledInFinal ? upscaled.shaderView : GetFrameTexture(FrameTextureSlot::Composed).shaderView) : GetFrameTexture(FrameTextureSlot::Composed).shaderView;
	mFrameInputDescriptors[6] = upscaled.shaderView;
	mFrameInputDescriptors[7] = GetFrameTexture(FrameTextureSlot::Validation).shaderView;
	mFrameInputDescriptors[8] = GetFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo).shaderView;
	mFrameInputDescriptors[9] = GetFrameTexture(FrameTextureSlot::RrGuideSpecularAlbedo).shaderView;
	mFrameInputDescriptors[10] = GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra).shaderView;
	mFrameInputDescriptors[11] = GetFrameTexture(FrameTextureSlot::DenoisedShadow).shaderView;
	mFrameInputDescriptors[12] = GetFrameTexture(FrameTextureSlot::DirectLighting).shaderView;
	mFrameInputDescriptors[13] = GetFrameTexture(FrameTextureSlot::DirectEmission).shaderView;
	if (constants.DebugMode == 10)
	{
		mFrameInputDescriptors[5] = GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse).shaderView;
	}
	else if (constants.DebugMode == 11)
	{
		mFrameInputDescriptors[5] = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular).shaderView;
	}
	UpdateFrameTextureSet();

	mOutputDescriptors.fill(final.storageView);
	mOutputDescriptors[2] = final.storageView;
	UpdateOutputSet();

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	BindSceneRootDescriptors();
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mSamplerSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 2, GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 3, mFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 4, mOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::Final));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mTargetWidth), GetDispatchSize(mTargetHeight), 1 });
	return true;
}

void NRIRenderer::UpdatePerFrameState(HWDrawInfo& di)
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.updateStateMs);
	Clocker clock(NriPTUpdateState);

	if (mHasPreviousCameraState)
	{
		Copy3(mCurrentCameraPos, mPreviousCameraPos);
		Copy3(mCurrentCameraForward, mPreviousCameraForward);
		Copy3(mCurrentCameraRight, mPreviousCameraRight);
		Copy3(mCurrentCameraUp, mPreviousCameraUp);
		mPreviousTanHalfFovX = mCurrentTanHalfFovX;
		mPreviousTanHalfFovY = mCurrentTanHalfFovY;
		Copy2(mCurrentJitter, mPreviousJitter);
		std::memcpy(mPreviousViewToClip, mCurrentViewToClip, sizeof(mPreviousViewToClip));
		std::memcpy(mPreviousWorldToView, mCurrentWorldToView, sizeof(mPreviousWorldToView));
	}

	VSMatrix inverseView;
	if (!di.VPUniforms.mViewMatrix.inverseMatrix(inverseView))
	{
		std::memset(mCurrentCameraPos, 0, sizeof(mCurrentCameraPos));
		std::memset(mCurrentCameraForward, 0, sizeof(mCurrentCameraForward));
		std::memset(mCurrentCameraRight, 0, sizeof(mCurrentCameraRight));
		std::memset(mCurrentCameraUp, 0, sizeof(mCurrentCameraUp));
		mCurrentCameraForward[2] = -1.0f;
		mCurrentCameraRight[0] = 1.0f;
		mCurrentCameraUp[1] = 1.0f;
	}
	else
	{
		float origin[4] = {};
		float rightPoint[4] = {};
		float upPoint[4] = {};
		float forwardPoint[4] = {};
		TransformPoint(inverseView, 0.0f, 0.0f, 0.0f, origin);
		TransformPoint(inverseView, 1.0f, 0.0f, 0.0f, rightPoint);
		TransformPoint(inverseView, 0.0f, 1.0f, 0.0f, upPoint);
		TransformPoint(inverseView, 0.0f, 0.0f, -1.0f, forwardPoint);

		const float cameraPos[3] = {
			origin[0],
			origin[1],
			origin[2]
		};
		const float rightDelta[3] = {
			rightPoint[0] - origin[0],
			rightPoint[1] - origin[1],
			rightPoint[2] - origin[2]
		};
		const float upDelta[3] = {
			upPoint[0] - origin[0],
			upPoint[1] - origin[1],
			upPoint[2] - origin[2]
		};
		const float forwardDelta[3] = {
			forwardPoint[0] - origin[0],
			forwardPoint[1] - origin[1],
			forwardPoint[2] - origin[2]
		};

		Copy3(cameraPos, mCurrentCameraPos);
		Copy3(rightDelta, mCurrentCameraRight);
		Copy3(upDelta, mCurrentCameraUp);
		Copy3(forwardDelta, mCurrentCameraForward);

		Normalize3(mCurrentCameraRight);
		Normalize3(mCurrentCameraUp);
		Normalize3(mCurrentCameraForward);
	}

	const float* projection = di.VPUniforms.mProjectionMatrix.get();
	const float projectionScaleX = projection != nullptr ? std::fabs(projection[0]) : 0.0f;
	const float projectionScaleY = projection != nullptr ? std::fabs(projection[5]) : 0.0f;
	if (projectionScaleX > 0.0001f && projectionScaleY > 0.0001f)
	{
		// Match the hardware backend frustum exactly instead of rebuilding Y-FOV from the PT render dimensions.
		mCurrentTanHalfFovX = 1.0f / projectionScaleX;
		mCurrentTanHalfFovY = 1.0f / projectionScaleY;
	}
	else
	{
		const float tanHalfFovX = tanf((float)di.Viewpoint.FieldOfView.Radians() * 0.5f);
		mCurrentTanHalfFovX = tanHalfFovX;
		mCurrentTanHalfFovY = tanHalfFovX * ((float)mRenderHeight / std::max(1.0f, (float)mRenderWidth));
	}
	const NRIMainUpscalerKind resolvedMainUpscaler = ResolveMainUpscalerKind(false);
	if (!nri_ptbootstrap && !mGuiCaptureActive && ShouldUseTemporalJitter(resolvedMainUpscaler))
	{
		ComputeTemporalJitter(mFrameIndex, mCurrentJitter);
	}
	else
	{
		mCurrentJitter[0] = 0.0f;
		mCurrentJitter[1] = 0.0f;
	}
	FillMatrix(mCurrentViewToClip, di.VPUniforms.mProjectionMatrix);
	FillMatrix(mCurrentWorldToView, di.VPUniforms.mViewMatrix);
	const BitArray& visibleSectors = di.GetVisibleSectors();
	const size_t visibleChunkWordCount = std::max<size_t>((mMapWorld.chunks.size() + 31u) / 32u, 1u);
	const size_t visibleFlatPlaneWordCount = std::max<size_t>(((size_t)sector.Size() * 2u + 31u) / 32u, 1u);
	mCurrentVisibleChunkWords.assign(visibleChunkWordCount, 0u);
	mCurrentVisibleFlatPlaneWords.assign(visibleFlatPlaneWordCount, 0u);
	for (unsigned sectorIndex = 0; sectorIndex < visibleSectors.Size(); ++sectorIndex)
	{
		if (!visibleSectors.Check(sectorIndex))
		{
			continue;
		}

		MarkVisibleChunkForSector(mMapWorld, (int32_t)sectorIndex, mCurrentVisibleChunkWords);
	}
	// HWDrawInfo can accumulate geometry from multiple RenderScene passes
	// while its final visible-sector bitset only reflects the last traversal.
	// Union the root sectors and accumulated drawlists so the PT chunk gate
	// tracks the scene the HAL actually built this frame.
	AccumulateVisibleChunksFromViewRoots(di, mMapWorld, mCurrentVisibleChunkWords);
	AccumulateVisibleChunksFromDrawLists(di, mMapWorld, mCurrentVisibleChunkWords);
	// Chunk visibility is still too coarse for overlapping static floors and ceilings.
	// Track the exact floor/ceiling sectors backed by the accumulated flat drawlists
	// so the RT primary path can reject hidden coplanar static flat sections.
	AccumulateVisibleFlatPlanesFromDrawLists(di, mCurrentVisibleFlatPlaneWords);
	if (ShouldEmitTemporalTraceLogs())
	{
		const uint32_t targetWidth = mFrameBuffer->mActiveTarget != nullptr ? mFrameBuffer->mActiveTarget->width : 0u;
		const uint32_t targetHeight = mFrameBuffer->mActiveTarget != nullptr ? mFrameBuffer->mActiveTarget->height : 0u;
		const int32_t sceneLeft = mFrameBuffer->mSceneViewport.left;
		const int32_t sceneBottom = mFrameBuffer->mSceneViewport.top;
		const int32_t sceneWidth = mFrameBuffer->mSceneViewport.width;
		const int32_t sceneHeight = mFrameBuffer->mSceneViewport.height;
		const int32_t sceneTop = (int32_t)targetHeight - sceneBottom - sceneHeight;
		const auto& uniformCameraPos = di.VPUniforms.mCameraPos;
		const FVector3 hwForward(di.Viewpoint.HWAngles);
		Printf("NRI PT camera: frame=%u hw_pitch=%.3f hw_yaw=%.3f hw_roll=%.3f scene_bl=(%d,%d %dx%d) scene_tl=(%u,%u %ux%u) target=%ux%u uniform_pos=(%.3f,%.3f,%.3f) inverse_pos=(%.3f,%.3f,%.3f) hw_forward=(%.3f,%.3f,%.3f) basis_fwd=(%.3f,%.3f,%.3f) basis_right=(%.3f,%.3f,%.3f) basis_up=(%.3f,%.3f,%.3f) tan=(%.6f,%.6f) proj=(%.6f,%.6f,%.6f,%.6f)\n",
			mFrameIndex,
			di.Viewpoint.HWAngles.Pitch.Degrees(),
			di.Viewpoint.HWAngles.Yaw.Degrees(),
			di.Viewpoint.HWAngles.Roll.Degrees(),
			mFrameBuffer->mSceneViewport.left,
			mFrameBuffer->mSceneViewport.top,
			mFrameBuffer->mSceneViewport.width,
			mFrameBuffer->mSceneViewport.height,
			sceneLeft,
			sceneTop,
			sceneWidth,
			sceneHeight,
			targetWidth,
			targetHeight,
			uniformCameraPos.X,
			uniformCameraPos.Y,
			uniformCameraPos.Z,
			mCurrentCameraPos[0],
			mCurrentCameraPos[1],
			mCurrentCameraPos[2],
			hwForward.X,
			hwForward.Y,
			hwForward.Z,
			mCurrentCameraForward[0],
			mCurrentCameraForward[1],
			mCurrentCameraForward[2],
			mCurrentCameraRight[0],
			mCurrentCameraRight[1],
			mCurrentCameraRight[2],
			mCurrentCameraUp[0],
			mCurrentCameraUp[1],
			mCurrentCameraUp[2],
			mCurrentTanHalfFovX,
			mCurrentTanHalfFovY,
			projection != nullptr ? projection[0] : 0.0f,
			projection != nullptr ? projection[5] : 0.0f,
			projection != nullptr ? projection[8] : 0.0f,
			projection != nullptr ? projection[9] : 0.0f);
	}

	if (mHasPreviousCameraState && !mResetHistory)
	{
		const float dx = mCurrentCameraPos[0] - mPreviousCameraPos[0];
		const float dy = mCurrentCameraPos[1] - mPreviousCameraPos[1];
		const float dz = mCurrentCameraPos[2] - mPreviousCameraPos[2];
		const float distanceSq = dx * dx + dy * dy + dz * dz;
		static constexpr float TeleportDistanceThreshold = 2048.0f;
		if (distanceSq > TeleportDistanceThreshold * TeleportDistanceThreshold)
		{
			RequestHistoryReset("camera-teleport", true, false);
		}
	}

	if (!mHasPreviousCameraState)
	{
		Copy3(mCurrentCameraPos, mPreviousCameraPos);
		Copy3(mCurrentCameraForward, mPreviousCameraForward);
		Copy3(mCurrentCameraRight, mPreviousCameraRight);
		Copy3(mCurrentCameraUp, mPreviousCameraUp);
		mPreviousTanHalfFovX = mCurrentTanHalfFovX;
		mPreviousTanHalfFovY = mCurrentTanHalfFovY;
		Copy2(mCurrentJitter, mPreviousJitter);
		std::memcpy(mPreviousViewToClip, mCurrentViewToClip, sizeof(mPreviousViewToClip));
		std::memcpy(mPreviousWorldToView, mCurrentWorldToView, sizeof(mPreviousWorldToView));
	}

	UpdateNightVisionState();
}

void NRIRenderer::LogBridgeStats(const nri_scene::SceneDebugStats& stats)
{
	if (!nri_ptscenestats)
	{
		mLastStats = stats;
		mHasLoggedStats = true;
		return;
	}

	if (!mHasLoggedStats || StatsDiffer(mLastStats, stats))
	{
		Printf("NRI PT scene: walls=%u flats=%u sprites=%u translucent=%u models=%u voxel_proxies=%u unsupported_models=%u mirrors=%u skies=%u portal_views=%u portal_skips=%u approx_tris=%u materials=%u\n",
			stats.wallDrawItems,
			stats.flatDrawItems,
			stats.spriteDrawItems,
			stats.translucentDrawItems,
			stats.modelDrawItems,
			stats.voxelProxyDrawItems,
			stats.unsupportedModelDrawItems,
			stats.mirrorSurfaces,
			stats.skySurfaces,
			stats.portalViews,
			stats.portalCapturesSkipped,
			stats.triangleEstimate,
			stats.materialRefs);
		mLastStats = stats;
		mHasLoggedStats = true;
	}
}

void NRIRenderer::TraceSkyState(const nri_scene::SceneView& sceneView, const char* action, uint64_t resolvedKey)
{
	if (!ShouldEmitTemporalTraceLogs())
	{
		return;
	}

	const SkyState tracedState = {
		sceneView.sky.mode,
		sceneView.sky.sourceType,
		sceneView.sky.texture,
		sceneView.sky.faceMask,
		sceneView.sky.flipTop
	};

	const bool changed =
		!mHasTracedSkyState ||
		mLastTracedSkyState.mode != tracedState.mode ||
		mLastTracedSkyState.sourceType != tracedState.sourceType ||
		mLastTracedSkyState.texture != tracedState.texture ||
		mLastTracedSkyState.faceMask != tracedState.faceMask ||
		mLastTracedSkyState.flipTop != tracedState.flipTop ||
		mLastTracedSkyResolvedKey != resolvedKey;

	if (!changed && action == nullptr)
	{
		return;
	}

	const NRITextureResource* activeSkyTexture = GetActiveSkyTexture();
	Printf("NRI PT sky: captured_mode=%s source=%s texture=%p face_mask=0x%x flip_top=%s skies=%u color=(%.3f, %.3f, %.3f) action=%s resolved_key=0x%llx active_mode=%s active_key=0x%llx active_size=%ux%u\n",
		GetSkyModeName(sceneView.sky.mode),
		GetSkySourceTypeName(sceneView.sky.sourceType),
		sceneView.sky.texture,
		sceneView.sky.faceMask,
		sceneView.sky.flipTop ? "true" : "false",
		sceneView.stats.skySurfaces,
		sceneView.skyColor[0],
		sceneView.skyColor[1],
		sceneView.skyColor[2],
		action != nullptr ? action : "unchanged",
		(unsigned long long)resolvedKey,
		GetSkyModeName(mSkyState.mode),
		(unsigned long long)mSkyTextureKey,
		activeSkyTexture != nullptr ? activeSkyTexture->width : 0,
		activeSkyTexture != nullptr ? activeSkyTexture->height : 0);

	mLastTracedSkyState = tracedState;
	mLastTracedSkyResolvedKey = resolvedKey;
	mHasTracedSkyState = true;
}

void NRIRenderer::CopyFinalToActiveTarget()
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.copyFinalMs);
	Clocker clock(NriPTCopyFinal);

	UpdateFrameGenerationFrameDesc();
	NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	CopyTextureToActiveTarget(final);
}

void NRIRenderer::UpdateFrameGenerationHistoryPolicy(int debugMode, const NRIFrameGenerationPolicy& frameGenPolicy, bool preserveHistory)
{
	if (preserveHistory)
	{
		return;
	}

	const NRIPTOutputPolicy outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	if (!mHasOutputPolicyState)
	{
		mHasOutputPolicyState = true;
		mLastOutputRequestedMode = outputPolicy.requestedMode;
		mLastOutputResolvedMode = outputPolicy.resolvedMode;
	}
	else if (outputPolicy.requestedMode != mLastOutputRequestedMode || outputPolicy.resolvedMode != mLastOutputResolvedMode)
	{
		RequestHistoryReset("output-mode-change");
		mFrameBuffer->PrintPathTracingOutputModeChange(mFrameIndex, mLastOutputRequestedMode, mLastOutputResolvedMode);
		if (ShouldEmitTemporalTraceLogs())
		{
			Printf("NRI PT temporal reset: reason=output-mode-change frame=%u requested_output=%s->%s resolved_output=%s->%s\n",
				mFrameIndex,
				GetNRIPTOutputModeName(mLastOutputRequestedMode),
				GetNRIPTOutputModeName(outputPolicy.requestedMode),
				GetNRIPTOutputModeName(mLastOutputResolvedMode),
				GetNRIPTOutputModeName(outputPolicy.resolvedMode));
		}
		mLastOutputRequestedMode = outputPolicy.requestedMode;
		mLastOutputResolvedMode = outputPolicy.resolvedMode;
	}

	const float temporalExposure = GetTemporalExposure(outputPolicy);
	if (!mHasTemporalExposureState)
	{
		mHasTemporalExposureState = true;
		mLastTemporalExposure = temporalExposure;
	}
	else
	{
		const float exposureDeltaStops = GetExposureDeltaStops(mLastTemporalExposure, temporalExposure);
		if (exposureDeltaStops >= NRI_TAA_EXPOSURE_RESET_THRESHOLD_STOPS)
		{
			RequestHistoryReset("exposure-change");
			if (ShouldEmitTemporalTraceLogs())
			{
				Printf("NRI PT temporal reset: reason=exposure-change frame=%u exposure=%.3f->%.3f delta_stops=%.3f threshold=%.3f\n",
					mFrameIndex,
					mLastTemporalExposure,
					temporalExposure,
					exposureDeltaStops,
					NRI_TAA_EXPOSURE_RESET_THRESHOLD_STOPS);
			}
		}

		mLastTemporalExposure = temporalExposure;
	}

	const NRIMainUpscalerKind resolvedMainUpscaler = ResolveMainUpscalerKind(false);
	const NRIPostSharpenKind resolvedPostSharpen = ResolvePostSharpenKind(false);
	const bool runAppTaa = ShouldRunAppTaa(resolvedMainUpscaler);
	if (!nri_ptbootstrap &&
		(debugMode != mLastDebugMode ||
		 resolvedMainUpscaler != mLastTemporalHistoryMainUpscaler ||
		 resolvedPostSharpen != mLastTemporalPostSharpen ||
		 runAppTaa != mLastTemporalAppTaaEnabled))
	{
		ArmTemporalTraceBudget("mode-change");
		if (ShouldEmitTemporalTraceLogs())
		{
			Printf("NRI PT temporal reset: reason=mode-change frame=%u debug=%d->%d main=%s->%s post=%s->%s app_taa=%s->%s\n",
				mFrameIndex,
				mLastDebugMode,
				debugMode,
				GetMainUpscalerName(mLastTemporalHistoryMainUpscaler),
				GetMainUpscalerName(resolvedMainUpscaler),
				GetPostSharpenName(mLastTemporalPostSharpen),
				GetPostSharpenName(resolvedPostSharpen),
				mLastTemporalAppTaaEnabled ? "yes" : "no",
				runAppTaa ? "yes" : "no");
		}
		RequestHistoryReset("mode-change");
	}
	mLastDebugMode = debugMode;
	mLastTemporalHistoryMainUpscaler = resolvedMainUpscaler;
	mLastTemporalPostSharpen = resolvedPostSharpen;
	mLastTemporalAppTaaEnabled = runAppTaa;

	if (!mHasFrameGenerationConfigState)
	{
		mHasFrameGenerationConfigState = true;
		mLastFrameGenerationRequestedEnabled = frameGenPolicy.requestedEnabled;
		mLastFrameGenerationRequestedProvider = frameGenPolicy.requestedProvider;
		mLastFrameGenerationResolvedUiMode = frameGenPolicy.resolvedUiMode;
		return;
	}

	const char* frameGenResetReason = nullptr;
	if (frameGenPolicy.requestedEnabled != mLastFrameGenerationRequestedEnabled)
	{
		frameGenResetReason = "framegen-toggle";
	}
	else if (frameGenPolicy.requestedProvider != mLastFrameGenerationRequestedProvider)
	{
		frameGenResetReason = "framegen-provider-change";
	}
	else if (frameGenPolicy.resolvedUiMode != mLastFrameGenerationResolvedUiMode)
	{
		frameGenResetReason = "framegen-ui-mode-change";
	}

	if (frameGenResetReason != nullptr)
	{
		RequestHistoryReset(frameGenResetReason);
		if (ShouldEmitTemporalTraceLogs())
		{
			Printf("NRI PT temporal reset: reason=%s frame=%u requested=%s->%s provider=%s->%s ui=%s->%s\n",
				frameGenResetReason,
				mFrameIndex,
				mLastFrameGenerationRequestedEnabled ? "on" : "off",
				frameGenPolicy.requestedEnabled ? "on" : "off",
				NRIFrameGenerationContext::GetProviderName(mLastFrameGenerationRequestedProvider),
				NRIFrameGenerationContext::GetProviderName(frameGenPolicy.requestedProvider),
				NRIFrameGenerationContext::GetUiModeName(mLastFrameGenerationResolvedUiMode),
				NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.resolvedUiMode));
		}
	}

	mLastFrameGenerationRequestedEnabled = frameGenPolicy.requestedEnabled;
	mLastFrameGenerationRequestedProvider = frameGenPolicy.requestedProvider;
	mLastFrameGenerationResolvedUiMode = frameGenPolicy.resolvedUiMode;
}

void NRIRenderer::NoteSuccessfulRealFrame()
{
	mLastFrameGenerationRealFrameTimeMs = mPendingFrameGenerationRealFrameTimeMs;
	mHasFrameGenerationRealFrameTime = mHasPendingFrameGenerationRealFrameTime;
	mLastFrameGenerationTimestamp = mPendingFrameGenerationTimestamp;
	mHasFrameGenerationTimestamp = true;
	++mFrameGenerationFrameId;
}

void NRIRenderer::UpdateFrameGenerationFrameDesc()
{
	if (mFrameBuffer == nullptr)
	{
		return;
	}

	NRIFrameGenerationFrameDesc desc = {};
	desc.frameId = mFrameGenerationFrameId + 1u;
	desc.renderWidth = mRenderWidth;
	desc.renderHeight = mRenderHeight;
	desc.outputWidth = mOutputWidth;
	desc.outputHeight = mOutputHeight;
	desc.renderRect = { 0u, 0u, mRenderWidth, mRenderHeight };
	desc.outputRect = { 0u, 0u, mOutputWidth, mOutputHeight };
	desc.hasPreviousCamera = mHasPreviousCameraState;
	desc.resetHistory = mResetHistory;
	desc.hasRealFrameTimeMs = mHasPendingFrameGenerationRealFrameTime;
	desc.realFrameTimeMs = mPendingFrameGenerationRealFrameTimeMs;
	const char* resetReason = mResetHistory && !mLastHistoryResetReason.empty() ? mLastHistoryResetReason.c_str() : "none";
	std::strncpy(desc.resetReason, resetReason, std::size(desc.resetReason) - 1u);
	desc.resetReason[std::size(desc.resetReason) - 1u] = '\0';
	desc.hudlessColorSource = NRIFrameGenerationColorSource::Final;
	desc.hudlessColor = &GetFrameTexture(FrameTextureSlot::Final);
	desc.uiTexture = nullptr;
	desc.motionVectors = &GetFrameTexture(FrameTextureSlot::Motion);
	desc.depth = &GetFrameTexture(FrameTextureSlot::UpscalerDepth);
	std::memcpy(desc.cameraJitter, mCurrentJitter, sizeof(desc.cameraJitter));
	std::memcpy(desc.previousCameraJitter, mPreviousJitter, sizeof(desc.previousCameraJitter));
	desc.motionVectorScale[0] = 1.0f;
	desc.motionVectorScale[1] = 1.0f;
	desc.motionVectorSpace = NRIFrameGenerationMotionVectorSpace::ScreenPixels;
	desc.motionVectorDirection = NRIFrameGenerationMotionVectorDirection::CurrentToPrevious;
	desc.depthType = NRIFrameGenerationDepthType::ClipDepth;
	desc.depthInverted = false;
	desc.depthInfinite = false;
	std::memcpy(desc.currentViewToClip, mCurrentViewToClip, sizeof(desc.currentViewToClip));
	std::memcpy(desc.previousViewToClip, mPreviousViewToClip, sizeof(desc.previousViewToClip));
	std::memcpy(desc.currentWorldToView, mCurrentWorldToView, sizeof(desc.currentWorldToView));
	std::memcpy(desc.previousWorldToView, mPreviousWorldToView, sizeof(desc.previousWorldToView));
	std::memcpy(desc.cameraPosition, mCurrentCameraPos, sizeof(desc.cameraPosition));
	std::memcpy(desc.cameraForward, mCurrentCameraForward, sizeof(desc.cameraForward));
	std::memcpy(desc.cameraRight, mCurrentCameraRight, sizeof(desc.cameraRight));
	std::memcpy(desc.cameraUp, mCurrentCameraUp, sizeof(desc.cameraUp));
	desc.cameraNear = screen->GetZNear();
	desc.cameraFar = screen->GetZFar();
	desc.cameraFovVerticalRadians = 2.0f * atanf(mCurrentTanHalfFovY);
	desc.viewSpaceToMetersFactor = 1.0f;
	mFrameBuffer->mFrameGeneration.SetFrameDesc(*mFrameBuffer, desc);
}

void NRIRenderer::CopyTexture(NRITextureResource& source, NRITextureResource& destination)
{
	mFrameBuffer->TransitionTexture(source, NRICopySourceState());
	mFrameBuffer->TransitionTexture(destination, NRICopyDestinationState());
	mFrameBuffer->mCore.CmdCopyTexture(*mFrameBuffer->mCommandBuffer, *destination.texture, nullptr, *source.texture, nullptr);
}

void NRIRenderer::CopyTextureToActiveTarget(NRITextureResource& source)
{
	mFrameBuffer->TransitionTexture(source, NRICopySourceState());
	mFrameBuffer->TransitionTexture(*mFrameBuffer->mActiveTarget, NRICopyDestinationState());
	mFrameBuffer->mCore.CmdCopyTexture(*mFrameBuffer->mCommandBuffer, *mFrameBuffer->mActiveTarget->texture, nullptr, *source.texture, nullptr);
	mFrameBuffer->mRenderState->NotifyExternalTargetWrite();
}

void NRIRenderer::DestroyCachedTextures()
{
	mStaticMapScene.texturesResident = false;
	for (auto& skyTexture : mSkyTextureCache)
	{
		mFrameBuffer->DestroyTextureResource(skyTexture.resource);
	}
	mSkyTextureCache.clear();
	mActiveSkyTextureIndex = UINT32_MAX;
	mSkyTextureKey = 0;
	mSkyLevel = nullptr;
	mSkyState = {};
	mLastTracedSkyState = {};
	mLastTracedSkyResolvedKey = 0;
	mHasTracedSkyState = false;
	for (auto& texture : mTextureCache)
	{
		mFrameBuffer->DestroyTextureResource(texture.resource);
	}
	mTextureCache.clear();
}

void NRIRenderer::DestroyFrameTextures()
{
	for (auto& texture : mFrameTextures)
	{
		mFrameBuffer->DestroyTextureResource(texture);
	}
	mRenderWidth = 0;
	mRenderHeight = 0;
	mOutputWidth = 0;
	mOutputHeight = 0;
	mTargetWidth = 0;
	mTargetHeight = 0;
	mSceneLeft = 0;
	mSceneTop = 0;
	mFinalSceneFormat = nri::Format::UNKNOWN;
}

void NRIRenderer::DestroySceneBuffers()
{
	mStaticMapScene.buffersResident = false;
	ResetStaticMapChunkAtlas(mStaticMapChunkAtlas);
	ResetResidentMapChunkRegistry();
	DestroyStaticMapSceneResources(
		mRuntimeMutationRebaselineCandidate.staticScene,
		mRuntimeMutationRebaselineCandidate.staticResources,
		false);
	mRuntimeMutationRebaselineCandidate.runtimeMutations = {};
	mRuntimeMutationRebaselineCandidate = {};
	for (auto& retired : mRetiredRuntimeMutationRebaselineStaticScenes)
	{
		DestroyStaticMapSceneResources(
			retired.staticScene,
			retired.staticResources,
			false);
	}
	mRetiredRuntimeMutationRebaselineStaticScenes.clear();
	ResetPersistentDynamicEmissiveCache();
	DestroyBufferResource(mStaticVertexBuffer);
	DestroyBufferResource(mStaticIndexBuffer);
	DestroyBufferResource(mStaticPrimitiveBuffer);
	DestroyBufferResource(mStaticMaterialBuffer);
	DestroyBufferResource(mVertexBuffer);
	DestroyBufferResource(mIndexBuffer);
	DestroyBufferResource(mPrimitiveBuffer);
	DestroyBufferResource(mMaterialBuffer);
	DestroyBufferResource(mTlasInstanceBuffer);
	DestroyBufferResource(mEmissiveTlasInstanceBuffer);
	DestroyBufferResource(mSceneInstanceBuffer);
	DestroyBufferResource(mPortalBuffer);
	DestroyBufferResource(mRuntimeLightBuffer);
	DestroyBufferResource(mRuntimeLightTileHeaderBuffer);
	DestroyBufferResource(mRuntimeLightTileIndexBuffer);
	DestroyBufferResource(mEmissivePrimitiveHeaderBuffer);
	DestroyBufferResource(mEmissivePrimitiveBuffer);
	DestroyBufferResource(mEmissivePrimitiveCdfBuffer);
	DestroyBufferResource(mSectorLightHeaderBuffer);
	DestroyBufferResource(mSectorLightBuffer);
	DestroyBufferResource(mReprojectionBuffer);
	DestroyBufferResource(mVisibleChunkBuffer);
	DestroyBufferResource(mVisibleFlatPlaneBuffer);
	DestroyBufferResource(mScratchBuffer);
	DestroyBufferResource(mTopLevelScratchBuffer);
	DestroyAccelerationStructureResource(mEmissiveTopLevelAS);
	for (uint8_t& initialized : mSceneDataDescriptorsInitialized)
	{
		initialized = 0u;
	}
	mSceneDataDescriptors.fill(nullptr);
	mBoundStaticPrimitiveCount = 0;
	mBoundDynamicPrimitiveCount = 0;
	mBoundStaticMaterialCount = 0;
	mBoundDynamicMaterialCount = 0;
	mBoundPortalCount = 0;
	mBoundRuntimeLightCount = 0;
	mBoundRuntimeLightTileCountX = 0;
	mBoundRuntimeLightTileCountY = 0;
	mBoundRuntimeLightTileSize = 0;
	mBoundRuntimeLightTileIndexCount = 0;
	mBoundRuntimeLightMaxTileOccupancy = 0;
	mRuntimeLightPayloadCacheValid = false;
	mRuntimeLightPayloadHash = 0;
	mRuntimeLightClusterCacheValid = false;
	mRuntimeLightClusterPayloadHash = 0;
	mRuntimeLightClusterCameraHash = 0;
	mBoundEmissivePrimitiveCount = 0;
	mBoundEmissiveDominantPrimitive = UINT32_MAX;
	mBoundEmissiveDominantTile = 0;
	mBoundEmissiveDominantFlags = 0;
	mBoundEmissiveDominantDataSource = 0;
	mEmissiveSamplingPayloadCacheValid = false;
	mEmissiveSamplingPayloadHash = 0;
	mEmissiveTlasInstanceCount = 0;
	mEmissiveTlasStaticInstanceCount = 0;
	mEmissiveTlasDynamicInstanceCount = 0;
	mEmissiveTlasBuildCount = 0;
	mEmissiveTlasInstancePayloadCacheValid = false;
	mEmissiveTlasInstancePayloadHash = 0;
	mBoundEmissiveTotalPower = 0.0f;
	mBoundEmissiveDominantPower = 0.0f;
	mBoundEmissivePrimitiveRecords.clear();
	mBoundSceneInstances.clear();
	mBoundSectorLightSectorCount = 0;
	mBoundSectorLightActiveCount = 0;
	mBoundSectorLightPulsingCount = 0;
	mBoundSectorLightDominantSector = UINT32_MAX;
	mBoundSectorLightDominantContribution = 0.0f;
	mSectorLightingPayloadCacheValid = false;
	mSectorLightingPayloadHash = 0;
}

void NRIRenderer::DestroyAccelerationStructures()
{
	mStaticMapScene.accelerationResident = false;
	for (auto& chunk : mRuntimeMutationRebaselineCandidate.staticScene.chunks)
	{
		DestroyAccelerationStructureResource(chunk.accelerationStructure);
	}
	for (auto& retired : mRetiredRuntimeMutationRebaselineStaticScenes)
	{
		for (auto& chunk : retired.staticScene.chunks)
		{
			DestroyAccelerationStructureResource(chunk.accelerationStructure);
		}
		DestroyAccelerationStructureResource(retired.staticResources.topLevelAS);
	}
	DestroyAccelerationStructureResource(mRuntimeMutationRebaselineCandidate.staticResources.topLevelAS);
	for (auto& chunk : mStaticMapScene.chunks)
	{
		DestroyAccelerationStructureResource(chunk.accelerationStructure);
	}
	DestroyAccelerationStructureResource(mDynamicBottomLevelAS);
	DestroyAccelerationStructureResource(mTopLevelAS);
	DestroyAccelerationStructureResource(mEmissiveTopLevelAS);
	mStaticAccelerationBuildSerial = 0;
	mActiveTlasInstanceCount = 0;
	mEmissiveTlasInstanceCount = 0;
	mEmissiveTlasStaticInstanceCount = 0;
	mEmissiveTlasDynamicInstanceCount = 0;
	mEmissiveTlasBuildCount = 0;
	mEmissiveTlasInstancePayloadCacheValid = false;
	mEmissiveTlasInstancePayloadHash = 0;
	SyncResidentMapChunkRegistryFromStaticScene();
}

void NRIRenderer::DestroyStaticMapSceneResources(StaticMapSceneCache& staticScene, StaticMapSceneResources& staticResources, bool waitForCommands)
{
	const bool hasResidentResources =
		!staticScene.chunks.empty() ||
		staticResources.vertexBuffer.buffer != nullptr ||
		staticResources.indexBuffer.buffer != nullptr ||
		staticResources.primitiveBuffer.buffer != nullptr ||
		staticResources.materialBuffer.buffer != nullptr ||
		staticResources.tlasInstanceBuffer.buffer != nullptr ||
		staticResources.scratchBuffer.buffer != nullptr ||
		staticResources.topLevelScratchBuffer.buffer != nullptr ||
		staticResources.topLevelAS.accelerationStructure != nullptr;
	if (waitForCommands && hasResidentResources && mFrameBuffer != nullptr)
	{
		WaitForCommandsTracked();
	}

	for (auto& chunk : staticScene.chunks)
	{
		DestroyAccelerationStructureResource(chunk.accelerationStructure);
	}

	DestroyAccelerationStructureResource(staticResources.topLevelAS);
	DestroyBufferResource(staticResources.vertexBuffer);
	DestroyBufferResource(staticResources.indexBuffer);
	DestroyBufferResource(staticResources.primitiveBuffer);
	DestroyBufferResource(staticResources.materialBuffer);
	DestroyBufferResource(staticResources.tlasInstanceBuffer);
	DestroyBufferResource(staticResources.scratchBuffer);
	DestroyBufferResource(staticResources.topLevelScratchBuffer);

	staticScene = {};
	staticResources = {};
}

void NRIRenderer::DestroyStaticMapSceneCache()
{
	ResetPersistentDynamicEmissiveCache();
	const bool hasResidentStaticSceneResources =
		!mStaticMapScene.chunks.empty() ||
		mStaticVertexBuffer.buffer != nullptr ||
		mStaticIndexBuffer.buffer != nullptr ||
		mStaticPrimitiveBuffer.buffer != nullptr ||
		mStaticMaterialBuffer.buffer != nullptr;
	if (hasResidentStaticSceneResources && mFrameBuffer != nullptr)
	{
		// The resident PT static scene can still be referenced by the previous frame's
		// TLAS and descriptor bindings. Wait before tearing it down for live rebuilds.
		WaitForCommandsTracked();
	}

	for (auto& chunk : mStaticMapScene.chunks)
	{
		DestroyAccelerationStructureResource(chunk.accelerationStructure);
	}

	DestroyBufferResource(mStaticVertexBuffer);
	DestroyBufferResource(mStaticIndexBuffer);
	DestroyBufferResource(mStaticPrimitiveBuffer);
	DestroyBufferResource(mStaticMaterialBuffer);
	mBoundStaticPrimitiveCount = 0;
	mBoundDynamicPrimitiveCount = 0;
	mBoundStaticMaterialCount = 0;
	mBoundDynamicMaterialCount = 0;
	mBoundPortalCount = 0;
	ResetStaticMapChunkAtlas(mStaticMapChunkAtlas);
	mRuntimeMapMutations.chunks.clear();
	mRuntimeMapMutations.replacedChunkMask.clear();
	mRuntimeMapLastFrame = {};
	ResetResidentMapChunkRegistry();
}

void NRIRenderer::DestroyBufferResource(NRIBufferResource& resource)
{
	if (resource.shaderView != nullptr)
	{
		mFrameBuffer->mCore.DestroyDescriptor(resource.shaderView);
		resource.shaderView = nullptr;
	}

	if (resource.buffer != nullptr)
	{
		mFrameBuffer->mCore.DestroyBuffer(resource.buffer);
		resource.buffer = nullptr;
	}

	resource.size = 0;
	resource.memorySize = 0;
	resource.usedSize = 0;
	resource.stride = 0;
	resource.memoryLocation = nri::MemoryLocation::DEVICE;
}

void NRIRenderer::DestroyAccelerationStructureResource(NRIAccelerationStructureResource& resource)
{
	if (resource.descriptor != nullptr)
	{
		mFrameBuffer->mCore.DestroyDescriptor(resource.descriptor);
		resource.descriptor = nullptr;
	}

	if (resource.accelerationStructure != nullptr)
	{
		mFrameBuffer->mRayTracing.DestroyAccelerationStructure(resource.accelerationStructure);
		resource.accelerationStructure = nullptr;
	}

	resource.memorySize = 0;
	resource.memoryLocation = nri::MemoryLocation::DEVICE;
}

bool NRIRenderer::IsMainUpscalerSupported(NRIMainUpscalerKind kind) const
{
	if (kind == NRIMainUpscalerKind::Off || mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return kind == NRIMainUpscalerKind::Off;
	}

	return mFrameBuffer->mUpscaler.IsUpscalerSupported(*mFrameBuffer->mDevice, ToMainUpscalerType(kind));
}

bool NRIRenderer::IsPostSharpenSupported(NRIPostSharpenKind kind) const
{
	if (kind == NRIPostSharpenKind::Off || mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return kind == NRIPostSharpenKind::Off;
	}

	return mFrameBuffer->mUpscaler.IsUpscalerSupported(*mFrameBuffer->mDevice, ToPostSharpenType(kind));
}

NRIMainUpscalerKind NRIRenderer::ResolveMainUpscalerKind(bool logFallback)
{
	SyncLegacyUpscalerConfig(logFallback);
	const NRIMainUpscalerKind requested = GetSelectedMainUpscalerKind();
	NRIMainUpscalerKind resolved = requested;

	switch (requested)
	{
	case NRIMainUpscalerKind::DLRR:
		if (!IsMainUpscalerSupported(NRIMainUpscalerKind::DLRR))
		{
			resolved =
				IsMainUpscalerSupported(NRIMainUpscalerKind::DLSR) ? NRIMainUpscalerKind::DLSR :
				NRIMainUpscalerKind::Off;
		}
		break;

	case NRIMainUpscalerKind::DLSR:
		if (!IsMainUpscalerSupported(NRIMainUpscalerKind::DLSR))
		{
			resolved = NRIMainUpscalerKind::Off;
		}
		break;

	default:
		break;
	}

	if (logFallback &&
		(requested != resolved) &&
		(mLastMainUpscalerRequest != (int)nri_upscaler || mLastMainUpscalerResolved != resolved))
	{
		Printf("NRI main upscaler fallback: requested %s is unavailable on %s, using %s\n",
			GetMainUpscalerName(requested),
			(const char*)nri_api,
			GetMainUpscalerName(resolved));
		mLastMainUpscalerRequest = (int)nri_upscaler;
		mLastMainUpscalerResolved = resolved;
	}

	return resolved;
}

NRIPostSharpenKind NRIRenderer::ResolvePostSharpenKind(bool logFallback)
{
	SyncLegacyUpscalerConfig(logFallback);
	const NRIPostSharpenKind requested = GetSelectedPostSharpenKind();
	NRIPostSharpenKind resolved = requested;

	if (requested == NRIPostSharpenKind::NIS && !IsPostSharpenSupported(NRIPostSharpenKind::NIS))
	{
		resolved = NRIPostSharpenKind::Off;
	}

	if (logFallback &&
		(requested != resolved) &&
		(mLastPostSharpenRequest != (int)nri_postsharpen || mLastPostSharpenResolved != resolved))
	{
		Printf("NRI post sharpen fallback: requested %s is unavailable on %s, using %s\n",
			GetPostSharpenName(requested),
			(const char*)nri_api,
			GetPostSharpenName(resolved));
		mLastPostSharpenRequest = (int)nri_postsharpen;
		mLastPostSharpenResolved = resolved;
	}

	return resolved;
}

const char* NRIRenderer::GetFrameTextureSlotName(FrameTextureSlot slot) const
{
	switch (slot)
	{
	case FrameTextureSlot::ViewZ: return "ViewZ";
	case FrameTextureSlot::Motion: return "Motion";
	case FrameTextureSlot::NormalRoughness: return "NormalRoughness";
	case FrameTextureSlot::BaseColorMetalness: return "BaseColorMetalness";
	case FrameTextureSlot::UnfilteredDiffuse: return "UnfilteredDiffuse";
	case FrameTextureSlot::UnfilteredSpecular: return "UnfilteredSpecular";
	case FrameTextureSlot::UnfilteredPenumbra: return "UnfilteredPenumbra";
	case FrameTextureSlot::DenoisedDiffuse: return "DenoisedDiffuse";
	case FrameTextureSlot::DenoisedSpecular: return "DenoisedSpecular";
	case FrameTextureSlot::DenoisedShadow: return "DenoisedShadow";
	case FrameTextureSlot::Composed: return "Composed";
	case FrameTextureSlot::TraceTransparentOutput: return "TraceTransparentOutput";
	case FrameTextureSlot::DirectLighting: return "DirectLighting";
	case FrameTextureSlot::DirectEmission: return "DirectEmission";
	case FrameTextureSlot::TaaHistoryPing: return "TaaHistoryPing";
	case FrameTextureSlot::TaaHistoryPong: return "TaaHistoryPong";
	case FrameTextureSlot::Validation: return "Validation";
	case FrameTextureSlot::SrInput: return "SrInput";
	case FrameTextureSlot::RrInput: return "RrInput";
	case FrameTextureSlot::UpscalerDepth: return "UpscalerDepth";
	case FrameTextureSlot::RrGuideDiffuseAlbedo: return "RrGuideDiffuseAlbedo";
	case FrameTextureSlot::RrGuideSpecularAlbedo: return "RrGuideSpecularAlbedo";
	case FrameTextureSlot::RrGuideSpecularHitDistance: return "RrGuideSpecularHitDistance";
	case FrameTextureSlot::RrGuideNormalRoughness: return "RrGuideNormalRoughness";
	case FrameTextureSlot::VendorOutput: return "VendorOutput";
	case FrameTextureSlot::PostSharpenOutput: return "PostSharpenOutput";
	case FrameTextureSlot::Final: return "Final";
	case FrameTextureSlot::Count: return "Count";
	default: return "Unknown";
	}
}

NRIMainUpscalerKind NRIRenderer::GetSelectedMainUpscalerKind() const
{
	SyncLegacyUpscalerConfig(false);
	switch ((int)nri_upscaler)
	{
	default:
	case 0: return NRIMainUpscalerKind::Off;
	case 2: return NRIMainUpscalerKind::DLSR;
	case 3: return NRIMainUpscalerKind::DLRR;
	}
}

NRIPostSharpenKind NRIRenderer::GetSelectedPostSharpenKind() const
{
	SyncLegacyUpscalerConfig(false);
	switch ((int)nri_postsharpen)
	{
	default:
	case 0: return NRIPostSharpenKind::Off;
	case 1: return NRIPostSharpenKind::NIS;
	}
}

NRIMainUpscalerKind NRIRenderer::GetResolvedMainUpscalerKindForStatus() const
{
	const NRIMainUpscalerKind requested = GetSelectedMainUpscalerKind();

	switch (requested)
	{
	case NRIMainUpscalerKind::DLRR:
		if (!IsMainUpscalerSupported(NRIMainUpscalerKind::DLRR))
		{
			return
				IsMainUpscalerSupported(NRIMainUpscalerKind::DLSR) ? NRIMainUpscalerKind::DLSR :
				NRIMainUpscalerKind::Off;
		}
		break;

	case NRIMainUpscalerKind::DLSR:
		if (!IsMainUpscalerSupported(NRIMainUpscalerKind::DLSR))
		{
			return NRIMainUpscalerKind::Off;
		}
		break;

	default:
		break;
	}

	return requested;
}

NRIPostSharpenKind NRIRenderer::GetResolvedPostSharpenKindForStatus() const
{
	const NRIPostSharpenKind requested = GetSelectedPostSharpenKind();
	if (requested == NRIPostSharpenKind::NIS && !IsPostSharpenSupported(NRIPostSharpenKind::NIS))
	{
		return NRIPostSharpenKind::Off;
	}

	return requested;
}

nri::UpscalerMode NRIRenderer::GetSelectedUpscalerMode() const
{
	switch ((int)nri_upscalermode)
	{
	default:
	case 0: return nri::UpscalerMode::NATIVE;
	case 1: return nri::UpscalerMode::ULTRA_QUALITY;
	case 2: return nri::UpscalerMode::QUALITY;
	case 3: return nri::UpscalerMode::BALANCED;
	case 4: return nri::UpscalerMode::PERFORMANCE;
	case 5: return nri::UpscalerMode::ULTRA_PERFORMANCE;
	}
}

void NRIRenderer::FillMatrix(float* outMatrix, const VSMatrix& matrix) const
{
	const_cast<VSMatrix&>(matrix).copy(outMatrix);
}
