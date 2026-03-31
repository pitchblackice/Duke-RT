#pragma once

#include "../renderer/nri_resources.h"
#include "../system/nri_local.h"

#include <cstdint>

class NRIRenderDevice;

enum class NRIFrameGenerationProvider : uint32_t
{
	Off = 0,
	FSR3 = 1,
};

enum class NRIFrameGenerationUiMode : uint32_t
{
	Auto = 0,
	Hudless = 1,
	UiTexture = 2,
	PresentCallback = 3,
};

struct NRIFrameGenerationPolicy
{
	bool initialized = false;
	bool requestedEnabled = false;
	bool resolvedEnabled = false;
	bool apiSupported = false;
	bool shaderModelSupported = false;
	bool providerImplemented = false;
	bool swapChainReady = false;
	uint32_t shaderModel = 0;
	const char* selectedApiName = "unknown";
	const char* resolvedReason = "not-initialized";
	NRIFrameGenerationProvider requestedProvider = NRIFrameGenerationProvider::Off;
	NRIFrameGenerationProvider resolvedProvider = NRIFrameGenerationProvider::Off;
	NRIFrameGenerationUiMode requestedUiMode = NRIFrameGenerationUiMode::Auto;
	NRIFrameGenerationUiMode resolvedUiMode = NRIFrameGenerationUiMode::Auto;
	bool requestedAsync = false;
	bool resolvedAsync = false;
	bool requestedLowLatency = false;
	bool resolvedLowLatency = false;
};

struct NRIFrameGenerationFrameDesc
{
	uint64_t frameId = 0;
	uint32_t renderWidth = 0;
	uint32_t renderHeight = 0;
	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	bool hasPreviousCamera = false;
	bool resetHistory = false;
	bool hasRealFrameTimeMs = false;
	float realFrameTimeMs = 0.0f;
	char resetReason[64] = "none";
	const NRITextureResource* hudlessColor = nullptr;
	const NRITextureResource* motionVectors = nullptr;
	const NRITextureResource* depth = nullptr;
	float cameraJitter[2] = {};
	float currentViewToClip[16] = {};
	float previousViewToClip[16] = {};
	float currentWorldToView[16] = {};
	float previousWorldToView[16] = {};
};

class NRIFrameGenerationContext
{
public:
	void Initialize(const NRIRenderDevice& frameBuffer);
	void Shutdown();
	void RefreshPolicy(const NRIRenderDevice& frameBuffer, bool logChanges);
	void OnSwapChainCreated(const NRIRenderDevice& frameBuffer);
	void OnSwapChainDestroyed(const NRIRenderDevice& frameBuffer);
	void BeginFrame(const NRIRenderDevice& frameBuffer);
	void EndFrame(const NRIRenderDevice& frameBuffer);
	void SetFrameDesc(const NRIFrameGenerationFrameDesc& desc);

	const NRIFrameGenerationPolicy& GetPolicy() const { return mPolicy; }
	const NRIFrameGenerationFrameDesc& GetFrameDesc() const { return mLastFrameDesc; }
	bool HasFrameDesc() const { return mHasFrameDesc; }

	static const char* GetProviderName(NRIFrameGenerationProvider provider);
	static const char* GetUiModeName(NRIFrameGenerationUiMode mode);

private:
	NRIFrameGenerationPolicy BuildPolicy(const NRIRenderDevice& frameBuffer) const;

	bool mInitialized = false;
	bool mSwapChainReady = false;
	bool mHasFrameDesc = false;
	bool mHasLoggedPolicy = false;
	NRIFrameGenerationPolicy mPolicy = {};
	NRIFrameGenerationFrameDesc mLastFrameDesc = {};
};
