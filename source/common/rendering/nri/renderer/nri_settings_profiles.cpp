#include "c_cvars.h"
#include "printf.h"
#include "nri_settings_profiles.h"

#include <algorithm>
#include <iterator>

EXTERN_CVAR(Bool, nri_denoise)
EXTERN_CVAR(Int, nri_nrddenoiser)
EXTERN_CVAR(Int, nri_upscaler)
EXTERN_CVAR(Int, nri_postsharpen)
EXTERN_CVAR(Int, nri_upscalermode)
EXTERN_CVAR(Bool, nri_pttaa)
EXTERN_CVAR(Int, nri_ptoutputmode)
EXTERN_CVAR(Bool, nri_validation)
EXTERN_CVAR(Bool, nri_apivalidation)
EXTERN_CVAR(Bool, nri_dred)
EXTERN_CVAR(Bool, nri_framegen)
EXTERN_CVAR(Int, nri_framegenprovider)

CVAR(String, nri_settingsprofilewarning, "Known bug: mirrors can look glitchy without DLRR.", CVAR_GLOBALCONFIG)

namespace
{
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

void NRIApplySafeSettingsProfileForRecovery()
{
	nri_settingsprofile = NRI_SETTINGS_PROFILE_SAFE;
	ApplyNRISettingsProfile(kNRISettingsProfilePresets[NRI_SETTINGS_PROFILE_SAFE]);
}
