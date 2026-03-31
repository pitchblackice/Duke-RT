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
	bool providerRuntimeSupported = false;
	bool swapChainReady = false;
	bool fullscreenActive = false;
	bool windowModeSupported = false;
	bool lowLatencyAvailable = false;
	bool lowLatencyInterfaceAvailable = false;
	bool lowLatencySwapChainEnabled = false;
	bool waitableSwapChainAvailable = false;
	bool asyncWorkloadAvailable = false;
	bool nativeDeviceAvailable = false;
	bool nativeGraphicsQueueAvailable = false;
	bool nativeSwapChainAvailable = false;
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

struct NRIFrameGenerationLowLatencyState
{
	bool interfaceAvailable = false;
	bool swapChainEnabled = false;
	bool sleepModeConfigured = false;
	bool sleepInvoked = false;
	bool presentBoundarySeen = false;
	nri::LatencySleepMode configuredSleepMode = {};
	nri::Result setSleepModeResult = nri::Result::FAILURE;
	nri::Result latencySleepResult = nri::Result::FAILURE;
	nri::Result simulationStartMarkerResult = nri::Result::FAILURE;
	nri::Result simulationEndMarkerResult = nri::Result::FAILURE;
	nri::Result renderSubmitStartMarkerResult = nri::Result::FAILURE;
	nri::Result renderSubmitEndMarkerResult = nri::Result::FAILURE;
	nri::Result latencyReportResult = nri::Result::FAILURE;
	uint64_t latencySleepCount = 0;
	uint64_t markerCount = 0;
	nri::LatencyReport latencyReport = {};
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
	void OnSimulationEnd(const NRIRenderDevice& frameBuffer);
	void OnRenderSubmitStart(const NRIRenderDevice& frameBuffer);
	void OnRenderSubmitEnd(const NRIRenderDevice& frameBuffer);
	void OnPresentStart(const NRIRenderDevice& frameBuffer);
	void OnPresentEnd(const NRIRenderDevice& frameBuffer, nri::Result presentResult);
	void SetFrameDesc(const NRIFrameGenerationFrameDesc& desc);

	const NRIFrameGenerationPolicy& GetPolicy() const { return mPolicy; }
	const NRIFrameGenerationFrameDesc& GetFrameDesc() const { return mLastFrameDesc; }
	const NRIFrameGenerationLowLatencyState& GetLowLatencyState() const { return mLowLatencyState; }
	bool HasFrameDesc() const { return mHasFrameDesc; }

	static const char* GetProviderName(NRIFrameGenerationProvider provider);
	static const char* GetUiModeName(NRIFrameGenerationUiMode mode);
	static const char* GetWindowModeName(bool fullscreen);
	static const char* GetAvailabilityName(bool available);

private:
	NRIFrameGenerationPolicy BuildPolicy(const NRIRenderDevice& frameBuffer) const;
	bool IsLowLatencyOperational(const NRIRenderDevice& frameBuffer) const;
	void ConfigureLowLatencyMode(const NRIRenderDevice& frameBuffer);
	void SetLowLatencyMarker(const NRIRenderDevice& frameBuffer, nri::LatencyMarker marker, nri::Result& resultSlot);
	void ResetLowLatencyState();

	bool mInitialized = false;
	bool mSwapChainReady = false;
	bool mHasFrameDesc = false;
	bool mHasLoggedPolicy = false;
	NRIFrameGenerationPolicy mPolicy = {};
	NRIFrameGenerationFrameDesc mLastFrameDesc = {};
	NRIFrameGenerationLowLatencyState mLowLatencyState = {};
};
