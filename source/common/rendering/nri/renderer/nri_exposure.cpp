#include "nri_exposure.h"

#include "c_cvars.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
	constexpr uint32_t NRI_AUTO_EXPOSURE_DEBUG_MAGIC = 0x45585033u;

	float AsFloat(uint32_t value)
	{
		float result = 0.0f;
		std::memcpy(&result, &value, sizeof(result));
		return result;
	}
}

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

const char* GetNRIAutoExposureMeteringModeName(NRIAutoExposureMeteringMode mode)
{
	switch (mode)
	{
	case NRIAutoExposureMeteringMode::FullFrame: return "full";
	case NRIAutoExposureMeteringMode::CenterWeighted: return "center";
	case NRIAutoExposureMeteringMode::BrightTailSuppressed: return "bright-tail";
	default: return "unknown";
	}
}

NRIAutoExposureSettings GetNRIAutoExposureSettings(float fallbackManualExposure, bool hdrControlsActive)
{
	NRIAutoExposureSettings settings = {};
	settings.hdrControlsActive = hdrControlsActive;
	settings.enabled = hdrControlsActive ? !!nri_pthdrautoexposure : !!nri_ptautoexposure;
	settings.freeze = !!nri_ptautoexposurefreeze;
	settings.stats = !!nri_ptautoexposurestats;
	settings.meteringMode = (NRIAutoExposureMeteringMode)std::clamp((int)nri_ptautoexposuremetering, 0, 2);
	settings.histogramBinCount = (uint32_t)std::clamp((int)nri_ptautoexposurebins, 16, 256);
	settings.sampleStep = (uint32_t)std::clamp((int)nri_ptautoexposuresamplestep, 1, 8);
	settings.targetLuminance = std::clamp(
		hdrControlsActive ? (float)nri_pthdrautoexposuretarget : (float)nri_ptautoexposuretarget,
		0.02f,
		1.0f);
	settings.minExposure = std::clamp(
		hdrControlsActive ? (float)nri_pthdrautoexposuremin : (float)nri_ptautoexposuremin,
		0.03125f,
		8.0f);
	settings.maxExposure = std::clamp(
		hdrControlsActive ? (float)nri_pthdrautoexposuremax : (float)nri_ptautoexposuremax,
		0.125f,
		32.0f);
	if (settings.maxExposure < settings.minExposure)
	{
		settings.maxExposure = settings.minExposure;
	}
	settings.exposureBias = std::clamp(
		hdrControlsActive ? (float)nri_pthdrautoexposurebias : (float)nri_ptautoexposurebias,
		0.125f,
		8.0f);
	settings.lowPercentile = std::clamp((float)nri_ptautoexposurelowpercentile, 0.0f, 99.0f);
	settings.highPercentile = std::clamp((float)nri_ptautoexposurehighpercentile, 1.0f, 100.0f);
	if (settings.highPercentile <= settings.lowPercentile)
	{
		settings.highPercentile = std::min(settings.lowPercentile + 1.0f, 100.0f);
	}
	if (settings.meteringMode == NRIAutoExposureMeteringMode::BrightTailSuppressed)
	{
		settings.highPercentile = std::min(settings.highPercentile, 95.0f);
		if (settings.highPercentile <= settings.lowPercentile)
		{
			settings.lowPercentile = std::max(settings.highPercentile - 1.0f, 0.0f);
		}
	}
	settings.adaptUpSpeed = std::clamp((float)nri_ptautoexposureadaptup, 0.0f, 16.0f);
	settings.adaptDownSpeed = std::clamp((float)nri_ptautoexposureadaptdown, 0.0f, 16.0f);
	settings.fallbackManualExposure = std::max(fallbackManualExposure, 0.0f);
	return settings;
}

const char* GetNRIAutoExposureResetReasonForSettingsChange(
	const NRIAutoExposureSettings& previous,
	const NRIAutoExposureSettings& current)
{
	if (previous.hdrControlsActive != current.hdrControlsActive)
	{
		return "auto-exposure-control-block-change";
	}
	if (previous.enabled != current.enabled)
	{
		return current.enabled ? "auto-exposure-enabled" : "auto-exposure-disabled";
	}
	if (previous.histogramBinCount != current.histogramBinCount ||
		previous.sampleStep != current.sampleStep ||
		previous.meteringMode != current.meteringMode ||
		previous.lowPercentile != current.lowPercentile ||
		previous.highPercentile != current.highPercentile)
	{
		return "auto-exposure-metering-change";
	}
	if (previous.minExposure != current.minExposure ||
		previous.maxExposure != current.maxExposure)
	{
		return "auto-exposure-clamp-change";
	}
	if (previous.targetLuminance != current.targetLuminance ||
		previous.exposureBias != current.exposureBias)
	{
		return "auto-exposure-target-change";
	}

	return nullptr;
}

bool NRIExposureController::MatchesRenderSize(uint32_t renderWidth, uint32_t renderHeight) const
{
	return
		mStatus.renderWidth == std::max(renderWidth, 1u) &&
		mStatus.renderHeight == std::max(renderHeight, 1u);
}

void NRIExposureController::MarkResourcesAllocated(uint32_t renderWidth, uint32_t renderHeight, uint64_t memoryBytes)
{
	mStatus.resourcesAllocated = HasRequiredResources();
	mStatus.histogramAllocated = HasHistogramResources();
	mStatus.debugBufferAllocated = HasDebugBuffer();
	mStatus.debugReadbackAllocated = HasDebugReadbackBuffer();
	mStatus.renderWidth = std::max(renderWidth, 1u);
	mStatus.renderHeight = std::max(renderHeight, 1u);
	mStatus.memoryBytes = memoryBytes;
	mStatus.allocationSerial++;
}

void NRIExposureController::MarkResourcesDestroyed()
{
	ResetStatus();
}

void NRIExposureController::MarkDebugReadback(uint64_t frameIndex, const uint32_t* words, uint32_t wordCount)
{
	if (words == nullptr || wordCount < NRI_AUTO_EXPOSURE_DEBUG_WORD_COUNT || words[0] != NRI_AUTO_EXPOSURE_DEBUG_MAGIC)
	{
		ClearDebugReadback();
		return;
	}

	mStatus.debugValid = true;
	mStatus.debugFrameIndex = frameIndex;
	mStatus.sampleCount = words[2];
	mStatus.lowBin = words[5];
	mStatus.highBin = words[6];
	mStatus.lowLogLuminance = AsFloat(words[7]);
	mStatus.highLogLuminance = AsFloat(words[8]);
	mStatus.meteredLogLuminance = AsFloat(words[9]);
	mStatus.targetExposure = AsFloat(words[10]);
	mStatus.adaptedExposure = AsFloat(words[11]);
	if (!std::isfinite(mStatus.lowLogLuminance) ||
		!std::isfinite(mStatus.highLogLuminance) ||
		!std::isfinite(mStatus.meteredLogLuminance) ||
		!std::isfinite(mStatus.targetExposure) ||
		!std::isfinite(mStatus.adaptedExposure))
	{
		ClearDebugReadback();
	}
}

void NRIExposureController::ClearDebugReadback()
{
	mStatus.debugValid = false;
	mStatus.debugFrameIndex = 0;
	mStatus.sampleCount = 0;
	mStatus.lowBin = 0;
	mStatus.highBin = 0;
	mStatus.lowLogLuminance = 0.0f;
	mStatus.highLogLuminance = 0.0f;
	mStatus.meteredLogLuminance = 0.0f;
	mStatus.targetExposure = 1.0f;
	mStatus.adaptedExposure = 1.0f;
}

void NRIExposureController::RequestReset(const char* reason, uint64_t frameIndex)
{
	mStatus.resetPending = true;
	mStatus.resetSerial++;
	mStatus.resetRequestFrame = frameIndex;
	const char* safeReason = reason != nullptr && *reason != '\0' ? reason : "unspecified";
	std::strncpy(mStatus.resetReason, safeReason, sizeof(mStatus.resetReason) - 1u);
	mStatus.resetReason[sizeof(mStatus.resetReason) - 1u] = '\0';
}

bool NRIExposureController::ConsumeResetRequest(uint64_t frameIndex)
{
	if (!mStatus.resetPending)
	{
		return false;
	}

	mStatus.resetPending = false;
	mStatus.resetConsumedFrame = frameIndex;
	return true;
}

const NRITextureResource* NRIExposureController::GetExposureStateTexture(uint32_t index) const
{
	if (index >= 2 || mExposureState[index].texture == nullptr)
	{
		return nullptr;
	}

	return &mExposureState[index];
}

bool NRIExposureController::HasRequiredResources() const
{
	return HasExposureStateTextures() && HasHistogramResources() && HasDebugBuffer();
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

bool NRIExposureController::HasHistogramResources() const
{
	return mHistogramBuffer.buffer != nullptr && mHistogramBuffer.shaderView != nullptr;
}

bool NRIExposureController::HasDebugBuffer() const
{
	return mDebugBuffer.buffer != nullptr && mDebugBuffer.shaderView != nullptr;
}

bool NRIExposureController::HasDebugReadbackBuffer() const
{
	return mDebugReadbackBuffer.buffer != nullptr;
}

void NRIExposureController::ResetStatus()
{
	mStatus.resourcesAllocated = false;
	mStatus.histogramAllocated = false;
	mStatus.debugBufferAllocated = false;
	mStatus.debugReadbackAllocated = false;
	mStatus.resetPending = false;
	mStatus.renderWidth = 0;
	mStatus.renderHeight = 0;
	mStatus.memoryBytes = 0;
	mStatus.resetRequestFrame = 0;
	mStatus.resetConsumedFrame = 0;
	mStatus.resetReason[0] = '\0';
	ClearDebugReadback();
}
