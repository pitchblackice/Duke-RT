#pragma once

#include "base_sysfb.h"
#include "../framegen/nri_framegen.h"
#include "nri_local.h"
#include "Extensions/NRIWrapperD3D12.h"

#include <memory>
#include <vector>

class NRIRenderState;
class NRIHardwareDataBuffer;
class NRIHardwareTexture;
class NRIHardwareVertexBuffer;
class NRIHardwareIndexBuffer;
class FCanvasTexture;
class FTexture;
class FGameTexture;
class NRIRenderer;
class NRIUpscalerContext;

#ifdef _WIN32
struct ID3D12Device;
struct ID3D12CommandQueue;
struct IDXGISwapChain4;
#endif

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
	void SetVSync(bool vsync) override;
	void SetSaveBuffers(bool yes) override;
	void ImageTransitionScene(bool unknown) override;
	void SetSceneRenderTarget(bool useSSAO) override;
	void SetActiveRenderTarget() override;
	void PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D) override;
	bool RenderPathTracedScene(HWDrawInfo& di, int drawmode, bool portal) override;
	bool HasActiveSceneFrame() const override;
	bool StartPathTracingLevelPreload() override;
	bool TickPathTracingLevelPreload() override;
	bool IsPathTracingLevelPreloadPending() const override;
	void CancelPathTracingLevelPreload() override;
	void PrintPathTracingSurfaceProbeStatus() const override;

	IHardwareTexture* CreateHardwareTexture(int numchannels) override;
	IVertexBuffer* CreateVertexBuffer() override;
	IIndexBuffer* CreateIndexBuffer() override;
	IDataBuffer* CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize) override;
	FTexture* WipeStartScreen() override;
	FTexture* WipeEndScreen() override;
	TArray<uint8_t> GetScreenshotBuffer(int& pitch, ESSType& color_type, float& gamma) override;
	bool FlipSavePic() const override { return false; }
	void PrintPathTracingCaps() const;
	void PrintPathTracingStatus() const;
	void PrintPathTracingBuffers() const;
	void ResetPathTracingHistory();
	bool SpawnPathTracingPointLight(float red, float green, float blue, float intensity, float radius, float offset, uint32_t& outId);
	bool RemovePathTracingPointLight(uint32_t id);
	void ClearPathTracingPointLights();
	void PrintPathTracingPointLights() const;
	bool SpawnPathTracingDebugSphere(float diameter, float distance, float metalness, float roughness, uint32_t& outId);
	bool RemovePathTracingDebugSphere(uint32_t id);
	void ClearPathTracingDebugSpheres();
	void PrintPathTracingDebugSpheres() const;
	bool AddPathTracingSpriteTileLightHeuristic(uint32_t textureId, float red, float green, float blue, float intensity, float radius, uint32_t flickerFrames, uint32_t& outRuleId);
	void ClearPathTracingLightHeuristics();
	void PrintPathTracingLightHeuristics() const;
	void PrintPathTracingSceneLightDump(float radius, uint32_t limit) const;
	void PrintPathTracingLightClusters() const;
	bool AddPathTracingTextureEmissiveHeuristic(uint32_t textureId, uint32_t emissiveMode, float intensityScale, const float* emissiveColor, bool hasExplicitColor, uint32_t& outRuleId);
	void ClearPathTracingEmissiveHeuristics();
	void PrintPathTracingEmissiveHeuristics() const;
	void NotifyPathTracingGlowControlChange();
	void NotifyPathTracingDebugSphereTessellationChange();
	void PrintPathTracingEmissiveSurfaces(float radius, uint32_t limit) const;
	void PrintPathTracingSectorLights(float radius, uint32_t limit) const;
	void PrintPathTracingMapChunkDump(int32_t chunkIndex) const;
	void PrintPathTracingMapChunkCompare(int32_t chunkIndex) const;
	bool ShouldSkipSceneBuildForPathTracedScene(int drawmode, bool portal) const override;
	bool IsFullscreenModeActive() const { return m_Fullscreen; }
#ifdef _WIN32
	ID3D12Device* GetNativeD3D12Device() const { return mNativeD3D12Device; }
	ID3D12CommandQueue* GetNativeD3D12GraphicsQueue() const { return mNativeD3D12GraphicsQueue; }
	IDXGISwapChain4* GetNativeD3D12SwapChain() const { return mNativeD3D12SwapChain; }
	bool IsFrameGenerationPresentPathActive() const { return !mFrameGenerationPresentImages.empty() && mFrameGeneration.ShouldUsePresentBridge(); }
#endif

private:
	static constexpr uint32_t FrameSequenceHistorySize = 8;
	static constexpr uint32_t QueuedFrameCount = 3;

	struct QueuedFrame
	{
		nri::CommandAllocator* commandAllocator = nullptr;
		nri::CommandBuffer* commandBuffer = nullptr;
		uint64_t lastSubmittedFenceValue = 0;
		uint64_t lastSubmittedFrameIndex = 0;
		bool hasSubmittedWork = false;
	};

	struct FrameSequenceEntry
	{
		uint64_t frameNumber = 0;
		uint64_t frameIndex = 0;
		uint64_t submittedFenceValue = 0;
		uint32_t queuedFrameIndex = 0;
		uint32_t acquiredImageIndex = 0;
		uint32_t acquireSemaphoreIndex = 0;
		uint32_t presentedImageIndex = 0;
		uint32_t releaseSemaphoreIndex = 0;
		nri::Result presentResult = nri::Result::FAILURE;
		bool sanityFrameUsed = false;
		bool valid = false;
	};

	struct FrameBoundaryDebugStats
	{
		uint64_t frameNumber = 0;
		uint64_t frameIndex = 0;
		double waitMs = 0.0;
		double waitForPresentMs = 0.0;
		double acquireMs = 0.0;
		double submitMs = 0.0;
		double presentMs = 0.0;
		uint64_t submittedFenceValue = 0;
		nri::Result waitForPresentResult = nri::Result::FAILURE;
		nri::Result acquireResult = nri::Result::FAILURE;
		nri::Result presentResult = nri::Result::FAILURE;
		uint32_t queuedFrameIndex = 0;
		uint32_t swapChainImageIndex = 0;
		uint32_t acquireSemaphoreIndex = 0;
		bool sanityModeEnabled = false;
		bool sanityFrameUsed = false;
		bool sceneTargetSelected = false;
		bool pathTracedSceneRendered = false;
		bool sceneCopiedToPresent = false;
		bool postProcessInvoked = false;
	};

	struct Texture2DDebugStats
	{
		uint64_t frameNumber = 0;
		uint32_t ensureCalls = 0;
		uint32_t canvasEnsures = 0;
		uint32_t cacheHits = 0;
		uint32_t cacheMisses = 0;
		uint32_t uploadAttempts = 0;
		uint32_t uploadFailures = 0;
		uint32_t resourceCreates = 0;
		uint32_t resourceRecreates = 0;
		uint64_t uploadedBytes = 0;
		uint64_t totalEnsureCalls = 0;
		uint64_t totalCanvasEnsures = 0;
		uint64_t totalCacheHits = 0;
		uint64_t totalCacheMisses = 0;
		uint64_t totalUploadAttempts = 0;
		uint64_t totalUploadFailures = 0;
		uint64_t totalResourceCreates = 0;
		uint64_t totalResourceRecreates = 0;
		uint64_t totalUploadedBytes = 0;
	};

	using PFN_nriEnumerateAdapters = nri::Result(NRI_CALL*)(nri::AdapterDesc*, uint32_t&);
	using PFN_nriCreateDevice = nri::Result(NRI_CALL*)(const nri::DeviceCreationDesc&, nri::Device*&);
	using PFN_nriDestroyDevice = void (NRI_CALL*)(nri::Device*);
	using PFN_nriGetInterface = nri::Result(NRI_CALL*)(const nri::Device&, const char*, size_t, void*);

	void LogStartup();
	bool LoadNRI();
	bool CreateDevice();
	bool CreateQueuedFrames();
	bool CreateSwapChain();
	void DestroyQueuedFrames();
	void DestroySwapChain();
	bool CreateRenderResources();
	void DestroyRenderResources();
	bool BeginCommandList(const char* reason, bool waitForSlotReuse = false);
	bool EnsureSwapChainSize();
	void EndFrameAndPresent();
	void LogD3D12FailureDiagnostics(const char* context);
	void RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect&)> renderFunc) override;
	void CopyScreenToBuffer(int width, int height, uint8_t* buffer) override;
	void TransitionTexture(NRITextureResource& texture, nri::AccessLayoutStage after);
	void PrepareTargetForRendering(NRITextureResource& target, bool clear);
	void FinishTargetRendering(NRITextureResource& target, nri::AccessLayoutStage after);
	void DestroyTextureResource(NRITextureResource& resource);
	bool CreateTextureViews(NRITextureResource& resource);
	bool CreateOwnedTexture(NRITextureResource& resource, uint32_t width, uint32_t height, nri::Format format, nri::TextureUsageBits usage, nri::TextureType type = nri::TextureType::TEXTURE_2D, uint32_t layerNum = 1, nri::TextureView shaderViewType = nri::TextureView::TEXTURE);
	bool UploadTextureData(NRITextureResource& resource, const void* data, uint32_t width, uint32_t height, uint32_t rowPitch);
	bool UploadTextureSubresources(NRITextureResource& resource, const nri::TextureSubresourceUploadDesc* subresources, uint32_t subresourceNum, uint32_t width, uint32_t height);
	bool CopyCurrentTargetToTexture(NRITextureResource& destination);
	bool LoadShaderBlob(const char* fileName, std::vector<uint8_t>& outBlob);
	const void* GetVertexShaderBytecode(size_t& size) const;
	const void* GetPixelShaderBytecode(size_t& size) const;
	nri::GraphicsAPI GetSelectedAPI() const;
	NRISamplerMode GetSamplerMode(int clampMode) const;
	nri::DescriptorSet* GetSamplerSet(NRISamplerMode mode) const;
	nri::DescriptorSet* CreateTextureSet(nri::Descriptor* shaderView);
	bool RenderPathTracingSanityFrame();
	void PrintFrameBoundaryStatus() const;
	void PrintFrameSequenceStatus() const;
	void PrintSwapChainStatus() const;
	void PrintFrameShellStatus() const;
	void Print2DTextureStatus() const;
	const char* DescribeTextureTarget(const NRITextureResource* target) const;
	void RecordFrameSequence(uint32_t releaseSemaphoreIndex, uint64_t submittedFenceValue, nri::Result presentResult);
	void Reset2DTextureFrameStats();
	void Note2DTextureEnsure(bool canvas);
	void Note2DTextureCacheHit();
	void Note2DTextureCacheMiss();
	void Note2DTextureUploadAttempt(uint64_t bytes, bool success);
	void Note2DTextureResourceCreate(bool recreated);
	void NoteSwapChainAcquire(uint32_t imageIndex);
	void NoteSwapChainPresent(uint32_t imageIndex);
	void NoteSwapChainAbandon(uint32_t imageIndex);
	uint32_t GetQueuedFrameIndex(uint64_t frameIndex) const;
	void SelectQueuedFrame(uint32_t queuedFrameIndex);
	void ResetFrameTracking(bool presentedAcquiredImage = false);
	void RefreshNativeFrameGenerationHandles();
	void RefreshNativeFrameGenerationSwapChain();
	bool RefreshFrameGenerationPresentTargets();
	void DestroyFrameGenerationPresentTargets();
	bool ShouldRequestFrameGenerationLowLatencySwapChain() const;
	nri::SwapChainBits GetEffectiveRequestedSwapChainFlags() const;
	bool ShouldUseFrameGenerationUiTarget() const;
	uint32_t GetFrameGenerationSceneBlendPrefixCount() const;
	bool EnsureFrameGenerationUiTexture(uint32_t width, uint32_t height);
	NRITextureResource* GetFrameGenerationUiTargetResource() const;
	void ClearTargetColor(NRITextureResource& target, float red, float green, float blue, float alpha);
	void BeginFrameGenerationUiTarget();
	void DrawFrameGenerationSceneBlendPrefix();
	void FinalizeFrameGenerationUiTarget();
	void CompositeFrameGenerationUiTexture();
	void DestroyFrameGenerationUiTexture();

	friend class NRIHardwareTexture;
	friend class NRIRenderState;
	friend class NRIRenderer;
	friend class NRIUpscalerContext;
	friend class NRIFrameGenerationContext;

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
	nri::UpscalerInterface mUpscaler = {};
	nri::LowLatencyInterface mLowLatency = {};
	nri::WrapperD3D12Interface mWrapperD3D12 = {};
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
	NRIFrameGenerationContext mFrameGeneration;

	std::vector<NRISwapChainImage> mSwapChainImages;
	std::vector<NRITextureResource> mFrameGenerationPresentImages;
	std::vector<QueuedFrame> mQueuedFrames;
	NRITextureResource mSceneTarget;
	NRITextureResource mSaveTarget;
	NRITextureResource* mActiveTarget = nullptr;
	NRITextureResource* mCurrentPresentTarget = nullptr;
	FGameTexture* mFrameGenerationUiTexture = nullptr;
	nri::DescriptorSet* mWhiteTextureSet = nullptr;
	NRIHardwareTexture* mWhiteTexture = nullptr;

	std::vector<uint8_t> mVertexShaderBlob;
	std::vector<uint8_t> mPixelShaderBlob;
	FString mDeviceName = "NRI";
	nri::SwapChainBits mSwapChainFlags = nri::SwapChainBits::NONE;
	uint8_t mSwapChainQueuedFrameNum = 0;
	uint8_t mSwapChainTextureCount = 0;
	uint64_t mObservedSwapChainAcquireMask = 0;
	uint64_t mObservedSwapChainPresentMask = 0;
	std::vector<uint64_t> mSwapChainAcquireCounts;
	std::vector<uint64_t> mSwapChainPresentCounts;
	std::vector<uint64_t> mSwapChainAbandonCounts;
#ifdef _WIN32
	ID3D12Device* mNativeD3D12Device = nullptr;
	ID3D12CommandQueue* mNativeD3D12GraphicsQueue = nullptr;
	IDXGISwapChain4* mNativeD3D12SwapChain = nullptr;
	bool mFrameGenerationPresentAllowsTearing = false;
#endif
	uint64_t mFrameIndex = 0;
	uint64_t mSubmittedFenceValue = 0;
	bool mFrameBegun = false;
	bool mUsingSaveTarget = false;
	bool mPathTracingLevelPreloadPending = false;
	bool mHasAcquiredSwapChainImage = false;
	bool mHasPresentedSwapChainFrame = false;
	bool mFrameGenerationUiTargetActive = false;
	uint32_t mCurrentSwapChainImage = 0;
	uint32_t mCurrentQueuedFrameIndex = 0;
	uint32_t mAcquireSemaphoreIndex = 0;
	bool mCommandBufferOpen = false;
	bool mInitialized = false;
	bool mLoggedStartup = false;
	bool mLoggedD3D12FailureDred = false;
	FrameBoundaryDebugStats mLastFrameBoundaryStats;
	FrameSequenceEntry mFrameSequenceHistory[FrameSequenceHistorySize] = {};
	uint32_t mFrameSequenceWriteIndex = 0;
	Texture2DDebugStats mTexture2DDebugStats;
	bool mTraceThisFrame = false;
};
