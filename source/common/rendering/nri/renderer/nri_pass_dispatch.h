#pragma once

#include "nri_renderer.h"

#include <array>
#include <vector>

struct HWDrawInfo;

namespace nri_scene
{
	struct GeometryData;
	struct MaterialData;
}

class NRIPassDispatchContext
{
public:
	using FrameTextureSlot = NRIRenderer::FrameTextureSlot;
	using PipelineSlot = NRIRenderer::PipelineSlot;
	using ExposureRoute = NRIRenderer::ExposureRoute;
	using ExposureDomain = NRIRenderer::ExposureDomain;

	explicit NRIPassDispatchContext(NRIRenderer& renderer);

	NRITextureResource& GetFrameTexture(FrameTextureSlot slot) const;
	nri::Pipeline* GetPipeline(PipelineSlot slot) const;
	nri::DescriptorSet* GetCurrentSceneTextureSet() const;
	nri::DescriptorSet* GetCurrentSceneDataSet() const;
	NRIResourceServices BuildResourceServices() const;
	bool UpdateReprojectionBuffer() const;
	void ReadbackAutoExposureStats() const;
	bool EnsureAutoExposureResources(const NRIAutoExposureSettings& settings) const;
	void RequestAutoExposureReset(const char* reason) const;
	bool DispatchAutoExposure(FrameTextureSlot sourceSlot) const;
	void ResetSelfTestRouteSnapshot() const;
	void SetSelfTestRouteSnapshot(const char* routeName, const char* presenterName, const char* ownerName, const char* passListName, bool denoiserRun, bool upscalerRun, bool exposureRun) const;
	void CopyFinalToActiveTarget() const;
	void CopyTexture(NRITextureResource& source, NRITextureResource& destination) const;
	void TransitionTexture(NRITextureResource& texture, nri::AccessLayoutStage after) const;
	void BindSceneRootDescriptors() const;
	bool UpdateFrameTextureSet() const;
	bool UpdateFrameTextureSet(nri::DescriptorSet* descriptorSet, const std::array<nri::Descriptor*, 14>& descriptors) const;
	bool UpdateOutputSet() const;
	bool UpdateOutputSet(nri::DescriptorSet* descriptorSet, const std::array<nri::Descriptor*, 15>& descriptors) const;
	NRIMainUpscalerKind ResolveMainUpscalerKind(bool logFallback) const;
	NRIPostSharpenKind ResolvePostSharpenKind(bool logFallback) const;
	nri::UpscalerMode GetSelectedUpscalerMode() const;
	bool ShouldRunAppTaaForFrameGraph(NRIMainUpscalerKind kind) const;
	ExposureRoute ResolveExposureRoute(FrameTextureSlot inputSlot, const NRIPTOutputPolicy& outputPolicy, NRIMainUpscalerKind mainKind, NRIPostSharpenKind postSharpenKind) const;
	void TraceTemporalState(const char* stage, NRIMainUpscalerKind resolvedMainUpscaler, NRIPostSharpenKind resolvedPostSharpen, bool runAppTaa, FrameTextureSlot primarySlot, FrameTextureSlot secondarySlot) const;
	uint32_t EstimatePersistentVoxelPrimitiveCountForInstanceOffset(uint32_t primitiveOffset) const;

	NRIRenderDevice* mFrameBuffer = nullptr;
	nri::CoreInterface* mCore = nullptr;
	nri::Device* mDevice = nullptr;
	nri::CommandBuffer* mCommandBuffer = nullptr;
	nri::PipelineLayout*& mPipelineLayout;
	nri::PipelineLayout*& mTaaPipelineLayout;
	nri::PipelineLayout*& mPresentPipelineLayout;
	nri::DescriptorSet*& mSamplerSet;
	nri::DescriptorSet*& mFrameTextureSet;
	nri::DescriptorSet*& mOutputSet;
	nri::DescriptorSet*& mCompositionFrameTextureSet;
	nri::DescriptorSet*& mCompositionOutputSet;
	nri::DescriptorSet*& mUpscalerPrepassFrameTextureSet;
	nri::DescriptorSet*& mUpscalerPrepassOutputSet;
	nri::DescriptorSet*& mTaaFrameTextureSet;
	nri::DescriptorSet*& mTaaOutputSet;
	nri::DescriptorSet*& mRawPresentFrameTextureSet;
	nri::DescriptorSet*& mRawPresentOutputSet;
	nri::DescriptorSet*& mFinalPresentFrameTextureSet;
	nri::DescriptorSet*& mFinalPresentOutputSet;
	std::array<nri::Descriptor*, 14>& mFrameInputDescriptors;
	std::array<nri::Descriptor*, 15>& mOutputDescriptors;
	NRIExposureController& mExposure;
	NRITraceShaderStats& mTraceShaderStats;
	NRINrdContext& mNrd;
	NRIUpscalerContext& mUpscaler;
	NRIPersistentVoxelResidency& mPersistentVoxels;
	std::vector<SceneInstanceData>& mBoundSceneInstances;
	NRIBufferResource& mSceneInstanceBuffer;
	NRIDirectionalLightState& mDirectionalLightState;
	NRIPTNightVisionState& mNightVisionState;
	NRIRenderer::PerfShellTraceStats& mLastPerfShellTraceStats;
	NRIRenderer::PerfTraceShaderStats& mLastPerfTraceShaderStats;
	NRIAutoExposureSettings& mLastAutoExposureSettings;
	uint32_t& mFrameIndex;
	uint32_t& mRenderWidth;
	uint32_t& mRenderHeight;
	uint32_t& mOutputWidth;
	uint32_t& mOutputHeight;
	uint32_t& mTargetWidth;
	uint32_t& mTargetHeight;
	int32_t& mSceneLeft;
	int32_t& mSceneTop;
	float* mCurrentCameraPos = nullptr;
	float* mCurrentCameraForward = nullptr;
	float* mCurrentCameraRight = nullptr;
	float* mCurrentCameraUp = nullptr;
	float* mPreviousCameraPos = nullptr;
	float* mPreviousCameraForward = nullptr;
	float* mPreviousCameraRight = nullptr;
	float* mPreviousCameraUp = nullptr;
	float& mCurrentTanHalfFovX;
	float& mCurrentTanHalfFovY;
	float& mPreviousTanHalfFovX;
	float& mPreviousTanHalfFovY;
	float* mCurrentJitter = nullptr;
	float* mPreviousJitter = nullptr;
	float* mCurrentViewToClip = nullptr;
	float* mPreviousViewToClip = nullptr;
	float* mCurrentWorldToView = nullptr;
	float* mPreviousWorldToView = nullptr;
	float* mSkyColor = nullptr;
	float* mGroundColor = nullptr;
	bool& mGuiCaptureActive;
	bool& mResetHistory;
	bool& mHasAutoExposureSettingsState;
	bool& mUseUpscaledInFinal;
	bool& mUseDenoisedCompositionInputs;
	bool& mUseSplitShadowDenoiser;
	uint32_t& mBoundStaticPrimitiveCount;
	uint32_t& mBoundDynamicPrimitiveCount;
	uint32_t& mBoundStaticMaterialCount;
	uint32_t& mBoundDynamicMaterialCount;
	uint32_t& mBoundPortalCount;
	uint32_t& mBoundRuntimeLightCount;
	uint32_t& mBoundRuntimeLightTileCountX;
	uint32_t& mBoundRuntimeLightTileCountY;
	FrameTextureSlot& mHistoryInputSlot;
	FrameTextureSlot& mHistoryOutputSlot;
	FrameTextureSlot& mUpscaledInputSlot;

private:
	NRIRenderer& mRenderer;
};

class NRIPassDispatcher
{
public:
	static bool DispatchBootstrapView(NRIPassDispatchContext& context);
	static bool DispatchFrameGraph(
		NRIPassDispatchContext& context,
		HWDrawInfo& di,
		const nri_scene::GeometryData& geometry,
		const std::vector<nri_scene::MaterialData>& materials,
		int drawmode);
	static bool DispatchTraceOpaque(
		NRIPassDispatchContext& context,
		HWDrawInfo& di,
		const nri_scene::GeometryData& geometry,
		const std::vector<nri_scene::MaterialData>& materials);
	static bool DispatchDenoiser(NRIPassDispatchContext& context);
	static bool DispatchComposition(NRIPassDispatchContext& context, NRIRenderer::FrameTextureSlot outputSlot = NRIRenderer::FrameTextureSlot::Composed);
	static bool DispatchTraceTransparent(NRIPassDispatchContext& context);
	static bool DispatchUpscalerPrepass(NRIPassDispatchContext& context, NRIMainUpscalerKind mainKind);
	static bool DispatchRawPresent(
		NRIPassDispatchContext& context,
		NRIRenderer::FrameTextureSlot inputSlot,
		NRIRenderer::FrameTextureSlot secondarySlot = NRIRenderer::FrameTextureSlot::Count,
		NRIRenderer::FrameTextureSlot tertiarySlot = NRIRenderer::FrameTextureSlot::Count);
	static bool DispatchFinalPresent(NRIPassDispatchContext& context, NRIRenderer::FrameTextureSlot inputSlot);
	static bool DispatchUpscaleChain(NRIPassDispatchContext& context);
	static bool DispatchFinal(NRIPassDispatchContext& context);
};
