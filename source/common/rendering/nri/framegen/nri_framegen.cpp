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
		return requested == NRIFrameGenerationUiMode::Auto ? NRIFrameGenerationUiMode::UiTexture : requested;
	}

	static bool ArePoliciesEquivalent(const NRIFrameGenerationPolicy& a, const NRIFrameGenerationPolicy& b)
	{
		return
			a.initialized == b.initialized &&
			a.requestedEnabled == b.requestedEnabled &&
			a.resolvedEnabled == b.resolvedEnabled &&
			a.apiSupported == b.apiSupported &&
			a.shaderModelSupported == b.shaderModelSupported &&
			a.providerImplemented == b.providerImplemented &&
			a.swapChainReady == b.swapChainReady &&
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
		Printf("NRI frame generation policy: requested=%s provider=%s resolved=%s ui=%s->%s async=%s->%s latency=%s->%s api=%s shader_model=%u.%u swapchain=%s reason=%s\n",
			mPolicy.requestedEnabled ? "on" : "off",
			GetProviderName(mPolicy.requestedProvider),
			GetProviderName(mPolicy.resolvedProvider),
			GetUiModeName(mPolicy.requestedUiMode),
			GetUiModeName(mPolicy.resolvedUiMode),
			mPolicy.requestedAsync ? "on" : "off",
			mPolicy.resolvedAsync ? "on" : "off",
			mPolicy.requestedLowLatency ? "on" : "off",
			mPolicy.resolvedLowLatency ? "on" : "off",
			mPolicy.selectedApiName,
			mPolicy.shaderModel / 10u,
			mPolicy.shaderModel % 10u,
			mPolicy.swapChainReady ? "ready" : "cold",
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

	const nri::GraphicsAPI api = frameBuffer.GetSelectedAPI();
	policy.selectedApiName = GetApiName(api);
	policy.apiSupported = api == nri::GraphicsAPI::D3D12;

	if (frameBuffer.mDevice != nullptr)
	{
		policy.shaderModel = frameBuffer.mCore.GetDeviceDesc(*frameBuffer.mDevice).shaderModel;
	}
	policy.shaderModelSupported = frameBuffer.mDevice != nullptr && policy.shaderModel >= 62u;

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

	if (!policy.shaderModelSupported)
	{
		policy.resolvedProvider = NRIFrameGenerationProvider::Off;
		policy.resolvedReason = "shader-model-below-6.2";
		return policy;
	}

	policy.providerImplemented = false;
	policy.resolvedProvider = NRIFrameGenerationProvider::Off;
	policy.resolvedReason = "provider-stubbed-phase0-1";
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
