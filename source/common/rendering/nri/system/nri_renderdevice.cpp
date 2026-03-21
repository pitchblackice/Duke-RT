#include "nri_renderdevice.h"

#include "../renderer/nri_renderer.h"
#include "../renderer/nri_renderstate.h"
#include "nri_hwbuffer.h"
#include "nri_hwtexture.h"
#include "c_cvars.h"
#include "cmdlib.h"
#include "i_mainwindow.h"
#include "printf.h"
#include "textures.h"
#include "v_2ddrawer.h"
#include "v_draw.h"
#include "version.h"
#include "hw_drawinfo.h"
#include "hwrenderer/data/hw_clock.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "flatvertices.h"
#include "hw_skydome.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"

#include <windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include <algorithm>
#include <fstream>

EXTERN_CVAR(String, nri_api)
EXTERN_CVAR(Int, nri_ptportaldepth)
EXTERN_CVAR(Bool, vid_vsync)
CVAR(Bool, nri_ptsanity, false, 0)
CVAR(Bool, nri_ptwaitpresent, true, 0)

namespace
{
	static constexpr int DefaultSwapChainTextureCount = 3;
	static NRIRenderDevice* GetActiveNRIRenderDevice();
}

CUSTOM_CVAR(Int, nri_ptswaptextures, 0, 0)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self == 1)
	{
		self = 2;
	}
	else if (self > 8)
	{
		self = 8;
	}

	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->SetVSync(vid_vsync);
	}
}

CUSTOM_CVAR(Int, nri_ptswapflags, -1, 0)
{
	if (self < -1)
	{
		self = -1;
	}
	else if (self > 3)
	{
		self = 3;
	}

	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->SetVSync(vid_vsync);
	}
}

namespace
{
	static nri::Result(NRI_CALL* gNriGetInterfaceForwarder)(const nri::Device&, const char*, size_t, void*) = nullptr;
	static void (NRI_CALL* gNriDestroyDeviceForwarder)(nri::Device*) = nullptr;

	template<typename T>
	static T NRIFlags(T a, T b)
	{
		return (T)((uint32_t)a | (uint32_t)b);
	}

	static nri::StageBits NRIShaderStages()
	{
		return (nri::StageBits)((uint32_t)nri::StageBits::VERTEX_SHADER | (uint32_t)nri::StageBits::FRAGMENT_SHADER);
	}

	static uint32_t AlignUp(uint32_t value, uint32_t alignment)
	{
		if (alignment <= 1)
		{
			return value;
		}

		const uint32_t remainder = value % alignment;
		return remainder == 0 ? value : value + alignment - remainder;
	}

	static NRIRenderDevice* GetActiveNRIRenderDevice()
	{
		if (screen == nullptr || screen->Backend() != 4)
		{
			Printf("The NRI backend is not active.\n");
			return nullptr;
		}

		return static_cast<NRIRenderDevice*>(screen);
	}

	class ScopedNriTiming
	{
	public:
		ScopedNriTiming(glcycle_t& aggregate, double& outputMs)
			: mAggregate(aggregate), mOutputMs(outputMs)
		{
			mOutputMs = 0.0;
			mAggregate.Clock();
			mTimer.ResetAndClock();
		}

		~ScopedNriTiming()
		{
			mTimer.Unclock();
			mOutputMs = mTimer.TimeMS();
			mAggregate.Unclock();
		}

		ScopedNriTiming(const ScopedNriTiming&) = delete;
		ScopedNriTiming& operator=(const ScopedNriTiming&) = delete;

	private:
		glcycle_t& mAggregate;
		double& mOutputMs;
		cycle_t mTimer;
	};

	static const char* GetNriResultName(nri::Result result)
	{
		switch (result)
		{
		case nri::Result::INVALID_SDK:
			return "invalid_sdk";
		case nri::Result::SUCCESS:
			return "success";
		case nri::Result::FAILURE:
			return "failure";
		case nri::Result::INVALID_ARGUMENT:
			return "invalid_argument";
		case nri::Result::OUT_OF_MEMORY:
			return "out_of_memory";
		case nri::Result::UNSUPPORTED:
			return "unsupported";
		case nri::Result::DEVICE_LOST:
			return "device_lost";
		case nri::Result::OUT_OF_DATE:
			return "out_of_date";
		default:
			return "other";
		}
	}

	static const char* GetNriMessageTypeName(nri::Message messageType)
	{
		switch (messageType)
		{
		case nri::Message::INFO:
			return "info";
		case nri::Message::WARNING:
			return "warning";
		case nri::Message::ERROR:
			return "error";
		default:
			return "other";
		}
	}

	static const char* GetNriVendorName(nri::Vendor vendor)
	{
		switch (vendor)
		{
		case nri::Vendor::NVIDIA:
			return "NVIDIA";
		case nri::Vendor::AMD:
			return "AMD";
		case nri::Vendor::INTEL:
			return "Intel";
		default:
			return "Unknown";
		}
	}

	static void NRI_CALL NriMessageCallback(nri::Message messageType, const char* file, uint32_t line, const char* message, void*)
	{
		if (file != nullptr && *file != '\0')
		{
			Printf("NRI %s: %s (%s:%u)\n", GetNriMessageTypeName(messageType), message, file, line);
		}
		else
		{
			Printf("NRI %s: %s\n", GetNriMessageTypeName(messageType), message);
		}
	}

	static uint32_t CountSetBits(uint64_t mask)
	{
		uint32_t count = 0;
		while (mask != 0)
		{
			count += (uint32_t)(mask & 1ull);
			mask >>= 1;
		}
		return count;
	}

	static FString DescribeSwapChainFlags(nri::SwapChainBits flags)
	{
		if (flags == nri::SwapChainBits::NONE)
		{
			return "NONE";
		}

		FString description;
		const auto appendFlag = [&description](const char* text)
		{
			if (!description.IsEmpty())
			{
				description << "|";
			}
			description << text;
		};

		if (((uint8_t)flags & (uint8_t)nri::SwapChainBits::VSYNC) != 0)
		{
			appendFlag("VSYNC");
		}

		if (((uint8_t)flags & (uint8_t)nri::SwapChainBits::ALLOW_TEARING) != 0)
		{
			appendFlag("ALLOW_TEARING");
		}

		if (((uint8_t)flags & (uint8_t)nri::SwapChainBits::WAITABLE) != 0)
		{
			appendFlag("WAITABLE");
		}

		if (((uint8_t)flags & (uint8_t)nri::SwapChainBits::ALLOW_LOW_LATENCY) != 0)
		{
			appendFlag("ALLOW_LOW_LATENCY");
		}

		return description;
	}

	static FString DescribeSwapChainImageMask(uint64_t mask, uint32_t textureCount)
	{
		if (textureCount == 0)
		{
			return "none";
		}

		FString description;
		for (uint32_t i = 0; i < textureCount; ++i)
		{
			if ((mask & (1ull << i)) == 0)
			{
				continue;
			}

			if (!description.IsEmpty())
			{
				description << ",";
			}
			description.AppendFormat("%u", i);
		}

		if (description.IsEmpty())
		{
			description = "none";
		}

		return description;
	}

	static FString DescribeSwapChainImageCounts(const std::vector<uint64_t>& counts)
	{
		if (counts.empty())
		{
			return "none";
		}

		FString description;
		for (size_t i = 0; i < counts.size(); ++i)
		{
			if (i != 0)
			{
				description << " ";
			}
			description.AppendFormat("%u:%llu", (uint32_t)i, (unsigned long long)counts[i]);
		}

		return description;
	}

	static uint8_t GetRequestedSwapChainTextureCount()
	{
		if (nri_ptswaptextures > 0)
		{
			return (uint8_t)nri_ptswaptextures;
		}

		return DefaultSwapChainTextureCount;
	}

	static nri::SwapChainBits GetRequestedSwapChainFlags()
	{
		switch ((int)nri_ptswapflags)
		{
		case 0:
			return nri::SwapChainBits::NONE;
		case 1:
			return nri::SwapChainBits::ALLOW_TEARING;
		case 2:
			return nri::SwapChainBits::VSYNC;
		case 3:
			return NRIFlags(nri::SwapChainBits::VSYNC, nri::SwapChainBits::ALLOW_TEARING);
		default:
			return vid_vsync ? nri::SwapChainBits::VSYNC : nri::SwapChainBits::ALLOW_TEARING;
		}
	}

	static const char* DescribeSwapChainFlagOverride()
	{
		switch ((int)nri_ptswapflags)
		{
		case -1: return "default";
		case 0: return "NONE";
		case 1: return "ALLOW_TEARING";
		case 2: return "VSYNC";
		case 3: return "VSYNC|ALLOW_TEARING";
		default: return "invalid";
		}
	}
}

extern "C" nri::Result NRI_CALL nriGetInterface(const nri::Device& device, const char* interfaceName, size_t interfaceSize, void* interfacePtr)
{
	return gNriGetInterfaceForwarder != nullptr ? gNriGetInterfaceForwarder(device, interfaceName, interfaceSize, interfacePtr) : nri::Result::FAILURE;
}

extern "C" void NRI_CALL nriDestroyDevice(nri::Device* device)
{
	if (gNriDestroyDeviceForwarder != nullptr)
	{
		gNriDestroyDeviceForwarder(device);
	}
}

CCMD(nri_ptcaps)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingCaps();
	}
}

CCMD(nri_ptstatus)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingStatus();
	}
}

CCMD(nri_ptbuffers)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingBuffers();
	}
}

CCMD(nri_ptreset)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->ResetPathTracingHistory();
	}
}

NRIRenderDevice::NRIRenderDevice(void* hMonitor, bool fullscreen)
	: SystemBaseFrameBuffer(hMonitor, fullscreen), mRenderState(std::make_unique<NRIRenderState>(this))
{
	vendorstring = "NRI";
	glslversion = 6.6f;
	mRenderer = std::make_unique<NRIRenderer>(this);
}

NRIRenderDevice::~NRIRenderDevice()
{
	WaitForCommands(true);

	delete mVertexData;
	mVertexData = nullptr;
	delete mSkyData;
	mSkyData = nullptr;
	delete mViewpoints;
	mViewpoints = nullptr;
	delete mLights;
	mLights = nullptr;
	delete mBones;
	mBones = nullptr;

	DestroyRenderResources();
	DestroySwapChain();

	if (mRenderer != nullptr)
	{
		mRenderer->Shutdown();
	}

	DestroyQueuedFrames();

	if (mFrameFence != nullptr)
	{
		mCore.DestroyFence(mFrameFence);
		mFrameFence = nullptr;
	}

	if (mStreamerInstance != nullptr)
	{
		mStreamer.DestroyStreamer(mStreamerInstance);
		mStreamerInstance = nullptr;
	}

	if (mDevice != nullptr && mDestroyDeviceFn != nullptr)
	{
		mDestroyDeviceFn(mDevice);
		mDevice = nullptr;
	}

	if (mNriModule != nullptr)
	{
		FreeLibrary((HMODULE)mNriModule);
		mNriModule = nullptr;
	}

	gNriDestroyDeviceForwarder = nullptr;
	gNriGetInterfaceForwarder = nullptr;
}

void NRIRenderDevice::Update()
{
	if (mInitialized && mFrameBegun)
	{
		SetActiveRenderTarget();
		Draw2D();
		twod->Clear();
		mRenderState->EndFrame();
		EndFrameAndPresent();
	}

	Super::Update();
}

void NRIRenderDevice::InitializeState()
{
	SetViewportRects(nullptr);

	if (!LoadNRI() || !CreateDevice() || !CreateRenderResources() || !CreateSwapChain())
	{
		Printf(TEXTCOLOR_RED "NRI backend initialization failed.\n");
		mInitialized = false;
		return;
	}

	mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight(), mPipelineNbr);
	mSkyData = new FSkyVertexBuffer;
	mViewpoints = new HWViewpointBuffer(mPipelineNbr);
	mLights = new FLightBuffer(mPipelineNbr);
	mBones = new BoneBuffer(mPipelineNbr);

	LogStartup();
	if (mRenderer != nullptr && !mRenderer->Initialize())
	{
		Printf(TEXTCOLOR_RED "NRI path tracing renderer initialization failed.\n");
	}
	mInitialized = true;
}

bool NRIRenderDevice::CompileNextShader()
{
	return true;
}

int NRIRenderDevice::GetShaderCount()
{
	return 0;
}

const char* NRIRenderDevice::DeviceName() const
{
	return mDeviceName.GetChars();
}

void NRIRenderDevice::BeginFrame()
{
	if (!mInitialized)
	{
		return;
	}

	if (mFrameBegun)
	{
		return;
	}

	Reset2DTextureFrameStats();
	mLastFrameBoundaryStats.frameNumber++;
	mLastFrameBoundaryStats.frameIndex = mFrameIndex;
	mLastFrameBoundaryStats.waitMs = 0.0;
	mLastFrameBoundaryStats.waitForPresentMs = 0.0;
	mLastFrameBoundaryStats.acquireMs = 0.0;
	mLastFrameBoundaryStats.submitMs = 0.0;
	mLastFrameBoundaryStats.presentMs = 0.0;
	mLastFrameBoundaryStats.submittedFenceValue = 0;
	mLastFrameBoundaryStats.waitForPresentResult = nri::Result::SUCCESS;
	mLastFrameBoundaryStats.acquireResult = nri::Result::FAILURE;
	mLastFrameBoundaryStats.presentResult = nri::Result::FAILURE;
	mCurrentQueuedFrameIndex = GetQueuedFrameIndex(mFrameIndex);
	mLastFrameBoundaryStats.queuedFrameIndex = mCurrentQueuedFrameIndex;
	mLastFrameBoundaryStats.swapChainImageIndex = 0;
	mLastFrameBoundaryStats.acquireSemaphoreIndex = 0;
	mLastFrameBoundaryStats.sanityModeEnabled = !!nri_ptsanity;
	mLastFrameBoundaryStats.sanityFrameUsed = false;
	SelectQueuedFrame(mCurrentQueuedFrameIndex);

	{
		ScopedNriTiming waitTiming(NriPTFrameWait, mLastFrameBoundaryStats.waitMs);
		WaitForCommands(false);
	}
	SetViewportRects(nullptr);

	if (!EnsureSwapChainSize())
	{
		return;
	}

	if (nri_ptwaitpresent && mHasPresentedSwapChainFrame && mSwapChain != nullptr)
	{
		nri::Result waitForPresentResult = nri::Result::FAILURE;
		{
			ScopedNriTiming waitPresentTiming(NriPTWaitPresent, mLastFrameBoundaryStats.waitForPresentMs);
			waitForPresentResult = mSwapChainInterface.WaitForPresent(*mSwapChain);
		}
		mLastFrameBoundaryStats.waitForPresentResult = waitForPresentResult;
	}

	mAcquireSemaphoreIndex = mSwapChainImages.empty() ? 0 : (uint32_t)(mFrameIndex % mSwapChainImages.size());
	mLastFrameBoundaryStats.acquireSemaphoreIndex = mAcquireSemaphoreIndex;

	nri::Result acquireResult = nri::Result::FAILURE;
	{
		ScopedNriTiming acquireTiming(NriPTAcquireSwap, mLastFrameBoundaryStats.acquireMs);
		acquireResult = mSwapChainInterface.AcquireNextTexture(*mSwapChain, *mSwapChainImages[mAcquireSemaphoreIndex].acquireSemaphore, mCurrentSwapChainImage);
	}
	mLastFrameBoundaryStats.acquireResult = acquireResult;
	mLastFrameBoundaryStats.swapChainImageIndex = mCurrentSwapChainImage;
	if (acquireResult == nri::Result::SUCCESS)
	{
		NoteSwapChainAcquire(mCurrentSwapChainImage);
	}
	if (acquireResult == nri::Result::OUT_OF_DATE)
	{
		if (!EnsureSwapChainSize())
		{
			return;
		}

		{
			ScopedNriTiming reacquireTiming(NriPTAcquireSwap, mLastFrameBoundaryStats.acquireMs);
			acquireResult = mSwapChainInterface.AcquireNextTexture(*mSwapChain, *mSwapChainImages[mAcquireSemaphoreIndex].acquireSemaphore, mCurrentSwapChainImage);
		}
		mLastFrameBoundaryStats.acquireResult = acquireResult;
		mLastFrameBoundaryStats.swapChainImageIndex = mCurrentSwapChainImage;
		if (acquireResult == nri::Result::SUCCESS)
		{
			NoteSwapChainAcquire(mCurrentSwapChainImage);
		}
	}

	if (acquireResult != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "NRI failed to acquire swapchain image.\n");
		return;
	}

	mHasAcquiredSwapChainImage = true;
	mCurrentPresentTarget = &mSwapChainImages[mCurrentSwapChainImage].target;
	mActiveTarget = mUsingSaveTarget ? &mSaveTarget : mCurrentPresentTarget;
	if (!BeginCommandList("BeginFrame", false))
	{
		ResetFrameTracking(false);
		return;
	}
	mRenderState->BeginFrame();

	if (mViewpoints != nullptr)
	{
		mViewpoints->Clear();
	}

	mFrameBegun = true;
}

FRenderState* NRIRenderDevice::RenderState()
{
	return mRenderState.get();
}

void NRIRenderDevice::Draw2D()
{
	if (!mInitialized || twod == nullptr)
	{
		return;
	}

	::Draw2D(twod, *mRenderState);
}

void NRIRenderDevice::SetVSync(bool vsync)
{
	Super::SetVSync(vsync);

	if (!mInitialized || mDevice == nullptr || mGraphicsQueue == nullptr)
	{
		return;
	}

	WaitForCommands(true);
	if (!CreateSwapChain())
	{
		Printf(TEXTCOLOR_RED "NRI failed to recreate swapchain after vsync change.\n");
	}
}

void NRIRenderDevice::WaitForCommands(bool finish)
{
	if (mDevice == nullptr)
	{
		return;
	}

	if (finish)
	{
		mCore.DeviceWaitIdle(mDevice);
		return;
	}

	if (mFrameFence == nullptr || mQueuedFrames.empty())
	{
		return;
	}

	if (mFrameIndex < mQueuedFrames.size())
	{
		return;
	}

	const uint64_t recycleFenceValue = 1 + mFrameIndex - mQueuedFrames.size();
	if (recycleFenceValue != 0)
	{
		mCore.Wait(*mFrameFence, recycleFenceValue);
	}
}

void NRIRenderDevice::SetSaveBuffers(bool yes)
{
	mUsingSaveTarget = yes;
	if (!mInitialized)
	{
		return;
	}

	if (yes && (mSaveTarget.texture == nullptr || mSaveTarget.width != SAVEPICWIDTH || mSaveTarget.height != SAVEPICHEIGHT))
	{
		DestroyTextureResource(mSaveTarget);
		CreateOwnedTexture(mSaveTarget, SAVEPICWIDTH, SAVEPICHEIGHT, nri::Format::BGRA8_UNORM, NRIFlags(nri::TextureUsageBits::SHADER_RESOURCE, nri::TextureUsageBits::COLOR_ATTACHMENT));
	}

	mRenderState->EndFrame();
	mActiveTarget = yes ? &mSaveTarget : mCurrentPresentTarget;
}

void NRIRenderDevice::ImageTransitionScene(bool)
{
}

void NRIRenderDevice::SetActiveRenderTarget()
{
	mRenderState->EndFrame();
	mActiveTarget = mUsingSaveTarget ? &mSaveTarget : mCurrentPresentTarget;
}

bool NRIRenderDevice::RenderPathTracedScene(HWDrawInfo& di, int drawmode, bool portal)
{
	if (!mInitialized)
	{
		return false;
	}

	if (nri_ptsanity && drawmode == DM_MAINVIEW && !portal)
	{
		mLastFrameBoundaryStats.sanityFrameUsed = true;
		return RenderPathTracingSanityFrame();
	}

	if (mRenderer == nullptr)
	{
		return false;
	}

	return mRenderer->RenderScene(di, drawmode, portal);
}

bool NRIRenderDevice::ShouldSkipSceneBuildForPathTracedScene(int drawmode, bool portal) const
{
	return !!nri_ptsanity && drawmode == DM_MAINVIEW && !portal;
}

bool NRIRenderDevice::RenderPathTracingSanityFrame()
{
	if (mCommandBuffer == nullptr || mActiveTarget == nullptr || mActiveTarget->colorAttachmentView == nullptr)
	{
		return false;
	}

	mRenderState->EndFrame();
	PrepareTargetForRendering(*mActiveTarget, true);

	nri::AttachmentDesc colorAttachment = {};
	colorAttachment.descriptor = mActiveTarget->colorAttachmentView;
	colorAttachment.loadOp = nri::LoadOp::CLEAR;
	colorAttachment.storeOp = nri::StoreOp::STORE;
	colorAttachment.clearValue.color.f.x = mSceneClearColor[0];
	colorAttachment.clearValue.color.f.y = mSceneClearColor[1];
	colorAttachment.clearValue.color.f.z = mSceneClearColor[2];
	colorAttachment.clearValue.color.f.w = mSceneClearColor[3];

	nri::RenderingDesc renderingDesc = {};
	renderingDesc.colors = &colorAttachment;
	renderingDesc.colorNum = 1;
	mCore.CmdBeginRendering(*mCommandBuffer, renderingDesc);
	mCore.CmdEndRendering(*mCommandBuffer);
	mRenderState->NotifyExternalTargetWrite();
	return true;
}

IHardwareTexture* NRIRenderDevice::CreateHardwareTexture(int numchannels)
{
	return new NRIHardwareTexture(this, numchannels);
}

IVertexBuffer* NRIRenderDevice::CreateVertexBuffer()
{
	return new NRIHardwareVertexBuffer();
}

IIndexBuffer* NRIRenderDevice::CreateIndexBuffer()
{
	return new NRIHardwareIndexBuffer();
}

IDataBuffer* NRIRenderDevice::CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
{
	return new NRIHardwareDataBuffer(bindingpoint, ssbo, needsresize);
}

FTexture* NRIRenderDevice::WipeStartScreen()
{
	SetViewportRects(nullptr);

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<NRIHardwareTexture*>(tex->GetSystemTexture());
	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeStartScreen");
	return tex;
}

FTexture* NRIRenderDevice::WipeEndScreen()
{
	Draw2D();
	twod->Clear();

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<NRIHardwareTexture*>(tex->GetSystemTexture());
	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeEndScreen");
	return tex;
}

TArray<uint8_t> NRIRenderDevice::GetScreenshotBuffer(int& pitch, ESSType& color_type, float& gamma)
{
	const int w = SCREENWIDTH;
	const int h = SCREENHEIGHT;

	TArray<uint8_t> buffer(w * h * 3, true);
	CopyScreenToBuffer(w, h, buffer.Data());

	pitch = w * 3;
	color_type = SS_RGB;
	gamma = 1.0f;
	return buffer;
}

void NRIRenderDevice::PrintPathTracingCaps() const
{
	if (mDevice == nullptr)
	{
		Printf("NRI PT capabilities are unavailable because the device is not initialized.\n");
		return;
	}

	const nri::DeviceDesc& deviceDesc = mCore.GetDeviceDesc(*mDevice);
	Printf("NRI PT caps: api=%s shader_model=%u.%u ray_tracing_tier=%u texture2D_max=%u root_constants=%u root_descriptors=%u descriptor_sets=%u\n",
		(const char*)nri_api,
		deviceDesc.shaderModel / 10,
		deviceDesc.shaderModel % 10,
		deviceDesc.tiers.rayTracing,
		deviceDesc.dimensions.texture2DMaxDim,
		deviceDesc.pipelineLayout.rootConstantMaxSize,
		deviceDesc.pipelineLayout.rootDescriptorMaxNum,
		deviceDesc.pipelineLayout.descriptorSetMaxNum);
	Printf("NRI PT upscalers: NIS=%s DLSS-SR=%s DLRR=%s portal_depth=%d\n",
		mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::NIS) ? "yes" : "no",
		mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::DLSR) ? "yes" : "no",
		mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::DLRR) ? "yes" : "no",
		(int)nri_ptportaldepth);

	if (mRenderer != nullptr)
	{
		Printf("NRI PT availability: %s", mRenderer->IsPathTracingSupported() ? "available" : "raster-fallback");
		if (!mRenderer->IsPathTracingSupported())
		{
			Printf(" (%s)", mRenderer->GetAvailabilityReason());
		}
		Printf("\n");
	}
}

void NRIRenderDevice::PrintPathTracingStatus() const
{
	PrintPathTracingCaps();
	PrintFrameBoundaryStatus();
	PrintFrameSequenceStatus();
	PrintSwapChainStatus();
	Print2DTextureStatus();
	if (mRenderer != nullptr)
	{
		mRenderer->PrintStatus();
	}
}

void NRIRenderDevice::PrintPathTracingBuffers() const
{
	PrintPathTracingCaps();
	PrintFrameBoundaryStatus();
	PrintFrameSequenceStatus();
	PrintSwapChainStatus();
	Print2DTextureStatus();
	if (mRenderer != nullptr)
	{
		mRenderer->PrintSceneBufferStatus();
	}
}

void NRIRenderDevice::PrintFrameBoundaryStatus() const
{
	const auto& stats = mLastFrameBoundaryStats;
	Printf("NRI PT frame boundary: frame=%llu frame_index=%llu qframe=%u sanity_mode=%s last_frame=%s wait=%2.3f wait_present=%2.3f acquire=%2.3f submit=%2.3f present=%2.3f wait_present_result=%s acquire_result=%s present_result=%s image=%u sem_index=%u submit_fence=%llu\n",
		(unsigned long long)stats.frameNumber,
		(unsigned long long)stats.frameIndex,
		stats.queuedFrameIndex,
		stats.sanityModeEnabled ? "on" : "off",
		stats.sanityFrameUsed ? "clear-only" : "normal",
		stats.waitMs,
		stats.waitForPresentMs,
		stats.acquireMs,
		stats.submitMs,
		stats.presentMs,
		GetNriResultName(stats.waitForPresentResult),
		GetNriResultName(stats.acquireResult),
		GetNriResultName(stats.presentResult),
		stats.swapChainImageIndex,
		stats.acquireSemaphoreIndex,
		(unsigned long long)stats.submittedFenceValue);
}

void NRIRenderDevice::PrintFrameSequenceStatus() const
{
	bool anyValid = false;
	FString line = "NRI PT frame sequence:";
	for (uint32_t i = 0; i < FrameSequenceHistorySize; ++i)
	{
		const uint32_t index = (mFrameSequenceWriteIndex + i) % FrameSequenceHistorySize;
		const FrameSequenceEntry& entry = mFrameSequenceHistory[index];
		if (!entry.valid)
		{
			continue;
		}

		anyValid = true;
		line.AppendFormat(" [f%llu i%llu q%u a%u@s%u -> p%u@s%u fence=%llu pres=%s %s]",
			(unsigned long long)entry.frameNumber,
			(unsigned long long)entry.frameIndex,
			entry.queuedFrameIndex,
			entry.acquiredImageIndex,
			entry.acquireSemaphoreIndex,
			entry.presentedImageIndex,
			entry.releaseSemaphoreIndex,
			(unsigned long long)entry.submittedFenceValue,
			GetNriResultName(entry.presentResult),
			entry.sanityFrameUsed ? "sanity" : "normal");
	}

	if (!anyValid)
	{
		line << " none";
	}

	Printf("%s\n", line.GetChars());
}

void NRIRenderDevice::PrintSwapChainStatus() const
{
	const FString flagText = DescribeSwapChainFlags(mSwapChainFlags);
	const FString acquiredImages = DescribeSwapChainImageMask(mObservedSwapChainAcquireMask, mSwapChainTextureCount);
	const FString presentedImages = DescribeSwapChainImageMask(mObservedSwapChainPresentMask, mSwapChainTextureCount);
	const FString acquireCounts = DescribeSwapChainImageCounts(mSwapChainAcquireCounts);
	const FString presentCounts = DescribeSwapChainImageCounts(mSwapChainPresentCounts);
	const FString abandonCounts = DescribeSwapChainImageCounts(mSwapChainAbandonCounts);
	Printf("NRI PT swapchain: textures=%u queued_frames=%u vsync=%s flags=%s texture_override=%d flag_override=%s wait_present=%s acquire_seen=%u/%u [%s] present_seen=%u/%u [%s]\n",
		(uint32_t)mSwapChainTextureCount,
		(uint32_t)mSwapChainQueuedFrameNum,
		vid_vsync ? "on" : "off",
		flagText.GetChars(),
		(int)nri_ptswaptextures,
		DescribeSwapChainFlagOverride(),
		nri_ptwaitpresent ? "on" : "off",
		CountSetBits(mObservedSwapChainAcquireMask),
		(uint32_t)mSwapChainTextureCount,
		acquiredImages.GetChars(),
		CountSetBits(mObservedSwapChainPresentMask),
		(uint32_t)mSwapChainTextureCount,
		presentedImages.GetChars());
	Printf("NRI PT swapchain counts: acquire=[%s] present=[%s] abandoned=[%s]\n",
		acquireCounts.GetChars(),
		presentCounts.GetChars(),
		abandonCounts.GetChars());
}

void NRIRenderDevice::RecordFrameSequence(uint32_t releaseSemaphoreIndex, uint64_t submittedFenceValue, nri::Result presentResult)
{
	FrameSequenceEntry& entry = mFrameSequenceHistory[mFrameSequenceWriteIndex];
	entry = {};
	entry.frameNumber = mLastFrameBoundaryStats.frameNumber;
	entry.frameIndex = mLastFrameBoundaryStats.frameIndex;
	entry.submittedFenceValue = submittedFenceValue;
	entry.queuedFrameIndex = mLastFrameBoundaryStats.queuedFrameIndex;
	entry.acquiredImageIndex = mLastFrameBoundaryStats.swapChainImageIndex;
	entry.acquireSemaphoreIndex = mLastFrameBoundaryStats.acquireSemaphoreIndex;
	entry.presentedImageIndex = mCurrentSwapChainImage;
	entry.releaseSemaphoreIndex = releaseSemaphoreIndex;
	entry.presentResult = presentResult;
	entry.sanityFrameUsed = mLastFrameBoundaryStats.sanityFrameUsed;
	entry.valid = true;
	mFrameSequenceWriteIndex = (mFrameSequenceWriteIndex + 1) % FrameSequenceHistorySize;
}

void NRIRenderDevice::Print2DTextureStatus() const
{
	const auto& stats = mTexture2DDebugStats;
	Printf("NRI 2D textures: frame=%llu ensure=%u canvas=%u hits=%u misses=%u uploads=%u failures=%u create=%u recreate=%u bytes=%llu total_bytes=%llu\n",
		(unsigned long long)stats.frameNumber,
		stats.ensureCalls,
		stats.canvasEnsures,
		stats.cacheHits,
		stats.cacheMisses,
		stats.uploadAttempts,
		stats.uploadFailures,
		stats.resourceCreates,
		stats.resourceRecreates,
		(unsigned long long)stats.uploadedBytes,
		(unsigned long long)stats.totalUploadedBytes);
	Printf("NRI 2D totals: ensures=%llu canvas=%llu hits=%llu misses=%llu uploads=%llu failures=%llu create=%llu recreate=%llu\n",
		(unsigned long long)stats.totalEnsureCalls,
		(unsigned long long)stats.totalCanvasEnsures,
		(unsigned long long)stats.totalCacheHits,
		(unsigned long long)stats.totalCacheMisses,
		(unsigned long long)stats.totalUploadAttempts,
		(unsigned long long)stats.totalUploadFailures,
		(unsigned long long)stats.totalResourceCreates,
		(unsigned long long)stats.totalResourceRecreates);
}

void NRIRenderDevice::Reset2DTextureFrameStats()
{
	mTexture2DDebugStats.frameNumber++;
	mTexture2DDebugStats.ensureCalls = 0;
	mTexture2DDebugStats.canvasEnsures = 0;
	mTexture2DDebugStats.cacheHits = 0;
	mTexture2DDebugStats.cacheMisses = 0;
	mTexture2DDebugStats.uploadAttempts = 0;
	mTexture2DDebugStats.uploadFailures = 0;
	mTexture2DDebugStats.resourceCreates = 0;
	mTexture2DDebugStats.resourceRecreates = 0;
	mTexture2DDebugStats.uploadedBytes = 0;
}

void NRIRenderDevice::Note2DTextureEnsure(bool canvas)
{
	mTexture2DDebugStats.ensureCalls++;
	mTexture2DDebugStats.totalEnsureCalls++;
	if (canvas)
	{
		mTexture2DDebugStats.canvasEnsures++;
		mTexture2DDebugStats.totalCanvasEnsures++;
	}
}

void NRIRenderDevice::Note2DTextureCacheHit()
{
	mTexture2DDebugStats.cacheHits++;
	mTexture2DDebugStats.totalCacheHits++;
}

void NRIRenderDevice::Note2DTextureCacheMiss()
{
	mTexture2DDebugStats.cacheMisses++;
	mTexture2DDebugStats.totalCacheMisses++;
}

void NRIRenderDevice::Note2DTextureUploadAttempt(uint64_t bytes, bool success)
{
	mTexture2DDebugStats.uploadAttempts++;
	mTexture2DDebugStats.totalUploadAttempts++;
	mTexture2DDebugStats.uploadedBytes += bytes;
	mTexture2DDebugStats.totalUploadedBytes += bytes;
	if (!success)
	{
		mTexture2DDebugStats.uploadFailures++;
		mTexture2DDebugStats.totalUploadFailures++;
	}
}

void NRIRenderDevice::Note2DTextureResourceCreate(bool recreated)
{
	if (recreated)
	{
		mTexture2DDebugStats.resourceRecreates++;
		mTexture2DDebugStats.totalResourceRecreates++;
	}
	else
	{
		mTexture2DDebugStats.resourceCreates++;
		mTexture2DDebugStats.totalResourceCreates++;
	}
}

void NRIRenderDevice::NoteSwapChainAcquire(uint32_t imageIndex)
{
	if (imageIndex < 64)
	{
		mObservedSwapChainAcquireMask |= (1ull << imageIndex);
	}

	if (imageIndex < mSwapChainAcquireCounts.size())
	{
		mSwapChainAcquireCounts[imageIndex]++;
	}
}

void NRIRenderDevice::NoteSwapChainPresent(uint32_t imageIndex)
{
	if (imageIndex < 64)
	{
		mObservedSwapChainPresentMask |= (1ull << imageIndex);
	}

	if (imageIndex < mSwapChainPresentCounts.size())
	{
		mSwapChainPresentCounts[imageIndex]++;
	}
}

void NRIRenderDevice::NoteSwapChainAbandon(uint32_t imageIndex)
{
	if (imageIndex < mSwapChainAbandonCounts.size())
	{
		mSwapChainAbandonCounts[imageIndex]++;
	}
}

void NRIRenderDevice::ResetPathTracingHistory()
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT history reset is unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->ResetHistory();
	Printf("NRI PT history reset requested.\n");
}

void NRIRenderDevice::LogStartup()
{
	if (mLoggedStartup || mDevice == nullptr)
	{
		return;
	}

	const nri::DeviceDesc& deviceDesc = mCore.GetDeviceDesc(*mDevice);
	mDeviceName = FStringf("NRI (%s) - %s", (const char*)nri_api, deviceDesc.adapterDesc.name);
	vendorstring = mDeviceName.GetChars();

	Printf("NRI device: " TEXTCOLOR_ORANGE "%s\n", deviceDesc.adapterDesc.name);
	Printf("NRI graphics API: %s\n", (const char*)nri_api);
	Printf("Max. texture size: %u\n", deviceDesc.dimensions.texture2DMaxDim);
	Printf("Root constant limit: %u\n", deviceDesc.pipelineLayout.rootConstantMaxSize);
	Printf("Shader model: %u.%u\n", deviceDesc.shaderModel / 10, deviceDesc.shaderModel % 10);
	Printf("Ray tracing tier: %u\n", deviceDesc.tiers.rayTracing);
	Printf("NRI queued frames: %u\n", (uint32_t)mQueuedFrames.size());
	Printf("NRI swapchain policy: textures=%u queued_frames=%u vsync=%s flags=%s\n",
		(uint32_t)mSwapChainTextureCount,
		(uint32_t)mSwapChainQueuedFrameNum,
		vid_vsync ? "on" : "off",
		DescribeSwapChainFlags(mSwapChainFlags).GetChars());
	Printf("Upscaler support: NIS=%s DLSS-SR=%s DLRR=%s\n",
		mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::NIS) ? "yes" : "no",
		mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::DLSR) ? "yes" : "no",
		mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::DLRR) ? "yes" : "no");

	mLoggedStartup = true;
}

bool NRIRenderDevice::LoadNRI()
{
	if (mNriModule != nullptr)
	{
		return true;
	}

	HMODULE module = LoadLibraryA("NRI.dll");
	if (module == nullptr)
	{
		FString localPath = progdir;
		localPath << "NRI.dll";
		module = LoadLibraryA(localPath.GetChars());
	}

	if (module == nullptr)
	{
		Printf(TEXTCOLOR_RED "Failed to load NRI.dll.\n");
		return false;
	}

	mEnumerateAdapters = (PFN_nriEnumerateAdapters)GetProcAddress(module, "nriEnumerateAdapters");
	mCreateDeviceFn = (PFN_nriCreateDevice)GetProcAddress(module, "nriCreateDevice");
	mDestroyDeviceFn = (PFN_nriDestroyDevice)GetProcAddress(module, "nriDestroyDevice");
	mGetInterfaceFn = (PFN_nriGetInterface)GetProcAddress(module, "nriGetInterface");

	if (mEnumerateAdapters == nullptr || mCreateDeviceFn == nullptr || mDestroyDeviceFn == nullptr || mGetInterfaceFn == nullptr)
	{
		Printf(TEXTCOLOR_RED "NRI.dll is missing required exports.\n");
		FreeLibrary(module);
		return false;
	}

	gNriDestroyDeviceForwarder = mDestroyDeviceFn;
	gNriGetInterfaceForwarder = mGetInterfaceFn;
	mNriModule = module;
	return true;
}

bool NRIRenderDevice::CreateDevice()
{
	nri::AdapterDesc adapters[8] = {};
	uint32_t adapterCount = (uint32_t)std::size(adapters);
	const nri::Result enumerateResult = mEnumerateAdapters(adapters, adapterCount);
	if (enumerateResult != nri::Result::SUCCESS || adapterCount == 0)
	{
		Printf(TEXTCOLOR_RED "Failed to enumerate NRI adapters (result=%s, count=%u).\n", GetNriResultName(enumerateResult), adapterCount);
		return false;
	}

	for (uint32_t i = 0; i < adapterCount; ++i)
	{
		const auto& adapter = adapters[i];
		const double videoMemoryGiB = (double)adapter.videoMemorySize / (1024.0 * 1024.0 * 1024.0);
		const double sharedMemoryGiB = (double)adapter.sharedSystemMemorySize / (1024.0 * 1024.0 * 1024.0);
		Printf("NRI adapter[%u]: %s (vendor=%s, video=%.2f GiB, shared=%.2f GiB, graphicsQueues=%u)\n",
			i,
			adapter.name,
			GetNriVendorName(adapter.vendor),
			videoMemoryGiB,
			sharedMemoryGiB,
			adapter.queueNum[(uint32_t)nri::QueueType::GRAPHICS]);
	}

	nri::DeviceCreationDesc creationDesc = {};
	creationDesc.graphicsAPI = GetSelectedAPI();
	creationDesc.adapterDesc = &adapters[0];
	creationDesc.callbackInterface.MessageCallback = &NriMessageCallback;
	creationDesc.enableGraphicsAPIValidation = false;
	creationDesc.enableNRIValidation = false;
	creationDesc.disableVKRayTracing = false;
	creationDesc.disableD3D12EnhancedBarriers = false;
	creationDesc.vkBindingOffsets = {};

	const nri::Result createResult = mCreateDeviceFn(creationDesc, mDevice);
	if (createResult != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI device for API '%s' using adapter '%s' (result=%s).\n",
			(const char*)nri_api,
			adapters[0].name,
			GetNriResultName(createResult));
		if (createResult == nri::Result::INVALID_SDK)
		{
			Printf(TEXTCOLOR_RED "NRI reported INVALID_SDK. Check that raze.exe exports D3D12SDKVersion/D3D12SDKPath and that an AgilitySDK runtime directory is staged beside the executable.\n");
		}
		return false;
	}

	if (mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::CoreInterface), &mCore) != nri::Result::SUCCESS ||
		mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::HelperInterface), &mHelper) != nri::Result::SUCCESS ||
		mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::RayTracingInterface), &mRayTracing) != nri::Result::SUCCESS ||
		mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::StreamerInterface), &mStreamer) != nri::Result::SUCCESS ||
		mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::SwapChainInterface), &mSwapChainInterface) != nri::Result::SUCCESS ||
		mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::UpscalerInterface), &mUpscaler) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to retrieve NRI interfaces.\n");
		return false;
	}

	if (mCore.GetQueue(*mDevice, nri::QueueType::GRAPHICS, 0, mGraphicsQueue) != nri::Result::SUCCESS ||
		mCore.CreateFence(*mDevice, 0, mFrameFence) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI queue objects.\n");
		return false;
	}

	if (!CreateQueuedFrames())
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI queued frame resources.\n");
		return false;
	}

	nri::StreamerDesc streamerDesc = {};
	streamerDesc.dynamicBufferMemoryLocation = nri::MemoryLocation::DEVICE_UPLOAD;
	streamerDesc.dynamicBufferDesc = {};
	streamerDesc.dynamicBufferDesc.usage = NRIFlags(nri::BufferUsageBits::VERTEX_BUFFER, nri::BufferUsageBits::INDEX_BUFFER);
	streamerDesc.queuedFrameNum = QueuedFrameCount;
	if (mStreamer.CreateStreamer(*mDevice, streamerDesc, mStreamerInstance) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI streamer.\n");
		return false;
	}

	return true;
}

bool NRIRenderDevice::CreateSwapChain()
{
	if (mDevice == nullptr || mGraphicsQueue == nullptr || mainwindow.GetHandle() == nullptr)
	{
		return false;
	}

	DestroySwapChain();

	const uint32_t width = (uint32_t)(std::max)(GetClientWidth(), 1);
	const uint32_t height = (uint32_t)(std::max)(GetClientHeight(), 1);

	nri::SwapChainDesc swapChainDesc = {};
	swapChainDesc.window.windows.hwnd = mainwindow.GetHandle();
	swapChainDesc.queue = mGraphicsQueue;
	swapChainDesc.width = width;
	swapChainDesc.height = height;
	swapChainDesc.textureNum = GetRequestedSwapChainTextureCount();
	swapChainDesc.format = nri::SwapChainFormat::BT709_G22_8BIT;
	swapChainDesc.flags = GetRequestedSwapChainFlags();
	swapChainDesc.queuedFrameNum = QueuedFrameCount;

	if (mSwapChainInterface.CreateSwapChain(*mDevice, swapChainDesc, mSwapChain) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI swapchain.\n");
		return false;
	}

	uint32_t textureCount = 0;
	nri::Texture* const* textures = mSwapChainInterface.GetSwapChainTextures(*mSwapChain, textureCount);
	mSwapChainImages.resize(textureCount);
	mSwapChainFlags = swapChainDesc.flags;
	mSwapChainQueuedFrameNum = swapChainDesc.queuedFrameNum;
	mSwapChainTextureCount = (uint8_t)(std::min<uint32_t>)(textureCount, 255u);
	mObservedSwapChainAcquireMask = 0;
	mObservedSwapChainPresentMask = 0;
	mSwapChainAcquireCounts.assign(textureCount, 0);
	mSwapChainPresentCounts.assign(textureCount, 0);
	mSwapChainAbandonCounts.assign(textureCount, 0);
	mHasPresentedSwapChainFrame = false;

	for (uint32_t i = 0; i < textureCount; ++i)
	{
		auto& image = mSwapChainImages[i];
		image.target.texture = textures[i];
		image.target.owned = false;

		const nri::TextureDesc& desc = mCore.GetTextureDesc(*textures[i]);
		image.target.width = desc.width;
		image.target.height = desc.height;
		image.target.format = desc.format;
		image.target.usage = desc.usage;
		image.target.state = { nri::AccessBits::NONE, nri::Layout::PRESENT, nri::StageBits::NONE };

		if (!CreateTextureViews(image.target))
		{
			return false;
		}

		if (mCore.CreateFence(*mDevice, nri::SWAPCHAIN_SEMAPHORE, image.acquireSemaphore) != nri::Result::SUCCESS ||
			mCore.CreateFence(*mDevice, nri::SWAPCHAIN_SEMAPHORE, image.releaseSemaphore) != nri::Result::SUCCESS)
		{
			return false;
		}
	}

	Printf("NRI swapchain created: textures=%u queued_frames=%u vsync=%s flags=%s texture_override=%d flag_override=%s wait_present=%s size=%ux%u\n",
		(uint32_t)mSwapChainTextureCount,
		(uint32_t)mSwapChainQueuedFrameNum,
		vid_vsync ? "on" : "off",
		DescribeSwapChainFlags(mSwapChainFlags).GetChars(),
		(int)nri_ptswaptextures,
		DescribeSwapChainFlagOverride(),
		nri_ptwaitpresent ? "on" : "off",
		width,
		height);

	return true;
}

bool NRIRenderDevice::CreateQueuedFrames()
{
	DestroyQueuedFrames();

	mQueuedFrames.resize(QueuedFrameCount);
	for (QueuedFrame& queuedFrame : mQueuedFrames)
	{
		if (mCore.CreateCommandAllocator(*mGraphicsQueue, queuedFrame.commandAllocator) != nri::Result::SUCCESS ||
			mCore.CreateCommandBuffer(*queuedFrame.commandAllocator, queuedFrame.commandBuffer) != nri::Result::SUCCESS)
		{
			DestroyQueuedFrames();
			return false;
		}
	}

	SelectQueuedFrame(0);
	return true;
}

void NRIRenderDevice::DestroySwapChain()
{
	ResetFrameTracking();
	mSwapChainFlags = nri::SwapChainBits::NONE;
	mSwapChainQueuedFrameNum = 0;
	mSwapChainTextureCount = 0;
	mObservedSwapChainAcquireMask = 0;
	mObservedSwapChainPresentMask = 0;
	mSwapChainAcquireCounts.clear();
	mSwapChainPresentCounts.clear();
	mSwapChainAbandonCounts.clear();
	mHasPresentedSwapChainFrame = false;

	for (auto& image : mSwapChainImages)
	{
		if (image.target.colorAttachmentView != nullptr)
		{
			mCore.DestroyDescriptor(image.target.colorAttachmentView);
			image.target.colorAttachmentView = nullptr;
		}

		if (image.target.shaderView != nullptr)
		{
			mCore.DestroyDescriptor(image.target.shaderView);
			image.target.shaderView = nullptr;
		}

		if (image.acquireSemaphore != nullptr)
		{
			mCore.DestroyFence(image.acquireSemaphore);
			image.acquireSemaphore = nullptr;
		}

		if (image.releaseSemaphore != nullptr)
		{
			mCore.DestroyFence(image.releaseSemaphore);
			image.releaseSemaphore = nullptr;
		}
	}

	mSwapChainImages.clear();

	if (mSwapChain != nullptr)
	{
		mSwapChainInterface.DestroySwapChain(mSwapChain);
		mSwapChain = nullptr;
	}
}

void NRIRenderDevice::DestroyQueuedFrames()
{
	mCommandAllocator = nullptr;
	mCommandBuffer = nullptr;
	mCurrentQueuedFrameIndex = 0;

	for (QueuedFrame& queuedFrame : mQueuedFrames)
	{
		if (queuedFrame.commandBuffer != nullptr)
		{
			mCore.DestroyCommandBuffer(queuedFrame.commandBuffer);
			queuedFrame.commandBuffer = nullptr;
		}

		if (queuedFrame.commandAllocator != nullptr)
		{
			mCore.DestroyCommandAllocator(queuedFrame.commandAllocator);
			queuedFrame.commandAllocator = nullptr;
		}
	}

	mQueuedFrames.clear();
}

bool NRIRenderDevice::CreateRenderResources()
{
	if (!LoadShaderBlob(GetSelectedAPI() == nri::GraphicsAPI::D3D12 ? "Nri2D.vs.dxil" : "Nri2D.vs.spirv", mVertexShaderBlob) ||
		!LoadShaderBlob(GetSelectedAPI() == nri::GraphicsAPI::D3D12 ? "Nri2D.ps.dxil" : "Nri2D.ps.spirv", mPixelShaderBlob))
	{
		return false;
	}

	nri::DescriptorRangeDesc samplerRange = {};
	samplerRange.baseRegisterIndex = 0;
	samplerRange.descriptorNum = 1;
	samplerRange.descriptorType = nri::DescriptorType::SAMPLER;
	samplerRange.shaderStages = NRIShaderStages();

	nri::DescriptorRangeDesc textureRange = {};
	textureRange.baseRegisterIndex = 0;
	textureRange.descriptorNum = 1;
	textureRange.descriptorType = nri::DescriptorType::TEXTURE;
	textureRange.shaderStages = NRIShaderStages();

	nri::DescriptorSetDesc descriptorSets[2] = {};
	descriptorSets[0].registerSpace = 0;
	descriptorSets[0].ranges = &samplerRange;
	descriptorSets[0].rangeNum = 1;
	descriptorSets[1].registerSpace = 1;
	descriptorSets[1].ranges = &textureRange;
	descriptorSets[1].rangeNum = 1;

	nri::RootConstantDesc rootConstant = {};
	rootConstant.registerIndex = 0;
	rootConstant.size = sizeof(NRIShaderConstants);
	rootConstant.shaderStages = NRIShaderStages();

	nri::PipelineLayoutDesc pipelineLayoutDesc = {};
	pipelineLayoutDesc.rootRegisterSpace = 2;
	pipelineLayoutDesc.rootConstants = &rootConstant;
	pipelineLayoutDesc.rootConstantNum = 1;
	pipelineLayoutDesc.descriptorSets = descriptorSets;
	pipelineLayoutDesc.descriptorSetNum = 2;
	pipelineLayoutDesc.shaderStages = NRIShaderStages();

	if (mCore.CreatePipelineLayout(*mDevice, pipelineLayoutDesc, mPipelineLayout) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI pipeline layout.\n");
		return false;
	}

	nri::DescriptorPoolDesc poolDesc = {};
	poolDesc.descriptorSetMaxNum = 4096;
	poolDesc.samplerMaxNum = 8;
	poolDesc.textureMaxNum = 4096;
	poolDesc.storageTextureMaxNum = 64;
	poolDesc.structuredBufferMaxNum = 64;
	poolDesc.accelerationStructureMaxNum = 8;

	if (mCore.CreateDescriptorPool(*mDevice, poolDesc, mDescriptorPool) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI descriptor pool.\n");
		return false;
	}

	auto createSampler = [this](NRISamplerMode mode, bool clamp, bool linear)
	{
		nri::SamplerDesc samplerDesc = {};
		samplerDesc.filters.min = linear ? nri::Filter::LINEAR : nri::Filter::NEAREST;
		samplerDesc.filters.mag = linear ? nri::Filter::LINEAR : nri::Filter::NEAREST;
		samplerDesc.filters.mip = linear ? nri::Filter::LINEAR : nri::Filter::NEAREST;
		samplerDesc.filters.op = nri::FilterOp::AVERAGE;
		samplerDesc.addressModes.u = clamp ? nri::AddressMode::CLAMP_TO_EDGE : nri::AddressMode::REPEAT;
		samplerDesc.addressModes.v = clamp ? nri::AddressMode::CLAMP_TO_EDGE : nri::AddressMode::REPEAT;
		samplerDesc.addressModes.w = clamp ? nri::AddressMode::CLAMP_TO_EDGE : nri::AddressMode::REPEAT;
		samplerDesc.compareOp = nri::CompareOp::NONE;

		return mCore.CreateSampler(*mDevice, samplerDesc, mSamplers[(size_t)mode]) == nri::Result::SUCCESS;
	};

	if (!createSampler(NRISamplerMode::ClampLinear, true, true) ||
		!createSampler(NRISamplerMode::WrapLinear, false, true) ||
		!createSampler(NRISamplerMode::ClampPoint, true, false) ||
		!createSampler(NRISamplerMode::WrapPoint, false, false))
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI samplers.\n");
		return false;
	}

	for (size_t i = 0; i < (size_t)NRISamplerMode::Count; ++i)
	{
		nri::DescriptorSet* set = nullptr;
		if (mCore.AllocateDescriptorSets(*mDescriptorPool, *mPipelineLayout, 0, &set, 1, 0) != nri::Result::SUCCESS)
		{
			return false;
		}

		const nri::Descriptor* samplerDescriptor = mSamplers[i];
		nri::UpdateDescriptorRangeDesc updateDesc = {};
		updateDesc.descriptorSet = set;
		updateDesc.rangeIndex = 0;
		updateDesc.descriptors = &samplerDescriptor;
		updateDesc.descriptorNum = 1;
		mCore.UpdateDescriptorRanges(&updateDesc, 1);
		mSamplerSets[i] = set;
	}

	mWhiteTexture = new NRIHardwareTexture(this, 4);
	uint32_t whitePixel = 0xffffffffu;
	mWhiteTexture->CreateTexture((unsigned char*)&whitePixel, 1, 1, 0, false, "WhiteTexture");
	mWhiteTextureSet = mWhiteTexture->GetResource().textureSet;
	return mWhiteTextureSet != nullptr;
}

void NRIRenderDevice::DestroyRenderResources()
{
	delete mWhiteTexture;
	mWhiteTexture = nullptr;
	mWhiteTextureSet = nullptr;

	DestroyTextureResource(mSaveTarget);

	if (mPipelineLayout != nullptr)
	{
		mCore.DestroyPipelineLayout(mPipelineLayout);
		mPipelineLayout = nullptr;
	}

	if (mDescriptorPool != nullptr)
	{
		mCore.DestroyDescriptorPool(mDescriptorPool);
		mDescriptorPool = nullptr;
	}

	for (auto& samplerSet : mSamplerSets)
	{
		samplerSet = nullptr;
	}

	for (auto& sampler : mSamplers)
	{
		if (sampler != nullptr)
		{
			mCore.DestroyDescriptor(sampler);
			sampler = nullptr;
		}
	}
}

bool NRIRenderDevice::BeginCommandList(const char* reason, bool waitForSlotReuse)
{
	if (mDescriptorPool == nullptr || mQueuedFrames.empty())
	{
		return false;
	}

	SelectQueuedFrame(mCurrentQueuedFrameIndex);
	if (mCommandAllocator == nullptr || mCommandBuffer == nullptr)
	{
		return false;
	}

	if (mCommandBufferOpen)
	{
		Printf(TEXTCOLOR_RED "NRI BeginCommandList blocked: command buffer already open (reason=%s queued_frame=%u frame_index=%llu frame_begun=%s).\n",
			reason != nullptr ? reason : "unknown",
			(unsigned)mCurrentQueuedFrameIndex,
			(unsigned long long)mFrameIndex,
			mFrameBegun ? "true" : "false");
		return false;
	}

	if (waitForSlotReuse && mFrameFence != nullptr)
	{
		QueuedFrame& queuedFrame = mQueuedFrames[mCurrentQueuedFrameIndex];
		if (queuedFrame.hasSubmittedWork && queuedFrame.lastSubmittedFenceValue != 0)
		{
			mCore.Wait(*mFrameFence, queuedFrame.lastSubmittedFenceValue);
		}
	}

	mCore.ResetCommandAllocator(*mCommandAllocator);
	const bool success = mCore.BeginCommandBuffer(*mCommandBuffer, mDescriptorPool) == nri::Result::SUCCESS;
	mCommandBufferOpen = success;
	if (!success)
	{
		Printf(TEXTCOLOR_RED "NRI BeginCommandList failed (reason=%s queued_frame=%u frame_index=%llu frame_begun=%s).\n",
			reason != nullptr ? reason : "unknown",
			(unsigned)mCurrentQueuedFrameIndex,
			(unsigned long long)mFrameIndex,
			mFrameBegun ? "true" : "false");
	}
	return success;
}

bool NRIRenderDevice::EnsureSwapChainSize()
{
	if (mSwapChain == nullptr)
	{
		return CreateSwapChain();
	}

	const uint32_t width = (uint32_t)(std::max)(GetClientWidth(), 1);
	const uint32_t height = (uint32_t)(std::max)(GetClientHeight(), 1);
	if (!mSwapChainImages.empty() &&
		mSwapChainImages[0].target.width == width &&
		mSwapChainImages[0].target.height == height)
	{
		return true;
	}

	WaitForCommands(true);
	return CreateSwapChain();
}

void NRIRenderDevice::EndFrameAndPresent()
{
	if (!mFrameBegun || mCommandBuffer == nullptr || mCurrentPresentTarget == nullptr)
	{
		ResetFrameTracking();
		return;
	}

	TransitionTexture(*mCurrentPresentTarget, { nri::AccessBits::NONE, nri::Layout::PRESENT, nri::StageBits::NONE });
	mCore.EndCommandBuffer(*mCommandBuffer);
	mCommandBufferOpen = false;

	const nri::FenceSubmitDesc waitFence = { mSwapChainImages[mAcquireSemaphoreIndex].acquireSemaphore, 0, nri::StageBits::COLOR_ATTACHMENT };
	const nri::FenceSubmitDesc releaseFence = { mSwapChainImages[mCurrentSwapChainImage].releaseSemaphore, 0, nri::StageBits::NONE };
	const uint64_t submittedFenceValue = 1 + mFrameIndex;
	mSubmittedFenceValue = submittedFenceValue;
	mLastFrameBoundaryStats.submittedFenceValue = submittedFenceValue;
	const nri::FenceSubmitDesc frameFence = { mFrameFence, submittedFenceValue, nri::StageBits::NONE };
	const nri::FenceSubmitDesc signalFences[] = { releaseFence, frameFence };
	const nri::CommandBuffer* commandBuffers[] = { mCommandBuffer };

	nri::QueueSubmitDesc submitDesc = {};
	submitDesc.waitFences = &waitFence;
	submitDesc.waitFenceNum = 1;
	submitDesc.commandBuffers = commandBuffers;
	submitDesc.commandBufferNum = 1;
	submitDesc.signalFences = signalFences;
	submitDesc.signalFenceNum = 2;
	{
		ScopedNriTiming submitTiming(NriPTQueueSubmit, mLastFrameBoundaryStats.submitMs);
		mCore.QueueSubmit(*mGraphicsQueue, submitDesc);
	}

	mStreamer.EndStreamerFrame(*mStreamerInstance);
	nri::Result presentResult = nri::Result::FAILURE;
	{
		ScopedNriTiming presentTiming(NriPTQueuePresent, mLastFrameBoundaryStats.presentMs);
		presentResult = mSwapChainInterface.QueuePresent(*mSwapChain, *mSwapChainImages[mCurrentSwapChainImage].releaseSemaphore);
	}
	mLastFrameBoundaryStats.presentResult = presentResult;
	if (presentResult == nri::Result::SUCCESS)
	{
		NoteSwapChainPresent(mCurrentSwapChainImage);
		mHasPresentedSwapChainFrame = true;
	}
	else
	{
		mHasPresentedSwapChainFrame = false;
		Printf(TEXTCOLOR_RED "NRI QueuePresent failed with result '%s'.\n", GetNriResultName(presentResult));
	}
	if (mCurrentQueuedFrameIndex < mQueuedFrames.size())
	{
		QueuedFrame& queuedFrame = mQueuedFrames[mCurrentQueuedFrameIndex];
		queuedFrame.lastSubmittedFenceValue = submittedFenceValue;
		queuedFrame.lastSubmittedFrameIndex = mFrameIndex;
		queuedFrame.hasSubmittedWork = true;
	}
	RecordFrameSequence(mCurrentSwapChainImage, submittedFenceValue, presentResult);
	ResetFrameTracking(presentResult == nri::Result::SUCCESS);
	mFrameIndex++;
}

void NRIRenderDevice::RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect&)> renderFunc)
{
	if (!mInitialized || tex == nullptr)
	{
		return;
	}

	auto* hwTex = static_cast<NRIHardwareTexture*>(tex->GetHardwareTexture(0, 0));
	hwTex->EnsureCanvas(tex);

	NRITextureResource* previousTarget = mActiveTarget;
	mRenderState->EndFrame();
	mActiveTarget = &hwTex->GetResource();

	IntRect bounds = {};
	bounds.width = tex->GetWidth();
	bounds.height = tex->GetHeight();
	renderFunc(bounds);

	mRenderState->EndFrame();
	TransitionTexture(hwTex->GetResource(), NRIShaderResourceState());
	mActiveTarget = previousTarget;
	tex->SetUpdated(true);
}

void NRIRenderDevice::CopyScreenToBuffer(int width, int height, uint8_t* buffer)
{
	if (buffer == nullptr || width <= 0 || height <= 0)
	{
		return;
	}

	NRITextureResource* source = mActiveTarget != nullptr ? mActiveTarget : mCurrentPresentTarget;
	if (source == nullptr || source->texture == nullptr)
	{
		memset(buffer, 0, (size_t)width * (size_t)height * 3);
		return;
	}

	if (mFrameBegun)
	{
		mRenderState->EndFrame();
	}

	if (!BeginCommandList("CopyScreenToBuffer", !mFrameBegun))
	{
		memset(buffer, 0, (size_t)width * (size_t)height * 3);
		return;
	}

	TransitionTexture(*source, NRICopySourceState());

	const nri::DeviceDesc& deviceDesc = mCore.GetDeviceDesc(*mDevice);
	const uint32_t rowPitch = AlignUp((uint32_t)width * 4u, deviceDesc.memoryAlignment.uploadBufferTextureRow);
	const uint32_t slicePitch = AlignUp(rowPitch * (uint32_t)height, deviceDesc.memoryAlignment.uploadBufferTextureSlice);

	nri::BufferDesc readbackDesc = {};
	readbackDesc.size = slicePitch;
	nri::Buffer* readbackBuffer = nullptr;
	if (mCore.CreateCommittedBuffer(*mDevice, nri::MemoryLocation::HOST_READBACK, 0.0f, readbackDesc, readbackBuffer) != nri::Result::SUCCESS)
	{
		memset(buffer, 0, (size_t)width * (size_t)height * 3);
		return;
	}

	nri::TextureRegionDesc region = {};
	region.width = (uint32_t)width;
	region.height = (uint32_t)height;
	region.depth = 1;
	region.planes = nri::PlaneBits::COLOR;

	nri::TextureDataLayoutDesc layout = {};
	layout.rowPitch = rowPitch;
	layout.slicePitch = slicePitch;

	mCore.CmdReadbackTextureToBuffer(*mCommandBuffer, *readbackBuffer, layout, *source->texture, region);
	mCore.EndCommandBuffer(*mCommandBuffer);
	mCommandBufferOpen = false;

	const nri::CommandBuffer* commandBuffers[] = { mCommandBuffer };
	nri::Fence* readbackFence = nullptr;
	if (mCore.CreateFence(*mDevice, 0, readbackFence) != nri::Result::SUCCESS)
	{
		mCore.DestroyBuffer(readbackBuffer);
		memset(buffer, 0, (size_t)width * (size_t)height * 3);
		return;
	}
	nri::FenceSubmitDesc frameFence = {};
	frameFence.fence = readbackFence;
	frameFence.value = 1;

	nri::QueueSubmitDesc submitDesc = {};
	submitDesc.commandBuffers = commandBuffers;
	submitDesc.commandBufferNum = 1;
	submitDesc.signalFences = &frameFence;
	submitDesc.signalFenceNum = 1;
	mCore.QueueSubmit(*mGraphicsQueue, submitDesc);
	mCore.Wait(*readbackFence, frameFence.value);

	const uint8_t* pixels = (const uint8_t*)mCore.MapBuffer(*readbackBuffer, 0, slicePitch);
	for (int y = 0; y < height; ++y)
	{
		const uint8_t* src = pixels + (size_t)(height - y - 1) * rowPitch;
		uint8_t* dst = buffer + (size_t)y * (size_t)width * 3u;

		for (int x = 0; x < width; ++x)
		{
			dst[x * 3 + 0] = src[x * 4 + 2];
			dst[x * 3 + 1] = src[x * 4 + 1];
			dst[x * 3 + 2] = src[x * 4 + 0];
		}
	}

	mCore.UnmapBuffer(*readbackBuffer);
	mCore.DestroyFence(readbackFence);
	mCore.DestroyBuffer(readbackBuffer);
}

void NRIRenderDevice::TransitionTexture(NRITextureResource& texture, nri::AccessLayoutStage after)
{
	if (texture.texture == nullptr)
	{
		return;
	}

	if (texture.state.access == after.access && texture.state.layout == after.layout && texture.state.stages == after.stages)
	{
		return;
	}

	nri::TextureBarrierDesc barrier = {};
	barrier.texture = texture.texture;
	barrier.before = texture.state;
	barrier.after = after;
	barrier.mipNum = 1;
	barrier.layerNum = 1;
	barrier.planes = nri::PlaneBits::COLOR;

	nri::BarrierDesc barriers = {};
	barriers.textures = &barrier;
	barriers.textureNum = 1;
	mCore.CmdBarrier(*mCommandBuffer, barriers);
	texture.state = after;
}

void NRIRenderDevice::PrepareTargetForRendering(NRITextureResource& target, bool)
{
	TransitionTexture(target, NRIColorAttachmentState());
}

void NRIRenderDevice::FinishTargetRendering(NRITextureResource& target, nri::AccessLayoutStage after)
{
	TransitionTexture(target, after);
}

void NRIRenderDevice::DestroyTextureResource(NRITextureResource& resource)
{
	if (resource.colorAttachmentView != nullptr)
	{
		mCore.DestroyDescriptor(resource.colorAttachmentView);
		resource.colorAttachmentView = nullptr;
	}

	if (resource.storageView != nullptr)
	{
		mCore.DestroyDescriptor(resource.storageView);
		resource.storageView = nullptr;
	}

	if (resource.shaderView != nullptr)
	{
		mCore.DestroyDescriptor(resource.shaderView);
		resource.shaderView = nullptr;
	}

	resource.textureSet = nullptr;

	if (resource.owned && resource.texture != nullptr)
	{
		mCore.DestroyTexture(resource.texture);
	}

	resource.texture = nullptr;
	resource.owned = false;
	resource.width = 0;
	resource.height = 0;
	resource.format = nri::Format::UNKNOWN;
	resource.usage = nri::TextureUsageBits::NONE;
	resource.state = {};
}

bool NRIRenderDevice::CreateTextureViews(NRITextureResource& resource)
{
	const uint32_t usage = (uint32_t)resource.usage;
	nri::TextureViewDesc shaderViewDesc = {};
	shaderViewDesc.texture = resource.texture;
	shaderViewDesc.type = nri::TextureView::TEXTURE;
	shaderViewDesc.format = resource.format;
	shaderViewDesc.mipNum = 1;
	shaderViewDesc.layerNum = 1;
	shaderViewDesc.sliceNum = 1;
	shaderViewDesc.readonlyPlanes = nri::PlaneBits::COLOR;
	shaderViewDesc.components = { nri::ComponentSwizzle::IDENTITY, nri::ComponentSwizzle::IDENTITY, nri::ComponentSwizzle::IDENTITY, nri::ComponentSwizzle::IDENTITY };

	if ((usage & (uint32_t)nri::TextureUsageBits::SHADER_RESOURCE) != 0)
	{
		if (mCore.CreateTextureView(shaderViewDesc, resource.shaderView) != nri::Result::SUCCESS)
		{
			return false;
		}

		resource.textureSet = CreateTextureSet(resource.shaderView);
		if (resource.textureSet == nullptr)
		{
			return false;
		}
	}

	if ((usage & (uint32_t)nri::TextureUsageBits::SHADER_RESOURCE_STORAGE) != 0)
	{
		nri::TextureViewDesc storageViewDesc = shaderViewDesc;
		storageViewDesc.type = nri::TextureView::STORAGE_TEXTURE;
		if (mCore.CreateTextureView(storageViewDesc, resource.storageView) != nri::Result::SUCCESS)
		{
			return false;
		}
	}

	if ((usage & (uint32_t)nri::TextureUsageBits::COLOR_ATTACHMENT) != 0)
	{
		nri::TextureViewDesc colorViewDesc = shaderViewDesc;
		colorViewDesc.type = nri::TextureView::COLOR_ATTACHMENT;
		if (mCore.CreateTextureView(colorViewDesc, resource.colorAttachmentView) != nri::Result::SUCCESS)
		{
			return false;
		}
	}

	return true;
}

bool NRIRenderDevice::CreateOwnedTexture(NRITextureResource& resource, uint32_t width, uint32_t height, nri::Format format, nri::TextureUsageBits usage)
{
	nri::TextureDesc textureDesc = {};
	textureDesc.type = nri::TextureType::TEXTURE_2D;
	textureDesc.usage = usage;
	textureDesc.format = format;
	textureDesc.width = width;
	textureDesc.height = height;
	textureDesc.depth = 1;
	textureDesc.mipNum = 1;
	textureDesc.layerNum = 1;
	textureDesc.sampleNum = 1;

	if (mCore.CreateCommittedTexture(*mDevice, nri::MemoryLocation::DEVICE, 0.0f, textureDesc, resource.texture) != nri::Result::SUCCESS)
	{
		return false;
	}

	resource.width = width;
	resource.height = height;
	resource.format = format;
	resource.usage = usage;
	resource.owned = true;
	resource.state = {};
	return CreateTextureViews(resource);
}

bool NRIRenderDevice::UploadTextureData(NRITextureResource& resource, const void* data, uint32_t width, uint32_t height, uint32_t rowPitch)
{
	if (data == nullptr || width == 0 || height == 0 || rowPitch == 0)
	{
		return false;
	}

	const uint32_t slicePitch = rowPitch * height;
	std::vector<uint8_t> uploadCopy(slicePitch);
	memcpy(uploadCopy.data(), data, slicePitch);

	nri::TextureSubresourceUploadDesc subresource = {};
	subresource.slices = uploadCopy.data();
	subresource.sliceNum = 1;
	subresource.rowPitch = rowPitch;
	subresource.slicePitch = slicePitch;

	nri::TextureUploadDesc uploadDesc = {};
	uploadDesc.subresources = &subresource;
	uploadDesc.texture = resource.texture;
	uploadDesc.after = NRIShaderResourceState();
	uploadDesc.planes = nri::PlaneBits::COLOR;

	const nri::Result result = mHelper.UploadData(*mGraphicsQueue, &uploadDesc, 1, nullptr, 0);
	if (result == nri::Result::SUCCESS)
	{
		resource.state = NRIShaderResourceState();
		resource.width = width;
		resource.height = height;
		return true;
	}

	return false;
}

bool NRIRenderDevice::CopyCurrentTargetToTexture(NRITextureResource& destination)
{
	NRITextureResource* source = mFrameBegun && mActiveTarget != nullptr ? mActiveTarget : mCurrentPresentTarget;
	if (source == nullptr || source->texture == nullptr || destination.texture == nullptr)
	{
		return false;
	}

	const bool useActiveFrameCommandBuffer = mFrameBegun && mCommandBuffer != nullptr;
	if (mFrameBegun)
	{
		mRenderState->EndFrame();
	}

	if (!useActiveFrameCommandBuffer && !BeginCommandList("CopyCurrentTargetToTexture", true))
	{
		return false;
	}

	const nri::AccessLayoutStage sourceStateBeforeCopy = source->state;
	TransitionTexture(*source, NRICopySourceState());
	TransitionTexture(destination, NRICopyDestinationState());
	mCore.CmdCopyTexture(*mCommandBuffer, *destination.texture, nullptr, *source->texture, nullptr);
	TransitionTexture(*source, sourceStateBeforeCopy);
	TransitionTexture(destination, NRIShaderResourceState());

	if (useActiveFrameCommandBuffer)
	{
		return true;
	}

	mCore.EndCommandBuffer(*mCommandBuffer);
	mCommandBufferOpen = false;

	const nri::CommandBuffer* commandBuffers[] = { mCommandBuffer };
	nri::Fence* copyFence = nullptr;
	if (mCore.CreateFence(*mDevice, 0, copyFence) != nri::Result::SUCCESS)
	{
		return false;
	}
	nri::FenceSubmitDesc frameFence = {};
	frameFence.fence = copyFence;
	frameFence.value = 1;

	nri::QueueSubmitDesc submitDesc = {};
	submitDesc.commandBuffers = commandBuffers;
	submitDesc.commandBufferNum = 1;
	submitDesc.signalFences = &frameFence;
	submitDesc.signalFenceNum = 1;
	mCore.QueueSubmit(*mGraphicsQueue, submitDesc);
	mCore.Wait(*copyFence, frameFence.value);
	mCore.DestroyFence(copyFence);
	return true;
}

bool NRIRenderDevice::LoadShaderBlob(const char* fileName, std::vector<uint8_t>& outBlob)
{
	FString shaderPath = progdir;
	shaderPath << "shaders/nri/" << fileName;

	std::ifstream file(shaderPath.GetChars(), std::ios::binary | std::ios::ate);
	if (!file.is_open())
	{
		Printf(TEXTCOLOR_RED "Failed to open NRI shader '%s'.\n", shaderPath.GetChars());
		return false;
	}

	const std::streamsize size = file.tellg();
	file.seekg(0, std::ios::beg);
	outBlob.resize((size_t)size);
	return file.read((char*)outBlob.data(), size).good();
}

const void* NRIRenderDevice::GetVertexShaderBytecode(size_t& size) const
{
	size = mVertexShaderBlob.size();
	return mVertexShaderBlob.empty() ? nullptr : mVertexShaderBlob.data();
}

const void* NRIRenderDevice::GetPixelShaderBytecode(size_t& size) const
{
	size = mPixelShaderBlob.size();
	return mPixelShaderBlob.empty() ? nullptr : mPixelShaderBlob.data();
}

nri::GraphicsAPI NRIRenderDevice::GetSelectedAPI() const
{
	return FString((const char*)nri_api).CompareNoCase("d3d12") == 0 ? nri::GraphicsAPI::D3D12 : nri::GraphicsAPI::VK;
}

NRISamplerMode NRIRenderDevice::GetSamplerMode(int clampMode) const
{
	const bool point = clampMode == CLAMP_NOFILTER || clampMode == CLAMP_NOFILTER_X || clampMode == CLAMP_NOFILTER_Y || clampMode == CLAMP_NOFILTER_XY;
	const bool clamp = clampMode != CLAMP_NONE && clampMode != CLAMP_CAMTEX;

	if (point)
	{
		return clamp ? NRISamplerMode::ClampPoint : NRISamplerMode::WrapPoint;
	}

	return clamp ? NRISamplerMode::ClampLinear : NRISamplerMode::WrapLinear;
}

nri::DescriptorSet* NRIRenderDevice::GetSamplerSet(NRISamplerMode mode) const
{
	return mSamplerSets[(size_t)mode];
}

nri::DescriptorSet* NRIRenderDevice::CreateTextureSet(nri::Descriptor* shaderView)
{
	nri::DescriptorSet* set = nullptr;
	if (mCore.AllocateDescriptorSets(*mDescriptorPool, *mPipelineLayout, 1, &set, 1, 0) != nri::Result::SUCCESS)
	{
		return nullptr;
	}

	const nri::Descriptor* descriptor = shaderView;
	nri::UpdateDescriptorRangeDesc updateDesc = {};
	updateDesc.descriptorSet = set;
	updateDesc.rangeIndex = 0;
	updateDesc.descriptors = &descriptor;
	updateDesc.descriptorNum = 1;
	mCore.UpdateDescriptorRanges(&updateDesc, 1);
	return set;
}

void NRIRenderDevice::ResetFrameTracking(bool presentedAcquiredImage)
{
	if (mHasAcquiredSwapChainImage && !presentedAcquiredImage)
	{
		NoteSwapChainAbandon(mCurrentSwapChainImage);
	}

	mFrameBegun = false;
	mCommandBufferOpen = false;
	mCurrentPresentTarget = nullptr;
	mActiveTarget = nullptr;
	mHasAcquiredSwapChainImage = false;
	mCurrentSwapChainImage = 0;
}

uint32_t NRIRenderDevice::GetQueuedFrameIndex(uint64_t frameIndex) const
{
	return mQueuedFrames.empty() ? 0 : (uint32_t)(frameIndex % mQueuedFrames.size());
}

void NRIRenderDevice::SelectQueuedFrame(uint32_t queuedFrameIndex)
{
	if (mQueuedFrames.empty())
	{
		mCurrentQueuedFrameIndex = 0;
		mCommandAllocator = nullptr;
		mCommandBuffer = nullptr;
		return;
	}

	mCurrentQueuedFrameIndex = queuedFrameIndex % (uint32_t)mQueuedFrames.size();
	QueuedFrame& queuedFrame = mQueuedFrames[mCurrentQueuedFrameIndex];
	mCommandAllocator = queuedFrame.commandAllocator;
	mCommandBuffer = queuedFrame.commandBuffer;
}
