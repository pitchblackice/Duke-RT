#include "nri_renderer.h"

#include "../framegen/nri_framegen.h"
#include "nri_acceleration.h"
#include "nri_debug_reporters.h"
#include "nri_frame_graph.h"
#include "nri_renderstate.h"
#include "nri_render_geometry_helpers.h"
#include "nri_renderer_settings.h"
#include "nri_scene_frame_builder.h"
#include "nri_scene_upload.h"
#include "nri_shader_contracts.h"
#include "nri_static_scene_geometry.h"
#include "nri_upload_hash.h"
#include "nri_runtime_mutation_shared.h"
#include "../scene/nri_map_builder.h"
#include "../scene/nri_scene_math.h"
#include "../scene/nri_scene_stats.h"
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
#include "gamestate.h"
#include "menustate.h"
#include "hw_sections.h"
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
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
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
CVAR(Int, nri_upscaler, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_postsharpen, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_upscalermode, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_pttaa, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_renderscale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_sharpness, 0.1375f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
EXTERN_CVAR(Bool, nri_ptscenestats)
EXTERN_CVAR(Bool, nri_ptautoexposurestats)
EXTERN_CVAR(Bool, nri_voxelstats)
EXTERN_CVAR(Bool, nri_ptslowdowntrace)
EXTERN_CVAR(Bool, nri_ptemissivelighteditmode)
EXTERN_CVAR(Float, nri_ptmirrordynamicdistance)
EXTERN_CVAR(Int, nri_pttraceframes)
EXTERN_CVAR(Int, nri_ptloadingtrace)
EXTERN_CVAR(Bool, nri_ptloadingvoxelgpu)
EXTERN_CVAR(Int, perf_looptraceframes)
CVAR(Bool, nri_ptselftest, false, 0)
CUSTOM_CVAR(Float, nri_ptemissivelighteditnotifyrange, 2048.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}
CUSTOM_CVAR(Int, nri_ptmutationworklistvalidate, 0, 0)
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
CUSTOM_CVAR(Int, nri_ptscenebufferdirtyrangegap, 256, 0)
{
	if (self < 0)
	{
		self = 0;
	}
}
CUSTOM_CVAR(Int, nri_ptscenebufferrangeuploadmaxranges, 256, 0)
{
	if (self < 1)
	{
		self = 1;
	}
}
CUSTOM_CVAR(Int, nri_ptscenebufferrangeuploadmaxpercent, 75, 0)
{
	if (self < 1)
	{
		self = 1;
	}
	else if (self > 100)
	{
		self = 100;
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

CUSTOM_CVAR(Int, nri_ptoutputmode, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Int, nri_pttonemap, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptexposure, 1.06016f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptcontrast, 1.14688f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptsaturation, 1.75f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptshoulder, 1.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_pttoe, 1.1f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptpaperwhite, 300.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Int, nri_pthdrtonemap, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_pthdrexposure, 0.986328f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_pthdrcontrast, 1.10312f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_pthdrsaturation, 1.75f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_pthdrshoulder, 0.84375f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_pthdrtoe, 1.1f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptnightvisionexposure, 1.39844f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptnightvisioncontrast, 0.971875f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptnightvisionsaturation, 1.10625f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptnightvisionred, 0.46875f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptnightvisiongreen, 1.0625f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptnightvisionblue, 0.16875f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
		const nri_scene::SceneDebugStats preservedStats = sceneView.stats;
		nri_scene::SceneDebugStats stats = {};
		stats.wallDrawItems = (uint32_t)sceneView.opaqueWalls.size();
		stats.flatDrawItems = (uint32_t)sceneView.opaqueFlats.size();
		stats.spriteDrawItems = (uint32_t)sceneView.opaqueSprites.size();

		for (const nri_scene::SurfaceRef& wall : sceneView.opaqueWalls)
		{
			stats.triangleEstimate += !wall.indices.empty() ? (uint32_t)(wall.indices.size() / 3u) : (wall.vertices.size() >= 3 ? (uint32_t)wall.vertices.size() - 2u : 0u);
			stats.materialRefs++;
			if (wall.provenance.sourceType == nri_scene::SurfaceSourceType::MirrorWall)
			{
				stats.mirrorSurfaces++;
			}
		}

		for (const nri_scene::SurfaceRef& flat : sceneView.opaqueFlats)
		{
			stats.triangleEstimate += !flat.indices.empty() ? (uint32_t)(flat.indices.size() / 3u) : (uint32_t)(flat.vertices.size() / 3u);
			stats.materialRefs++;
		}

		for (const nri_scene::SurfaceRef& sprite : sceneView.opaqueSprites)
		{
			stats.triangleEstimate += !sprite.indices.empty() ? (uint32_t)(sprite.indices.size() / 3u) : (sprite.vertices.size() >= 3 ? (uint32_t)sprite.vertices.size() - 2u : 0u);
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
		stats.voxelStableCandidates = preservedStats.voxelStableCandidates;
		stats.voxelStableUncacheable = preservedStats.voxelStableUncacheable;
		stats.voxelStableSignatureHits = preservedStats.voxelStableSignatureHits;
		stats.voxelStableSignatureMisses = preservedStats.voxelStableSignatureMisses;
		stats.voxelStableSignatureChanges = preservedStats.voxelStableSignatureChanges;
		stats.voxelStableSplitStable = preservedStats.voxelStableSplitStable;
		stats.voxelStableSplitLive = preservedStats.voxelStableSplitLive;
		stats.voxelCacheEntries = preservedStats.voxelCacheEntries;
		stats.voxelCacheSurfaceHits = preservedStats.voxelCacheSurfaceHits;
		stats.voxelCacheSurfaceStores = preservedStats.voxelCacheSurfaceStores;
		stats.voxelCacheSurfaceRebuilds = preservedStats.voxelCacheSurfaceRebuilds;
		stats.voxelCacheTransformRebakes = preservedStats.voxelCacheTransformRebakes;
		stats.voxelCacheSurfaceRemoves = preservedStats.voxelCacheSurfaceRemoves;
		stats.voxelCacheNotCaptured = preservedStats.voxelCacheNotCaptured;
		stats.voxelCachePrimitives = preservedStats.voxelCachePrimitives;
		sceneView.stats = stats;
	}

	static HWPortal* SelectPrimaryMirrorPortal(const HWDrawInfo& di, uint32_t& outCandidateCount, int32_t& outSelectedWallIndex, int32_t preferredWallIndex = -1)
	{
		outCandidateCount = 0;
		outSelectedWallIndex = -1;
		const walltype* wallData = wall.Size() > 0 ? wall.Data() : nullptr;
		const walltype* wallDataEnd = wallData != nullptr ? wallData + wall.Size() : nullptr;
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
			if (wallData == nullptr || mirrorLine < wallData || mirrorLine >= wallDataEnd)
			{
				continue;
			}
			if (!validWallIndex(mirrorLine->point2))
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

	struct MirrorExtendedAppendStats
	{
		uint32_t source = 0;
		uint32_t accepted = 0;
		uint32_t rejectedDistance = 0;
		uint32_t rejectedDuplicate = 0;
	};

	struct MirrorExtendedDrawListStats
	{
		uint32_t modelSprites = 0;
		uint32_t voxelSprites = 0;
		uint32_t facingSprites = 0;
	};

	static bool ShouldTraceMirrorDynamicCapture();

	static MirrorExtendedDrawListStats GatherMirrorExtendedDrawListStats(HWDrawInfo& di, uint32_t frameIndex)
	{
		MirrorExtendedDrawListStats stats = {};
		stats.modelSprites = di.drawlists[GLDL_MODELS].Size();
		stats.facingSprites = di.drawlists[GLDL_TRANSLUCENT].Size();

		const bool trace = ShouldTraceMirrorDynamicCapture();
		uint32_t tracedVoxelSprites = 0;
		for (auto* sprite : di.drawlists[GLDL_MODELS].sprites)
		{
			if (sprite == nullptr ||
				sprite->modelframe >= 0 ||
				sprite->voxel == nullptr ||
				sprite->voxel->model == nullptr)
			{
				continue;
			}

			stats.voxelSprites++;
			if (trace && tracedVoxelSprites < 16u)
			{
				const DCoreActor* actor = sprite->Sprite != nullptr ? sprite->Sprite->ownerActor : nullptr;
				const int actorIndex = actor != nullptr ? actor->GetIndex() : -1;
				const int statnum = sprite->Sprite != nullptr ? sprite->Sprite->statnum : -1;
				const int picnum = sprite->Sprite != nullptr ? sprite->Sprite->picnum : -1;
				Printf("PERF pt mirror voxel drawlist NRI: frame=%u rank=%u actor=%d stat=%d pic=%d tex=%d pos=(%.2f,%.2f,%.2f) alpha=%.3f voxel=%p model=%p\n",
					frameIndex,
					tracedVoxelSprites + 1u,
					actorIndex,
					statnum,
					picnum,
					sprite->texture != nullptr ? sprite->texture->GetID().GetIndex() : -1,
					(double)sprite->x,
					(double)sprite->y,
					(double)sprite->z,
					(double)sprite->alpha,
					(void*)sprite->voxel,
					sprite->voxel != nullptr ? (void*)sprite->voxel->model : nullptr);
			}
			tracedVoxelSprites++;
		}

		return stats;
	}

	static void AppendMirrorExtendedSurfaceList(
		const std::vector<nri_scene::SurfaceRef>& source,
		const FRenderViewpoint& viewpoint,
		float maxDistance,
		std::unordered_set<uint64_t>& existingKeys,
		std::vector<nri_scene::SurfaceRef>& destination,
		MirrorExtendedAppendStats& stats)
	{
		(void)viewpoint;
		(void)maxDistance;
		for (const nri_scene::SurfaceRef& surface : source)
		{
			stats.source++;
			const uint64_t key = BuildDynamicSurfaceMergeKey(surface);
			if (!existingKeys.insert(key).second)
			{
				stats.rejectedDuplicate++;
				continue;
			}

			destination.push_back(surface);
			stats.accepted++;
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
		int32_t selectedMirrorWallIndex,
		const nri_scene::SceneView* baseDynamicSceneView,
		uint32_t frameIndex,
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
		const FRenderViewpoint mirrorCaptureViewpoint = captureDi->Viewpoint;

		captureDi->CreateScene(false);
		const MirrorExtendedDrawListStats rawDrawListStats = GatherMirrorExtendedDrawListStats(*captureDi, frameIndex);
		nri_scene::SceneView capturedView;
		const bool hasCapture = nri_scene::CaptureDynamicScene(*captureDi, capturedView, nri_scene::DynamicVoxelCaptureMode::MirrorResidencyRequest);
		captureDi->EndDrawInfo();
		if (!hasCapture)
		{
			if (ShouldTraceMirrorDynamicCapture())
			{
				Printf("PERF pt mirror capture NRI: frame=%u result=empty wall=%d distance=%.1f raw_models=%u raw_voxels=%u raw_facing=%u captured_walls=0 captured_flats=0 captured_sprites=0 accepted_walls=0 accepted_flats=0 accepted_sprites=0 dist_rejects=0 duplicate_rejects=0 voxel_candidates=0 voxel_hits=0 voxel_misses=0 voxel_not_captured=0\n",
					frameIndex,
					selectedMirrorWallIndex,
					(double)nri_ptmirrordynamicdistance,
					rawDrawListStats.modelSprites,
					rawDrawListStats.voxelSprites,
					rawDrawListStats.facingSprites);
			}
			return false;
		}

		std::unordered_set<uint64_t> existingKeys;
		if (baseDynamicSceneView != nullptr)
		{
			SeedDynamicSurfaceMergeKeys(*baseDynamicSceneView, existingKeys);
		}

		outView.drawInfo = &di;
		MirrorExtendedAppendStats wallAppendStats = {};
		MirrorExtendedAppendStats flatAppendStats = {};
		MirrorExtendedAppendStats spriteAppendStats = {};
		AppendMirrorExtendedSurfaceList(
			capturedView.opaqueWalls,
			mirrorCaptureViewpoint,
			nri_ptmirrordynamicdistance,
			existingKeys,
			outView.opaqueWalls,
			wallAppendStats);
		AppendMirrorExtendedSurfaceList(
			capturedView.opaqueFlats,
			mirrorCaptureViewpoint,
			nri_ptmirrordynamicdistance,
			existingKeys,
			outView.opaqueFlats,
			flatAppendStats);
		AppendMirrorExtendedSurfaceList(
			capturedView.opaqueSprites,
			mirrorCaptureViewpoint,
			nri_ptmirrordynamicdistance,
			existingKeys,
			outView.opaqueSprites,
			spriteAppendStats);
		if (ShouldTraceMirrorDynamicCapture())
		{
			Printf("PERF pt mirror capture NRI: frame=%u result=%s wall=%d distance=%.1f raw_models=%u raw_voxels=%u raw_facing=%u captured_walls=%u captured_flats=%u captured_sprites=%u accepted_walls=%u accepted_flats=%u accepted_sprites=%u dist_rejects=%u duplicate_rejects=%u voxel_candidates=%u voxel_hits=%u voxel_misses=%u voxel_not_captured=%u\n",
				frameIndex,
				(outView.opaqueWalls.empty() && outView.opaqueFlats.empty() && outView.opaqueSprites.empty()) ? "filtered" : "accepted",
				selectedMirrorWallIndex,
				(double)nri_ptmirrordynamicdistance,
				rawDrawListStats.modelSprites,
				rawDrawListStats.voxelSprites,
				rawDrawListStats.facingSprites,
				(uint32_t)capturedView.opaqueWalls.size(),
				(uint32_t)capturedView.opaqueFlats.size(),
				(uint32_t)capturedView.opaqueSprites.size(),
				wallAppendStats.accepted,
				flatAppendStats.accepted,
				spriteAppendStats.accepted,
				wallAppendStats.rejectedDistance + flatAppendStats.rejectedDistance + spriteAppendStats.rejectedDistance,
				wallAppendStats.rejectedDuplicate + flatAppendStats.rejectedDuplicate + spriteAppendStats.rejectedDuplicate,
				capturedView.stats.voxelStableCandidates,
				capturedView.stats.voxelCacheSurfaceHits,
				capturedView.stats.voxelStableSignatureMisses,
				capturedView.stats.voxelCacheNotCaptured);
		}
		if (outView.opaqueWalls.empty() && outView.opaqueFlats.empty() && outView.opaqueSprites.empty())
		{
			outView = {};
			return false;
		}

		outView.primitiveFlags = nri_scene::PrimitiveFlag_ReflectionOnly;
		RebuildSceneViewStats(outView);
		return true;
	}

	static bool CaptureMirrorPlayerDynamicScene(
		HWDrawInfo& di,
		HWPortal* mirrorPortal,
		int32_t selectedMirrorWallIndex,
		uint32_t mirrorPortalCandidates,
		nri_scene::SceneView& outView,
		MirrorPlayerCaptureStats* outStats = nullptr)
	{
		outView = {};
		MirrorPlayerCaptureStats captureStats = {};
		captureStats.viewpointActorIndex = di.Viewpoint.CameraActor != nullptr ? (int32_t)di.Viewpoint.CameraActor->GetIndex() : -1;
		const auto publishStats = [&]()
		{
			if (outStats != nullptr)
			{
				*outStats = captureStats;
			}
			TraceMirrorPlayerCaptureStats(captureStats);
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
			publishStats();
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
		publishStats();
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

	static bool ShouldTraceResidentGeometryOrderHash()
	{
		return (int)perf_looptraceframes > 0;
	}

	static bool ShouldTraceSceneBufferDirtyRanges()
	{
		return (int)perf_looptraceframes > 0;
	}

	static bool ShouldTraceMirrorDynamicCapture()
	{
		return nri_voxelstats || (int)nri_pttraceframes > 0 || (int)perf_looptraceframes > 0;
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

	static uint64_t HashUploadPayloadBytes(const void* data, uint64_t size)
	{
		return NRIHashUploadPayloadBytes(data, size);
	}

	static uint64_t HashMaterialPayloadData(const nri_scene::MaterialBridgeData& materialBridge)
	{
		const uint64_t materialSize = (uint64_t)materialBridge.materials.size() * sizeof(nri_scene::MaterialData);
		return HashUploadPayloadBytes(
			materialBridge.materials.empty() ? nullptr : materialBridge.materials.data(),
			materialSize);
	}

	struct SceneViewUploadStampBuildResult
	{
		uint64_t vertexPayloadStamp = 0;
		uint64_t indexPayloadStamp = 0;
		uint64_t primitivePayloadStamp = 0;
		uint64_t primitiveProvenanceStamp = 0;
		uint64_t materialPayloadStamp = 0;
	};

	static uint64_t HashSurfaceProvenanceStamp(uint64_t hash, const nri_scene::SurfaceProvenance& provenance)
	{
		hash = CoherencyHashCombine64(hash, (uint64_t)(uint32_t)provenance.sourceType);
		hash = CoherencyHashCombine64(hash, (uint64_t)(uint32_t)(provenance.sectorIndex + 1));
		hash = CoherencyHashCombine64(hash, (uint64_t)(uint32_t)(provenance.wallIndex + 1));
		hash = CoherencyHashCombine64(hash, (uint64_t)(uint32_t)(provenance.sectionIndex + 1));
		hash = CoherencyHashCombine64(hash, (uint64_t)(uint32_t)(provenance.mapChunkIndex + 1));
		hash = CoherencyHashCombine64(hash, (uint64_t)(uint32_t)(provenance.nextSectorIndex + 1));
		hash = CoherencyHashCombine64(hash, (uint64_t)(uint32_t)(provenance.actorIndex + 1));
		hash = CoherencyHashCombine64(hash, (uint64_t)provenance.drawListType);
		hash = CoherencyHashCombine64(hash, (uint64_t)provenance.cstat);
		hash = CoherencyHashCombine64(hash, (uint64_t)provenance.materialFlags);
		return hash;
	}

	static uint64_t HashCapturedVertexStamp(uint64_t hash, const nri_scene::CapturedVertex& vertex)
	{
		for (int i = 0; i < 3; ++i)
		{
			hash = CoherencyHashCombine64(hash, CoherencyFloatBits(vertex.position[i]));
		}
		for (int i = 0; i < 3; ++i)
		{
			hash = CoherencyHashCombine64(hash, CoherencyFloatBits(vertex.prevPosition[i]));
		}
		hash = CoherencyHashCombine64(hash, CoherencyFloatBits(vertex.uv[0]));
		hash = CoherencyHashCombine64(hash, CoherencyFloatBits(vertex.uv[1]));
		return hash;
	}

	static uint64_t HashMaterialRefStamp(uint64_t hash, const nri_scene::MaterialRef& material)
	{
		hash = CoherencyHashCombine64(hash, material.texture != nullptr ? (uint64_t)(uint32_t)material.texture->GetID().GetIndex() + 1ull : 0ull);
		hash = CoherencyHashCombine64(hash, material.emissiveSourceTexture != nullptr ? (uint64_t)(uint32_t)material.emissiveSourceTexture->GetID().GetIndex() + 1ull : 0ull);
		hash = CoherencyHashCombine64(hash, (uint64_t)(uint32_t)(material.palette + 1));
		hash = CoherencyHashCombine64(hash, (uint64_t)(uint32_t)(material.shade + 1));
		hash = CoherencyHashCombine64(hash, CoherencyFloatBits(material.alpha));
		hash = CoherencyHashCombine64(hash, (uint64_t)material.flags);
		return hash;
	}

	static uint32_t CountStampedSurfacePrimitives(const nri_scene::SurfaceRef& surface, bool triangleList)
	{
		if (!surface.indices.empty())
		{
			return (uint32_t)(surface.indices.size() / 3u);
		}
		if (triangleList)
		{
			return (uint32_t)(surface.vertices.size() / 3u);
		}
		return surface.vertices.size() >= 3 ? (uint32_t)surface.vertices.size() - 2u : 0u;
	}

	static SceneViewUploadStampBuildResult BuildSceneViewUploadProducerStamp(const nri_scene::SceneView& sceneView, uint64_t mapWorldBuildSerial)
	{
		SceneViewUploadStampBuildResult result = {};
		result.vertexPayloadStamp = 1469598103934665603ull;
		result.indexPayloadStamp = 1469598103934665603ull;
		result.primitivePayloadStamp = 1469598103934665603ull;
		result.primitiveProvenanceStamp = 1469598103934665603ull;
		result.materialPayloadStamp = 1469598103934665603ull;
		auto appendSurface =
			[&](const nri_scene::SurfaceRef& surface, uint32_t surfaceKind, bool triangleList, uint32_t materialIndex)
		{
			const uint32_t primitiveCount = CountStampedSurfacePrimitives(surface, triangleList);
			const uint64_t surfaceHeader =
				CoherencyHashCombine64(
					CoherencyHashCombine64(
						CoherencyHashCombine64(1469598103934665603ull, (uint64_t)surfaceKind),
						(uint64_t)materialIndex),
					(uint64_t)primitiveCount);
			result.vertexPayloadStamp = CoherencyHashCombine64(result.vertexPayloadStamp, surfaceHeader);
			result.indexPayloadStamp = CoherencyHashCombine64(result.indexPayloadStamp, surfaceHeader);
			result.primitivePayloadStamp = CoherencyHashCombine64(result.primitivePayloadStamp, surfaceHeader);
			result.primitiveProvenanceStamp = CoherencyHashCombine64(result.primitiveProvenanceStamp, surfaceHeader);
			result.materialPayloadStamp = CoherencyHashCombine64(result.materialPayloadStamp, surfaceHeader);
			result.vertexPayloadStamp = CoherencyHashCombine64(result.vertexPayloadStamp, (uint64_t)surface.vertices.size());
			result.indexPayloadStamp = CoherencyHashCombine64(result.indexPayloadStamp, (uint64_t)surface.indices.size());
			result.primitivePayloadStamp = CoherencyHashCombine64(result.primitivePayloadStamp, (uint64_t)sceneView.primitiveFlags);
			result.primitivePayloadStamp = CoherencyHashCombine64(result.primitivePayloadStamp, (uint64_t)surface.material.flags);
			result.primitivePayloadStamp = CoherencyHashCombine64(result.primitivePayloadStamp, mapWorldBuildSerial);
			result.primitiveProvenanceStamp = HashSurfaceProvenanceStamp(result.primitiveProvenanceStamp, surface.provenance);
			result.materialPayloadStamp = HashMaterialRefStamp(result.materialPayloadStamp, surface.material);
			for (const nri_scene::CapturedVertex& vertex : surface.vertices)
			{
				result.vertexPayloadStamp = HashCapturedVertexStamp(result.vertexPayloadStamp, vertex);
				result.primitivePayloadStamp = HashCapturedVertexStamp(result.primitivePayloadStamp, vertex);
			}
			for (uint32_t index : surface.indices)
			{
				result.indexPayloadStamp = CoherencyHashCombine64(result.indexPayloadStamp, (uint64_t)index);
				result.primitivePayloadStamp = CoherencyHashCombine64(result.primitivePayloadStamp, (uint64_t)index);
			}
		};

		uint32_t materialIndex = 0;
		for (const nri_scene::SurfaceRef& surface : sceneView.opaqueWalls)
		{
			appendSurface(surface, 0u, false, materialIndex++);
		}
		for (const nri_scene::SurfaceRef& surface : sceneView.opaqueFlats)
		{
			appendSurface(surface, 1u, true, materialIndex++);
		}
		for (const nri_scene::SurfaceRef& surface : sceneView.opaqueSprites)
		{
			appendSurface(surface, 2u, false, materialIndex++);
		}
		result.vertexPayloadStamp = CoherencyHashCombine64(result.vertexPayloadStamp, (uint64_t)materialIndex);
		result.indexPayloadStamp = CoherencyHashCombine64(result.indexPayloadStamp, (uint64_t)materialIndex);
		result.primitivePayloadStamp = CoherencyHashCombine64(result.primitivePayloadStamp, (uint64_t)materialIndex);
		result.primitiveProvenanceStamp = CoherencyHashCombine64(result.primitiveProvenanceStamp, (uint64_t)materialIndex);
		result.materialPayloadStamp = CoherencyHashCombine64(result.materialPayloadStamp, (uint64_t)materialIndex);
		result.vertexPayloadStamp = result.vertexPayloadStamp != 0 ? result.vertexPayloadStamp : 1;
		result.indexPayloadStamp = result.indexPayloadStamp != 0 ? result.indexPayloadStamp : 1;
		result.primitivePayloadStamp = result.primitivePayloadStamp != 0 ? result.primitivePayloadStamp : 1;
		result.primitiveProvenanceStamp = result.primitiveProvenanceStamp != 0 ? result.primitiveProvenanceStamp : 1;
		result.materialPayloadStamp = result.materialPayloadStamp != 0 ? result.materialPayloadStamp : 1;
		return result;
	}

	static uint64_t BuildConservativeMirrorPlayerPayloadStamp(
		uint64_t kind,
		uint64_t frameIndex,
		const nri_scene::GeometryData& geometry,
		const nri_scene::MaterialBridgeData& materials,
		uint64_t mapWorldBuildSerial)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = CoherencyHashCombine64(hash, kind);
		hash = CoherencyHashCombine64(hash, frameIndex);
		hash = CoherencyHashCombine64(hash, mapWorldBuildSerial);
		hash = CoherencyHashCombine64(hash, (uint64_t)geometry.vertices.size());
		hash = CoherencyHashCombine64(hash, (uint64_t)geometry.indices.size());
		hash = CoherencyHashCombine64(hash, (uint64_t)geometry.primitives.size());
		hash = CoherencyHashCombine64(hash, (uint64_t)geometry.primitiveProvenance.size());
		hash = CoherencyHashCombine64(hash, (uint64_t)materials.materials.size());
		return hash != 0 ? hash : 1;
	}

	static SceneViewUploadStampBuildResult BuildMirrorPlayerUploadProducerStamp(
		const nri_scene::GeometryData& geometry,
		const nri_scene::MaterialBridgeData& materials,
		uint64_t frameIndex,
		uint64_t mapWorldBuildSerial)
	{
		SceneViewUploadStampBuildResult stamp = {};
		stamp.vertexPayloadStamp = BuildConservativeMirrorPlayerPayloadStamp(1u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.indexPayloadStamp = BuildConservativeMirrorPlayerPayloadStamp(2u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.primitivePayloadStamp = BuildConservativeMirrorPlayerPayloadStamp(3u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.primitiveProvenanceStamp = BuildConservativeMirrorPlayerPayloadStamp(4u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.materialPayloadStamp = HashMaterialPayloadData(materials);
		return stamp;
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

	static bool SceneViewHasSectorDrivenWallBands(const nri_scene::SceneView& sceneView)
	{
		for (const nri_scene::SurfaceRef& surface : sceneView.opaqueWalls)
		{
			if (surface.provenance.sourceType == nri_scene::SurfaceSourceType::MapWallBand)
			{
				return true;
			}
		}

		return false;
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

	static bool IsSupportedSurfaceLightRule(const ResolvedLightOverlaySurfaceLightRule& rule)
	{
		return rule.lightType.IsEmpty() || rule.lightType.CompareNoCase("point") == 0 || rule.lightType.CompareNoCase("rect") == 0;
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

	static uint32_t BuildEmissiveOverrideRuleId(const ResolvedLightOverlayEmissiveOverrideRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.mapName.GetChars(), rule.source);
	}

	static uint32_t BuildSurfaceLightRuleId(const ResolvedLightOverlaySurfaceLightRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.mapName.GetChars(), rule.source);
	}

	static std::string NormalizeLightOverlayTextureSelector(const char* value)
	{
		std::string normalized = value != nullptr ? value : "";
		for (char& c : normalized)
		{
			c = (char)std::tolower((unsigned char)c);
		}

		const size_t slash = normalized.find_last_of("/\\");
		const size_t dot = normalized.find_last_of('.');
		if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
		{
			normalized.erase(dot);
		}
		return normalized;
	}

	static uint64_t BuildMapOverlayStableKey(uint32_t ruleId, const float position[3])
	{
		uint64_t key = 1469598103934665603ull;
		key = HashCombineLightOverlay(key, (uint64_t)ruleId);
		key = HashCombineLightOverlay(key, QuantizeLightOverlayPositionKey(position));
		return key;
	}

	static void ConvertMapOverlayWorldVectorToPathTracing(const float source[3], float destination[3])
	{
		destination[0] = source[0];
		destination[1] = -source[2];
		destination[2] = -source[1];
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
			ConvertMapOverlayWorldVectorToPathTracing(rule.anchorPosition, outPosition);
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
				actorRule.flags = (!resolvedRule.hasShadowCast || resolvedRule.shadowCast) ? SceneAnalyticLightFlag_CastsShadow : SceneAnalyticLightFlag_None;
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
			float offset[3] = {};
			ConvertMapOverlayWorldVectorToPathTracing(resolvedRule.offset, offset);
			overlayRule.ruleId = BuildMapOverlayRuleId(resolvedRule);
			overlayRule.source = SceneLightRecordSource::StaticMapScene;
			overlayRule.position[0] = anchorPosition[0] + offset[0];
			overlayRule.position[1] = anchorPosition[1] + offset[1];
			overlayRule.position[2] = anchorPosition[2] + offset[2];
			overlayRule.stableKey = BuildMapOverlayStableKey(overlayRule.ruleId, overlayRule.position);
			overlayRule.color[0] = resolvedRule.color[0];
			overlayRule.color[1] = resolvedRule.color[1];
			overlayRule.color[2] = resolvedRule.color[2];
			overlayRule.intensity = resolvedRule.intensity;
			overlayRule.radius = resolvedRule.radius;
			overlayRule.flickerFrames = resolvedRule.flickerFrames;
			outRules.push_back(overlayRule);
		}

		for (const auto& resolvedRule : resolved.surfaceLightRules)
		{
			if (!IsSupportedSurfaceLightRule(resolvedRule) ||
				!resolvedRule.hasPosition ||
				!resolvedRule.hasNormal ||
				resolvedRule.intensity <= 0.0f ||
				resolvedRule.radius <= 0.0f)
			{
				continue;
			}

			SceneLightSystem::AnalyticLightRegistry::MapOverlayRule overlayRule = {};
			const float offset = resolvedRule.hasOffset ? resolvedRule.offset : 0.0f;
			overlayRule.ruleId = BuildSurfaceLightRuleId(resolvedRule);
			overlayRule.source = SceneLightRecordSource::DynamicScene;
			overlayRule.position[0] = resolvedRule.position[0] + resolvedRule.normal[0] * offset;
			overlayRule.position[1] = resolvedRule.position[1] + resolvedRule.normal[1] * offset;
			overlayRule.position[2] = resolvedRule.position[2] + resolvedRule.normal[2] * offset;
			overlayRule.stableKey = BuildMapOverlayStableKey(overlayRule.ruleId, overlayRule.position);
			overlayRule.color[0] = resolvedRule.color[0];
			overlayRule.color[1] = resolvedRule.color[1];
			overlayRule.color[2] = resolvedRule.color[2];
			overlayRule.intensity = resolvedRule.intensity;
			overlayRule.radius = resolvedRule.radius;
			overlayRule.hasSectorResponse = resolvedRule.hasSectorResponse;
			overlayRule.sectorResponse = resolvedRule.sectorResponse;
			overlayRule.hasSignalSector = resolvedRule.hasSignalSector;
			overlayRule.signalSector = resolvedRule.signalSector;
			overlayRule.hasResponseIntensity = resolvedRule.hasResponseIntensity;
			overlayRule.responseIntensity = resolvedRule.responseIntensity;
			overlayRule.hasResponseMin = resolvedRule.hasResponseMin;
			overlayRule.responseMin = resolvedRule.responseMin;
			overlayRule.hasResponseMax = resolvedRule.hasResponseMax;
			overlayRule.responseMax = resolvedRule.responseMax;
			overlayRule.hasResponseInputMin = resolvedRule.hasResponseInputMin;
			overlayRule.responseInputMin = resolvedRule.responseInputMin;
			overlayRule.hasResponseInputMax = resolvedRule.hasResponseInputMax;
			overlayRule.responseInputMax = resolvedRule.responseInputMax;
			outRules.push_back(overlayRule);
		}
	}

	static void BuildEmissiveOverrideRules(
		const ResolvedLightOverlaySet& resolved,
		std::vector<SceneLightSystem::EmissiveOverrideRule>& outRules)
	{
		outRules.clear();
		outRules.reserve((size_t)resolved.emissiveOverrideRules.Size());
		for (const auto& resolvedRule : resolved.emissiveOverrideRules)
		{
			if (!resolvedRule.hasSectorFilter &&
				!resolvedRule.hasWallFilter &&
				!resolvedRule.hasTileFilter)
			{
				continue;
			}

			SceneLightSystem::EmissiveOverrideRule rule = {};
			rule.ruleId = BuildEmissiveOverrideRuleId(resolvedRule);
			rule.hasSectorFilter = resolvedRule.hasSectorFilter;
			rule.sectorFilter = resolvedRule.sectorFilter;
			rule.hasWallFilter = resolvedRule.hasWallFilter;
			rule.wallFilter = resolvedRule.wallFilter;
			rule.hasTileFilter = resolvedRule.hasTileFilter && resolvedRule.tileFilter >= 0;
			rule.tileFilter = rule.hasTileFilter ? (uint32_t)resolvedRule.tileFilter : 0u;
			rule.hasIntensityScale = resolvedRule.hasIntensityScale;
			rule.intensityScale = resolvedRule.intensityScale;
			rule.hasReachScale = resolvedRule.hasReachScale;
			rule.reachScale = resolvedRule.reachScale;
			rule.hasSectorResponse = resolvedRule.hasSectorResponse;
			rule.sectorResponse = resolvedRule.sectorResponse;
			rule.hasSignalSector = resolvedRule.hasSignalSector && resolvedRule.signalSector >= 0;
			rule.signalSector = rule.hasSignalSector ? resolvedRule.signalSector : -1;
			rule.hasResponseIntensity = resolvedRule.hasResponseIntensity;
			rule.responseIntensity = resolvedRule.responseIntensity;
			rule.hasResponseMin = resolvedRule.hasResponseMin;
			rule.responseMin = resolvedRule.responseMin;
			rule.hasResponseMax = resolvedRule.hasResponseMax;
			rule.responseMax = resolvedRule.responseMax;
			rule.hasResponseInputMin = resolvedRule.hasResponseInputMin;
			rule.responseInputMin = resolvedRule.responseInputMin;
			rule.hasResponseInputMax = resolvedRule.hasResponseInputMax;
			rule.responseInputMax = resolvedRule.responseInputMax;
			rule.hasResponseIntensityMin = resolvedRule.hasResponseIntensityMin;
			rule.responseIntensityMin = resolvedRule.responseIntensityMin;
			rule.hasResponseIntensityMax = resolvedRule.hasResponseIntensityMax;
			rule.responseIntensityMax = resolvedRule.responseIntensityMax;
			rule.hasResponseReachMin = resolvedRule.hasResponseReachMin;
			rule.responseReachMin = resolvedRule.responseReachMin;
			rule.hasResponseReachMax = resolvedRule.hasResponseReachMax;
			rule.responseReachMax = resolvedRule.responseReachMax;
			rule.hasMaterialResponse = resolvedRule.hasMaterialResponse;
			rule.materialResponse = resolvedRule.materialResponse;
			rule.hasMaterialResponseMin = resolvedRule.hasMaterialResponseMin;
			rule.materialResponseMin = resolvedRule.materialResponseMin;
			rule.hasMaterialResponseMax = resolvedRule.hasMaterialResponseMax;
			rule.materialResponseMax = resolvedRule.materialResponseMax;
			outRules.push_back(rule);
		}
	}

	static void BuildEmissiveMaterialResponseRules(
		const ResolvedLightOverlaySet& resolved,
		std::vector<SceneLightSystem::EmissiveMaterialResponseRule>& outRules)
	{
		outRules.clear();
		outRules.reserve((size_t)resolved.emissiveMaterialResponseRules.Size());
		for (const auto& resolvedRule : resolved.emissiveMaterialResponseRules)
		{
			SceneLightSystem::EmissiveMaterialResponseRule rule = {};
			rule.ruleId = BuildResolvedLightOverlayRuleId(resolvedRule.id.GetChars(), "", resolvedRule.source);
			rule.textureIds.reserve((size_t)resolvedRule.tileFilters.Size() + (size_t)resolvedRule.textureNames.Size());
			for (int tile : resolvedRule.tileFilters)
			{
				if (tile >= 0)
				{
					rule.textureIds.push_back((uint32_t)tile);
				}
			}
			rule.textureRanges.reserve((size_t)resolvedRule.tileRanges.Size());
			for (const auto& range : resolvedRule.tileRanges)
			{
				if (range.first >= 0 && range.last >= 0)
				{
					rule.textureRanges.emplace_back((uint32_t)range.first, (uint32_t)range.last);
				}
			}
			for (const auto& textureName : resolvedRule.textureNames)
			{
				rule.textureNames.push_back(NormalizeLightOverlayTextureSelector(textureName.GetChars()));
			}
			if (rule.textureIds.empty() && rule.textureRanges.empty() && rule.textureNames.empty())
			{
				continue;
			}
			rule.hasMaterialResponse = resolvedRule.hasMaterialResponse;
			rule.materialResponse = resolvedRule.materialResponse;
			rule.hasMaterialResponseMin = resolvedRule.hasMaterialResponseMin;
			rule.materialResponseMin = resolvedRule.materialResponseMin;
			rule.hasMaterialResponseMax = resolvedRule.hasMaterialResponseMax;
			rule.materialResponseMax = resolvedRule.materialResponseMax;
			outRules.push_back(rule);
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

CUSTOM_CVAR(Int, nri_ptspherelongs, 256, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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

CUSTOM_CVAR(Int, nri_ptspherelats, 128, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Int, nri_framegenui, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CVAR(Bool, nri_framegenasync, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Bool, nri_framegenlatency, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	RefreshActiveFrameGenerationSwapChain();
}
CVAR(Int, nri_nrdmaxframes, 13, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_nrdfastframes, 20, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_nrdstabilizationframes, 48, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_nrdantifirefly, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_nrdhitdistrecon, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_nrdsplit, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_nrdfasthistorysigma, 2.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_nrdprepassdiffuse, 3.45f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_nrdprepassspecular, 3.675f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_nrdblurmin, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_nrdblurmax, 2.025f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_nrdsigmastabilization, 5, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_nrdsigmaplanedistance, 0.0418375f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_apivalidation, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_dred, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptbootstrap, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptbootstrapmode, 13, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptdirectscene, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptdirectionallight, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptbaseambient, 0.021875f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptmetalambient, 0.03125f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptlightbounces, 4, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptmirrorbounces, 8, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Float, nri_ptmirrordynamicdistance, 2048.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}
CVAR(Int, nri_ptsurfaceprobe, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_pttemporaltrace, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptscenestats, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptceilingnudge, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Float, nri_ptceilingnudgedistance, 0.01f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}
CVAR(Int, nri_ptmutationtracechunk, 66, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptmutationtracesector, 198, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptruntimelinktrace, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptemissiveminpower, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptemissiveminsurface, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Float, nri_ptglowscale, 3.025f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	NotifyActiveGlowControlChange();
}
CUSTOM_CVAR(Float, nri_ptglowreach, 16.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	NotifyActiveGlowControlChange();
}
CUSTOM_CVAR(Float, nri_ptglowfalloff, 0.847656f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptglowblend, 0.20625f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 3.0f)
	{
		self = 3.0f;
	}
	NotifyActiveGlowControlChange();
}
CUSTOM_CVAR(Float, nri_voxelemissionboost, 3.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	NotifyActiveMaterialLightingCalibrationChange();
}
CUSTOM_CVAR(Float, nri_ptfullbrightboost, 1.50781f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptskybrightness, 0.15f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 4.0f)
	{
		self = 4.0f;
	}
}
CVAR(Bool, nri_ptemissivetlas, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptemissivefastshadow, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptemissivesamples, 4, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptsectorlighting, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Float, nri_ptsectorlightmultiplier, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptsectoremissionsignalstrength, 4.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}
CUSTOM_CVAR(Float, nri_ptsectoremissionresponsemin, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}
CUSTOM_CVAR(Float, nri_ptsectoremissionresponsemax, 2.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}
CUSTOM_CVAR(Float, nri_ptsectoremissionlightmin, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}
CUSTOM_CVAR(Float, nri_ptsectoremissionlightmax, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}
CUSTOM_CVAR(Float, nri_ptsectoremissionreachmin, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}
CUSTOM_CVAR(Float, nri_ptsectoremissionreachmax, 1.6f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}
CUSTOM_CVAR(Float, nri_ptsectoremissionmaterialmin, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}
CUSTOM_CVAR(Float, nri_ptsectoremissionmaterialmax, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}
CVAR(Bool, nri_ptvisiblechunkgate, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptshaderstats, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
EXTERN_CVAR(String, nri_api)
EXTERN_CVAR(Int, nri_ptportaldepth)
EXTERN_CVAR(Int, nri_pttraceframes)

namespace
{
	constexpr uint32_t NRI_MAX_SCENE_TEXTURES = 512;
	constexpr uint32_t NRI_SCENE_DESCRIPTOR_NUM = 2 + NRI_MAX_SCENE_TEXTURES;
	constexpr uint32_t NRI_SCENE_DATA_DESCRIPTOR_NUM = 26;
	constexpr uint32_t NRI_INPUT_DESCRIPTOR_NUM = 14;
	constexpr uint32_t NRI_OUTPUT_DESCRIPTOR_NUM = 15;
	constexpr uint32_t NRI_TRACE_SHADER_STATS_DESCRIPTOR_NUM = 1;
	constexpr uint32_t NRI_TRACE_SHADER_STATS_COUNTER_COUNT = NRIRenderer::TraceShaderStatCount;
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
	constexpr uint32_t NRI_SCENE_DATA_SOURCE_PERSISTENT_VOXEL = 2;
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
	constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_AUTO_EXPOSURE = 0x10u;
	constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_EXPOSURE_TEXTURE_VALID = 0x20u;
	constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_INPUT_PRE_EXPOSED = 0x40u;
	constexpr uint32_t NRI_TEMPORAL_FLAG_AUTO_EXPOSURE = 0x1000u;
	constexpr uint32_t NRI_TEMPORAL_FLAG_EXPOSURE_TEXTURE_VALID = 0x2000u;
	constexpr uint32_t NRI_FLAG_SPLIT_SHADOW_DENOISER = 0x20u;
	constexpr uint32_t NRI_FLAG_USE_JITTER = 0x40u;
	constexpr uint32_t NRI_FLAG_DIRECTIONAL_LIGHT = 0x80u;
	constexpr uint32_t NRI_FLAG_FAST_EMISSIVE_SHADOW = 0x100u;
	constexpr uint32_t NRI_FLAG_GATE_PRIMARY_VISIBLE_CHUNKS = 0x200u;
	constexpr uint32_t NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW = 0x400u;
	constexpr uint32_t NRI_FLAG_TRACE_SHADER_STATS = 0x800u;
	constexpr uint32_t NRI_JITTER_PHASE_SHIFT = 16u;
	constexpr int NRI_TEMPORAL_TRACE_REARM_FRAME_COUNT = 8;
	constexpr uint32_t NRI_TAA_JITTER_PHASE_COUNT = 8;
	constexpr float NRI_TAA_EXPOSURE_RESET_THRESHOLD_STOPS = 0.5f;
	constexpr uint32_t NRI_PORTAL_FLAG_RUNTIME_BOUND = 0x1u;

	constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_NONE = 0u;
	constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_REFLECTIVE = 1u;
	constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER = 2u;
	constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_RUNTIME_BOUND = 3u;
	constexpr uint32_t NRI_SECTOR_LIGHTING_FLAG_ENABLED = 0x1u;


	bool ShouldTracePtPerf()
	{
		return PerfLoopTraceActive() || ShouldEmitRendererTemporalTraceLogs();
	}

	bool ShouldCollectPtPerfTiming()
	{
		return ShouldTracePtPerf() || (bool)nri_ptslowdowntrace;
	}

	bool ShouldCollectTraceShaderStats()
	{
		return !!nri_ptshaderstats && ShouldTracePtPerf();
	}

	uint32_t ScoreRuntimeSectorDirtyTruthEntry(const NRIRenderer::RuntimeSectorDirtyTruthTraceEntry& entry)
	{
		uint32_t score = 0;
		if (entry.forceTopology)
		{
			score += 1u << 18;
		}
		if (entry.baselineChanged)
		{
			score += 1u << 17;
		}
		if (entry.geometryChanged)
		{
			score += 1u << 16;
		}
		if (entry.materialChanged)
		{
			score += 1u << 15;
		}
		score += entry.liveTriangleCount * 8u;
		score += entry.liveSurfaceCount * 4u;
		return score;
	}

	uint32_t ScoreRuntimeAnimatedChurnTraceEntry(const NRIRenderer::RuntimeAnimatedChurnTraceEntry& entry)
	{
		uint32_t score = entry.materialRefreshes * 96u +
			entry.runtimeAttempts * 64u +
			entry.residentApplies * 48u +
			entry.syncSkips * 32u +
			entry.suppressionEmits * 16u;
		if (entry.suppressed)
		{
			score += 1u << 20;
		}
		return score;
	}

	uint32_t ScoreRuntimeMaterialOnlyMismatchTraceEntry(const NRIRenderer::RuntimeMaterialOnlyMismatchTraceEntry& entry)
	{
		const uint32_t materialDelta =
			entry.residentMaterialCount > entry.filteredMaterialCount ?
			entry.residentMaterialCount - entry.filteredMaterialCount :
			entry.filteredMaterialCount - entry.residentMaterialCount;
		uint32_t score = materialDelta * 128u;
		score += entry.residentMaterialCount * 16u;
		score += entry.filteredSurfaceCount * 8u;
		score += (entry.filteredWallCount + entry.filteredFlatCount) * 4u;
		if (entry.filteredWallCount != 0 && entry.filteredFlatCount != 0)
		{
			score += 1u << 18;
		}
		if (entry.residentWallCount != 0 && entry.residentFlatCount != 0)
		{
			score += 1u << 17;
		}
		return score;
	}

	enum RuntimeResidentBlasRecreateFallbackBits : uint32_t
	{
		RuntimeResidentBlasRecreateFallback_NoPreviousAs = 1u << 0,
		RuntimeResidentBlasRecreateFallback_RecoveredEmpty = 1u << 1,
		RuntimeResidentBlasRecreateFallback_SliceMoved = 1u << 2,
		RuntimeResidentBlasRecreateFallback_TopologyChanged = 1u << 3,
		RuntimeResidentBlasRecreateFallback_ForceTopology = 1u << 4,
	};

	uint32_t ScoreRuntimeResidentBlasRecreateTraceEntry(const NRIRenderer::RuntimeResidentBlasRecreateTraceEntry& entry)
	{
		uint32_t score = entry.triangleCount * 8u;
		score += entry.surfaceCount * 4u;
		score += entry.materialCount * 2u;
		if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_TopologyChanged) != 0)
		{
			score += 1u << 20;
		}
		if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_ForceTopology) != 0)
		{
			score += 1u << 19;
		}
		if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_SliceMoved) != 0)
		{
			score += 1u << 18;
		}
		if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_NoPreviousAs) != 0)
		{
			score += 1u << 17;
		}
		if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_RecoveredEmpty) != 0)
		{
			score += 1u << 16;
		}
		return score;
	}

	enum RuntimeResidentBlasRefitRejectBits : uint32_t
	{
		RuntimeResidentBlasRefitReject_NoPreviousAs = 1u << 0,
		RuntimeResidentBlasRefitReject_IndexCountMismatch = 1u << 1,
		RuntimeResidentBlasRefitReject_PrimitiveCountMismatch = 1u << 2,
		RuntimeResidentBlasRefitReject_ZeroIndexCount = 1u << 3,
		RuntimeResidentBlasRefitReject_ZeroPrimitiveCount = 1u << 4,
	};

	uint32_t ScoreRuntimeResidentBlasRefitRejectTraceEntry(const NRIRenderer::RuntimeResidentBlasRefitRejectTraceEntry& entry)
	{
		const uint32_t indexDelta =
			entry.previousIndexCount > entry.liveIndexCount ?
			entry.previousIndexCount - entry.liveIndexCount :
			entry.liveIndexCount - entry.previousIndexCount;
		const uint32_t primitiveDelta =
			entry.previousPrimitiveCount > entry.livePrimitiveCount ?
			entry.previousPrimitiveCount - entry.livePrimitiveCount :
			entry.livePrimitiveCount - entry.previousPrimitiveCount;
		uint32_t score = indexDelta * 16u;
		score += primitiveDelta * 16u;
		score += entry.livePrimitiveCount * 4u;
		score += entry.liveIndexCount * 2u;
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_IndexCountMismatch) != 0)
		{
			score += 1u << 20;
		}
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_PrimitiveCountMismatch) != 0)
		{
			score += 1u << 19;
		}
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_NoPreviousAs) != 0)
		{
			score += 1u << 18;
		}
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_ZeroIndexCount) != 0)
		{
			score += 1u << 17;
		}
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_ZeroPrimitiveCount) != 0)
		{
			score += 1u << 16;
		}
		return score;
	}

	enum RuntimeStructuralRebuildTriggerBits : uint32_t
	{
		RuntimeStructuralRebuildTrigger_ReplacementDelta = 1u << 0,
		RuntimeStructuralRebuildTrigger_ViewChanged = 1u << 1,
		RuntimeStructuralRebuildTrigger_StaticAnimatedFlip = 1u << 2,
		RuntimeStructuralRebuildTrigger_ExcludeStaticFlip = 1u << 3,
		RuntimeStructuralRebuildTrigger_ForceTopology = 1u << 4,
		RuntimeStructuralRebuildTrigger_Invalid = 1u << 5,
	};

	uint32_t ScoreRuntimeStructuralRebuildTraceEntry(const NRIRenderer::RuntimeStructuralRebuildTraceEntry& entry)
	{
		uint32_t score = entry.triangleCount * 8u;
		score += entry.surfaceCount * 4u;
		score += entry.materialCount * 2u;
		if ((entry.triggerMask & RuntimeStructuralRebuildTrigger_ForceTopology) != 0)
		{
			score += 1u << 20;
		}
		if ((entry.triggerMask & RuntimeStructuralRebuildTrigger_StaticAnimatedFlip) != 0)
		{
			score += 1u << 19;
		}
		if ((entry.triggerMask & RuntimeStructuralRebuildTrigger_ExcludeStaticFlip) != 0)
		{
			score += 1u << 18;
		}
		if (entry.mixedMaterialOnly)
		{
			score += 1u << 17;
		}
		if (entry.geometryOrDirty)
		{
			score += 1u << 16;
		}
		return score;
	}

	enum RuntimeGeometryDirtyFamilyBits : uint32_t
	{
		RuntimeGeometryDirtyFamily_SectorGeometryOnly = 1u << 0,
		RuntimeGeometryDirtyFamily_WallGeometryOnly = 1u << 1,
		RuntimeGeometryDirtyFamily_SectorWallGeometry = 1u << 2,
		RuntimeGeometryDirtyFamily_DirtyOnly = 1u << 3,
		RuntimeGeometryDirtyFamily_GeometryDirtyMixed = 1u << 4,
	};

	uint32_t ScoreRuntimeGeometryDirtyTraceEntry(const NRIRenderer::RuntimeGeometryDirtyTraceEntry& entry)
	{
		const uint32_t triangleDelta =
			entry.previousTriangleCount > entry.liveTriangleCount ?
			entry.previousTriangleCount - entry.liveTriangleCount :
			entry.liveTriangleCount - entry.previousTriangleCount;
		const uint32_t materialDelta =
			entry.previousMaterialCount > entry.liveMaterialCount ?
			entry.previousMaterialCount - entry.liveMaterialCount :
			entry.liveMaterialCount - entry.previousMaterialCount;
		uint32_t score = triangleDelta * 32u;
		score += materialDelta * 16u;
		score += entry.liveTriangleCount * 4u;
		score += entry.liveMaterialCount * 2u;
		if ((entry.familyMask & RuntimeGeometryDirtyFamily_GeometryDirtyMixed) != 0)
		{
			score += 1u << 20;
		}
		if ((entry.familyMask & RuntimeGeometryDirtyFamily_SectorWallGeometry) != 0)
		{
			score += 1u << 19;
		}
		if (entry.forceTopology)
		{
			score += 1u << 18;
		}
		if (entry.countChanged)
		{
			score += 1u << 17;
		}
		if (entry.wallsChanged && entry.flatsChanged)
		{
			score += 1u << 16;
		}
		return score;
	}

	uint32_t ScoreRuntimeRecurringChunkTraceEntry(const NRIRenderer::RuntimeRecurringChunkTraceEntry& entry)
	{
		uint32_t score = entry.repeatedStateHitCount * 256u;
		score += entry.abaRecurrenceCount * 192u;
		score += entry.transitionCount * 64u;
		score += entry.uniqueStateCount * 32u;
		score += entry.visitCount * 8u;
		score += entry.lastTriangleCount * 4u;
		score += entry.lastMaterialCount * 2u;
		return score;
	}

	template <typename Entry, size_t N, typename ScoreFn>
	void InsertRankedTraceEntry(std::array<Entry, N>& entries, Entry entry, ScoreFn scoreFn)
	{
		entry.score = scoreFn(entry);
		size_t insertIndex = N;
		for (size_t i = 0; i < N; ++i)
		{
			if (!entries[i].valid || entry.score > entries[i].score)
			{
				insertIndex = i;
				break;
			}
		}

		if (insertIndex >= N)
		{
			return;
		}

		for (size_t i = N - 1; i > insertIndex; --i)
		{
			entries[i] = entries[i - 1];
		}
		entries[insertIndex] = entry;
		entries[insertIndex].valid = true;
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
		case NRIRenderer::MaterialBuildTraceSlot::ResidentRuntimeMutationChunk: return "resident_runtime_mutation_chunk";
		case NRIRenderer::MaterialBuildTraceSlot::ResidentRuntimeMutationChunkRecover: return "resident_runtime_mutation_chunk_recover";
		case NRIRenderer::MaterialBuildTraceSlot::RuntimeSpaceLinkChunk: return "runtime_space_link_chunk";
		case NRIRenderer::MaterialBuildTraceSlot::Unknown: return "unknown";
		case NRIRenderer::MaterialBuildTraceSlot::Count: break;
		}

		return "unknown";
	}

	class ScopedPtPerfTimer
	{
	public:
		explicit ScopedPtPerfTimer(double& targetMs)
			: mTarget(ShouldCollectPtPerfTiming() ? &targetMs : nullptr)
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
		case NRI_SCENE_DATA_SOURCE_PERSISTENT_VOXEL: return "persistent_voxel";
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
		if (!surface.indices.empty())
		{
			return (uint32_t)(surface.indices.size() / 3u);
		}
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

	static nri::AccessStage NRICopySourceAccess()
	{
		return { nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY };
	}

	static nri::AccessStage NRICopyDestinationAccess()
	{
		return { nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY };
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

	static const char* GetNrdHitDistanceReconstructionModeName(uint32_t mode)
	{
		switch (mode)
		{
		case 1: return "area_3x3";
		case 2: return "area_5x5";
		default: return "off";
		}
	}

	static bool IsSupportedPtDebugMode(uint32_t debugMode)
	{
		return IsNRIFrameGraphSupportedDebugMode(debugMode);
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

	static float GetGlowmapVisibleBlendScale()
	{
		return std::clamp((float)nri_ptglowblend, 0.0f, 3.0f);
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
		NRIFrameRouteRequest request = {};
		request.debugMode = debugMode;
		request.bootstrap = bootstrap;
		request.bootstrapMode = bootstrap ? GetBootstrapMode() : 0u;
		return ResolveNRIFrameRoute(request);
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


	static nri::AccessStage NRIComputeAccelerationStructureReadAccess()
	{
		return { nri::AccessBits::ACCELERATION_STRUCTURE_READ, nri::StageBits::COMPUTE_SHADER };
	}

	static uint32_t GetDispatchSize(uint32_t value)
	{
		return (value + 7u) / 8u;
	}

	static uint64_t HashPrimitiveRewriteProvenancePayload(const std::vector<nri_scene::SurfaceProvenance>& provenanceList)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = CoherencyHashCombine64(hash, (uint64_t)provenanceList.size());
		for (const nri_scene::SurfaceProvenance& provenance : provenanceList)
		{
			hash = CoherencyHashCombine64(hash, (uint64_t)(uint32_t)provenance.sourceType);
			hash = CoherencyHashCombine64(hash, (uint64_t)(uint32_t)(provenance.sectorIndex + 1));
			hash = CoherencyHashCombine64(hash, (uint64_t)(uint32_t)(provenance.wallIndex + 1));
			hash = CoherencyHashCombine64(hash, (uint64_t)(uint32_t)(provenance.sectionIndex + 1));
			hash = CoherencyHashCombine64(hash, (uint64_t)(uint32_t)(provenance.mapChunkIndex + 1));
			hash = CoherencyHashCombine64(hash, (uint64_t)(uint32_t)(provenance.nextSectorIndex + 1));
			hash = CoherencyHashCombine64(hash, (uint64_t)(uint32_t)(provenance.actorIndex + 1));
			hash = CoherencyHashCombine64(hash, (uint64_t)provenance.drawListType);
			hash = CoherencyHashCombine64(hash, (uint64_t)provenance.cstat);
			hash = CoherencyHashCombine64(hash, (uint64_t)provenance.materialFlags);
		}
		return hash != 0 ? hash : 1;
	}

	static uint64_t HashPrimitiveRewriteVisibilityIdentity(const nri_scene::PTMapWorld& mapWorld)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = CoherencyHashCombine64(hash, mapWorld.valid ? 1ull : 0ull);
		hash = CoherencyHashCombine64(hash, mapWorld.buildSerial);
		hash = CoherencyHashCombine64(hash, (uint64_t)mapWorld.chunks.size());
		hash = CoherencyHashCombine64(hash, (uint64_t)mapWorld.stats.chunkCount);
		for (const nri_scene::PTMapChunk& chunk : mapWorld.chunks)
		{
			hash = CoherencyHashCombine64(hash, (uint64_t)chunk.chunkIndex);
			hash = CoherencyHashCombine64(hash, (uint64_t)(uint32_t)(chunk.sectorIndex + 1));
			hash = CoherencyHashCombine64(hash, (uint64_t)chunk.firstSurface);
			hash = CoherencyHashCombine64(hash, (uint64_t)chunk.surfaceCount);
		}
		return hash != 0 ? hash : 1;
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
		const int32_t chunkIndex = nri_static_scene_geometry::FindMapChunkIndexForSector(mapWorld, sectorIndex);
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

	static float Clamp01(float value)
	{
		return std::max(0.0f, std::min(value, 1.0f));
	}

	static uint32_t ClampTraceBounceCount(int value, uint32_t maxValue)
	{
		return (uint32_t)std::max(0, std::min(value, (int)maxValue));
	}

	static float GetBaseAmbient()
	{
		return std::max(0.0f, (float)nri_ptbaseambient);
	}

	static float GetMetalAmbient()
	{
		return std::max(0.0f, (float)nri_ptmetalambient);
	}

	static uint32_t PackAmbientMultiplier12(float value)
	{
		return (uint32_t)std::min(4095.0f, std::max(0.0f, value) * 1024.0f + 0.5f);
	}

	static uint32_t PackPortalDepthAndAmbientMultipliers(uint32_t portalDepth, float baseAmbient, float metalAmbient)
	{
		return
			(portalDepth & 0xffu) |
			(PackAmbientMultiplier12(baseAmbient) << 8u) |
			(PackAmbientMultiplier12(metalAmbient) << 20u);
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

	static uint64_t EstimateSceneTextureUploadBytes(const nri_scene::TextureUpload& upload)
	{
		if (upload.width == 0 || upload.height == 0)
		{
			return 0;
		}
		const uint64_t bytesPerPixel = upload.indexed ? 1ull : 4ull;
		return (uint64_t)upload.width * (uint64_t)upload.height * bytesPerPixel;
	}

	struct StartupMapWorldChunkDiffSample
	{
		uint32_t chunkIndex = 0;
		int32_t currentSectorIndex = -1;
		int32_t rebuiltSectorIndex = -1;
		uint32_t currentSurfaceCount = 0;
		uint32_t rebuiltSurfaceCount = 0;
		uint32_t currentTriangleCount = 0;
		uint32_t rebuiltTriangleCount = 0;
		uint32_t surfaceDiffCount = 0;
	};

	struct StartupMapWorldDiffDetails
	{
		bool validMismatch = false;
		bool chunkCountMismatch = false;
		bool surfaceCountMismatch = false;
		bool portalCountMismatch = false;
		bool portalTargetCountMismatch = false;
		bool statsMismatch = false;
		std::vector<StartupMapWorldChunkDiffSample> chunkSamples;
		std::vector<uint32_t> lateVisibleValidationChunks;
	};

	static bool StartupMapWorldSurfaceDiffers(
		const nri_scene::PTMapSurface& currentSurface,
		const nri_scene::PTMapSurface& rebuiltSurface)
	{
		const auto& a = currentSurface.surface;
		const auto& b = rebuiltSurface.surface;
		return currentSurface.kind != rebuiltSurface.kind ||
			a.vertices.size() != b.vertices.size() ||
			a.material.flags != b.material.flags ||
			a.material.texture != b.material.texture ||
			a.material.palette != b.material.palette ||
			a.provenance.sourceType != b.provenance.sourceType ||
			a.provenance.sectorIndex != b.provenance.sectorIndex ||
			a.provenance.wallIndex != b.provenance.wallIndex ||
			a.provenance.sectionIndex != b.provenance.sectionIndex;
	}

	static std::string BuildStartupMapWorldDiffReasonSummary(const StartupMapWorldDiffDetails& details)
	{
		std::string summary;
		auto appendReason = [&summary](const char* reason)
		{
			if (reason == nullptr || reason[0] == '\0')
			{
				return;
			}
			if (!summary.empty())
			{
				summary += ",";
			}
			summary += reason;
		};

		appendReason(details.validMismatch ? "valid" : nullptr);
		appendReason(details.chunkCountMismatch ? "chunk-count" : nullptr);
		appendReason(details.surfaceCountMismatch ? "surface-count" : nullptr);
		appendReason(details.portalCountMismatch ? "portal-count" : nullptr);
		appendReason(details.portalTargetCountMismatch ? "portal-target-count" : nullptr);
		appendReason(details.statsMismatch ? "stats" : nullptr);
		return summary.empty() ? "chunk-or-surface" : summary;
	}

	static bool StartupMapWorldStructureDiffers(
		const nri_scene::PTMapWorld& currentWorld,
		const nri_scene::PTMapWorld& rebuiltWorld,
		uint32_t& outChunkDiffCount,
		uint32_t& outSurfaceDiffCount,
		StartupMapWorldDiffDetails* outDetails = nullptr)
	{
		outChunkDiffCount = 0;
		outSurfaceDiffCount = 0;

		StartupMapWorldDiffDetails localDetails = {};
		StartupMapWorldDiffDetails& details = outDetails != nullptr ? *outDetails : localDetails;
		details = {};

		details.validMismatch = currentWorld.valid != rebuiltWorld.valid;
		details.chunkCountMismatch = currentWorld.chunks.size() != rebuiltWorld.chunks.size();
		details.surfaceCountMismatch = currentWorld.surfaces.size() != rebuiltWorld.surfaces.size();
		details.portalCountMismatch = currentWorld.portals.size() != rebuiltWorld.portals.size();
		details.portalTargetCountMismatch = currentWorld.portalTargets.size() != rebuiltWorld.portalTargets.size();
		details.statsMismatch = std::memcmp(&currentWorld.stats, &rebuiltWorld.stats, sizeof(currentWorld.stats)) != 0;

		const size_t chunkCount = std::min(currentWorld.chunks.size(), rebuiltWorld.chunks.size());
		details.chunkSamples.reserve(std::min<size_t>(chunkCount, 12u));
		details.lateVisibleValidationChunks.reserve(std::min<size_t>(chunkCount, 64u));
		for (size_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
		{
			const auto& currentChunk = currentWorld.chunks[chunkIndex];
			const auto& rebuiltChunk = rebuiltWorld.chunks[chunkIndex];
			const bool chunkMetadataDiff =
				currentChunk.kind != rebuiltChunk.kind ||
				currentChunk.chunkIndex != rebuiltChunk.chunkIndex ||
				currentChunk.sectorIndex != rebuiltChunk.sectorIndex ||
				currentChunk.localSpaceIndex != rebuiltChunk.localSpaceIndex ||
				currentChunk.firstSurface != rebuiltChunk.firstSurface ||
				currentChunk.surfaceCount != rebuiltChunk.surfaceCount ||
				currentChunk.triangleCount != rebuiltChunk.triangleCount;

			const uint32_t currentAvailableSurfaceCount =
				currentChunk.firstSurface < currentWorld.surfaces.size() ?
				(uint32_t)std::min<size_t>(currentChunk.surfaceCount, currentWorld.surfaces.size() - currentChunk.firstSurface) :
				0u;
			const uint32_t rebuiltAvailableSurfaceCount =
				rebuiltChunk.firstSurface < rebuiltWorld.surfaces.size() ?
				(uint32_t)std::min<size_t>(rebuiltChunk.surfaceCount, rebuiltWorld.surfaces.size() - rebuiltChunk.firstSurface) :
				0u;
			const uint32_t comparableSurfaceCount = std::min(currentAvailableSurfaceCount, rebuiltAvailableSurfaceCount);

			uint32_t chunkSurfaceDiffCount = 0;
			for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < comparableSurfaceCount; ++localSurfaceIndex)
			{
				const auto& currentSurface = currentWorld.surfaces[currentChunk.firstSurface + localSurfaceIndex];
				const auto& rebuiltSurface = rebuiltWorld.surfaces[rebuiltChunk.firstSurface + localSurfaceIndex];
				if (StartupMapWorldSurfaceDiffers(currentSurface, rebuiltSurface))
				{
					chunkSurfaceDiffCount++;
				}
			}

			chunkSurfaceDiffCount +=
				currentAvailableSurfaceCount > rebuiltAvailableSurfaceCount ?
				(currentAvailableSurfaceCount - rebuiltAvailableSurfaceCount) :
				(rebuiltAvailableSurfaceCount - currentAvailableSurfaceCount);

			outSurfaceDiffCount += chunkSurfaceDiffCount;
			if (chunkMetadataDiff || chunkSurfaceDiffCount > 0u)
			{
				outChunkDiffCount++;
				const bool lateVisibleValidationCandidate =
					chunkSurfaceDiffCount > 0u &&
					currentAvailableSurfaceCount == rebuiltAvailableSurfaceCount &&
					currentChunk.triangleCount == rebuiltChunk.triangleCount &&
					currentChunk.chunkIndex == rebuiltChunk.chunkIndex;
				if (lateVisibleValidationCandidate)
				{
					details.lateVisibleValidationChunks.push_back(currentChunk.chunkIndex);
				}
				if (details.chunkSamples.size() < 12u)
				{
					StartupMapWorldChunkDiffSample sample = {};
					sample.chunkIndex = (uint32_t)chunkIndex;
					sample.currentSectorIndex = currentChunk.sectorIndex;
					sample.rebuiltSectorIndex = rebuiltChunk.sectorIndex;
					sample.currentSurfaceCount = currentAvailableSurfaceCount;
					sample.rebuiltSurfaceCount = rebuiltAvailableSurfaceCount;
					sample.currentTriangleCount = currentChunk.triangleCount;
					sample.rebuiltTriangleCount = rebuiltChunk.triangleCount;
					sample.surfaceDiffCount = chunkSurfaceDiffCount;
					details.chunkSamples.push_back(sample);
				}
			}
		}

		return details.validMismatch ||
			details.chunkCountMismatch ||
			details.surfaceCountMismatch ||
			details.portalCountMismatch ||
			details.portalTargetCountMismatch ||
			outChunkDiffCount > 0 ||
			outSurfaceDiffCount > 0;
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

	static void RemapMaterialBridgeAgainstTextureTable(
		const nri_scene::MaterialBridgeData& source,
		nri_scene::MaterialBridgeData& inOutTextureTable,
		nri_scene::MaterialBridgeData& outRemapped,
		bool* outTextureTableGrew = nullptr)
	{
		if (outTextureTableGrew != nullptr)
		{
			*outTextureTableGrew = false;
		}

		std::unordered_map<uint64_t, uint32_t> textureLookup;
		textureLookup.reserve(inOutTextureTable.textures.size() + source.textures.size());
		for (uint32_t i = 0; i < (uint32_t)inOutTextureTable.textures.size(); ++i)
		{
			textureLookup.emplace(inOutTextureTable.textures[i].key, i);
		}

		auto remapTextureIndex = [&source, &inOutTextureTable, &textureLookup, outTextureTableGrew](uint32_t textureIndex) -> uint32_t
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
			if (it != textureLookup.end())
			{
				return it->second;
			}

			const uint32_t newIndex = (uint32_t)inOutTextureTable.textures.size();
			textureLookup.emplace(texture.key, newIndex);
			inOutTextureTable.textures.push_back(texture);
			if (outTextureTableGrew != nullptr)
			{
				*outTextureTableGrew = true;
			}
			return newIndex;
		};

		outRemapped = {};
		outRemapped.materials.reserve(source.materials.size());
		outRemapped.lightMetadata.reserve(source.lightMetadata.size());

		for (size_t materialIndex = 0; materialIndex < source.materials.size(); ++materialIndex)
		{
			const auto& material = source.materials[materialIndex];
			nri_scene::MaterialData copy = material;
			copy.textureIndex = remapTextureIndex(material.textureIndex);
			copy.normalTextureIndex = remapTextureIndex(material.normalTextureIndex);
			copy.metallicTextureIndex = remapTextureIndex(material.metallicTextureIndex);
			copy.roughnessTextureIndex = remapTextureIndex(material.roughnessTextureIndex);
			copy.emissiveTextureIndex = remapTextureIndex(material.emissiveTextureIndex);
			outRemapped.materials.push_back(copy);

			if (materialIndex < source.lightMetadata.size())
			{
				nri_scene::MaterialLightingMetadata metadata = source.lightMetadata[materialIndex];
				metadata.textureIndex = remapTextureIndex(metadata.textureIndex);
				metadata.glowmapTextureIndex = remapTextureIndex(metadata.glowmapTextureIndex);
				metadata.normalTextureIndex = remapTextureIndex(metadata.normalTextureIndex);
				metadata.metallicTextureIndex = remapTextureIndex(metadata.metallicTextureIndex);
				metadata.roughnessTextureIndex = remapTextureIndex(metadata.roughnessTextureIndex);
				metadata.emissiveTextureIndex = remapTextureIndex(metadata.emissiveTextureIndex);
				outRemapped.lightMetadata.push_back(metadata);
			}
		}

		if (!source.paletteLookup.empty())
		{
			outRemapped.paletteLookup = source.paletteLookup;
			outRemapped.paletteWidth = source.paletteWidth;
			outRemapped.paletteHeight = source.paletteHeight;
			if (inOutTextureTable.paletteLookup.empty())
			{
				inOutTextureTable.paletteLookup = source.paletteLookup;
				inOutTextureTable.paletteWidth = source.paletteWidth;
				inOutTextureTable.paletteHeight = source.paletteHeight;
			}
		}
	}

	static bool TryBuildMergedSectorMaterialOnlyBridge(
		const nri_scene::SceneView& residentChunkView,
		const nri_scene::MaterialBridgeData& residentChunkMaterials,
		const nri_scene::SceneView& filteredLiveChunkView,
		const nri_scene::MaterialBridgeData& filteredLiveMaterials,
		nri_scene::MaterialBridgeData& outMergedMaterials)
	{
		if (!filteredLiveChunkView.opaqueWalls.empty())
		{
			return false;
		}

		const uint32_t residentWallCount = (uint32_t)residentChunkView.opaqueWalls.size();
		const uint32_t residentFlatCount = (uint32_t)residentChunkView.opaqueFlats.size();
		if (residentFlatCount == 0 ||
			filteredLiveChunkView.opaqueFlats.size() != residentFlatCount ||
			filteredLiveMaterials.materials.size() != residentFlatCount ||
			filteredLiveMaterials.lightMetadata.size() != residentFlatCount)
		{
			return false;
		}

		if (residentChunkMaterials.materials.size() != residentChunkMaterials.lightMetadata.size() ||
			residentWallCount + residentFlatCount > residentChunkMaterials.materials.size())
		{
			return false;
		}

		outMergedMaterials = residentChunkMaterials;
		nri_scene::MaterialBridgeData remappedFlatMaterials;
		RemapMaterialBridgeAgainstTextureTable(
			filteredLiveMaterials,
			outMergedMaterials,
			remappedFlatMaterials);
		if (remappedFlatMaterials.materials.size() != residentFlatCount ||
			remappedFlatMaterials.lightMetadata.size() != residentFlatCount)
		{
			return false;
		}

		std::copy_n(
			remappedFlatMaterials.materials.data(),
			residentFlatCount,
			outMergedMaterials.materials.begin() + residentWallCount);
		std::copy_n(
			remappedFlatMaterials.lightMetadata.data(),
			residentFlatCount,
			outMergedMaterials.lightMetadata.begin() + residentWallCount);
		return true;
	}

	static bool TryBuildMergedSectorMaterialOnlySceneView(
		const nri_scene::SceneView& residentChunkView,
		const nri_scene::SceneView& filteredLiveChunkView,
		nri_scene::SceneView& outMergedSceneView)
	{
		if (!filteredLiveChunkView.opaqueWalls.empty() ||
			residentChunkView.opaqueFlats.size() != filteredLiveChunkView.opaqueFlats.size())
		{
			return false;
		}

		outMergedSceneView = filteredLiveChunkView;
		outMergedSceneView.opaqueWalls = residentChunkView.opaqueWalls;
		outMergedSceneView.opaqueSprites = residentChunkView.opaqueSprites;
		outMergedSceneView.stats.totalDrawItems =
			(unsigned)(outMergedSceneView.opaqueWalls.size() +
				outMergedSceneView.opaqueFlats.size() +
				outMergedSceneView.opaqueSprites.size());
		outMergedSceneView.stats.wallDrawItems = (unsigned)outMergedSceneView.opaqueWalls.size();
		outMergedSceneView.stats.flatDrawItems = (unsigned)outMergedSceneView.opaqueFlats.size();
		outMergedSceneView.stats.spriteDrawItems = (unsigned)outMergedSceneView.opaqueSprites.size();
		outMergedSceneView.stats.materialRefs = outMergedSceneView.stats.totalDrawItems;
		return true;
	}

	static bool StructuredBufferUpdateNeedsWait(
		const NRIBufferResource& resource,
		const void* data,
		uint64_t size,
		uint32_t stride)
	{
		const uint64_t requiredSize = std::max<uint64_t>(size, stride);
		const bool needsGrowth =
			resource.buffer == nullptr ||
			resource.shaderView == nullptr ||
			resource.stride != stride ||
			resource.size < requiredSize;
		if (needsGrowth)
		{
			return resource.buffer != nullptr || resource.shaderView != nullptr;
		}

		return data != nullptr && size != 0;
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

	static uint32_t PackTemporalJitterPhaseCount(uint32_t jitterPhaseCount)
	{
		return (std::min(jitterPhaseCount, 255u) & 0xffu) << NRI_JITTER_PHASE_SHIFT;
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

	static void ComputeTemporalJitter(uint32_t frameIndex, uint32_t jitterPhaseCount, float outJitter[2])
	{
		jitterPhaseCount = std::max(jitterPhaseCount, 1u);
		const uint32_t sampleIndex = (frameIndex % jitterPhaseCount) + 1u;
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

	static const char* GetGraphicsApiName(nri::GraphicsAPI api)
	{
		switch (api)
		{
		case nri::GraphicsAPI::D3D12: return "d3d12";
		case nri::GraphicsAPI::VK: return "vulkan";
		default: return "unknown";
		}
	}

	static bool HasAutoEmissiveSourceFlags(uint32_t sourceFlags)
	{
		return (sourceFlags & (
			SceneEmissiveSurfaceSourceFlag_AutoFullbright |
			SceneEmissiveSurfaceSourceFlag_AutoTextureGlow |
			SceneEmissiveSurfaceSourceFlag_AutoGlowmap)) != 0;
	}

	static bool IsEmissiveSurfaceSectorResponseEligible(const SceneLightSystem::EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface)
	{
		return
			surface.sectorResponseEnabled &&
			surface.sectorIndex >= 0 &&
			(((surface.sourceFlags & SceneEmissiveSurfaceSourceFlag_LightOverlayOverride) != 0) ||
				(HasAutoEmissiveSourceFlags(surface.sourceFlags) &&
					(surface.sourceFlags & SceneEmissiveSurfaceSourceFlag_ExplicitTextureRule) == 0));
	}

	static bool IsEmissiveSurfaceMaterialResponseEligible(const SceneLightSystem::EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface)
	{
		return
			surface.materialResponseEnabled &&
			surface.sectorIndex >= 0 &&
			(surface.materialResponseExplicit || IsEmissiveSurfaceSectorResponseEligible(surface));
	}

	static float ComputeSectorEmitterResponseScaleForRenderer(float brightness, float neutralBrightness, float intensity, float minScale, float maxScale)
	{
		const float clampedMin = std::max(0.0f, minScale);
		const float clampedMax = std::max(clampedMin, maxScale);
		if (neutralBrightness <= 0.0001f || intensity <= 0.0f)
		{
			return clamp(1.0f, clampedMin, clampedMax);
		}

		const float normalizedDelta = (brightness - neutralBrightness) / neutralBrightness;
		return clamp(1.0f + normalizedDelta * intensity, clampedMin, clampedMax);
	}

	static float ComputeSectorEmitterRangeResponseScaleForRenderer(float signal, float inputMin, float inputMax, float minScale, float maxScale)
	{
		const float clampedMin = std::max(0.0f, minScale);
		const float clampedMax = std::max(clampedMin, maxScale);
		const float inputRange = inputMax - inputMin;
		if (std::abs(inputRange) <= 0.0001f)
		{
			return clamp(1.0f, clampedMin, clampedMax);
		}

		const float t = clamp((signal - inputMin) / inputRange, 0.0f, 1.0f);
		return clampedMin + (clampedMax - clampedMin) * t;
	}

	static float GetSectorEmitterNeutralBrightnessForRenderer()
	{
		const float sectorClamp = std::max(0.0f, (float)nri_ptsectorclamp);
		const float ambientScale = std::max(0.0f, (float)nri_ptsectorambientscale);
		return std::min(sectorClamp, ambientScale * (0.10f + 0.75f * 0.55f));
	}

	static float ResolveSectorEmissionScale(
		const SceneLightSystem::SectorLightingRegistry& sectorRegistry,
		const SceneLightSystem::EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface,
		bool& outApplied)
	{
		outApplied = false;
		if (!IsEmissiveSurfaceSectorResponseEligible(surface))
		{
			return 1.0f;
		}

		const uint32_t sectorIndex = (uint32_t)surface.sectorIndex;
		if (sectorIndex >= sectorRegistry.sectors.size())
		{
			return 1.0f;
		}

		const auto& sector = sectorRegistry.sectors[sectorIndex];
		const float scale = surface.hasSectorResponseInputRange ?
			ComputeSectorEmitterRangeResponseScaleForRenderer(
				sector.rawResponseSignal,
				surface.sectorResponseInputMin,
				surface.sectorResponseInputMax,
				std::max(0.0f, surface.sectorResponseMin),
				std::max(surface.sectorResponseMin, surface.sectorResponseMax)) :
			surface.hasSectorResponseParams ?
			ComputeSectorEmitterResponseScaleForRenderer(
				sector.rawResponseBrightness,
				GetSectorEmitterNeutralBrightnessForRenderer(),
				std::max(0.0f, surface.sectorResponseIntensity),
				std::max(0.0f, surface.sectorResponseMin),
				std::max(surface.sectorResponseMin, surface.sectorResponseMax)) :
			std::max(0.0f, sector.emitterResponseScale);
		outApplied = scale != 1.0f;
		return scale;
	}

	static float ClampSectorEmissionIntensityScale(
		const SceneLightSystem::EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface,
		float scale)
	{
		const float minScale = surface.hasSectorResponseIntensityMin ?
			std::max(0.0f, surface.sectorResponseIntensityMin) :
			std::max(0.0f, (float)nri_ptsectoremissionlightmin);
		const float maxScale = surface.hasSectorResponseIntensityMax ?
			std::max(minScale, surface.sectorResponseIntensityMax) :
			std::max(minScale, (float)nri_ptsectoremissionlightmax);
		return clamp(std::max(0.0f, scale), minScale, maxScale);
	}

	static float ClampSectorEmissionReachScale(
		const SceneLightSystem::EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface,
		float scale)
	{
		const float minScale = surface.hasSectorResponseReachMin ?
			std::max(0.0f, surface.sectorResponseReachMin) :
			std::max(0.0f, (float)nri_ptsectoremissionreachmin);
		const float maxScale = surface.hasSectorResponseReachMax ?
			std::max(minScale, surface.sectorResponseReachMax) :
			std::max(minScale, (float)nri_ptsectoremissionreachmax);
		return clamp(std::max(0.0f, scale), minScale, maxScale);
	}

	static float ResolveEmissiveMaterialResponseScale(
		const SceneLightSystem::SectorLightingRegistry& sectorRegistry,
		const SceneLightSystem::EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface,
		bool& outApplied)
	{
		outApplied = false;
		if (!IsEmissiveSurfaceMaterialResponseEligible(surface))
		{
			return 1.0f;
		}

		const uint32_t sectorIndex = (uint32_t)surface.sectorIndex;
		if (sectorIndex >= sectorRegistry.sectors.size())
		{
			return 1.0f;
		}

		const auto& sector = sectorRegistry.sectors[sectorIndex];
		const float globalMinScale = std::max(0.0f, (float)nri_ptsectoremissionmaterialmin);
		const float globalMaxScale = std::max(globalMinScale, (float)nri_ptsectoremissionmaterialmax);
		const float minScale = surface.hasMaterialResponseMin ? std::max(0.0f, surface.materialResponseMin) : globalMinScale;
		const float maxScale = surface.hasMaterialResponseMax ? std::max(minScale, surface.materialResponseMax) : globalMaxScale;
		const float scale = surface.hasSectorResponseInputRange ?
			ComputeSectorEmitterRangeResponseScaleForRenderer(
				sector.rawResponseSignal,
				surface.sectorResponseInputMin,
				surface.sectorResponseInputMax,
				minScale,
				maxScale) :
			surface.hasSectorResponseParams ?
			ComputeSectorEmitterResponseScaleForRenderer(
				sector.rawResponseBrightness,
				GetSectorEmitterNeutralBrightnessForRenderer(),
				std::max(0.0f, surface.sectorResponseIntensity),
				minScale,
				maxScale) :
			clamp(std::max(0.0f, sector.emitterResponseScale), minScale, maxScale);

		outApplied = scale != 1.0f;
		return scale;
	}


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

	static bool IsAuthoredTextureCurrentlyUnresolved(FTextureID textureId)
	{
		if (!textureId.isValid())
		{
			return false;
		}

		FGameTexture* texture = TexMan.GetGameTexture(textureId, true);
		return texture == nullptr || !texture->isValid();
	}

	static bool ChunkHasUnresolvedAuthoredTextures(const nri_scene::PTMapChunk& chunk)
	{
		if (chunk.kind != nri_scene::PTMapChunkKind::Sector ||
			chunk.sectorIndex < 0 ||
			(unsigned)chunk.sectorIndex >= sector.Size())
		{
			return false;
		}

		const sectortype& sec = sector[(unsigned)chunk.sectorIndex];
		if (IsAuthoredTextureCurrentlyUnresolved(sec.floortexture) ||
			IsAuthoredTextureCurrentlyUnresolved(sec.ceilingtexture))
		{
			return true;
		}

		for (const walltype& wal : sec.walls)
		{
			if (IsAuthoredTextureCurrentlyUnresolved(wal.walltexture) ||
				IsAuthoredTextureCurrentlyUnresolved(wal.overtexture))
			{
				return true;
			}

			if (wal.nextwall >= 0 && (unsigned)wal.nextwall < wall.Size())
			{
				const walltype& nextWall = wall[(unsigned)wal.nextwall];
				if (IsAuthoredTextureCurrentlyUnresolved(nextWall.walltexture) ||
					IsAuthoredTextureCurrentlyUnresolved(nextWall.overtexture))
				{
					return true;
				}
			}
		}

		return false;
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
		case nri_scene::SurfaceSourceType::SurfaceLightOverlay: return "surface_light_overlay";
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
		case SceneLightRecordSource::PersistentVoxelScene: return "persistent_voxel_scene";
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

	if (!mSceneTextures.LimitLogPrinted())
	{
		LogSceneTextureDescriptorLimits(mFrameBuffer->mCore.GetDeviceDesc(*mFrameBuffer->mDevice));
		mSceneTextures.LimitLogPrinted() = true;
	}

	if (!CheckPathTracingSupport())
	{
		return true;
	}

	if (mPipelineLayout != nullptr)
	{
		return true;
	}

	return CreatePipelineLayout() && CreateTaaPipelineLayout() && CreatePresentPipelineLayout() && CreateExposurePipelineLayout() && AllocateDescriptorSets() && UpdateSamplerSet() && CreatePipelines();
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
	mFrameBuffer->DestroyTextureResource(mSceneTextures.PaletteTexture());
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
	mSceneTextures.LimitLogPrinted() = false;
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
	if (mExposurePipelineLayout != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipelineLayout(mExposurePipelineLayout);
		mExposurePipelineLayout = nullptr;
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
	mExposureInputSets = {};
	mExposureOutputSets = {};
	mAutoExposureInputSourceSlot = FrameTextureSlot::Count;
	mSceneDataDescriptorsInitialized.clear();
}

void NRIRenderer::OnLevelUnloadBegin(const LevelTransitionInfo& info)
{
	WaitForCommandsTracked("level-unload");
	RequestHistoryReset("level-unload", true, true);

	DestroyStaticMapSceneCache("level-unload");
	mStaticMapScene = {};
	mStaticAccelerationBuildSerial = 0;
	mSkyEnvironment.PreservedStaticMapSky() = {};

	mMapWorld = {};
	mObservedMapWorldBuildSerial = 0;

	DestroyCachedTextures();
	ResetPersistentDynamicEmissiveCache();
	ResetMuzzleFlashOverlayState("level-unload");
	mLastResolvedLightOverlayGeneration = 0;

	ClearRuntimePointLights();
	ClearRuntimeDebugSpheres();
	mSceneLights.ResetLevelState();

	mPendingStaticMapLightingInvalidation = false;
	mAllowStartupMapWorldCorrection = false;
	mAllowStartupMutationRebaseline = false;
	mPendingStartupMutationRebaseline = false;
	mPendingStartupVisibleChunkValidation.clear();
	mRuntimeMutation.ResetWorklist();
	mStartupMapWorldCorrectionDeadlineFrame = 0;
	mStartupMutationRebaselineDeadlineFrame = 0;

	mCurrentVisibleChunkWords.clear();
	mCurrentVisibleFlatPlaneWords.clear();
	mLastSurfaceProbe = {};
	mLastLoggedSurfaceProbe = {};
	mSurfaceProbeFrame = {};
	mDynamicSceneLastFrame = {};
	mRuntimeMutation.ResetFrameState();
	mRuntimeSpaceLinkLastFrame = {};
	mLastRuntimeLinkTraceState = {};
	mHasRuntimeLinkTraceState = false;
	mRuntimeChunkTranslationHistory.clear();
	mLastStats = {};
	mHasLoggedStats = false;

	mSceneTextures.CacheStats() = {};
	mPersistentDynamicEmissiveHighWaterStats = {};
	mPersistentDynamicEmissiveHighWaterSurfaceCount = 0;
	mPersistentDynamicEmissiveHighWaterPrimitiveCount = 0;
	mPersistentDynamicEmissiveHighWaterMaterialCount = 0;
	mRuntimeMutation.ResetHighWaterStats();

	mUsedStaticMapSceneLastFrame = false;
	mUsedDynamicSceneLastFrame = false;
	mHasVisibleMirrorPortalLastFrame = false;
	mGpuSceneHasDynamicOverlay = false;
	mUploadedStaticMapSceneLastFrame = false;
	mBuiltStaticMapSceneASLastFrame = false;
	mBuiltDynamicSceneASLastFrame = false;

	mActiveTlasInstanceCount = 0;
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
	mRuntimeLightSceneDataDirty = false;
	mBoundEmissivePrimitiveCount = 0;
	mBoundEmissiveDominantPrimitive = UINT32_MAX;
	mBoundEmissiveDominantTile = 0;
	mBoundEmissiveDominantFlags = 0;
	mBoundEmissiveDominantDataSource = 0;
	mEmissiveSamplingPayloadCacheValid = false;
	mEmissiveSamplingPayloadHash = 0;
	mEmissiveSectorResponsePayloadCacheValid = false;
	mEmissiveSectorResponsePayloadHash = 0;
	mEmissiveSectorResponseTraceCacheValid = false;
	mEmissiveSectorResponseTraceHash = 0;
	mEmissiveSectorResponseNotifyCacheValid = false;
	mLastEmissiveSectorResponseNotifyFrame = 0;
	mEmissiveSectorResponseNotifyScales.clear();
	mSectorLightingEditNotifyCacheValid = false;
	mLastSectorLightingEditNotifyFrame = 0;
	mSectorLightingEditNotifyHashes.clear();
	mEmissiveTlasInstanceCount = 0;
	mEmissiveTlasStaticInstanceCount = 0;
	mEmissiveTlasDynamicInstanceCount = 0;
	mEmissiveTlasBuildCount = 0;
	mEmissiveTlasInstancePayloadCacheValid = false;
	mEmissiveTlasInstancePayloadHash = 0;
	mBoundEmissiveTotalPower = 0.0f;
	mBoundEmissiveDominantPower = 0.0f;
	mBoundEmissivePrimitiveRecords.clear();
	mSectorLightingPayloadCacheValid = false;
	mSectorLightingPayloadHash = 0;
	mBoundSectorLightSectorCount = 0;
	mBoundSectorLightActiveCount = 0;
	mBoundSectorLightPulsingCount = 0;
	mBoundSectorLightDominantSector = UINT32_MAX;
	mBoundSectorLightDominantContribution = 0.0f;

	if (info.newLevel == nullptr)
	{
		mNextRuntimePointLightId = 1;
		mNextRuntimeDebugSphereId = 1;
	}
}

void NRIRenderer::OnLevelUnloadComplete(const LevelTransitionInfo& info)
{
	assert(!mMapWorld.valid);
	assert(!mStaticMapScene.valid);
	assert(!mStaticMapScene.texturesResident);
	assert(!mStaticMapScene.buffersResident);
	assert(!mStaticMapScene.accelerationResident);
	assert(mStaticMapScene.chunks.empty());
	assert(!mStaticMapChunkAtlas.valid);
	assert(mStaticMapChunkAtlas.chunks.empty());
	assert(!mStaticSceneResidency.Registry().valid);
	assert(mStaticSceneResidency.Registry().entries.empty());
	assert(mRuntimeMutation.IsCacheEmpty());
	assert(!mPendingStaticMapLightingInvalidation);
	assert(!mLastSurfaceProbe.valid);
	assert(!mLastLoggedSurfaceProbe.valid);
	assert(mRuntimeDebugSpheres.empty());
	assert(mSceneLights.GetManualAnalyticLightCount() == 0);

	if (info.newLevel == nullptr)
	{
		mCurrentVisibleChunkWords.clear();
		mCurrentVisibleFlatPlaneWords.clear();
	}
}

void NRIRenderer::OnLevelLoadBegin(const LevelTransitionInfo& info)
{
	mMapWorld = {};
	mObservedMapWorldBuildSerial = 0;
	mAllowStartupMapWorldCorrection = false;
	mAllowStartupMutationRebaseline = false;
	mPendingStartupMutationRebaseline = false;
	mPendingStartupVisibleChunkValidation.clear();
	mRuntimeMutation.ResetWorklist();
	mStartupMapWorldCorrectionDeadlineFrame = 0;
	mStartupMutationRebaselineDeadlineFrame = 0;
	mLastSurfaceProbe = {};
	mLastLoggedSurfaceProbe = {};
	mSurfaceProbeFrame = {};
	mDynamicSceneLastFrame = {};
	mRuntimeMutation.ResetFrameState();
	mRuntimeSpaceLinkLastFrame = {};
	mRuntimeChunkTranslationHistory.clear();
	mSceneTextures.CacheStats() = {};
	mPersistentDynamicEmissiveHighWaterStats = {};
	mPersistentDynamicEmissiveHighWaterSurfaceCount = 0;
	mPersistentDynamicEmissiveHighWaterPrimitiveCount = 0;
	mPersistentDynamicEmissiveHighWaterMaterialCount = 0;
	mRuntimeMutation.ResetHighWaterStats();
	mLastStats = {};
	mHasLoggedStats = false;
	mLastRuntimeLinkTraceState = {};
	mHasRuntimeLinkTraceState = false;
	mNextRuntimePointLightId = 1;
	mNextRuntimeDebugSphereId = 1;

	if (info.newLevel == nullptr)
	{
		mCurrentVisibleChunkWords.clear();
		mCurrentVisibleFlatPlaneWords.clear();
	}
}

NRIRenderer::LevelTransitionSnapshot NRIRenderer::BuildLevelTransitionSnapshot() const
{
	LevelTransitionSnapshot snapshot = {};
	snapshot.mapWorldValid = mMapWorld.valid;
	snapshot.mapWorldBuildSerial = mMapWorld.buildSerial;
	snapshot.mapWorldChunkCount = (uint32_t)mMapWorld.chunks.size();
	snapshot.mapWorldSurfaceCount = (uint32_t)mMapWorld.surfaces.size();
	snapshot.staticSceneValid = mStaticMapScene.valid;
	snapshot.staticSceneTexturesResident = mStaticMapScene.texturesResident;
	snapshot.staticSceneBuffersResident = mStaticMapScene.buffersResident;
	snapshot.staticSceneAccelerationResident = mStaticMapScene.accelerationResident;
	snapshot.staticSceneBuildSerial = mStaticMapScene.buildSerial;
	snapshot.staticSceneChunkCount = (uint32_t)mStaticMapScene.chunks.size();
	snapshot.staticSceneMaterialCount = (uint32_t)mStaticMapScene.gpuMaterials.size();
	snapshot.textureCacheCount = mSceneTextures.CacheCount();
	snapshot.skyTextureCacheCount = (uint32_t)mSkyEnvironment.CachedTextures().size();
	const RuntimeMutationCacheStats runtimeMutationCacheStats = mRuntimeMutation.GatherCacheStats();
	snapshot.runtimeMutationChunkCount = mRuntimeMutation.GetCacheChunkCount();
	snapshot.runtimeMutationActiveChunkCount = runtimeMutationCacheStats.activeChunkCount;
	snapshot.runtimeMutationValidChunkCount = runtimeMutationCacheStats.validChunkCount;
	snapshot.residentChunkRegistryValid = mStaticSceneResidency.Registry().valid;
	snapshot.residentChunkRegistryEntryCount = (uint32_t)mStaticSceneResidency.Registry().entries.size();
	snapshot.residentChunkRegistryChunkCount = mStaticSceneResidency.Registry().chunkCount;
	snapshot.residentChunkRegistryActiveChunkCount = mStaticSceneResidency.Registry().activeChunkCount;
	snapshot.residentChunkRegistryMappedChunkCount = mStaticSceneResidency.Registry().mappedChunkCount;
	snapshot.residentChunkRegistryAccelerationResidentChunkCount = mStaticSceneResidency.Registry().accelerationResidentChunkCount;
	snapshot.pendingStaticMapLightingInvalidation = mPendingStaticMapLightingInvalidation;
	snapshot.surfaceProbeValid = mLastSurfaceProbe.valid;
	snapshot.surfaceProbeHit = mLastSurfaceProbe.hit;
	snapshot.surfaceProbeWallIndex = mLastSurfaceProbe.provenance.wallIndex;
	snapshot.surfaceProbeMapChunkIndex = mLastSurfaceProbe.provenance.mapChunkIndex;
	snapshot.transientMuzzleFlashSlotCount = (uint32_t)mTransientMuzzleFlashSlots.size();
	for (const TransientMuzzleFlashSlot& slot : mTransientMuzzleFlashSlots)
	{
		if (slot.occupied)
		{
			snapshot.transientMuzzleFlashActiveCount++;
		}
	}
	snapshot.analyticLightCount = (uint32_t)mSceneLights.GetAnalyticLights().activeLights.size();
	snapshot.manualLightCount = mSceneLights.GetManualAnalyticLightCount();
	snapshot.emissiveSurfaceCount = (uint32_t)mSceneLights.GetEmissiveSurfaces().activeSurfaces.size();
	snapshot.activeSectorLightCount = mSceneLights.GetSectorLighting().activeSectorCount;
	snapshot.runtimeDebugSphereCount = (uint32_t)mRuntimeDebugSpheres.size();
	snapshot.runtimeTestLightCount = mSceneLights.GetManualAnalyticLightCount();
	return snapshot;
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
}

void NRIRenderer::WaitForCommandsTracked(const char* reason)
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
		const double waitMs = DurationMs(start, std::chrono::steady_clock::now());
		mLastPerfResourceTraceStats.waitCalls++;
		mLastPerfResourceTraceStats.waitMs += waitMs;
		if (reason != nullptr)
		{
			if (std::strcmp(reason, "resident_chunk_write") == 0)
			{
				mLastPerfResourceTraceStats.residentChunkWriteWaitCalls++;
				mLastPerfResourceTraceStats.residentChunkWriteWaitMs += waitMs;
			}
			else if (std::strcmp(reason, "resident_chunk_blas_rebuild") == 0)
			{
				mLastPerfResourceTraceStats.residentChunkBlasRebuildWaitCalls++;
				mLastPerfResourceTraceStats.residentChunkBlasRebuildWaitMs += waitMs;
			}
			else if (std::strcmp(reason, "scene_data_upload") == 0)
			{
				mLastPerfResourceTraceStats.sceneDataUploadWaitCalls++;
				mLastPerfResourceTraceStats.sceneDataUploadWaitMs += waitMs;
			}
			else if (std::strcmp(reason, "scene_buffer_upload") == 0)
			{
				mLastPerfResourceTraceStats.sceneBufferUploadWaitCalls++;
				mLastPerfResourceTraceStats.sceneBufferUploadWaitMs += waitMs;
			}
			else if (std::strcmp(reason, "emissive_sampling_upload") == 0)
			{
				mLastPerfResourceTraceStats.emissiveSamplingUploadWaitCalls++;
				mLastPerfResourceTraceStats.emissiveSamplingUploadWaitMs += waitMs;
			}
			else if (std::strcmp(reason, "world_tlas_instance_upload") == 0)
			{
				mLastPerfResourceTraceStats.worldTlasInstanceUploadWaitCalls++;
				mLastPerfResourceTraceStats.worldTlasInstanceUploadWaitMs += waitMs;
			}
			else if (std::strcmp(reason, "world_tlas_scratch_resize") == 0)
			{
				mLastPerfResourceTraceStats.worldTlasScratchResizeWaitCalls++;
				mLastPerfResourceTraceStats.worldTlasScratchResizeWaitMs += waitMs;
			}
			else if (std::strcmp(reason, "emissive_tlas_instance_upload") == 0)
			{
				mLastPerfResourceTraceStats.emissiveTlasInstanceUploadWaitCalls++;
				mLastPerfResourceTraceStats.emissiveTlasInstanceUploadWaitMs += waitMs;
			}
			else if (std::strcmp(reason, "emissive_tlas_scratch_resize") == 0)
			{
				mLastPerfResourceTraceStats.emissiveTlasScratchResizeWaitCalls++;
				mLastPerfResourceTraceStats.emissiveTlasScratchResizeWaitMs += waitMs;
			}
			else
			{
				mLastPerfResourceTraceStats.otherWaitCalls++;
				mLastPerfResourceTraceStats.otherWaitMs += waitMs;
			}
		}
		else
		{
			mLastPerfResourceTraceStats.otherWaitCalls++;
			mLastPerfResourceTraceStats.otherWaitMs += waitMs;
		}
	}
}

void NRIRenderer::ReleaseWorldAccelerationBuildScratch(const char* reason)
{
	const uint64_t scratchBytes = mScratchBuffer.memorySize + mTopLevelScratchBuffer.memorySize;
	const uint32_t scratchBuffers =
		(mScratchBuffer.buffer != nullptr ? 1u : 0u) +
		(mTopLevelScratchBuffer.buffer != nullptr ? 1u : 0u);
	if (scratchBuffers == 0)
	{
		return;
	}

	DestroyBufferResource(mScratchBuffer);
	DestroyBufferResource(mTopLevelScratchBuffer);
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT transient scratch: event=release reason=%s buffers=%u bytes=%llu\n",
			reason != nullptr ? reason : "unspecified",
			scratchBuffers,
			(unsigned long long)scratchBytes);
	}
}

void NRIRenderer::NotePerfBufferUpload(const SceneBufferDebugStats* stats, uint64_t size, bool growth, const char* reason, int uploadKind)
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
		int effectiveUploadKind = uploadKind;
		if (effectiveUploadKind < 0)
		{
			if (stats == &mVertexBufferStats)
			{
				effectiveUploadKind = ResidentUploadKind_Vertex;
			}
			else if (stats == &mIndexBufferStats)
			{
				effectiveUploadKind = ResidentUploadKind_Index;
			}
			else if (stats == &mPrimitiveBufferStats)
			{
				effectiveUploadKind = ResidentUploadKind_Primitive;
			}
			else if (stats == &mMaterialBufferStats)
			{
				effectiveUploadKind = ResidentUploadKind_Material;
			}
		}
		switch (effectiveUploadKind)
		{
		case ResidentUploadKind_Vertex: perf.sceneVertexUploadBytes += size; break;
		case ResidentUploadKind_Index: perf.sceneIndexUploadBytes += size; break;
		case ResidentUploadKind_Primitive: perf.scenePrimitiveUploadBytes += size; break;
		case ResidentUploadKind_Material: perf.sceneMaterialUploadBytes += size; break;
		default: break;
		}

		if (reason != nullptr && std::strcmp(reason, "scene_buffer_upload") == 0)
		{
			noteBytes(perf.sceneDynamicUploadCalls, perf.sceneDynamicUploadBytes);
		}
		else if (reason != nullptr && std::strcmp(reason, "resident_chunk_write") == 0)
		{
			noteBytes(perf.sceneResidentChunkUploadCalls, perf.sceneResidentChunkUploadBytes);
		}
		else if (reason != nullptr && std::strcmp(reason, "persistent_voxel_scene_upload") == 0)
		{
			noteBytes(perf.scenePersistentVoxelUploadCalls, perf.scenePersistentVoxelUploadBytes);
		}
		else if (reason != nullptr &&
			(std::strcmp(reason, "persistent_voxel_mesh_vertex") == 0 ||
				std::strcmp(reason, "persistent_voxel_mesh_index") == 0 ||
				std::strcmp(reason, "persistent_voxel_mesh_primitive") == 0 ||
				std::strcmp(reason, "persistent_voxel_material_variant") == 0))
		{
			noteBytes(perf.scenePersistentVoxelVariantUploadCalls, perf.scenePersistentVoxelVariantUploadBytes);
		}
		else if (reason == nullptr)
		{
			noteBytes(perf.sceneStaticRefreshUploadCalls, perf.sceneStaticRefreshUploadBytes);
		}
		else
		{
			noteBytes(perf.sceneOtherUploadCalls, perf.sceneOtherUploadBytes);
		}
	}
	else if (stats == &mEmissivePrimitiveHeaderBufferStats || stats == &mEmissivePrimitiveBufferStats || stats == &mEmissivePrimitiveCdfBufferStats || stats == &mEmissiveMaterialResponseBufferStats || stats == &mEmissiveTlasInstanceBufferStats)
	{
		noteBytes(perf.emissiveUploadCalls, perf.emissiveUploadBytes);
	}
	else
	{
		noteBytes(perf.sceneDataUploadCalls, perf.sceneDataUploadBytes);
	}
}

NRIRendererFrameContext NRIRenderer::BuildFrameContext(int drawmode, bool portal, int debugMode, bool preserveHistory) const
{
	NRIRendererFrameContext context = {};
	context.frameIndex = mFrameIndex;
	context.drawMode = drawmode;
	context.debugMode = debugMode;
	context.portal = portal;
	context.preserveHistory = preserveHistory;
	if (mFrameBuffer != nullptr)
	{
		context.outputWidth = std::max<uint32_t>((uint32_t)mFrameBuffer->mSceneViewport.width, 1u);
		context.outputHeight = std::max<uint32_t>((uint32_t)mFrameBuffer->mSceneViewport.height, 1u);
		if (mFrameBuffer->mActiveTarget != nullptr)
		{
			context.targetWidth = mFrameBuffer->mActiveTarget->width;
			context.targetHeight = mFrameBuffer->mActiveTarget->height;
		}
	}
	return context;
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

	const uint32_t bootstrapMode = GetBootstrapMode();
	const bool bootstrapSimpleView = nri_ptbootstrap && bootstrapMode <= 3u;
	const bool bootstrapCapturedView = nri_ptbootstrap && bootstrapMode >= 4u && bootstrapMode <= 12u;
	const bool bootstrapCapturedDiagnostics = nri_ptbootstrap && bootstrapMode >= 4u && bootstrapMode <= 10u;
	const bool bootstrapCapturedFlat = nri_ptbootstrap && bootstrapMode == 11u;
	const bool bootstrapCapturedBaseColor = nri_ptbootstrap && bootstrapMode == 12u;
	const bool rawTraceDirectScene = !nri_ptbootstrap && nri_ptdirectscene;
	const int debugMode = (int)nri_ptdebug;

	const bool preserveHistory = drawmode != DM_MAINVIEW;
	const NRIRendererFrameContext frameContext = BuildFrameContext(drawmode, portal, debugMode, preserveHistory);
	const NRIPersistentVoxelSettings persistentVoxelSettings = BuildNRIPersistentVoxelSettingsFromCVars();
	const uint32_t traceFrameIndex = frameContext.frameIndex;
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
				frameContext.outputWidth,
				frameContext.outputHeight,
				frameContext.targetWidth,
				frameContext.targetHeight);
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
	mRuntimeMutation.ResetFrameState();
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
	UpdateFrameGenerationHistoryPolicy(frameContext.debugMode, mFrameBuffer->mFrameGeneration.GetPolicy(), frameContext.preserveHistory);

	RefreshMapWorld();
	if (!ApplyStartupMapWorldCorrectionIfNeeded("render-frame-start"))
	{
		LogFallback("PT startup world correction failed.");
		if (preserveHistory)
		{
			restoreHistory();
		}
		return false;
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
	nri_scene::GeometryData surfaceLightGeometry;
	nri_scene::MaterialBridgeData materialBridge;
	nri_scene::MaterialBridgeData runtimeMutationMaterialBridge;
	nri_scene::MaterialBridgeData runtimeSpaceLinkMaterialBridge;
	nri_scene::MaterialBridgeData dynamicMaterialBridge;
	nri_scene::MaterialBridgeData mirrorExtendedDynamicMaterialBridge;
	nri_scene::MaterialBridgeData mirrorPlayerMaterialBridge;
	nri_scene::MaterialBridgeData sceneLightMergedDynamicMaterialBridge;
	nri_scene::MaterialBridgeData mergedDynamicMaterialBridge;
	nri_scene::MaterialBridgeData debugSphereMaterialBridge;
	nri_scene::MaterialBridgeData surfaceLightMaterialBridge;
	nri_scene::GeometryData& overlayGeometry = mSelectOverlayGeometryScratch;
	nri_scene::MaterialBridgeData& overlayMaterialBridge = mSelectOverlayMaterialBridgeScratch;
	nri_scene::MaterialBridgeData combinedMaterialBridge;
	auto& capturedGpuMaterials = mSelectCapturedGpuMaterialScratch;
	auto& dynamicGpuMaterials = mSelectDynamicGpuMaterialScratch;
	auto& persistentVoxelGpuMaterials = mSelectPersistentVoxelGpuMaterialScratch;
	auto& combinedGpuMaterials = mSelectCombinedGpuMaterialScratch;
	auto& refreshedCombinedGpuMaterials = mSelectRefreshedCombinedGpuMaterialScratch;
	capturedGpuMaterials.clear();
	dynamicGpuMaterials.clear();
	persistentVoxelGpuMaterials.clear();
	combinedGpuMaterials.clear();
	refreshedCombinedGpuMaterials.clear();
	nri_scene::ClearGeometryRetainingCapacity(mSelectMirrorPlayerGeometryScratch);
	nri_scene::ClearGeometryRetainingCapacity(mSelectOverlayGeometryScratch);
	nri_scene::ClearMaterialBridgeRetainingCapacity(mSelectOverlayMaterialBridgeScratch);
	mSelectTopLevelInstanceScratch.clear();
	mSelectSceneInstanceScratch.clear();
	mSelectCapturedTopLevelInstanceScratch.clear();
	mSelectCapturedSceneInstanceScratch.clear();
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
	nri_scene::GeometryData& mirrorPlayerGeometry = mSelectMirrorPlayerGeometryScratch;
	MirrorPlayerCaptureStats mirrorPlayerCaptureStats = {};
	nri_scene::GeometryBuildTraceStats mirrorPlayerGeometryTraceStats = {};
	std::vector<SceneBufferUploadDomainSpan> sceneUploadDomainSpans;
	uint32_t activeStaticProbePrimitiveCount = 0;
	EmissiveSamplingBuildContext emissiveSamplingContext = {};
	bool sceneLightUsesStaticMapScene = false;
	nri_scene::SceneDebugStats activeStats = {};
	bool paletteReady = true;
	bool texturesReady = true;
	bool buffersReady = true;
	bool accelerationReady = true;
	uint32_t combinedOverlayMaterialOffset = 0;
	bool usingPersistentDynamicEmissiveCache = false;
	bool liveDynamicHasEmissive = false;
	bool hasPersistentVoxelBatch = false;
	bool appendPersistentVoxelSceneLights = false;
	uint32_t selectedStaticSceneInstanceCount = 0;
	uint32_t selectedDynamicSceneInstanceCount = 0;
	uint32_t selectedPersistentVoxelSceneInstanceCount = 0;
	uint32_t selectedSceneInstanceCount = 0;
	uint32_t selectedTlasInstanceCount = 0;

	{
		ScopedPtPerfTimer sceneSelectTimer(mLastPerfShellTraceStats.sceneSelectMs);
		const bool hasStaticMapScene = allowStaticMapScene && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectStaticMapMs);
			return EnsureStaticMapScene();
		}();
		if (hasStaticMapScene)
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

			bool residentStaticWorldGeometryChanged = false;
			const bool deferOverlayThisFrame = mUploadedStaticMapSceneLastFrame || mBuiltStaticMapSceneASLastFrame;
			const bool hasRuntimeSpaceLinkOverlay = !deferOverlayThisFrame && [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeSpaceLinkMs);
				return BuildRuntimeSpaceLinkOverlay(di, runtimeSpaceLinkGeometry, runtimeSpaceLinkMaterialBridge);
			}();
			mLastPerfShellTraceStats.runtimeSpaceLinkPrimitiveCount = (uint32_t)runtimeSpaceLinkGeometry.primitives.size();
			mLastPerfShellTraceStats.runtimeSpaceLinkMaterialCount = (uint32_t)runtimeSpaceLinkMaterialBridge.materials.size();
			const bool hasRuntimeMutationOverlay = [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationMs);
				return mRuntimeMutation.BuildOverlay(
					BuildRuntimeMutationOverlayServices(),
					runtimeMutationGeometry,
					runtimeMutationMaterialBridge,
					&residentStaticWorldGeometryChanged);
			}();
			const bool hasDynamicScene = !deferOverlayThisFrame && [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.dynamicCaptureMs);
				(void)nri_scene::ConsumeDynamicCapturePerfStats();
				const bool captured = nri_scene::CaptureDynamicScene(di, dynamicSceneView);
				const nri_scene::DynamicCapturePerfStats captureStats = nri_scene::ConsumeDynamicCapturePerfStats();
				mLastPerfShellTraceStats.dynamicCaptureCalls += captureStats.calls;
				mLastPerfShellTraceStats.dynamicCaptureWallSurfaces += captureStats.wallSurfaces;
				mLastPerfShellTraceStats.dynamicCaptureFlatSurfaces += captureStats.flatSurfaces;
				mLastPerfShellTraceStats.dynamicCaptureSpriteSurfaces += captureStats.spriteSurfaces;
				mLastPerfShellTraceStats.dynamicCaptureVoxelProxySurfaces += captureStats.voxelProxySurfaces;
				mLastPerfShellTraceStats.dynamicCaptureUnsupportedModelSurfaces += captureStats.unsupportedModelSurfaces;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCacheStores += captureStats.voxelCacheStores;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCacheRebuilds += captureStats.voxelCacheRebuilds;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCacheDeferred += captureStats.voxelCacheDeferred;
				mLastPerfShellTraceStats.dynamicCaptureVoxelMeshBuilds += captureStats.voxelMeshCacheBuilds;
				mLastPerfShellTraceStats.dynamicCaptureVoxelMeshDeferred += captureStats.voxelMeshCacheDeferred;
				mLastPerfShellTraceStats.dynamicCaptureVoxelMeshInvalid += captureStats.voxelMeshCacheInvalid;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCanonicalSurfaceBuilds += captureStats.voxelCanonicalSurfaceBuilds;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCanonicalSurfaceHits += captureStats.voxelCanonicalSurfaceHits;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCanonicalSurfaceInvalid += captureStats.voxelCanonicalSurfaceInvalid;
				mLastPerfShellTraceStats.voxelCacheActorEntries = dynamicSceneView.stats.voxelCacheEntries;
				mLastPerfShellTraceStats.voxelCacheActorSurfaces = dynamicSceneView.stats.voxelCacheActorSurfaces;
				mLastPerfShellTraceStats.voxelCacheUniqueMeshKeys = dynamicSceneView.stats.voxelCacheUniqueMeshKeys;
				mLastPerfShellTraceStats.voxelCacheUniqueMaterialKeys = dynamicSceneView.stats.voxelCacheUniqueMaterialKeys;
				mLastPerfShellTraceStats.voxelCacheLocalSpaceSurfaces = dynamicSceneView.stats.voxelCacheLocalSpaceSurfaces;
				mLastPerfShellTraceStats.voxelCacheBakedTransformSurfaces = dynamicSceneView.stats.voxelCacheBakedTransformSurfaces;
				mLastPerfShellTraceStats.voxelCacheUnknownSpaceSurfaces = dynamicSceneView.stats.voxelCacheUnknownSpaceSurfaces;
				mLastPerfShellTraceStats.voxelCacheTransformKeyedSurfaces = dynamicSceneView.stats.voxelCacheTransformKeyedSurfaces;
				mLastPerfShellTraceStats.voxelCacheUniqueTransformBases = dynamicSceneView.stats.voxelCacheUniqueTransformBases;
				mLastPerfShellTraceStats.voxelCacheInvariantWarnings = dynamicSceneView.stats.voxelCacheInvariantWarnings;
				mLastPerfShellTraceStats.voxelCacheActorPrimitives = dynamicSceneView.stats.voxelCachePrimitives;
				mLastPerfShellTraceStats.voxelCacheDuplicatedVertexBytes = dynamicSceneView.stats.voxelCacheDuplicatedVertexBytes;
				mLastPerfShellTraceStats.voxelCacheDuplicatedIndexBytes = dynamicSceneView.stats.voxelCacheDuplicatedIndexBytes;
				mLastPerfShellTraceStats.voxelCacheDuplicatedPrimitiveBytes = dynamicSceneView.stats.voxelCacheDuplicatedPrimitiveBytes;
				mLastPerfShellTraceStats.voxelCacheDuplicatedTotalBytes = dynamicSceneView.stats.voxelCacheDuplicatedTotalBytes;
				mLastPerfShellTraceStats.voxelCacheDuplicateTopCount = dynamicSceneView.stats.voxelCacheDuplicateTopCount;
				mLastPerfShellTraceStats.voxelCacheDuplicateTopEntries = dynamicSceneView.stats.voxelCacheDuplicateTopEntries;
				mLastPerfShellTraceStats.dynamicVoxelEscapeActorCount = dynamicSceneView.stats.dynamicVoxelEscapeActorCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapeEligibleActorCount = dynamicSceneView.stats.dynamicVoxelEscapeEligibleActorCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapeForcedActorCount = dynamicSceneView.stats.dynamicVoxelEscapeForcedActorCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapePrimitiveCount = dynamicSceneView.stats.dynamicVoxelEscapePrimitiveCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapeVertexBytes = dynamicSceneView.stats.dynamicVoxelEscapeVertexBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapeIndexBytes = dynamicSceneView.stats.dynamicVoxelEscapeIndexBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapePrimitiveBytes = dynamicSceneView.stats.dynamicVoxelEscapePrimitiveBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapeMaterialBytes = dynamicSceneView.stats.dynamicVoxelEscapeMaterialBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapeTotalBytes = dynamicSceneView.stats.dynamicVoxelEscapeTotalBytes;
				mLastPerfShellTraceStats.dynamicVoxelExpectedEscapeActorCount = dynamicSceneView.stats.dynamicVoxelExpectedEscapeActorCount;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapeActorCount = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapeActorCount;
				mLastPerfShellTraceStats.dynamicVoxelExpectedEscapePrimitiveCount = dynamicSceneView.stats.dynamicVoxelExpectedEscapePrimitiveCount;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapePrimitiveCount = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapePrimitiveCount;
				mLastPerfShellTraceStats.dynamicVoxelExpectedEscapeTotalBytes = dynamicSceneView.stats.dynamicVoxelExpectedEscapeTotalBytes;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapeTotalBytes = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapeTotalBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapeTopCount = dynamicSceneView.stats.dynamicVoxelEscapeTopCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapeTopEntries = dynamicSceneView.stats.dynamicVoxelEscapeTopEntries;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapeTopCount = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapeTopCount;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapeTopEntries = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapeTopEntries;
				mLastPerfShellTraceStats.dynamicCaptureCountMs += captureStats.countMs;
				mLastPerfShellTraceStats.dynamicCaptureWallsMs += captureStats.wallsMs;
				mLastPerfShellTraceStats.dynamicCaptureFlatsMs += captureStats.flatsMs;
				mLastPerfShellTraceStats.dynamicCaptureFacingSpritesMs += captureStats.facingSpritesMs;
				mLastPerfShellTraceStats.dynamicCaptureModelSpritesMs += captureStats.modelSpritesMs;
				mLastPerfShellTraceStats.dynamicCaptureModelClassifyMs += captureStats.modelClassifyMs;
				mLastPerfShellTraceStats.dynamicCaptureModelMeshMs += captureStats.modelMeshMs;
				mLastPerfShellTraceStats.dynamicCaptureModelSurfaceMs += captureStats.modelSurfaceMs;
				mLastPerfShellTraceStats.dynamicCaptureModelStoreMs += captureStats.modelStoreMs;
				mLastPerfShellTraceStats.dynamicCaptureVoxelFrameMs += captureStats.voxelFrameMs;
				mLastPerfShellTraceStats.dynamicCaptureStatsMs += captureStats.statsMs;
				return captured;
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
		HWPortal* const visibleMirrorPortal = !deferOverlayThisFrame ? [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectMirrorPortalMs);
				return SelectPrimaryMirrorPortal(di, visibleMirrorPortalCandidates, selectedVisibleMirrorWallIndex, preferredMirrorWallIndex);
			}() :
			nullptr;
		mHasVisibleMirrorPortalLastFrame = visibleMirrorPortal != nullptr;
		const bool hasMirrorExtendedDynamicScene = !deferOverlayThisFrame && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectMirrorCaptureMs);
			return CaptureMirrorExtendedDynamicScene(
				di,
				visibleMirrorPortal,
				selectedVisibleMirrorWallIndex,
				hasDynamicScene ? &dynamicSceneView : nullptr,
				mFrameIndex,
				mirrorExtendedDynamicSceneView);
		}();
		const bool hasMirrorPlayerScene = !deferOverlayThisFrame && IsMirrorPlayerPreviewCaptureEnabled() && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectMirrorCaptureMs);
			ScopedPtPerfTimer mirrorPlayerTimer(mLastPerfShellTraceStats.mirrorPlayerCaptureMs);
			return CaptureMirrorPlayerDynamicScene(
				di,
				visibleMirrorPortal,
				selectedVisibleMirrorWallIndex,
				visibleMirrorPortalCandidates,
				mirrorPlayerSceneView,
				&mirrorPlayerCaptureStats);
		}();
		if (hasDynamicScene)
		{
			{
				Clocker clock(NriPTGeometryBuild);
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildDynamicLiveMs);
				nri_scene::BuildGeometry(dynamicSceneView, dynamicGeometry);
				AssignGeometryPortalIndices(mMapWorld, dynamicGeometry);
			}
			mLastPerfShellTraceStats.geometryBuildDynamicLivePrimitives += (uint32_t)dynamicGeometry.primitives.size();

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
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildMirrorExtendedMs);
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
				ScopedPtPerfTimer mergePerfTimer(mLastPerfShellTraceStats.sceneSelectLightMergeMs);
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
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildMirrorPlayerMs);
				{
					ScopedPtPerfTimer buildTimer(mLastPerfShellTraceStats.mirrorPlayerGeometryBuildMs);
					nri_scene::BuildGeometry(mirrorPlayerSceneView, mirrorPlayerGeometry, &mirrorPlayerGeometryTraceStats, true);
				}
				{
					ScopedPtPerfTimer portalTimer(mLastPerfShellTraceStats.mirrorPlayerPortalAssignMs);
					AssignGeometryPortalIndices(mMapWorld, mirrorPlayerGeometry);
				}
				mLastPerfShellTraceStats.mirrorPlayerGeometryBuildWallMs = mirrorPlayerGeometryTraceStats.wallMs;
				mLastPerfShellTraceStats.mirrorPlayerGeometryBuildFlatMs = mirrorPlayerGeometryTraceStats.flatMs;
				mLastPerfShellTraceStats.mirrorPlayerGeometryBuildSpriteMs = mirrorPlayerGeometryTraceStats.spriteMs;
				mLastPerfShellTraceStats.mirrorPlayerCaptureRawFacingSprites = mirrorPlayerCaptureStats.rawFacingSprites;
				mLastPerfShellTraceStats.mirrorPlayerCaptureRawVoxelSprites = mirrorPlayerCaptureStats.rawVoxelSprites;
				mLastPerfShellTraceStats.mirrorPlayerCaptureSurfaces = mirrorPlayerCaptureStats.capturedSurfaceCount;
				mLastPerfShellTraceStats.mirrorPlayerCaptureMatchingActorSurfaces = mirrorPlayerCaptureStats.capturedMatchingActorSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerCaptureOtherActorSurfaces = mirrorPlayerCaptureStats.capturedOtherActorSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerCaptureActorlessSurfaces = mirrorPlayerCaptureStats.capturedActorlessSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerCaptureFilteredSurfaces = mirrorPlayerCaptureStats.filteredSurfaceCount;
				mLastPerfShellTraceStats.mirrorPlayerGeometryWallSurfaces = mirrorPlayerGeometryTraceStats.wallSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometryFlatSurfaces = mirrorPlayerGeometryTraceStats.flatSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometrySpriteSurfaces = mirrorPlayerGeometryTraceStats.spriteSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometryIndexedSurfaces = mirrorPlayerGeometryTraceStats.indexedSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometryTriangleFanSurfaces = mirrorPlayerGeometryTraceStats.triangleFanSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometrySpriteStripSurfaces = mirrorPlayerGeometryTraceStats.spriteStripSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometrySkippedSurfaces = mirrorPlayerGeometryTraceStats.skippedSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometrySourceVertices = mirrorPlayerGeometryTraceStats.sourceVertexCount;
				mLastPerfShellTraceStats.mirrorPlayerGeometrySourceIndices = mirrorPlayerGeometryTraceStats.sourceIndexCount;
				mLastPerfShellTraceStats.mirrorPlayerGeometryVertexGrowths = mirrorPlayerGeometryTraceStats.vertexCapacityGrowths;
				mLastPerfShellTraceStats.mirrorPlayerGeometryIndexGrowths = mirrorPlayerGeometryTraceStats.indexCapacityGrowths;
				mLastPerfShellTraceStats.mirrorPlayerGeometryPrimitiveGrowths = mirrorPlayerGeometryTraceStats.primitiveCapacityGrowths;
				mLastPerfShellTraceStats.mirrorPlayerGeometryProvenanceGrowths = mirrorPlayerGeometryTraceStats.provenanceCapacityGrowths;
			}

			if (!mirrorPlayerGeometry.primitives.empty())
			{
				Clocker clock(NriPTMaterialBuild);
				ScopedPtPerfTimer materialTimer(mLastPerfShellTraceStats.mirrorPlayerMaterialBuildMs);
				BuildMaterialsWithActorOverrides(mirrorPlayerSceneView, mirrorPlayerMaterialBridge, "mirror_player");
			}
		}

		hasPersistentVoxelBatch = !deferOverlayThisFrame && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectPersistentVoxelBatchMs);
			const MemoryTelemetry telemetry = GetMemoryTelemetry();
			mPersistentVoxels.PumpAdmissionQueue(
				"runtime",
				mMapWorld.buildSerial,
				mFrameIndex,
				persistentVoxelSettings,
				telemetry.totalTrackedBytes,
				mFrameBuffer != nullptr ? mFrameBuffer->GetAdapterLocalBudgetBytes() : 0ull,
				(int)nri_ptloadingtrace,
				(bool)nri_voxelstats,
				BuildPersistentVoxelResetServices(),
				BuildPersistentVoxelAdmissionServices());
			return EnsurePersistentVoxelBatch();
		}();

		PersistentDynamicSurfaceStats persistentDynamicStats = {};
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectPersistentEmissiveMs);
			PrunePersistentDynamicEmissiveCacheToLiveActors();
			persistentDynamicStats = GatherPersistentDynamicEmissiveSurfaceStats();
		}
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
				ScopedPtPerfTimer mergePerfTimer(mLastPerfShellTraceStats.sceneSelectDynamicMergeMs);
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
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildMergedDynamicMs);
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
				ScopedPtPerfTimer mergePerfTimer(mLastPerfShellTraceStats.sceneSelectLightMergeMs);
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

		if (hasPersistentVoxelBatch && mPersistentVoxels.HasValidBatch())
		{
			appendPersistentVoxelSceneLights = true;
		}

		const bool hasActiveDynamicOverlay =
			activeDynamicGeometry != nullptr &&
			!activeDynamicGeometry->primitives.empty() &&
			activeDynamicMaterials != nullptr;
		const bool hasPersistentVoxelOverlay =
			hasPersistentVoxelBatch &&
			mPersistentVoxels.HasRenderableOverlay();
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
		const bool hasSurfaceLightOverlay = !deferOverlayThisFrame &&
			BuildSurfaceLightOverlay(surfaceLightGeometry, surfaceLightMaterialBridge);

		if (hasPersistentVoxelOverlay || hasRuntimeSpaceLinkOverlay || hasRuntimeMutationOverlay || hasActiveDynamicOverlay || hasMirrorExtendedDynamicOverlay || hasMirrorPlayerOverlay || hasRuntimeDebugSphereOverlay || hasSurfaceLightOverlay)
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.overlayAssembleMs);

			{
				ScopedPtPerfTimer appendTimer(mLastPerfShellTraceStats.overlayAppendMs);
				{
					ScopedPtPerfTimer resetTimer(mLastPerfShellTraceStats.overlayAppendResetMs);
					nri_scene::ClearGeometryRetainingCapacity(overlayGeometry);
					nri_scene::ClearMaterialBridgeRetainingCapacity(overlayMaterialBridge);
				}

				auto appendOverlaySource =
					[&](
						const nri_scene::GeometryData* geometry,
						const SceneBufferUploadProducerStamp* producerStamp,
						const nri_scene::MaterialBridgeData& materials,
						double& totalMs,
						double& geometryMs,
						double& materialMs,
						uint32_t& primitiveCount,
						uint32_t& materialCount,
						PerfShellTraceStats::OverlayAppendSourceTraceEntry& sourceTrace,
						SceneBufferUploadDomain uploadDomain)
				{
					NRISceneContribution contribution = {};
					contribution.geometry = geometry;
					contribution.producerStamp = producerStamp;
					contribution.materials = &materials;
					contribution.uploadDomain = uploadDomain;
					NRISceneContributionAppendStats appendStats = {};
					appendStats.totalMs = &totalMs;
					appendStats.geometryMs = &geometryMs;
					appendStats.materialMs = &materialMs;
					appendStats.primitiveCount = &primitiveCount;
					appendStats.materialCount = &materialCount;
					appendStats.sourceTrace = &sourceTrace;
					AppendNRISceneContribution(contribution, appendStats, overlayGeometry, overlayMaterialBridge, sceneUploadDomainSpans);
				};

				{
					ScopedPtPerfTimer sourceAggregateTimer(mLastPerfShellTraceStats.overlayAppendSourcesMs);

					const auto buildProducerStamp =
						[&](const nri_scene::SceneView& sceneView, double& timerMs) -> SceneBufferUploadProducerStamp
					{
						ScopedPtPerfTimer aggregateTimer(mLastPerfShellTraceStats.overlayAppendProducerStampMs);
						ScopedPtPerfTimer sourceTimer(timerMs);
						const SceneViewUploadStampBuildResult built = BuildSceneViewUploadProducerStamp(sceneView, mMapWorld.buildSerial);
						SceneBufferUploadProducerStamp stamp = {};
						stamp.vertexPayloadStamp = built.vertexPayloadStamp;
						stamp.indexPayloadStamp = built.indexPayloadStamp;
						stamp.primitivePayloadStamp = built.primitivePayloadStamp;
						stamp.primitiveProvenanceStamp = built.primitiveProvenanceStamp;
						stamp.materialPayloadStamp = built.materialPayloadStamp;
						return stamp;
					};
					const auto buildMirrorPlayerProducerStamp =
						[&]() -> SceneBufferUploadProducerStamp
					{
						ScopedPtPerfTimer aggregateTimer(mLastPerfShellTraceStats.overlayAppendProducerStampMs);
						ScopedPtPerfTimer sourceTimer(mLastPerfShellTraceStats.overlayAppendMirrorPlayerStampMs);
						const SceneViewUploadStampBuildResult built = BuildMirrorPlayerUploadProducerStamp(
							mirrorPlayerGeometry,
							mirrorPlayerMaterialBridge,
							mFrameIndex,
							mMapWorld.buildSerial);
						SceneBufferUploadProducerStamp stamp = {};
						stamp.vertexPayloadStamp = built.vertexPayloadStamp;
						stamp.indexPayloadStamp = built.indexPayloadStamp;
						stamp.primitivePayloadStamp = built.primitivePayloadStamp;
						stamp.primitiveProvenanceStamp = built.primitiveProvenanceStamp;
						stamp.materialPayloadStamp = built.materialPayloadStamp;
						return stamp;
					};
					const SceneBufferUploadProducerStamp dynamicStamp =
						hasActiveDynamicOverlay && activeDynamicSceneView != nullptr ? buildProducerStamp(*activeDynamicSceneView, mLastPerfShellTraceStats.overlayAppendDynamicStampMs) : SceneBufferUploadProducerStamp {};
					const SceneBufferUploadProducerStamp mirrorExtendedStamp =
						hasMirrorExtendedDynamicOverlay ? buildProducerStamp(mirrorExtendedDynamicSceneView, mLastPerfShellTraceStats.overlayAppendMirrorExtendedStampMs) : SceneBufferUploadProducerStamp {};
					const SceneBufferUploadProducerStamp mirrorPlayerStamp =
						hasMirrorPlayerOverlay ? buildMirrorPlayerProducerStamp() : SceneBufferUploadProducerStamp {};

					NRISceneContributionReserve overlayReserve = {};
					auto addOverlayReserve =
						[&](const nri_scene::GeometryData* geometry, const nri_scene::MaterialBridgeData& materials)
					{
						NRISceneContribution contribution = {};
						contribution.geometry = geometry;
						contribution.materials = &materials;
						AccumulateNRISceneContributionReserve(contribution, overlayReserve);
					};

					if (hasRuntimeSpaceLinkOverlay)
					{
						addOverlayReserve(&runtimeSpaceLinkGeometry, runtimeSpaceLinkMaterialBridge);
					}
					if (hasRuntimeMutationOverlay)
					{
						addOverlayReserve(&runtimeMutationGeometry, runtimeMutationMaterialBridge);
					}
					if (hasActiveDynamicOverlay)
					{
						addOverlayReserve(activeDynamicGeometry, *activeDynamicMaterials);
					}
					if (hasMirrorExtendedDynamicOverlay)
					{
						addOverlayReserve(&mirrorExtendedDynamicGeometry, mirrorExtendedDynamicMaterialBridge);
					}
					if (hasMirrorPlayerOverlay)
					{
						addOverlayReserve(&mirrorPlayerGeometry, mirrorPlayerMaterialBridge);
					}
					if (hasRuntimeDebugSphereOverlay)
					{
						addOverlayReserve(&debugSphereGeometry, debugSphereMaterialBridge);
					}
					if (hasSurfaceLightOverlay)
					{
						addOverlayReserve(&surfaceLightGeometry, surfaceLightMaterialBridge);
					}
					ReserveNRISceneContributionCapacity(overlayReserve, overlayGeometry, overlayMaterialBridge);

					if (hasRuntimeSpaceLinkOverlay)
					{
						appendOverlaySource(
							&runtimeSpaceLinkGeometry,
							nullptr,
							runtimeSpaceLinkMaterialBridge,
							mLastPerfShellTraceStats.overlayRuntimeSpaceLinkMs,
							mLastPerfShellTraceStats.overlayRuntimeSpaceLinkGeometryMs,
							mLastPerfShellTraceStats.overlayRuntimeSpaceLinkMaterialMs,
							mLastPerfShellTraceStats.overlayRuntimeSpaceLinkPrimitiveCount,
							mLastPerfShellTraceStats.overlayRuntimeSpaceLinkMaterialCount,
							mLastPerfShellTraceStats.overlayRuntimeSpaceLinkAppend,
							SceneBufferUploadDomain::StaticOverlay);
					}

					if (hasRuntimeMutationOverlay)
					{
						appendOverlaySource(
							&runtimeMutationGeometry,
							nullptr,
							runtimeMutationMaterialBridge,
							mLastPerfShellTraceStats.overlayRuntimeMutationMs,
							mLastPerfShellTraceStats.overlayRuntimeMutationGeometryMs,
							mLastPerfShellTraceStats.overlayRuntimeMutationMaterialMs,
							mLastPerfShellTraceStats.overlayRuntimeMutationPrimitiveCount,
							mLastPerfShellTraceStats.overlayRuntimeMutationMaterialCount,
							mLastPerfShellTraceStats.overlayRuntimeMutationAppend,
							SceneBufferUploadDomain::RuntimeMutation);
					}

					if (hasActiveDynamicOverlay)
					{
						appendOverlaySource(
							activeDynamicGeometry,
							&dynamicStamp,
							*activeDynamicMaterials,
							mLastPerfShellTraceStats.overlayDynamicMs,
							mLastPerfShellTraceStats.overlayDynamicGeometryMs,
							mLastPerfShellTraceStats.overlayDynamicMaterialMs,
							mLastPerfShellTraceStats.overlayDynamicPrimitiveCount,
							mLastPerfShellTraceStats.overlayDynamicMaterialCount,
							mLastPerfShellTraceStats.overlayDynamicAppend,
							SceneBufferUploadDomain::Dynamic);
					}

					if (hasMirrorExtendedDynamicOverlay)
					{
						appendOverlaySource(
							&mirrorExtendedDynamicGeometry,
							&mirrorExtendedStamp,
							mirrorExtendedDynamicMaterialBridge,
							mLastPerfShellTraceStats.overlayMirrorExtendedMs,
							mLastPerfShellTraceStats.overlayMirrorExtendedGeometryMs,
							mLastPerfShellTraceStats.overlayMirrorExtendedMaterialMs,
							mLastPerfShellTraceStats.overlayMirrorExtendedPrimitiveCount,
							mLastPerfShellTraceStats.overlayMirrorExtendedMaterialCount,
							mLastPerfShellTraceStats.overlayMirrorExtendedAppend,
							SceneBufferUploadDomain::MirrorExtended);
					}

					if (hasMirrorPlayerOverlay)
					{
						appendOverlaySource(
							&mirrorPlayerGeometry,
							&mirrorPlayerStamp,
							mirrorPlayerMaterialBridge,
							mLastPerfShellTraceStats.overlayMirrorPlayerMs,
							mLastPerfShellTraceStats.overlayMirrorPlayerGeometryMs,
							mLastPerfShellTraceStats.overlayMirrorPlayerMaterialMs,
							mLastPerfShellTraceStats.overlayMirrorPlayerPrimitiveCount,
							mLastPerfShellTraceStats.overlayMirrorPlayerMaterialCount,
							mLastPerfShellTraceStats.overlayMirrorPlayerAppend,
							SceneBufferUploadDomain::MirrorPlayer);
					}

					if (hasRuntimeDebugSphereOverlay)
					{
						appendOverlaySource(
							&debugSphereGeometry,
							nullptr,
							debugSphereMaterialBridge,
							mLastPerfShellTraceStats.overlayDebugSphereMs,
							mLastPerfShellTraceStats.overlayDebugSphereGeometryMs,
							mLastPerfShellTraceStats.overlayDebugSphereMaterialMs,
							mLastPerfShellTraceStats.overlayDebugSpherePrimitiveCount,
							mLastPerfShellTraceStats.overlayDebugSphereMaterialCount,
							mLastPerfShellTraceStats.overlayDebugSphereAppend,
							SceneBufferUploadDomain::StaticOverlay);
					}

					if (hasSurfaceLightOverlay)
					{
						double surfaceLightOverlayMs = 0.0;
						double surfaceLightGeometryMs = 0.0;
						double surfaceLightMaterialMs = 0.0;
						uint32_t surfaceLightPrimitiveCount = 0;
						uint32_t surfaceLightMaterialCount = 0;
						PerfShellTraceStats::OverlayAppendSourceTraceEntry surfaceLightAppend = {};
						appendOverlaySource(
							&surfaceLightGeometry,
							nullptr,
							surfaceLightMaterialBridge,
							surfaceLightOverlayMs,
							surfaceLightGeometryMs,
							surfaceLightMaterialMs,
							surfaceLightPrimitiveCount,
							surfaceLightMaterialCount,
							surfaceLightAppend,
							SceneBufferUploadDomain::StaticOverlay);
					}
				}

				{
					ScopedPtPerfTimer bookkeepingTimer(mLastPerfShellTraceStats.overlayAppendBookkeepingMs);
					if (hasPersistentVoxelOverlay)
					{
						const NRIPersistentVoxelOverlayStats persistentVoxelOverlayStats = mPersistentVoxels.BuildOverlayStats();
						mLastPerfShellTraceStats.overlayPersistentVoxelActorCount = persistentVoxelOverlayStats.actorCount;
						mLastPerfShellTraceStats.overlayPersistentVoxelPrimitiveCount = persistentVoxelOverlayStats.primitiveCount;
						mLastPerfShellTraceStats.overlayPersistentVoxelMaterialCount = persistentVoxelOverlayStats.materialCount;
						mLastPerfShellTraceStats.overlayPersistentVoxelAppend.primitiveCount = persistentVoxelOverlayStats.primitiveCount;
						mLastPerfShellTraceStats.overlayPersistentVoxelAppend.materialCount = persistentVoxelOverlayStats.materialCount;
						mLastPerfShellTraceStats.overlayPersistentVoxelAppend.indexCount = persistentVoxelOverlayStats.indexCount;
						mLastPerfShellTraceStats.overlayPersistentVoxelAppend.byteCount = persistentVoxelOverlayStats.byteCount;
					}
					mLastPerfShellTraceStats.overlayPrimitiveCount = (uint32_t)overlayGeometry.primitives.size();
					mLastPerfShellTraceStats.overlayMaterialCount = (uint32_t)overlayMaterialBridge.materials.size();
				}
			}

			auto& instances = mSelectTopLevelInstanceScratch;
			auto& sceneInstances = mSelectSceneInstanceScratch;
			instances.clear();
			sceneInstances.clear();
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectStaticInstancesMs);
				BuildStaticMapInstances(instances, sceneInstances);
			}
			const uint32_t staticSceneInstanceBaselineCount = (uint32_t)sceneInstances.size();
			selectedStaticSceneInstanceCount = staticSceneInstanceBaselineCount;
			selectedSceneInstanceCount = (uint32_t)sceneInstances.size();
			selectedTlasInstanceCount = (uint32_t)instances.size();
			bool selectedSceneHasDynamicOverlay = false;

			if (overlayGeometry.primitives.empty() && !hasPersistentVoxelOverlay)
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
				{
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectMaterialBridgeMs);
					combinedMaterialBridge = mStaticMapScene.materialBridge;
					combinedOverlayMaterialOffset = (uint32_t)combinedMaterialBridge.materials.size();
					if (hasPersistentVoxelOverlay)
					{
						mPersistentVoxels.AppendMaterialBridgeTo(combinedMaterialBridge);
						combinedOverlayMaterialOffset = (uint32_t)combinedMaterialBridge.materials.size();
					}
					nri_scene::AppendMaterialBridge(overlayMaterialBridge, combinedMaterialBridge);
				}
				paletteReady = [&]()
				{
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectPaletteMs);
					return EnsurePaletteTexture(combinedMaterialBridge);
				}();
				if (ShouldTraceSkyPerf())
				{
					gRendererSkyPerfTraceStats.combinedOverlayTextureBuilds++;
				}
				texturesReady = paletteReady && [&]()
				{
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectTexturesMs);
					return EnsureSceneTextures(mStaticMapScene.sceneView, combinedMaterialBridge, combinedGpuMaterials, false, "static_map_overlay_combined");
				}();
				dynamicGpuMaterials.clear();
				persistentVoxelGpuMaterials.clear();
				if (texturesReady)
				{
					{
						ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectMaterialSplitMs);
						const size_t staticMaterialCount = mStaticMapScene.gpuMaterials.size();
						const size_t persistentVoxelMaterialCount = hasPersistentVoxelOverlay ? mPersistentVoxels.OverlayMaterialCount() : 0u;
						if (combinedGpuMaterials.size() < staticMaterialCount + persistentVoxelMaterialCount)
						{
							texturesReady = false;
						}
						else
						{
							persistentVoxelGpuMaterials.assign(
								combinedGpuMaterials.begin() + staticMaterialCount,
								combinedGpuMaterials.begin() + staticMaterialCount + persistentVoxelMaterialCount);
							dynamicGpuMaterials.assign(combinedGpuMaterials.begin() + staticMaterialCount + persistentVoxelMaterialCount, combinedGpuMaterials.end());
						}
					}
				}
				buffersReady = texturesReady && [&]()
				{
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadMs);
					return UploadSceneBuffers(overlayGeometry, dynamicGpuMaterials, &sceneUploadDomainSpans) &&
						(!hasPersistentVoxelOverlay || UploadPersistentVoxelArenaMaterialBuffers(persistentVoxelGpuMaterials));
				}();
				accelerationReady = false;
				const uint32_t liveOverlayPrimitiveCount = (uint32_t)overlayGeometry.primitives.size();
				const uint32_t liveOverlayIndexOffset = 0u;
				const uint32_t liveOverlayIndexCount = (uint32_t)overlayGeometry.indices.size();
				NRIAccelerationStructureResource& dynamicBottomLevelAS = GetCurrentDynamicBottomLevelAS();
				if (buffersReady)
				{
					bool persistentVoxelAsReady = true;
					bool dynamicAsReady = true;
					if (hasPersistentVoxelOverlay)
					{
						ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.persistentVoxelAsMs);
						NRIPersistentVoxelAccelerationBuildStats persistentVoxelAsStats = {};
						persistentVoxelAsReady = mPersistentVoxels.BuildAccelerationStructures(
							mFrameIndex,
							(bool)nri_voxelstats,
							BuildPersistentVoxelResetServices(),
							BuildPersistentVoxelAccelerationServices(),
							persistentVoxelAsStats);
						mLastPerfShellTraceStats.persistentVoxelAsCalls += persistentVoxelAsStats.calls;
						mLastPerfShellTraceStats.persistentVoxelAsBuilds += persistentVoxelAsStats.builds;
						mLastPerfShellTraceStats.persistentVoxelAsUniqueMeshBuilds += persistentVoxelAsStats.uniqueMeshBuilds;
						mLastPerfShellTraceStats.persistentVoxelAsInstances += persistentVoxelAsStats.instances;
					}
					if (liveOverlayPrimitiveCount > 0)
					{
						mLastPerfShellTraceStats.dynamicAsRuntimeSpaceLinkPrimitives = mLastPerfShellTraceStats.overlayRuntimeSpaceLinkAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsRuntimeMutationPrimitives = mLastPerfShellTraceStats.overlayRuntimeMutationAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsDynamicPrimitives = mLastPerfShellTraceStats.overlayDynamicAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsMirrorExtendedPrimitives = mLastPerfShellTraceStats.overlayMirrorExtendedAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsMirrorPlayerPrimitives = mLastPerfShellTraceStats.overlayMirrorPlayerAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsDebugSpherePrimitives = mLastPerfShellTraceStats.overlayDebugSphereAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsRuntimeSpaceLinkBytes = mLastPerfShellTraceStats.overlayRuntimeSpaceLinkAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsRuntimeMutationBytes = mLastPerfShellTraceStats.overlayRuntimeMutationAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsDynamicBytes = mLastPerfShellTraceStats.overlayDynamicAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsMirrorExtendedBytes = mLastPerfShellTraceStats.overlayMirrorExtendedAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsMirrorPlayerBytes = mLastPerfShellTraceStats.overlayMirrorPlayerAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsDebugSphereBytes = mLastPerfShellTraceStats.overlayDebugSphereAppend.byteCount;
						dynamicAsReady =
							BuildDynamicAccelerationStructure(
								overlayGeometry,
								liveOverlayIndexOffset,
								liveOverlayIndexCount,
								liveOverlayPrimitiveCount,
								dynamicBottomLevelAS,
								true) &&
							dynamicBottomLevelAS.accelerationStructure != nullptr;
					}
					else
					{
						mLastPerfShellTraceStats.dynamicAsPrimitiveCount = 0;
						mLastPerfShellTraceStats.dynamicAsVertexCount = 0;
						mLastPerfShellTraceStats.dynamicAsIndexCount = 0;
					}
					accelerationReady = persistentVoxelAsReady && dynamicAsReady;
				}
				emissiveSamplingContext.runtimeMutationGeometry = hasRuntimeMutationOverlay ? &runtimeMutationGeometry : nullptr;
				emissiveSamplingContext.runtimeMutationPrimitiveBaseOffset = (uint32_t)runtimeSpaceLinkGeometry.primitives.size();
				emissiveSamplingContext.dynamicGeometry = hasActiveDynamicOverlay ? activeDynamicGeometry : nullptr;
				emissiveSamplingContext.dynamicPrimitiveBaseOffset = (uint32_t)(runtimeSpaceLinkGeometry.primitives.size() + runtimeMutationGeometry.primitives.size());
				if (accelerationReady)
				{
					if (hasPersistentVoxelOverlay)
					{
						ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectInstanceHandlesMs);
						ScopedPtPerfTimer persistentVoxelTlasTimer(mLastPerfShellTraceStats.persistentVoxelTlasInstanceMs);
						NRIPersistentVoxelTlasServices persistentVoxelTlasServices = {};
						persistentVoxelTlasServices.user = this;
						persistentVoxelTlasServices.getAccelerationStructureHandle = [](void* user, const NRIAccelerationStructureResource& resource) -> uint64_t
						{
							NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
							return resource.accelerationStructure != nullptr ?
								renderer->mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*resource.accelerationStructure) :
								0ull;
						};
						NRIPersistentVoxelTlasBuildStats persistentVoxelTlasStats = {};
						if (!mPersistentVoxels.AppendTlasInstances(
							instances,
							sceneInstances,
							mFrameIndex,
							persistentVoxelSettings,
							(bool)nri_voxelstats,
							persistentVoxelTlasServices,
							persistentVoxelTlasStats))
						{
							accelerationReady = false;
						}
						mLastPerfShellTraceStats.persistentVoxelSharedMeshResources = persistentVoxelTlasStats.sharedMeshResourceCount;
						mLastPerfShellTraceStats.persistentVoxelTlasInstances += persistentVoxelTlasStats.instanceCount;
						mLastPerfShellTraceStats.persistentVoxelBakedFallbackInstances += persistentVoxelTlasStats.bakedFallbackInstanceCount;
					}

					if (liveOverlayPrimitiveCount > 0 && dynamicBottomLevelAS.accelerationStructure != nullptr)
					{
						ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectInstanceHandlesMs);
						nri::TopLevelInstance dynamicInstance = {};
						dynamicInstance.transform[0][0] = 1.0f;
						dynamicInstance.transform[1][1] = 1.0f;
						dynamicInstance.transform[2][2] = 1.0f;
						dynamicInstance.instanceId = (uint32_t)sceneInstances.size();
						dynamicInstance.mask = 0xFF;
						dynamicInstance.shaderBindingTableLocalOffset = 0;
						dynamicInstance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
						dynamicInstance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*dynamicBottomLevelAS.accelerationStructure);
						instances.push_back(dynamicInstance);
						sceneInstances.push_back({ 0u, NRI_SCENE_DATA_SOURCE_DYNAMIC, 0u, UINT32_MAX });
					}

					selectedStaticSceneInstanceCount = 0;
					selectedDynamicSceneInstanceCount = 0;
					selectedPersistentVoxelSceneInstanceCount = 0;
					for (const SceneInstanceData& sceneInstance : sceneInstances)
					{
						if (sceneInstance.dataSource == NRI_SCENE_DATA_SOURCE_STATIC)
						{
							selectedStaticSceneInstanceCount++;
						}
						else if (sceneInstance.dataSource == NRI_SCENE_DATA_SOURCE_DYNAMIC)
						{
							selectedDynamicSceneInstanceCount++;
						}
						else if (sceneInstance.dataSource == NRI_SCENE_DATA_SOURCE_PERSISTENT_VOXEL)
						{
							selectedPersistentVoxelSceneInstanceCount++;
						}
					}
					selectedSceneInstanceCount = (uint32_t)sceneInstances.size();
					selectedTlasInstanceCount = (uint32_t)instances.size();
					const bool hasEffectiveOverlayInstances = sceneInstances.size() > staticSceneInstanceBaselineCount;
					selectedSceneHasDynamicOverlay =
						liveOverlayPrimitiveCount > 0 ||
						selectedDynamicSceneInstanceCount > 0 ||
						selectedPersistentVoxelSceneInstanceCount > 0 ||
						hasEffectiveOverlayInstances;
					if (selectedSceneHasDynamicOverlay)
					{
						accelerationReady =
							BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static | SceneDataBufferMask_Dynamic) &&
							UpdateSceneDataSet(
								mStaticVertexBuffer,
								mStaticIndexBuffer,
								mStaticPrimitiveBuffer,
								mStaticMaterialBuffer,
								GetCurrentDynamicVertexBuffer(),
								GetCurrentDynamicIndexBuffer(),
								GetCurrentDynamicPrimitiveBuffer(),
								GetCurrentDynamicMaterialBuffer(),
								sceneInstances,
								(uint32_t)mStaticMapScene.geometry.primitives.size(),
								(uint32_t)overlayGeometry.primitives.size(),
								(uint32_t)mStaticMapScene.gpuMaterials.size(),
								(uint32_t)dynamicGpuMaterials.size(),
								"static_plus_overlay_scene");
					}
					else
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
								"static_only_effective_scene");
					}
				}
			}

			if (overlayGeometry.primitives.empty() || texturesReady)
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectTexturePrepMs);
				PrepareSceneTextureInputsForCompute();
			}

			if (paletteReady && texturesReady && buffersReady && accelerationReady)
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectStateCommitMs);
				{
					ScopedPtPerfTimer stateFlagsTimer(mLastPerfShellTraceStats.sceneSelectStateCommitFlagsMs);
					mUsedDynamicSceneLastFrame = selectedSceneHasDynamicOverlay;
					mGpuSceneHasDynamicOverlay = selectedSceneHasDynamicOverlay;
					mLastPerfShellTraceStats.sceneSelectStateCommitSelectedDynamic = selectedSceneHasDynamicOverlay ? 1u : 0u;
				}
				{
					NRISceneFrameDynamicStateInputs dynamicStateInputs = {};
					dynamicStateInputs.activeDynamicSceneView = activeDynamicSceneView;
					dynamicStateInputs.activeDynamicGeometry = activeDynamicGeometry;
					dynamicStateInputs.activeDynamicMaterials = activeDynamicMaterials;
					dynamicStateInputs.mirrorExtendedSceneView = hasMirrorExtendedDynamicScene ? &mirrorExtendedDynamicSceneView : nullptr;
					dynamicStateInputs.mirrorExtendedGeometry = hasMirrorExtendedDynamicScene ? &mirrorExtendedDynamicGeometry : nullptr;
					dynamicStateInputs.mirrorExtendedMaterials = hasMirrorExtendedDynamicScene ? &mirrorExtendedDynamicMaterialBridge : nullptr;
					dynamicStateInputs.mirrorPlayerSceneView = hasMirrorPlayerScene ? &mirrorPlayerSceneView : nullptr;
					dynamicStateInputs.mirrorPlayerGeometry = hasMirrorPlayerScene ? &mirrorPlayerGeometry : nullptr;
					dynamicStateInputs.mirrorPlayerMaterials = hasMirrorPlayerScene ? &mirrorPlayerMaterialBridge : nullptr;
					dynamicStateInputs.totalMs = &mLastPerfShellTraceStats.sceneSelectStateCommitDynamicStateMs;
					dynamicStateInputs.dynamicCoreMs = &mLastPerfShellTraceStats.sceneSelectStateCommitDynamicCoreMs;
					dynamicStateInputs.mirrorExtendedMs = &mLastPerfShellTraceStats.sceneSelectStateCommitDynamicMirrorExtendedMs;
					dynamicStateInputs.mirrorPlayerMs = &mLastPerfShellTraceStats.sceneSelectStateCommitDynamicMirrorPlayerMs;
					mDynamicSceneLastFrame = BuildNRISceneFrameDynamicState(dynamicStateInputs, mDynamicSceneLastFrame, mLastPerfShellTraceStats);
				}
				{
					NRISceneFrameGeometrySelectionInputs geometrySelectionInputs = {};
					geometrySelectionInputs.staticBuildSerial = mStaticMapScene.buildSerial;
					geometrySelectionInputs.staticGeometry = &mStaticMapScene.geometry;
					geometrySelectionInputs.staticMaterialBridge = &mStaticMapScene.materialBridge;
					geometrySelectionInputs.staticGpuMaterials = &mStaticMapScene.gpuMaterials;
					geometrySelectionInputs.overlayGeometry = &overlayGeometry;
					geometrySelectionInputs.overlayMaterialOffset = combinedOverlayMaterialOffset;
					geometrySelectionInputs.combinedMaterialBridge = &combinedMaterialBridge;
					geometrySelectionInputs.combinedGpuMaterials = &combinedGpuMaterials;
					geometrySelectionInputs.totalMs = &mLastPerfShellTraceStats.sceneSelectStateCommitGeometryStateMs;
					geometrySelectionInputs.staticCopyMs = &mLastPerfShellTraceStats.sceneSelectStateCommitGeometryStaticCopyMs;
					geometrySelectionInputs.overlayAppendMs = &mLastPerfShellTraceStats.sceneSelectStateCommitGeometryAppendMs;
					geometrySelectionInputs.selectMs = &mLastPerfShellTraceStats.sceneSelectStateCommitGeometrySelectMs;
					const NRISceneFrameGeometrySelection geometrySelection = mSceneFrameGeometry.SelectActiveGeometry(geometrySelectionInputs);
					if (geometrySelection.usedCombinedGeometry)
					{
						mLastPerfShellTraceStats.sceneSelectStateCommitGeometryCombined = 1;
					}
					if (geometrySelection.usedStaticOnlyGeometry)
					{
						mLastPerfShellTraceStats.sceneSelectStateCommitGeometryStaticOnly = 1;
					}
					activeStaticProbePrimitiveCount = geometrySelection.staticProbePrimitiveCount;
					activeGeometry = geometrySelection.geometry;
					activeGpuMaterials = geometrySelection.gpuMaterials;
					activeMaterialBridge = geometrySelection.materialBridge;
					mLastPerfShellTraceStats.sceneSelectStateCommitCombinedPrimitiveCount = geometrySelection.combinedPrimitiveCount;
					mLastPerfShellTraceStats.sceneSelectStateCommitCombinedMaterialCount = geometrySelection.combinedMaterialCount;
				}

				{
					ScopedPtPerfTimer statsTimer(mLastPerfShellTraceStats.sceneSelectStateCommitStatsMs);
					nri_scene::SceneDebugStats persistentVoxelOverlayStats;
					if (hasPersistentVoxelOverlay)
					{
						ScopedPtPerfTimer persistentVoxelStatsTimer(mLastPerfShellTraceStats.sceneSelectStateCommitStatsPersistentVoxelMs);
						persistentVoxelOverlayStats = mPersistentVoxels.BuildOverlayDebugStats();
					}
					NRISceneFrameDebugStatsInputs debugStatsInputs = {};
					debugStatsInputs.staticMapStats = &mStaticMapScene.sceneView.stats;
					debugStatsInputs.deferredDynamicSceneView = !deferOverlayThisFrame ? &dynamicSceneView : nullptr;
					debugStatsInputs.activeDynamicSceneView = activeDynamicSceneView;
					debugStatsInputs.persistentVoxelStats = hasPersistentVoxelOverlay ? &persistentVoxelOverlayStats : nullptr;
					debugStatsInputs.mirrorExtendedSceneView = hasMirrorExtendedDynamicScene ? &mirrorExtendedDynamicSceneView : nullptr;
					debugStatsInputs.mirrorPlayerSceneView = hasMirrorPlayerScene ? &mirrorPlayerSceneView : nullptr;
					debugStatsInputs.baseMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsBaseMs;
					debugStatsInputs.persistentVoxelMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsPersistentVoxelMs;
					debugStatsInputs.mirrorExtendedMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsMirrorExtendedMs;
					debugStatsInputs.mirrorPlayerMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsMirrorPlayerMs;
					debugStatsInputs.mergeMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsMergeMs;
					activeStats = BuildNRISceneFrameDebugStats(debugStatsInputs, mLastPerfShellTraceStats);
				}

				{
					NRISceneFrameGenerationInputs generationInputs = {};
					generationInputs.staticMapBuildSerial = mStaticMapScene.buildSerial;
					generationInputs.runtimeMutationGeneration = mRuntimeMutation.BuildFrameGenerationHash(hasRuntimeMutationOverlay);
					generationInputs.persistentVoxelGeneration = hasPersistentVoxelOverlay ? mPersistentVoxels.BuildSceneGenerationHash() : 0ull;
					generationInputs.frameIndex = mFrameIndex;
					generationInputs.staticAccelerationBuildSerial = mStaticAccelerationBuildSerial;
					generationInputs.renderWidth = mRenderWidth;
					generationInputs.renderHeight = mRenderHeight;
					generationInputs.currentCameraPos = mCurrentCameraPos;
					generationInputs.currentCameraForward = mCurrentCameraForward;
					generationInputs.currentCameraRight = mCurrentCameraRight;
					generationInputs.currentCameraUp = mCurrentCameraUp;
					generationInputs.currentTanHalfFovX = mCurrentTanHalfFovX;
					generationInputs.currentTanHalfFovY = mCurrentTanHalfFovY;
					generationInputs.selectedSceneHasDynamicOverlay = selectedSceneHasDynamicOverlay;
					generationInputs.activeDynamicSceneView = activeDynamicSceneView;
					generationInputs.activeDynamicGeometry = activeDynamicGeometry;
					generationInputs.activeDynamicMaterials = activeDynamicMaterials;
					generationInputs.hasMirrorPlayerScene = hasMirrorPlayerScene;
					generationInputs.mirrorPlayerGeometry = &mirrorPlayerGeometry;
					generationInputs.mirrorPlayerMaterials = &mirrorPlayerMaterialBridge;
					generationInputs.activeMaterialBridge = activeMaterialBridge;
					generationInputs.activeGpuMaterials = activeGpuMaterials;
					generationInputs.sceneTextureCacheCount = mSceneTextures.CacheCount();
					generationInputs.selectedTlasInstanceCount = selectedTlasInstanceCount;
					generationInputs.selectedSceneInstanceCount = selectedSceneInstanceCount;
					generationInputs.selectedStaticSceneInstanceCount = selectedStaticSceneInstanceCount;
					generationInputs.selectedDynamicSceneInstanceCount = selectedDynamicSceneInstanceCount;
					generationInputs.selectedPersistentVoxelSceneInstanceCount = selectedPersistentVoxelSceneInstanceCount;
					const NRISceneFrameGenerationResult generationResult =
						BuildNRISceneFrameGenerationResult(generationInputs, mLastStateCommitDomainGenerations, mHasLastStateCommitDomainGenerations);
					WriteNRISceneFrameGenerationTraceStats(generationResult, mLastPerfShellTraceStats);
					mLastStateCommitDomainGenerations = generationResult.current;
					mHasLastStateCommitDomainGenerations = true;
				}
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
		else if (mGpuSceneHasDynamicOverlay || residentStaticWorldGeometryChanged)
		{
			if (!RestoreStaticTopLevelScene())
			{
				LogFallback("PT static scene restore failed after dynamic overlay or resident chunk rebuild.");
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
		else if (deferOverlayThisFrame)
		{
			Printf("NRI PT dynamic scene deferred: skipping non-map dynamic overlay on the same frame that rebuilt resident static map assets.\n");
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
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildCapturedMs);
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
		auto& sceneInstances = mSelectCapturedSceneInstanceScratch;
		sceneInstances.clear();
		if (buffersReady)
		{
			sceneInstances.push_back({ 0u, NRI_SCENE_DATA_SOURCE_DYNAMIC, 0u, UINT32_MAX });
			buffersReady = UpdateSceneDataSet(
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
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
			NRIAccelerationStructureResource& dynamicBottomLevelAS = GetCurrentDynamicBottomLevelAS();
			accelerationReady =
				BuildDynamicAccelerationStructure(capturedGeometry) &&
				dynamicBottomLevelAS.accelerationStructure != nullptr;
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
				instance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*dynamicBottomLevelAS.accelerationStructure);

				auto& instances = mSelectCapturedTopLevelInstanceScratch;
				instances.clear();
				instances.push_back(instance);
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
		sceneLightDynamicMaterials,
		appendPersistentVoxelSceneLights);

	bool refreshedSceneDataAfterLightRebuild = false;
	if (mGpuSceneHasDynamicOverlay &&
		activeMaterialBridge == &combinedMaterialBridge &&
		!overlayGeometry.primitives.empty())
	{
		refreshedCombinedGpuMaterials = combinedMaterialBridge.materials;
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

			combinedGpuMaterials.swap(refreshedCombinedGpuMaterials);
			dynamicGpuMaterials.assign(combinedGpuMaterials.begin() + staticMaterialCount, combinedGpuMaterials.end());
			if (!UploadSceneBuffers(overlayGeometry, dynamicGpuMaterials) ||
				!UpdateSceneDataSet(
					mStaticVertexBuffer,
					mStaticIndexBuffer,
					mStaticPrimitiveBuffer,
					mStaticMaterialBuffer,
					GetCurrentDynamicVertexBuffer(),
					GetCurrentDynamicIndexBuffer(),
					GetCurrentDynamicPrimitiveBuffer(),
					GetCurrentDynamicMaterialBuffer(),
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
			refreshedSceneDataAfterLightRebuild = true;
		}
	}

	if (mRuntimeLightSceneDataDirty && !refreshedSceneDataAfterLightRebuild)
	{
		if (mGpuSceneHasDynamicOverlay)
		{
			if (!UpdateSceneDataSet(
				mStaticVertexBuffer,
				mStaticIndexBuffer,
				mStaticPrimitiveBuffer,
				mStaticMaterialBuffer,
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				mBoundSceneInstances,
				(uint32_t)mStaticMapScene.geometry.primitives.size(),
				(uint32_t)overlayGeometry.primitives.size(),
				(uint32_t)mStaticMapScene.gpuMaterials.size(),
				(uint32_t)dynamicGpuMaterials.size(),
				"runtime_overlay_light_refresh"))
			{
				LogFallback("PT runtime overlay light refresh failed after scene-light rebuild.");
				if (preserveHistory)
				{
					restoreHistory();
				}
				return false;
			}
		}
		else if (!sceneLightUsesStaticMapScene)
		{
			if (!UpdateSceneDataSet(
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				mBoundSceneInstances,
				0u,
				(uint32_t)capturedGeometry.primitives.size(),
				0u,
				(uint32_t)capturedGpuMaterials.size(),
				"captured_scene_light_refresh"))
			{
				LogFallback("PT captured scene light refresh failed after scene-light rebuild.");
				if (preserveHistory)
				{
					restoreHistory();
				}
				return false;
			}
		}
	}

	if (sceneLightUsesStaticMapScene && !mGpuSceneHasDynamicOverlay)
	{
		const bool needsResidentStaticLightRefresh =
			mRuntimeLightSceneDataDirty ||
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
	NRISceneSurfaceProbeFrameInputs surfaceProbeFrameInputs = {};
	surfaceProbeFrameInputs.usesStaticMapScene = mUsedStaticMapSceneLastFrame;
	surfaceProbeFrameInputs.activeStaticProbePrimitiveCount = activeStaticProbePrimitiveCount;
	surfaceProbeFrameInputs.runtimeSpaceLinkGeometry = &runtimeSpaceLinkGeometry;
	surfaceProbeFrameInputs.runtimeMutationGeometry = &runtimeMutationGeometry;
	surfaceProbeFrameInputs.overlayGeometry = &overlayGeometry;
	surfaceProbeFrameInputs.activeDynamicGeometry = activeDynamicGeometry;
	mSurfaceProbeFrame = BuildNRISceneSurfaceProbeFrameState(surfaceProbeFrameInputs);

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
		mLastPerfShellTraceStats.sceneInstanceStaticCount = 0;
		mLastPerfShellTraceStats.sceneInstanceDynamicCount = 0;
		mLastPerfShellTraceStats.sceneInstancePersistentVoxelCount = 0;
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneInstanceStatsMs);
			for (const SceneInstanceData& instance : mBoundSceneInstances)
			{
				if (instance.dataSource == NRI_SCENE_DATA_SOURCE_STATIC)
				{
					mLastPerfShellTraceStats.sceneInstanceStaticCount++;
				}
				else if (instance.dataSource == NRI_SCENE_DATA_SOURCE_DYNAMIC)
				{
					mLastPerfShellTraceStats.sceneInstanceDynamicCount++;
				}
				else if (instance.dataSource == NRI_SCENE_DATA_SOURCE_PERSISTENT_VOXEL)
				{
					mLastPerfShellTraceStats.sceneInstancePersistentVoxelCount++;
				}
			}
		}
		NRIPersistentVoxelStatusSnapshot persistentVoxelStatus = {};
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.persistentVoxelResourceStatsMs);
			mPersistentVoxels.FillResourceStatusSnapshot(persistentVoxelStatus);
		}
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.persistentVoxelBatchStatsMs);
			mPersistentVoxels.FillBatchStatusSnapshot(persistentVoxelStatus);
		}
		mLastPerfShellTraceStats.persistentVoxelMeshVariantResourceCount = persistentVoxelStatus.meshVariantResourceCount;
		mLastPerfShellTraceStats.persistentVoxelMaterialVariantResourceCount = persistentVoxelStatus.materialVariantResourceCount;
		mLastPerfShellTraceStats.persistentVoxelBatchActorCount = persistentVoxelStatus.batchActorCount;
		mLastPerfShellTraceStats.persistentVoxelInstanceRecordCount = persistentVoxelStatus.instanceRecordCount;
		mLastPerfShellTraceStats.persistentVoxelAdmissionQueueCount = persistentVoxelStatus.admissionQueueCount;
		mLastPerfShellTraceStats.persistentVoxelPendingInstanceCount = persistentVoxelStatus.pendingInstanceCount;
		mLastPerfShellTraceStats.persistentVoxelResidentResourceBytes = persistentVoxelStatus.residentResourceBytes;
		mLastPerfShellTraceStats.persistentVoxelZeroRefResourceBytes = persistentVoxelStatus.zeroRefResourceBytes;
		mLastPerfShellTraceStats.persistentVoxelZeroRefMeshResourceCount = persistentVoxelStatus.zeroRefMeshResourceCount;
		mLastPerfShellTraceStats.persistentVoxelZeroRefMaterialResourceCount = persistentVoxelStatus.zeroRefMaterialResourceCount;
		mLastPerfShellTraceStats.persistentVoxelInstanceActiveCount = persistentVoxelStatus.activeInstanceCount;
		mLastPerfShellTraceStats.persistentVoxelInstancePrimitiveCount = persistentVoxelStatus.instancePrimitiveCount;
		mLastPerfShellTraceStats.persistentVoxelInstanceMaterialCount = persistentVoxelStatus.instanceMaterialCount;
		mLastPerfShellTraceStats.persistentVoxelInstanceMinPrimitiveCount = persistentVoxelStatus.instanceMinPrimitiveCount;
		mLastPerfShellTraceStats.persistentVoxelInstanceMaxPrimitiveCount = persistentVoxelStatus.instanceMaxPrimitiveCount;
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

	if (success)
	{
		EmitSelfTestSummary(traceFrameIndex, drawmode, portal);
	}

	if (ShouldEmitRendererTemporalTraceLogs())
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
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=wait reason=frame-target-not-ready framebuffer=%u command_buffer=%u active_target=%u output=%ux%u target=%ux%u\n",
				mFrameBuffer != nullptr ? 1u : 0u,
				mFrameBuffer != nullptr && mFrameBuffer->mCommandBuffer != nullptr ? 1u : 0u,
				mFrameBuffer != nullptr && mFrameBuffer->mActiveTarget != nullptr ? 1u : 0u,
				outputWidth,
				outputHeight,
				targetWidth,
				targetHeight);
		}
		return false;
	}

	if (!RefreshPathTracingAvailability() || !mPathTracingSupported)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=pt-unsupported output=%ux%u target=%ux%u\n",
				outputWidth,
				outputHeight,
				targetWidth,
				targetHeight);
		}
		return true;
	}

	const auto preloadStart = std::chrono::steady_clock::now();
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading gate: event=renderer-preload result=begin output=%ux%u target=%ux%u map_valid=%u static_valid=%u static_resident=%u\n",
			outputWidth,
			outputHeight,
			targetWidth,
			targetHeight,
			mMapWorld.valid ? 1u : 0u,
			mStaticMapScene.valid ? 1u : 0u,
			mStaticMapScene.valid && mStaticMapScene.texturesResident && mStaticMapScene.buffersResident && mStaticMapScene.accelerationResident ? 1u : 0u);
	}

	ResetPerfTraceStats();
	{
		ScopedPtPerfTimer initPerfTimer(mLastPerfShellTraceStats.initResourcesMs);
		if (!Initialize() || !EnsureFrameResources(outputWidth, outputHeight, targetWidth, targetHeight))
		{
			LogFallback("PT preload frame resources or pipelines failed to initialize.");
			if ((int)nri_ptloadingtrace >= 1)
			{
				Printf("NRI PT loading gate: event=renderer-preload result=ready reason=init-failed ms=%.3f\n",
					DurationMs(preloadStart, std::chrono::steady_clock::now()));
			}
			return true;
		}
	}

	ResetSceneBufferFrameStats();
	ResetRendererSkyPerfTraceStats();
	nri_scene::ResetAverageTextureColorCache();
	nri_scene::ResetSkyPerfStats();

	RefreshMapWorld();
	if (!mMapWorld.valid)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=map-invalid ms=%.3f\n",
				DurationMs(preloadStart, std::chrono::steady_clock::now()));
		}
		return true;
	}

	if (!PreloadStaticMapResources())
	{
		LogFallback("PT preload resident static scene build failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=static-map-failed ms=%.3f\n",
				DurationMs(preloadStart, std::chrono::steady_clock::now()));
		}
		return true;
	}

	if (!ApplyStartupMapWorldCorrectionIfNeeded("renderer-preload"))
	{
		LogFallback("PT preload startup map-world correction failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=startup-correction-failed ms=%.3f\n",
				DurationMs(preloadStart, std::chrono::steady_clock::now()));
		}
		return true;
	}
	if (!mStaticMapScene.valid ||
		!mStaticMapScene.texturesResident ||
		!mStaticMapScene.buffersResident ||
		!mStaticMapScene.accelerationResident ||
		mStaticMapScene.buildSerial != mMapWorld.buildSerial)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=continue reason=startup-correction-rebuild static_valid=%u textures=%u buffers=%u acceleration=%u scene_build_serial=%llu map_build_serial=%llu ms=%.3f\n",
				mStaticMapScene.valid ? 1u : 0u,
				mStaticMapScene.texturesResident ? 1u : 0u,
				mStaticMapScene.buffersResident ? 1u : 0u,
				mStaticMapScene.accelerationResident ? 1u : 0u,
				(unsigned long long)mStaticMapScene.buildSerial,
				(unsigned long long)mMapWorld.buildSerial,
				DurationMs(preloadStart, std::chrono::steady_clock::now()));
		}
		if (!PreloadStaticMapResources())
		{
			LogFallback("PT preload corrected resident static scene build failed.");
			if ((int)nri_ptloadingtrace >= 1)
			{
				Printf("NRI PT loading gate: event=renderer-preload result=ready reason=startup-correction-static-map-failed ms=%.3f\n",
					DurationMs(preloadStart, std::chrono::steady_clock::now()));
			}
			return true;
		}
	}

	RefreshSceneLightSystem(true, nullptr, nullptr, nullptr, nullptr, false);
	bool staticLightRefreshReady = true;
	if (!mGpuSceneHasDynamicOverlay)
	{
		const bool needsResidentStaticLightRefresh =
			!mSceneLights.GetAnalyticLights().activeLights.empty() ||
			mBoundRuntimeLightCount != 0 ||
			mSceneLights.GetSectorLighting().activeSectorCount > 0 ||
			mBoundSectorLightActiveCount != 0;
		if (needsResidentStaticLightRefresh && !RefreshResidentStaticSceneDataSet())
		{
			staticLightRefreshReady = false;
			LogFallback("PT preload static scene light refresh failed.");
			if ((int)nri_ptloadingtrace >= 1)
			{
				Printf("NRI PT loading gate: event=renderer-preload result=continue reason=static-light-refresh-failed analytic=%u runtime_bound=%u sector_active=%u sector_bound=%u ms=%.3f\n",
					mSceneLights.GetAnalyticLights().activeLights.empty() ? 0u : 1u,
					mBoundRuntimeLightCount,
					mSceneLights.GetSectorLighting().activeSectorCount,
					mBoundSectorLightActiveCount,
					DurationMs(preloadStart, std::chrono::steady_clock::now()));
			}
		}
	}

	if (!PreloadPersistentVoxelResources())
	{
		if (mPersistentVoxels.HasPreloadPending())
		{
			if ((int)nri_ptloadingtrace >= 1)
			{
				uint32_t requiredPending = 0;
				uint32_t requiredReady = 0;
				uint32_t optionalPending = 0;
				uint32_t failed = 0;
				mPersistentVoxels.CountAdmissionWork(requiredPending, requiredReady, optionalPending, failed);
				Printf("NRI PT loading gate: event=renderer-preload result=wait reason=persistent-voxel-pending required_pending=%u required_ready=%u optional_pending=%u failed=%u ms=%.3f\n",
					requiredPending,
					requiredReady,
					optionalPending,
					failed,
					DurationMs(preloadStart, std::chrono::steady_clock::now()));
			}
			return false;
		}
		LogFallback("PT preload persistent voxel resource admission failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=persistent-voxel-failed ms=%.3f\n",
				DurationMs(preloadStart, std::chrono::steady_clock::now()));
		}
		return true;
	}
	if (!PreloadMaterialResources())
	{
		LogFallback("PT preload material warmup failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=material-failed ms=%.3f\n",
				DurationMs(preloadStart, std::chrono::steady_clock::now()));
		}
		return true;
	}

	EmissiveSamplingBuildContext emissiveSamplingContext = {};
	emissiveSamplingContext.staticGeometry = &mStaticMapScene.geometry;
	if (!UpdateEmissiveSamplingBuffers(emissiveSamplingContext))
	{
		LogFallback("PT preload emissive primitive update failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=emissive-sampling-failed ms=%.3f\n",
				DurationMs(preloadStart, std::chrono::steady_clock::now()));
		}
		return true;
	}
	if (!BuildEmissiveTopLevelAccelerationStructure())
	{
		LogFallback("PT preload emissive TLAS update failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=emissive-tlas-failed ms=%.3f\n",
				DurationMs(preloadStart, std::chrono::steady_clock::now()));
		}
		return true;
	}

	PrepareSceneTextureInputsForCompute();
	Printf("NRI PT preload ready: level=%s build_serial=%llu chunks=%u tris=%u materials=%u\n",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)mMapWorld.buildSerial,
		(uint32_t)mStaticMapScene.chunks.size(),
		(uint32_t)mStaticMapScene.geometry.primitives.size(),
		(uint32_t)mStaticMapScene.gpuMaterials.size());
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading gate: event=renderer-preload result=ready reason=complete static_light_refresh=%u ms=%.3f\n",
			staticLightRefreshReady ? 1u : 0u,
			DurationMs(preloadStart, std::chrono::steady_clock::now()));
	}
	return true;
}

void NRIRenderer::ResetHistory()
{
	RequestHistoryReset("history-reset", true, true);
}

void NRIRenderer::RequestAutoExposureReset(const char* reason)
{
	const char* safeReason = reason != nullptr && *reason != '\0' ? reason : "unspecified";
	mExposure.RequestReset(safeReason, (uint64_t)mFrameIndex);
	if (nri_ptautoexposurestats)
	{
		const NRIAutoExposureStatus& status = mExposure.GetStatus();
		Printf("NRI PT auto exposure reset: reason=%s frame=%u serial=%llu\n",
			safeReason,
			mFrameIndex,
			(unsigned long long)status.resetSerial);
	}
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
	RequestAutoExposureReset(mLastHistoryResetReason.c_str());
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
	if (ShouldEmitRendererTemporalTraceLogs())
	{
		Printf("NRI PT light change: reason=%s frame=%u reset=no\n",
			(reason != nullptr && *reason != '\0') ? reason : "unspecified",
			mFrameIndex);
	}
}

void NRIRenderer::InvalidateRuntimeLightSceneData()
{
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
	mRuntimeLightSceneDataDirty = true;
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
	InvalidateRuntimeLightSceneData();
	NoteLightHistoryChange("runtime-light-change");
	return true;
}

bool NRIRenderer::UpdateRuntimePointLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius)
{
	if (!mSceneLights.UpdateManualAnalyticLight(id, position, color, intensity, radius))
	{
		return false;
	}

	InvalidateRuntimeLightSceneData();
	NoteLightHistoryChange("runtime-light-change");
	return true;
}

bool NRIRenderer::RemoveRuntimePointLight(uint32_t id)
{
	if (!mSceneLights.RemoveManualAnalyticLight(id))
	{
		return false;
	}

	InvalidateRuntimeLightSceneData();
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
	InvalidateRuntimeLightSceneData();
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
		Printf("NRI PT analytic light %u: id=%u topology=0x%016llx prev_match=%s added=%s rebound=%s prop_changed=%s shadow=%s source=%s%s rule=%u actor=%d tile=%u render_pos=(%.3f, %.3f, %.3f) color=(%.3f, %.3f, %.3f) intensity=%.3f radius=%.3f\n",
			light.id,
			light.id,
			(unsigned long long)light.stableKey,
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_PreviousMatch) != 0),
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_Added) != 0),
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_Rebound) != 0),
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_PropertyChanged) != 0),
			YesNo((light.flags & SceneAnalyticLightFlag_CastsShadow) != 0),
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
	Printf("NRI PT emissive heuristics: rules=%u auto_tagged=%u explicit_matches=%u overrides=%u override_matches=%u material_response_rules=%u material_response_matches=%u active=%u total_power=%.3f glow_scale=%.3f glow_reach=%.3f glow_falloff=%.3f glow_blend=%.3f truncated=%u\n",
		(uint32_t)emissive.textureRules.size(),
		emissive.autoTaggedCount,
		emissive.explicitRuleMatchCount,
		emissive.overrideRuleCount,
		emissive.overrideMatchedSurfaceCount,
		emissive.materialResponseRuleCount,
		emissive.materialResponseMatchedSurfaceCount,
		(uint32_t)emissive.activeSurfaces.size(),
		emissive.totalPowerEstimate,
		(float)nri_ptglowscale,
		(float)nri_ptglowreach,
		(float)nri_ptglowfalloff,
		(float)nri_ptglowblend,
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

	Printf("NRI PT emissive primitives: active=%u source_surfaces=%u auto=%u explicit=%u overrides=%u override_matches=%u material_response_rules=%u material_response_matches=%u total_power=%.3f topo_changed=%s prop_changed=%s added=%u removed=%u rebound=%u min_surface=%.3f min_power=%.3f\n",
		(uint32_t)mBoundEmissivePrimitiveRecords.size(),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().activeSurfaces.size(),
		mSceneLights.GetEmissiveSurfaces().autoTaggedCount,
		mSceneLights.GetEmissiveSurfaces().explicitRuleMatchCount,
		mSceneLights.GetEmissiveSurfaces().overrideRuleCount,
		mSceneLights.GetEmissiveSurfaces().overrideMatchedSurfaceCount,
		mSceneLights.GetEmissiveSurfaces().materialResponseRuleCount,
		mSceneLights.GetEmissiveSurfaces().materialResponseMatchedSurfaceCount,
		mBoundEmissiveTotalPower,
		YesNo(mSceneLights.GetEmissiveSurfaces().lastBuildTopologyChanged),
		YesNo(mSceneLights.GetEmissiveSurfaces().lastBuildPropertiesChanged),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().addedTopologyKeys.size(),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().removedTopologyKeys.size(),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().reboundTopologyKeys.size(),
		(float)nri_ptemissiveminsurface,
		(float)nri_ptemissiveminpower);

	const auto& emissiveSurfaces = mSceneLights.GetEmissiveSurfaces();
	const uint32_t printCount = std::min<uint32_t>((uint32_t)candidates.size(), limit);
	for (uint32_t i = 0; i < printCount; ++i)
	{
		const auto& record = *candidates[i].record;
		const auto diagnosticIt = emissiveSurfaces.activeDiagnosticFlags.find(record.surfaceStableKey);
		const uint32_t diagnosticFlags = diagnosticIt != emissiveSurfaces.activeDiagnosticFlags.end() ? diagnosticIt->second : SceneLightDiagnosticFlag_None;
		Printf("NRI PT emissive %u: primitive_key=0x%016llx surface_key=0x%016llx prev_match=%s added=%s rebound=%s prop_changed=%s source=%s primitive=%u material=%u flags=0x%x rule=%u override_rule=%u actor=%d sector=%d sector_scale=%.3f reach_scale=%.3f sector_applied=%s material_response=%s material_scale=%.3f tile=%u mode=%s emissive_tex=%u area=%.2f power=%.3f sample_weight=%.3f pdf=%.6f center=(%.2f, %.2f, %.2f) color=(%.3f, %.3f, %.3f) intensity=%.3f\n",
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
			record.overrideRuleId,
			record.actorIndex,
			record.sectorIndex,
			record.sectorResponseScale,
			record.sectorReachScale,
			YesNo(record.sectorResponseApplied),
			YesNo(record.materialResponseEnabled),
			record.materialResponseScale,
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
	if (registry.activeSectorIndices.empty() && registry.rawActiveSectorIndices.empty())
	{
		Printf("NRI PT sector lights: no active sector-light records are available. raw_active=%u raw_nonneutral=%u eligible=%u\n",
			registry.rawActiveSectorCount,
			registry.rawNonNeutralSectorCount,
			registry.eligibleSectorCount);
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
	candidates.reserve(std::max(registry.activeSectorIndices.size(), registry.rawActiveSectorIndices.size()));
	std::vector<uint8_t> candidateSectors(registry.sectorCount, 0u);
	for (uint32_t sectorIndex : registry.activeSectorIndices)
	{
		if (sectorIndex < candidateSectors.size())
		{
			candidateSectors[sectorIndex] = 1u;
		}
	}
	for (uint32_t sectorIndex : registry.rawActiveSectorIndices)
	{
		if (sectorIndex < candidateSectors.size())
		{
			candidateSectors[sectorIndex] = 1u;
		}
	}
	const float radiusSq = radius > 0.0f ? radius * radius : std::numeric_limits<float>::max();
	for (uint32_t sectorIndex = 0; sectorIndex < (uint32_t)candidateSectors.size(); ++sectorIndex)
	{
		if (candidateSectors[sectorIndex] == 0u)
		{
			continue;
		}
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

	Printf("NRI PT sector lights: active=%u raw_active=%u raw_nonneutral=%u response=boost:%u dim:%u neutral:%u eligible=%u fog=%u pulsing=%u radius=%.1f limit=%u multiplier=%.3f scales=(%.3f, %.3f, %.3f) clamp=%.3f sector_response=%.3f/[%.3f,%.3f] intensity=[%.3f,%.3f] reach=[%.3f,%.3f] filter=pal=%d shade=[%d,%d] lotag=%d pulse=%d/%.3f\n",
		registry.activeSectorCount,
		registry.rawActiveSectorCount,
		registry.rawNonNeutralSectorCount,
		registry.responseBoostSectorCount,
		registry.responseDimSectorCount,
		registry.responseNeutralSectorCount,
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
		(float)nri_ptsectoremissionsignalstrength,
		(float)nri_ptsectoremissionresponsemin,
		(float)nri_ptsectoremissionresponsemax,
		(float)nri_ptsectoremissionlightmin,
		(float)nri_ptsectoremissionlightmax,
		(float)nri_ptsectoremissionreachmin,
		(float)nri_ptsectoremissionreachmax,
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
		Printf("NRI PT sector light %u: sector=%u dist=%.2f center=(%.2f, %.2f, %.2f) applied=(%.3f, %.3f, %.3f)*%.3f hemi=%.3f fog=%.3f raw_light=%.3f raw_floor=%.3f raw_ceil=%.3f raw_ambient=%.3f raw_hemi=%.3f raw_brightness=%.3f response=%.3f raw_fog=%.3f pulse=%.3f palette=%d shade=%d raw_shade=%d lotag=%d hitag=%d flags=0x%x\n",
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
			entry.rawLightLevel,
			entry.rawFloorLight,
			entry.rawCeilingLight,
			entry.rawAmbientIntensity,
			entry.rawHemisphereAmount,
			entry.rawResponseBrightness,
			entry.emitterResponseScale,
			entry.rawFogAmount,
			entry.pulseScale,
			entry.paletteIndex,
			entry.averageShade,
			entry.rawAverageShade,
			entry.lotag,
			entry.hitag,
			entry.sourceFlags);
	}

	if (printCount == 0)
	{
		Printf("NRI PT sector lights: no active or raw sector lights matched the requested radius.\n");
	}
}

void NRIRenderer::PrintStatus()
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
	const NRITraceSettings traceSettings = BuildNRITraceSettingsFromCVars();
	const NRIDenoiserSettings denoiserSettings = BuildNRIDenoiserSettingsFromCVars();
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
	const NRIAutoExposureSettings autoExposureSettings = GetNRIAutoExposureSettings(
		outputPolicy.exposure,
		IsNRIPTHdrOutputActive(outputPolicy));
	mExposure.SetSettings(autoExposureSettings);
	ReadbackAutoExposureStats();
	const NRIAutoExposureStatus& autoExposureStatus = mExposure.GetStatus();
	const NRIMainUpscalerKind autoExposureResolvedMain = GetResolvedMainUpscalerKindForStatus();
	const NRIPostSharpenKind autoExposureResolvedPost = GetResolvedPostSharpenKindForStatus();
	const FrameTextureSlot autoExposurePresentSlot = mUseUpscaledInFinal ? mUpscaledInputSlot : mHistoryOutputSlot;
	const ExposureRoute autoExposurePresentRoute = ResolveExposureRoute(
		autoExposurePresentSlot,
		outputPolicy,
		autoExposureResolvedMain,
		autoExposureResolvedPost);
	const NRITextureResource* autoExposureStateTexture = mExposure.GetExposureStateTexture(mFrameIndex & 1u);
	const bool autoExposureSceneHdrInput = autoExposurePresentRoute.inputDomain == ExposureDomain::SceneHDR;
	const bool autoExposureTextureValid =
		autoExposureStateTexture != nullptr &&
		autoExposureStateTexture->shaderView != nullptr;
	const bool autoExposurePresentEligible =
		autoExposureSettings.enabled &&
		autoExposureSceneHdrInput &&
		autoExposureTextureValid;
	const bool vendorExposurePath =
		autoExposureResolvedMain == NRIMainUpscalerKind::DLSR ||
		autoExposureResolvedMain == NRIMainUpscalerKind::DLRR;
	const bool vendorExposureEngine =
		vendorExposurePath &&
		autoExposureSettings.enabled &&
		autoExposureTextureValid;
	const char* vendorExposureMode =
		!vendorExposurePath ? "none" :
		vendorExposureEngine ? "engine" :
		"vendor-auto";

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
	Printf("NRI PT auto exposure: enabled=%s control_block=%s freeze=%s stats=%s resources=%s state_textures=%s meter_source=%s meter_mode=%s histogram_bins=%u sample_step=%u target=%.3f range=%.3f..%.3f bias=%.3f percentiles=%.2f..%.2f hist_log_range=%.1f..%.1f adapt=%.3f/%.3f fallback_manual=%.3f resource_render=%ux%u memory=%llu alloc_serial=%u reset_pending=%s reset_serial=%llu reset_request_frame=%llu reset_consumed_frame=%llu reset_reason=%s\n",
		YesNo(autoExposureSettings.enabled),
		autoExposureSettings.hdrControlsActive ? "hdr" : "sdr",
		YesNo(autoExposureSettings.freeze),
		YesNo(autoExposureSettings.stats),
		YesNo(autoExposureStatus.resourcesAllocated),
		autoExposureStatus.resourcesAllocated ? "allocated" : "not_allocated",
		GetFrameTextureSlotName(mAutoExposureInputSourceSlot),
		GetNRIAutoExposureMeteringModeName(autoExposureSettings.meteringMode),
		autoExposureSettings.histogramBinCount,
		autoExposureSettings.sampleStep,
		autoExposureSettings.targetLuminance,
		autoExposureSettings.minExposure,
		autoExposureSettings.maxExposure,
		autoExposureSettings.exposureBias,
		autoExposureSettings.lowPercentile,
		autoExposureSettings.highPercentile,
		NRI_EXPOSURE_LOG_LUMINANCE_MIN,
		NRI_EXPOSURE_LOG_LUMINANCE_MAX,
		autoExposureSettings.adaptUpSpeed,
		autoExposureSettings.adaptDownSpeed,
		autoExposureSettings.fallbackManualExposure,
		autoExposureStatus.renderWidth,
		autoExposureStatus.renderHeight,
		(unsigned long long)autoExposureStatus.memoryBytes,
		autoExposureStatus.allocationSerial,
		YesNo(autoExposureStatus.resetPending),
		(unsigned long long)autoExposureStatus.resetSerial,
		(unsigned long long)autoExposureStatus.resetRequestFrame,
		(unsigned long long)autoExposureStatus.resetConsumedFrame,
		autoExposureStatus.resetReason[0] != '\0' ? autoExposureStatus.resetReason : "none");
	Printf("NRI PT auto exposure stats: valid=%s readback=%s frame=%llu samples=%u bins=%u..%u log_lum=%.3f..%.3f metered_log_lum=%.3f target_exposure=%.3f adapted_exposure=%.3f target_ev=%.3f adapted_ev=%.3f\n",
		YesNo(autoExposureStatus.debugValid),
		YesNo(autoExposureStatus.debugReadbackAllocated),
		(unsigned long long)autoExposureStatus.debugFrameIndex,
		autoExposureStatus.sampleCount,
		autoExposureStatus.lowBin,
		autoExposureStatus.highBin,
		autoExposureStatus.lowLogLuminance,
		autoExposureStatus.highLogLuminance,
		autoExposureStatus.meteredLogLuminance,
		autoExposureStatus.targetExposure,
		autoExposureStatus.adaptedExposure,
		std::log2(std::max(autoExposureStatus.targetExposure, 1.0e-6f)),
		std::log2(std::max(autoExposureStatus.adaptedExposure, 1.0e-6f)));
	Printf("NRI PT auto exposure present: slot=%s domain=%s enabled=%s scene_hdr=%s texture_valid=%s apply=%s manual_fallback=%.3f\n",
		GetFrameTextureSlotName(autoExposurePresentSlot),
		GetExposureDomainName(autoExposurePresentRoute.inputDomain),
		YesNo(autoExposureSettings.enabled),
		YesNo(autoExposureSceneHdrInput),
		YesNo(autoExposureTextureValid),
		YesNo(autoExposurePresentEligible),
		autoExposurePresentRoute.presentExposure);
	Printf("NRI PT auto exposure vendor: main=%s mode=%s texture_valid=%s engine_enabled=%s recreate_on_policy_change=yes\n",
		GetMainUpscalerName(autoExposureResolvedMain),
		vendorExposureMode,
		YesNo(autoExposureTextureValid),
		YesNo(autoExposureSettings.enabled));
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
	Printf("NRI PT material calibration: fullbright_boost=%.3f voxel_emission_boost=%.3f\n",
		(float)nri_ptfullbrightboost,
		(float)nri_voxelemissionboost);
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
	Printf("NRI PT tracing: direct_scene_fallback=%s light_bounces=%u mirror_bounces=%u portal_depth=%u surface_probe=%d ceiling_nudge=%s ceiling_nudge_distance=%.4f\n",
		nri_ptdirectscene ? "on" : "off",
		traceSettings.lightBounceCount,
		traceSettings.mirrorBounceCount,
		traceSettings.portalDepth,
		(int)nri_ptsurfaceprobe,
		nri_ptceilingnudge ? "on" : "off",
		(float)nri_ptceilingnudgedistance);
	Printf("NRI PT lighting shell: directional=%s sector=%s\n",
		mDirectionalLightState.enabled ? "on" : "off",
		nri_ptsectorlighting ? "on" : "off");
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
		GetNrdDenoiserModeName(denoiserSettings.denoiserMode),
		"2.5D",
		"interpolated",
		"16=denoised_diff 17=denoised_spec 18=metalness 19=roughness 20=motion_z 21=live_raw_penumbra 22=live_raw_shadow 23=temporal_sigma_shadow 24=direct_lighting 25=direct_emission 26=analytic_direct 27=emissive_tags 28=emissive_direct 29=sector_ambient 30=emissive_uv 31=emissive_radiance 32=emissive_primitive 33=emissive_visibility 34=trace_transparent 35=sr_input 36=sr_depth 37=vendor_output 38=vendor_output_final 39=rr_input 40=rr_diffuse_albedo 41=rr_specular_albedo 42=rr_normal_roughness 43=rr_specular_hit_distance 44=post_sharpen_output 45=taa_pre_exposed_input");
	const char* shadowSplitMode =
		!mUseSplitShadowDenoiser ? "off" :
		(GetEffectivePtDebugMode() >= 21 && GetEffectivePtDebugMode() <= 23) ? "sigma-debug" :
		"sigma-beauty";
	Printf("NRI PT NRD settings: max_frames=%u fast_frames=%u stabilization_frames=%u anti_firefly=%s hit_recon=%s input_split=%s shadow_split=%s\n",
		denoiserSettings.maxAccumulatedFrameNum,
		denoiserSettings.maxFastAccumulatedFrameNum,
		denoiserSettings.maxStabilizedFrameNum,
		denoiserSettings.enableAntiFirefly ? "on" : "off",
		GetNrdHitDistanceReconstructionModeName(denoiserSettings.hitDistanceReconstructionMode),
		GetNrdInputSplitModeName(denoiserSettings.inputSplitMode),
		shadowSplitMode);
	Printf("NRI PT SIGMA tuning: stabilization_frames=%u plane_distance_sensitivity=%.3f\n",
		denoiserSettings.sigmaMaxStabilizedFrameNum,
		denoiserSettings.sigmaPlaneDistanceSensitivity);
	if (denoiserSettings.denoiserMode == NRINrdDenoiserMode::Relax)
	{
		Printf("NRI PT NRD tuning: fast_history_sigma=%.2f prepass=%.2f/%.2f material_floor=1/2 blur_radius=n/a_relax\n",
			denoiserSettings.fastHistoryClampingSigmaScale,
			denoiserSettings.diffusePrepassBlurRadius,
			denoiserSettings.specularPrepassBlurRadius);
	}
	else
	{
		Printf("NRI PT NRD tuning: fast_history_sigma=%.2f blur_radius=%.2f..%.2f prepass=%.2f/%.2f material_floor=1/2\n",
			denoiserSettings.fastHistoryClampingSigmaScale,
			denoiserSettings.minBlurRadius,
			denoiserSettings.maxBlurRadius,
			denoiserSettings.diffusePrepassBlurRadius,
			denoiserSettings.specularPrepassBlurRadius);
	}
	Printf("NRI PT NRD guides: diffuse_signal=primary_demodulated_radiance specular_signal=primary_demodulated_radiance hit_distance=%s roughness=material_hint metalness=material_hint material_id=semantic_class\n",
		denoiserSettings.denoiserMode == NRINrdDenoiserMode::Relax ? "secondary_transport_linear_hitdist" : "secondary_transport_reblur_norm");
	Printf("NRI PT scene stats: %s\n", nri_ptscenestats ? "on" : "off");
	Printf("NRI PT mutation trace: chunk=%d sector=%d\n",
		(int)nri_ptmutationtracechunk,
		(int)nri_ptmutationtracesector);
	Printf("NRI PT runtime link trace: %s\n", nri_ptruntimelinktrace ? "on" : "off");
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
	Printf("NRI PT emissive surfaces: active=%u rules=%u auto=%u explicit=%u overrides=%u override_matches=%u material_response_rules=%u material_response_matches=%u total_power=%.3f topo_changed=%s prop_changed=%s added=%u removed=%u rebound=%u debug_mode=%u/%u thresholds=area>=%.3f power>=%.3f light=[%.3f,%.3f] reach=[%.3f,%.3f] glow_scale=%.3f glow_reach=%.3f glow_falloff=%.3f glow_blend=%.3f\n",
		(uint32_t)mSceneLights.GetEmissiveSurfaces().activeSurfaces.size(),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().textureRules.size(),
		mSceneLights.GetEmissiveSurfaces().autoTaggedCount,
		mSceneLights.GetEmissiveSurfaces().explicitRuleMatchCount,
		mSceneLights.GetEmissiveSurfaces().overrideRuleCount,
		mSceneLights.GetEmissiveSurfaces().overrideMatchedSurfaceCount,
		mSceneLights.GetEmissiveSurfaces().materialResponseRuleCount,
		mSceneLights.GetEmissiveSurfaces().materialResponseMatchedSurfaceCount,
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
		(float)nri_ptsectoremissionlightmin,
		(float)nri_ptsectoremissionlightmax,
		(float)nri_ptsectoremissionreachmin,
		(float)nri_ptsectoremissionreachmax,
		(float)nri_ptglowscale,
		(float)nri_ptglowreach,
		(float)nri_ptglowfalloff,
		(float)nri_ptglowblend);
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
		traceSettings.emissiveSampleCount,
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
	Printf("NRI PT sector lighting: enabled=%s active=%u raw_active=%u raw_nonneutral=%u response=boost:%u dim:%u neutral:%u eligible=%u fog=%u pulsing=%u debug_mode=%u multiplier=%.3f scales=ambient=%.3f hemi=%.3f fog=%.3f clamp=%.3f sector_response=%.3f/[%.3f,%.3f] intensity=[%.3f,%.3f] reach=[%.3f,%.3f] filter=pal=%d shade=[%d,%d] lotag=%d pulse=%d/%.3f\n",
		nri_ptsectorlighting ? "on" : "off",
		mSceneLights.GetSectorLighting().activeSectorCount,
		mSceneLights.GetSectorLighting().rawActiveSectorCount,
		mSceneLights.GetSectorLighting().rawNonNeutralSectorCount,
		mSceneLights.GetSectorLighting().responseBoostSectorCount,
		mSceneLights.GetSectorLighting().responseDimSectorCount,
		mSceneLights.GetSectorLighting().responseNeutralSectorCount,
		mSceneLights.GetSectorLighting().eligibleSectorCount,
		mSceneLights.GetSectorLighting().fogSectorCount,
		mSceneLights.GetSectorLighting().pulsingSectorCount,
		NRI_PTDEBUG_SECTOR_AMBIENT,
		GetSectorLightMultiplier(),
		(float)nri_ptsectorambientscale,
		(float)nri_ptsectorhemiscale,
		(float)nri_ptsectorfogscale,
		(float)nri_ptsectorclamp,
		(float)nri_ptsectoremissionsignalstrength,
		(float)nri_ptsectoremissionresponsemin,
		(float)nri_ptsectoremissionresponsemax,
		(float)nri_ptsectoremissionlightmin,
		(float)nri_ptsectoremissionlightmax,
		(float)nri_ptsectoremissionreachmin,
		(float)nri_ptsectoremissionreachmax,
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
		Printf("NRI PT last scene: walls=%u flats=%u sprites=%u translucent=%u models=%u voxel_proxies=%u unsupported_models=%u voxel_cache=candidates:%u uncacheable:%u hits:%u misses:%u changes:%u split_stable:%u split_live:%u entries:%u surface_hits:%u stores:%u rebuilds:%u transform_rebakes:%u removes:%u not_captured:%u cached_prims:%u mirrors=%u skies=%u portal_views=%u portal_skips=%u approx_tris=%u materials=%u\n",
			stats.wallDrawItems,
			stats.flatDrawItems,
			stats.spriteDrawItems,
			stats.translucentDrawItems,
			stats.modelDrawItems,
			stats.voxelProxyDrawItems,
			stats.unsupportedModelDrawItems,
			stats.voxelStableCandidates,
			stats.voxelStableUncacheable,
			stats.voxelStableSignatureHits,
			stats.voxelStableSignatureMisses,
			stats.voxelStableSignatureChanges,
			stats.voxelStableSplitStable,
			stats.voxelStableSplitLive,
			stats.voxelCacheEntries,
			stats.voxelCacheSurfaceHits,
			stats.voxelCacheSurfaceStores,
			stats.voxelCacheSurfaceRebuilds,
			stats.voxelCacheTransformRebakes,
			stats.voxelCacheSurfaceRemoves,
			stats.voxelCacheNotCaptured,
			stats.voxelCachePrimitives,
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
	mRuntimeMutation.PrintStatus();
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

	accumulateTexture(mSceneTextures.PaletteTexture(), telemetry.sceneTextureBytes);
	for (const NRISceneCachedTexture& texture : mSceneTextures.CachedTextures())
	{
		accumulateTexture(texture.resource, telemetry.sceneTextureBytes);
	}

	for (const NRICachedSkyTexture& texture : mSkyEnvironment.CachedTextures())
	{
		accumulateTexture(texture.resource, telemetry.skyTextureBytes);
	}

	accumulateBuffer(mVertexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mIndexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mPrimitiveBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mMaterialBuffer, telemetry.sceneBufferBytes);
	for (const SceneUploadBufferRingSlot& slot : mSceneUploadBufferRing)
	{
		accumulateBuffer(slot.vertexBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.indexBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.primitiveBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.materialBuffer, telemetry.sceneBufferBytes);
		accumulateAs(slot.dynamicBottomLevelAS, telemetry.accelerationStructureBytes);
	}
	accumulateBuffer(mStaticVertexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mStaticIndexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mStaticPrimitiveBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mStaticMaterialBuffer, telemetry.sceneBufferBytes);
	const NRIPersistentVoxelMemoryUsage persistentVoxelMemory = mPersistentVoxels.GetMemoryUsage();
	telemetry.sceneBufferBytes += persistentVoxelMemory.sceneBufferBytes;
	telemetry.accelerationStructureBytes += persistentVoxelMemory.accelerationStructureBytes;
	accumulateBuffer(mTlasInstanceBuffer, telemetry.sceneBufferBytes);
	for (const NRIBufferResource& tlasInstanceBuffer : mTlasInstanceBufferRing)
	{
		accumulateBuffer(tlasInstanceBuffer, telemetry.sceneBufferBytes);
	}
	accumulateBuffer(mSceneInstanceBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mPortalBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mRuntimeLightBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mRuntimeLightTileHeaderBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mRuntimeLightTileIndexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissivePrimitiveHeaderBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissivePrimitiveBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissivePrimitiveCdfBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissiveMaterialResponseBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissiveTlasInstanceBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mSectorLightHeaderBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mSectorLightBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mReprojectionBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mVisibleChunkBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mVisibleFlatPlaneBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mScratchBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mResidentStaticBlasScratchBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mTopLevelScratchBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissiveTopLevelScratchBuffer, telemetry.sceneBufferBytes);

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

void NRIRenderer::PrintSwapChainRenderConfig() const
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
	const bool beautyDenoiseActive = !!nri_denoise && resolvedMain != NRIMainUpscalerKind::DLRR;
	NRIPTOutputPolicy outputPolicy = {};
	if (mFrameBuffer != nullptr)
	{
		outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	}
	const bool nisSupported =
		mFrameBuffer != nullptr &&
		mFrameBuffer->mDevice != nullptr &&
		mFrameBuffer->mUpscaler.IsUpscalerSupported(*mFrameBuffer->mDevice, nri::UpscalerType::NIS);
	const bool dlsrSupported = IsMainUpscalerSupported(NRIMainUpscalerKind::DLSR);
	const bool dlrrSupported = IsMainUpscalerSupported(NRIMainUpscalerKind::DLRR);

	Printf("NRI swapchain render config: main_upscaler=%s->%s mode=%s->%s post_sharpen=%s->%s support=NIS:%s DLSS-SR:%s DLRR:%s app_taa=requested:%s active:%s denoise=requested:%s beauty_active:%s nrd=%s render_scale=%.3f->%.3f jitter=%s phases=%u output=%s->%s hdr_swapchain=%s display_hdr=%s tonemap=%s sharpness=%.3f\n",
		GetMainUpscalerName(requestedMain),
		GetMainUpscalerName(resolvedMain),
		GetUpscalerModeName(requestedUpscalerMode),
		GetUpscalerModeName(resolvedUpscalerMode),
		GetPostSharpenName(requestedPost),
		GetPostSharpenName(resolvedPost),
		nisSupported ? "yes" : "no",
		dlsrSupported ? "yes" : "no",
		dlrrSupported ? "yes" : "no",
		nri_pttaa ? "on" : "off",
		runAppTaa ? "on" : "off",
		nri_denoise ? "on" : "off",
		beautyDenoiseActive ? "on" : "off",
		GetNrdDenoiserModeName(GetSelectedNrdDenoiserMode()),
		requestedRenderScale,
		resolvedRenderScale,
		GetTemporalJitterModeName(resolvedMain, mGuiCaptureActive),
		GetTemporalJitterPhaseCount(resolvedMain, resolvedUpscalerMode, mGuiCaptureActive),
		GetNRIPTOutputModeName(outputPolicy.requestedMode),
		GetNRIPTOutputModeName(outputPolicy.resolvedMode),
		outputPolicy.hdrSwapChainActive ? "yes" : "no",
		!outputPolicy.displayInfoAvailable ? "unknown" : (outputPolicy.displayHdrSupported ? "yes" : "no"),
		GetNRIPTTonemapModeName(outputPolicy.tonemapMode),
		(float)nri_sharpness);
}

const char* NRIRenderer::GetExposureDomainName(ExposureDomain domain) const
{
	switch (domain)
	{
	case ExposureDomain::SceneHDR: return "scene_hdr";
	case ExposureDomain::PreExposedHDR: return "pre_exposed_hdr";
	case ExposureDomain::DisplayMappedOutput: return "display_mapped_output";
	default: return "unknown";
	}
}

NRIRenderer::ExposureDomain NRIRenderer::ResolveFrameTextureExposureDomain(FrameTextureSlot slot, NRIMainUpscalerKind mainKind, NRIPostSharpenKind postSharpenKind) const
{
	switch (slot)
	{
	case FrameTextureSlot::Composed:
	case FrameTextureSlot::TraceTransparentOutput:
	case FrameTextureSlot::SrInput:
	case FrameTextureSlot::RrInput:
		return ExposureDomain::SceneHDR;
	case FrameTextureSlot::TaaHistoryPing:
	case FrameTextureSlot::TaaHistoryPong:
		return ShouldRunAppTaa(mainKind) ? ExposureDomain::PreExposedHDR : ExposureDomain::SceneHDR;
	case FrameTextureSlot::VendorOutput:
		return ExposureDomain::SceneHDR;
	case FrameTextureSlot::PostSharpenOutput:
		if (postSharpenKind == NRIPostSharpenKind::Off)
		{
			return ExposureDomain::SceneHDR;
		}
		if (mainKind != NRIMainUpscalerKind::Off)
		{
			return ExposureDomain::SceneHDR;
		}
		return ShouldRunAppTaa(mainKind) ? ExposureDomain::PreExposedHDR : ExposureDomain::SceneHDR;
	case FrameTextureSlot::Final:
		return ExposureDomain::DisplayMappedOutput;
	default:
		return ExposureDomain::SceneHDR;
	}
}

NRIRenderer::ExposureRoute NRIRenderer::ResolveExposureRoute(FrameTextureSlot inputSlot, const NRIPTOutputPolicy& outputPolicy, NRIMainUpscalerKind mainKind, NRIPostSharpenKind postSharpenKind) const
{
	ExposureRoute route = {};
	route.inputDomain = ResolveFrameTextureExposureDomain(inputSlot, mainKind, postSharpenKind);
	route.temporalExposure = GetTemporalExposure(outputPolicy);
	route.presentExposure =
		route.inputDomain == ExposureDomain::PreExposedHDR ?
		1.0f :
		outputPolicy.exposure;
	return route;
}

void NRIRenderer::ResetSelfTestRouteSnapshot()
{
	mSelfTestRoute = {};
}

void NRIRenderer::SetSelfTestRouteSnapshot(const char* routeName, const char* presenterName, const char* ownerName, const char* passes, bool denoiserRun, bool upscalerRun, bool exposureRun)
{
	if (!nri_ptselftest)
	{
		return;
	}

	mSelfTestRoute.routeName = routeName != nullptr ? routeName : "unknown";
	mSelfTestRoute.presenterName = presenterName != nullptr ? presenterName : "unknown";
	mSelfTestRoute.ownerName = ownerName != nullptr ? ownerName : "unknown";
	mSelfTestRoute.passes = passes != nullptr ? passes : "unknown";
	mSelfTestRoute.denoiserRun = denoiserRun;
	mSelfTestRoute.upscalerRun = upscalerRun;
	mSelfTestRoute.exposureRun = exposureRun;
}

void NRIRenderer::EmitSelfTestSummary(uint32_t traceFrameIndex, int drawmode, bool portal) const
{
	if (!nri_ptselftest)
	{
		return;
	}

	const PerfShellTraceStats& shell = mLastPerfShellTraceStats;
	const NRIPTOutputPolicy outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	const NRIAutoExposureSettings& exposureSettings = mExposure.GetSettings();
	const NRIAutoExposureStatus& exposureStatus = mExposure.GetStatus();
	const NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	const bool finalTextureValid = final.texture != nullptr && final.shaderView != nullptr;
	const bool worldActive = gamestate == GS_LEVEL && currentLevel != nullptr;
	const bool gameplayFrame = worldActive && drawmode == DM_MAINVIEW && !portal;
	const uint64_t sceneSignature = HashCombine64(
		HashCombine64(
			HashCombine64(mVertexBuffer.payloadHash, mIndexBuffer.payloadHash),
			mPrimitiveBuffer.payloadHash),
		mSceneInstanceBuffer.payloadHash);
	const uint64_t materialSignature = mMaterialBuffer.payloadHash;
	const uint64_t instanceSignature = mSceneInstanceBuffer.payloadHash;
	const uint64_t skySignature = HashCombine64(mSkyEnvironment.ActiveKey(), (uint64_t)mSkyEnvironment.ActiveState().faceMask);
	const NRIBufferResource& vertexBuffer = mVertexBuffer;
	const NRIBufferResource& indexBuffer = mIndexBuffer;
	const NRIBufferResource& primitiveBuffer = mPrimitiveBuffer;
	const NRIBufferResource& materialBuffer = mMaterialBuffer;
	const uint64_t vertexBytes = vertexBuffer.payloadSize != 0 ? vertexBuffer.payloadSize : vertexBuffer.usedSize;
	const uint64_t indexBytes = indexBuffer.payloadSize != 0 ? indexBuffer.payloadSize : indexBuffer.usedSize;
	const uint64_t primitiveBytes = primitiveBuffer.payloadSize != 0 ? primitiveBuffer.payloadSize : primitiveBuffer.usedSize;
	const uint64_t materialBytes = materialBuffer.payloadSize != 0 ? materialBuffer.payloadSize : materialBuffer.usedSize;

	Printf("NRI PT selftest: frame=%u engine_frame=%u map=%s level=%s backend=nri api=%s world_active=%u menu_active=%s gameplay_frame=%u portal=%u drawmode=%d route=%s debug=%d passes=%s presenter=%s owner=%s denoiser_run=%u upscaler_run=%u exposure_run=%u present_kind=%s render_width=%u render_height=%u output_width=%u output_height=%u swapchain_format=%u hdr=%u prims=%u mats=%u scene_instances=%u static_instances=%u dynamic_instances=%u voxel_instances=%u emissive_instances=%u vertices=%u indices=%u vertex_bytes=%llu index_bytes=%llu primitive_bytes=%llu material_bytes=%llu instance_bytes=%llu scene_sig=0x%llx material_sig=0x%llx instance_sig=0x%llx sky_sig=0x%llx sky_mode=%s sky_source=%s sky_key=0x%llx sky_brightness=%.3f sky_action=%s auto_exposure=%u exposure_texture=%u exposure=%.6f target_exposure=%.6f adapted_exposure=%.6f metered_log_lum=%.6f exposure_stats_valid=%u exposure_stats_frame=%llu final_valid=%u final_nonzero=unknown final_nonzero_ratio=unknown final_luma_mean=unknown final_luma_min=unknown final_luma_max=unknown final_nan=unknown final_inf=unknown scene_reason=ok route_reason=ok exposure_reason=%s present_reason=ok\n",
		traceFrameIndex,
		mFrameIndex,
		currentLevel != nullptr ? currentLevel->labelName.GetChars() : "none",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "none",
		GetGraphicsApiName(mFrameBuffer->GetLiveAPI()),
		worldActive ? 1u : 0u,
		menuactive != MENU_Off ? "yes" : "no",
		gameplayFrame ? 1u : 0u,
		portal ? 1u : 0u,
		drawmode,
		mSelfTestRoute.routeName,
		(int)GetEffectivePtDebugMode(),
		mSelfTestRoute.passes,
		mSelfTestRoute.presenterName,
		mSelfTestRoute.ownerName,
		mSelfTestRoute.denoiserRun ? 1u : 0u,
		mSelfTestRoute.upscalerRun ? 1u : 0u,
		mSelfTestRoute.exposureRun ? 1u : 0u,
		mSelfTestRoute.presenterName,
		mRenderWidth,
		mRenderHeight,
		mOutputWidth,
		mOutputHeight,
		(uint32_t)mFrameBuffer->mCreatedSwapChainFormat,
		outputPolicy.hdrSwapChainActive ? 1u : 0u,
		shell.activePrimitiveCount,
		shell.activeMaterialCount,
		shell.sceneInstanceCount,
		shell.sceneInstanceStaticCount,
		shell.sceneInstanceDynamicCount,
		shell.sceneInstancePersistentVoxelCount,
		mBoundEmissivePrimitiveCount,
		mVertexBuffer.stride != 0 ? (uint32_t)(vertexBytes / mVertexBuffer.stride) : 0u,
		mIndexBuffer.stride != 0 ? (uint32_t)(indexBytes / mIndexBuffer.stride) : 0u,
		(unsigned long long)vertexBytes,
		(unsigned long long)indexBytes,
		(unsigned long long)primitiveBytes,
		(unsigned long long)materialBytes,
		(unsigned long long)(mSceneInstanceBuffer.payloadSize != 0 ? mSceneInstanceBuffer.payloadSize : mSceneInstanceBuffer.usedSize),
		(unsigned long long)sceneSignature,
		(unsigned long long)materialSignature,
		(unsigned long long)instanceSignature,
		(unsigned long long)skySignature,
		GetSkyModeName(mSkyEnvironment.ActiveState().mode),
		GetSkySourceTypeName(mSkyEnvironment.ActiveState().sourceType),
		(unsigned long long)mSkyEnvironment.ActiveKey(),
		mSkyEnvironment.ActiveState().brightness,
		mSkyEnvironment.HasTracedState() ? "traced" : "untraced",
		exposureSettings.enabled ? 1u : 0u,
		mExposure.HasExposureStateTextures() ? 1u : 0u,
		outputPolicy.exposure,
		exposureStatus.targetExposure,
		exposureStatus.adaptedExposure,
		exposureStatus.meteredLogLuminance,
		exposureStatus.debugValid ? 1u : 0u,
		(unsigned long long)exposureStatus.debugFrameIndex,
		finalTextureValid ? 1u : 0u,
		exposureSettings.enabled ? "ok" : "disabled");
}

void NRIRenderer::PrintTemporalStatus() const
{
	SyncLegacyUpscalerConfig(false);
	const NRIPTOutputPolicy outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	const NRIMainUpscalerKind requestedMain = GetSelectedMainUpscalerKind();
	const NRIMainUpscalerKind resolvedMain = GetResolvedMainUpscalerKindForStatus();
	const NRIPostSharpenKind requestedPost = GetSelectedPostSharpenKind();
	const NRIPostSharpenKind resolvedPost = GetResolvedPostSharpenKindForStatus();
	const bool runAppTaa = ShouldRunAppTaa(resolvedMain);
	const float exposure = GetTemporalExposure(outputPolicy);
	const float exposureStops = std::log2(std::max(exposure, 0.125f));
	const FrameTextureSlot presentSlot = mUseUpscaledInFinal ? mUpscaledInputSlot : mHistoryOutputSlot;
	const ExposureRoute exposureRoute = ResolveExposureRoute(presentSlot, outputPolicy, resolvedMain, resolvedPost);
	const NRIAutoExposureSettings& autoExposureSettings = mExposure.GetSettings();
	const NRITextureResource* autoExposureStateTexture = mExposure.GetExposureStateTexture(mFrameIndex & 1u);
	const bool autoExposureTextureValid =
		autoExposureStateTexture != nullptr &&
		autoExposureStateTexture->shaderView != nullptr;
	const bool autoExposureTaaApply =
		runAppTaa &&
		autoExposureSettings.enabled &&
		autoExposureTextureValid;
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
	Printf("NRI PT temporal domain: history=%s present=%s temporal_exposure=%.3f present_exposure=%.3f exposure_stops=%.3f reset_threshold_stops=%.3f auto_exposure=%s exposure_texture=%s taa_apply=%s\n",
		GetExposureDomainName(ResolveFrameTextureExposureDomain(mHistoryOutputSlot, resolvedMain, resolvedPost)),
		GetExposureDomainName(exposureRoute.inputDomain),
		exposure,
		exposureRoute.presentExposure,
		exposureStops,
		NRI_TAA_EXPOSURE_RESET_THRESHOLD_STOPS,
		YesNo(autoExposureSettings.enabled),
		YesNo(autoExposureTextureValid),
		YesNo(autoExposureTaaApply));
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

NRIStaticSceneGeometryUploadServices NRIRenderer::BuildStaticSceneGeometryUploadServices()
{
	NRIStaticSceneGeometryUploadServices services = {};
	services.user = this;
	services.ensureResidentStructuredBuffer = [](void* user, NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, const char* waitReason, int uploadKind) -> bool
	{
		return static_cast<NRIRenderer*>(user)->EnsureResidentStructuredBuffer(resource, stats, data, size, stride, usage, after, waitReason, uploadKind);
	};
	services.refreshResidentStaticSceneDataSet = [](void* user) -> bool
	{
		return static_cast<NRIRenderer*>(user)->RefreshResidentStaticSceneDataSet();
	};
	services.noteResidentStaticAtlasGrow = [](void* user)
	{
		static_cast<NRIRenderer*>(user)->NoteResidentStaticAtlasGrow();
	};
	return services;
}

void NRIRenderer::SyncResidentMapChunkRegistryFromStaticScene()
{
	std::vector<RuntimeMutationResidentReplacementInfo> replacements;
	if (mMapWorld.valid)
	{
		mRuntimeMutation.CollectResidentReplacementInfo((uint32_t)mMapWorld.chunks.size(), replacements);
	}

	NRIStaticSceneRegistrySyncInput input = {};
	input.mapWorld = &mMapWorld;
	input.staticScene = &mStaticMapScene;
	input.atlas = &mStaticMapChunkAtlas;
	input.replacements = &replacements;
	input.hashResidentMaterialPayload = nri_runtime_mutation::HashResidentMaterialPayload;
	mStaticSceneResidency.SyncResidentMapChunkRegistryFromStaticScene(input);
}

void NRIRenderer::TraceTemporalState(const char* stage, NRIMainUpscalerKind resolvedMainUpscaler, NRIPostSharpenKind resolvedPostSharpen, bool runAppTaa, FrameTextureSlot primarySlot, FrameTextureSlot secondarySlot) const
{
	if (!ShouldEmitRendererTemporalTraceLogs())
	{
		return;
	}

	const NRITextureResource& historyInput = GetFrameTexture(mHistoryInputSlot);
	const NRITextureResource& historyOutput = GetFrameTexture(mHistoryOutputSlot);
	const NRITextureResource& primary = GetFrameTexture(primarySlot);
	const NRITextureResource& secondary = secondarySlot == FrameTextureSlot::Count ? GetFrameTexture(mHistoryOutputSlot) : GetFrameTexture(secondarySlot);
	const FrameTextureSlot resolvedSecondarySlot = secondarySlot == FrameTextureSlot::Count ? mHistoryOutputSlot : secondarySlot;
	const NRIPTOutputPolicy outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	const ExposureRoute primaryExposureRoute = ResolveExposureRoute(primarySlot, outputPolicy, resolvedMainUpscaler, resolvedPostSharpen);
	const ExposureRoute secondaryExposureRoute = ResolveExposureRoute(resolvedSecondarySlot, outputPolicy, resolvedMainUpscaler, resolvedPostSharpen);
	Printf("NRI PT temporal trace: stage=%s frame=%u debug=%d resolved_main=%s resolved_post=%s run_app_taa=%s gui_capture=%s primary_domain=%s secondary_domain=%s temporal_exposure=%.3f primary_present_exposure=%.3f secondary_present_exposure=%.3f reset=%s reset_reason=%s prev_camera=%s history_in=%s[%ux%u a=%u l=%u s=0x%x] history_out=%s[%ux%u a=%u l=%u s=0x%x] primary=%s[%ux%u a=%u l=%u s=0x%x] secondary=%s[%ux%u a=%u l=%u s=0x%x] use_upscaled=%s\n",
		stage != nullptr ? stage : "unknown",
		mFrameIndex,
		(int)nri_ptdebug,
		GetMainUpscalerName(resolvedMainUpscaler),
		GetPostSharpenName(resolvedPostSharpen),
		runAppTaa ? "yes" : "no",
		mGuiCaptureActive ? "yes" : "no",
		GetExposureDomainName(primaryExposureRoute.inputDomain),
		GetExposureDomainName(secondaryExposureRoute.inputDomain),
		primaryExposureRoute.temporalExposure,
		primaryExposureRoute.presentExposure,
		secondaryExposureRoute.presentExposure,
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
		GetFrameTextureSlotName(resolvedSecondarySlot),
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

void NRIRenderer::PrintResidentMapChunkRegistryStatus() const
{
	if (!mStaticSceneResidency.Registry().valid)
	{
		Printf("NRI PT resident chunk registry: unavailable.\n");
		return;
	}

	Printf("NRI PT resident chunk registry: build_serial=%llu chunks=%u active=%u mapped=%u acceleration_resident=%u animated_candidates=%u animated_refresh_suppressed=%u\n",
		(unsigned long long)mStaticSceneResidency.Registry().buildSerial,
		mStaticSceneResidency.Registry().chunkCount,
		mStaticSceneResidency.Registry().activeChunkCount,
		mStaticSceneResidency.Registry().mappedChunkCount,
		mStaticSceneResidency.Registry().accelerationResidentChunkCount,
		mStaticSceneResidency.Registry().animatedCandidateChunkCount,
		mStaticSceneResidency.Registry().animatedRefreshSuppressedChunkCount);

	const NRIRuntimeMutationSettings runtimeMutationSettings = BuildNRIRuntimeMutationSettingsFromCVars();
	const float nearDistance = runtimeMutationSettings.nearDistance;
	const float nearDistanceSquared = nearDistance * nearDistance;
	uint32_t boundsValidCount = 0;
	uint32_t boundsInvalidCount = 0;
	uint32_t visibleCount = 0;
	uint32_t invisibleNearCount = 0;
	uint32_t invisibleFarCount = 0;
	uint32_t invisibleUnknownCount = 0;
	const nri_scene::PTMapChunk* sampleChunk = nullptr;
	float sampleDistance = 0.0f;
	const char* sampleTier = "none";
	const auto computeChunkDistanceSquared = [&](const nri_scene::PTMapChunk& chunk, float& outDistanceSquared) -> bool
	{
		if (!chunk.bounds.valid)
		{
			outDistanceSquared = 0.0f;
			return false;
		}

		outDistanceSquared = 0.0f;
		for (int axis = 0; axis < 3; ++axis)
		{
			float distance = 0.0f;
			if (mCurrentCameraPos[axis] < chunk.bounds.min[axis])
			{
				distance = chunk.bounds.min[axis] - mCurrentCameraPos[axis];
			}
			else if (mCurrentCameraPos[axis] > chunk.bounds.max[axis])
			{
				distance = mCurrentCameraPos[axis] - chunk.bounds.max[axis];
			}
			outDistanceSquared += distance * distance;
		}
		return true;
	};
	for (const nri_scene::PTMapChunk& chunk : mMapWorld.chunks)
	{
		float distanceSquared = 0.0f;
		const bool boundsValid = computeChunkDistanceSquared(chunk, distanceSquared);
		if (boundsValid)
		{
			boundsValidCount++;
		}
		else
		{
			boundsInvalidCount++;
		}

		const bool visible = IsChunkMarkedVisible(mCurrentVisibleChunkWords, chunk.chunkIndex);
		if (visible)
		{
			visibleCount++;
		}
		else if (!boundsValid)
		{
			invisibleUnknownCount++;
		}
		else if (distanceSquared <= nearDistanceSquared)
		{
			invisibleNearCount++;
		}
		else
		{
			invisibleFarCount++;
		}

		if (sampleChunk == nullptr && boundsValid)
		{
			sampleChunk = &chunk;
			sampleDistance = sqrtf(distanceSquared);
			sampleTier =
				visible ? "visible" :
				(distanceSquared <= nearDistanceSquared ? "near" : "far");
		}
	}
	Printf("NRI PT map chunk bounds: chunks=%u valid=%u invalid=%u near_distance=%.1f visible=%u invisible_near=%u invisible_far=%u invisible_unknown=%u sample_chunk=%u center=(%.1f,%.1f,%.1f) radius=%.1f distance=%.1f tier=%s\n",
		(uint32_t)mMapWorld.chunks.size(),
		boundsValidCount,
		boundsInvalidCount,
		(double)nearDistance,
		visibleCount,
		invisibleNearCount,
		invisibleFarCount,
		invisibleUnknownCount,
		sampleChunk != nullptr ? sampleChunk->chunkIndex : UINT32_MAX,
		sampleChunk != nullptr ? (double)sampleChunk->bounds.center[0] : 0.0,
		sampleChunk != nullptr ? (double)sampleChunk->bounds.center[1] : 0.0,
		sampleChunk != nullptr ? (double)sampleChunk->bounds.center[2] : 0.0,
		sampleChunk != nullptr ? (double)sampleChunk->bounds.radius : 0.0,
		(double)sampleDistance,
		sampleTier);
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
		mSceneTextures.OverflowStats().textureCountLastBuild,
		mSceneTextures.OverflowStats().truncatedTextureCountLastBuild,
		mSceneTextures.OverflowStats().baseTextureClampCountLastBuild,
		mSceneTextures.OverflowStats().normalTextureClampCountLastBuild,
		mSceneTextures.OverflowStats().metallicTextureClampCountLastBuild,
		mSceneTextures.OverflowStats().roughnessTextureClampCountLastBuild,
		mSceneTextures.OverflowStats().emissiveTextureClampCountLastBuild,
		(unsigned long long)mSceneTextures.OverflowStats().totalOverflowBuilds,
		mSceneTextures.OverflowStats().warningLogged ? "yes" : "no");
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
		mSceneTextures.CacheStats().cacheEntriesLastBuild,
		mSceneTextures.CacheStats().cacheEntriesHighWater,
		mSceneTextures.CacheStats().lookupMissesLastBuild,
		mSceneTextures.CacheStats().insertCountLastBuild,
		mSceneTextures.CacheStats().transitionCountLastFrame,
		mSceneTextures.CacheStats().lookupMsLastBuild,
		mSceneTextures.CacheStats().realizeMsLastBuild,
		mSceneTextures.CacheStats().descriptorMsLastBuild,
		mSceneTextures.CacheStats().transitionMsLastFrame);
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

NRIPersistentVoxelResetServices NRIRenderer::BuildPersistentVoxelResetServices()
{
	NRIPersistentVoxelResetServices services = {};
	services.user = this;
	services.retireBuffer = [](void* user, NRIBufferResource& resource)
	{
		static_cast<NRIRenderer*>(user)->RetireResidentBufferResource(resource);
	};
	services.retireAccelerationStructure = [](void* user, NRIAccelerationStructureResource& resource)
	{
		static_cast<NRIRenderer*>(user)->RetireResidentAccelerationStructure(resource);
	};
	services.invalidateSceneDataDescriptors = [](void* user)
	{
		static_cast<NRIRenderer*>(user)->SetCurrentSceneDataDescriptorsInitialized(false);
	};
	return services;
}

NRIPersistentVoxelAdmissionServices NRIRenderer::BuildPersistentVoxelAdmissionServices()
{
	NRIPersistentVoxelAdmissionServices services = {};
	services.user = this;
	services.admitVariantResource = [](
		void* user,
		PersistentVoxelAdmissionEntry& entry,
		uint64_t byteBudget,
		uint32_t& blasBudget,
		uint64_t& outUploadBytes,
		bool& outReusedMesh,
		bool& outReusedMaterial,
		bool& outInProgress,
		bool isolateBlasBuild,
		const char*& outFailureReason) -> bool
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		return renderer->mPersistentVoxels.AdmitVariantResource(
			entry,
			byteBudget,
			blasBudget,
			outUploadBytes,
			outReusedMesh,
			outReusedMaterial,
			outInProgress,
			isolateBlasBuild,
			outFailureReason,
			renderer->mFrameIndex,
			(int)nri_ptloadingtrace,
			(bool)nri_voxelstats,
			renderer->BuildPersistentVoxelAdmissionServices());
	};
	services.submitWaitAndRestart = [](void* user, const char* reason) -> bool
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		return renderer->mFrameBuffer != nullptr && renderer->mFrameBuffer->SubmitWaitAndRestartCommandList(reason);
	};
	services.retireBuffer = [](void* user, NRIBufferResource& resource)
	{
		static_cast<NRIRenderer*>(user)->RetireResidentBufferResource(resource);
	};
	services.retireAccelerationStructure = [](void* user, NRIAccelerationStructureResource& resource)
	{
		static_cast<NRIRenderer*>(user)->RetireResidentAccelerationStructure(resource);
	};
	services.buildMaterials = [](void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label)
	{
		Clocker materialClock(NriPTMaterialBuild);
		static_cast<NRIRenderer*>(user)->BuildMaterialsWithActorOverrides(sceneView, materials, label);
	};
	services.prewarmTexture = [](void* user, const nri_scene::TextureUpload& upload) -> bool
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		if (upload.width == 0 || upload.height == 0)
		{
			return true;
		}
		if (renderer->mFrameBuffer != nullptr &&
			renderer->mFrameBuffer->mActiveCanvasSourceTexture != nullptr &&
			upload.sourceTexture == renderer->mFrameBuffer->mActiveCanvasSourceTexture)
		{
			return true;
		}
		if (upload.sourceTexture != nullptr && upload.sourceTexture->isHardwareCanvas())
		{
			return true;
		}
		return renderer->EnsureSceneTextureCacheEntry(upload);
	};
	services.assignGeometryPortalIndices = [](void* user, nri_scene::GeometryData& geometry)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		AssignGeometryPortalIndices(renderer->mMapWorld, geometry);
	};
	services.createStructuredBufferNoUpload = [](void* user, NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage) -> bool
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		if (renderer->mFrameBuffer == nullptr ||
			!renderer->CreateBufferWithoutViewAtLocation(resource, size, stride, usage, nri::MemoryLocation::DEVICE))
		{
			return false;
		}
		nri::BufferViewDesc viewDesc = {};
		viewDesc.buffer = resource.buffer;
		viewDesc.type = nri::BufferView::STRUCTURED_BUFFER;
		viewDesc.offset = 0;
		viewDesc.size = nri::WHOLE_SIZE;
		viewDesc.structureStride = stride;
		if (renderer->mFrameBuffer->mCore.CreateBufferView(viewDesc, resource.shaderView) != nri::Result::SUCCESS)
		{
			renderer->DestroyBufferResource(resource);
			return false;
		}
		resource.usedSize = size;
		return true;
	};
	services.ensureArenaBuffer = [](void* user, NRIBufferResource& resource, uint64_t requiredSize, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after) -> bool
	{
		return static_cast<NRIRenderer*>(user)->EnsureResidentArenaBuffer(resource, requiredSize, stride, usage, after);
	};
	services.stageBufferCopyRange = [](void* user, NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after, int uploadKind) -> bool
	{
		return static_cast<NRIRenderer*>(user)->StageResidentBufferCopyRange(resource, byteOffset, data, size, after, uploadKind);
	};
	services.noteBufferUpload = [](void* user, int uploadKind, uint64_t size, const char* reason)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		SceneBufferDebugStats* stats =
			uploadKind == ResidentUploadKind_Index ? &renderer->mIndexBufferStats :
			(uploadKind == ResidentUploadKind_Primitive ? &renderer->mPrimitiveBufferStats : &renderer->mVertexBufferStats);
		renderer->NotePerfBufferUpload(stats, size, false, reason, uploadKind);
	};
	services.buildBottomLevel = [](
		void* user,
		const NRIBufferResource& vertexBuffer,
		const NRIBufferResource& indexBuffer,
		uint32_t vertexCount,
		uint32_t indexOffset,
		uint32_t indexCount,
		uint32_t primitiveCount,
		NRIAccelerationStructureResource& outAccelerationStructure) -> bool
	{
		return static_cast<NRIRenderer*>(user)->BuildBottomLevelAccelerationStructure(
			vertexBuffer,
			indexBuffer,
			vertexCount,
			indexOffset,
			indexCount,
			primitiveCount,
			outAccelerationStructure,
			false);
	};
	services.barrierBuildInputs = [](void* user, const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer) -> bool
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		if (renderer->mFrameBuffer == nullptr || renderer->mFrameBuffer->mCommandBuffer == nullptr)
		{
			return false;
		}
		nri::BufferBarrierDesc inputBarriers[2] = {};
		inputBarriers[0].buffer = vertexBuffer.buffer;
		inputBarriers[0].before = NRIAccelerationStructureBuildInputAccess();
		inputBarriers[0].after = NRIComputeShaderResourceAccess();
		inputBarriers[1].buffer = indexBuffer.buffer;
		inputBarriers[1].before = NRIAccelerationStructureBuildInputAccess();
		inputBarriers[1].after = NRIComputeShaderResourceAccess();
		nri::BarrierDesc inputBarrierDesc = {};
		inputBarrierDesc.buffers = inputBarriers;
		inputBarrierDesc.bufferNum = 2;
		renderer->mFrameBuffer->mCore.CmdBarrier(*renderer->mFrameBuffer->mCommandBuffer, inputBarrierDesc);
		return true;
	};
	return services;
}

NRIPersistentVoxelAccelerationServices NRIRenderer::BuildPersistentVoxelAccelerationServices()
{
	NRIPersistentVoxelAccelerationServices services = {};
	services.user = this;
	services.buildBottomLevel = [](
		void* user,
		const NRIBufferResource& vertexBuffer,
		const NRIBufferResource& indexBuffer,
		uint32_t vertexCount,
		uint32_t indexOffset,
		uint32_t indexCount,
		uint32_t primitiveCount,
		NRIAccelerationStructureResource& outAccelerationStructure) -> bool
	{
		return static_cast<NRIRenderer*>(user)->BuildBottomLevelAccelerationStructure(
			vertexBuffer,
			indexBuffer,
			vertexCount,
			indexOffset,
			indexCount,
			primitiveCount,
			outAccelerationStructure,
			false);
	};
	services.barrierBuildInputs = [](void* user, const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer) -> bool
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		if (renderer->mFrameBuffer == nullptr || renderer->mFrameBuffer->mCommandBuffer == nullptr)
		{
			return false;
		}
		nri::BufferBarrierDesc inputBarriers[2] = {};
		inputBarriers[0].buffer = vertexBuffer.buffer;
		inputBarriers[0].before = NRIAccelerationStructureBuildInputAccess();
		inputBarriers[0].after = NRIComputeShaderResourceAccess();
		inputBarriers[1].buffer = indexBuffer.buffer;
		inputBarriers[1].before = NRIAccelerationStructureBuildInputAccess();
		inputBarriers[1].after = NRIComputeShaderResourceAccess();
		nri::BarrierDesc inputBarrierDesc = {};
		inputBarrierDesc.buffers = inputBarriers;
		inputBarrierDesc.bufferNum = 2;
		renderer->mFrameBuffer->mCore.CmdBarrier(*renderer->mFrameBuffer->mCommandBuffer, inputBarrierDesc);
		return true;
	};
	return services;
}

bool NRIRenderer::PreloadPersistentVoxelResources()
{
	std::vector<nri_scene::PrecachedVoxelVariantView> variants;
	std::vector<nri_scene::PersistentVoxelCacheEntryView> cacheEntries;
	const bool gpuLoadingEnabled = (bool)nri_ptloadingvoxelgpu;
	bool hasCacheEntries = false;
	if (gpuLoadingEnabled)
	{
		nri_scene::BuildPrecachedVoxelVariantViews(variants);
		hasCacheEntries = nri_scene::BuildPersistentVoxelCacheEntries(cacheEntries);
	}

	const NRIPersistentVoxelSettings persistentVoxelSettings = BuildNRIPersistentVoxelSettingsFromCVars();
	NRIPersistentVoxelPreloadServices preloadServices = {};
	preloadServices.user = this;
	preloadServices.pumpAdmissionQueue = [](void* user, const char* phase) -> bool
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		const NRIPersistentVoxelSettings settings = BuildNRIPersistentVoxelSettingsFromCVars();
		const NRIRenderer::MemoryTelemetry telemetry = renderer->GetMemoryTelemetry();
		return renderer->mPersistentVoxels.PumpAdmissionQueue(
			phase,
			renderer->mMapWorld.buildSerial,
			renderer->mFrameIndex,
			settings,
			telemetry.totalTrackedBytes,
			renderer->mFrameBuffer != nullptr ? renderer->mFrameBuffer->GetAdapterLocalBudgetBytes() : 0ull,
			(int)nri_ptloadingtrace,
			(bool)nri_voxelstats,
			renderer->BuildPersistentVoxelResetServices(),
			renderer->BuildPersistentVoxelAdmissionServices());
	};
	preloadServices.ensureBatch = [](void* user) -> bool
	{
		return static_cast<NRIRenderer*>(user)->EnsurePersistentVoxelBatch();
	};
	return mPersistentVoxels.PreloadResources(
		variants,
		cacheEntries,
		hasCacheEntries,
		gpuLoadingEnabled,
		mMapWorld.buildSerial,
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : nullptr,
		mFrameIndex,
		persistentVoxelSettings,
		(int)nri_ptloadingtrace,
		(bool)nri_voxelstats,
		BuildPersistentVoxelResetServices(),
		preloadServices);
}

bool NRIRenderer::PreloadMaterialResources()
{
	struct MaterialWarmupStats
	{
		uint32_t textureRequests = 0;
		uint32_t textureHits = 0;
		uint32_t textureMisses = 0;
		uint32_t textureInserts = 0;
		uint64_t estimatedBytes = 0;
		double realizeMs = 0.0;
	};

	auto isTextureCached = [&](const nri_scene::TextureUpload& upload) -> bool
	{
		return FindSceneTextureCacheIndex(upload.key) != UINT32_MAX;
	};

	auto warmMaterialTextures = [&](const nri_scene::MaterialBridgeData& materials, MaterialWarmupStats& stats) -> bool
	{
		for (const nri_scene::TextureUpload& upload : materials.textures)
		{
			if (upload.width == 0 || upload.height == 0)
			{
				continue;
			}

			stats.textureRequests++;
			const bool wasCached = isTextureCached(upload);
			if (wasCached)
			{
				stats.textureHits++;
				continue;
			}

			stats.textureMisses++;
			stats.estimatedBytes += EstimateSceneTextureUploadBytes(upload);
			double realizeMs = 0.0;
			if (!EnsureSceneTextureCacheEntry(upload, &realizeMs))
			{
				return false;
			}
			stats.realizeMs += realizeMs;
			if (isTextureCached(upload))
			{
				stats.textureInserts++;
			}
		}
		return true;
	};

	NRIPersistentVoxelMaterialWarmupServices voxelWarmupServices = {};
	voxelWarmupServices.user = this;
	voxelWarmupServices.ensurePalette = [](void* user, const nri_scene::MaterialBridgeData& materials) -> bool
	{
		return static_cast<NRIRenderer*>(user)->EnsurePaletteTexture(materials);
	};
	voxelWarmupServices.warmTextures = [](void* user, const nri_scene::MaterialBridgeData& materials, NRIPersistentVoxelMaterialWarmupStats& stats) -> bool
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		for (const nri_scene::TextureUpload& upload : materials.textures)
		{
			if (upload.width == 0 || upload.height == 0)
			{
				continue;
			}

			stats.textureRequests++;
			const bool wasCached = renderer->FindSceneTextureCacheIndex(upload.key) != UINT32_MAX;
			if (wasCached)
			{
				stats.textureHits++;
				continue;
			}

			stats.textureMisses++;
			stats.estimatedBytes += EstimateSceneTextureUploadBytes(upload);
			double realizeMs = 0.0;
			if (!renderer->EnsureSceneTextureCacheEntry(upload, &realizeMs))
			{
				return false;
			}
			stats.realizeMs += realizeMs;
			if (renderer->FindSceneTextureCacheIndex(upload.key) != UINT32_MAX)
			{
				stats.textureInserts++;
			}
		}
		return true;
	};

	const auto start = std::chrono::steady_clock::now();
	MaterialWarmupStats staticStats = {};
	NRIPersistentVoxelMaterialWarmupResult voxelWarmup = {};
	const bool hasStaticMaterials = mStaticMapScene.valid && !mStaticMapScene.materialBridge.materials.empty();
	bool paletteReady = true;
	if (hasStaticMaterials)
	{
		paletteReady = EnsurePaletteTexture(mStaticMapScene.materialBridge);
		if (!paletteReady || !warmMaterialTextures(mStaticMapScene.materialBridge, staticStats))
		{
			return false;
		}
	}
	if (!mPersistentVoxels.WarmMaterialResources(voxelWarmupServices, voxelWarmup))
	{
		return false;
	}
	paletteReady = paletteReady && voxelWarmup.paletteReady;

	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading material: event=warm palette=%u static_materials=%u static_textures=%u static_hits=%u static_misses=%u static_inserts=%u static_bytes=%llu voxel_materials=%u voxel_variants=%u voxel_textures=%u voxel_hits=%u voxel_misses=%u voxel_inserts=%u voxel_bytes=%llu cache=%u realize_ms=%.3f ms=%.3f\n",
			paletteReady ? 1u : 0u,
			hasStaticMaterials ? (uint32_t)mStaticMapScene.materialBridge.materials.size() : 0u,
			staticStats.textureRequests,
			staticStats.textureHits,
			staticStats.textureMisses,
			staticStats.textureInserts,
			(unsigned long long)staticStats.estimatedBytes,
			voxelWarmup.materialCount,
			voxelWarmup.variantResourceCount,
			voxelWarmup.textureStats.textureRequests,
			voxelWarmup.textureStats.textureHits,
			voxelWarmup.textureStats.textureMisses,
			voxelWarmup.textureStats.textureInserts,
			(unsigned long long)voxelWarmup.textureStats.estimatedBytes,
			mSceneTextures.CacheCount(),
			staticStats.realizeMs + voxelWarmup.textureStats.realizeMs,
			DurationMs(start, std::chrono::steady_clock::now()));
	}
	return true;
}

bool NRIRenderer::EnsurePersistentVoxelBatch()
{
	NRIPersistentVoxelBatchServices batchServices = {};
	batchServices.user = this;
	batchServices.buildMaterials = [](void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label)
	{
		static_cast<NRIRenderer*>(user)->BuildMaterialsWithActorOverrides(sceneView, materials, label);
	};
	batchServices.isTextureCached = [](void* user, const nri_scene::TextureUpload& upload) -> bool
	{
		return static_cast<NRIRenderer*>(user)->FindSceneTextureCacheIndex(upload.key) != UINT32_MAX;
	};
	batchServices.prewarmTexture = [](void* user, const nri_scene::TextureUpload& upload, double* outMs) -> bool
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		if (upload.width == 0 || upload.height == 0)
		{
			return true;
		}
		if (renderer->mFrameBuffer != nullptr &&
			renderer->mFrameBuffer->mActiveCanvasSourceTexture != nullptr &&
			upload.sourceTexture == renderer->mFrameBuffer->mActiveCanvasSourceTexture)
		{
			return true;
		}
		if (upload.sourceTexture != nullptr && upload.sourceTexture->isHardwareCanvas())
		{
			return true;
		}
		return renderer->EnsureSceneTextureCacheEntry(upload, outMs);
	};
	batchServices.assignGeometryPortalIndices = [](void* user, nri_scene::GeometryData& geometry)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		AssignGeometryPortalIndices(renderer->mMapWorld, geometry);
	};
	batchServices.ensureStructuredBuffer = [](void* user, NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, const char* reason, int uploadKind) -> bool
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		SceneBufferDebugStats* stats =
			uploadKind == ResidentUploadKind_Index ? &renderer->mIndexBufferStats :
			(uploadKind == ResidentUploadKind_Primitive ? &renderer->mPrimitiveBufferStats : &renderer->mVertexBufferStats);
		return renderer->EnsureResidentStructuredBuffer(resource, *stats, data, size, stride, usage, after, reason, uploadKind);
	};
	batchServices.ensureArenaBuffer = [](void* user, NRIBufferResource& resource, uint64_t requiredSize, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after) -> bool
	{
		return static_cast<NRIRenderer*>(user)->EnsureResidentArenaBuffer(resource, requiredSize, stride, usage, after);
	};
	batchServices.stageBufferCopyRange = [](void* user, NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after, int uploadKind) -> bool
	{
		return static_cast<NRIRenderer*>(user)->StageResidentBufferCopyRange(resource, byteOffset, data, size, after, uploadKind);
	};
	batchServices.noteBufferUpload = [](void* user, int uploadKind, uint64_t size, const char* reason)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		SceneBufferDebugStats* stats =
			uploadKind == ResidentUploadKind_Index ? &renderer->mIndexBufferStats :
			(uploadKind == ResidentUploadKind_Primitive ? &renderer->mPrimitiveBufferStats : &renderer->mVertexBufferStats);
		renderer->NotePerfBufferUpload(stats, size, false, reason, uploadKind);
	};
	batchServices.retireAccelerationStructure = [](void* user, NRIAccelerationStructureResource& resource)
	{
		static_cast<NRIRenderer*>(user)->RetireResidentAccelerationStructure(resource);
	};
	batchServices.materialWouldEmit = [](void* user, const nri_scene::MaterialLightingMetadata& metadata) -> bool
	{
		return static_cast<NRIRenderer*>(user)->mSceneLights.MaterialWouldEmit(metadata);
	};
	batchServices.buildSurfaceRecord = [](void* user, const nri_scene::SurfaceRef& surface, const nri_scene::MaterialBridgeData& materials, SceneLightRecordSource source, uint32_t materialIndex, uint32_t primitiveIndex) -> SceneLightSystem::SurfaceRecord
	{
		return static_cast<NRIRenderer*>(user)->mSceneLights.BuildSurfaceRecord(surface, materials, source, materialIndex, primitiveIndex);
	};

	NRIPersistentVoxelBatchStats batchStats = {};
	const bool result = mPersistentVoxels.EnsureBatch(
		mMapWorld.buildSerial,
		mFrameIndex,
		BuildNRIPersistentVoxelSettingsFromCVars(),
		(int)nri_ptloadingtrace,
		(bool)nri_voxelstats,
		BuildPersistentVoxelResetServices(),
		batchServices,
		batchStats);

	mLastPerfShellTraceStats.geometryBuildPersistentVoxelVariantMs += batchStats.geometryBuildPersistentVoxelVariantMs;
	mLastPerfShellTraceStats.geometryBuildPersistentVoxelAppendMs += batchStats.geometryBuildPersistentVoxelAppendMs;
	mLastPerfShellTraceStats.geometryBuildPersistentVoxelRebuildMs += batchStats.geometryBuildPersistentVoxelRebuildMs;
	mLastPerfShellTraceStats.persistentVoxelTexturePrewarmMs += batchStats.persistentVoxelTexturePrewarmMs;
	mLastPerfShellTraceStats.geometryBuildPersistentVoxelVariantCalls += batchStats.geometryBuildPersistentVoxelVariantCalls;
	mLastPerfShellTraceStats.geometryBuildPersistentVoxelVariantPrimitives += batchStats.geometryBuildPersistentVoxelVariantPrimitives;
	mLastPerfShellTraceStats.persistentVoxelTexturePrewarmHitCount += batchStats.persistentVoxelTexturePrewarmHitCount;
	mLastPerfShellTraceStats.persistentVoxelTexturePrewarmQueuedCount += batchStats.persistentVoxelTexturePrewarmQueuedCount;
	mLastPerfShellTraceStats.persistentVoxelTexturePrewarmMissCount += batchStats.persistentVoxelTexturePrewarmMissCount;
	mLastPerfShellTraceStats.persistentVoxelTexturePrewarmDeferredCount += batchStats.persistentVoxelTexturePrewarmDeferredCount;
	mLastPerfShellTraceStats.persistentVoxelTexturePrewarmProcessedCount += batchStats.persistentVoxelTexturePrewarmProcessedCount;
	mLastPerfShellTraceStats.persistentVoxelTexturePrewarmByteBudget = batchStats.persistentVoxelTexturePrewarmByteBudget;
	mLastPerfShellTraceStats.persistentVoxelTexturePrewarmEstimatedBytes += batchStats.persistentVoxelTexturePrewarmEstimatedBytes;
	mLastPerfShellTraceStats.persistentVoxelTexturePrewarmDeferredBytes += batchStats.persistentVoxelTexturePrewarmDeferredBytes;
	mLastPerfShellTraceStats.persistentVoxelTexturePrewarmProcessedBytes += batchStats.persistentVoxelTexturePrewarmProcessedBytes;
	mLastPerfShellTraceStats.persistentVoxelOnboardingCandidateCount += batchStats.persistentVoxelOnboardingCandidateCount;
	mLastPerfShellTraceStats.persistentVoxelOnboardingDeferredCount += batchStats.persistentVoxelOnboardingDeferredCount;
	mLastPerfShellTraceStats.persistentVoxelOnboardingPrimitiveBudgetHits += batchStats.persistentVoxelOnboardingPrimitiveBudgetHits;
	mLastPerfShellTraceStats.persistentVoxelOnboardingByteBudgetHits += batchStats.persistentVoxelOnboardingByteBudgetHits;
	mLastPerfShellTraceStats.persistentVoxelOnboardingActorBudgetHits += batchStats.persistentVoxelOnboardingActorBudgetHits;
	mLastPerfShellTraceStats.persistentVoxelOnboardingAdmittedCount += batchStats.persistentVoxelOnboardingAdmittedCount;
	mLastPerfShellTraceStats.persistentVoxelOnboardingTextureBudgetHits += batchStats.persistentVoxelOnboardingTextureBudgetHits;
	mLastPerfShellTraceStats.persistentVoxelOnboardingEstimatedBytes += batchStats.persistentVoxelOnboardingEstimatedBytes;
	mLastPerfShellTraceStats.persistentVoxelOnboardingDeferredBytes += batchStats.persistentVoxelOnboardingDeferredBytes;
	mLastPerfShellTraceStats.persistentVoxelOnboardingAdmittedBytes += batchStats.persistentVoxelOnboardingAdmittedBytes;
	mLastPerfShellTraceStats.persistentVoxelOnboardingByteBudget = batchStats.persistentVoxelOnboardingByteBudget;
	mLastPerfShellTraceStats.persistentVoxelInstanceTransformUpdates += batchStats.persistentVoxelInstanceTransformUpdates;
	return result;
}
bool NRIRenderer::UploadPersistentVoxelArenaMaterialBuffers(const std::vector<nri_scene::MaterialData>& materials)
{
	if (!mPersistentVoxels.HasValidBatch())
	{
		return true;
	}
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialMs);

	NRIPersistentVoxelMaterialUploadServices services = {};
	services.user = this;
	services.ensureMaterialArenaBuffer = [](void* user, NRIBufferResource& resource, uint64_t sizeBytes) -> bool
	{
		return static_cast<NRIRenderer*>(user)->EnsureResidentArenaBuffer(
			resource,
			sizeBytes,
			sizeof(nri_scene::MaterialData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess());
	};
	services.stageMaterialRanges = [](
		void* user,
		const NRIBufferResource& targetBuffer,
		const std::vector<RuntimeMutationResidentUploadRange>& ranges,
		const uint8_t* data,
		uint64_t availableBytes) -> bool
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		return renderer->StageResidentMaterialUploadRanges(
			targetBuffer,
			ranges,
			data,
			availableBytes,
			renderer->mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialBatches,
			renderer->mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialBatchRanges,
			renderer->mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialBatchBarrierCommands,
			renderer->mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialBatchCopyCommands);
	};
	services.noteMaterialUpload = [](void* user, uint64_t sizeBytes)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		renderer->NotePerfBufferUpload(
			&renderer->mMaterialBufferStats,
			sizeBytes,
			false,
			"persistent_voxel_material_variant",
			ResidentUploadKind_Material);
	};

	NRIPersistentVoxelMaterialUploadStats uploadStats = {};
	const bool uploaded = mPersistentVoxels.UploadArenaMaterialBuffers(
		materials,
		services,
		mFrameIndex,
		(bool)nri_voxelstats,
		uploadStats);

	auto& persistentVoxelDomain =
		mLastPerfShellTraceStats.sceneSelectBufferUploadDomains[(size_t)SceneBufferUploadDomain::PersistentVoxelMaterial];
	mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialRequestedBytes += uploadStats.requestedBytes;
	mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialUploads += uploadStats.uploads;
	mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialDirtyBytes += uploadStats.dirtyBytes;
	mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialBatchRejects += uploadStats.batchRejects;
	mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialBatchGapBytes += uploadStats.batchGapBytes;
	mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialUploadedBytes += uploadStats.uploadedBytes;
	persistentVoxelDomain.payloadBytes += uploadStats.domainPayloadBytes;
	persistentVoxelDomain.materialPayloadBytes += uploadStats.domainMaterialPayloadBytes;
	persistentVoxelDomain.hashChecks += uploadStats.domainHashChecks;
	persistentVoxelDomain.hashMisses += uploadStats.domainHashMisses;
	persistentVoxelDomain.uploadedBytes += uploadStats.domainUploadedBytes;
	persistentVoxelDomain.materialUploadedBytes += uploadStats.domainMaterialUploadedBytes;
	return uploaded;
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
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildPersistentEmissivePruneMs);
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
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildPersistentEmissiveRebuildMs);
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

void NRIRenderer::PrintSceneBufferStatus() const
{
	NRISceneBufferStatusSnapshot snapshot = {};
	const auto appendBuffer = [&snapshot](const NRIBufferResource& resource, const SceneBufferDebugStats& stats)
	{
		snapshot.buffers.push_back(BuildNRIBufferStatusSnapshot(resource, stats));
	};

	const NRIBufferResource& activeVertexBuffer = GetActiveVertexBuffer();
	const NRIBufferResource& activeIndexBuffer = GetActiveIndexBuffer();
	const NRIBufferResource& activePrimitiveBuffer = GetActivePrimitiveBuffer();
	const NRIBufferResource& activeMaterialBuffer = GetActiveMaterialBuffer();
	snapshot.totalUsedBytes = activeVertexBuffer.usedSize + activeIndexBuffer.usedSize + activePrimitiveBuffer.usedSize + activeMaterialBuffer.usedSize;
	snapshot.totalCapacityBytes = activeVertexBuffer.size + activeIndexBuffer.size + activePrimitiveBuffer.size + activeMaterialBuffer.size;
	snapshot.lastFrameUploadBytes =
		mVertexBufferStats.bytesUploadedLastFrame +
		mIndexBufferStats.bytesUploadedLastFrame +
		mPrimitiveBufferStats.bytesUploadedLastFrame +
		mMaterialBufferStats.bytesUploadedLastFrame;
	snapshot.lastFrameGrowEvents =
		mVertexBufferStats.growEventsLastFrame +
		mIndexBufferStats.growEventsLastFrame +
		mPrimitiveBufferStats.growEventsLastFrame +
		mMaterialBufferStats.growEventsLastFrame;
	snapshot.lastFrameOverwriteEvents =
		mVertexBufferStats.overwriteEventsLastFrame +
		mIndexBufferStats.overwriteEventsLastFrame +
		mPrimitiveBufferStats.overwriteEventsLastFrame +
		mMaterialBufferStats.overwriteEventsLastFrame;

	appendBuffer(activeVertexBuffer, mVertexBufferStats);
	appendBuffer(activeIndexBuffer, mIndexBufferStats);
	appendBuffer(activePrimitiveBuffer, mPrimitiveBufferStats);
	appendBuffer(activeMaterialBuffer, mMaterialBufferStats);
	appendBuffer(mPortalBuffer, mPortalBufferStats);
	appendBuffer(mRuntimeLightBuffer, mRuntimeLightBufferStats);
	appendBuffer(mRuntimeLightTileHeaderBuffer, mRuntimeLightTileHeaderBufferStats);
	appendBuffer(mRuntimeLightTileIndexBuffer, mRuntimeLightTileIndexBufferStats);
	appendBuffer(mEmissivePrimitiveHeaderBuffer, mEmissivePrimitiveHeaderBufferStats);
	appendBuffer(mEmissivePrimitiveBuffer, mEmissivePrimitiveBufferStats);
	appendBuffer(mEmissivePrimitiveCdfBuffer, mEmissivePrimitiveCdfBufferStats);
	appendBuffer(mEmissiveMaterialResponseBuffer, mEmissiveMaterialResponseBufferStats);
	appendBuffer(mSectorLightHeaderBuffer, mSectorLightHeaderBufferStats);
	appendBuffer(mSectorLightBuffer, mSectorLightBufferStats);
	PrintNRISceneBufferStatusSnapshot(snapshot);
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
		if ((primitive.flags & nri_scene::PrimitiveFlag_ReflectionOnly) != 0)
		{
			continue;
		}

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

	if (result.hit &&
		materials != nullptr &&
		result.materialIndex < materials->lightMetadata.size() &&
		result.materialIndex < materials->materials.size())
	{
		const auto& metadata = materials->lightMetadata[result.materialIndex];
		const auto& materialData = materials->materials[result.materialIndex];
		result.materialLightingFlags = metadata.lightingFlags;
		result.textureId = metadata.textureId;
		result.baseTextureId = metadata.baseTextureId != 0 ? metadata.baseTextureId : metadata.textureId;
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
			a.baseTextureId == b.baseTextureId &&
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
		const uint32_t preferredChunkListIndex = FindPreferredStaticSceneChunkListIndex(chunkIndex);
		if (preferredChunkListIndex != UINT32_MAX)
		{
			chunkResidentStatic = true;
			const auto& chunkCache = mStaticMapScene.chunks[preferredChunkListIndex];
			chunkStaticTlasInstanced =
				chunkCache.active &&
				chunkCache.accelerationStructure.accelerationStructure != nullptr;
			chunkStaticProbeIncluded = chunkCache.active;
		}
		if (const auto* replacement = mRuntimeMutation.FindReplacement(chunkIndex))
		{
			chunkReplaced = replacement->active;
			chunkSectorDirty = replacement->sectorDirty;
			chunkDragged = replacement->dragged;
			chunkBlindSpot = replacement->blindSpot;
			chunkReasonMask = replacement->reasonMask;
			chunkSectionDirtyCount = replacement->sectionDirtyCount;
			replacementSurfaceCount = replacement->surfaceCount;
			replacementTriangleCount = replacement->triangleCount;
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
	FString materialTextureName;
	int32_t materialLegacyTile = -1;
	ResolveSurfaceProbeTextureDebugInfo(result.baseTextureId, materialTextureName, materialLegacyTile);
	Printf("NRI PT surface probe: hit source=%s drawlist=%s owner=%s data_source=%s chunk=%d gate_visible=%s flat_drawlist_visible=%s static_resident=%s static_tlas_instanced=%s static_probe_included=%s chunk_replaced=%s chunk_reasons=%s section_dirty=%u sector_dirty=%s dragged=%s blind_spot=%s replacement_surfaces=%u replacement_tris=%u local_space=%d portal_graph=%d sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x primitive=%u material=%u texid=%u legacy_tile=%d texture_name=%s material_texid=%u material_legacy_tile=%d material_texture_name=%s distance=%.2f pos=(%.2f, %.2f, %.2f) normal=(%.3f, %.3f, %.3f) flags=0x%x indexed=%s fullbright=%s flat=%s sprite=%s mirror=%s sky=%s portal=%s facing_billboard=%s point_sampled=%s tex_fullbright=%s glowing=%s auto_glow=%s glowmap=%s normalmap=%s metallic=%s roughness=%s normal_tex=%u metallic_tex=%u roughness_tex=%u metalness_hint=%.3f roughness_hint=%.3f material_class=%u emissive_mode=%s emissive_tex=%u light_surface=%s light_mat=%u emissive_surface=%s emissive_prims=%u emissive_hit=%s emissive_flags=0x%x emissive_rule=%u emissive_override=%u emissive_sector=%d sector_scale=%.3f sector_reach=%.3f sector_applied=%s emissive_area=%.2f emissive_power=%.3f emissive_sample_weight=%.3f emissive_pdf=%.6f emissive_intensity=%.3f material_response=%s material_scale=%.3f light=%.3f alpha=%.3f avg=(%.2f, %.2f, %.2f) emissive=(%.2f, %.2f, %.2f) glow=(%.2f, %.2f, %.2f)\n",
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
		result.baseTextureId,
		materialLegacyTile,
		materialTextureName.GetChars(),
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
		YesNo((flags & nri_scene::MaterialFlag_PointSampled) != 0),
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
		YesNo(emissiveDiagnostics.exactEmissivePrimitiveMatch),
		emissiveDiagnostics.emissiveSourceFlags,
		emissiveDiagnostics.emissiveSourceRuleId,
		emissiveDiagnostics.emissiveOverrideRuleId,
		emissiveDiagnostics.emissiveSectorIndex,
		emissiveDiagnostics.sectorResponseScale,
		emissiveDiagnostics.sectorReachScale,
		YesNo(emissiveDiagnostics.sectorResponseApplied),
		emissiveDiagnostics.emissivePrimitiveArea,
		emissiveDiagnostics.emissivePowerEstimate,
		emissiveDiagnostics.emissiveSelectionWeight,
		emissiveDiagnostics.emissiveSelectionPdf,
		emissiveDiagnostics.emissiveIntensity,
		YesNo(emissiveDiagnostics.materialResponseEnabled),
		emissiveDiagnostics.materialResponseScale,
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
		if (record.dataSource == probe.sceneDataSource &&
			record.primitiveIndex == probe.primitiveIndex)
		{
			diagnostics.exactEmissivePrimitiveMatch = true;
			diagnostics.emissiveSourceFlags = record.sourceFlags;
			diagnostics.emissiveSourceRuleId = record.sourceRuleId;
			diagnostics.emissiveOverrideRuleId = record.overrideRuleId;
			diagnostics.emissiveSectorIndex = record.sectorIndex;
			diagnostics.emissivePrimitiveArea = record.primitiveArea;
			diagnostics.emissivePowerEstimate = record.powerEstimate;
			diagnostics.emissiveSelectionWeight = record.selectionWeight;
			diagnostics.emissiveSelectionPdf = record.selectionPdf;
			diagnostics.emissiveIntensity = record.emissiveIntensity;
			diagnostics.sectorResponseScale = record.sectorResponseScale;
			diagnostics.sectorReachScale = record.sectorReachScale;
			diagnostics.sectorResponseApplied = record.sectorResponseApplied;
			diagnostics.materialResponseEnabled = record.materialResponseEnabled;
			diagnostics.materialResponseScale = record.materialResponseScale;
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
				chunkStaticTlasInstanced = true;
				chunkStaticProbeIncluded = true;
				break;
			}
		}
		if (const auto* replacement = mRuntimeMutation.FindReplacement(chunkIndex))
		{
			chunkReplaced = replacement->active;
			chunkSectorDirty = replacement->sectorDirty;
			chunkDragged = replacement->dragged;
			chunkBlindSpot = replacement->blindSpot;
			chunkReasonMask = replacement->reasonMask;
			chunkSectionDirtyCount = replacement->sectionDirtyCount;
			replacementSurfaceCount = replacement->surfaceCount;
			replacementTriangleCount = replacement->triangleCount;
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
	Printf("NRI PT surface probe: source=%s drawlist=%s owner=%s data_source=%s chunk=%d gate_visible=%s flat_drawlist_visible=%s static_resident=%s static_tlas_instanced=%s static_probe_included=%s chunk_replaced=%s chunk_reasons=%s section_dirty=%u sector_dirty=%s dragged=%s blind_spot=%s replacement_surfaces=%u replacement_tris=%u local_space=%d portal_graph=%d sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x primitive=%u material=%u tile=%u material_tile=%u distance=%.2f pos=(%.2f, %.2f, %.2f) flags=0x%x indexed=%s fullbright=%s flat=%s sprite=%s mirror=%s sky=%s portal=%s facing_billboard=%s point_sampled=%s tex_fullbright=%s glowing=%s auto_glow=%s glowmap=%s normalmap=%s metallic=%s roughness=%s normal_tex=%u metallic_tex=%u roughness_tex=%u metalness_hint=%.3f roughness_hint=%.3f material_class=%u emissive_mode=%s emissive_tex=%u light_surface=%s light_mat=%u emissive_surface=%s emissive_prims=%u emissive_hit=%s emissive_flags=0x%x emissive_rule=%u emissive_override=%u emissive_sector=%d sector_scale=%.3f sector_reach=%.3f sector_applied=%s emissive_area=%.2f emissive_power=%.3f emissive_sample_weight=%.3f emissive_pdf=%.6f emissive_intensity=%.3f material_response=%s material_scale=%.3f light=%.3f alpha=%.3f avg=(%.2f, %.2f, %.2f) emissive=(%.2f, %.2f, %.2f) glow=(%.2f, %.2f, %.2f)\n",
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
		mLastSurfaceProbe.baseTextureId,
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
		YesNo((flags & nri_scene::MaterialFlag_PointSampled) != 0),
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
		YesNo(emissiveDiagnostics.exactEmissivePrimitiveMatch),
		emissiveDiagnostics.emissiveSourceFlags,
		emissiveDiagnostics.emissiveSourceRuleId,
		emissiveDiagnostics.emissiveOverrideRuleId,
		emissiveDiagnostics.emissiveSectorIndex,
		emissiveDiagnostics.sectorResponseScale,
		emissiveDiagnostics.sectorReachScale,
		YesNo(emissiveDiagnostics.sectorResponseApplied),
		emissiveDiagnostics.emissivePrimitiveArea,
		emissiveDiagnostics.emissivePowerEstimate,
		emissiveDiagnostics.emissiveSelectionWeight,
		emissiveDiagnostics.emissiveSelectionPdf,
		emissiveDiagnostics.emissiveIntensity,
		YesNo(emissiveDiagnostics.materialResponseEnabled),
		emissiveDiagnostics.materialResponseScale,
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

bool NRIRenderer::BuildEmissiveLightEditTarget(PathTracingEmissiveLightEditTarget& outTarget) const
{
	outTarget = {};
	if (!mLastSurfaceProbe.valid)
	{
		outTarget.failureReason = "no sampled center hit has been recorded yet";
		return false;
	}

	if (!mLastSurfaceProbe.hit)
	{
		outTarget.valid = true;
		outTarget.failureReason = "last sampled center ray missed translated PT geometry";
		return false;
	}

	const SurfaceProbeEmissiveDiagnostics emissiveDiagnostics = BuildSurfaceProbeEmissiveDiagnostics(mLastSurfaceProbe);
	outTarget.valid = true;
	outTarget.hit = true;
	outTarget.emissive = emissiveDiagnostics.activeEmissiveSurfaceMatch;
	outTarget.sectorIndex = mLastSurfaceProbe.provenance.sectorIndex;
	outTarget.wallIndex = mLastSurfaceProbe.provenance.wallIndex;
	outTarget.textureId = (int)mLastSurfaceProbe.textureId;
	outTarget.baseTextureId = (int)mLastSurfaceProbe.baseTextureId;
	outTarget.materialIndex = (int)mLastSurfaceProbe.materialIndex;
	outTarget.surfaceLightOverlay = mLastSurfaceProbe.provenance.sourceType == nri_scene::SurfaceSourceType::SurfaceLightOverlay;
	outTarget.surfaceLightRuleId = outTarget.surfaceLightOverlay ? mLastSurfaceProbe.provenance.cstat : 0u;
	outTarget.position[0] = mLastSurfaceProbe.position[0];
	outTarget.position[1] = mLastSurfaceProbe.position[1];
	outTarget.position[2] = mLastSurfaceProbe.position[2];
	outTarget.normal[0] = mLastSurfaceProbe.normal[0];
	outTarget.normal[1] = mLastSurfaceProbe.normal[1];
	outTarget.normal[2] = mLastSurfaceProbe.normal[2];
	int32_t legacyTile = -1;
	ResolveSurfaceProbeTextureDebugInfo(mLastSurfaceProbe.textureId, outTarget.textureName, legacyTile);
	ResolveSurfaceProbeTextureDebugInfo(mLastSurfaceProbe.baseTextureId, outTarget.materialTextureName, legacyTile);
	outTarget.sectorResponseIntensity = std::max(0.0f, (float)nri_ptsectoremissionsignalstrength);
	outTarget.sectorResponseMin = std::max(0.0f, (float)nri_ptsectoremissionresponsemin);
	outTarget.sectorResponseMax = std::max(outTarget.sectorResponseMin, (float)nri_ptsectoremissionresponsemax);
	if (!outTarget.emissive)
	{
		outTarget.failureReason = emissiveDiagnostics.sceneLightSurfaceMatch ?
			"aimed surface is not currently an active emissive surface" :
			"aimed surface is not present in the scene-light surface registry";
		return false;
	}

	return true;
}

bool NRIRenderer::BuildSurfaceLightEditTarget(PathTracingEmissiveLightEditTarget& outTarget) const
{
	outTarget = {};
	if (!mLastSurfaceProbe.valid)
	{
		outTarget.failureReason = "no sampled center hit has been recorded yet";
		return false;
	}

	if (!mLastSurfaceProbe.hit)
	{
		outTarget.valid = true;
		outTarget.failureReason = "last sampled center ray missed translated PT geometry";
		return false;
	}

	const SurfaceProbeEmissiveDiagnostics emissiveDiagnostics = BuildSurfaceProbeEmissiveDiagnostics(mLastSurfaceProbe);
	outTarget.valid = true;
	outTarget.hit = true;
	outTarget.emissive = emissiveDiagnostics.activeEmissiveSurfaceMatch;
	outTarget.sectorIndex = mLastSurfaceProbe.provenance.sectorIndex;
	outTarget.wallIndex = mLastSurfaceProbe.provenance.wallIndex;
	outTarget.textureId = (int)mLastSurfaceProbe.textureId;
	outTarget.baseTextureId = (int)mLastSurfaceProbe.baseTextureId;
	outTarget.materialIndex = (int)mLastSurfaceProbe.materialIndex;
	outTarget.position[0] = mLastSurfaceProbe.position[0];
	outTarget.position[1] = mLastSurfaceProbe.position[1];
	outTarget.position[2] = mLastSurfaceProbe.position[2];
	outTarget.normal[0] = mLastSurfaceProbe.normal[0];
	outTarget.normal[1] = mLastSurfaceProbe.normal[1];
	outTarget.normal[2] = mLastSurfaceProbe.normal[2];
	float viewDirection[3] = { mCurrentCameraForward[0], mCurrentCameraForward[1], mCurrentCameraForward[2] };
	Normalize3(viewDirection);
	if (outTarget.normal[0] * viewDirection[0] + outTarget.normal[1] * viewDirection[1] + outTarget.normal[2] * viewDirection[2] > 0.0f)
	{
		outTarget.normal[0] = -outTarget.normal[0];
		outTarget.normal[1] = -outTarget.normal[1];
		outTarget.normal[2] = -outTarget.normal[2];
	}
	int32_t legacyTile = -1;
	ResolveSurfaceProbeTextureDebugInfo(mLastSurfaceProbe.textureId, outTarget.textureName, legacyTile);
	ResolveSurfaceProbeTextureDebugInfo(mLastSurfaceProbe.baseTextureId, outTarget.materialTextureName, legacyTile);
	outTarget.sectorResponseIntensity = std::max(0.0f, (float)nri_ptsectoremissionsignalstrength);
	outTarget.sectorResponseMin = std::max(0.0f, (float)nri_ptsectoremissionresponsemin);
	outTarget.sectorResponseMax = std::max(outTarget.sectorResponseMin, (float)nri_ptsectoremissionresponsemax);
	return true;
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
	const uint32_t preferredChunkListIndex = FindPreferredStaticSceneChunkListIndex((uint32_t)chunkIndex);
	const uint32_t duplicateChunkSlotCount = CountStaticSceneChunkSlots((uint32_t)chunkIndex);
	const StaticMapSceneCache::ChunkCache* staticChunk =
		preferredChunkListIndex < mStaticMapScene.chunks.size() ?
		&mStaticMapScene.chunks[preferredChunkListIndex] :
		nullptr;
	const bool residentStatic = staticChunk != nullptr;
	const bool staticTlasInstanced =
		staticChunk != nullptr &&
		staticChunk->active &&
		staticChunk->accelerationStructure.accelerationStructure != nullptr;
	const bool staticProbeIncluded = staticChunk != nullptr && staticChunk->active;
	const auto* replacement = mRuntimeMutation.FindReplacement((uint32_t)chunkIndex);

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

	if (duplicateChunkSlotCount > 1)
	{
		Printf("NRI PT chunk dump slots: chunk=%d duplicate_slots=%u preferred_slot=%u\n",
			chunkIndex,
			duplicateChunkSlotCount,
			preferredChunkListIndex);
	}

	if (staticChunk != nullptr)
	{
		Printf("NRI PT chunk dump static: primitive_offset=%u primitive_count=%u material_offset=%u material_count=%u as_ready=%s\n",
			staticChunk->primitiveOffset,
			staticChunk->primitiveCount,
			staticChunk->materialOffset,
			staticChunk->materialCount,
			YesNo(staticChunk->accelerationStructure.accelerationStructure != nullptr));
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
		Printf("NRI PT chunk surface %u: kind=%s source=%s section=%d sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x flags=0x%x flat=%s sprite=%s mirror=%s sky=%s portal=%s one_way=%s facing_billboard=%s point_sampled=%s tile=%u pal=%d shade=%d alpha=%.3f verts=%u tris=%u\n",
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
			YesNo((flags & nri_scene::MaterialFlag_PointSampled) != 0),
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
	const auto* replacement = mRuntimeMutation.FindReplacement((uint32_t)chunkIndex);

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
				mRuntimeMutation.IsReplacementActive(adjacentChunkIt->second);
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
			mRuntimeMutation.IsReplacementActive((uint32_t)adjacentChunkIndex);
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
	const nri_scene::MaterialBridgeData* dynamicMaterials,
	bool appendPersistentVoxelSceneLights)
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
				if (!staticChunk.active)
				{
					continue;
				}
				const uint32_t mapChunkIndex = staticChunk.chunkIndex;
				const bool useRuntimeMutationReplacement = mRuntimeMutation.IsReplacementActiveAndValid(mapChunkIndex);
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

		{
			ScopedPtPerfTimer appendTimer(mLastPerfShellTraceStats.sceneLightRuntimeMutationAppendMs);
			mRuntimeMutation.AppendSceneLightRecords(mSceneLights);
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

	if (appendPersistentVoxelSceneLights)
	{
		ScopedPtPerfTimer appendTimer(mLastPerfShellTraceStats.sceneLightPersistentVoxelAppendMs);
		mPersistentVoxels.AppendSceneLights(mSceneLights, mFrameIndex, (bool)nri_voxelstats);
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
	std::vector<SceneLightSystem::EmissiveOverrideRule> emissiveOverrideRules;
	std::vector<SceneLightSystem::EmissiveMaterialResponseRule> emissiveMaterialResponseRules;
	BuildActorAnalyticOverlayRules(resolvedLightOverlays, actorOverlayRules);
	BuildEmissiveOverrideRules(resolvedLightOverlays, emissiveOverrideRules);
	BuildEmissiveMaterialResponseRules(resolvedLightOverlays, emissiveMaterialResponseRules);
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
		mSceneLights.RebuildEmissiveSurfaces(
			NRI_MAX_EMISSIVE_SURFACES,
			emissiveOverrideRules.empty() ? nullptr : &emissiveOverrideRules,
			emissiveMaterialResponseRules.empty() ? nullptr : &emissiveMaterialResponseRules);
	}
	{
		ScopedPtPerfTimer rebuildTimer(mLastPerfShellTraceStats.sceneLightSectorMs);
		mSceneLights.RebuildSectorLighting(gameplayLightTimeIndex, (uint32_t)sector.Size());
	}
	TraceEmissiveSectorResponseChange();
	const auto& frameAppendStats = mSceneLights.GetFrameAppendStats();
	mLastPerfShellTraceStats.sceneLightSurfaceRecordCount = frameAppendStats.totalRecordCount;
	mLastPerfShellTraceStats.sceneLightStaticRecordCount = frameAppendStats.staticRecordCount;
	mLastPerfShellTraceStats.sceneLightRuntimeMutationRecordCount = frameAppendStats.runtimeMutationRecordCount;
	mLastPerfShellTraceStats.sceneLightDynamicRecordCount = frameAppendStats.dynamicRecordCount;
	mLastPerfShellTraceStats.sceneLightCapturedRecordCount = frameAppendStats.capturedRecordCount;
	mLastPerfShellTraceStats.sceneLightPersistentVoxelRecordCount = frameAppendStats.persistentVoxelRecordCount;
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

	if (analyticLightTopologyChanged || analyticLightPropertiesChanged)
	{
		InvalidateRuntimeLightSceneData();
	}
	if (analyticLightTopologyChanged)
	{
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
		nri_scene::MaterialData& material = inOutGpuMaterials[materialIndex];
		mSceneLights.ApplyEmissiveMaterialSettings(materials.lightMetadata[materialIndex], material);
		if (material.emissiveMode == nri_scene::MaterialEmissiveMode_UseGlowmapTexture)
		{
			material.emissiveReserved = GetGlowmapVisibleBlendScale();
		}
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

uint64_t NRIRenderer::ComputeChunkActorOverrideHash(const nri_scene::MaterialBridgeData& materials)
{
	const auto& actorOverrides = GetActorMaterialOverrideMapForFrame();
	if (actorOverrides.empty() || materials.lightMetadata.empty())
	{
		return 0;
	}

	uint64_t hash = 1469598103934665603ull;
	bool touched = false;
	for (const auto& metadata : materials.lightMetadata)
	{
		if (metadata.actorIndex < 0)
		{
			continue;
		}

		auto it = actorOverrides.find(metadata.actorIndex);
		if (it == actorOverrides.end() || it->second == ActorMaterialOverride_None)
		{
			continue;
		}

		touched = true;
		hash = CoherencyHashCombine64(hash, (uint64_t)(uint32_t)metadata.actorIndex);
		hash = CoherencyHashCombine64(hash, (uint64_t)it->second);
	}

	return touched ? hash : 0;
}

uint64_t NRIRenderer::ComputeChunkEmissiveOverrideHash(const nri_scene::MaterialBridgeData& materials) const
{
	const uint32_t count = std::min<uint32_t>((uint32_t)materials.materials.size(), (uint32_t)materials.lightMetadata.size());
	if (count == 0)
	{
		return 0;
	}

	uint64_t hash = 1469598103934665603ull;
	bool touched = false;
	for (uint32_t materialIndex = 0; materialIndex < count; ++materialIndex)
	{
		nri_scene::MaterialData effectiveMaterial = materials.materials[materialIndex];
		const bool emissiveApplied = mSceneLights.ApplyEmissiveMaterialSettings(materials.lightMetadata[materialIndex], effectiveMaterial);
		if (!emissiveApplied)
		{
			continue;
		}

		touched = true;
		hash = CoherencyHashCombine64(hash, (uint64_t)materialIndex);
		hash = CoherencyHashCombine64(hash, (uint64_t)effectiveMaterial.materialClass);
		hash = CoherencyHashCombine64(hash, (uint64_t)effectiveMaterial.emissiveMode);
		hash = CoherencyHashCombine64(hash, (uint64_t)effectiveMaterial.emissiveTextureIndex);
		hash = CoherencyHashCombine64(hash, (uint64_t)CoherencyFloatBits(effectiveMaterial.emissiveColor[0]));
		hash = CoherencyHashCombine64(hash, (uint64_t)CoherencyFloatBits(effectiveMaterial.emissiveColor[1]));
		hash = CoherencyHashCombine64(hash, (uint64_t)CoherencyFloatBits(effectiveMaterial.emissiveColor[2]));
		hash = CoherencyHashCombine64(hash, (uint64_t)CoherencyFloatBits(effectiveMaterial.emissiveIntensity));
		hash = CoherencyHashCombine64(hash, (uint64_t)CoherencyFloatBits(effectiveMaterial.emissiveMaskScale));
		hash = CoherencyHashCombine64(hash, (uint64_t)CoherencyFloatBits(effectiveMaterial.emissiveReserved));
	}

	return touched ? hash : 0;
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
		DestroyStaticMapSceneCache("material-lighting-refresh-failed");
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

	const size_t requiredRootConstantSize = std::max({ sizeof(NRITraceSceneConstants), sizeof(NRITemporalConstants), sizeof(NRIPresentConstants), sizeof(NRIExposureConstants) });
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
	const size_t requiredRootConstantSize = std::max({ sizeof(NRITraceSceneConstants), sizeof(NRITemporalConstants), sizeof(NRIPresentConstants), sizeof(NRIExposureConstants) });
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

void NRIRenderer::RefreshMapWorld()
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.mapWorldMs);
	const uint64_t pendingBuildSerial = nri_scene::GetPendingLevelGeometryBuildSerial();
	const bool levelChanged = mMapWorld.level != currentLevel;
	if (levelChanged)
	{
		RequestHistoryReset("map-load", true, true);
		mSceneTextures.CacheStats() = {};
		mPersistentDynamicEmissiveHighWaterStats = {};
		mPersistentDynamicEmissiveHighWaterSurfaceCount = 0;
		mPersistentDynamicEmissiveHighWaterPrimitiveCount = 0;
		mPersistentDynamicEmissiveHighWaterMaterialCount = 0;
		mRuntimeMutation.ResetHighWaterStats();
	}
	const bool needsBuild = !mMapWorld.valid || levelChanged || pendingBuildSerial != mObservedMapWorldBuildSerial;
	if (!needsBuild)
	{
		if (mAllowStartupMapWorldCorrection && mFrameIndex > mStartupMapWorldCorrectionDeadlineFrame)
		{
			mAllowStartupMapWorldCorrection = false;
		}
		return;
	}

	ResetPersistentDynamicEmissiveCache();
	mAllowStartupMapWorldCorrection = true;
	mStartupMapWorldCorrectionDeadlineFrame = mFrameIndex + 8u;
	mAllowStartupMutationRebaseline = false;
	mPendingStartupMutationRebaseline = false;
	mStartupMutationRebaselineDeadlineFrame = 0;

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
		mPendingStartupVisibleChunkValidation.clear();
		mRuntimeMutation.ResetWorklist();
		mAllowStartupMapWorldCorrection = false;
		mStartupMapWorldCorrectionDeadlineFrame = 0;
		mAllowStartupMutationRebaseline = false;
		mPendingStartupMutationRebaseline = false;
		mStartupMutationRebaselineDeadlineFrame = 0;
		return;
	}

	mMapWorld = std::move(world);
	mObservedMapWorldBuildSerial = pendingBuildSerial;
	mPendingStartupVisibleChunkValidation.clear();
	mPendingStartupVisibleChunkValidation.resize(mMapWorld.chunks.size(), 0u);
	mRuntimeMutation.PrepareSignatureWatchlist(mMapWorld.buildSerial, (uint32_t)mMapWorld.chunks.size());
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
	mPendingStartupMutationRebaseline = false;
	mAllowStartupMutationRebaseline = false;
}

bool NRIRenderer::ApplyStartupMapWorldCorrectionIfNeeded(const char* trigger)
{
	if (!mAllowStartupMapWorldCorrection)
	{
		return true;
	}

	if (mFrameIndex > mStartupMapWorldCorrectionDeadlineFrame)
	{
		mAllowStartupMapWorldCorrection = false;
		return true;
	}

	if (!mMapWorld.valid ||
		!mStaticMapScene.valid ||
		!mRuntimeMutation.HasCacheChunkCount((uint32_t)mMapWorld.chunks.size()))
	{
		return true;
	}

	nri_scene::PTMapWorld correctedWorld = {};
	if (!nri_scene::BuildMapWorld(correctedWorld))
	{
		Printf(TEXTCOLOR_RED "NRI PT startup world correction: authoritative rebuild failed for %s trigger=%s frame=%u.\n",
			currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)",
			trigger != nullptr ? trigger : "unknown",
			mFrameIndex);
		mAllowStartupMapWorldCorrection = false;
		mStartupMapWorldCorrectionDeadlineFrame = 0;
		return true;
	}

	uint32_t chunkDiffCount = 0;
	uint32_t surfaceDiffCount = 0;
	StartupMapWorldDiffDetails diffDetails = {};
	if (!StartupMapWorldStructureDiffers(mMapWorld, correctedWorld, chunkDiffCount, surfaceDiffCount, &diffDetails))
	{
		return true;
	}

	DestroyStaticMapSceneCache("startup-world-correction");
	mStaticMapScene = {};
	mStaticAccelerationBuildSerial = 0;
	mSkyEnvironment.PreservedStaticMapSky() = {};
	mMapWorld = std::move(correctedWorld);
	mObservedMapWorldBuildSerial = nri_scene::GetPendingLevelGeometryBuildSerial();
	mAllowStartupMapWorldCorrection = false;
	mStartupMapWorldCorrectionDeadlineFrame = 0;
	if (mPendingStartupVisibleChunkValidation.size() < mMapWorld.chunks.size())
	{
		mPendingStartupVisibleChunkValidation.resize(mMapWorld.chunks.size(), 0u);
	}
	mRuntimeMutation.PrepareSignatureWatchlist(mMapWorld.buildSerial, (uint32_t)mMapWorld.chunks.size());
	for (uint32_t chunkIndex : diffDetails.lateVisibleValidationChunks)
	{
		if (chunkIndex < mPendingStartupVisibleChunkValidation.size())
		{
			mPendingStartupVisibleChunkValidation[chunkIndex] = 1u;
		}
	}
	RequestHistoryReset("startup-world-correction");

	Printf("NRI PT startup world correction: trigger=%s level=%s frame=%u build_serial=%llu chunk_diffs=%u surface_diffs=%u chunks=%u surfaces=%u tris=%u\n",
		trigger != nullptr ? trigger : "unknown",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		mFrameIndex,
		(unsigned long long)mMapWorld.buildSerial,
		chunkDiffCount,
		surfaceDiffCount,
		mMapWorld.stats.chunkCount,
		mMapWorld.stats.surfaceCount,
		mMapWorld.stats.triangleCount);

	if (nri_ptscenestats)
	{
		const auto& stats = mMapWorld.stats;
		Printf("NRI PT startup world correction: trigger=%s level=%s frame=%u build_serial=%llu chunk_diffs=%u surface_diffs=%u chunks=%u surfaces=%u walls=%u flats=%u portals=%u skies=%u tris=%u\n",
			trigger != nullptr ? trigger : "unknown",
			mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
			mFrameIndex,
			(unsigned long long)mMapWorld.buildSerial,
			chunkDiffCount,
			surfaceDiffCount,
			stats.chunkCount,
			stats.surfaceCount,
			stats.wallSurfaceCount,
			stats.flatSurfaceCount,
			stats.portalSurfaceCount,
			stats.skySurfaceCount,
			stats.triangleCount);
		const std::string reasonSummary = BuildStartupMapWorldDiffReasonSummary(diffDetails);
		Printf("NRI PT startup world correction detail: trigger=%s frame=%u reasons=%s sampled_chunks=%u/%u\n",
			trigger != nullptr ? trigger : "unknown",
			mFrameIndex,
			reasonSummary.c_str(),
			(uint32_t)diffDetails.chunkSamples.size(),
			chunkDiffCount);
		if (!diffDetails.lateVisibleValidationChunks.empty())
		{
			Printf("NRI PT startup world correction late-visible: trigger=%s frame=%u chunks=%u\n",
				trigger != nullptr ? trigger : "unknown",
				mFrameIndex,
				(uint32_t)diffDetails.lateVisibleValidationChunks.size());
		}
		for (size_t sampleIndex = 0; sampleIndex < diffDetails.chunkSamples.size(); ++sampleIndex)
		{
			const auto& sample = diffDetails.chunkSamples[sampleIndex];
			Printf("NRI PT startup world correction chunk: trigger=%s frame=%u sample=%u/%u chunk=%u sector=%d->%d surfaces=%u->%u tris=%u->%u surface_diffs=%u\n",
				trigger != nullptr ? trigger : "unknown",
				mFrameIndex,
				(uint32_t)(sampleIndex + 1u),
				(uint32_t)diffDetails.chunkSamples.size(),
				sample.chunkIndex,
				sample.currentSectorIndex,
				sample.rebuiltSectorIndex,
				sample.currentSurfaceCount,
				sample.rebuiltSurfaceCount,
				sample.currentTriangleCount,
				sample.rebuiltTriangleCount,
				sample.surfaceDiffCount);
		}
	}
	return true;
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

	nri::DescriptorRangeDesc traceStatsRange = {};
	traceStatsRange.baseRegisterIndex = NRI_OUTPUT_DESCRIPTOR_NUM;
	traceStatsRange.descriptorNum = NRI_TRACE_SHADER_STATS_DESCRIPTOR_NUM;
	traceStatsRange.descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
	traceStatsRange.shaderStages = NRIComputeStage();
	traceStatsRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc outputRanges[2] = { outputRange, traceStatsRange };

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
	descriptorSets[4].ranges = outputRanges;
	descriptorSets[4].rangeNum = (uint32_t)std::size(outputRanges);
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
	inputRange.descriptorNum = 4;
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

bool NRIRenderer::CreateExposurePipelineLayout()
{
	nri::DescriptorRangeDesc inputRange = {};
	inputRange.baseRegisterIndex = 0;
	inputRange.descriptorNum = NRI_EXPOSURE_INPUT_DESCRIPTOR_NUM;
	inputRange.descriptorType = nri::DescriptorType::TEXTURE;
	inputRange.shaderStages = NRIComputeStage();
	inputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc outputTextureRange = {};
	outputTextureRange.baseRegisterIndex = NRI_EXPOSURE_OUTPUT_TEXTURE_BASE_REGISTER;
	outputTextureRange.descriptorNum = NRI_EXPOSURE_OUTPUT_TEXTURE_DESCRIPTOR_NUM;
	outputTextureRange.descriptorType = nri::DescriptorType::STORAGE_TEXTURE;
	outputTextureRange.shaderStages = NRIComputeStage();
	outputTextureRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc outputBufferRange = {};
	outputBufferRange.baseRegisterIndex = NRI_EXPOSURE_OUTPUT_BUFFER_BASE_REGISTER;
	outputBufferRange.descriptorNum = NRI_EXPOSURE_OUTPUT_BUFFER_DESCRIPTOR_NUM;
	outputBufferRange.descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
	outputBufferRange.shaderStages = NRIComputeStage();
	outputBufferRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc outputRanges[2] = { outputTextureRange, outputBufferRange };

	nri::DescriptorSetDesc descriptorSets[2] = {};
	descriptorSets[0].registerSpace = NRI_EXPOSURE_SET_INPUTS;
	descriptorSets[0].ranges = &inputRange;
	descriptorSets[0].rangeNum = 1;
	descriptorSets[0].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[1].registerSpace = NRI_EXPOSURE_SET_OUTPUTS;
	descriptorSets[1].ranges = outputRanges;
	descriptorSets[1].rangeNum = (uint32_t)std::size(outputRanges);
	descriptorSets[1].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;

	nri::RootConstantDesc rootConstant = {};
	rootConstant.registerIndex = NRI_EXPOSURE_ROOT_REGISTER;
	rootConstant.size = sizeof(NRIExposureConstants);
	rootConstant.shaderStages = NRIComputeStage();

	nri::PipelineLayoutDesc desc = {};
	desc.rootRegisterSpace = NRI_EXPOSURE_SET_ROOT;
	desc.rootConstants = &rootConstant;
	desc.rootConstantNum = 1;
	desc.descriptorSets = descriptorSets;
	desc.descriptorSetNum = (uint32_t)std::size(descriptorSets);
	desc.shaderStages = NRIComputeStage();

	return mFrameBuffer->mCore.CreatePipelineLayout(*mFrameBuffer->mDevice, desc, mExposurePipelineLayout) == nri::Result::SUCCESS;
}

bool NRIRenderer::CreatePipelines()
{
	auto createPipeline = [this](const char* fileName, PipelineSlot slot, nri::PipelineLayout* layout)
	{
		std::vector<uint8_t> shaderBlob;
		if (!mFrameBuffer->LoadShaderBlob(fileName, shaderBlob))
		{
			Printf("NRI PT pipeline create failed: shader=%s reason=load\n", fileName);
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
		const nri::Result result = mFrameBuffer->mCore.CreateComputePipeline(*mFrameBuffer->mDevice, pipelineDesc, mPipelines[(size_t)slot]);
		if (result != nri::Result::SUCCESS)
		{
			Printf("NRI PT pipeline create failed: shader=%s slot=%u result=%d\n", fileName, (unsigned)slot, (int)result);
			return false;
		}
		return true;
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
	FString exposureHistogramClear = FStringf("ExposureHistogramClear.cs.%s", suffix);
	FString exposureHistogramBuild = FStringf("ExposureHistogramBuild.cs.%s", suffix);
	FString exposureResolve = FStringf("ExposureResolve.cs.%s", suffix);

	return
		createPipeline(trace.GetChars(), PipelineSlot::TraceOpaque, mPipelineLayout) &&
		createPipeline(composition.GetChars(), PipelineSlot::Composition, mPipelineLayout) &&
		createPipeline(traceTransparent.GetChars(), PipelineSlot::TraceTransparent, mPipelineLayout) &&
		createPipeline(exposureHistogramClear.GetChars(), PipelineSlot::ExposureHistogramClear, mExposurePipelineLayout) &&
		createPipeline(exposureHistogramBuild.GetChars(), PipelineSlot::ExposureHistogramBuild, mExposurePipelineLayout) &&
		createPipeline(exposureResolve.GetChars(), PipelineSlot::ExposureResolve, mExposurePipelineLayout) &&
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
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPresentPipelineLayout, 1, &mFinalPresentOutputSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mExposurePipelineLayout, 0, &mExposureInputSets[0], 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mExposurePipelineLayout, 1, &mExposureOutputSets[0], 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mExposurePipelineLayout, 0, &mExposureInputSets[1], 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mExposurePipelineLayout, 1, &mExposureOutputSets[1], 1, 0) == nri::Result::SUCCESS;
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
		gpuLight.flags = light.flags;
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
		hash = HashCombine64(hash, (uint64_t)light.flags);
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
	std::vector<EmissiveMaterialResponseGpuData>& outMaterialResponses,
	std::vector<EmissivePrimitiveDebugRecord>& outDebugRecords) const
{
	outHeader = {};
	outHeader.dominantIndex = UINT32_MAX;
	outHeader.flags = 0u;
	outPrimitives.clear();
	outCdf.clear();
	outMaterialResponses.clear();
	outDebugRecords.clear();
	EmissiveMaterialResponseGpuData materialResponseHeader = {};
	materialResponseHeader.primitiveIndex = UINT32_MAX;
	materialResponseHeader.materialScale = 1.0f;
	outMaterialResponses.push_back(materialResponseHeader);

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
	std::unordered_map<uint64_t, uint32_t> materialResponseLookup;
	const auto& activeSurfaces = mSceneLights.GetEmissiveSurfaces().activeSurfaces;
	const auto& sectorRegistry = mSceneLights.GetSectorLighting();
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
		const float samplingScale = ResolveGlowSamplingScale(surface.sourceFlags, surface.emissiveMode) * std::max(surface.reachScale, 0.0f);
		bool sectorResponseApplied = false;
		const float sectorRawResponseScale = ResolveSectorEmissionScale(sectorRegistry, surface, sectorResponseApplied);
		const float sectorResponseScale = sectorResponseApplied ? ClampSectorEmissionIntensityScale(surface, sectorRawResponseScale) : 1.0f;
		const float sectorReachScale = sectorResponseApplied ? ClampSectorEmissionReachScale(surface, sectorRawResponseScale) : 1.0f;
		bool materialResponseApplied = false;
		const float materialResponseScale = ResolveEmissiveMaterialResponseScale(sectorRegistry, surface, materialResponseApplied);
		const bool materialResponseEligible = IsEmissiveSurfaceMaterialResponseEligible(surface);

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
			const float basePowerEstimate = std::max(primitiveArea * representativeLuminance * surface.emissiveIntensity, 0.0f);
			candidate.gpu.powerEstimate = basePowerEstimate * sectorResponseScale;
			candidate.gpu.selectionWeight = basePowerEstimate * samplingScale * sectorReachScale;
			candidate.gpu.emissionScale = sectorResponseScale;

			candidate.debug.stableKey = HashCombine64(surface.stableKey, ((uint64_t)dataSource << 32u) | primitiveIndex);
			candidate.debug.surfaceStableKey = surface.stableKey;
			candidate.debug.dataSource = dataSource;
			candidate.debug.primitiveIndex = primitiveIndex;
			candidate.debug.materialIndex = surface.materialIndex;
			candidate.debug.sourceFlags = surface.sourceFlags;
			candidate.debug.sourceRuleId = surface.sourceRuleId;
			candidate.debug.overrideRuleId = surface.overrideRuleId;
			candidate.debug.textureId = surface.textureId;
			candidate.debug.emissiveMode = surface.emissiveMode;
			candidate.debug.emissiveTextureIndex = surface.emissiveTextureIndex;
			candidate.debug.actorIndex = surface.actorIndex;
			candidate.debug.sectorIndex = surface.sectorIndex;
			candidate.debug.primitiveArea = primitiveArea;
			candidate.debug.powerEstimate = candidate.gpu.powerEstimate;
			candidate.debug.selectionWeight = candidate.gpu.selectionWeight;
			candidate.debug.selectionPdf = 0.0f;
			candidate.debug.emissiveIntensity = surface.emissiveIntensity * sectorResponseScale;
			candidate.debug.sectorResponseScale = sectorResponseScale;
			candidate.debug.sectorReachScale = sectorReachScale;
			candidate.debug.materialResponseEnabled = materialResponseEligible;
			candidate.debug.materialResponseScale = materialResponseScale;
			candidate.debug.sectorResponseApplied = sectorResponseApplied;
			Copy3(surface.emissiveColor, candidate.debug.emissiveColor);
			ComputePrimitiveCenter(*geometry, localPrimitiveIndex, candidate.debug.center);

			candidate.gpu.stableKeyLo = (uint32_t)(candidate.debug.stableKey & 0xffffffffu);
			candidate.gpu.stableKeyHi = (uint32_t)(candidate.debug.stableKey >> 32u);
			candidates.push_back(candidate);

			if (materialResponseEligible)
			{
				const uint64_t responseKey = ((uint64_t)dataSource << 32u) | primitiveIndex;
				if (materialResponseLookup.find(responseKey) == materialResponseLookup.end())
				{
					materialResponseLookup.emplace(responseKey, (uint32_t)outMaterialResponses.size());
					EmissiveMaterialResponseGpuData response = {};
					response.dataSource = dataSource;
					response.primitiveIndex = primitiveIndex;
					response.materialScale = std::max(0.0f, materialResponseScale);
					outMaterialResponses.push_back(response);
				}
			}
		}
	};

	for (const auto& surface : activeSurfaces)
	{
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
		case SceneLightRecordSource::PersistentVoxelScene:
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
	outMaterialResponses[0].dataSource = (uint32_t)outMaterialResponses.size() - 1u;

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
	hash = HashCombine64(hash, HashGeometryForEmissiveSampling(context.staticGeometry));
	hash = HashCombine64(hash, HashGeometryForEmissiveSampling(context.capturedGeometry));
	hash = HashCombine64(hash, HashGeometryForEmissiveSampling(context.runtimeMutationGeometry));
	hash = HashCombine64(hash, (uint64_t)context.runtimeMutationPrimitiveBaseOffset);
	hash = HashCombine64(hash, HashGeometryForEmissiveSampling(context.dynamicGeometry));
	hash = HashCombine64(hash, (uint64_t)context.dynamicPrimitiveBaseOffset);

	const auto& emissiveRegistry = mSceneLights.GetEmissiveSurfaces();
	const auto& sectorRegistry = mSceneLights.GetSectorLighting();
	hash = HashCombine64(hash, (uint64_t)emissiveRegistry.activeSurfaces.size());
	for (const auto& surface : emissiveRegistry.activeSurfaces)
	{
		hash = HashCombine64(hash, surface.stableKey);

		const auto propertyIt = emissiveRegistry.activePropertyHashes.find(surface.stableKey);
		hash = HashCombine64(hash, propertyIt != emissiveRegistry.activePropertyHashes.end() ? propertyIt->second : 0ull);

		const auto bindingIt = emissiveRegistry.activeBindingHashes.find(surface.stableKey);
		hash = HashCombine64(hash, bindingIt != emissiveRegistry.activeBindingHashes.end() ? bindingIt->second : 0ull);

		const bool sectorResponseEligible = IsEmissiveSurfaceSectorResponseEligible(surface);
		if (sectorResponseEligible)
		{
			const uint32_t sectorIndex = (uint32_t)surface.sectorIndex;
			bool applied = false;
			const float responseScale = ResolveSectorEmissionScale(sectorRegistry, surface, applied);
			const float intensityScale = applied ? ClampSectorEmissionIntensityScale(surface, responseScale) : 1.0f;
			const float reachScale = applied ? ClampSectorEmissionReachScale(surface, responseScale) : 1.0f;
			hash = HashCombine64(hash, (uint64_t)sectorIndex);
			hash = HashCombine64(hash, (uint64_t)FloatBits(responseScale));
			hash = HashCombine64(hash, (uint64_t)FloatBits(intensityScale));
			hash = HashCombine64(hash, (uint64_t)FloatBits(reachScale));
		}
		if (IsEmissiveSurfaceMaterialResponseEligible(surface))
		{
			bool applied = false;
			const float materialScale = ResolveEmissiveMaterialResponseScale(sectorRegistry, surface, applied);
			hash = HashCombine64(hash, 0x4d415452455350ull);
			hash = HashCombine64(hash, (uint64_t)(uint32_t)surface.sectorIndex);
			hash = HashCombine64(hash, (uint64_t)FloatBits(materialScale));
		}
	}

	return hash;
}

uint64_t NRIRenderer::BuildEmissiveSectorResponsePayloadHash() const
{
	uint64_t hash = 1469598103934665603ull;
	const auto& emissiveRegistry = mSceneLights.GetEmissiveSurfaces();
	const auto& sectorRegistry = mSceneLights.GetSectorLighting();
	for (const auto& surface : emissiveRegistry.activeSurfaces)
	{
		const bool sectorResponseEligible = IsEmissiveSurfaceSectorResponseEligible(surface);
		if (!sectorResponseEligible)
		{
			continue;
		}

		const uint32_t sectorIndex = (uint32_t)surface.sectorIndex;
		bool applied = false;
		const float responseScale = ResolveSectorEmissionScale(sectorRegistry, surface, applied);
		const float intensityScale = applied ? ClampSectorEmissionIntensityScale(surface, responseScale) : 1.0f;
		const float reachScale = applied ? ClampSectorEmissionReachScale(surface, responseScale) : 1.0f;
		hash = HashCombine64(hash, surface.stableKey);
		hash = HashCombine64(hash, (uint64_t)sectorIndex);
		hash = HashCombine64(hash, (uint64_t)FloatBits(responseScale));
		hash = HashCombine64(hash, (uint64_t)FloatBits(intensityScale));
		hash = HashCombine64(hash, (uint64_t)FloatBits(reachScale));
	}

	return hash;
}

void NRIRenderer::NotifyEmissiveSectorResponseEditModeChanges()
{
	if (!nri_ptemissivelighteditmode)
	{
		mEmissiveSectorResponseNotifyCacheValid = false;
		mSectorLightingEditNotifyCacheValid = false;
		return;
	}

	const auto& emissiveRegistry = mSceneLights.GetEmissiveSurfaces();
	const auto& sectorRegistry = mSceneLights.GetSectorLighting();
	const float nearbyRadius = std::max(0.0f, (float)nri_ptemissivelighteditnotifyrange);
	const float nearbyRadiusSq = nearbyRadius * nearbyRadius;
	if (mSectorLightingEditNotifyHashes.size() != sectorRegistry.sectorCount)
	{
		mSectorLightingEditNotifyHashes.assign(sectorRegistry.sectorCount, 0u);
		mSectorLightingEditNotifyCacheValid = false;
	}

	struct SectorSurfaceEditAggregate
	{
		uint64_t hash = 1469598103934665603ull;
		float center[3] = {};
		int64_t shadeSum = 0;
		int32_t minShade = std::numeric_limits<int32_t>::max();
		int32_t maxShade = std::numeric_limits<int32_t>::min();
		uint32_t count = 0;
	};

	std::vector<SectorSurfaceEditAggregate> sectorSurfaceAggregates(sectorRegistry.sectorCount);
	for (const SceneLightSystem::SurfaceRecord& record : mSceneLights.GetSurfaceRecords())
	{
		if (record.provenance.sectorIndex < 0)
		{
			continue;
		}

		const uint32_t sectorIndex = (uint32_t)record.provenance.sectorIndex;
		if (sectorIndex >= sectorSurfaceAggregates.size())
		{
			continue;
		}

		auto& aggregate = sectorSurfaceAggregates[sectorIndex];
		aggregate.center[0] += record.center[0];
		aggregate.center[1] += record.center[1];
		aggregate.center[2] += record.center[2];
		aggregate.shadeSum += record.material.shade;
		aggregate.minShade = std::min(aggregate.minShade, record.material.shade);
		aggregate.maxShade = std::max(aggregate.maxShade, record.material.shade);
		aggregate.count++;
		aggregate.hash = HashCombine64(aggregate.hash, (uint64_t)(uint32_t)record.material.shade);
		aggregate.hash = HashCombine64(aggregate.hash, (uint64_t)record.material.paletteIndex);
		aggregate.hash = HashCombine64(aggregate.hash, (uint64_t)FloatBits(record.material.lightLevel));
	}

	std::vector<uint32_t> changedNearbySurfaceSectors;
	std::vector<uint64_t> nextSurfaceHashes(sectorRegistry.sectorCount, 0u);
	for (uint32_t sectorIndex = 0; sectorIndex < (uint32_t)sectorSurfaceAggregates.size(); ++sectorIndex)
	{
		const auto& aggregate = sectorSurfaceAggregates[sectorIndex];
		if (aggregate.count == 0)
		{
			continue;
		}

		uint64_t hash = aggregate.hash;
		if (sectorIndex < sectorRegistry.sectors.size())
		{
			const auto& sector = sectorRegistry.sectors[sectorIndex];
			hash = HashCombine64(hash, (uint64_t)(uint32_t)sector.averageShade);
			hash = HashCombine64(hash, (uint64_t)(uint32_t)sector.rawAverageShade);
			hash = HashCombine64(hash, (uint64_t)(uint32_t)sector.paletteIndex);
			hash = HashCombine64(hash, (uint64_t)FloatBits(sector.rawFloorLight));
			hash = HashCombine64(hash, (uint64_t)FloatBits(sector.rawCeilingLight));
			hash = HashCombine64(hash, (uint64_t)FloatBits(sector.emitterResponseScale));
		}
		nextSurfaceHashes[sectorIndex] = hash;

		if (!mSectorLightingEditNotifyCacheValid ||
			sectorIndex >= mSectorLightingEditNotifyHashes.size() ||
			mSectorLightingEditNotifyHashes[sectorIndex] == 0u ||
			mSectorLightingEditNotifyHashes[sectorIndex] == hash)
		{
			continue;
		}

		const float invCount = 1.0f / (float)aggregate.count;
		const float centerX = aggregate.center[0] * invCount;
		const float centerY = aggregate.center[1] * invCount;
		const float centerZ = aggregate.center[2] * invCount;
		const float dx = centerX - mCurrentCameraPos[0];
		const float dy = centerY - mCurrentCameraPos[1];
		const float dz = centerZ - mCurrentCameraPos[2];
		if (dx * dx + dy * dy + dz * dz <= nearbyRadiusSq)
		{
			changedNearbySurfaceSectors.push_back(sectorIndex);
		}
	}

	if (!changedNearbySurfaceSectors.empty() && mFrameIndex - mLastSectorLightingEditNotifyFrame >= 12)
	{
		const uint32_t printCount = std::min<uint32_t>((uint32_t)changedNearbySurfaceSectors.size(), 6u);
		for (uint32_t i = 0; i < printCount; ++i)
		{
			const uint32_t sectorIndex = changedNearbySurfaceSectors[i];
			const auto& aggregate = sectorSurfaceAggregates[sectorIndex];
			const int32_t avgShade = aggregate.count > 0 ? (int32_t)(aggregate.shadeSum / (int64_t)aggregate.count) : 0;
			const SceneLightSystem::SectorLightingRegistry::SectorLightRecord* sector =
				sectorIndex < sectorRegistry.sectors.size() ? &sectorRegistry.sectors[sectorIndex] : nullptr;
			Printf(
				PRINT_LOW | PRINT_NOTIFY,
				"NRI PT sector %u surface light changed avg_shade=%d range=[%d,%d] sector_raw=(%.2f,%.2f) signal=%.2f response=%.2f\n",
				sectorIndex,
				avgShade,
				aggregate.minShade,
				aggregate.maxShade,
				sector != nullptr ? sector->rawFloorLight : 0.0f,
				sector != nullptr ? sector->rawCeilingLight : 0.0f,
				sector != nullptr ? sector->rawResponseSignal : 0.0f,
				sector != nullptr ? sector->emitterResponseScale : 1.0f);
		}
		if (changedNearbySurfaceSectors.size() > printCount)
		{
			Printf(PRINT_LOW | PRINT_NOTIFY, "NRI PT sector surface light changed: +%u more nearby sectors\n", (uint32_t)changedNearbySurfaceSectors.size() - printCount);
		}
		mLastSectorLightingEditNotifyFrame = mFrameIndex;
	}

	mSectorLightingEditNotifyHashes = std::move(nextSurfaceHashes);
	mSectorLightingEditNotifyCacheValid = true;

	if (mEmissiveSectorResponseNotifyScales.size() != sectorRegistry.sectors.size())
	{
		mEmissiveSectorResponseNotifyScales.assign(sectorRegistry.sectors.size(), -1.0f);
		mEmissiveSectorResponseNotifyCacheValid = false;
	}

	std::vector<float> nextScales(sectorRegistry.sectors.size(), -1.0f);
	std::vector<uint32_t> changedNearbySectors;

	for (const auto& surface : emissiveRegistry.activeSurfaces)
	{
		const bool sectorResponseEligible = IsEmissiveSurfaceSectorResponseEligible(surface);
		if (!sectorResponseEligible)
		{
			continue;
		}

		const uint32_t sectorIndex = (uint32_t)surface.sectorIndex;
		if (sectorIndex >= nextScales.size())
		{
			continue;
		}

		bool applied = false;
		const float scale = ResolveSectorEmissionScale(sectorRegistry, surface, applied);
		nextScales[sectorIndex] = std::max(nextScales[sectorIndex], scale);
		const float previousScale = sectorIndex < mEmissiveSectorResponseNotifyScales.size() ? mEmissiveSectorResponseNotifyScales[sectorIndex] : -1.0f;
		if (!mEmissiveSectorResponseNotifyCacheValid || previousScale < 0.0f || std::abs(previousScale - scale) <= 0.02f)
		{
			continue;
		}

		const float dx = surface.center[0] - mCurrentCameraPos[0];
		const float dy = surface.center[1] - mCurrentCameraPos[1];
		const float dz = surface.center[2] - mCurrentCameraPos[2];
		if (dx * dx + dy * dy + dz * dz > nearbyRadiusSq)
		{
			continue;
		}

		if (std::find(changedNearbySectors.begin(), changedNearbySectors.end(), sectorIndex) == changedNearbySectors.end())
		{
			changedNearbySectors.push_back(sectorIndex);
		}
	}

	if (!changedNearbySectors.empty() && mFrameIndex - mLastEmissiveSectorResponseNotifyFrame >= 12)
	{
		for (uint32_t sectorIndex : changedNearbySectors)
		{
			const float scale = sectorIndex < nextScales.size() && nextScales[sectorIndex] >= 0.0f ? nextScales[sectorIndex] : 1.0f;
			const char* state = scale > 1.01f ? "boosted" : (scale < 0.99f ? "dimmed" : "neutral");
			Printf(PRINT_LOW | PRINT_NOTIFY, "NRI PT sector %u emission %s %.2fx\n", sectorIndex, state, scale);
		}
		mLastEmissiveSectorResponseNotifyFrame = mFrameIndex;
	}

	mEmissiveSectorResponseNotifyScales = std::move(nextScales);
	mEmissiveSectorResponseNotifyCacheValid = true;
}

void NRIRenderer::TraceEmissiveSectorResponseChange()
{
	NotifyEmissiveSectorResponseEditModeChanges();

	if (!ShouldTracePtPerf())
	{
		mEmissiveSectorResponseTraceCacheValid = false;
		return;
	}

	const uint64_t sectorResponsePayloadHash = BuildEmissiveSectorResponsePayloadHash();
	if (!mEmissiveSectorResponseTraceCacheValid)
	{
		mEmissiveSectorResponseTraceCacheValid = true;
		mEmissiveSectorResponseTraceHash = sectorResponsePayloadHash;
		return;
	}

	if (mEmissiveSectorResponseTraceHash == sectorResponsePayloadHash)
	{
		return;
	}

	const auto& emissiveRegistry = mSceneLights.GetEmissiveSurfaces();
	const auto& sectorRegistry = mSceneLights.GetSectorLighting();
	uint32_t affectedEmitterCount = 0;
	for (const auto& surface : emissiveRegistry.activeSurfaces)
	{
		const bool sectorResponseEligible = IsEmissiveSurfaceSectorResponseEligible(surface);
		if (sectorResponseEligible)
		{
			affectedEmitterCount++;
		}
	}

	Printf("NRI PT sector response change: frame=%u affected_emitters=%u total_emitters=%u sector_response_hash=0x%016llx->0x%016llx response=boost:%u dim:%u neutral:%u\n",
		mFrameIndex,
		affectedEmitterCount,
		(uint32_t)emissiveRegistry.activeSurfaces.size(),
		(unsigned long long)mEmissiveSectorResponseTraceHash,
		(unsigned long long)sectorResponsePayloadHash,
		sectorRegistry.responseBoostSectorCount,
		sectorRegistry.responseDimSectorCount,
		sectorRegistry.responseNeutralSectorCount);

	mEmissiveSectorResponseTraceHash = sectorResponsePayloadHash;
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

bool NRIRenderer::UpdateEmissiveSamplingBuffers(const EmissiveSamplingBuildContext& context, bool* ioWaitedForWrites)
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.emissiveUpdateMs);
	const uint64_t payloadHash = BuildEmissiveSamplingPayloadHash(context);
	const uint64_t sectorResponsePayloadHash = BuildEmissiveSectorResponsePayloadHash();
	const bool sectorResponseChanged =
		mEmissiveSectorResponsePayloadCacheValid &&
		mEmissiveSectorResponsePayloadHash != sectorResponsePayloadHash;
	if (mEmissiveSamplingPayloadCacheValid &&
		mEmissiveSamplingPayloadHash == payloadHash &&
		mEmissivePrimitiveHeaderBuffer.shaderView != nullptr &&
		mEmissivePrimitiveBuffer.shaderView != nullptr &&
		mEmissivePrimitiveCdfBuffer.shaderView != nullptr &&
		mEmissiveMaterialResponseBuffer.shaderView != nullptr)
	{
		if (!mEmissiveSectorResponsePayloadCacheValid)
		{
			mEmissiveSectorResponsePayloadCacheValid = true;
			mEmissiveSectorResponsePayloadHash = sectorResponsePayloadHash;
		}
		return true;
	}

	EmissivePrimitiveHeaderGpuData emissiveHeader = {};
	std::vector<EmissivePrimitiveGpuData> emissivePrimitives;
	std::vector<float> emissiveCdf;
	std::vector<EmissiveMaterialResponseGpuData> emissiveMaterialResponses;
	std::vector<EmissivePrimitiveDebugRecord> emissiveDebugRecords;
	BuildEmissiveSamplingUpload(context, emissiveHeader, emissivePrimitives, emissiveCdf, emissiveMaterialResponses, emissiveDebugRecords);

	const auto ensureStructuredBufferBatched = [this, ioWaitedForWrites](NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after) -> bool
	{
		if (ioWaitedForWrites != nullptr &&
			!*ioWaitedForWrites &&
			StructuredBufferUpdateNeedsWait(resource, data, size, stride))
		{
			WaitForCommandsTracked("emissive_sampling_upload");
			*ioWaitedForWrites = true;
		}

		return EnsureStructuredBuffer(resource, stats, data, size, stride, usage, after, ioWaitedForWrites != nullptr && *ioWaitedForWrites, "emissive_sampling_upload");
	};

	if (!ensureStructuredBufferBatched(
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

	if (!ensureStructuredBufferBatched(
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

	if (!ensureStructuredBufferBatched(
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

	if (!ensureStructuredBufferBatched(
		mEmissiveMaterialResponseBuffer,
		mEmissiveMaterialResponseBufferStats,
		emissiveMaterialResponses.empty() ? nullptr : emissiveMaterialResponses.data(),
		emissiveMaterialResponses.empty() ? 0u : emissiveMaterialResponses.size() * sizeof(EmissiveMaterialResponseGpuData),
		sizeof(EmissiveMaterialResponseGpuData),
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
	mSceneDataDescriptors[25] = mEmissiveMaterialResponseBuffer.shaderView;

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
	if (sectorResponseChanged && ShouldTracePtPerf())
	{
		const auto& sectorRegistry = mSceneLights.GetSectorLighting();
		Printf("NRI PT emissive sampling refresh: frame=%u reason=sector-response-change primitives=%u total_power=%.3f dominant_primitive=%u dominant_tile=%u sector_response_hash=0x%016llx->0x%016llx response=boost:%u dim:%u neutral:%u\n",
			mFrameIndex,
			mBoundEmissivePrimitiveCount,
			mBoundEmissiveTotalPower,
			mBoundEmissiveDominantPrimitive,
			mBoundEmissiveDominantTile,
			(unsigned long long)mEmissiveSectorResponsePayloadHash,
			(unsigned long long)sectorResponsePayloadHash,
			sectorRegistry.responseBoostSectorCount,
			sectorRegistry.responseDimSectorCount,
			sectorRegistry.responseNeutralSectorCount);
	}
	mEmissiveSamplingPayloadCacheValid = true;
	mEmissiveSamplingPayloadHash = payloadHash;
	mEmissiveSectorResponsePayloadCacheValid = true;
	mEmissiveSectorResponsePayloadHash = sectorResponsePayloadHash;
	return true;
}

bool NRIRenderer::UpdateReprojectionBuffer(bool* ioWaitedForWrites)
{
	NRIReprojectionData data = {};
	std::memcpy(data.currentViewToClip, mCurrentViewToClip, sizeof(data.currentViewToClip));
	std::memcpy(data.previousViewToClip, mPreviousViewToClip, sizeof(data.previousViewToClip));
	std::memcpy(data.currentWorldToView, mCurrentWorldToView, sizeof(data.currentWorldToView));
	std::memcpy(data.previousWorldToView, mPreviousWorldToView, sizeof(data.previousWorldToView));
	if (ioWaitedForWrites != nullptr &&
		!*ioWaitedForWrites &&
		StructuredBufferUpdateNeedsWait(
			mReprojectionBuffer,
			&data,
			sizeof(data),
			sizeof(data)))
	{
		WaitForCommandsTracked("scene_data_upload");
		*ioWaitedForWrites = true;
	}
	if (!EnsureStructuredBuffer(
		mReprojectionBuffer,
		mReprojectionBufferStats,
		&data,
		sizeof(data),
		sizeof(data),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess(),
		ioWaitedForWrites != nullptr && *ioWaitedForWrites,
		"scene_data_upload"))
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

bool NRIRenderer::UpdateVisibleChunkBuffer(bool* ioWaitedForWrites)
{
	const uint32_t defaultVisibleChunkWord = 0u;
	const void* visibleChunkData = mCurrentVisibleChunkWords.empty() ? (const void*)&defaultVisibleChunkWord : mCurrentVisibleChunkWords.data();
	const size_t visibleChunkSize = mCurrentVisibleChunkWords.empty() ? sizeof(uint32_t) : mCurrentVisibleChunkWords.size() * sizeof(uint32_t);
	if (ioWaitedForWrites != nullptr &&
		!*ioWaitedForWrites &&
		StructuredBufferUpdateNeedsWait(
			mVisibleChunkBuffer,
			visibleChunkData,
			visibleChunkSize,
			sizeof(uint32_t)))
	{
		WaitForCommandsTracked("scene_data_upload");
		*ioWaitedForWrites = true;
	}
	if (!EnsureStructuredBuffer(
		mVisibleChunkBuffer,
		mVisibleChunkBufferStats,
		visibleChunkData,
		visibleChunkSize,
		sizeof(uint32_t),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess(),
		ioWaitedForWrites != nullptr && *ioWaitedForWrites,
		"scene_data_upload"))
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

bool NRIRenderer::UpdateVisibleFlatPlaneBuffer(bool* ioWaitedForWrites)
{
	const uint32_t defaultVisibleFlatPlaneWord = 0u;
	const void* visibleFlatPlaneData = mCurrentVisibleFlatPlaneWords.empty() ? (const void*)&defaultVisibleFlatPlaneWord : mCurrentVisibleFlatPlaneWords.data();
	const size_t visibleFlatPlaneSize = mCurrentVisibleFlatPlaneWords.empty() ? sizeof(uint32_t) : mCurrentVisibleFlatPlaneWords.size() * sizeof(uint32_t);
	if (ioWaitedForWrites != nullptr &&
		!*ioWaitedForWrites &&
		StructuredBufferUpdateNeedsWait(
			mVisibleFlatPlaneBuffer,
			visibleFlatPlaneData,
			visibleFlatPlaneSize,
			sizeof(uint32_t)))
	{
		WaitForCommandsTracked("scene_data_upload");
		*ioWaitedForWrites = true;
	}
	if (!EnsureStructuredBuffer(
		mVisibleFlatPlaneBuffer,
		mVisibleFlatPlaneBufferStats,
		visibleFlatPlaneData,
		visibleFlatPlaneSize,
		sizeof(uint32_t),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess(),
		ioWaitedForWrites != nullptr && *ioWaitedForWrites,
		"scene_data_upload"))
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
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneDataSetMs);
	mLastPerfShellTraceStats.sceneDataSetCalls++;
	SetCurrentSceneDataDescriptorsInitialized(false);
	bool waitedForWrites = false;
	const auto noteDataSetUpload = [&](const SceneBufferDebugStats& stats, uint64_t size, uint64_t& requestedBytes, uint64_t& uploadedBytes)
	{
		requestedBytes += size;
		uploadedBytes += stats.bytesUploadedLastFrame;
		mLastPerfShellTraceStats.sceneDataSetResourceGrowEvents += stats.growEventsLastFrame;
		mLastPerfShellTraceStats.sceneDataSetResourceOverwriteEvents += stats.overwriteEventsLastFrame;
	};
	const auto ensureStructuredBufferBatched = [&](NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, double& uploadMs, uint64_t& requestedBytes, uint64_t& uploadedBytes) -> bool
	{
		bool needsWait = false;
		{
			ScopedPtPerfTimer waitCheckTimer(mLastPerfShellTraceStats.sceneDataSetWaitCheckMs);
			needsWait = !waitedForWrites && StructuredBufferUpdateNeedsWait(resource, data, size, stride);
		}
		if (needsWait)
		{
			{
				ScopedPtPerfTimer waitTimer(mLastPerfShellTraceStats.sceneDataSetWaitMs);
				WaitForCommandsTracked("scene_data_upload");
			}
			mLastPerfShellTraceStats.sceneDataSetWaitCount++;
			waitedForWrites = true;
		}

		bool updated = false;
		{
			ScopedPtPerfTimer uploadTimer(uploadMs);
			updated = EnsureStructuredBuffer(resource, stats, data, size, stride, usage, after, waitedForWrites, "scene_data_upload");
		}
		if (updated)
		{
			noteDataSetUpload(stats, size, requestedBytes, uploadedBytes);
		}
		return updated;
	};

	{
		ScopedPtPerfTimer reprojectionTimer(mLastPerfShellTraceStats.sceneDataSetReprojectionMs);
		if (!UpdateReprojectionBuffer(&waitedForWrites))
		{
			return false;
		}
	}

	{
		ScopedPtPerfTimer visibleFlatTimer(mLastPerfShellTraceStats.sceneDataSetVisibleFlatPlaneMs);
		if (!UpdateVisibleFlatPlaneBuffer(&waitedForWrites))
		{
			return false;
		}
	}

	{
		ScopedPtPerfTimer visibleChunkTimer(mLastPerfShellTraceStats.sceneDataSetVisibleChunkMs);
		if (!UpdateVisibleChunkBuffer(&waitedForWrites))
		{
			return false;
		}
	}

	if (sceneInstances.empty())
	{
		return false;
	}

	mBoundRuntimeLightCount = 0;

	if (!ensureStructuredBufferBatched(
		mSceneInstanceBuffer,
		mSceneInstanceBufferStats,
		sceneInstances.data(),
		sceneInstances.size() * sizeof(SceneInstanceData),
		sizeof(SceneInstanceData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess(),
		mLastPerfShellTraceStats.sceneDataSetSceneInstanceMs,
		mLastPerfShellTraceStats.sceneDataSetSceneInstanceRequestedBytes,
		mLastPerfShellTraceStats.sceneDataSetSceneInstanceUploadedBytes))
	{
		return false;
	}
	mBoundSceneInstances = sceneInstances;

	std::vector<ScenePortalData> scenePortals;
	{
		ScopedPtPerfTimer portalTimer(mLastPerfShellTraceStats.sceneDataSetPortalMs);
		scenePortals = BuildScenePortalData(mMapWorld);
	}
	if (!ensureStructuredBufferBatched(
		mPortalBuffer,
		mPortalBufferStats,
		scenePortals.data(),
		scenePortals.size() * sizeof(ScenePortalData),
		sizeof(ScenePortalData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess(),
		mLastPerfShellTraceStats.sceneDataSetPortalMs,
		mLastPerfShellTraceStats.sceneDataSetPortalRequestedBytes,
		mLastPerfShellTraceStats.sceneDataSetPortalUploadedBytes))
	{
		return false;
	}

	uint64_t runtimeLightPayloadHash = 0;
	{
		ScopedPtPerfTimer hashTimer(mLastPerfShellTraceStats.sceneDataSetRuntimeLightHashMs);
		runtimeLightPayloadHash = BuildRuntimeLightPayloadHash();
	}
	const uint32_t activeRuntimeLightCount = (uint32_t)mSceneLights.GetAnalyticLights().activeLights.size();
	if (!mRuntimeLightPayloadCacheValid ||
		mRuntimeLightPayloadHash != runtimeLightPayloadHash ||
		mRuntimeLightBuffer.shaderView == nullptr)
	{
		mLastPerfShellTraceStats.sceneDataSetRuntimeLightUploads++;
		std::vector<RuntimePointLightGpuData> runtimeLights;
		{
			ScopedPtPerfTimer runtimeLightTimer(mLastPerfShellTraceStats.sceneDataSetRuntimeLightUploadMs);
			BuildRuntimePointLightUpload(runtimeLights);
		}
		if (!ensureStructuredBufferBatched(
			mRuntimeLightBuffer,
			mRuntimeLightBufferStats,
			runtimeLights.empty() ? nullptr : runtimeLights.data(),
			runtimeLights.size() * sizeof(RuntimePointLightGpuData),
			sizeof(RuntimePointLightGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess(),
			mLastPerfShellTraceStats.sceneDataSetRuntimeLightUploadMs,
			mLastPerfShellTraceStats.sceneDataSetRuntimeLightRequestedBytes,
			mLastPerfShellTraceStats.sceneDataSetRuntimeLightUploadedBytes))
		{
			return false;
		}

		mRuntimeLightPayloadCacheValid = true;
		mRuntimeLightPayloadHash = runtimeLightPayloadHash;
	}
	else
	{
		mLastPerfShellTraceStats.sceneDataSetRuntimeLightCacheHits++;
	}

	uint32_t runtimeLightTileCountX = 0;
	uint32_t runtimeLightTileCountY = 0;
	uint32_t runtimeLightTileIndexCount = 0;
	uint32_t runtimeLightMaxTileOccupancy = 0;
	uint64_t runtimeLightClusterCameraHash = 0;
	{
		ScopedPtPerfTimer runtimeLightClusterTimer(mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterMs);
		runtimeLightClusterCameraHash = BuildRuntimeLightClusterCameraHash();
	}
	const uint64_t runtimeLightClusterPayloadHash =
		HashCombine64(runtimeLightPayloadHash, runtimeLightClusterCameraHash);
	if (!mRuntimeLightClusterCacheValid ||
		mRuntimeLightClusterPayloadHash != runtimeLightClusterPayloadHash ||
		mRuntimeLightTileHeaderBuffer.shaderView == nullptr ||
		mRuntimeLightTileIndexBuffer.shaderView == nullptr)
	{
		mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterUploads++;
		std::vector<RuntimeLightTileHeaderGpuData> runtimeLightTileHeaders;
		std::vector<uint32_t> runtimeLightTileIndices;
		{
			ScopedPtPerfTimer runtimeLightClusterTimer(mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterMs);
			BuildRuntimeLightClusterUpload(
				runtimeLightTileHeaders,
				runtimeLightTileIndices,
				runtimeLightTileCountX,
				runtimeLightTileCountY,
				runtimeLightTileIndexCount,
				runtimeLightMaxTileOccupancy);
		}
		if (!ensureStructuredBufferBatched(
			mRuntimeLightTileHeaderBuffer,
			mRuntimeLightTileHeaderBufferStats,
			runtimeLightTileHeaders.data(),
			runtimeLightTileHeaders.size() * sizeof(RuntimeLightTileHeaderGpuData),
			sizeof(RuntimeLightTileHeaderGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess(),
			mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterMs,
			mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterRequestedBytes,
			mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterUploadedBytes))
		{
			return false;
		}

		if (!ensureStructuredBufferBatched(
			mRuntimeLightTileIndexBuffer,
			mRuntimeLightTileIndexBufferStats,
			runtimeLightTileIndices.data(),
			runtimeLightTileIndices.size() * sizeof(uint32_t),
			sizeof(uint32_t),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess(),
			mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterMs,
			mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterRequestedBytes,
			mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterUploadedBytes))
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
		mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterCacheHits++;
	}

	if (!mEmissiveSamplingPayloadCacheValid ||
		mEmissivePrimitiveHeaderBuffer.shaderView == nullptr ||
		mEmissivePrimitiveBuffer.shaderView == nullptr ||
		mEmissivePrimitiveCdfBuffer.shaderView == nullptr ||
		mEmissiveMaterialResponseBuffer.shaderView == nullptr)
	{
		mLastPerfShellTraceStats.sceneDataSetEmissiveUploads++;
		EmissivePrimitiveHeaderGpuData emissiveHeader = {};
		std::vector<EmissivePrimitiveGpuData> emissivePrimitives;
		std::vector<float> emissiveCdf;
		std::vector<EmissiveMaterialResponseGpuData> emissiveMaterialResponses;
		std::vector<EmissivePrimitiveDebugRecord> ignoredEmissiveDebugRecords;
		{
			ScopedPtPerfTimer emissiveTimer(mLastPerfShellTraceStats.sceneDataSetEmissiveMs);
			BuildEmissiveSamplingUpload({}, emissiveHeader, emissivePrimitives, emissiveCdf, emissiveMaterialResponses, ignoredEmissiveDebugRecords);
		}
		if (!ensureStructuredBufferBatched(
			mEmissivePrimitiveHeaderBuffer,
			mEmissivePrimitiveHeaderBufferStats,
			&emissiveHeader,
			sizeof(emissiveHeader),
			sizeof(EmissivePrimitiveHeaderGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess(),
			mLastPerfShellTraceStats.sceneDataSetEmissiveMs,
			mLastPerfShellTraceStats.sceneDataSetEmissiveRequestedBytes,
			mLastPerfShellTraceStats.sceneDataSetEmissiveUploadedBytes))
		{
			return false;
		}

		if (!ensureStructuredBufferBatched(
			mEmissivePrimitiveBuffer,
			mEmissivePrimitiveBufferStats,
			emissivePrimitives.empty() ? nullptr : emissivePrimitives.data(),
			emissivePrimitives.empty() ? 0u : emissivePrimitives.size() * sizeof(EmissivePrimitiveGpuData),
			sizeof(EmissivePrimitiveGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess(),
			mLastPerfShellTraceStats.sceneDataSetEmissiveMs,
			mLastPerfShellTraceStats.sceneDataSetEmissiveRequestedBytes,
			mLastPerfShellTraceStats.sceneDataSetEmissiveUploadedBytes))
		{
			return false;
		}

		if (!ensureStructuredBufferBatched(
			mEmissivePrimitiveCdfBuffer,
			mEmissivePrimitiveCdfBufferStats,
			emissiveCdf.data(),
			emissiveCdf.size() * sizeof(float),
			sizeof(float),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess(),
			mLastPerfShellTraceStats.sceneDataSetEmissiveMs,
			mLastPerfShellTraceStats.sceneDataSetEmissiveRequestedBytes,
			mLastPerfShellTraceStats.sceneDataSetEmissiveUploadedBytes))
		{
			return false;
		}

		if (!ensureStructuredBufferBatched(
			mEmissiveMaterialResponseBuffer,
			mEmissiveMaterialResponseBufferStats,
			emissiveMaterialResponses.empty() ? nullptr : emissiveMaterialResponses.data(),
			emissiveMaterialResponses.empty() ? 0u : emissiveMaterialResponses.size() * sizeof(EmissiveMaterialResponseGpuData),
			sizeof(EmissiveMaterialResponseGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess(),
			mLastPerfShellTraceStats.sceneDataSetEmissiveMs,
			mLastPerfShellTraceStats.sceneDataSetEmissiveRequestedBytes,
			mLastPerfShellTraceStats.sceneDataSetEmissiveUploadedBytes))
		{
			return false;
		}
	}
	else
	{
		mLastPerfShellTraceStats.sceneDataSetEmissiveCacheHits++;
	}

	uint64_t sectorLightingPayloadHash = 0;
	{
		ScopedPtPerfTimer sectorLightTimer(mLastPerfShellTraceStats.sceneDataSetSectorLightMs);
		UpdateBoundSectorLightingState();
		sectorLightingPayloadHash = BuildSectorLightingPayloadHash();
	}
	if (!mSectorLightingPayloadCacheValid ||
		mSectorLightingPayloadHash != sectorLightingPayloadHash ||
		mSectorLightHeaderBuffer.shaderView == nullptr ||
		mSectorLightBuffer.shaderView == nullptr)
	{
		mLastPerfShellTraceStats.sceneDataSetSectorLightUploads++;
		SectorLightHeaderGpuData sectorLightHeader = {};
		std::vector<SectorLightGpuData> sectorLights;
		{
			ScopedPtPerfTimer sectorLightTimer(mLastPerfShellTraceStats.sceneDataSetSectorLightMs);
			BuildSectorLightingUpload(sectorLightHeader, sectorLights);
		}
		if (!ensureStructuredBufferBatched(
			mSectorLightHeaderBuffer,
			mSectorLightHeaderBufferStats,
			&sectorLightHeader,
			sizeof(sectorLightHeader),
			sizeof(SectorLightHeaderGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess(),
			mLastPerfShellTraceStats.sceneDataSetSectorLightMs,
			mLastPerfShellTraceStats.sceneDataSetSectorLightRequestedBytes,
			mLastPerfShellTraceStats.sceneDataSetSectorLightUploadedBytes))
		{
			return false;
		}

		if (!ensureStructuredBufferBatched(
			mSectorLightBuffer,
			mSectorLightBufferStats,
			sectorLights.empty() ? nullptr : sectorLights.data(),
			sectorLights.empty() ? 0u : sectorLights.size() * sizeof(SectorLightGpuData),
			sizeof(SectorLightGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIComputeShaderResourceAccess(),
			mLastPerfShellTraceStats.sceneDataSetSectorLightMs,
			mLastPerfShellTraceStats.sceneDataSetSectorLightRequestedBytes,
			mLastPerfShellTraceStats.sceneDataSetSectorLightUploadedBytes))
		{
			return false;
		}

		mSectorLightingPayloadCacheValid = true;
		mSectorLightingPayloadHash = sectorLightingPayloadHash;
	}
	else
	{
		mLastPerfShellTraceStats.sceneDataSetSectorLightCacheHits++;
	}

	auto selectView = [](const NRIBufferResource& primary, const NRIBufferResource& fallback) -> nri::Descriptor*
	{
		return primary.shaderView != nullptr ? primary.shaderView : fallback.shaderView;
	};

	{
		ScopedPtPerfTimer descriptorBuildTimer(mLastPerfShellTraceStats.sceneDataSetDescriptorBuildMs);
		mSceneDataDescriptors.fill(nullptr);
		mSceneDataDescriptors[0] = selectView(staticVertexBuffer, dynamicVertexBuffer);
		mSceneDataDescriptors[1] = selectView(staticIndexBuffer, dynamicIndexBuffer);
		mSceneDataDescriptors[2] = selectView(staticPrimitiveBuffer, dynamicPrimitiveBuffer);
		mSceneDataDescriptors[3] = selectView(staticMaterialBuffer, dynamicMaterialBuffer);
		mSceneDataDescriptors[4] = selectView(dynamicVertexBuffer, staticVertexBuffer);
		mSceneDataDescriptors[5] = selectView(dynamicIndexBuffer, staticIndexBuffer);
		mSceneDataDescriptors[6] = selectView(dynamicPrimitiveBuffer, staticPrimitiveBuffer);
		mSceneDataDescriptors[7] = selectView(dynamicMaterialBuffer, staticMaterialBuffer);
		mSceneDataDescriptors[8] = mSceneInstanceBuffer.shaderView;
		mSceneDataDescriptors[9] = mPortalBuffer.shaderView;
		mSceneDataDescriptors[10] = mRuntimeLightBuffer.shaderView;
		mSceneDataDescriptors[11] = mRuntimeLightTileHeaderBuffer.shaderView;
		mSceneDataDescriptors[12] = mRuntimeLightTileIndexBuffer.shaderView;
		mSceneDataDescriptors[13] = mEmissivePrimitiveHeaderBuffer.shaderView;
		mSceneDataDescriptors[14] = mEmissivePrimitiveBuffer.shaderView;
		mSceneDataDescriptors[15] = mEmissivePrimitiveCdfBuffer.shaderView;
		mSceneDataDescriptors[16] = mSectorLightHeaderBuffer.shaderView;
		mSceneDataDescriptors[17] = mSectorLightBuffer.shaderView;
		mSceneDataDescriptors[18] = mReprojectionBuffer.shaderView;
		mSceneDataDescriptors[19] = mVisibleChunkBuffer.shaderView;
		mSceneDataDescriptors[20] = mVisibleFlatPlaneBuffer.shaderView;
		const NRIPersistentVoxelDescriptorSnapshot persistentVoxelDescriptors =
			mPersistentVoxels.BuildDescriptorSnapshot(dynamicVertexBuffer, dynamicIndexBuffer, dynamicPrimitiveBuffer, dynamicMaterialBuffer);
		mSceneDataDescriptors[21] = persistentVoxelDescriptors.vertex;
		mSceneDataDescriptors[22] = persistentVoxelDescriptors.index;
		mSceneDataDescriptors[23] = persistentVoxelDescriptors.primitive;
		mSceneDataDescriptors[24] = persistentVoxelDescriptors.material;
		mSceneDataDescriptors[25] = mEmissiveMaterialResponseBuffer.shaderView;
	}

	{
		ScopedPtPerfTimer descriptorValidateTimer(mLastPerfShellTraceStats.sceneDataSetDescriptorValidateMs);
		for (const nri::Descriptor* descriptor : mSceneDataDescriptors)
		{
			if (descriptor == nullptr)
			{
				mLastPerfShellTraceStats.sceneDataSetDescriptorNullCount++;
				return false;
			}
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
	mRuntimeLightSceneDataDirty = false;
	return true;
}

bool NRIRenderer::CommitSceneDataDescriptors(const char* reason)
{
	{
		ScopedPtPerfTimer descriptorValidateTimer(mLastPerfShellTraceStats.sceneDataSetDescriptorValidateMs);
		for (const nri::Descriptor* descriptor : mSceneDataDescriptors)
		{
			if (descriptor == nullptr)
			{
				mLastPerfShellTraceStats.sceneDataSetDescriptorNullCount++;
				return false;
			}
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
	{
		ScopedPtPerfTimer descriptorUpdateTimer(mLastPerfShellTraceStats.sceneDataSetDescriptorUpdateMs);
		mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	}
	mLastPerfShellTraceStats.sceneDataSetDescriptorUpdateCount++;
	SetCurrentSceneDataDescriptorsInitialized(true);
	{
		ScopedPtPerfTimer descriptorHashTimer(mLastPerfShellTraceStats.sceneDataSetDescriptorHashMs);
		TraceSharedDescriptorRewrite(
			"scene_data",
			reason != nullptr ? reason : "unlabeled",
			HashDescriptorList(reinterpret_cast<const nri::Descriptor* const*>(mSceneDataDescriptors.data()), mSceneDataDescriptors.size()),
			NRI_SCENE_DATA_DESCRIPTOR_NUM,
			false);
	}
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
	if (!EnsureTraceShaderStatsResources())
	{
		return false;
	}

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

	const nri::Descriptor* traceStatsDescriptor = mTraceShaderStatsBuffer.shaderView;
	nri::UpdateDescriptorRangeDesc statsUpdate = {};
	statsUpdate.descriptorSet = set;
	statsUpdate.rangeIndex = 1;
	statsUpdate.descriptors = &traceStatsDescriptor;
	statsUpdate.descriptorNum = NRI_TRACE_SHADER_STATS_DESCRIPTOR_NUM;
	mFrameBuffer->mCore.UpdateDescriptorRanges(&statsUpdate, 1);
	return true;
}

bool NRIRenderer::EnsureTraceShaderStatsResources()
{
	constexpr uint32_t kStride = sizeof(uint32_t);
	const uint64_t byteSize = (uint64_t)NRI_TRACE_SHADER_STATS_COUNTER_COUNT * kStride;
	if (mTraceShaderStatsBuffer.buffer == nullptr || mTraceShaderStatsBuffer.shaderView == nullptr)
	{
		DestroyBufferResource(mTraceShaderStatsBuffer);
		nri::BufferDesc desc = {};
		desc.size = byteSize;
		desc.structureStride = kStride;
		desc.usage = NRIFlags(
			nri::BufferUsageBits::SHADER_RESOURCE_STORAGE,
			nri::BufferUsageBits::SHADER_RESOURCE);
		if (mFrameBuffer->mCore.CreateCommittedBuffer(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, desc, mTraceShaderStatsBuffer.buffer) != nri::Result::SUCCESS)
		{
			return false;
		}

		nri::MemoryDesc memoryDesc = {};
		mFrameBuffer->mCore.GetBufferMemoryDesc(*mTraceShaderStatsBuffer.buffer, nri::MemoryLocation::DEVICE, memoryDesc);
		mTraceShaderStatsBuffer.size = desc.size;
		mTraceShaderStatsBuffer.memorySize = memoryDesc.size;
		mTraceShaderStatsBuffer.memoryLocation = nri::MemoryLocation::DEVICE;
		mTraceShaderStatsBuffer.usedSize = byteSize;
		mTraceShaderStatsBuffer.stride = kStride;

		nri::BufferViewDesc viewDesc = {};
		viewDesc.buffer = mTraceShaderStatsBuffer.buffer;
		viewDesc.type = nri::BufferView::STORAGE_STRUCTURED_BUFFER;
		viewDesc.offset = 0;
		viewDesc.size = nri::WHOLE_SIZE;
		viewDesc.structureStride = kStride;
		if (mFrameBuffer->mCore.CreateBufferView(viewDesc, mTraceShaderStatsBuffer.shaderView) != nri::Result::SUCCESS)
		{
			return false;
		}
	}

	if (mTraceShaderStatsReadbackBuffer.buffer == nullptr)
	{
		if (!CreateBufferWithoutViewAtLocation(
			mTraceShaderStatsReadbackBuffer,
			byteSize,
			kStride,
			nri::BufferUsageBits::NONE,
			nri::MemoryLocation::HOST_READBACK))
		{
			return false;
		}
	}

	if (mTraceShaderStatsZeroBuffer.buffer == nullptr)
	{
		if (!CreateBufferWithoutViewAtLocation(
			mTraceShaderStatsZeroBuffer,
			byteSize,
			kStride,
			nri::BufferUsageBits::NONE,
			nri::MemoryLocation::DEVICE_UPLOAD))
		{
			return false;
		}

		void* mapped = mFrameBuffer->mCore.MapBuffer(*mTraceShaderStatsZeroBuffer.buffer, 0, byteSize);
		if (mapped == nullptr)
		{
			return false;
		}
		std::memset(mapped, 0, (size_t)byteSize);
		mFrameBuffer->mCore.UnmapBuffer(*mTraceShaderStatsZeroBuffer.buffer);
	}

	return true;
}

void NRIRenderer::ResetTraceShaderStatsBuffer()
{
	if (!ShouldCollectTraceShaderStats() || mFrameBuffer == nullptr || mFrameBuffer->mCommandBuffer == nullptr || !EnsureTraceShaderStatsResources())
	{
		return;
	}

	const uint64_t byteSize = (uint64_t)NRI_TRACE_SHADER_STATS_COUNTER_COUNT * sizeof(uint32_t);
	nri::BufferBarrierDesc beforeBarriers[2] = {};
	beforeBarriers[0].buffer = mTraceShaderStatsZeroBuffer.buffer;
	beforeBarriers[0].before = {};
	beforeBarriers[0].after = NRICopySourceAccess();
	beforeBarriers[1].buffer = mTraceShaderStatsBuffer.buffer;
	beforeBarriers[1].before = {};
	beforeBarriers[1].after = NRICopyDestinationAccess();
	nri::BarrierDesc beforeDesc = {};
	beforeDesc.buffers = beforeBarriers;
	beforeDesc.bufferNum = 2;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, beforeDesc);
	mFrameBuffer->mCore.CmdCopyBuffer(
		*mFrameBuffer->mCommandBuffer,
		*mTraceShaderStatsBuffer.buffer,
		0,
		*mTraceShaderStatsZeroBuffer.buffer,
		0,
		byteSize);

	nri::BufferBarrierDesc afterBarrier = {};
	afterBarrier.buffer = mTraceShaderStatsBuffer.buffer;
	afterBarrier.before = NRICopyDestinationAccess();
	afterBarrier.after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	nri::BarrierDesc afterDesc = {};
	afterDesc.buffers = &afterBarrier;
	afterDesc.bufferNum = 1;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, afterDesc);
}

void NRIRenderer::CopyTraceShaderStatsForReadback(uint64_t frameNumber)
{
	if (!ShouldCollectTraceShaderStats() || mFrameBuffer == nullptr || mFrameBuffer->mCommandBuffer == nullptr || !EnsureTraceShaderStatsResources())
	{
		return;
	}

	const uint64_t byteSize = (uint64_t)NRI_TRACE_SHADER_STATS_COUNTER_COUNT * sizeof(uint32_t);
	nri::BufferBarrierDesc beforeBarrier = {};
	beforeBarrier.buffer = mTraceShaderStatsBuffer.buffer;
	beforeBarrier.before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	beforeBarrier.after = NRICopySourceAccess();
	nri::BarrierDesc beforeDesc = {};
	beforeDesc.buffers = &beforeBarrier;
	beforeDesc.bufferNum = 1;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, beforeDesc);
	mFrameBuffer->mCore.CmdCopyBuffer(
		*mFrameBuffer->mCommandBuffer,
		*mTraceShaderStatsReadbackBuffer.buffer,
		0,
		*mTraceShaderStatsBuffer.buffer,
		0,
		byteSize);
	mPendingTraceShaderStatsFrame = frameNumber;
}

void NRIRenderer::ReadbackTraceShaderStats()
{
	if (!nri_ptshaderstats || mPendingTraceShaderStatsFrame == 0 || mTraceShaderStatsReadbackBuffer.buffer == nullptr)
	{
		return;
	}

	WaitForCommandsTracked("trace_shader_stats_readback");
	const uint64_t byteSize = (uint64_t)NRI_TRACE_SHADER_STATS_COUNTER_COUNT * sizeof(uint32_t);
	const void* mapped = mFrameBuffer->mCore.MapBuffer(*mTraceShaderStatsReadbackBuffer.buffer, 0, byteSize);
	if (mapped == nullptr)
	{
		mPendingTraceShaderStatsFrame = 0;
		return;
	}

	mLastPerfTraceShaderStats.valid = true;
	mLastPerfTraceShaderStats.frameNumber = mPendingTraceShaderStatsFrame;
	std::memcpy(mLastPerfTraceShaderStats.counters.data(), mapped, (size_t)byteSize);
	mLastPerfTraceShaderStats.hotInstanceCount = 0;
	mLastPerfTraceShaderStats.hotInstances = {};
	struct TraceShaderHotCandidate
	{
		uint32_t instanceId = 0;
		uint32_t committed = 0;
		uint32_t accepted = 0;
	};
	std::vector<TraceShaderHotCandidate> hotCandidates;
	const uint32_t instanceBucketCount = std::min<uint32_t>((uint32_t)mBoundSceneInstances.size(), TraceShaderInstanceBucketCount);
	hotCandidates.reserve(instanceBucketCount);
	for (uint32_t instanceId = 0; instanceId < instanceBucketCount; ++instanceId)
	{
		const uint32_t committed = mLastPerfTraceShaderStats.counters[TraceShaderInstanceCommittedBase + instanceId];
		const uint32_t accepted = mLastPerfTraceShaderStats.counters[TraceShaderInstanceAcceptedBase + instanceId];
		if (committed == 0 && accepted == 0)
		{
			continue;
		}
		hotCandidates.push_back({ instanceId, committed, accepted });
	}
	std::sort(
		hotCandidates.begin(),
		hotCandidates.end(),
		[](const TraceShaderHotCandidate& a, const TraceShaderHotCandidate& b)
		{
			if (a.committed != b.committed)
			{
				return a.committed > b.committed;
			}
			return a.accepted > b.accepted;
		});
	auto getDataSourcePrimitiveTotal = [this](uint32_t dataSource) -> uint32_t
	{
		switch (dataSource)
		{
		case NRI_SCENE_DATA_SOURCE_STATIC: return mBoundStaticPrimitiveCount;
		case NRI_SCENE_DATA_SOURCE_DYNAMIC: return mBoundDynamicPrimitiveCount;
		case NRI_SCENE_DATA_SOURCE_PERSISTENT_VOXEL: return mPersistentVoxels.BoundPrimitiveCount();
		default: return 0;
		}
	};
	auto estimateInstancePrimitiveCount = [this, &getDataSourcePrimitiveTotal](const SceneInstanceData& instance) -> uint32_t
	{
		if (instance.dataSource == NRI_SCENE_DATA_SOURCE_PERSISTENT_VOXEL)
		{
			const uint32_t persistentVoxelPrimitiveCount = mPersistentVoxels.EstimatePrimitiveCountForInstanceOffset(instance.primitiveOffset);
			if (persistentVoxelPrimitiveCount > 0)
			{
				return persistentVoxelPrimitiveCount;
			}
		}

		const uint32_t total = getDataSourcePrimitiveTotal(instance.dataSource);
		if (instance.primitiveOffset >= total)
		{
			return 0;
		}

		uint32_t endOffset = total;
		for (const SceneInstanceData& other : mBoundSceneInstances)
		{
			if (other.dataSource == instance.dataSource &&
				other.primitiveOffset > instance.primitiveOffset &&
				other.primitiveOffset < endOffset)
			{
				endOffset = other.primitiveOffset;
			}
		}
		return endOffset - instance.primitiveOffset;
	};
	const uint32_t hotCount = std::min<uint32_t>((uint32_t)hotCandidates.size(), TraceShaderHotInstanceCount);
	mLastPerfTraceShaderStats.hotInstanceCount = hotCount;
	for (uint32_t hotIndex = 0; hotIndex < hotCount; ++hotIndex)
	{
		const TraceShaderHotCandidate& candidate = hotCandidates[hotIndex];
		const SceneInstanceData& instance = mBoundSceneInstances[candidate.instanceId];
		PerfTraceShaderHotInstance& hot = mLastPerfTraceShaderStats.hotInstances[hotIndex];
		hot.instanceId = candidate.instanceId;
		hot.dataSource = instance.dataSource;
		hot.primitiveOffset = instance.primitiveOffset;
		hot.primitiveCount = estimateInstancePrimitiveCount(instance);
		hot.metadata0 = instance.reserved0;
		hot.metadata1 = instance.reserved1;
		hot.committed = candidate.committed;
		hot.accepted = candidate.accepted;
		hot.primaryCommitted = mLastPerfTraceShaderStats.counters[TraceShaderInstanceKindCommittedBase + 0u * TraceShaderInstanceBucketCount + candidate.instanceId];
		hot.ungatedCommitted = mLastPerfTraceShaderStats.counters[TraceShaderInstanceKindCommittedBase + 1u * TraceShaderInstanceBucketCount + candidate.instanceId];
		hot.sunCommitted = mLastPerfTraceShaderStats.counters[TraceShaderInstanceKindCommittedBase + 2u * TraceShaderInstanceBucketCount + candidate.instanceId];
		hot.pointCommitted = mLastPerfTraceShaderStats.counters[TraceShaderInstanceKindCommittedBase + 3u * TraceShaderInstanceBucketCount + candidate.instanceId];
		hot.emissiveCommitted = mLastPerfTraceShaderStats.counters[TraceShaderInstanceKindCommittedBase + 4u * TraceShaderInstanceBucketCount + candidate.instanceId];
		hot.fastEmissiveCommitted = mLastPerfTraceShaderStats.counters[TraceShaderInstanceKindCommittedBase + 5u * TraceShaderInstanceBucketCount + candidate.instanceId];
	}
	mFrameBuffer->mCore.UnmapBuffer(*mTraceShaderStatsReadbackBuffer.buffer);
	mPendingTraceShaderStatsFrame = 0;
}

bool NRIRenderer::EnsureAutoExposureResources(const NRIAutoExposureSettings& settings)
{
	return EnsureNRIRendererAutoExposureResources(*this, settings);
}

void NRIRenderer::DestroyAutoExposureResources()
{
	DestroyNRIRendererAutoExposureResources(*this);
}

bool NRIRenderer::UpdateAutoExposureDescriptorSets(FrameTextureSlot sourceSlot)
{
	return UpdateNRIRendererAutoExposureDescriptorSets(*this, (uint32_t)sourceSlot);
}

bool NRIRenderer::DispatchAutoExposure(FrameTextureSlot sourceSlot)
{
	return DispatchNRIRendererAutoExposure(*this, (uint32_t)sourceSlot);
}

void NRIRenderer::CopyAutoExposureStatsForReadback(uint64_t frameNumber)
{
	CopyNRIRendererAutoExposureStatsForReadback(*this, frameNumber);
}

void NRIRenderer::ReadbackAutoExposureStats()
{
	ReadbackNRIRendererAutoExposureStats(*this);
}

bool NRIRenderer::CreateFrameTexture(FrameTextureSlot slot, uint32_t width, uint32_t height, nri::Format format)
{
	return mFrameBuffer->CreateOwnedTexture(GetFrameTexture(slot), width, height, format, NRIFlags(nri::TextureUsageBits::SHADER_RESOURCE, nri::TextureUsageBits::SHADER_RESOURCE_STORAGE));
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
	if (nri_ptscenestats)
	{
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
	}

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
	mVertexBufferStats.growthOldBytesLastFrame = 0;
	mVertexBufferStats.growthRequestedBytesLastFrame = 0;
	mVertexBufferStats.growthAllocatedBytesLastFrame = 0;
	mIndexBufferStats.bytesUploadedLastFrame = 0;
	mIndexBufferStats.growEventsLastFrame = 0;
	mIndexBufferStats.overwriteEventsLastFrame = 0;
	mIndexBufferStats.growthOldBytesLastFrame = 0;
	mIndexBufferStats.growthRequestedBytesLastFrame = 0;
	mIndexBufferStats.growthAllocatedBytesLastFrame = 0;
	mPrimitiveBufferStats.bytesUploadedLastFrame = 0;
	mPrimitiveBufferStats.growEventsLastFrame = 0;
	mPrimitiveBufferStats.overwriteEventsLastFrame = 0;
	mPrimitiveBufferStats.growthOldBytesLastFrame = 0;
	mPrimitiveBufferStats.growthRequestedBytesLastFrame = 0;
	mPrimitiveBufferStats.growthAllocatedBytesLastFrame = 0;
	mMaterialBufferStats.bytesUploadedLastFrame = 0;
	mMaterialBufferStats.growEventsLastFrame = 0;
	mMaterialBufferStats.overwriteEventsLastFrame = 0;
	mMaterialBufferStats.growthOldBytesLastFrame = 0;
	mMaterialBufferStats.growthRequestedBytesLastFrame = 0;
	mMaterialBufferStats.growthAllocatedBytesLastFrame = 0;
	mPortalBufferStats.bytesUploadedLastFrame = 0;
	mPortalBufferStats.growEventsLastFrame = 0;
	mPortalBufferStats.overwriteEventsLastFrame = 0;
}

const NRIBufferResource& NRIRenderer::GetActiveVertexBuffer() const
{
	return mBoundDynamicPrimitiveCount > 0 ? GetCurrentDynamicVertexBuffer() : mStaticVertexBuffer;
}

const NRIBufferResource& NRIRenderer::GetActiveIndexBuffer() const
{
	return mBoundDynamicPrimitiveCount > 0 ? GetCurrentDynamicIndexBuffer() : mStaticIndexBuffer;
}

const NRIBufferResource& NRIRenderer::GetActivePrimitiveBuffer() const
{
	return mBoundDynamicPrimitiveCount > 0 ? GetCurrentDynamicPrimitiveBuffer() : mStaticPrimitiveBuffer;
}

const NRIBufferResource& NRIRenderer::GetActiveMaterialBuffer() const
{
	return mBoundDynamicMaterialCount > 0 ? GetCurrentDynamicMaterialBuffer() : mStaticMaterialBuffer;
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
		(mSkyEnvironment.PreservedStaticMapSky().valid && mSkyEnvironment.PreservedStaticMapSky().buildSerial == mMapWorld.buildSerial)
		? &mSkyEnvironment.PreservedStaticMapSky().sceneView
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
		if (targetChunk.chunkIndex < mStaticSceneResidency.Registry().entries.size() &&
			mStaticSceneResidency.Registry().entries[targetChunk.chunkIndex].valid)
		{
			mStaticSceneResidency.Registry().entries[targetChunk.chunkIndex].animatedRefreshSuppressed = true;
			mStaticSceneResidency.Registry().entries[targetChunk.chunkIndex].animatedSuppressionEmitCount++;
		}
		mLastPerfShellTraceStats.runtimeAnimatedSuppressionEmitCount++;
		if (nri_ptscenestats)
		{
			Printf("NRI PT static scene anim: suppressing chunk=%u resident animated refresh (%s).\n",
				targetChunk.chunkIndex,
				reason != nullptr ? reason : "unknown");
		}
	};

	for (size_t chunkListIndex = 0; chunkListIndex < mStaticMapScene.chunks.size(); ++chunkListIndex)
	{
		auto& chunkCache = mStaticMapScene.chunks[chunkListIndex];
		if (chunkListIndex >= mStaticMapScene.lightChunkViews.size() || chunkCache.chunkIndex >= mMapWorld.chunks.size())
		{
			DestroyStaticMapSceneCache("animated-refresh-layout-mismatch");
			mStaticMapScene = {};
			mStaticAccelerationBuildSerial = 0;
			mSkyEnvironment.PreservedStaticMapSky() = {};
			return EnsureStaticMapScene();
		}
		if (!chunkCache.active)
		{
			continue;
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
		const uint64_t liveAnimatedMaterialSignature = nri_runtime_mutation::ComputeAnimatedMaterialSignature(liveChunkView);
		if (liveAnimatedMaterialSignature == chunkCache.animatedMaterialSignature)
		{
			continue;
		}

		const uint64_t liveAnimatedGeometrySignature = nri_runtime_mutation::ComputeAnimatedGeometrySignature(liveChunkView);
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
	if (!nri_static_scene::RebuildResidentStaticMaterialBridgeFromChunks(
		mStaticMapScene,
		mStaticMapChunkAtlas,
		nri_ptscenestats && ShouldTracePtPerf()))
	{
		mStaticMapScene.animatedGeometryFallbackCount++;
		DestroyStaticMapSceneCache("animated-refresh-material-bridge-failed");
		mStaticMapScene = {};
		mStaticAccelerationBuildSerial = 0;
		mSkyEnvironment.PreservedStaticMapSky() = {};
		return EnsureStaticMapScene();
	}

	if (!EnsurePaletteTexture(mStaticMapScene.materialBridge) ||
		!EnsureSceneTextures(mStaticMapScene.sceneView, mStaticMapScene.materialBridge, mStaticMapScene.gpuMaterials, false, "static_map_scene_anim") ||
		!nri_static_scene_geometry_upload::UploadStaticMapChunkMaterialAtlas(
			BuildStaticSceneGeometryUploadServices(),
			mStaticMaterialBuffer,
			mMaterialBufferStats,
			mStaticMapChunkAtlas,
			mStaticMapScene,
			mStaticMapScene.gpuMaterials))
	{
		DestroyStaticMapSceneCache("animated-refresh-upload-failed");
		mStaticMapScene = {};
		mStaticAccelerationBuildSerial = 0;
		mSkyEnvironment.PreservedStaticMapSky() = {};
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

	const char* rebuildReason = nullptr;
	if (mStaticMapScene.buildSerial != mMapWorld.buildSerial)
	{
		rebuildReason = "build-serial-mismatch";
		DestroyStaticMapSceneCache("ensure-static-scene-build-serial-mismatch");
		mStaticMapScene = {};
		mStaticAccelerationBuildSerial = 0;
		mSkyEnvironment.PreservedStaticMapSky() = {};
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

	if (rebuildReason == nullptr)
	{
		rebuildReason =
			!mStaticMapScene.valid ? "scene-invalid" :
			!mStaticMapScene.texturesResident ? "textures-not-resident" :
			!mStaticMapScene.buffersResident ? "buffers-not-resident" :
			!mStaticMapScene.accelerationResident ? "acceleration-not-resident" :
			"resident-rebuild";
	}
	if (nri_ptscenestats)
	{
		Printf("NRI PT static scene trace: event=rebuild reason=%s frame=%u scene_valid=%s textures=%s buffers=%s acceleration=%s scene_build_serial=%llu map_build_serial=%llu chunks=%u\n",
			rebuildReason,
			mFrameIndex,
			YesNo(mStaticMapScene.valid),
			YesNo(mStaticMapScene.texturesResident),
			YesNo(mStaticMapScene.buffersResident),
			YesNo(mStaticMapScene.accelerationResident),
			(unsigned long long)mStaticMapScene.buildSerial,
			(unsigned long long)mMapWorld.buildSerial,
			(uint32_t)mStaticMapScene.chunks.size());
	}

	NRIStaticSceneCacheBuildServices staticSceneCacheBuildServices = {};
	staticSceneCacheBuildServices.user = this;
	staticSceneCacheBuildServices.resetMutationCacheForStaticSceneBuild = [](void* user, uint32_t chunkCount)
	{
		static_cast<NRIRenderer*>(user)->mRuntimeMutation.ResetCacheForStaticSceneBuild(chunkCount);
	};
	staticSceneCacheBuildServices.initializeStaticChunkReplacement = [](void* user, const nri_scene::PTMapChunk& chunk)
	{
		static_cast<NRIRenderer*>(user)->mRuntimeMutation.InitializeStaticChunkReplacement(chunk);
	};
	staticSceneCacheBuildServices.buildMaterialsWithActorOverrides = [](void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label)
	{
		static_cast<NRIRenderer*>(user)->BuildMaterialsWithActorOverrides(sceneView, materials, label);
	};
	staticSceneCacheBuildServices.chunkHasAnimatedStaticMapSurfaceCandidates = [](void*, const nri_scene::PTMapWorld& mapWorld, const nri_scene::PTMapChunk& chunk)
	{
		return ChunkHasAnimatedStaticMapSurfaceCandidates(mapWorld, chunk);
	};
	staticSceneCacheBuildServices.geometryBuildStaticChunkMs = &mLastPerfShellTraceStats.geometryBuildStaticChunkMs;
	staticSceneCacheBuildServices.geometryBuildStaticChunkCalls = &mLastPerfShellTraceStats.geometryBuildStaticChunkCalls;
	staticSceneCacheBuildServices.geometryBuildStaticChunkPrimitives = &mLastPerfShellTraceStats.geometryBuildStaticChunkPrimitives;
	staticSceneCacheBuildServices.ceilingNudge = nri_ptceilingnudge;
	staticSceneCacheBuildServices.ceilingNudgeDistance = (float)nri_ptceilingnudgedistance;

	if (!nri_static_scene::BuildStaticMapSceneCache(
		mMapWorld,
		(mSkyEnvironment.PreservedStaticMapSky().valid && mSkyEnvironment.PreservedStaticMapSky().buildSerial == mMapWorld.buildSerial) ? &mSkyEnvironment.PreservedStaticMapSky() : nullptr,
		staticSceneCacheBuildServices,
		mStaticMapScene))
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
		!nri_static_scene_geometry_upload::UploadStaticMapChunkAtlas(
			mMapWorld,
			BuildStaticSceneGeometryUploadServices(),
			mStaticVertexBuffer,
			mVertexBufferStats,
			mStaticIndexBuffer,
			mIndexBufferStats,
			mStaticPrimitiveBuffer,
			mPrimitiveBufferStats,
			mStaticMaterialBuffer,
			mMaterialBufferStats,
			mStaticMapChunkAtlas,
			mStaticMapScene,
			mStaticMapScene.gpuMaterials) ||
		!BuildStaticMapAccelerationStructures())
	{
		return false;
	}
	if (!nri_static_scene_geometry::RebuildResidentStaticCpuAtlasMirror(mMapWorld, mStaticMapScene, mStaticMapChunkAtlas) ||
		!nri_static_scene::RebuildResidentStaticMaterialBridgeFromChunks(
			mStaticMapScene,
			mStaticMapChunkAtlas,
			nri_ptscenestats && ShouldTracePtPerf()))
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
	mSkyEnvironment.PreservedStaticMapSky() = {};

	Printf("NRI PT static scene resident: level=%s build_serial=%llu chunks=%u tris=%u materials=%u uploads=%u as_builds=%u\n",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)mStaticMapScene.buildSerial,
		(uint32_t)mStaticMapScene.chunks.size(),
		(uint32_t)mStaticMapScene.geometry.primitives.size(),
		(uint32_t)mStaticMapScene.gpuMaterials.size(),
		mStaticMapScene.gpuUploadCount,
		mStaticMapScene.accelerationBuildCount);
	if (mFrameBuffer != nullptr && mFrameBuffer->mCommandBuffer != nullptr)
	{
		if (!mFrameBuffer->SubmitWaitAndRestartCommandList("static-map-scene-build"))
		{
			return false;
		}
		ReleaseWorldAccelerationBuildScratch("static-map-scene-build");
	}
	return true;
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

NRIRenderer::SceneUploadBufferRingSlot& NRIRenderer::GetCurrentSceneUploadBufferRingSlot()
{
	const uint32_t queuedFrameCount =
		mFrameBuffer != nullptr && !mFrameBuffer->mQueuedFrames.empty() ?
		(uint32_t)mFrameBuffer->mQueuedFrames.size() :
		1u;
	if (mSceneUploadBufferRing.size() < queuedFrameCount)
	{
		mSceneUploadBufferRing.resize(queuedFrameCount);
	}

	return mSceneUploadBufferRing[GetCurrentQueuedFrameIndex() % (uint32_t)mSceneUploadBufferRing.size()];
}

const NRIRenderer::SceneUploadBufferRingSlot* NRIRenderer::GetCurrentSceneUploadBufferRingSlot() const
{
	if (mSceneUploadBufferRing.empty())
	{
		return nullptr;
	}

	return &mSceneUploadBufferRing[GetCurrentQueuedFrameIndex() % (uint32_t)mSceneUploadBufferRing.size()];
}

NRIBufferResource& NRIRenderer::GetCurrentDynamicVertexBuffer()
{
	return GetCurrentSceneUploadBufferRingSlot().vertexBuffer;
}

NRIBufferResource& NRIRenderer::GetCurrentDynamicIndexBuffer()
{
	return GetCurrentSceneUploadBufferRingSlot().indexBuffer;
}

NRIBufferResource& NRIRenderer::GetCurrentDynamicPrimitiveBuffer()
{
	return GetCurrentSceneUploadBufferRingSlot().primitiveBuffer;
}

NRIBufferResource& NRIRenderer::GetCurrentDynamicMaterialBuffer()
{
	return GetCurrentSceneUploadBufferRingSlot().materialBuffer;
}

NRIAccelerationStructureResource& NRIRenderer::GetCurrentDynamicBottomLevelAS()
{
	return GetCurrentSceneUploadBufferRingSlot().dynamicBottomLevelAS;
}

NRIBufferResource& NRIRenderer::GetCurrentTlasInstanceBuffer()
{
	const uint32_t queuedFrameCount =
		mFrameBuffer != nullptr && !mFrameBuffer->mQueuedFrames.empty() ?
		(uint32_t)mFrameBuffer->mQueuedFrames.size() :
		1u;
	if (mTlasInstanceBufferRing.size() < queuedFrameCount)
	{
		mTlasInstanceBufferRing.resize(queuedFrameCount);
	}

	return mTlasInstanceBufferRing[GetCurrentQueuedFrameIndex() % (uint32_t)mTlasInstanceBufferRing.size()];
}

const NRIBufferResource& NRIRenderer::GetCurrentDynamicVertexBuffer() const
{
	const SceneUploadBufferRingSlot* slot = GetCurrentSceneUploadBufferRingSlot();
	return slot != nullptr ? slot->vertexBuffer : mVertexBuffer;
}

const NRIBufferResource& NRIRenderer::GetCurrentDynamicIndexBuffer() const
{
	const SceneUploadBufferRingSlot* slot = GetCurrentSceneUploadBufferRingSlot();
	return slot != nullptr ? slot->indexBuffer : mIndexBuffer;
}

const NRIBufferResource& NRIRenderer::GetCurrentDynamicPrimitiveBuffer() const
{
	const SceneUploadBufferRingSlot* slot = GetCurrentSceneUploadBufferRingSlot();
	return slot != nullptr ? slot->primitiveBuffer : mPrimitiveBuffer;
}

const NRIBufferResource& NRIRenderer::GetCurrentDynamicMaterialBuffer() const
{
	const SceneUploadBufferRingSlot* slot = GetCurrentSceneUploadBufferRingSlot();
	return slot != nullptr ? slot->materialBuffer : mMaterialBuffer;
}

const NRIAccelerationStructureResource* NRIRenderer::GetCurrentDynamicBottomLevelAS() const
{
	const SceneUploadBufferRingSlot* slot = GetCurrentSceneUploadBufferRingSlot();
	return slot != nullptr ? &slot->dynamicBottomLevelAS : nullptr;
}

const NRIBufferResource& NRIRenderer::GetCurrentTlasInstanceBuffer() const
{
	if (mTlasInstanceBufferRing.empty())
	{
		return mTlasInstanceBuffer;
	}

	return mTlasInstanceBufferRing[GetCurrentQueuedFrameIndex() % (uint32_t)mTlasInstanceBufferRing.size()];
}

bool NRIRenderer::HasAnyDynamicBottomLevelAS() const
{
	for (const SceneUploadBufferRingSlot& slot : mSceneUploadBufferRing)
	{
		if (slot.dynamicBottomLevelAS.accelerationStructure != nullptr ||
			slot.dynamicBottomLevelAS.descriptor != nullptr)
		{
			return true;
		}
	}

	return false;
}

void NRIRenderer::DestroyDynamicBottomLevelAccelerationStructures()
{
	for (SceneUploadBufferRingSlot& slot : mSceneUploadBufferRing)
	{
		DestroyAccelerationStructureResource(slot.dynamicBottomLevelAS);
	}
}

NRIRenderer::ResidentUploadScratchFrame& NRIRenderer::GetResidentUploadScratchFrame()
{
	const uint32_t frameSlot = GetCurrentQueuedFrameIndex() % (uint32_t)mResidentUploadScratchFrames.size();
	auto& frameScratch = mResidentUploadScratchFrames[frameSlot];
	if (frameScratch.frameIndex != mFrameIndex)
	{
		for (NRIBufferResource& retired : frameScratch.retiredBuffers)
		{
			DestroyBufferResource(retired);
		}
		frameScratch.retiredBuffers.clear();
		for (NRIAccelerationStructureResource& retired : frameScratch.retiredAccelerationStructures)
		{
			DestroyAccelerationStructureResource(retired);
		}
		frameScratch.retiredAccelerationStructures.clear();
		frameScratch.frameIndex = mFrameIndex;
		frameScratch.vertex.cursor = 0;
		frameScratch.vertex.copySourceActive = false;
		frameScratch.index.cursor = 0;
		frameScratch.index.copySourceActive = false;
		frameScratch.primitive.cursor = 0;
		frameScratch.primitive.copySourceActive = false;
		frameScratch.material.cursor = 0;
		frameScratch.material.copySourceActive = false;
	}

	return frameScratch;
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

	if (event.hasVoxelKeys)
	{
		Printf("NRI PT actor-sprite %s: actor=%d stat=%d pic=%d base_tex=%d resolved_tex=%d pal=%d shade=%d cstat=0x%x cstat2=0x%x noanimate=%s fullbright=%s drawlist=%u tex_ptr=%p voxel_action=%s voxel_mesh_key=0x%llx voxel_mat_key=0x%llx voxel_inst_key=0x%llx voxel_surface_sig=0x%llx\n",
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
			event.resolvedGameTexture,
			event.voxelAction != nullptr ? event.voxelAction : "unknown",
			(unsigned long long)event.voxelMeshKeyHash,
			(unsigned long long)event.voxelMaterialKeyHash,
			(unsigned long long)event.voxelInstanceKeyHash,
			(unsigned long long)event.voxelSurfaceSignature);
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

bool NRIRenderer::StageResidentMaterialUploadRanges(
	const NRIBufferResource& targetBuffer,
	const std::vector<RuntimeMutationResidentUploadRange>& ranges,
	const uint8_t* data,
	uint64_t availableBytes,
	uint32_t& batchCount,
	uint32_t& batchRangeCount,
	uint32_t& barrierCommandCount,
	uint32_t& copyCommandCount)
{
	if (ranges.empty())
	{
		return true;
	}

	if (targetBuffer.buffer == nullptr ||
		data == nullptr ||
		mFrameBuffer == nullptr ||
		mFrameBuffer->mCommandBuffer == nullptr)
	{
		return false;
	}

	constexpr uint64_t kResidentUploadScratchAlignment = 16u;
	auto& frameScratch = GetResidentUploadScratchFrame();
	ResidentBufferUploadScratch& scratch = frameScratch.material;
	uint64_t requiredSize = scratch.cursor;
	for (const RuntimeMutationResidentUploadRange& range : ranges)
	{
		if (range.uploadKind != ResidentUploadKind_Material ||
			range.size == 0 ||
			range.byteOffset > availableBytes ||
			range.size > availableBytes - range.byteOffset ||
			range.byteOffset > targetBuffer.size ||
			range.size > targetBuffer.size - range.byteOffset)
		{
			return false;
		}

		requiredSize =
			(requiredSize + kResidentUploadScratchAlignment - 1u) &
			~(kResidentUploadScratchAlignment - 1u);
		requiredSize += range.size;
	}

	if (!EnsureResidentUploadScratchBuffer(scratch, frameScratch, requiredSize))
	{
		return false;
	}

	struct StagedCopy
	{
		uint64_t targetOffset = 0;
		uint64_t scratchOffset = 0;
		uint64_t size = 0;
		const uint8_t* data = nullptr;
	};

	std::vector<StagedCopy> stagedCopies;
	stagedCopies.reserve(ranges.size());
	uint64_t mapStart = UINT64_MAX;
	uint64_t mapEnd = 0;
	for (const RuntimeMutationResidentUploadRange& range : ranges)
	{
		const uint64_t scratchOffset =
			(scratch.cursor + kResidentUploadScratchAlignment - 1u) &
			~(kResidentUploadScratchAlignment - 1u);
		const uint64_t rangeEnd = scratchOffset + range.size;
		if (rangeEnd > scratch.buffer.size)
		{
			return false;
		}

		scratch.cursor = rangeEnd;
		mapStart = std::min(mapStart, scratchOffset);
		mapEnd = std::max(mapEnd, rangeEnd);
		stagedCopies.push_back({ range.byteOffset, scratchOffset, range.size, data + range.byteOffset });
	}

	const uint64_t mapSize = mapEnd - mapStart;
	void* mapped = mFrameBuffer->mCore.MapBuffer(*scratch.buffer.buffer, mapStart, mapSize);
	if (mapped == nullptr)
	{
		return false;
	}

	for (const StagedCopy& copy : stagedCopies)
	{
		std::memcpy(static_cast<uint8_t*>(mapped) + (copy.scratchOffset - mapStart), copy.data, (size_t)copy.size);
	}
	mFrameBuffer->mCore.UnmapBuffer(*scratch.buffer.buffer);

	batchCount++;
	batchRangeCount += (uint32_t)stagedCopies.size();

	if (!scratch.copySourceActive)
	{
		nri::BufferBarrierDesc sourceBarrier = {};
		sourceBarrier.buffer = scratch.buffer.buffer;
		sourceBarrier.before = {};
		sourceBarrier.after = NRICopySourceAccess();

		nri::BarrierDesc sourceBarrierDesc = {};
		sourceBarrierDesc.buffers = &sourceBarrier;
		sourceBarrierDesc.bufferNum = 1;
		mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, sourceBarrierDesc);
		scratch.copySourceActive = true;
		barrierCommandCount++;
	}

	nri::BufferBarrierDesc beforeCopyBarrier = {};
	beforeCopyBarrier.buffer = targetBuffer.buffer;
	beforeCopyBarrier.before = NRIComputeShaderResourceAccess();
	beforeCopyBarrier.after = NRICopyDestinationAccess();

	nri::BarrierDesc beforeCopyBarrierDesc = {};
	beforeCopyBarrierDesc.buffers = &beforeCopyBarrier;
	beforeCopyBarrierDesc.bufferNum = 1;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, beforeCopyBarrierDesc);
	barrierCommandCount++;

	for (const StagedCopy& copy : stagedCopies)
	{
		mFrameBuffer->mCore.CmdCopyBuffer(
			*mFrameBuffer->mCommandBuffer,
			*targetBuffer.buffer,
			copy.targetOffset,
			*scratch.buffer.buffer,
			copy.scratchOffset,
			copy.size);
		copyCommandCount++;
	}

	nri::BufferBarrierDesc afterCopyBarrier = {};
	afterCopyBarrier.buffer = targetBuffer.buffer;
	afterCopyBarrier.before = NRICopyDestinationAccess();
	afterCopyBarrier.after = NRIComputeShaderResourceAccess();

	nri::BarrierDesc afterCopyBarrierDesc = {};
	afterCopyBarrierDesc.buffers = &afterCopyBarrier;
	afterCopyBarrierDesc.bufferNum = 1;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, afterCopyBarrierDesc);
	barrierCommandCount++;

	return true;
}

void NRIRenderer::RefreshStateCommitCombinedGeometryStaticPrefixForResidentUpdate(const std::vector<uint32_t>& changedGeometryChunkListIndices)
{
	std::vector<NRISceneFrameGeometryStaticChunkSlice> changedChunks;
	changedChunks.reserve(changedGeometryChunkListIndices.size());
	for (uint32_t chunkListIndex : changedGeometryChunkListIndices)
	{
		if (chunkListIndex >= mStaticMapScene.chunks.size())
		{
			NRISceneFrameGeometryStaticPrefixRefresh invalidRefresh = {};
			mSceneFrameGeometry.RefreshStaticPrefixForResidentUpdate(invalidRefresh);
			return;
		}
		const StaticMapSceneCache::ChunkCache& chunk = mStaticMapScene.chunks[chunkListIndex];
		NRISceneFrameGeometryStaticChunkSlice slice = {};
		slice.active = chunk.active;
		slice.vertexOffset = chunk.vertexOffset;
		slice.vertexCount = chunk.vertexCount;
		slice.indexOffset = chunk.indexOffset;
		slice.indexCount = chunk.indexCount;
		slice.primitiveOffset = chunk.primitiveOffset;
		slice.primitiveCount = chunk.primitiveCount;
		changedChunks.push_back(slice);
	}
	NRISceneFrameGeometryStaticPrefixRefresh refresh = {};
	refresh.staticBuildSerial = mStaticMapScene.buildSerial;
	refresh.staticGeometry = &mStaticMapScene.geometry;
	refresh.staticMaterialCount = (uint32_t)mStaticMapScene.materialBridge.materials.size();
	refresh.changedChunks = &changedChunks;
	mSceneFrameGeometry.RefreshStaticPrefixForResidentUpdate(refresh);
}

bool NRIRenderer::UploadSceneBuffers(
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& materials,
	const std::vector<SceneBufferUploadDomainSpan>* domainSpans)
{
	return UploadSceneBuffers(GetCurrentSceneUploadBufferRingSlot(), geometry, materials, domainSpans);
}

bool NRIRenderer::UploadSceneBuffers(
	SceneUploadBufferRingSlot& uploadSlot,
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& materials,
	const std::vector<SceneBufferUploadDomainSpan>* domainSpans)
{
	Clocker clock(NriPTSceneBuffers);
	NRIBufferResource& vertexBuffer = uploadSlot.vertexBuffer;
	NRIBufferResource& indexBuffer = uploadSlot.indexBuffer;
	NRIBufferResource& primitiveBuffer = uploadSlot.primitiveBuffer;
	NRIBufferResource& materialBuffer = uploadSlot.materialBuffer;
	std::vector<uint8_t>& vertexMirror = uploadSlot.vertexMirror;
	std::vector<uint8_t>& indexMirror = uploadSlot.indexMirror;
	std::vector<uint8_t>& primitiveMirror = uploadSlot.primitiveMirror;
	std::vector<uint8_t>& materialMirror = uploadSlot.materialMirror;
	mVertexBufferStats.bytesUploadedLastFrame = 0;
	mVertexBufferStats.growEventsLastFrame = 0;
	mVertexBufferStats.overwriteEventsLastFrame = 0;
	mVertexBufferStats.growthOldBytesLastFrame = 0;
	mVertexBufferStats.growthRequestedBytesLastFrame = 0;
	mVertexBufferStats.growthAllocatedBytesLastFrame = 0;
	mIndexBufferStats.bytesUploadedLastFrame = 0;
	mIndexBufferStats.growEventsLastFrame = 0;
	mIndexBufferStats.overwriteEventsLastFrame = 0;
	mIndexBufferStats.growthOldBytesLastFrame = 0;
	mIndexBufferStats.growthRequestedBytesLastFrame = 0;
	mIndexBufferStats.growthAllocatedBytesLastFrame = 0;
	mPrimitiveBufferStats.bytesUploadedLastFrame = 0;
	mPrimitiveBufferStats.growEventsLastFrame = 0;
	mPrimitiveBufferStats.overwriteEventsLastFrame = 0;
	mPrimitiveBufferStats.growthOldBytesLastFrame = 0;
	mPrimitiveBufferStats.growthRequestedBytesLastFrame = 0;
	mPrimitiveBufferStats.growthAllocatedBytesLastFrame = 0;
	mMaterialBufferStats.bytesUploadedLastFrame = 0;
	mMaterialBufferStats.growEventsLastFrame = 0;
	mMaterialBufferStats.overwriteEventsLastFrame = 0;
	mMaterialBufferStats.growthOldBytesLastFrame = 0;
	mMaterialBufferStats.growthRequestedBytesLastFrame = 0;
	mMaterialBufferStats.growthAllocatedBytesLastFrame = 0;
	{
		mLastPerfShellTraceStats.sceneSelectBufferUploadVertexRequestedBytes = geometry.vertices.size() * sizeof(nri_scene::SceneVertex);
		mLastPerfShellTraceStats.sceneSelectBufferUploadIndexRequestedBytes = geometry.indices.size() * sizeof(uint32_t);
		mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRequestedBytes = geometry.primitives.size() * sizeof(nri_scene::PrimitiveData);
		mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialRequestedBytes = materials.size() * sizeof(nri_scene::MaterialData);
	}
	enum class SceneUploadBufferKind
	{
		Vertex,
		Index,
		Primitive,
		Material
	};
	const auto getDomainEntry = [&](SceneBufferUploadDomain domain) -> PerfShellTraceStats::SceneBufferUploadDomainTraceEntry*
	{
		const size_t domainIndex = (size_t)domain;
		if (domainIndex >= SceneBufferUploadDomainCount)
		{
			return nullptr;
		}
		return &mLastPerfShellTraceStats.sceneSelectBufferUploadDomains[domainIndex];
	};
	const auto getSpanByteRange =
		[](const SceneBufferUploadDomainSpan& span, SceneUploadBufferKind kind, uint64_t& outOffset, uint64_t& outSize)
	{
		switch (kind)
		{
		case SceneUploadBufferKind::Vertex:
			outOffset = (uint64_t)span.vertexOffset * sizeof(nri_scene::SceneVertex);
			outSize = (uint64_t)span.vertexCount * sizeof(nri_scene::SceneVertex);
			break;
		case SceneUploadBufferKind::Index:
			outOffset = (uint64_t)span.indexOffset * sizeof(uint32_t);
			outSize = (uint64_t)span.indexCount * sizeof(uint32_t);
			break;
		case SceneUploadBufferKind::Primitive:
			outOffset = (uint64_t)span.primitiveOffset * sizeof(nri_scene::PrimitiveData);
			outSize = (uint64_t)span.primitiveCount * sizeof(nri_scene::PrimitiveData);
			break;
		case SceneUploadBufferKind::Material:
			outOffset = (uint64_t)span.materialOffset * sizeof(nri_scene::MaterialData);
			outSize = (uint64_t)span.materialCount * sizeof(nri_scene::MaterialData);
			break;
		}
	};
	const auto addDomainPayload =
		[&](SceneUploadBufferKind kind, bool skipped)
	{
		if (domainSpans == nullptr)
		{
			return;
		}
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			if (size == 0)
			{
				continue;
			}
			auto* domain = getDomainEntry(span.domain);
			if (domain == nullptr)
			{
				continue;
			}
			domain->payloadBytes += size;
			domain->hashChecks++;
			if (!skipped)
			{
				domain->hashMisses++;
			}
			switch (kind)
			{
			case SceneUploadBufferKind::Vertex: domain->vertexPayloadBytes += size; break;
			case SceneUploadBufferKind::Index: domain->indexPayloadBytes += size; break;
			case SceneUploadBufferKind::Primitive: domain->primitivePayloadBytes += size; break;
			case SceneUploadBufferKind::Material: domain->materialPayloadBytes += size; break;
			}
		}
	};
	const auto addDomainFullUpload =
		[&](SceneUploadBufferKind kind)
	{
		if (domainSpans == nullptr)
		{
			return;
		}
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			if (size == 0)
			{
				continue;
			}
			auto* domain = getDomainEntry(span.domain);
			if (domain == nullptr)
			{
				continue;
			}
			domain->uploadedBytes += size;
			if (kind == SceneUploadBufferKind::Primitive)
			{
				domain->primitiveUploadedBytes += size;
			}
			else if (kind == SceneUploadBufferKind::Material)
			{
				domain->materialUploadedBytes += size;
			}
		}
	};
	const auto addDomainRangeBytes =
		[&](SceneUploadBufferKind kind, const std::vector<SceneUploadDirtyRange>& ranges, bool countDirty)
	{
		if (domainSpans == nullptr)
		{
			return;
		}
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t spanOffset = 0;
			uint64_t spanSize = 0;
			getSpanByteRange(span, kind, spanOffset, spanSize);
			if (spanSize == 0)
			{
				continue;
			}
			const uint64_t spanEnd = spanOffset + spanSize;
			uint64_t domainBytes = 0;
			uint32_t domainRanges = 0;
			for (const SceneUploadDirtyRange& range : ranges)
			{
				const uint64_t rangeEnd = range.byteOffset + range.size;
				const uint64_t overlapStart = std::max(spanOffset, range.byteOffset);
				const uint64_t overlapEnd = std::min(spanEnd, rangeEnd);
				if (overlapEnd > overlapStart)
				{
					domainBytes += overlapEnd - overlapStart;
					domainRanges++;
				}
			}
			if (domainBytes == 0)
			{
				continue;
			}
			auto* domain = getDomainEntry(span.domain);
			if (domain == nullptr)
			{
				continue;
			}
			if (countDirty)
			{
				domain->dirtyRanges += domainRanges;
				domain->dirtyChangedBytes += domainBytes;
				domain->dirtyUploadedBytes += domainBytes;
			}
			else
			{
				domain->uploadedBytes += domainBytes;
				if (kind == SceneUploadBufferKind::Primitive)
				{
					domain->primitiveUploadedBytes += domainBytes;
				}
				else if (kind == SceneUploadBufferKind::Material)
				{
					domain->materialUploadedBytes += domainBytes;
				}
			}
		}
	};
	const auto addDomainWait =
		[&](SceneUploadBufferKind kind, double waitMs)
	{
		if (domainSpans == nullptr || waitMs <= 0.0)
		{
			return;
		}
		uint64_t totalBytes = 0;
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			totalBytes += size;
		}
		if (totalBytes == 0)
		{
			return;
		}
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			if (size == 0)
			{
				continue;
			}
			auto* domain = getDomainEntry(span.domain);
			if (domain != nullptr)
			{
				domain->waitMs += waitMs * ((double)size / (double)totalBytes);
			}
		}
	};
	const auto addDomainGrowth =
		[&](SceneUploadBufferKind kind, uint64_t requestedBytes, uint64_t allocatedBytes)
	{
		if (domainSpans == nullptr || requestedBytes == 0 || allocatedBytes == 0)
		{
			return;
		}
		uint64_t totalBytes = 0;
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			totalBytes += size;
		}
		if (totalBytes == 0)
		{
			return;
		}
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			if (size == 0)
			{
				continue;
			}
			auto* domain = getDomainEntry(span.domain);
			if (domain != nullptr)
			{
				domain->growthEvents++;
				domain->growthRequestedBytes += (uint64_t)((double)requestedBytes * ((double)size / (double)totalBytes));
				domain->growthAllocatedBytes += (uint64_t)((double)allocatedBytes * ((double)size / (double)totalBytes));
			}
		}
	};
	const auto buildProducerPayloadHash =
		[&](SceneUploadBufferKind kind, uint64_t payloadSize, uint32_t payloadStride, uint64_t extraIdentity, uint64_t& outHash) -> bool
	{
		mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampChecks++;
		if (domainSpans == nullptr)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampFallbacks++;
			return false;
		}
		uint64_t coveredBytes = 0;
		uint64_t hash = 1469598103934665603ull;
		hash = CoherencyHashCombine64(hash, (uint64_t)kind);
		hash = CoherencyHashCombine64(hash, payloadSize);
		hash = CoherencyHashCombine64(hash, payloadStride);
		hash = CoherencyHashCombine64(hash, extraIdentity);
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			if (size == 0)
			{
				continue;
			}
			uint64_t stamp = 0;
			switch (kind)
			{
			case SceneUploadBufferKind::Vertex:
				stamp = span.stamp.vertexPayloadStamp;
				break;
			case SceneUploadBufferKind::Index:
				stamp = span.stamp.indexPayloadStamp;
				break;
			case SceneUploadBufferKind::Primitive:
				stamp = span.stamp.primitivePayloadStamp;
				break;
			case SceneUploadBufferKind::Material:
				stamp = span.stamp.materialPayloadStamp;
				break;
			}
			if (stamp == 0)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampFallbacks++;
				return false;
			}
			coveredBytes += size;
			hash = CoherencyHashCombine64(hash, (uint64_t)span.domain);
			hash = CoherencyHashCombine64(hash, offset);
			hash = CoherencyHashCombine64(hash, size);
			hash = CoherencyHashCombine64(hash, stamp);
		}
		if (coveredBytes != payloadSize)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampFallbacks++;
			return false;
		}
		outHash = hash != 0 ? hash : 1;
		mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampUses++;
		switch (kind)
		{
		case SceneUploadBufferKind::Vertex:
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampVertexUses++;
			break;
		case SceneUploadBufferKind::Index:
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampIndexUses++;
			break;
		case SceneUploadBufferKind::Primitive:
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampPrimitiveUses++;
			break;
		case SceneUploadBufferKind::Material:
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampMaterialUses++;
			break;
		}
		return true;
	};
	const auto buildProducerProvenanceHash =
		[&](uint64_t primitiveCount, uint64_t& outHash) -> bool
	{
		mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampChecks++;
		if (domainSpans == nullptr)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampFallbacks++;
			return false;
		}
		uint64_t coveredPrimitives = 0;
		uint64_t hash = 1469598103934665603ull;
		hash = CoherencyHashCombine64(hash, primitiveCount);
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			if (span.primitiveCount == 0)
			{
				continue;
			}
			if (span.stamp.primitiveProvenanceStamp == 0)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampFallbacks++;
				return false;
			}
			coveredPrimitives += span.primitiveCount;
			hash = CoherencyHashCombine64(hash, (uint64_t)span.domain);
			hash = CoherencyHashCombine64(hash, (uint64_t)span.primitiveOffset);
			hash = CoherencyHashCombine64(hash, (uint64_t)span.primitiveCount);
			hash = CoherencyHashCombine64(hash, span.stamp.primitiveProvenanceStamp);
		}
		if (coveredPrimitives != primitiveCount)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampFallbacks++;
			return false;
		}
		outHash = hash != 0 ? hash : 1;
		mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampUses++;
		return true;
	};
	const uint64_t primitiveInputSize = geometry.primitives.size() * sizeof(nri_scene::PrimitiveData);
	uint64_t primitiveInputPayloadHash = 0;
	uint64_t primitiveProvenanceHash = 0;
	uint64_t primitiveVisibilityIdentityHash = 0;
	const std::vector<nri_scene::PrimitiveData>* gpuPrimitives = nullptr;
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteMs);
		if (buildProducerPayloadHash(SceneUploadBufferKind::Primitive, primitiveInputSize, sizeof(nri_scene::PrimitiveData), 0, primitiveInputPayloadHash))
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampRewritePrimitiveUses++;
		}
		else
		{
			ScopedPtPerfTimer hashTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewritePrimitiveHashMs);
			primitiveInputPayloadHash = HashUploadPayloadBytes(geometry.primitives.data(), primitiveInputSize);
		}
		if (buildProducerProvenanceHash((uint64_t)geometry.primitiveProvenance.size(), primitiveProvenanceHash))
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampRewriteProvenanceUses++;
		}
		else
		{
			ScopedPtPerfTimer hashTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteProvenanceHashMs);
			primitiveProvenanceHash = HashPrimitiveRewriteProvenancePayload(geometry.primitiveProvenance);
		}
		{
			ScopedPtPerfTimer hashTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteVisibilityHashMs);
			primitiveVisibilityIdentityHash = HashPrimitiveRewriteVisibilityIdentity(mMapWorld);
		}
		mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheChecks++;
		if (mSelectPrimitiveRewriteCache.valid &&
			mSelectPrimitiveRewriteCache.primitivePayloadHash == primitiveInputPayloadHash &&
			mSelectPrimitiveRewriteCache.primitiveProvenanceHash == primitiveProvenanceHash &&
			mSelectPrimitiveRewriteCache.visibilityIdentityHash == primitiveVisibilityIdentityHash &&
			mSelectPrimitiveRewriteCache.primitiveCount == geometry.primitives.size() &&
			mSelectPrimitiveRewriteCache.primitives.size() == geometry.primitives.size())
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheHits++;
			gpuPrimitives = &mSelectPrimitiveRewriteCache.primitives;
		}
		else
		{
			if (!mSelectPrimitiveRewriteCache.valid)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheRejectInvalid++;
			}
			else
			{
				if (mSelectPrimitiveRewriteCache.primitivePayloadHash != primitiveInputPayloadHash)
				{
					mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheRejectPrimitive++;
				}
				if (mSelectPrimitiveRewriteCache.primitiveProvenanceHash != primitiveProvenanceHash)
				{
					mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheRejectProvenance++;
				}
				if (mSelectPrimitiveRewriteCache.visibilityIdentityHash != primitiveVisibilityIdentityHash)
				{
					mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheRejectVisibility++;
				}
				if (mSelectPrimitiveRewriteCache.primitiveCount != geometry.primitives.size() ||
					mSelectPrimitiveRewriteCache.primitives.size() != geometry.primitives.size())
				{
					mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheRejectCount++;
				}
			}
			mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheMisses++;
			{
				ScopedPtPerfTimer copyTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCopyMs);
				mSelectPrimitiveRewriteCache.primitives.assign(geometry.primitives.begin(), geometry.primitives.end());
			}
			{
				ScopedPtPerfTimer resolveTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteResolveMs);
				std::vector<nri_scene::PrimitiveData>& rewrittenPrimitives = mSelectPrimitiveRewriteCache.primitives;
				const size_t primitiveCount = std::min(rewrittenPrimitives.size(), geometry.primitiveProvenance.size());
				for (size_t primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
				{
					const nri_scene::SurfaceProvenance& provenance = geometry.primitiveProvenance[primitiveIndex];
					mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteResolvePrimitives++;
					int32_t chunkIndex = provenance.mapChunkIndex;
					if (chunkIndex >= 0)
					{
						mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteResolveMapChunk++;
					}
					else
					{
						mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteResolveSectorFallback++;
						chunkIndex = nri_static_scene_geometry::FindMapChunkIndexForSector(mMapWorld, provenance.sectorIndex);
						if (chunkIndex < 0)
						{
							mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteResolveSectorMiss++;
						}
					}
					rewrittenPrimitives[primitiveIndex].reserved0 = chunkIndex >= 0 ? (uint32_t)chunkIndex : UINT32_MAX;
				}
				for (size_t primitiveIndex = primitiveCount; primitiveIndex < rewrittenPrimitives.size(); ++primitiveIndex)
				{
					rewrittenPrimitives[primitiveIndex].reserved0 = UINT32_MAX;
				}
			}

			{
				ScopedPtPerfTimer storeTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteStoreMs);
				mSelectPrimitiveRewriteCache.valid = true;
				mSelectPrimitiveRewriteCache.primitivePayloadHash = primitiveInputPayloadHash;
				mSelectPrimitiveRewriteCache.primitiveProvenanceHash = primitiveProvenanceHash;
				mSelectPrimitiveRewriteCache.visibilityIdentityHash = primitiveVisibilityIdentityHash;
				mSelectPrimitiveRewriteCache.primitiveCount = geometry.primitives.size();
			}
			gpuPrimitives = &mSelectPrimitiveRewriteCache.primitives;
		}
	}

	const uint64_t vertexSize = geometry.vertices.size() * sizeof(nri_scene::SceneVertex);
	const uint64_t indexSize = geometry.indices.size() * sizeof(uint32_t);
	const uint64_t primitiveSize = gpuPrimitives != nullptr ? gpuPrimitives->size() * sizeof(nri_scene::PrimitiveData) : 0;
	const uint64_t materialSize = materials.size() * sizeof(nri_scene::MaterialData);
	uint64_t vertexPayloadHash = 0;
	uint64_t indexPayloadHash = 0;
	uint64_t primitivePayloadHash = 0;
	uint64_t materialPayloadHash = 0;
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMs);
		if (!buildProducerPayloadHash(SceneUploadBufferKind::Vertex, vertexSize, sizeof(nri_scene::SceneVertex), 0, vertexPayloadHash))
		{
			vertexPayloadHash = HashUploadPayloadBytes(geometry.vertices.data(), vertexSize);
		}
		if (!buildProducerPayloadHash(SceneUploadBufferKind::Index, indexSize, sizeof(uint32_t), 0, indexPayloadHash))
		{
			indexPayloadHash = HashUploadPayloadBytes(geometry.indices.data(), indexSize);
		}
		if (!buildProducerPayloadHash(SceneUploadBufferKind::Primitive, primitiveSize, sizeof(nri_scene::PrimitiveData), primitiveVisibilityIdentityHash, primitivePayloadHash))
		{
			primitivePayloadHash = HashUploadPayloadBytes(gpuPrimitives != nullptr && !gpuPrimitives->empty() ? gpuPrimitives->data() : nullptr, primitiveSize);
		}
		if (!buildProducerPayloadHash(SceneUploadBufferKind::Material, materialSize, sizeof(nri_scene::MaterialData), 0, materialPayloadHash))
		{
			materialPayloadHash = HashUploadPayloadBytes(materials.data(), materialSize);
		}
	}

	// Scene upload buffers are ringed by queued frame. The frame shell waits before
	// reusing a queued-frame slot, so the selected slot is safe to overwrite here.
	bool waitedForWrites = true;
	const auto notePayloadHashState =
		[&](const NRIBufferResource& resource,
			uint64_t payloadHash,
			uint64_t payloadSize,
			uint32_t payloadStride,
			uint32_t& bufferHitCount,
			uint32_t& bufferSkipCount,
			uint32_t& bufferMissCount) -> bool
	{
		mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashChecks++;
		if (resource.buffer == nullptr || resource.shaderView == nullptr || resource.payloadHash == 0)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMisses++;
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashRejectMissing++;
			bufferMissCount++;
			return false;
		}
		const uint64_t requiredSize = std::max<uint64_t>(payloadSize, payloadStride);
		if (resource.size < requiredSize ||
			resource.usedSize != payloadSize ||
			resource.payloadSize != payloadSize)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMisses++;
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashRejectSize++;
			bufferMissCount++;
			return false;
		}
		if (resource.stride != payloadStride ||
			resource.payloadStride != payloadStride)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMisses++;
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashRejectStride++;
			bufferMissCount++;
			return false;
		}
		if (resource.payloadHash == payloadHash)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashHits++;
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashSkips++;
			bufferHitCount++;
			bufferSkipCount++;
			return true;
		}

		mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMisses++;
		bufferMissCount++;
		return false;
	};
	struct SceneUploadDirtyRangeStats
	{
		uint32_t rawRanges = 0;
		uint32_t coalescedRanges = 0;
		uint32_t rejectedCoalesces = 0;
		uint64_t changedBytes = 0;
		uint64_t uploadedBytes = 0;
		uint64_t gapBytes = 0;
	};
	const auto noteDirtyRanges =
		[&](const std::vector<uint8_t>& mirror,
			const void* bufferData,
			uint64_t bufferSize,
			bool skipUpload,
			bool forceFullDirty,
			std::vector<SceneUploadDirtyRange>* outRanges) -> SceneUploadDirtyRangeStats
	{
		if (outRanges != nullptr)
		{
			outRanges->clear();
		}
		SceneUploadDirtyRangeStats result = {};
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeChecks++;
		if (skipUpload)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeSkips++;
			return result;
		}
		if (bufferSize == 0)
		{
			return result;
		}
		if (forceFullDirty || bufferData == nullptr || mirror.empty() || mirror.size() != bufferSize)
		{
			if (forceFullDirty || bufferData == nullptr)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeForcedFull++;
			}
			if (mirror.empty())
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeMissingMirror++;
			}
			else if (mirror.size() != bufferSize)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeSizeMismatch++;
			}
			result.rawRanges = 1;
			result.coalescedRanges = 1;
			result.changedBytes = bufferSize;
			result.uploadedBytes = bufferSize;
			if (outRanges != nullptr)
			{
				outRanges->push_back({ 0, bufferSize });
			}
			return result;
		}

		const uint64_t maxGapBytes = (uint64_t)(int)nri_ptscenebufferdirtyrangegap;
		const uint8_t* current = static_cast<const uint8_t*>(bufferData);
		const uint8_t* previous = mirror.data();
		const size_t byteCount = (size_t)bufferSize;
		bool hasCoalescedRange = false;
		size_t coalescedStart = 0;
		size_t coalescedEnd = 0;
		size_t cursor = 0;
		while (cursor < byteCount)
		{
			while (cursor < byteCount && current[cursor] == previous[cursor])
			{
				cursor++;
			}
			if (cursor >= byteCount)
			{
				break;
			}
			const size_t rangeStart = cursor;
			while (cursor < byteCount && current[cursor] != previous[cursor])
			{
				cursor++;
			}
			const size_t rangeEnd = cursor;
			result.rawRanges++;
			result.changedBytes += (uint64_t)(rangeEnd - rangeStart);
			if (!hasCoalescedRange)
			{
				hasCoalescedRange = true;
				coalescedStart = rangeStart;
				coalescedEnd = rangeEnd;
				result.coalescedRanges = 1;
				continue;
			}

			const uint64_t gapBytes = (uint64_t)(rangeStart - coalescedEnd);
			if (gapBytes <= maxGapBytes)
			{
				result.gapBytes += gapBytes;
				coalescedEnd = rangeEnd;
			}
			else
			{
				result.uploadedBytes += (uint64_t)(coalescedEnd - coalescedStart);
				if (outRanges != nullptr)
				{
					outRanges->push_back({ (uint64_t)coalescedStart, (uint64_t)(coalescedEnd - coalescedStart) });
				}
				result.rejectedCoalesces++;
				result.coalescedRanges++;
				coalescedStart = rangeStart;
				coalescedEnd = rangeEnd;
			}
		}
		if (hasCoalescedRange)
		{
			result.uploadedBytes += (uint64_t)(coalescedEnd - coalescedStart);
			if (outRanges != nullptr)
			{
				outRanges->push_back({ (uint64_t)coalescedStart, (uint64_t)(coalescedEnd - coalescedStart) });
			}
		}
		return result;
	};
	const auto addDirtyRangeStats =
		[&](const SceneUploadDirtyRangeStats& dirtyStats,
			uint32_t& bufferRangeCount,
			uint64_t& bufferChangedBytes,
			uint64_t& bufferUploadedBytes)
	{
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeRawRanges += dirtyStats.rawRanges;
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeCoalescedRanges += dirtyStats.coalescedRanges;
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeRejectedCoalesces += dirtyStats.rejectedCoalesces;
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeChangedBytes += dirtyStats.changedBytes;
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeUploadedBytes += dirtyStats.uploadedBytes;
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeGapBytes += dirtyStats.gapBytes;
		bufferRangeCount = dirtyStats.coalescedRanges;
		bufferChangedBytes = dirtyStats.changedBytes;
		bufferUploadedBytes = dirtyStats.uploadedBytes;
	};
	const auto updatePayloadMirror =
		[](std::vector<uint8_t>& mirror, const void* bufferData, uint64_t bufferSize)
	{
		if (bufferData == nullptr || bufferSize == 0)
		{
			mirror.clear();
			return;
		}
		const uint8_t* bytes = static_cast<const uint8_t*>(bufferData);
		mirror.assign(bytes, bytes + (size_t)bufferSize);
	};
	const auto updatePayloadMirrorRanges =
		[](std::vector<uint8_t>& mirror, const void* bufferData, const std::vector<SceneUploadDirtyRange>& ranges)
	{
		if (bufferData == nullptr || mirror.empty())
		{
			return;
		}
		const uint8_t* bytes = static_cast<const uint8_t*>(bufferData);
		for (const SceneUploadDirtyRange& range : ranges)
		{
			if (range.size != 0 && range.byteOffset <= mirror.size() && range.size <= mirror.size() - range.byteOffset)
			{
				std::memcpy(mirror.data() + range.byteOffset, bytes + range.byteOffset, (size_t)range.size);
			}
		}
	};
	const auto updateStructuredBufferRanges =
		[&](NRIBufferResource& resource,
			SceneBufferDebugStats& stats,
			std::vector<uint8_t>& payloadMirror,
			const void* bufferData,
			uint64_t bufferSize,
			uint32_t bufferStride,
			uint64_t payloadHash,
			const std::vector<SceneUploadDirtyRange>& ranges,
			nri::AccessStage afterAccess,
			double& uploadMs,
			uint64_t& uploadedBytes,
			uint32_t& growEvents,
			uint32_t& overwriteEvents,
			uint32_t& bufferRangeUploadCount) -> bool
	{
		const uint8_t* bytes = static_cast<const uint8_t*>(bufferData);
		uint64_t rangeBytes = 0;
		for (const SceneUploadDirtyRange& range : ranges)
		{
			if (bytes == nullptr ||
				range.size == 0 ||
				range.byteOffset > bufferSize ||
				range.size > bufferSize - range.byteOffset ||
				range.byteOffset > resource.size ||
				range.size > resource.size - range.byteOffset)
			{
				return false;
			}
			rangeBytes += range.size;
		}

		bool result = true;
		{
			ScopedPtPerfTimer perfTimer(uploadMs);
			for (const SceneUploadDirtyRange& range : ranges)
			{
				void* mapped = mFrameBuffer->mCore.MapBuffer(*resource.buffer, range.byteOffset, range.size);
				if (mapped == nullptr)
				{
					result = false;
					break;
				}
				std::memcpy(mapped, bytes + range.byteOffset, (size_t)range.size);
				mFrameBuffer->mCore.UnmapBuffer(*resource.buffer);
			}

			if (result && mFrameBuffer->mCommandBuffer != nullptr && afterAccess.access != nri::AccessBits::NONE)
			{
				nri::BufferBarrierDesc barrier = {};
				barrier.buffer = resource.buffer;
				barrier.before = {};
				barrier.after = afterAccess;

				nri::BarrierDesc barrierDesc = {};
				barrierDesc.buffers = &barrier;
				barrierDesc.bufferNum = 1;
				mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrierDesc);
			}
		}
		if (!result)
		{
			return false;
		}

		stats.bytesUploadedLastFrame = rangeBytes;
		stats.growEventsLastFrame = 0;
		stats.overwriteEventsLastFrame = 1;
		stats.growthOldBytesLastFrame = 0;
		stats.growthRequestedBytesLastFrame = 0;
		stats.growthAllocatedBytesLastFrame = 0;
		stats.uploadCount++;
		stats.overwriteCount++;
		stats.peakUsedBytes = std::max(stats.peakUsedBytes, bufferSize);
		NotePerfBufferUpload(&stats, rangeBytes, false, "scene_buffer_upload_range", -1);

		resource.usedSize = bufferSize;
		resource.payloadHash = payloadHash;
		resource.payloadSize = bufferSize;
		resource.payloadStride = bufferStride;
		updatePayloadMirrorRanges(payloadMirror, bufferData, ranges);

		uploadedBytes = rangeBytes;
		growEvents = 0;
		overwriteEvents = 1;
		mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashUploads++;
		mLastPerfShellTraceStats.sceneSelectBufferUploadRangeUploads++;
		mLastPerfShellTraceStats.sceneSelectBufferUploadRangeUploadedBytes += rangeBytes;
		bufferRangeUploadCount++;
		return true;
	};
	const auto ensureStructuredBufferBatched =
		[&](NRIBufferResource& resource,
			SceneBufferDebugStats& stats,
			std::vector<uint8_t>& payloadMirror,
			std::vector<SceneUploadDirtyRange>* dirtyRangeScratch,
			const void* bufferData,
			uint64_t bufferSize,
			uint32_t bufferStride,
			uint64_t payloadHash,
			bool skipUpload,
			bool allowRangeUpload,
			nri::BufferUsageBits usageBits,
			nri::AccessStage afterAccess,
			double& uploadMs,
			uint64_t& uploadedBytes,
			uint32_t& growEvents,
			uint32_t& overwriteEvents,
			uint32_t& dirtyRanges,
			uint64_t& dirtyChangedBytes,
			uint64_t& dirtyUploadedBytes,
			uint32_t& bufferRangeUploadCount,
			SceneUploadBufferKind bufferKind) -> bool
	{
		const uint64_t requiredSize = std::max<uint64_t>(bufferSize, bufferStride);
		const bool forceFullDirty =
			resource.buffer == nullptr ||
			resource.shaderView == nullptr ||
			resource.memoryLocation != nri::MemoryLocation::DEVICE_UPLOAD ||
			resource.payloadHash == 0 ||
			resource.size < requiredSize ||
			resource.usedSize != bufferSize ||
			resource.payloadSize != bufferSize ||
			resource.stride != bufferStride ||
			resource.payloadStride != bufferStride;
		const bool traceDirtyRanges = ShouldTraceSceneBufferDirtyRanges();
		SceneUploadDirtyRangeStats dirtyStats = {};
		const bool collectDirtyRanges = allowRangeUpload && (traceDirtyRanges || (!skipUpload && !forceFullDirty));
		if (!collectDirtyRanges)
		{
			if (dirtyRangeScratch != nullptr)
			{
				dirtyRangeScratch->clear();
			}
			dirtyRanges = 0;
			dirtyChangedBytes = 0;
			dirtyUploadedBytes = 0;
		}
		else
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeMs);
			dirtyStats = noteDirtyRanges(payloadMirror, bufferData, bufferSize, skipUpload, forceFullDirty, dirtyRangeScratch);
			addDirtyRangeStats(dirtyStats, dirtyRanges, dirtyChangedBytes, dirtyUploadedBytes);
			if (dirtyRangeScratch != nullptr)
			{
				addDomainRangeBytes(bufferKind, *dirtyRangeScratch, true);
			}
		}

		if (skipUpload)
		{
			resource.usedSize = bufferSize;
			uploadedBytes = 0;
			growEvents = 0;
			overwriteEvents = 0;
			return true;
		}

		const bool canRangeUpload =
			allowRangeUpload &&
			!forceFullDirty &&
			dirtyRangeScratch != nullptr &&
			!dirtyRangeScratch->empty() &&
			dirtyStats.uploadedBytes != 0 &&
			dirtyStats.uploadedBytes < bufferSize;
		bool useRangeUpload = false;
		if (canRangeUpload)
		{
			const uint32_t maxRangeCount = (uint32_t)(int)nri_ptscenebufferrangeuploadmaxranges;
			const uint32_t maxUploadPercent = (uint32_t)(int)nri_ptscenebufferrangeuploadmaxpercent;
			if (dirtyStats.coalescedRanges > maxRangeCount)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadRangeFallbacks++;
				mLastPerfShellTraceStats.sceneSelectBufferUploadRangeFallbackFragmented++;
			}
			else if (dirtyStats.uploadedBytes * 100u >= bufferSize * maxUploadPercent)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadRangeFallbacks++;
				mLastPerfShellTraceStats.sceneSelectBufferUploadRangeFallbackLarge++;
			}
			else
			{
				useRangeUpload = true;
			}
		}
		else if (allowRangeUpload && !forceFullDirty && dirtyRangeScratch != nullptr && dirtyRangeScratch->empty() && dirtyStats.changedBytes == 0)
		{
			resource.usedSize = bufferSize;
			resource.payloadHash = payloadHash;
			resource.payloadSize = bufferSize;
			resource.payloadStride = bufferStride;
			uploadedBytes = 0;
			growEvents = 0;
			overwriteEvents = 0;
			return true;
		}

		bool needsWait = false;
		if (!waitedForWrites)
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadWaitCheckMs);
			needsWait = StructuredBufferUpdateNeedsWait(resource, bufferData, bufferSize, bufferStride);
		}
		if (!waitedForWrites && needsWait)
		{
			const auto waitStart = std::chrono::steady_clock::now();
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadWaitMs);
			mLastPerfShellTraceStats.sceneSelectBufferUploadWaitCount++;
			WaitForCommandsTracked("scene_buffer_upload");
			addDomainWait(bufferKind, DurationMs(waitStart, std::chrono::steady_clock::now()));
			waitedForWrites = true;
		}

		if (useRangeUpload)
		{
			if (updateStructuredBufferRanges(resource, stats, payloadMirror, bufferData, bufferSize, bufferStride, payloadHash, *dirtyRangeScratch, afterAccess, uploadMs, uploadedBytes, growEvents, overwriteEvents, bufferRangeUploadCount))
			{
				addDomainRangeBytes(bufferKind, *dirtyRangeScratch, false);
				return true;
			}
			mLastPerfShellTraceStats.sceneSelectBufferUploadRangeFallbacks++;
		}

		bool result = false;
		{
			ScopedPtPerfTimer perfTimer(uploadMs);
			result = EnsureStructuredBuffer(resource, stats, bufferData, bufferSize, bufferStride, usageBits, afterAccess, waitedForWrites, "scene_buffer_upload");
		}
		uploadedBytes = stats.bytesUploadedLastFrame;
		growEvents = stats.growEventsLastFrame;
		overwriteEvents = stats.overwriteEventsLastFrame;
		if (result)
		{
			if (stats.growEventsLastFrame != 0)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadGrowthEvents += stats.growEventsLastFrame;
				mLastPerfShellTraceStats.sceneSelectBufferUploadGrowthOldBytes += stats.growthOldBytesLastFrame;
				mLastPerfShellTraceStats.sceneSelectBufferUploadGrowthRequestedBytes += stats.growthRequestedBytesLastFrame;
				mLastPerfShellTraceStats.sceneSelectBufferUploadGrowthAllocatedBytes += stats.growthAllocatedBytesLastFrame;
				if (stats.growthAllocatedBytesLastFrame > stats.growthRequestedBytesLastFrame)
				{
					mLastPerfShellTraceStats.sceneSelectBufferUploadGrowthHeadroomBytes +=
						stats.growthAllocatedBytesLastFrame - stats.growthRequestedBytesLastFrame;
				}
				addDomainGrowth(bufferKind, stats.growthRequestedBytesLastFrame, stats.growthAllocatedBytesLastFrame);
			}
			resource.payloadHash = payloadHash;
			resource.payloadSize = bufferSize;
			resource.payloadStride = bufferStride;
			updatePayloadMirror(payloadMirror, bufferData, bufferSize);
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashUploads++;
			addDomainFullUpload(bufferKind);
		}
		return result;
	};

	const bool skipVertexUpload = notePayloadHashState(vertexBuffer, vertexPayloadHash, vertexSize, sizeof(nri_scene::SceneVertex), mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashVertexHits, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashVertexSkips, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashVertexMisses);
	const bool skipIndexUpload = notePayloadHashState(indexBuffer, indexPayloadHash, indexSize, sizeof(uint32_t), mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashIndexHits, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashIndexSkips, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashIndexMisses);
	const bool skipPrimitiveUpload = notePayloadHashState(primitiveBuffer, primitivePayloadHash, primitiveSize, sizeof(nri_scene::PrimitiveData), mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashPrimitiveHits, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashPrimitiveSkips, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashPrimitiveMisses);
	const bool skipMaterialUpload = notePayloadHashState(materialBuffer, materialPayloadHash, materialSize, sizeof(nri_scene::MaterialData), mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMaterialHits, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMaterialSkips, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMaterialMisses);
	addDomainPayload(SceneUploadBufferKind::Vertex, skipVertexUpload);
	addDomainPayload(SceneUploadBufferKind::Index, skipIndexUpload);
	addDomainPayload(SceneUploadBufferKind::Primitive, skipPrimitiveUpload);
	addDomainPayload(SceneUploadBufferKind::Material, skipMaterialUpload);
	uint32_t ignoredRangeUploadCount = 0;

	return
		ensureStructuredBufferBatched(vertexBuffer, mVertexBufferStats, vertexMirror, nullptr, geometry.vertices.data(), vertexSize, sizeof(nri_scene::SceneVertex), vertexPayloadHash, skipVertexUpload, false, NRIFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), NRIAccelerationStructureBuildInputAccess(), mLastPerfShellTraceStats.sceneSelectBufferUploadVertexMs, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexGrowEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexOverwriteEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexDirtyRanges, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexDirtyChangedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexDirtyUploadedBytes, ignoredRangeUploadCount, SceneUploadBufferKind::Vertex) &&
		ensureStructuredBufferBatched(indexBuffer, mIndexBufferStats, indexMirror, nullptr, geometry.indices.data(), indexSize, sizeof(uint32_t), indexPayloadHash, skipIndexUpload, false, NRIFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), NRIAccelerationStructureBuildInputAccess(), mLastPerfShellTraceStats.sceneSelectBufferUploadIndexMs, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexGrowEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexOverwriteEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexDirtyRanges, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexDirtyChangedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexDirtyUploadedBytes, ignoredRangeUploadCount, SceneUploadBufferKind::Index) &&
		ensureStructuredBufferBatched(primitiveBuffer, mPrimitiveBufferStats, primitiveMirror, &mSceneUploadPrimitiveDirtyRangeScratch, gpuPrimitives != nullptr && !gpuPrimitives->empty() ? gpuPrimitives->data() : nullptr, primitiveSize, sizeof(nri_scene::PrimitiveData), primitivePayloadHash, skipPrimitiveUpload, true, nri::BufferUsageBits::SHADER_RESOURCE, NRIComputeShaderResourceAccess(), mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveMs, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveGrowEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveOverwriteEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveDirtyRanges, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveDirtyChangedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveDirtyUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRangeUploads, SceneUploadBufferKind::Primitive) &&
		ensureStructuredBufferBatched(materialBuffer, mMaterialBufferStats, materialMirror, &mSceneUploadMaterialDirtyRangeScratch, materials.data(), materialSize, sizeof(nri_scene::MaterialData), materialPayloadHash, skipMaterialUpload, true, nri::BufferUsageBits::SHADER_RESOURCE, NRIComputeShaderResourceAccess(), mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialMs, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialGrowEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialOverwriteEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialDirtyRanges, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialDirtyChangedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialDirtyUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialRangeUploads, SceneUploadBufferKind::Material);
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
			nri_scene::AppendMaterialBridge(sphere.materialBridge, outMaterials);
		}
	}

	mLastPerfShellTraceStats.runtimeDebugSpherePrimitiveCount = (uint32_t)outGeometry.primitives.size();
	mLastPerfShellTraceStats.runtimeDebugSphereMaterialCount = (uint32_t)outMaterials.materials.size();

	return !outGeometry.primitives.empty() && !outMaterials.materials.empty();
}

bool NRIRenderer::BuildSurfaceLightOverlay(nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials)
{
	outGeometry = {};
	outMaterials = {};

	const ResolvedLightOverlaySet& resolved = GetResolvedLightOverlaySet();
	if (resolved.surfaceLightRules.Size() == 0)
	{
		return false;
	}

	auto resolveFixtureTexture = [](const ResolvedLightOverlaySurfaceLightRule& rule) -> FGameTexture*
	{
		if (rule.hasFixtureTexture && rule.fixtureTexture.IsNotEmpty())
		{
			FTextureID textureId = TexMan.CheckForTexture(
				rule.fixtureTexture.GetChars(),
				ETextureType::Any,
				FTextureManager::TEXMAN_TryAny | FTextureManager::TEXMAN_ForceLookup);
			if (textureId.isValid())
			{
				if (FGameTexture* texture = TexMan.GetGameTexture(textureId, true))
				{
					return texture;
				}
			}
		}
		return nullptr;
	};

	auto normalize3 = [](float vector[3]) -> bool
	{
		const float lengthSq = vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2];
		if (!std::isfinite(lengthSq) || lengthSq <= 0.000001f)
		{
			return false;
		}
		const float invLength = 1.0f / std::sqrt(lengthSq);
		vector[0] *= invLength;
		vector[1] *= invLength;
		vector[2] *= invLength;
		return true;
	};

	auto cross3 = [](const float a[3], const float b[3], float out[3])
	{
		out[0] = a[1] * b[2] - a[2] * b[1];
		out[1] = a[2] * b[0] - a[0] * b[2];
		out[2] = a[0] * b[1] - a[1] * b[0];
	};

	nri_scene::SceneView surfaceLightView = {};
	for (const auto& rule : resolved.surfaceLightRules)
	{
		if (!rule.hasPosition || !rule.hasNormal)
		{
			continue;
		}

		float normal[3] = { rule.normal[0], rule.normal[1], rule.normal[2] };
		if (!normalize3(normal))
		{
			continue;
		}

		const float worldUp[3] = { 0.0f, 1.0f, 0.0f };
		const float worldRight[3] = { 1.0f, 0.0f, 0.0f };
		float tangent[3] = {};
		cross3(worldUp, normal, tangent);
		if (!normalize3(tangent))
		{
			cross3(worldRight, normal, tangent);
			if (!normalize3(tangent))
			{
				continue;
			}
		}

		float bitangent[3] = {};
		cross3(normal, tangent, bitangent);
		if (!normalize3(bitangent))
		{
			continue;
		}
		if (rule.hasRotation && std::isfinite(rule.rotation) && rule.rotation != 0.0f)
		{
			const float radians = rule.rotation * 0.017453292519943295f;
			const float c = std::cos(radians);
			const float s = std::sin(radians);
			const float rotatedTangent[3] =
			{
				tangent[0] * c + bitangent[0] * s,
				tangent[1] * c + bitangent[1] * s,
				tangent[2] * c + bitangent[2] * s,
			};
			const float rotatedBitangent[3] =
			{
				bitangent[0] * c - tangent[0] * s,
				bitangent[1] * c - tangent[1] * s,
				bitangent[2] * c - tangent[2] * s,
			};
			tangent[0] = rotatedTangent[0];
			tangent[1] = rotatedTangent[1];
			tangent[2] = rotatedTangent[2];
			bitangent[0] = rotatedBitangent[0];
			bitangent[1] = rotatedBitangent[1];
			bitangent[2] = rotatedBitangent[2];
		}

		const float offset = rule.hasOffset ? std::max(0.0f, rule.offset) : 0.5f;
		const float halfWidth = std::max(1.0f, rule.hasSize ? rule.size[0] : 32.0f) * 0.5f;
		const float halfHeight = std::max(1.0f, rule.hasSize ? rule.size[1] : 32.0f) * 0.5f;
		const float center[3] =
		{
			rule.position[0] + normal[0] * offset,
			rule.position[1] + normal[1] * offset,
			rule.position[2] + normal[2] * offset,
		};

		auto makeVertex = [&](float tangentScale, float bitangentScale, float u, float v) -> nri_scene::CapturedVertex
		{
			nri_scene::CapturedVertex vertex = {};
			vertex.position[0] = center[0] + tangent[0] * tangentScale + bitangent[0] * bitangentScale;
			vertex.position[1] = center[1] + tangent[1] * tangentScale + bitangent[1] * bitangentScale;
			vertex.position[2] = center[2] + tangent[2] * tangentScale + bitangent[2] * bitangentScale;
			vertex.prevPosition[0] = vertex.position[0];
			vertex.prevPosition[1] = vertex.position[1];
			vertex.prevPosition[2] = vertex.position[2];
			vertex.uv[0] = u;
			vertex.uv[1] = v;
			return vertex;
		};

		nri_scene::SurfaceRef surface = {};
		surface.vertices.reserve(6u);
		const nri_scene::CapturedVertex v00 = makeVertex(-halfWidth, -halfHeight, 0.0f, 1.0f);
		const nri_scene::CapturedVertex v10 = makeVertex(halfWidth, -halfHeight, 1.0f, 1.0f);
		const nri_scene::CapturedVertex v11 = makeVertex(halfWidth, halfHeight, 1.0f, 0.0f);
		const nri_scene::CapturedVertex v01 = makeVertex(-halfWidth, halfHeight, 0.0f, 0.0f);
		surface.vertices.push_back(v00);
		surface.vertices.push_back(v10);
		surface.vertices.push_back(v11);
		surface.vertices.push_back(v00);
		surface.vertices.push_back(v11);
		surface.vertices.push_back(v01);
		surface.material.texture = resolveFixtureTexture(rule);
		surface.material.emissiveSourceTexture = surface.material.texture;
		surface.material.palette = 0;
		surface.material.shade = 0;
		surface.material.alpha = 1.0f;
		surface.material.flags = nri_scene::MaterialFlag_Fullbright | nri_scene::MaterialFlag_Flat;
		surface.provenance.sourceType = nri_scene::SurfaceSourceType::SurfaceLightOverlay;
		surface.provenance.sectorIndex = rule.hasSector ? rule.sector : -1;
		surface.provenance.wallIndex = rule.hasWall ? rule.wall : -1;
		surface.provenance.mapChunkIndex = -1;
		surface.provenance.cstat = BuildSurfaceLightRuleId(rule);
		surfaceLightView.opaqueFlats.push_back(std::move(surface));
	}

	if (surfaceLightView.opaqueFlats.empty())
	{
		return false;
	}

	RebuildSceneViewStats(surfaceLightView);
	nri_scene::BuildGeometry(surfaceLightView, outGeometry);
	nri_scene::BuildMaterials(surfaceLightView, outMaterials);
	for (size_t i = 0; i < outMaterials.materials.size(); ++i)
	{
		nri_scene::MaterialData& material = outMaterials.materials[i];
		material.flags |= nri_scene::MaterialFlag_Fullbright | nri_scene::MaterialFlag_Flat;
		material.lightingFlags |=
			nri_scene::MaterialLightingFlag_MaterialFullbright |
			nri_scene::MaterialLightingFlag_NoShadowReceive |
			nri_scene::MaterialLightingFlag_NoShadowCast;
		material.lightLevel = 1.0f;
		material.emissiveMode = nri_scene::MaterialEmissiveMode_UseBaseTexture;
		material.emissiveTextureIndex = material.textureIndex;
		material.emissiveIntensity = 1.0f;
		material.emissiveColor[0] = 1.0f;
		material.emissiveColor[1] = 1.0f;
		material.emissiveColor[2] = 1.0f;
		if (i < outMaterials.lightMetadata.size())
		{
			nri_scene::MaterialLightingMetadata& metadata = outMaterials.lightMetadata[i];
			metadata.materialFlags = material.flags;
			metadata.lightingFlags |=
				nri_scene::MaterialLightingFlag_MaterialFullbright |
				nri_scene::MaterialLightingFlag_NoShadowReceive |
				nri_scene::MaterialLightingFlag_NoShadowCast;
			metadata.lightLevel = 1.0f;
			metadata.emissiveMode = nri_scene::MaterialEmissiveMode_None;
			metadata.emissiveTextureIndex = UINT32_MAX;
			metadata.emissiveIntensity = 0.0f;
			metadata.emissiveColor[0] = 1.0f;
			metadata.emissiveColor[1] = 1.0f;
			metadata.emissiveColor[2] = 1.0f;
		}
	}

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
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildDebugSphereMs);
		nri_scene::BuildGeometry(sphereView, sphere.geometry);
	}
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

bool NRIRenderer::BuildDynamicAccelerationStructure(const nri_scene::GeometryData& geometry)
{
	return NRIAccelerationStructureManager::BuildDynamic(*this, geometry);
}

bool NRIRenderer::BuildDynamicAccelerationStructure(
	const nri_scene::GeometryData& geometry,
	uint32_t indexOffset,
	uint32_t indexCount,
	uint32_t primitiveCount,
	NRIAccelerationStructureResource& outAccelerationStructure,
	bool updateDynamicPerfStats)
{
	return NRIAccelerationStructureManager::BuildDynamic(*this, geometry, indexOffset, indexCount, primitiveCount, outAccelerationStructure, updateDynamicPerfStats);
}

bool NRIRenderer::BuildBottomLevelAccelerationStructure(
	const NRIBufferResource& vertexBuffer,
	const NRIBufferResource& indexBuffer,
	uint32_t vertexCount,
	uint32_t indexOffset,
	uint32_t indexCount,
	uint32_t primitiveCount,
	NRIAccelerationStructureResource& outAccelerationStructure,
	bool updateDynamicPerfStats)
{
	return NRIAccelerationStructureManager::BuildBottomLevel(
		*this,
		vertexBuffer,
		indexBuffer,
		vertexCount,
		indexOffset,
		indexCount,
		primitiveCount,
		outAccelerationStructure,
		updateDynamicPerfStats);
}

bool NRIRenderer::BuildEmissiveTopLevelAccelerationStructure()
{
	return NRIAccelerationStructureManager::BuildEmissiveTopLevel(*this);
}

bool NRIRenderer::BuildTopLevelAccelerationStructure(const std::vector<nri::TopLevelInstance>& instances, uint32_t sceneBufferMask)
{
	return NRIAccelerationStructureManager::BuildTopLevel(*this, instances, sceneBufferMask);
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
	bool updateLiveState,
	bool tlasInstanceWritesQuiesced)
{
	return NRIAccelerationStructureManager::BuildTopLevel(
		*this,
		instances,
		sceneBufferMask,
		topLevelAS,
		tlasInstanceBuffer,
		topLevelScratchBuffer,
		staticVertexBuffer,
		staticIndexBuffer,
		outTlasInstanceCount,
		updateLiveState,
		tlasInstanceWritesQuiesced);
}

bool NRIRenderer::DispatchFrameGraph(HWDrawInfo& di, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials, int)
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.frameGraphMs);
	Clocker clock(NriPTFrameGraph);

	const int ptDebugMode = (int)GetEffectivePtDebugMode();
	NRIFrameGraphExecutionRequest request = {};
	request.ptDebugMode = ptDebugMode;
	request.denoise = !!nri_denoise;
	request.presentRoute = ResolvePresentRouteInfo((uint32_t)ptDebugMode, !!nri_ptbootstrap);
	return ExecuteNRIFrameGraph(*this, di, geometry, materials, request);
}

bool NRIRenderer::DispatchTraceOpaque(HWDrawInfo&, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials)
{
	Clocker clock(NriPTTraceOpaque);
	ScopedPtPerfTimer traceOpaqueTimer(mLastPerfShellTraceStats.traceOpaqueMs);
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.traceOpaqueReadbackMs);
		ReadbackTraceShaderStats();
		ReadbackAutoExposureStats();
	}

	if (!UpdateReprojectionBuffer())
	{
		return false;
	}

	NRITraceSceneConstants constants = {};
	const NRITraceSettings traceSettings = BuildNRITraceSettingsFromCVars();
	const NRIDenoiserSettings denoiserSettings = BuildNRIDenoiserSettingsFromCVars();
	const uint32_t bootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	const NRIMainUpscalerKind resolvedMainUpscaler = ResolveMainUpscalerKind(false);
	const nri::UpscalerMode resolvedUpscalerMode = ResolveUpscalerModeForMain(resolvedMainUpscaler, GetSelectedUpscalerMode());
	const uint32_t jitterPhaseCount = GetTemporalJitterPhaseCount(resolvedMainUpscaler, resolvedUpscalerMode, mGuiCaptureActive);
	const bool directSceneTrace = (!nri_ptbootstrap && nri_ptdirectscene) || bootstrapMode == 11u || bootstrapMode == 12u;
	const bool useTemporalJitter =
		!nri_ptbootstrap &&
		!mGuiCaptureActive &&
		ShouldUseTemporalJitter(resolvedMainUpscaler);
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
		(ShouldCollectTraceShaderStats() ? NRI_FLAG_TRACE_SHADER_STATS : 0u) |
		(useTemporalJitter ? NRI_FLAG_USE_JITTER : 0u) |
		PackTemporalJitterPhaseCount(jitterPhaseCount);
	constants.StaticMaterialCount = mBoundStaticMaterialCount;
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = mBoundDynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(
		traceSettings.lightBounceCount,
		traceSettings.mirrorBounceCount,
		mDirectionalLightState.color);
	constants.PortalCount = mBoundPortalCount;
	constants.RuntimeLightCount = mBoundRuntimeLightCount;
	constants.PortalDepth = PackPortalDepthAndAmbientMultipliers(
		traceSettings.portalDepth,
		GetBaseAmbient(),
		GetMetalAmbient());
	constants.ReservedTrace0 = (mBoundRuntimeLightTileCountX & 0xffffu) | ((mBoundRuntimeLightTileCountY & 0xffffu) << 16u);
	constants.ReservedTrace1 = PackTraceAux1(
		(uint32_t)denoiserSettings.denoiserMode,
		traceSettings.emissiveSampleCount,
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
	const uint32_t dispatchX = GetDispatchSize(mRenderWidth);
	const uint32_t dispatchY = GetDispatchSize(mRenderHeight);
	const uint32_t dispatchZ = 1;
	mLastPerfShellTraceStats.traceOpaqueDispatchX = dispatchX;
	mLastPerfShellTraceStats.traceOpaqueDispatchY = dispatchY;
	mLastPerfShellTraceStats.traceOpaqueDispatchZ = dispatchZ;
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.traceOpaqueCommandMs);
		ResetTraceShaderStatsBuffer();
		mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::TraceOpaque));
		mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { dispatchX, dispatchY, dispatchZ });
	}
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.traceOpaqueStatsCopyMs);
		CopyTraceShaderStatsForReadback((uint64_t)mFrameIndex);
	}
	return true;
}

bool NRIRenderer::DispatchDenoiser()
{
	Clocker clock(NriPTDenoiser);
	const NRIDenoiserSettings denoiserSettings = BuildNRIDenoiserSettingsFromCVars();

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
	desc.denoiserMode = denoiserSettings.denoiserMode;
	desc.maxAccumulatedFrameNum = denoiserSettings.maxAccumulatedFrameNum;
	desc.maxFastAccumulatedFrameNum = denoiserSettings.maxFastAccumulatedFrameNum;
	desc.maxStabilizedFrameNum = denoiserSettings.maxStabilizedFrameNum;
	desc.hitDistanceReconstructionMode = denoiserSettings.hitDistanceReconstructionMode;
	desc.fastHistoryClampingSigmaScale = denoiserSettings.fastHistoryClampingSigmaScale;
	desc.diffusePrepassBlurRadius = denoiserSettings.diffusePrepassBlurRadius;
	desc.specularPrepassBlurRadius = denoiserSettings.specularPrepassBlurRadius;
	desc.minBlurRadius = denoiserSettings.minBlurRadius;
	desc.maxBlurRadius = denoiserSettings.maxBlurRadius;
	desc.sigmaMaxStabilizedFrameNum = denoiserSettings.sigmaMaxStabilizedFrameNum;
	desc.sigmaPlaneDistanceSensitivity = denoiserSettings.sigmaPlaneDistanceSensitivity;
	desc.resetHistory = mResetHistory;
	desc.enableAntiFirefly = denoiserSettings.enableAntiFirefly;
	desc.enableValidation = denoiserSettings.enableValidation;
	desc.enableSigmaShadow = mUseSplitShadowDenoiser;
	return mNrd.Denoise(desc);
}

bool NRIRenderer::DispatchComposition(FrameTextureSlot outputSlot)
{
	Clocker clock(NriPTComposition);

	NRITraceSceneConstants constants = {};
	const NRIDenoiserSettings denoiserSettings = BuildNRIDenoiserSettingsFromCVars();
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
	constants.ReservedTrace0 = denoiserSettings.inputSplitMode;
	constants.ReservedTrace1 = PackDenoiserAux1((uint32_t)denoiserSettings.denoiserMode, mDirectionalLightState.angularSize);
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

	const NRIPTOutputPolicy outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	const NRIMainUpscalerKind resolvedMain = ResolveMainUpscalerKind(false);
	const NRIPostSharpenKind resolvedPost = ResolvePostSharpenKind(false);
	const ExposureRoute exposureRoute = ResolveExposureRoute(inputSlot, outputPolicy, resolvedMain, resolvedPost);
	NRIPresentConstants constants = {};
	ApplyOutputPolicyToPresentConstants(outputPolicy, constants);
	ApplyNightVisionStateToPresentConstants(mNightVisionState, constants);
	constants.Exposure = exposureRoute.presentExposure;
	const bool finalPresentInputPreExposed = exposureRoute.inputDomain == ExposureDomain::PreExposedHDR;
	const bool finalPresentAutoExposureEligible =
		mExposure.GetSettings().enabled &&
		exposureRoute.inputDomain == ExposureDomain::SceneHDR;
	NRITextureResource* exposureStateTexture = nullptr;
	if (finalPresentAutoExposureEligible)
	{
		NRITextureResource& candidateExposureState = mExposure.GetMutableExposureStateTexture(mFrameIndex & 1u);
		if (candidateExposureState.texture != nullptr)
		{
			exposureStateTexture = &candidateExposureState;
		}
	}
	const bool exposureStateTextureValid =
		exposureStateTexture != nullptr &&
		exposureStateTexture->shaderView != nullptr;
	constants.OutputFlags |=
		(finalPresentAutoExposureEligible ? NRI_PRESENT_OUTPUT_FLAG_AUTO_EXPOSURE : 0u) |
		(exposureStateTextureValid ? NRI_PRESENT_OUTPUT_FLAG_EXPOSURE_TEXTURE_VALID : 0u) |
		(finalPresentInputPreExposed ? NRI_PRESENT_OUTPUT_FLAG_INPUT_PRE_EXPOSED : 0u);
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
	if (exposureStateTextureValid)
	{
		mFrameBuffer->TransitionTexture(*exposureStateTexture, NRIComputeShaderResourceState());
	}
	mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	const nri::Descriptor* inputs[3] = {
		input.shaderView,
		exposureStateTextureValid ? exposureStateTexture->shaderView : input.shaderView,
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
			(useAppTaaJitter ? NRI_FLAG_USE_JITTER : 0u) |
			PackTemporalJitterPhaseCount(GetTemporalJitterPhaseCount(
				mainKind,
				ResolveUpscalerModeForMain(mainKind, GetSelectedUpscalerMode()),
				mGuiCaptureActive));
		constants.Exposure = GetTemporalExposure(mFrameBuffer->GetPathTracingOutputPolicy());
		NRITextureResource* exposureStateTexture = nullptr;
		if (mExposure.GetSettings().enabled)
		{
			NRITextureResource& candidateExposureState = mExposure.GetMutableExposureStateTexture(mFrameIndex & 1u);
			if (candidateExposureState.texture != nullptr)
			{
				exposureStateTexture = &candidateExposureState;
			}
		}
		const bool exposureStateTextureValid =
			exposureStateTexture != nullptr &&
			exposureStateTexture->shaderView != nullptr;
		constants.Flags |=
			(mExposure.GetSettings().enabled ? NRI_TEMPORAL_FLAG_AUTO_EXPOSURE : 0u) |
			(exposureStateTextureValid ? NRI_TEMPORAL_FLAG_EXPOSURE_TEXTURE_VALID : 0u);

		mFrameBuffer->TransitionTexture(composed, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(historyInput, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeShaderResourceState());
		if (exposureStateTextureValid)
		{
			mFrameBuffer->TransitionTexture(*exposureStateTexture, NRIComputeShaderResourceState());
		}
		mFrameBuffer->TransitionTexture(historyOutput, NRIComputeStorageState());

		const nri::Descriptor* taaInputs[4] = {
			historyInput.shaderView,
			GetFrameTexture(FrameTextureSlot::Motion).shaderView,
			composed.shaderView,
			exposureStateTextureValid ? exposureStateTexture->shaderView : composed.shaderView
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
		NRITextureResource* vendorExposure = nullptr;
		if (mExposure.GetSettings().enabled)
		{
			NRITextureResource& candidateExposureState = mExposure.GetMutableExposureStateTexture(mFrameIndex & 1u);
			if (candidateExposureState.texture != nullptr && candidateExposureState.shaderView != nullptr)
			{
				vendorExposure = &candidateExposureState;
			}
		}

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
		if (vendorExposure != nullptr)
		{
			mFrameBuffer->TransitionTexture(*vendorExposure, NRIComputeShaderResourceState());
		}
		mFrameBuffer->TransitionTexture(vendorOutput, NRIComputeStorageState());

		const nri::UpscalerMode resolvedUpscalerMode = ResolveUpscalerModeForMain(mainKind, GetSelectedUpscalerMode());
		if (!mUpscaler.EnsureMainUpscaler(*mFrameBuffer, mainKind, resolvedUpscalerMode, mOutputWidth, mOutputHeight, vendorExposure != nullptr))
		{
			return false;
		}

		NRIUpscalerDispatchDesc upscalerDesc = {};
		upscalerDesc.commandBuffer = mFrameBuffer->mCommandBuffer;
		upscalerDesc.input = &vendorInput;
		upscalerDesc.output = &vendorOutput;
		upscalerDesc.motion = &GetFrameTexture(FrameTextureSlot::Motion);
		upscalerDesc.depth = &upscalerDepth;
		upscalerDesc.exposure = vendorExposure;
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
		const nri::UpscalerMode resolvedUpscalerMode = ResolveUpscalerModeForMain(resolvedMainUpscaler, GetSelectedUpscalerMode());
		const uint32_t jitterPhaseCount = GetTemporalJitterPhaseCount(resolvedMainUpscaler, resolvedUpscalerMode, mGuiCaptureActive);
		ComputeTemporalJitter(mFrameIndex, jitterPhaseCount, mCurrentJitter);
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
	if (ShouldEmitRendererTemporalTraceLogs())
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

	if (!mHasLoggedStats || nri_scene::SceneDebugStatsDiffer(mLastStats, stats))
	{
		Printf("NRI PT scene: walls=%u flats=%u sprites=%u translucent=%u models=%u voxel_proxies=%u unsupported_models=%u voxel_cache=candidates:%u uncacheable:%u hits:%u misses:%u changes:%u split_stable:%u split_live:%u entries:%u surface_hits:%u stores:%u rebuilds:%u transform_rebakes:%u removes:%u not_captured:%u deferred:%u cached_prims:%u mirrors=%u skies=%u portal_views=%u portal_skips=%u approx_tris=%u materials=%u\n",
			stats.wallDrawItems,
			stats.flatDrawItems,
			stats.spriteDrawItems,
			stats.translucentDrawItems,
			stats.modelDrawItems,
			stats.voxelProxyDrawItems,
			stats.unsupportedModelDrawItems,
			stats.voxelStableCandidates,
			stats.voxelStableUncacheable,
			stats.voxelStableSignatureHits,
			stats.voxelStableSignatureMisses,
			stats.voxelStableSignatureChanges,
			stats.voxelStableSplitStable,
			stats.voxelStableSplitLive,
			stats.voxelCacheEntries,
			stats.voxelCacheSurfaceHits,
			stats.voxelCacheSurfaceStores,
			stats.voxelCacheSurfaceRebuilds,
			stats.voxelCacheTransformRebakes,
			stats.voxelCacheSurfaceRemoves,
			stats.voxelCacheNotCaptured,
			stats.voxelCacheDeferred,
			stats.voxelCachePrimitives,
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
		if (ShouldEmitRendererTemporalTraceLogs())
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
			if (ShouldEmitRendererTemporalTraceLogs())
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
		if (ShouldEmitRendererTemporalTraceLogs())
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
		if (ShouldEmitRendererTemporalTraceLogs())
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
	for (auto& skyTexture : mSkyEnvironment.CachedTextures())
	{
		mFrameBuffer->DestroyTextureResource(skyTexture.resource);
	}
	mSkyEnvironment.ClearCache();
	for (auto& texture : mSceneTextures.CachedTextures())
	{
		mFrameBuffer->DestroyTextureResource(texture.resource);
	}
	mSceneTextures.ClearCachedTextures();
}

void NRIRenderer::DestroyFrameTextures()
{
	DestroyAutoExposureResources();
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
	nri_static_scene_geometry::ResetStaticMapChunkAtlas(mStaticMapChunkAtlas);
	ResetResidentMapChunkRegistry();
	ResetPersistentDynamicEmissiveCache();
	mPersistentVoxels.Reset("destroy-scene-buffers", true, (int)nri_ptloadingtrace >= 1 || (bool)nri_voxelstats, BuildPersistentVoxelResetServices());
	DestroyBufferResource(mStaticVertexBuffer);
	DestroyBufferResource(mStaticIndexBuffer);
	DestroyBufferResource(mStaticPrimitiveBuffer);
	DestroyBufferResource(mStaticMaterialBuffer);
	NRIPersistentVoxelDestroyServices persistentVoxelDestroyServices = {};
	persistentVoxelDestroyServices.user = this;
	persistentVoxelDestroyServices.destroyBuffer = [](void* user, NRIBufferResource& resource)
	{
		static_cast<NRIRenderer*>(user)->DestroyBufferResource(resource);
	};
	mPersistentVoxels.DestroyArenaBuffers(persistentVoxelDestroyServices);
	DestroyBufferResource(mVertexBuffer);
	DestroyBufferResource(mIndexBuffer);
	DestroyBufferResource(mPrimitiveBuffer);
	DestroyBufferResource(mMaterialBuffer);
	for (SceneUploadBufferRingSlot& slot : mSceneUploadBufferRing)
	{
		DestroyAccelerationStructureResource(slot.dynamicBottomLevelAS);
		DestroyBufferResource(slot.vertexBuffer);
		DestroyBufferResource(slot.indexBuffer);
		DestroyBufferResource(slot.primitiveBuffer);
		DestroyBufferResource(slot.materialBuffer);
	}
	mSceneUploadBufferRing.clear();
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
	DestroyBufferResource(mEmissiveMaterialResponseBuffer);
	DestroyBufferResource(mSectorLightHeaderBuffer);
	DestroyBufferResource(mSectorLightBuffer);
	DestroyBufferResource(mReprojectionBuffer);
	DestroyBufferResource(mVisibleChunkBuffer);
	DestroyBufferResource(mVisibleFlatPlaneBuffer);
	DestroyBufferResource(mTraceShaderStatsBuffer);
	DestroyBufferResource(mTraceShaderStatsReadbackBuffer);
	DestroyBufferResource(mTraceShaderStatsZeroBuffer);
	DestroyBufferResource(mScratchBuffer);
	DestroyBufferResource(mResidentStaticBlasScratchBuffer);
	DestroyBufferResource(mTopLevelScratchBuffer);
	DestroyBufferResource(mEmissiveTopLevelScratchBuffer);
	for (NRIBufferResource& tlasInstanceBuffer : mTlasInstanceBufferRing)
	{
		DestroyBufferResource(tlasInstanceBuffer);
	}
	mTlasInstanceBufferRing.clear();
	for (auto& frameScratch : mResidentUploadScratchFrames)
	{
		DestroyBufferResource(frameScratch.vertex.buffer);
		DestroyBufferResource(frameScratch.index.buffer);
		DestroyBufferResource(frameScratch.primitive.buffer);
		DestroyBufferResource(frameScratch.material.buffer);
		for (NRIBufferResource& retired : frameScratch.retiredBuffers)
		{
			DestroyBufferResource(retired);
		}
		for (NRIAccelerationStructureResource& retired : frameScratch.retiredAccelerationStructures)
		{
			DestroyAccelerationStructureResource(retired);
		}
		frameScratch = {};
	}
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
	mRuntimeLightSceneDataDirty = false;
	mBoundEmissivePrimitiveCount = 0;
	mBoundEmissiveDominantPrimitive = UINT32_MAX;
	mBoundEmissiveDominantTile = 0;
	mBoundEmissiveDominantFlags = 0;
	mBoundEmissiveDominantDataSource = 0;
	mEmissiveSamplingPayloadCacheValid = false;
	mEmissiveSamplingPayloadHash = 0;
	mEmissiveSectorResponsePayloadCacheValid = false;
	mEmissiveSectorResponsePayloadHash = 0;
	mEmissiveSectorResponseTraceCacheValid = false;
	mEmissiveSectorResponseTraceHash = 0;
	mEmissiveSectorResponseNotifyCacheValid = false;
	mLastEmissiveSectorResponseNotifyFrame = 0;
	mEmissiveSectorResponseNotifyScales.clear();
	mSectorLightingEditNotifyCacheValid = false;
	mLastSectorLightingEditNotifyFrame = 0;
	mSectorLightingEditNotifyHashes.clear();
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
	for (auto& chunk : mStaticMapScene.chunks)
	{
		DestroyAccelerationStructureResource(chunk.accelerationStructure);
		chunk.residentBlasScratchSizeCacheKey = nullptr;
		chunk.residentBlasBuildScratchSize = 0;
		chunk.residentBlasUpdateScratchSize = 0;
	}
	DestroyDynamicBottomLevelAccelerationStructures();
	mPersistentVoxels.Reset("destroy-acceleration-structures", true, (int)nri_ptloadingtrace >= 1 || (bool)nri_voxelstats, BuildPersistentVoxelResetServices());
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
	resource.payloadHash = 0;
	resource.payloadSize = 0;
	resource.stride = 0;
	resource.payloadStride = 0;
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
	resource.buildScratchSize = 0;
	resource.buildVertexBuffer = nullptr;
	resource.buildIndexBuffer = nullptr;
	resource.buildVertexCount = 0;
	resource.buildIndexOffset = 0;
	resource.buildIndexCount = 0;
	resource.buildPrimitiveCount = 0;
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

bool NRIRenderer::ShouldRunAppTaaForFrameGraph(NRIMainUpscalerKind kind) const
{
	return ShouldRunAppTaa(kind);
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
