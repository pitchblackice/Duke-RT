#pragma once

#include "base_sysfb.h"
#include "nri_local.h"

#include <memory>
#include <vector>

class NRIRenderState;
class NRIHardwareDataBuffer;
class NRIHardwareTexture;
class NRIHardwareVertexBuffer;
class NRIHardwareIndexBuffer;
class FCanvasTexture;
class FTexture;
class NRIRenderer;

class NRIRenderDevice : public SystemBaseFrameBuffer
{
	typedef SystemBaseFrameBuffer Super;

public:
	NRIRenderDevice(void *hMonitor, bool fullscreen);
	~NRIRenderDevice();

	void Update() override;
	void InitializeState() override;
	bool CompileNextShader() override;
	int GetShaderCount() override;
	int Backend() override { return 4; }
	const char* DeviceName() const override;
	void BeginFrame() override;
	FRenderState* RenderState() override;
	void Draw2D() override;
	void WaitForCommands(bool finish) override;
	void SetSaveBuffers(bool yes) override;
	void ImageTransitionScene(bool unknown) override;
	void SetActiveRenderTarget() override;
	bool RenderPathTracedScene(HWDrawInfo& di, int drawmode, bool portal) override;

	IHardwareTexture* CreateHardwareTexture(int numchannels) override;
	IVertexBuffer* CreateVertexBuffer() override;
	IIndexBuffer* CreateIndexBuffer() override;
	IDataBuffer* CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize) override;
	FTexture* WipeStartScreen() override;
	FTexture* WipeEndScreen() override;
	TArray<uint8_t> GetScreenshotBuffer(int& pitch, ESSType& color_type, float& gamma) override;
	bool FlipSavePic() const override { return false; }

private:
	using PFN_nriEnumerateAdapters = nri::Result(NRI_CALL*)(nri::AdapterDesc*, uint32_t&);
	using PFN_nriCreateDevice = nri::Result(NRI_CALL*)(const nri::DeviceCreationDesc&, nri::Device*&);
	using PFN_nriDestroyDevice = void (NRI_CALL*)(nri::Device*);
	using PFN_nriGetInterface = nri::Result(NRI_CALL*)(const nri::Device&, const char*, size_t, void*);

	void LogStartup();
	bool LoadNRI();
	bool CreateDevice();
	bool CreateSwapChain();
	void DestroySwapChain();
	bool CreateRenderResources();
	void DestroyRenderResources();
	bool BeginCommandList();
	bool EnsureSwapChainSize();
	void EndFrameAndPresent();
	void RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect&)> renderFunc) override;
	void CopyScreenToBuffer(int width, int height, uint8_t* buffer) override;
	void TransitionTexture(NRITextureResource& texture, nri::AccessLayoutStage after);
	void PrepareTargetForRendering(NRITextureResource& target, bool clear);
	void FinishTargetRendering(NRITextureResource& target, nri::AccessLayoutStage after);
	void DestroyTextureResource(NRITextureResource& resource);
	bool CreateTextureViews(NRITextureResource& resource);
	bool CreateOwnedTexture(NRITextureResource& resource, uint32_t width, uint32_t height, nri::Format format, nri::TextureUsageBits usage);
	bool UploadTextureData(NRITextureResource& resource, const void* data, uint32_t width, uint32_t height, uint32_t rowPitch);
	bool CopyCurrentTargetToTexture(NRITextureResource& destination);
	bool LoadShaderBlob(const char* fileName, std::vector<uint8_t>& outBlob);
	const void* GetVertexShaderBytecode(size_t& size) const;
	const void* GetPixelShaderBytecode(size_t& size) const;
	nri::GraphicsAPI GetSelectedAPI() const;
	NRISamplerMode GetSamplerMode(int clampMode) const;
	nri::DescriptorSet* GetSamplerSet(NRISamplerMode mode) const;
	nri::DescriptorSet* CreateTextureSet(nri::Descriptor* shaderView);
	void ResetFrameTracking();

	friend class NRIHardwareTexture;
	friend class NRIRenderState;
	friend class NRIRenderer;

	std::unique_ptr<NRIRenderState> mRenderState;
	std::unique_ptr<NRIRenderer> mRenderer;
	void* mNriModule = nullptr;
	PFN_nriEnumerateAdapters mEnumerateAdapters = nullptr;
	PFN_nriCreateDevice mCreateDeviceFn = nullptr;
	PFN_nriDestroyDevice mDestroyDeviceFn = nullptr;
	PFN_nriGetInterface mGetInterfaceFn = nullptr;

	nri::CoreInterface mCore = {};
	nri::HelperInterface mHelper = {};
	nri::RayTracingInterface mRayTracing = {};
	nri::StreamerInterface mStreamer = {};
	nri::SwapChainInterface mSwapChainInterface = {};
	nri::Device* mDevice = nullptr;
	nri::Queue* mGraphicsQueue = nullptr;
	nri::SwapChain* mSwapChain = nullptr;
	nri::Streamer* mStreamerInstance = nullptr;
	nri::Fence* mFrameFence = nullptr;
	nri::CommandAllocator* mCommandAllocator = nullptr;
	nri::CommandBuffer* mCommandBuffer = nullptr;
	nri::DescriptorPool* mDescriptorPool = nullptr;
	nri::PipelineLayout* mPipelineLayout = nullptr;
	nri::Descriptor* mConstantBufferView = nullptr;
	nri::Descriptor* mSamplers[(size_t)NRISamplerMode::Count] = {};
	nri::DescriptorSet* mSamplerSets[(size_t)NRISamplerMode::Count] = {};

	std::vector<NRISwapChainImage> mSwapChainImages;
	NRITextureResource mSaveTarget;
	NRITextureResource* mActiveTarget = nullptr;
	NRITextureResource* mCurrentPresentTarget = nullptr;
	nri::DescriptorSet* mWhiteTextureSet = nullptr;
	NRIHardwareTexture* mWhiteTexture = nullptr;

	std::vector<uint8_t> mVertexShaderBlob;
	std::vector<uint8_t> mPixelShaderBlob;
	FString mDeviceName = "NRI";
	uint64_t mSubmittedFenceValue = 0;
	bool mFrameBegun = false;
	bool mUsingSaveTarget = false;
	uint32_t mCurrentSwapChainImage = 0;
	uint32_t mAcquireSemaphoreIndex = 0;
	bool mInitialized = false;
	bool mLoggedStartup = false;
};
