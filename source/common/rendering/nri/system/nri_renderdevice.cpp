#include "nri_renderdevice.h"

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
#include "hwrenderer/data/hw_viewpointbuffer.h"

#include <windows.h>

#include <algorithm>
#include <fstream>

EXTERN_CVAR(String, nri_api)

namespace
{
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
}

NRIRenderDevice::NRIRenderDevice(void* hMonitor, bool fullscreen)
	: SystemBaseFrameBuffer(hMonitor, fullscreen), mRenderState(std::make_unique<NRIRenderState>(this))
{
	vendorstring = "NRI";
	glslversion = 6.6f;
}

NRIRenderDevice::~NRIRenderDevice()
{
	WaitForCommands(true);
	DestroyRenderResources();
	DestroySwapChain();

	if (mCommandBuffer != nullptr)
	{
		mCore.DestroyCommandBuffer(mCommandBuffer);
		mCommandBuffer = nullptr;
	}

	if (mCommandAllocator != nullptr)
	{
		mCore.DestroyCommandAllocator(mCommandAllocator);
		mCommandAllocator = nullptr;
	}

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
}

void NRIRenderDevice::Update()
{
	if (mInitialized && mFrameBegun)
	{
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

	LogStartup();
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

	WaitForCommands(false);
	SetViewportRects(nullptr);

	if (!EnsureSwapChainSize())
	{
		return;
	}

	if (!BeginCommandList())
	{
		return;
	}

	mAcquireSemaphoreIndex = mSwapChainImages.empty() ? 0 : (uint32_t)(mSubmittedFenceValue % mSwapChainImages.size());
	nri::Result acquireResult = mSwapChainInterface.AcquireNextTexture(*mSwapChain, *mSwapChainImages[mAcquireSemaphoreIndex].acquireSemaphore, mCurrentSwapChainImage);
	if (acquireResult == nri::Result::OUT_OF_DATE)
	{
		if (!EnsureSwapChainSize())
		{
			return;
		}

		acquireResult = mSwapChainInterface.AcquireNextTexture(*mSwapChain, *mSwapChainImages[mAcquireSemaphoreIndex].acquireSemaphore, mCurrentSwapChainImage);
	}

	if (acquireResult != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "NRI failed to acquire swapchain image.\n");
		return;
	}

	mCurrentPresentTarget = &mSwapChainImages[mCurrentSwapChainImage].target;
	mActiveTarget = mUsingSaveTarget ? &mSaveTarget : mCurrentPresentTarget;
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

	if (mFrameFence != nullptr && mSubmittedFenceValue != 0)
	{
		mCore.Wait(*mFrameFence, mSubmittedFenceValue);
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

	mNriModule = module;
	return true;
}

bool NRIRenderDevice::CreateDevice()
{
	nri::AdapterDesc adapters[8] = {};
	uint32_t adapterCount = (uint32_t)std::size(adapters);
	if (mEnumerateAdapters(adapters, adapterCount) != nri::Result::SUCCESS || adapterCount == 0)
	{
		Printf(TEXTCOLOR_RED "Failed to enumerate NRI adapters.\n");
		return false;
	}

	nri::DeviceCreationDesc creationDesc = {};
	creationDesc.graphicsAPI = GetSelectedAPI();
	creationDesc.adapterDesc = &adapters[0];
	creationDesc.enableGraphicsAPIValidation = false;
	creationDesc.enableNRIValidation = false;
	creationDesc.disableVKRayTracing = true;
	creationDesc.disableD3D12EnhancedBarriers = false;
	creationDesc.vkBindingOffsets = {};

	if (mCreateDeviceFn(creationDesc, mDevice) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI device for API '%s'.\n", (const char*)nri_api);
		return false;
	}

	if (mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::CoreInterface), &mCore) != nri::Result::SUCCESS ||
		mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::HelperInterface), &mHelper) != nri::Result::SUCCESS ||
		mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::StreamerInterface), &mStreamer) != nri::Result::SUCCESS ||
		mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::SwapChainInterface), &mSwapChainInterface) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to retrieve NRI interfaces.\n");
		return false;
	}

	if (mCore.GetQueue(*mDevice, nri::QueueType::GRAPHICS, 0, mGraphicsQueue) != nri::Result::SUCCESS ||
		mCore.CreateFence(*mDevice, 0, mFrameFence) != nri::Result::SUCCESS ||
		mCore.CreateCommandAllocator(*mGraphicsQueue, mCommandAllocator) != nri::Result::SUCCESS ||
		mCore.CreateCommandBuffer(*mCommandAllocator, mCommandBuffer) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI queue objects.\n");
		return false;
	}

	nri::StreamerDesc streamerDesc = {};
	streamerDesc.dynamicBufferMemoryLocation = nri::MemoryLocation::DEVICE_UPLOAD;
	streamerDesc.dynamicBufferDesc = {};
	streamerDesc.dynamicBufferDesc.usage = NRIFlags(nri::BufferUsageBits::VERTEX_BUFFER, nri::BufferUsageBits::INDEX_BUFFER);
	streamerDesc.queuedFrameNum = 1;
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
	swapChainDesc.textureNum = 3;
	swapChainDesc.format = nri::SwapChainFormat::BT709_G22_8BIT;
	swapChainDesc.flags = nri::SwapChainBits::NONE;

	if (mSwapChainInterface.CreateSwapChain(*mDevice, swapChainDesc, mSwapChain) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI swapchain.\n");
		return false;
	}

	uint32_t textureCount = 0;
	nri::Texture* const* textures = mSwapChainInterface.GetSwapChainTextures(*mSwapChain, textureCount);
	mSwapChainImages.resize(textureCount);

	for (uint32_t i = 0; i < textureCount; ++i)
	{
		auto& image = mSwapChainImages[i];
		image.target.texture = textures[i];
		image.target.owned = false;

		const nri::TextureDesc& desc = mCore.GetTextureDesc(*textures[i]);
		image.target.width = desc.width;
		image.target.height = desc.height;
		image.target.format = desc.format;
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

	return true;
}

void NRIRenderDevice::DestroySwapChain()
{
	ResetFrameTracking();

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

bool NRIRenderDevice::BeginCommandList()
{
	if (mCommandAllocator == nullptr || mCommandBuffer == nullptr || mDescriptorPool == nullptr)
	{
		return false;
	}

	mCore.ResetCommandAllocator(*mCommandAllocator);
	return mCore.BeginCommandBuffer(*mCommandBuffer, mDescriptorPool) == nri::Result::SUCCESS;
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

	const nri::FenceSubmitDesc waitFence = { mSwapChainImages[mAcquireSemaphoreIndex].acquireSemaphore, 0, nri::StageBits::COLOR_ATTACHMENT };
	const nri::FenceSubmitDesc releaseFence = { mSwapChainImages[mCurrentSwapChainImage].releaseSemaphore, 0, nri::StageBits::NONE };
	const nri::FenceSubmitDesc frameFence = { mFrameFence, ++mSubmittedFenceValue, nri::StageBits::NONE };
	const nri::FenceSubmitDesc signalFences[] = { releaseFence, frameFence };
	const nri::CommandBuffer* commandBuffers[] = { mCommandBuffer };

	nri::QueueSubmitDesc submitDesc = {};
	submitDesc.waitFences = &waitFence;
	submitDesc.waitFenceNum = 1;
	submitDesc.commandBuffers = commandBuffers;
	submitDesc.commandBufferNum = 1;
	submitDesc.signalFences = signalFences;
	submitDesc.signalFenceNum = 2;
	mCore.QueueSubmit(*mGraphicsQueue, submitDesc);

	mStreamer.EndStreamerFrame(*mStreamerInstance);
	mSwapChainInterface.QueuePresent(*mSwapChain, *mSwapChainImages[mCurrentSwapChainImage].releaseSemaphore);
	ResetFrameTracking();
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

	if (!BeginCommandList())
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

	const nri::CommandBuffer* commandBuffers[] = { mCommandBuffer };
	nri::FenceSubmitDesc frameFence = {};
	frameFence.fence = mFrameFence;
	frameFence.value = ++mSubmittedFenceValue;

	nri::QueueSubmitDesc submitDesc = {};
	submitDesc.commandBuffers = commandBuffers;
	submitDesc.commandBufferNum = 1;
	submitDesc.signalFences = &frameFence;
	submitDesc.signalFenceNum = 1;
	mCore.QueueSubmit(*mGraphicsQueue, submitDesc);
	mCore.Wait(*mFrameFence, mSubmittedFenceValue);

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
	resource.state = {};
}

bool NRIRenderDevice::CreateTextureViews(NRITextureResource& resource)
{
	nri::TextureViewDesc shaderViewDesc = {};
	shaderViewDesc.texture = resource.texture;
	shaderViewDesc.type = nri::TextureView::TEXTURE;
	shaderViewDesc.format = resource.format;
	shaderViewDesc.mipNum = 1;
	shaderViewDesc.layerNum = 1;
	shaderViewDesc.sliceNum = 1;
	shaderViewDesc.readonlyPlanes = nri::PlaneBits::COLOR;
	shaderViewDesc.components = { nri::ComponentSwizzle::IDENTITY, nri::ComponentSwizzle::IDENTITY, nri::ComponentSwizzle::IDENTITY, nri::ComponentSwizzle::IDENTITY };

	if (mCore.CreateTextureView(shaderViewDesc, resource.shaderView) != nri::Result::SUCCESS)
	{
		return false;
	}

	resource.textureSet = CreateTextureSet(resource.shaderView);
	if (resource.textureSet == nullptr)
	{
		return false;
	}

	nri::TextureViewDesc colorViewDesc = shaderViewDesc;
	colorViewDesc.type = nri::TextureView::COLOR_ATTACHMENT;
	mCore.CreateTextureView(colorViewDesc, resource.colorAttachmentView);

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
	resource.owned = true;
	resource.state = {};
	return CreateTextureViews(resource);
}

bool NRIRenderDevice::UploadTextureData(NRITextureResource& resource, const void* data, uint32_t width, uint32_t height, uint32_t rowPitch)
{
	nri::TextureSubresourceUploadDesc subresource = {};
	subresource.slices = data;
	subresource.sliceNum = 1;
	subresource.rowPitch = rowPitch;
	subresource.slicePitch = rowPitch * height;

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

	if (mFrameBegun)
	{
		mRenderState->EndFrame();
	}

	if (!BeginCommandList())
	{
		return false;
	}

	TransitionTexture(*source, NRICopySourceState());
	TransitionTexture(destination, NRICopyDestinationState());
	mCore.CmdCopyTexture(*mCommandBuffer, *destination.texture, nullptr, *source->texture, nullptr);
	TransitionTexture(destination, NRIShaderResourceState());
	mCore.EndCommandBuffer(*mCommandBuffer);

	const nri::CommandBuffer* commandBuffers[] = { mCommandBuffer };
	nri::FenceSubmitDesc frameFence = {};
	frameFence.fence = mFrameFence;
	frameFence.value = ++mSubmittedFenceValue;

	nri::QueueSubmitDesc submitDesc = {};
	submitDesc.commandBuffers = commandBuffers;
	submitDesc.commandBufferNum = 1;
	submitDesc.signalFences = &frameFence;
	submitDesc.signalFenceNum = 1;
	mCore.QueueSubmit(*mGraphicsQueue, submitDesc);
	mCore.Wait(*mFrameFence, mSubmittedFenceValue);
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

void NRIRenderDevice::ResetFrameTracking()
{
	mFrameBegun = false;
	mCurrentPresentTarget = nullptr;
	mActiveTarget = nullptr;
	mCurrentSwapChainImage = 0;
}
