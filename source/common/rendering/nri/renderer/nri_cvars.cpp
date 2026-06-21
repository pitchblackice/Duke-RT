#include "nri_cvars.h"

#include "nri_settings_profiles.h"
#include "../scene/nri_map_builder.h"
#include "../system/nri_renderdevice.h"
#include "printf.h"
#include "v_video.h"

#include <algorithm>
#include <iterator>

namespace
{
	static constexpr int kPtDebugMenuModes[] = {
		0, 1, 2, 3, 4, 5,
		9, 10, 11, 12,
		16, 17, 18, 19,
		21, 22, 24, 25,
		26, 27, 28, 29,
		33, 34, 45
	};

	int ClampPtDebugMenuIndex(int index)
	{
		return std::clamp(index, 0, (int)std::size(kPtDebugMenuModes) - 1);
	}

	int FindPtDebugMenuIndex(int debugMode)
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

	int ResolvePtDebugModeFromMenuIndex(int index)
	{
		return kPtDebugMenuModes[ClampPtDebugMenuIndex(index)];
	}

	int ResolvePtDebugMenuIndexFromMode(int debugMode)
	{
		const int index = FindPtDebugMenuIndex(debugMode);
		return index >= 0 ? index : 0;
	}

	bool gSyncingPtDebugMenu = false;

	NRIRenderDevice* GetActiveNriRenderDeviceForCVar()
	{
		return screen != nullptr && screen->Backend() == 4 ? static_cast<NRIRenderDevice*>(screen) : nullptr;
	}

	void RefreshActiveFrameGenerationSwapChain()
	{
		if (NRIRenderDevice* frameBuffer = GetActiveNriRenderDeviceForCVar())
		{
			frameBuffer->SetVSync(vid_vsync);
		}
	}

	void NotifyActiveGlowControlChange()
	{
		if (NRIRenderDevice* frameBuffer = GetActiveNriRenderDeviceForCVar())
		{
			frameBuffer->NotifyPathTracingGlowControlChange();
		}
	}

	void NotifyActiveMaterialLightingCalibrationChange()
	{
		if (NRIRenderDevice* frameBuffer = GetActiveNriRenderDeviceForCVar())
		{
			frameBuffer->NotifyPathTracingMaterialLightingCalibrationChange();
		}
	}

	void NotifyActiveDebugSphereTessellationChange()
	{
		if (NRIRenderDevice* frameBuffer = GetActiveNriRenderDeviceForCVar())
		{
			frameBuffer->NotifyPathTracingDebugSphereTessellationChange();
		}
	}

	enum NRISettingsProfile
	{
		NRI_SETTINGS_PROFILE_SAFE = 0,
		NRI_SETTINGS_PROFILE_DLRR_FAST,
		NRI_SETTINGS_PROFILE_DLRR_MEDIUM,
		NRI_SETTINGS_PROFILE_DLRR_BEAUTIFUL,
		NRI_SETTINGS_PROFILE_DLSS_SR_FAST,
		NRI_SETTINGS_PROFILE_DLSS_SR_MEDIUM,
		NRI_SETTINGS_PROFILE_DLSS_SR_BEAUTIFUL,
	};

	struct NRISettingsProfilePreset
	{
		const char* name;
		int upscaler;
		int upscalerMode;
		int outputMode;
		bool denoise;
		const char* warning;
	};

	constexpr int kNRIUpscalerOff = 0;
	constexpr int kNRIUpscalerDlssSr = 2;
	constexpr int kNRIUpscalerDlrr = 3;

	constexpr int kNRIUpscalerModeNative = 0;
	constexpr int kNRIUpscalerModeQuality = 2;
	constexpr int kNRIUpscalerModeBalanced = 3;

	constexpr int kNRIOutputSdr = 0;
	constexpr int kNRIOutputHdr = 1;

	constexpr int kNRDDenoiserRelax = 1;

	constexpr const char* kMirrorsWithoutRayReconstructionWarning =
		"Warning: mirrors are currently bugged when ray reconstruction is not used.";
	constexpr const char* kMirrorsWithoutRayReconstructionMenuWarning =
		"Known bug: mirrors can look glitchy without DLRR.";

	constexpr NRISettingsProfilePreset kNRISettingsProfilePresets[] = {
		{ "Safe Mode", kNRIUpscalerOff, kNRIUpscalerModeNative, kNRIOutputSdr, true, kMirrorsWithoutRayReconstructionWarning },
		{ "Fast Preset - DLRR", kNRIUpscalerDlrr, kNRIUpscalerModeBalanced, kNRIOutputHdr, false, nullptr },
		{ "Medium Preset - DLRR", kNRIUpscalerDlrr, kNRIUpscalerModeQuality, kNRIOutputHdr, false, nullptr },
		{ "Beautiful Preset - DLRR", kNRIUpscalerDlrr, kNRIUpscalerModeNative, kNRIOutputHdr, false, nullptr },
		{ "Fast Preset - DLSS-SR", kNRIUpscalerDlssSr, kNRIUpscalerModeBalanced, kNRIOutputHdr, true, kMirrorsWithoutRayReconstructionWarning },
		{ "Medium Preset - DLSS-SR", kNRIUpscalerDlssSr, kNRIUpscalerModeQuality, kNRIOutputHdr, true, kMirrorsWithoutRayReconstructionWarning },
		{ "Beautiful Preset - DLSS-SR", kNRIUpscalerDlssSr, kNRIUpscalerModeNative, kNRIOutputHdr, true, kMirrorsWithoutRayReconstructionWarning },
	};

	bool gSkipInitialProfileApply = true;

	int ClampNRISettingsProfile(int profile)
	{
		return std::clamp(profile, 0, (int)std::size(kNRISettingsProfilePresets) - 1);
	}

	void SyncNRISettingsProfileWarning(const NRISettingsProfilePreset& preset)
	{
		nri_settingsprofilewarning = preset.warning != nullptr ? kMirrorsWithoutRayReconstructionMenuWarning : "";
	}

	void ApplyNRISettingsProfile(const NRISettingsProfilePreset& preset)
	{
		SyncNRISettingsProfileWarning(preset);

		nri_upscaler = preset.upscaler;
		nri_postsharpen = 0;
		nri_upscalermode = preset.upscalerMode;
		nri_ptoutputmode = preset.outputMode;
		nri_pttaa = false;

		nri_denoise = preset.denoise;
		nri_nrddenoiser = kNRDDenoiserRelax;

		nri_validation = false;
		nri_apivalidation = false;
		nri_dred = false;

		nri_framegen = false;
		nri_framegenprovider = 0;

		Printf(PRINT_NOTIFY, "Applied settings profile: %s\n", preset.name);
		if (preset.warning != nullptr && preset.warning[0] != '\0')
		{
			Printf(PRINT_NOTIFY, "%s\n", preset.warning);
		}
	}
}


// Moved from source/common/rendering/nri/system/nri_renderdevice.cpp

CVAR(Bool, nri_ptsanity, false, 0)

CVAR(Bool, nri_ptwaitpresent, true, 0)

CVAR(Bool, nri_ptslowdowntrace, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, nri_pttraceframes, 0, 0)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 600)
	{
		self = 600;
	}
}

CUSTOM_CVAR(Int, nri_ptslowdowntraceinterval, 300, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1)
	{
		self = 1;
	}
	else if (self > 36000)
	{
		self = 36000;
	}
}

CUSTOM_CVAR(Int, nri_ptslowdowntop, 5, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 16)
	{
		self = 16;
	}
}

CUSTOM_CVAR(Int, nri_ptnudgetrace, 0, 0)
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

CUSTOM_CVAR(Int, nri_ptswaptextures, 0, 0)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self == 1)
	{
		self = 2;
	}
	else if (self > 8)
	{
		self = 8;
	}

	if (auto* frameBuffer = GetActiveNriRenderDeviceForCVar())
	{
		frameBuffer->SetVSync(vid_vsync);
	}
}

CUSTOM_CVAR(Int, nri_ptswapflags, -1, 0)
{
	if (self < -1)
	{
		self = -1;
	}
	else if (self > 3)
	{
		self = 3;
	}

	if (auto* frameBuffer = GetActiveNriRenderDeviceForCVar())
	{
		frameBuffer->SetVSync(vid_vsync);
	}
}


// Moved from source/common/rendering/nri/scene/nri_portal_bridge.cpp

CVAR(Int, nri_ptportaldepth, 6, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)


// Moved from source/common/rendering/nri/scene/nri_scene_bridge.cpp

CVAR(Bool, nri_voxelstats, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeltrianglebudget, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelmaxtriangles, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelcaptureactors, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelpersistentpromoteframes, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelmeshbuilds, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingtrace, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptloadingvoxelcpu, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptloadingvoxelgpu, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptloadingvoxelgpuwhitelistonly, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelgpuminprims, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelgpumaxprims, 9000000, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelgpumaxbytes, 1630491936, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelgpumaxvariants, 256, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelactors, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelvariants, 128, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelvariantprims, 2000000, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelpicrange, 64, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelcpubudget, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelcpumaxvariants, 64, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelcpumaxprims, 9000000, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelcpumaxms, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelgpubudget, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptloadingvoxellist, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)


// Moved from source/common/rendering/nri/renderer/nri_exposure.cpp

CVAR(Bool, nri_ptautoexposure, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_pthdrautoexposure, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptautoexposurefreeze, false, 0)

CVAR(Bool, nri_ptautoexposurestats, false, 0)

CUSTOM_CVAR(Int, nri_ptautoexposuremetering, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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

CUSTOM_CVAR(Int, nri_ptautoexposurebins, 256, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 16)
	{
		self = 16;
	}
	else if (self > 256)
	{
		self = 256;
	}
}

CUSTOM_CVAR(Int, nri_ptautoexposuresamplestep, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1)
	{
		self = 1;
	}
	else if (self > 8)
	{
		self = 8;
	}
}

CUSTOM_CVAR(Float, nri_ptautoexposuretarget, 0.03225f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.02f)
	{
		self = 0.02f;
	}
	else if (self > 1.0f)
	{
		self = 1.0f;
	}
}

CUSTOM_CVAR(Float, nri_pthdrautoexposuretarget, 0.0445f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.02f)
	{
		self = 0.02f;
	}
	else if (self > 1.0f)
	{
		self = 1.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptautoexposuremin, 2.74561f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.03125f)
	{
		self = 0.03125f;
	}
	else if (self > 8.0f)
	{
		self = 8.0f;
	}
}

CUSTOM_CVAR(Float, nri_pthdrautoexposuremin, 0.604004f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.03125f)
	{
		self = 0.03125f;
	}
	else if (self > 8.0f)
	{
		self = 8.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptautoexposuremax, 4.00977f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.125f)
	{
		self = 0.125f;
	}
	else if (self > 32.0f)
	{
		self = 32.0f;
	}
}

CUSTOM_CVAR(Float, nri_pthdrautoexposuremax, 7.09766f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.125f)
	{
		self = 0.125f;
	}
	else if (self > 32.0f)
	{
		self = 32.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptautoexposurebias, 0.469531f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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

CUSTOM_CVAR(Float, nri_pthdrautoexposurebias, 0.371094f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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

CUSTOM_CVAR(Float, nri_ptautoexposurelowpercentile, 1.01563f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 99.0f)
	{
		self = 99.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptautoexposurehighpercentile, 98.9844f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1.0f)
	{
		self = 1.0f;
	}
	else if (self > 100.0f)
	{
		self = 100.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptautoexposureadaptup, 3.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 16.0f)
	{
		self = 16.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptautoexposureadaptdown, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 16.0f)
	{
		self = 16.0f;
	}
}

CVAR(Bool, nri_ptbloom, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Float, nri_ptbloomintensity, 0.053125f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 1.0f)
	{
		self = 1.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptbloomsigma, 0.796875f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 1.0f)
	{
		self = 1.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptbloomcutoff, 0.65f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 16.0f)
	{
		self = 16.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptbloomfuzziness, 0.146875f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 1.0f)
	{
		self = 1.0f;
	}
}

CUSTOM_CVAR(Int, nri_ptbloomlevels, 6, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1)
	{
		self = 1;
	}
	else if (self > 8)
	{
		self = 8;
	}
}

CVAR(Bool, nri_ptbloomenergyconstrained, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, nri_ptbloomdebug, 0, 0)
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


// Moved from source/common/rendering/nri/renderer/nri_renderer.cpp

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


// Moved from source/common/rendering/nri/renderer/nri_renderer_settings.cpp

CVAR(Int, nri_ptpersistentvoxelbuildactors, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptpersistentvoxelbuildprims, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptpersistentvoxelbuildbytes, 4 * 1024 * 1024, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptpersistentvoxeltextureprewarms, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptpersistentvoxeltexturebytes, 1024 * 1024, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelruntimebudget, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmissionloadvariants, 4, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmissionloadbytes, 64 * 1024 * 1024, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmissionruntimevariants, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmissionruntimebytes, 16 * 1024 * 1024, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmitmaxbytesloading, 64 * 1024 * 1024, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmitmaxbytesruntime, 16 * 1024 * 1024, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmitmaxmsloading, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmitmaxmsruntime, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmitmaxblasloading, 4, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmitmaxblasruntime, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmitmaxblasprims, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmitisolateblasprims, 65536, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelresidentmaxbytes, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelresidentminheadroombytes, 512 * 1024 * 1024, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelresidentmaxcoldmaps, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptvoxeltransformkeyed, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelexcludeindex, -1, 0)

CVAR(Int, nri_ptvoxelexcludeindex2, -1, 0)

CVAR(Int, nri_ptvoxelexcludeindex3, -1, 0)

CVAR(Int, nri_ptvoxelexcludeminprims, 0, 0)

CVAR(Bool, nri_ptruntimeworklist, true, 0)

CVAR(Int, nri_ptruntimeworklistsweepbudget, 32, 0)

CVAR(Bool, nri_ptruntimedeferfarmaterial, true, 0)

CVAR(Bool, nri_ptruntimedefernearinvisiblematerial, true, 0)

CVAR(Int, nri_ptruntimenearinvisiblematerialbudget, 4, 0)

CVAR(Bool, nri_ptruntimedeferfarstructural, true, 0)

CVAR(Int, nri_ptruntimefarstructuralbudget, 2, 0)

CVAR(Bool, nri_ptruntimedefernearinvisiblestructural, true, 0)

CVAR(Int, nri_ptruntimenearinvisiblestructuralbudget, 2, 0)

CUSTOM_CVAR(Float, nri_ptruntimemutationneardistance, 1024.0f, 0)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}


// Moved from source/common/rendering/nri/renderer/nri_settings_profiles.cpp

CVAR(String, nri_settingsprofilewarning, "Known bug: mirrors can look glitchy without DLRR.", CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, nri_settingsprofile, NRI_SETTINGS_PROFILE_SAFE, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	const int clampedProfile = ClampNRISettingsProfile(self);
	if (self != clampedProfile)
	{
		self = clampedProfile;
		return;
	}

	const auto& preset = kNRISettingsProfilePresets[self];
	SyncNRISettingsProfileWarning(preset);

	if (gSkipInitialProfileApply)
	{
		gSkipInitialProfileApply = false;
		return;
	}

	ApplyNRISettingsProfile(preset);
}


// Moved from source/common/rendering/nri/renderer/nri_scene_lights.cpp

CVAR(Int, nri_ptactoroverlaylighttrace, 0, 0)

void NRIApplySafeSettingsProfileForRecovery()
{
	nri_settingsprofile = NRI_SETTINGS_PROFILE_SAFE;
	ApplyNRISettingsProfile(kNRISettingsProfilePresets[NRI_SETTINGS_PROFILE_SAFE]);
}
