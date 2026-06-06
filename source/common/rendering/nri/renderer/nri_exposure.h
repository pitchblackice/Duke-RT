#pragma once

#include "../system/nri_local.h"

#include <cstdint>

class NRIRenderDevice;

struct NRIAutoExposureSettings
{
	bool enabled = false;
	bool freeze = false;
	bool stats = false;
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
	uint32_t renderWidth = 0;
	uint32_t renderHeight = 0;
	uint32_t allocationSerial = 0;
	uint64_t memoryBytes = 0;
};

NRIAutoExposureSettings GetNRIAutoExposureSettings(float fallbackManualExposure);

class NRIExposureController
{
public:
	void SetSettings(const NRIAutoExposureSettings& settings) { mSettings = settings; }
	bool ShouldAllocateResources() const { return mSettings.enabled || mSettings.stats; }
	bool HasExposureStateTextures() const;
	bool MatchesRenderSize(uint32_t renderWidth, uint32_t renderHeight) const;
	void MarkResourcesAllocated(uint32_t renderWidth, uint32_t renderHeight, uint64_t memoryBytes);
	void MarkResourcesDestroyed();

	const NRIAutoExposureSettings& GetSettings() const { return mSettings; }
	const NRIAutoExposureStatus& GetStatus() const { return mStatus; }
	const NRITextureResource* GetExposureStateTexture(uint32_t index) const;
	NRITextureResource& GetMutableExposureStateTexture(uint32_t index) { return mExposureState[index < 2 ? index : 0]; }
	float GetFallbackExposure() const { return mSettings.fallbackManualExposure; }

private:
	void ResetStatus();

	NRIAutoExposureSettings mSettings = {};
	NRIAutoExposureStatus mStatus = {};
	NRITextureResource mExposureState[2] = {};
};
