#include "nri_framegen.h"

#include "../system/nri_renderdevice.h"
#include "c_cvars.h"
#include "printf.h"

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
}

void NRIFrameGenerationContext::RefreshPolicy(const NRIRenderDevice& frameBuffer, bool logChanges)
{
	const NRIFrameGenerationPolicy newPolicy = BuildPolicy(frameBuffer);
	const bool changed = !ArePoliciesEquivalent(mPolicy, newPolicy);
	mPolicy = newPolicy;

	if (logChanges && (!mHasLoggedPolicy || changed))
	{
		Printf("NRI frame generation policy: requested=%s provider=%s resolved=%s api=%s shader_model=%u.%u window=%s low_latency=%s->%s(avail=%s) async=%s->%s(avail=%s) ui=%s->%s swapchain=%s native=device:%s queue:%s swapchain:%s waitable=%s runtime=%s reason=%s\n",
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
		policy.lowLatencyAvailable = !!deviceDesc.features.lowLatency;
		policy.waitableSwapChainAvailable = !!deviceDesc.features.waitableSwapChain;
	}
	policy.shaderModelSupported = frameBuffer.mDevice != nullptr && policy.shaderModel >= 62u;
	policy.providerRuntimeSupported = false;
	policy.asyncWorkloadAvailable = false;
	policy.resolvedAsync = false;
	policy.resolvedLowLatency = false;

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
	RefreshPolicy(frameBuffer, true);
}

void NRIFrameGenerationContext::OnSwapChainDestroyed(const NRIRenderDevice& frameBuffer)
{
	mSwapChainReady = false;
	RefreshPolicy(frameBuffer, false);
}

void NRIFrameGenerationContext::BeginFrame(const NRIRenderDevice& frameBuffer)
{
	RefreshPolicy(frameBuffer, true);
}

void NRIFrameGenerationContext::EndFrame(const NRIRenderDevice& frameBuffer)
{
	RefreshPolicy(frameBuffer, false);
}

void NRIFrameGenerationContext::SetFrameDesc(const NRIFrameGenerationFrameDesc& desc)
{
	mLastFrameDesc = desc;
	mHasFrameDesc = true;
}
