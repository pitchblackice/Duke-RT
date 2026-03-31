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

enum class NRIFrameGenerationColorSource : uint32_t
{
	Unknown = 0,
	Final = 1,
};

enum class NRIFrameGenerationMotionVectorSpace : uint32_t
{
	Unknown = 0,
	ScreenPixels = 1,
};

enum class NRIFrameGenerationMotionVectorDirection : uint32_t
{
	Unknown = 0,
	CurrentToPrevious = 1,
	PreviousToCurrent = 2,
};

enum class NRIFrameGenerationDepthType : uint32_t
{
	Unknown = 0,
	ClipDepth = 1,
	ViewZ = 2,
};

enum class NRIFrameGenerationAdapterRequirement : uint32_t
{
	None = 0,
	MotionVectors = 1,
	Depth = 2,
	MotionAndDepth = 3,
};

struct NRIFrameGenerationRect
{
	uint32_t left = 0;
	uint32_t top = 0;
	uint32_t width = 0;
	uint32_t height = 0;
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
	NRIFrameGenerationRect renderRect = {};
	NRIFrameGenerationRect outputRect = {};
	bool hasPreviousCamera = false;
	bool resetHistory = false;
	bool hasRealFrameTimeMs = false;
	float realFrameTimeMs = 0.0f;
	char resetReason[64] = "none";
	NRIFrameGenerationColorSource hudlessColorSource = NRIFrameGenerationColorSource::Unknown;
	const NRITextureResource* hudlessColor = nullptr;
	const NRITextureResource* uiTexture = nullptr;
	const NRITextureResource* motionVectors = nullptr;
	const NRITextureResource* depth = nullptr;
	float cameraJitter[2] = {};
	float previousCameraJitter[2] = {};
	float motionVectorScale[2] = { 1.0f, 1.0f };
	NRIFrameGenerationMotionVectorSpace motionVectorSpace = NRIFrameGenerationMotionVectorSpace::Unknown;
	NRIFrameGenerationMotionVectorDirection motionVectorDirection = NRIFrameGenerationMotionVectorDirection::Unknown;
	NRIFrameGenerationDepthType depthType = NRIFrameGenerationDepthType::Unknown;
	bool depthInverted = false;
	bool depthInfinite = false;
	float currentViewToClip[16] = {};
	float previousViewToClip[16] = {};
	float currentWorldToView[16] = {};
	float previousWorldToView[16] = {};
};

struct NRIFrameGenerationInputAudit
{
	bool complete = false;
	bool renderRectValid = false;
	bool outputRectValid = false;
	bool currentJitterValid = false;
	bool previousJitterValid = false;
	bool hudlessColorAvailable = false;
	bool motionVectorsAvailable = false;
	bool depthAvailable = false;
	bool motionResolutionMatchesRender = false;
	bool depthResolutionMatchesRender = false;
	bool fsr3MotionCompatible = false;
	bool fsr3DepthCompatible = false;
	bool fsr3PrepareInputsRequired = false;
	NRIFrameGenerationAdapterRequirement adapterRequirement = NRIFrameGenerationAdapterRequirement::None;
	char statusReason[64] = "not-captured";
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
	void SetUiTexture(const NRITextureResource* uiTexture);

	const NRIFrameGenerationPolicy& GetPolicy() const { return mPolicy; }
	const NRIFrameGenerationFrameDesc& GetFrameDesc() const { return mLastFrameDesc; }
	const NRIFrameGenerationInputAudit& GetInputAudit() const { return mLastInputAudit; }
	const NRIFrameGenerationLowLatencyState& GetLowLatencyState() const { return mLowLatencyState; }
	bool HasFrameDesc() const { return mHasFrameDesc; }

	static const char* GetProviderName(NRIFrameGenerationProvider provider);
	static const char* GetUiModeName(NRIFrameGenerationUiMode mode);
	static const char* GetColorSourceName(NRIFrameGenerationColorSource source);
	static const char* GetMotionVectorSpaceName(NRIFrameGenerationMotionVectorSpace space);
	static const char* GetMotionVectorDirectionName(NRIFrameGenerationMotionVectorDirection direction);
	static const char* GetDepthTypeName(NRIFrameGenerationDepthType type);
	static const char* GetAdapterRequirementName(NRIFrameGenerationAdapterRequirement requirement);
	static const char* GetWindowModeName(bool fullscreen);
	static const char* GetAvailabilityName(bool available);

private:
	NRIFrameGenerationPolicy BuildPolicy(const NRIRenderDevice& frameBuffer) const;
	NRIFrameGenerationInputAudit BuildInputAudit(const NRIFrameGenerationFrameDesc& desc) const;
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
	NRIFrameGenerationInputAudit mLastInputAudit = {};
	NRIFrameGenerationLowLatencyState mLowLatencyState = {};
};
