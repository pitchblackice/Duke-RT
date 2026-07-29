//
// nri_framegen_vk.cpp
//
// Vulkan frame generation for the Linux port.
//
// The Windows implementation (nri_framegen.cpp) drives the AMD FidelityFX
// runtime through amd_fidelityfx_dx12.dll and takes over presentation with an
// FFX-owned DXGI swapchain. This file is the Vulkan counterpart: it loads
// libamd_fidelityfx_vk.so and creates the FFX Vulkan backend plus the
// (backend-agnostic) FrameGeneration effect context from NRI's native Vulkan
// handles.
//
// Presentation is deliberately NOT taken over yet. On Vulkan the FFX frame
// generation swapchain does not wrap an existing swapchain the way the DX12
// path does - it *replaces* the VkSwapchainKHR and requires every
// acquire/present call to be routed through FFX-supplied replacement
// functions. NRI owns its swapchain and exposes neither the VkSwapchainKHR nor
// a way to adopt an external one, so that bridge needs a separate presentation
// path built on the SDL surface. Until then Present() declines and the renderer
// keeps using the native path, and the policy reports exactly why.
//

#include "nri_framegen.h"
#include "nri_ffx_api.h"

#include "nri/system/nri_renderdevice.h"
#include "c_cvars.h"
#include "printf.h"

#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <string>

#include "Extensions/NRIWrapperVK.h"

EXTERN_CVAR(Bool, nri_framegen)
EXTERN_CVAR(Int, nri_framegenprovider)
EXTERN_CVAR(Int, nri_framegenui)
EXTERN_CVAR(Bool, nri_framegenlatency)
EXTERN_CVAR(Bool, nri_framegenasync)

namespace
{
	// Shipped alongside the executable by the build (see source/CMakeLists.txt).
	constexpr const char* kFfxVulkanLibrary = "libamd_fidelityfx_vk.so";

	// The Vulkan swapchain bridge is not implemented yet; keep the reason in one
	// place so the policy, the present contract and the log all agree.
	constexpr const char* kPresentBridgeUnavailable = "vk-present-bridge-not-implemented";

	NRIFrameGenerationProvider ResolveRequestedProvider()
	{
		const int provider = (int)nri_framegenprovider;
		switch (provider)
		{
		case 1: return NRIFrameGenerationProvider::FSR3;
		default: return NRIFrameGenerationProvider::Off;
		}
	}

	NRIFrameGenerationUiMode ResolveUiMode()
	{
		const int mode = (int)nri_framegenui;
		switch (mode)
		{
		case 1: return NRIFrameGenerationUiMode::Hudless;
		case 2: return NRIFrameGenerationUiMode::UiTexture;
		default: return NRIFrameGenerationUiMode::Auto;
		}
	}
}

// ---------------------------------------------------------------------------
// Runtime loading
// ---------------------------------------------------------------------------

bool NRIFrameGenerationContext::EnsureProviderRuntime(const NRIRenderDevice& frameBuffer)
{
	if (mFfxModule != nullptr)
	{
		return mFfxCreateContextFn != nullptr;
	}

	mFfxModule = dlopen(kFfxVulkanLibrary, RTLD_NOW | RTLD_LOCAL);
	if (mFfxModule == nullptr)
	{
		// A bare soname only searches the system paths; the runtime is staged
		// next to the executable, so look there too.
		char exePath[4096] = {};
		const ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
		if (len > 0)
		{
			std::string path(exePath, (size_t)len);
			const size_t slash = path.find_last_of('/');
			if (slash != std::string::npos)
			{
				path = path.substr(0, slash + 1) + kFfxVulkanLibrary;
				mFfxModule = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
			}
		}
	}

	if (mFfxModule == nullptr)
	{
		mProviderState.runtimeLoaded = false;
		mProviderState.runtimeFunctionsLoaded = false;
		Printf(TEXTCOLOR_YELLOW "NRI frame generation: could not load %s (%s)\n", kFfxVulkanLibrary, dlerror());
		return false;
	}

	mFfxCreateContextFn  = dlsym(mFfxModule, "ffxCreateContext");
	mFfxDestroyContextFn = dlsym(mFfxModule, "ffxDestroyContext");
	mFfxConfigureFn      = dlsym(mFfxModule, "ffxConfigure");
	mFfxQueryFn          = dlsym(mFfxModule, "ffxQuery");
	mFfxDispatchFn       = dlsym(mFfxModule, "ffxDispatch");

	if (mFfxCreateContextFn == nullptr || mFfxDestroyContextFn == nullptr ||
		mFfxConfigureFn == nullptr || mFfxQueryFn == nullptr || mFfxDispatchFn == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "NRI frame generation: %s is missing required ffx-api entry points\n", kFfxVulkanLibrary);
		dlclose(mFfxModule);
		mFfxModule = nullptr;
		mFfxCreateContextFn = mFfxDestroyContextFn = mFfxConfigureFn = mFfxQueryFn = mFfxDispatchFn = nullptr;
		mProviderState.runtimeLoaded = true;
		mProviderState.runtimeFunctionsLoaded = false;
		return false;
	}

	mProviderState.runtimeLoaded = true;
	mProviderState.runtimeFunctionsLoaded = true;
	return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void NRIFrameGenerationContext::Initialize(const NRIRenderDevice& frameBuffer)
{
	if (mInitialized)
	{
		return;
	}
	mInitialized = true;
	RefreshPolicy(frameBuffer, true);
}

void NRIFrameGenerationContext::ShutdownProvider()
{
	if (mFfxContext != nullptr && mFfxDestroyContextFn != nullptr)
	{
		auto destroy = reinterpret_cast<PfnFfxDestroyContext>(mFfxDestroyContextFn);
		destroy(reinterpret_cast<ffxContext*>(&mFfxContext), nullptr);
		mFfxContext = nullptr;
	}
	ResetProviderState();
}

void NRIFrameGenerationContext::Shutdown()
{
	ShutdownProvider();
	DestroyProviderPresentBridge();

	if (mFfxModule != nullptr)
	{
		dlclose(mFfxModule);
		mFfxModule = nullptr;
	}
	mFfxCreateContextFn = mFfxDestroyContextFn = mFfxConfigureFn = mFfxQueryFn = mFfxDispatchFn = nullptr;
	mInitialized = false;
	mSwapChainReady = false;
	mHasFrameDesc = false;
	mHasLoggedPolicy = false;
}

void NRIFrameGenerationContext::DestroyProviderPresentBridge()
{
	// Nothing to unwind until the Vulkan swapchain bridge exists.
	mFfxSwapChainContext = nullptr;
	mSwapChainReady = false;
}

void NRIFrameGenerationContext::ResetProviderState()
{
	// Preserve what we know about the loaded runtime across a context reset.
	const bool runtimeLoaded = mProviderState.runtimeLoaded;
	const bool runtimeFunctionsLoaded = mProviderState.runtimeFunctionsLoaded;
	mProviderState = {};
	mProviderState.runtimeLoaded = runtimeLoaded;
	mProviderState.runtimeFunctionsLoaded = runtimeFunctionsLoaded;
}

void NRIFrameGenerationContext::ResetLowLatencyState()
{
	mLowLatencyState = {};
}

// ---------------------------------------------------------------------------
// Provider context
// ---------------------------------------------------------------------------

bool NRIFrameGenerationContext::EnsureProviderContext(const NRIRenderDevice& frameBuffer, const NRIFrameGenerationFrameDesc& desc)
{
	if (mFfxContext != nullptr)
	{
		return true;
	}
	if (!EnsureProviderRuntime(frameBuffer))
	{
		return false;
	}

	nri::Device* device = frameBuffer.GetDevice();
	if (device == nullptr)
	{
		mProviderState.contextCreated = false;
		return false;
	}

	nri::CoreInterface* core = const_cast<NRIRenderDevice&>(frameBuffer).GetCoreInterface();
	if (core == nullptr || core->GetDeviceNativeObject == nullptr)
	{
		mProviderState.contextCreated = false;
		return false;
	}

	nri::WrapperVKInterface wrapperVK = {};
	if (nri::nriGetInterface(*device, NRI_INTERFACE(nri::WrapperVKInterface), &wrapperVK) != nri::Result::SUCCESS)
	{
		mProviderState.contextCreated = false;
		return false;
	}

	void* vkDevice = core->GetDeviceNativeObject(device);
	void* vkPhysicalDevice = reinterpret_cast<void*>(wrapperVK.GetPhysicalDeviceVK(*device));
	void* vkGetDeviceProcAddr = wrapperVK.GetDeviceProcAddrVK(*device);

	if (vkDevice == nullptr || vkPhysicalDevice == nullptr || vkGetDeviceProcAddr == nullptr)
	{
		mProviderState.contextCreated = false;
		return false;
	}

	// The FFX backend descriptor is chained ahead of the effect descriptor.
	ffxCreateBackendVKDesc backendDesc = {};
	backendDesc.header.type = NRI_FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
	backendDesc.header.pNext = nullptr;
	backendDesc.vkDevice = vkDevice;
	backendDesc.vkPhysicalDevice = vkPhysicalDevice;
	backendDesc.vkDeviceProcAddr = vkGetDeviceProcAddr;

	ffxCreateContextDescFrameGeneration createDesc = {};
	createDesc.header.type = NRI_FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
	createDesc.header.pNext = &backendDesc.header;
	createDesc.displaySize.width = desc.outputWidth != 0 ? desc.outputWidth : desc.renderWidth;
	createDesc.displaySize.height = desc.outputHeight != 0 ? desc.outputHeight : desc.renderHeight;
	createDesc.maxRenderSize.width = desc.renderWidth;
	createDesc.maxRenderSize.height = desc.renderHeight;

	auto createContext = reinterpret_cast<PfnFfxCreateContext>(mFfxCreateContextFn);
	const ffxReturnCode_t result = createContext(reinterpret_cast<ffxContext*>(&mFfxContext), &createDesc.header, nullptr);
	if (result != NRI_FFX_API_RETURN_OK)
	{
		mFfxContext = nullptr;
		mProviderState.contextCreated = false;
		Printf(TEXTCOLOR_YELLOW "NRI frame generation: ffxCreateContext failed (%u)\n", (unsigned)result);
		return false;
	}

	mProviderState.contextCreated = true;
	return true;
}

bool NRIFrameGenerationContext::EnsureProviderPresentBridge(const NRIRenderDevice&)
{
	// See the file header: the Vulkan FFX swapchain replaces the VkSwapchainKHR
	// outright, which NRI does not currently allow.
	mSwapChainReady = false;
	mProviderState.presentBridgeReady = false;
	mProviderState.swapChainContextCreated = false;
	return false;
}

// ---------------------------------------------------------------------------
// Policy / reporting
// ---------------------------------------------------------------------------

NRIFrameGenerationSettings NRIFrameGenerationContext::CaptureSettings()
{
	NRIFrameGenerationSettings settings = {};
	settings.enabled = (bool)nri_framegen;
	settings.provider = ResolveRequestedProvider();
	settings.uiMode = ResolveUiMode();
	settings.async = (bool)nri_framegenasync;
	settings.lowLatency = (bool)nri_framegenlatency;
	return settings;
}

NRIFrameGenerationPresentContract NRIFrameGenerationContext::BuildPresentContract(const NRIRenderDevice&) const
{
	NRIFrameGenerationPresentContract contract = {};
	contract.resolvedReason = kPresentBridgeUnavailable;
	return contract;
}

NRIFrameGenerationPolicy NRIFrameGenerationContext::BuildPolicy(const NRIRenderDevice& frameBuffer, const NRIFrameGenerationPresentContract&) const
{
	const NRIFrameGenerationSettings settings = CaptureSettings();

	NRIFrameGenerationPolicy policy = {};
	policy.initialized = mInitialized;
	policy.requestedEnabled = settings.enabled && settings.provider != NRIFrameGenerationProvider::Off;
	// The Vulkan runtime and backend are genuinely available; only presentation
	// is missing, so report the pieces honestly rather than as a flat "no".
	policy.apiSupported = true;
	policy.providerRuntimeSupported = mProviderState.runtimeFunctionsLoaded;
	policy.requestedProvider = settings.provider;
	policy.resolvedProvider = NRIFrameGenerationProvider::Off;
	policy.requestedUiMode = settings.uiMode;
	policy.windowModeSupported = true;
	policy.nativeDeviceAvailable = frameBuffer.GetDevice() != nullptr;
	policy.swapChainReady = false;
	policy.resolvedEnabled = false;
	policy.resolvedReason = kPresentBridgeUnavailable;
	return policy;
}

void NRIFrameGenerationContext::RefreshPolicy(const NRIRenderDevice& frameBuffer, bool logChanges)
{
	// Probe the FFX runtime once the user actually asks for frame generation, so
	// the reported policy reflects whether the library is really loadable rather
	// than assuming it is.
	if ((bool)nri_framegen && ResolveRequestedProvider() != NRIFrameGenerationProvider::Off && mFfxModule == nullptr)
	{
		EnsureProviderRuntime(frameBuffer);
	}

	mPresentContract = BuildPresentContract(frameBuffer);
	const NRIFrameGenerationPolicy previous = mPolicy;
	mPolicy = BuildPolicy(frameBuffer, mPresentContract);

	if (!logChanges && mHasLoggedPolicy && previous.resolvedEnabled == mPolicy.resolvedEnabled &&
		previous.requestedEnabled == mPolicy.requestedEnabled)
	{
		return;
	}

	if (mPolicy.requestedEnabled && !mPolicy.resolvedEnabled)
	{
		Printf(TEXTCOLOR_YELLOW "NRI frame generation requested but unavailable: %s\n",
			mPolicy.resolvedReason != nullptr ? mPolicy.resolvedReason : "unknown");
	}
	mHasLoggedPolicy = true;
}

NRIFrameGenerationInputAudit NRIFrameGenerationContext::BuildInputAudit(const NRIFrameGenerationFrameDesc& desc) const
{
	NRIFrameGenerationInputAudit audit = {};
	audit.hudlessColorAvailable = desc.hudlessColor != nullptr;
	audit.motionVectorsAvailable = desc.motionVectors != nullptr;
	audit.renderRectValid = desc.renderWidth != 0 && desc.renderHeight != 0;
	audit.outputRectValid = desc.outputWidth != 0 && desc.outputHeight != 0;
	audit.complete = audit.hudlessColorAvailable && audit.motionVectorsAvailable &&
		audit.renderRectValid && audit.outputRectValid && desc.depth != nullptr;
	return audit;
}

// ---------------------------------------------------------------------------
// Frame hooks
// ---------------------------------------------------------------------------

void NRIFrameGenerationContext::OnSwapChainCreated(const NRIRenderDevice& frameBuffer) { RefreshPolicy(frameBuffer, false); }
void NRIFrameGenerationContext::OnSwapChainDestroyed(const NRIRenderDevice&) { DestroyProviderPresentBridge(); }
void NRIFrameGenerationContext::BeginFrame(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::EndFrame(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::OnSimulationEnd(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::OnRenderSubmitStart(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::OnRenderSubmitEnd(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::OnPresentStart(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::OnPresentEnd(const NRIRenderDevice&, nri::Result) {}

void NRIFrameGenerationContext::SetFrameDesc(const NRIRenderDevice&, const NRIFrameGenerationFrameDesc& desc)
{
	mLastFrameDesc = desc;
	mHasFrameDesc = true;
	mLastInputAudit = BuildInputAudit(desc);
}

void NRIFrameGenerationContext::SetUiTexture(const NRITextureResource*) {}

void NRIFrameGenerationContext::ConfigureAndDispatchFrame(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::ConfigureAndPrepareProvider(const NRIRenderDevice&, const NRIFrameGenerationFrameDesc&) {}

bool NRIFrameGenerationContext::Present(const NRIRenderDevice&, bool, bool, nri::Result&)
{
	// Declining hands presentation back to the native NRI path.
	return false;
}

void NRIFrameGenerationContext::NoteReset(const char*) {}
void NRIFrameGenerationContext::RequestNativeFallback(const char*) {}
bool NRIFrameGenerationContext::ConsumeNativeFallbackRequest() { return false; }
bool NRIFrameGenerationContext::IsPresentBridgeActive() const { return false; }
bool NRIFrameGenerationContext::ShouldUsePresentBridge() const { return false; }

bool NRIFrameGenerationContext::IsLowLatencyOperational(const NRIRenderDevice&) const { return false; }
void NRIFrameGenerationContext::ConfigureLowLatencyMode(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::SetLowLatencyMarker(const NRIRenderDevice&, nri::LatencyMarker, nri::Result&) {}

// ---------------------------------------------------------------------------
// Name helpers (shared with the Windows build's semantics)
// ---------------------------------------------------------------------------

const char* NRIFrameGenerationContext::GetProviderName(NRIFrameGenerationProvider provider)
{
	switch (provider)
	{
	case NRIFrameGenerationProvider::FSR3: return "fsr3";
	default: return "off";
	}
}

const char* NRIFrameGenerationContext::GetUiModeName(NRIFrameGenerationUiMode mode)
{
	switch (mode)
	{
	case NRIFrameGenerationUiMode::Hudless: return "hudless";
	case NRIFrameGenerationUiMode::UiTexture: return "ui-texture";
	default: return "auto";
	}
}

const char* NRIFrameGenerationContext::GetColorSourceName(NRIFrameGenerationColorSource) { return "unknown"; }
const char* NRIFrameGenerationContext::GetMotionVectorSpaceName(NRIFrameGenerationMotionVectorSpace) { return "unknown"; }
const char* NRIFrameGenerationContext::GetMotionVectorDirectionName(NRIFrameGenerationMotionVectorDirection) { return "unknown"; }
const char* NRIFrameGenerationContext::GetDepthTypeName(NRIFrameGenerationDepthType) { return "unknown"; }
const char* NRIFrameGenerationContext::GetAdapterRequirementName(NRIFrameGenerationAdapterRequirement) { return "unknown"; }
const char* NRIFrameGenerationContext::GetOutputContractName(NRIFrameGenerationOutputContract) { return "none"; }
const char* NRIFrameGenerationContext::GetPresentTransferFunctionName(NRIFrameGenerationPresentTransferFunction) { return "unknown"; }
const char* NRIFrameGenerationContext::GetSwapChainFormatName(nri::SwapChainFormat) { return "unknown"; }
const char* NRIFrameGenerationContext::GetNriFormatName(nri::Format) { return "unknown"; }
const char* NRIFrameGenerationContext::GetDxgiFormatName(uint32_t) { return "n/a"; }
const char* NRIFrameGenerationContext::GetWindowModeName(bool fullscreen) { return fullscreen ? "fullscreen" : "windowed"; }
const char* NRIFrameGenerationContext::GetAvailabilityName(bool available) { return available ? "yes" : "no"; }
const char* NRIFrameGenerationContext::GetProviderReturnCodeName(uint32_t) { return "unknown"; }
const char* NRIFrameGenerationContext::GetPresentResultName(nri::Result) { return "unknown"; }
