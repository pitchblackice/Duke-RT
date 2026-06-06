#include "nri_exposure.h"

#include "c_cvars.h"

#include <algorithm>

CVAR(Bool, nri_ptautoexposure, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptautoexposurefreeze, false, 0)
CVAR(Bool, nri_ptautoexposurestats, false, 0)
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
CUSTOM_CVAR(Float, nri_ptautoexposuretarget, 0.18f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptautoexposuremin, 0.125f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptautoexposuremax, 8.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptautoexposurebias, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptautoexposurelowpercentile, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CUSTOM_CVAR(Float, nri_ptautoexposurehighpercentile, 99.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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

NRIAutoExposureSettings GetNRIAutoExposureSettings(float fallbackManualExposure)
{
	NRIAutoExposureSettings settings = {};
	settings.enabled = !!nri_ptautoexposure;
	settings.freeze = !!nri_ptautoexposurefreeze;
	settings.stats = !!nri_ptautoexposurestats;
	settings.histogramBinCount = (uint32_t)std::clamp((int)nri_ptautoexposurebins, 16, 256);
	settings.sampleStep = (uint32_t)std::clamp((int)nri_ptautoexposuresamplestep, 1, 8);
	settings.targetLuminance = std::clamp((float)nri_ptautoexposuretarget, 0.02f, 1.0f);
	settings.minExposure = std::clamp((float)nri_ptautoexposuremin, 0.03125f, 8.0f);
	settings.maxExposure = std::clamp((float)nri_ptautoexposuremax, 0.125f, 32.0f);
	if (settings.maxExposure < settings.minExposure)
	{
		settings.maxExposure = settings.minExposure;
	}
	settings.exposureBias = std::clamp((float)nri_ptautoexposurebias, 0.125f, 8.0f);
	settings.lowPercentile = std::clamp((float)nri_ptautoexposurelowpercentile, 0.0f, 99.0f);
	settings.highPercentile = std::clamp((float)nri_ptautoexposurehighpercentile, 1.0f, 100.0f);
	if (settings.highPercentile <= settings.lowPercentile)
	{
		settings.highPercentile = std::min(settings.lowPercentile + 1.0f, 100.0f);
	}
	settings.adaptUpSpeed = std::clamp((float)nri_ptautoexposureadaptup, 0.0f, 16.0f);
	settings.adaptDownSpeed = std::clamp((float)nri_ptautoexposureadaptdown, 0.0f, 16.0f);
	settings.fallbackManualExposure = std::max(fallbackManualExposure, 0.0f);
	return settings;
}

bool NRIExposureController::MatchesRenderSize(uint32_t renderWidth, uint32_t renderHeight) const
{
	return
		mStatus.renderWidth == std::max(renderWidth, 1u) &&
		mStatus.renderHeight == std::max(renderHeight, 1u);
}

void NRIExposureController::MarkResourcesAllocated(uint32_t renderWidth, uint32_t renderHeight, uint64_t memoryBytes)
{
	mStatus.resourcesAllocated = HasExposureStateTextures();
	mStatus.renderWidth = std::max(renderWidth, 1u);
	mStatus.renderHeight = std::max(renderHeight, 1u);
	mStatus.memoryBytes = memoryBytes;
	mStatus.allocationSerial++;
}

void NRIExposureController::MarkResourcesDestroyed()
{
	ResetStatus();
}

const NRITextureResource* NRIExposureController::GetExposureStateTexture(uint32_t index) const
{
	if (index >= 2 || mExposureState[index].texture == nullptr)
	{
		return nullptr;
	}

	return &mExposureState[index];
}

bool NRIExposureController::HasExposureStateTextures() const
{
	return
		mExposureState[0].texture != nullptr &&
		mExposureState[0].shaderView != nullptr &&
		mExposureState[0].storageView != nullptr &&
		mExposureState[1].texture != nullptr &&
		mExposureState[1].shaderView != nullptr &&
		mExposureState[1].storageView != nullptr;
}

void NRIExposureController::ResetStatus()
{
	mStatus.resourcesAllocated = false;
	mStatus.renderWidth = 0;
	mStatus.renderHeight = 0;
	mStatus.memoryBytes = 0;
}
