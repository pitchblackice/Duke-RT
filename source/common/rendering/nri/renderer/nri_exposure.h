#pragma once

#include "../system/nri_local.h"
#include "nri_resources.h"

#include <cstdint>

class NRIRenderDevice;

static constexpr uint32_t NRI_AUTO_EXPOSURE_MAX_HISTOGRAM_BINS = 256;
static constexpr uint32_t NRI_AUTO_EXPOSURE_DEBUG_WORD_COUNT = 16;

enum class NRIAutoExposureMeteringMode : uint32_t
{
	FullFrame = 0,
	CenterWeighted = 1,
	BrightTailSuppressed = 2,
};

struct NRIAutoExposureSettings
{
	bool enabled = false;
	bool freeze = false;
	bool stats = false;
	bool hdrControlsActive = false;
	NRIAutoExposureMeteringMode meteringMode = NRIAutoExposureMeteringMode::FullFrame;
	uint32_t histogramBinCount = 256;
	uint32_t sampleStep = 2;
	float targetLuminance = 0.18f;
	float minExposure = 0.125f;
	float maxExposure = 8.0f;
	float exposureBias = 1.0f;
	float lowPercentile = 1.0f;
	float highPercentile = 99.0f;
	float adaptUpSpeed = 3.0f;
	float adaptDownSpeed = 1.0f;
	float fallbackManualExposure = 1.0f;
};

struct NRIAutoExposureStatus
{
	bool resourcesAllocated = false;
	bool histogramAllocated = false;
	bool debugBufferAllocated = false;
	bool debugReadbackAllocated = false;
	bool debugValid = false;
	bool resetPending = false;
	uint32_t renderWidth = 0;
	uint32_t renderHeight = 0;
	uint32_t allocationSerial = 0;
	uint64_t memoryBytes = 0;
	uint64_t debugFrameIndex = 0;
	uint64_t resetSerial = 0;
	uint64_t resetRequestFrame = 0;
	uint64_t resetConsumedFrame = 0;
	uint32_t sampleCount = 0;
	uint32_t lowBin = 0;
	uint32_t highBin = 0;
	float lowLogLuminance = 0.0f;
	float highLogLuminance = 0.0f;
	float meteredLogLuminance = 0.0f;
	float targetExposure = 1.0f;
	float adaptedExposure = 1.0f;
	char resetReason[64] = {};
};

NRIAutoExposureSettings GetNRIAutoExposureSettings(float fallbackManualExposure, bool hdrControlsActive);
const char* GetNRIAutoExposureResetReasonForSettingsChange(const NRIAutoExposureSettings& previous, const NRIAutoExposureSettings& current);
const char* GetNRIAutoExposureMeteringModeName(NRIAutoExposureMeteringMode mode);

class NRIExposureController
{
public:
	void SetSettings(const NRIAutoExposureSettings& settings) { mSettings = settings; }
	bool ShouldAllocateResources() const { return mSettings.enabled || mSettings.stats; }
	bool HasRequiredResources() const;
	bool HasExposureStateTextures() const;
	bool HasHistogramResources() const;
	bool HasDebugBuffer() const;
	bool HasDebugReadbackBuffer() const;
	bool MatchesRenderSize(uint32_t renderWidth, uint32_t renderHeight) const;
	void MarkResourcesAllocated(uint32_t renderWidth, uint32_t renderHeight, uint64_t memoryBytes);
	void MarkResourcesDestroyed();
	void MarkDebugReadback(uint64_t frameIndex, const uint32_t* words, uint32_t wordCount);
	void ClearDebugReadback();
	void RequestReset(const char* reason, uint64_t frameIndex);
	bool ConsumeResetRequest(uint64_t frameIndex);

	const NRIAutoExposureSettings& GetSettings() const { return mSettings; }
	const NRIAutoExposureStatus& GetStatus() const { return mStatus; }
	const NRITextureResource* GetExposureStateTexture(uint32_t index) const;
	NRITextureResource& GetMutableExposureStateTexture(uint32_t index) { return mExposureState[index < 2 ? index : 0]; }
	NRIBufferResource& GetMutableHistogramBuffer() { return mHistogramBuffer; }
	NRIBufferResource& GetMutableDebugBuffer() { return mDebugBuffer; }
	NRIBufferResource& GetMutableDebugReadbackBuffer() { return mDebugReadbackBuffer; }
	float GetFallbackExposure() const { return mSettings.fallbackManualExposure; }

private:
	void ResetStatus();

	NRIAutoExposureSettings mSettings = {};
	NRIAutoExposureStatus mStatus = {};
	NRITextureResource mExposureState[2] = {};
	NRIBufferResource mHistogramBuffer = {};
	NRIBufferResource mDebugBuffer = {};
	NRIBufferResource mDebugReadbackBuffer = {};
};
