#include "nri_framegen.h"

#include "../system/nri_renderdevice.h"
#include "c_cvars.h"
#include "printf.h"

#include <cstring>

EXTERN_CVAR(Bool, nri_framegen)
EXTERN_CVAR(Int, nri_framegenprovider)
EXTERN_CVAR(Int, nri_framegenui)
EXTERN_CVAR(Bool, nri_framegenasync)
EXTERN_CVAR(Bool, nri_framegenlatency)

namespace
{
	static const char* GetApiName(nri::GraphicsAPI api)
	{
		switch (api)
		{
		case nri::GraphicsAPI::D3D12: return "d3d12";
		case nri::GraphicsAPI::VK: return "vulkan";
		default: return "unknown";
		}
	}

	static NRIFrameGenerationProvider GetRequestedProvider()
	{
		switch ((int)nri_framegenprovider)
		{
		default:
		case 0: return NRIFrameGenerationProvider::Off;
		case 1: return NRIFrameGenerationProvider::FSR3;
		}
	}

	static NRIFrameGenerationUiMode GetRequestedUiMode()
	{
		switch ((int)nri_framegenui)
		{
		default:
		case 0: return NRIFrameGenerationUiMode::Auto;
		case 1: return NRIFrameGenerationUiMode::Hudless;
		case 2: return NRIFrameGenerationUiMode::UiTexture;
		case 3: return NRIFrameGenerationUiMode::PresentCallback;
		}
	}

	static NRIFrameGenerationUiMode ResolveUiMode(NRIFrameGenerationUiMode requested)
	{
		switch (requested)
		{
		default:
		case NRIFrameGenerationUiMode::Auto:
			return NRIFrameGenerationUiMode::UiTexture;
		case NRIFrameGenerationUiMode::PresentCallback:
			// Present callbacks stay out of scope until the proxy-swapchain path exists.
			return NRIFrameGenerationUiMode::UiTexture;
		case NRIFrameGenerationUiMode::Hudless:
		case NRIFrameGenerationUiMode::UiTexture:
			return requested;
		}
	}

	static bool ArePoliciesEquivalent(const NRIFrameGenerationPolicy& a, const NRIFrameGenerationPolicy& b)
	{
		return
			a.initialized == b.initialized &&
			a.requestedEnabled == b.requestedEnabled &&
			a.resolvedEnabled == b.resolvedEnabled &&
			a.apiSupported == b.apiSupported &&
			a.shaderModelSupported == b.shaderModelSupported &&
			a.providerRuntimeSupported == b.providerRuntimeSupported &&
			a.swapChainReady == b.swapChainReady &&
			a.fullscreenActive == b.fullscreenActive &&
			a.windowModeSupported == b.windowModeSupported &&
			a.lowLatencyAvailable == b.lowLatencyAvailable &&
			a.lowLatencyInterfaceAvailable == b.lowLatencyInterfaceAvailable &&
			a.lowLatencySwapChainEnabled == b.lowLatencySwapChainEnabled &&
			a.waitableSwapChainAvailable == b.waitableSwapChainAvailable &&
			a.asyncWorkloadAvailable == b.asyncWorkloadAvailable &&
			a.nativeDeviceAvailable == b.nativeDeviceAvailable &&
			a.nativeGraphicsQueueAvailable == b.nativeGraphicsQueueAvailable &&
			a.nativeSwapChainAvailable == b.nativeSwapChainAvailable &&
			a.shaderModel == b.shaderModel &&
			a.selectedApiName == b.selectedApiName &&
			a.resolvedReason == b.resolvedReason &&
			a.requestedProvider == b.requestedProvider &&
			a.resolvedProvider == b.resolvedProvider &&
			a.requestedUiMode == b.requestedUiMode &&
			a.resolvedUiMode == b.resolvedUiMode &&
			a.requestedAsync == b.requestedAsync &&
			a.resolvedAsync == b.resolvedAsync &&
			a.requestedLowLatency == b.requestedLowLatency &&
			a.resolvedLowLatency == b.resolvedLowLatency;
	}

}

const char* NRIFrameGenerationContext::GetProviderName(NRIFrameGenerationProvider provider)
{
	switch (provider)
	{
	default:
	case NRIFrameGenerationProvider::Off: return "off";
	case NRIFrameGenerationProvider::FSR3: return "fsr3";
	}
}

const char* NRIFrameGenerationContext::GetUiModeName(NRIFrameGenerationUiMode mode)
{
	switch (mode)
	{
	default:
	case NRIFrameGenerationUiMode::Auto: return "auto";
	case NRIFrameGenerationUiMode::Hudless: return "hudless";
	case NRIFrameGenerationUiMode::UiTexture: return "ui_texture";
	case NRIFrameGenerationUiMode::PresentCallback: return "present_callback";
	}
}

const char* NRIFrameGenerationContext::GetColorSourceName(NRIFrameGenerationColorSource source)
{
	switch (source)
	{
	default:
	case NRIFrameGenerationColorSource::Unknown: return "unknown";
	case NRIFrameGenerationColorSource::Final: return "final";
	}
}

const char* NRIFrameGenerationContext::GetMotionVectorSpaceName(NRIFrameGenerationMotionVectorSpace space)
{
	switch (space)
	{
	default:
	case NRIFrameGenerationMotionVectorSpace::Unknown: return "unknown";
	case NRIFrameGenerationMotionVectorSpace::ScreenPixels: return "screen_pixels";
	}
}

const char* NRIFrameGenerationContext::GetMotionVectorDirectionName(NRIFrameGenerationMotionVectorDirection direction)
{
	switch (direction)
	{
	default:
	case NRIFrameGenerationMotionVectorDirection::Unknown: return "unknown";
	case NRIFrameGenerationMotionVectorDirection::CurrentToPrevious: return "current_to_previous";
	case NRIFrameGenerationMotionVectorDirection::PreviousToCurrent: return "previous_to_current";
	}
}

const char* NRIFrameGenerationContext::GetDepthTypeName(NRIFrameGenerationDepthType type)
{
	switch (type)
	{
	default:
	case NRIFrameGenerationDepthType::Unknown: return "unknown";
	case NRIFrameGenerationDepthType::ClipDepth: return "clip_depth";
	case NRIFrameGenerationDepthType::ViewZ: return "view_z";
	}
}

const char* NRIFrameGenerationContext::GetAdapterRequirementName(NRIFrameGenerationAdapterRequirement requirement)
{
	switch (requirement)
	{
	default:
	case NRIFrameGenerationAdapterRequirement::None: return "none";
	case NRIFrameGenerationAdapterRequirement::MotionVectors: return "motion";
	case NRIFrameGenerationAdapterRequirement::Depth: return "depth";
	case NRIFrameGenerationAdapterRequirement::MotionAndDepth: return "motion+depth";
	}
}

const char* NRIFrameGenerationContext::GetWindowModeName(bool fullscreen)
{
	return fullscreen ? "fullscreen" : "windowed";
}

const char* NRIFrameGenerationContext::GetAvailabilityName(bool available)
{
	return available ? "yes" : "no";
}

void NRIFrameGenerationContext::Initialize(const NRIRenderDevice& frameBuffer)
{
	mInitialized = true;
	mSwapChainReady = frameBuffer.mSwapChain != nullptr;
	ResetLowLatencyState();
	RefreshPolicy(frameBuffer, true);
}

void NRIFrameGenerationContext::Shutdown()
{
	mInitialized = false;
	mSwapChainReady = false;
	mHasFrameDesc = false;
	mHasLoggedPolicy = false;
	mPolicy = {};
	mLastFrameDesc = {};
	mLastInputAudit = {};
	ResetLowLatencyState();
}

void NRIFrameGenerationContext::RefreshPolicy(const NRIRenderDevice& frameBuffer, bool logChanges)
{
	const NRIFrameGenerationPolicy newPolicy = BuildPolicy(frameBuffer);
	const bool changed = !ArePoliciesEquivalent(mPolicy, newPolicy);
	mPolicy = newPolicy;
	mLowLatencyState.interfaceAvailable = mPolicy.lowLatencyInterfaceAvailable;
	mLowLatencyState.swapChainEnabled = mPolicy.lowLatencySwapChainEnabled;

	if (logChanges && (!mHasLoggedPolicy || changed))
	{
		Printf("NRI frame generation policy: requested=%s provider=%s resolved=%s api=%s shader_model=%u.%u window=%s low_latency=%s->%s(avail=%s iface=%s swapchain=%s) async=%s->%s(avail=%s) ui=%s->%s swapchain=%s native=device:%s queue:%s swapchain:%s waitable=%s runtime=%s reason=%s\n",
			mPolicy.requestedEnabled ? "on" : "off",
			GetProviderName(mPolicy.requestedProvider),
			GetProviderName(mPolicy.resolvedProvider),
			mPolicy.selectedApiName,
			mPolicy.shaderModel / 10u,
			mPolicy.shaderModel % 10u,
			GetWindowModeName(mPolicy.fullscreenActive),
			mPolicy.requestedLowLatency ? "on" : "off",
			mPolicy.resolvedLowLatency ? "on" : "off",
			GetAvailabilityName(mPolicy.lowLatencyAvailable),
			GetAvailabilityName(mPolicy.lowLatencyInterfaceAvailable),
			GetAvailabilityName(mPolicy.lowLatencySwapChainEnabled),
			mPolicy.requestedAsync ? "on" : "off",
			mPolicy.resolvedAsync ? "on" : "off",
			GetAvailabilityName(mPolicy.asyncWorkloadAvailable),
			GetUiModeName(mPolicy.requestedUiMode),
			GetUiModeName(mPolicy.resolvedUiMode),
			mPolicy.swapChainReady ? "ready" : "cold",
			mPolicy.nativeDeviceAvailable ? "ok" : "missing",
			mPolicy.nativeGraphicsQueueAvailable ? "ok" : "missing",
			mPolicy.nativeSwapChainAvailable ? "ok" : "missing",
			GetAvailabilityName(mPolicy.waitableSwapChainAvailable),
			GetAvailabilityName(mPolicy.providerRuntimeSupported),
			mPolicy.resolvedReason);
	}

	mHasLoggedPolicy = true;
}

NRIFrameGenerationPolicy NRIFrameGenerationContext::BuildPolicy(const NRIRenderDevice& frameBuffer) const
{
	NRIFrameGenerationPolicy policy = {};
	policy.initialized = true;
	policy.requestedEnabled = !!nri_framegen;
	policy.requestedProvider = GetRequestedProvider();
	policy.requestedUiMode = GetRequestedUiMode();
	policy.requestedAsync = !!nri_framegenasync;
	policy.requestedLowLatency = !!nri_framegenlatency;
	policy.resolvedUiMode = ResolveUiMode(policy.requestedUiMode);
	policy.swapChainReady = mSwapChainReady;
	policy.fullscreenActive = frameBuffer.IsFullscreenModeActive();
	policy.windowModeSupported = !policy.fullscreenActive;

	const nri::GraphicsAPI api = frameBuffer.GetSelectedAPI();
	policy.selectedApiName = GetApiName(api);
	policy.apiSupported = api == nri::GraphicsAPI::D3D12;

#ifdef _WIN32
	policy.nativeDeviceAvailable = frameBuffer.GetNativeD3D12Device() != nullptr;
	policy.nativeGraphicsQueueAvailable = frameBuffer.GetNativeD3D12GraphicsQueue() != nullptr;
	policy.nativeSwapChainAvailable = frameBuffer.GetNativeD3D12SwapChain() != nullptr;
#endif

	if (frameBuffer.mDevice != nullptr)
	{
		const nri::DeviceDesc& deviceDesc = frameBuffer.mCore.GetDeviceDesc(*frameBuffer.mDevice);
		policy.shaderModel = deviceDesc.shaderModel;
		policy.lowLatencyInterfaceAvailable = frameBuffer.mLowLatency.SetLatencySleepMode != nullptr &&
			frameBuffer.mLowLatency.SetLatencyMarker != nullptr &&
			frameBuffer.mLowLatency.LatencySleep != nullptr &&
			frameBuffer.mLowLatency.GetLatencyReport != nullptr;
		policy.lowLatencyAvailable = !!deviceDesc.features.lowLatency && policy.lowLatencyInterfaceAvailable;
		policy.lowLatencySwapChainEnabled = policy.swapChainReady &&
			(((uint32_t)frameBuffer.mSwapChainFlags & (uint32_t)nri::SwapChainBits::ALLOW_LOW_LATENCY) != 0);
		policy.waitableSwapChainAvailable = !!deviceDesc.features.waitableSwapChain;
	}
	policy.shaderModelSupported = frameBuffer.mDevice != nullptr && policy.shaderModel >= 62u;
	policy.providerRuntimeSupported = false;
	policy.asyncWorkloadAvailable = false;
	policy.resolvedAsync = false;
	policy.resolvedLowLatency =
		policy.requestedEnabled &&
		policy.requestedLowLatency &&
		policy.apiSupported &&
		policy.windowModeSupported &&
		policy.lowLatencyAvailable &&
		policy.lowLatencySwapChainEnabled;

	if (!policy.requestedEnabled)
	{
		policy.resolvedProvider = NRIFrameGenerationProvider::Off;
		policy.resolvedReason = "disabled-by-cvar";
		return policy;
	}

	if (policy.requestedProvider == NRIFrameGenerationProvider::Off)
	{
		policy.resolvedProvider = NRIFrameGenerationProvider::Off;
		policy.resolvedReason = "provider-off";
		return policy;
	}

	if (frameBuffer.mDevice == nullptr)
	{
		policy.resolvedProvider = NRIFrameGenerationProvider::Off;
		policy.resolvedReason = "device-unavailable";
		return policy;
	}

	if (!policy.apiSupported)
	{
		policy.resolvedProvider = NRIFrameGenerationProvider::Off;
		policy.resolvedReason = "api-not-d3d12";
		return policy;
	}

	if (!policy.windowModeSupported)
	{
		policy.resolvedProvider = NRIFrameGenerationProvider::Off;
		policy.resolvedReason = "fullscreen-not-supported";
		return policy;
	}

	if (!policy.shaderModelSupported)
	{
		policy.resolvedProvider = NRIFrameGenerationProvider::Off;
		policy.resolvedReason = "shader-model-below-6.2";
		return policy;
	}

	if (!policy.nativeDeviceAvailable)
	{
		policy.resolvedProvider = NRIFrameGenerationProvider::Off;
		policy.resolvedReason = "native-device-unavailable";
		return policy;
	}

	if (!policy.nativeGraphicsQueueAvailable)
	{
		policy.resolvedProvider = NRIFrameGenerationProvider::Off;
		policy.resolvedReason = "native-queue-unavailable";
		return policy;
	}

	if (!policy.swapChainReady)
	{
		policy.resolvedProvider = NRIFrameGenerationProvider::Off;
		policy.resolvedReason = "swapchain-cold";
		return policy;
	}

	if (!policy.nativeSwapChainAvailable)
	{
		policy.resolvedProvider = NRIFrameGenerationProvider::Off;
		policy.resolvedReason = "native-swapchain-unresolved";
		return policy;
	}

	if (!policy.providerRuntimeSupported)
	{
		policy.resolvedProvider = NRIFrameGenerationProvider::Off;
		policy.resolvedReason = "provider-runtime-unavailable";
		return policy;
	}

	policy.resolvedEnabled = true;
	policy.resolvedAsync = policy.requestedAsync && policy.asyncWorkloadAvailable;
	policy.resolvedLowLatency = policy.requestedLowLatency && policy.lowLatencyAvailable;
	policy.resolvedProvider = policy.requestedProvider;
	policy.resolvedReason = "enabled";
	return policy;
}

void NRIFrameGenerationContext::OnSwapChainCreated(const NRIRenderDevice& frameBuffer)
{
	mSwapChainReady = true;
	ResetLowLatencyState();
	RefreshPolicy(frameBuffer, false);
	ConfigureLowLatencyMode(frameBuffer);
	RefreshPolicy(frameBuffer, true);
}

void NRIFrameGenerationContext::OnSwapChainDestroyed(const NRIRenderDevice& frameBuffer)
{
	mSwapChainReady = false;
	ResetLowLatencyState();
	RefreshPolicy(frameBuffer, false);
}

void NRIFrameGenerationContext::BeginFrame(const NRIRenderDevice& frameBuffer)
{
	RefreshPolicy(frameBuffer, true);
	mLowLatencyState.sleepInvoked = false;
	mLowLatencyState.presentBoundarySeen = false;
	mLowLatencyState.latencySleepResult = nri::Result::FAILURE;
	mLowLatencyState.simulationStartMarkerResult = nri::Result::FAILURE;
	mLowLatencyState.simulationEndMarkerResult = nri::Result::FAILURE;
	mLowLatencyState.renderSubmitStartMarkerResult = nri::Result::FAILURE;
	mLowLatencyState.renderSubmitEndMarkerResult = nri::Result::FAILURE;
	mLowLatencyState.latencyReportResult = nri::Result::FAILURE;
	mLowLatencyState.latencyReport = {};
	ConfigureLowLatencyMode(frameBuffer);
	if (!IsLowLatencyOperational(frameBuffer))
	{
		return;
	}

	if (frameBuffer.mSwapChain != nullptr)
	{
		mLowLatencyState.latencySleepResult = frameBuffer.mLowLatency.LatencySleep(*frameBuffer.mSwapChain);
		mLowLatencyState.sleepInvoked = mLowLatencyState.latencySleepResult == nri::Result::SUCCESS;
		if (mLowLatencyState.sleepInvoked)
		{
			mLowLatencyState.latencySleepCount++;
		}
	}

	SetLowLatencyMarker(frameBuffer, nri::LatencyMarker::SIMULATION_START, mLowLatencyState.simulationStartMarkerResult);
}

void NRIFrameGenerationContext::EndFrame(const NRIRenderDevice& frameBuffer)
{
	RefreshPolicy(frameBuffer, false);
}

void NRIFrameGenerationContext::SetFrameDesc(const NRIFrameGenerationFrameDesc& desc)
{
	mLastFrameDesc = desc;
	mLastInputAudit = BuildInputAudit(desc);
	mHasFrameDesc = true;
}

void NRIFrameGenerationContext::SetUiTexture(const NRITextureResource* uiTexture)
{
	if (!mHasFrameDesc)
	{
		return;
	}

	mLastFrameDesc.uiTexture = uiTexture;
}

NRIFrameGenerationInputAudit NRIFrameGenerationContext::BuildInputAudit(const NRIFrameGenerationFrameDesc& desc) const
{
	NRIFrameGenerationInputAudit audit = {};
	audit.renderRectValid =
		desc.renderRect.left == 0u &&
		desc.renderRect.top == 0u &&
		desc.renderRect.width == desc.renderWidth &&
		desc.renderRect.height == desc.renderHeight;
	audit.outputRectValid =
		desc.outputRect.left == 0u &&
		desc.outputRect.top == 0u &&
		desc.outputRect.width == desc.outputWidth &&
		desc.outputRect.height == desc.outputHeight;
	audit.currentJitterValid = true;
	audit.previousJitterValid = desc.hasPreviousCamera;
	audit.hudlessColorAvailable = desc.hudlessColor != nullptr;
	audit.motionVectorsAvailable = desc.motionVectors != nullptr;
	audit.depthAvailable = desc.depth != nullptr;
	audit.motionResolutionMatchesRender =
		audit.motionVectorsAvailable &&
		desc.motionVectors->width == desc.renderWidth &&
		desc.motionVectors->height == desc.renderHeight;
	audit.depthResolutionMatchesRender =
		audit.depthAvailable &&
		desc.depth->width == desc.renderWidth &&
		desc.depth->height == desc.renderHeight;
	audit.fsr3MotionCompatible =
		audit.motionResolutionMatchesRender &&
		desc.motionVectorSpace == NRIFrameGenerationMotionVectorSpace::ScreenPixels &&
		desc.motionVectorDirection == NRIFrameGenerationMotionVectorDirection::CurrentToPrevious &&
		desc.motionVectorScale[0] == 1.0f &&
		desc.motionVectorScale[1] == 1.0f;
	audit.fsr3DepthCompatible =
		audit.depthResolutionMatchesRender &&
		desc.depthType == NRIFrameGenerationDepthType::ClipDepth;
	audit.fsr3PrepareInputsRequired = true;
	audit.complete =
		audit.renderRectValid &&
		audit.outputRectValid &&
		audit.currentJitterValid &&
		audit.hudlessColorAvailable &&
		audit.motionVectorsAvailable &&
		audit.depthAvailable;

	if (!audit.complete)
	{
		std::strncpy(audit.statusReason, "missing-required-input", std::size(audit.statusReason) - 1u);
		audit.statusReason[std::size(audit.statusReason) - 1u] = '\0';
		audit.adapterRequirement = NRIFrameGenerationAdapterRequirement::MotionAndDepth;
		return audit;
	}

	if (!audit.fsr3MotionCompatible && !audit.fsr3DepthCompatible)
	{
		audit.adapterRequirement = NRIFrameGenerationAdapterRequirement::MotionAndDepth;
		std::strncpy(audit.statusReason, "fsr3-motion-depth-adapter", std::size(audit.statusReason) - 1u);
	}
	else if (!audit.fsr3MotionCompatible)
	{
		audit.adapterRequirement = NRIFrameGenerationAdapterRequirement::MotionVectors;
		std::strncpy(audit.statusReason, "fsr3-motion-adapter", std::size(audit.statusReason) - 1u);
	}
	else if (!audit.fsr3DepthCompatible)
	{
		audit.adapterRequirement = NRIFrameGenerationAdapterRequirement::Depth;
		std::strncpy(audit.statusReason, "fsr3-depth-adapter", std::size(audit.statusReason) - 1u);
	}
	else
	{
		audit.adapterRequirement = NRIFrameGenerationAdapterRequirement::None;
		std::strncpy(audit.statusReason, "fsr3-prepare-pass-pending", std::size(audit.statusReason) - 1u);
	}

	audit.statusReason[std::size(audit.statusReason) - 1u] = '\0';
	return audit;
}

void NRIFrameGenerationContext::OnSimulationEnd(const NRIRenderDevice& frameBuffer)
{
	SetLowLatencyMarker(frameBuffer, nri::LatencyMarker::SIMULATION_END, mLowLatencyState.simulationEndMarkerResult);
}

void NRIFrameGenerationContext::OnRenderSubmitStart(const NRIRenderDevice& frameBuffer)
{
	SetLowLatencyMarker(frameBuffer, nri::LatencyMarker::RENDER_SUBMIT_START, mLowLatencyState.renderSubmitStartMarkerResult);
}

void NRIFrameGenerationContext::OnRenderSubmitEnd(const NRIRenderDevice& frameBuffer)
{
	SetLowLatencyMarker(frameBuffer, nri::LatencyMarker::RENDER_SUBMIT_END, mLowLatencyState.renderSubmitEndMarkerResult);
}

void NRIFrameGenerationContext::OnPresentStart(const NRIRenderDevice&)
{
	mLowLatencyState.presentBoundarySeen = true;
}

void NRIFrameGenerationContext::OnPresentEnd(const NRIRenderDevice& frameBuffer, nri::Result presentResult)
{
	mLowLatencyState.presentBoundarySeen = mLowLatencyState.presentBoundarySeen || presentResult == nri::Result::SUCCESS;
	if (!IsLowLatencyOperational(frameBuffer) || presentResult != nri::Result::SUCCESS || frameBuffer.mSwapChain == nullptr)
	{
		return;
	}

	mLowLatencyState.latencyReportResult = frameBuffer.mLowLatency.GetLatencyReport(*frameBuffer.mSwapChain, mLowLatencyState.latencyReport);
}

bool NRIFrameGenerationContext::IsLowLatencyOperational(const NRIRenderDevice& frameBuffer) const
{
	return
		mPolicy.resolvedLowLatency &&
		frameBuffer.mSwapChain != nullptr &&
		frameBuffer.mLowLatency.SetLatencySleepMode != nullptr &&
		frameBuffer.mLowLatency.SetLatencyMarker != nullptr &&
		frameBuffer.mLowLatency.LatencySleep != nullptr &&
		frameBuffer.mLowLatency.GetLatencyReport != nullptr;
}

void NRIFrameGenerationContext::ConfigureLowLatencyMode(const NRIRenderDevice& frameBuffer)
{
	if (frameBuffer.mSwapChain == nullptr || frameBuffer.mLowLatency.SetLatencySleepMode == nullptr || !mPolicy.lowLatencySwapChainEnabled)
	{
		return;
	}

	nri::LatencySleepMode sleepMode = {};
	sleepMode.minIntervalUs = 0;
	sleepMode.lowLatencyMode = mPolicy.resolvedLowLatency;
	sleepMode.lowLatencyBoost = false;
	mLowLatencyState.configuredSleepMode = sleepMode;
	mLowLatencyState.setSleepModeResult = frameBuffer.mLowLatency.SetLatencySleepMode(*frameBuffer.mSwapChain, sleepMode);
	mLowLatencyState.sleepModeConfigured = mLowLatencyState.setSleepModeResult == nri::Result::SUCCESS;
}

void NRIFrameGenerationContext::SetLowLatencyMarker(const NRIRenderDevice& frameBuffer, nri::LatencyMarker marker, nri::Result& resultSlot)
{
	resultSlot = nri::Result::FAILURE;
	if (!IsLowLatencyOperational(frameBuffer))
	{
		return;
	}

	resultSlot = frameBuffer.mLowLatency.SetLatencyMarker(*frameBuffer.mSwapChain, marker);
	if (resultSlot == nri::Result::SUCCESS)
	{
		mLowLatencyState.markerCount++;
	}
}

void NRIFrameGenerationContext::ResetLowLatencyState()
{
	mLowLatencyState = {};
}
