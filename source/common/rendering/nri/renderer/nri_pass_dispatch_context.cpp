#include "nri_pass_dispatch.h"

#include "nri_descriptor_sets.h"
#include "nri_scene_upload.h"
#include "../system/nri_renderdevice.h"

NRIPassDispatchContext::NRIPassDispatchContext(NRIRenderer& renderer)
	: mFrameBuffer(renderer.mFrameBuffer),
	mCore(renderer.mFrameBuffer != nullptr ? &renderer.mFrameBuffer->mCore : nullptr),
	mDevice(renderer.mFrameBuffer != nullptr ? renderer.mFrameBuffer->mDevice : nullptr),
	mCommandBuffer(renderer.mFrameBuffer != nullptr ? renderer.mFrameBuffer->mCommandBuffer : nullptr),
	mPipelineLayout(renderer.mPipelineLayout),
	mTaaPipelineLayout(renderer.mTaaPipelineLayout),
	mPresentPipelineLayout(renderer.mPresentPipelineLayout),
	mSamplerSet(renderer.mSamplerSet),
	mFrameTextureSet(renderer.mFrameTextureSet),
	mOutputSet(renderer.mOutputSet),
	mCompositionFrameTextureSet(renderer.mCompositionFrameTextureSet),
	mCompositionOutputSet(renderer.mCompositionOutputSet),
	mUpscalerPrepassFrameTextureSet(renderer.mUpscalerPrepassFrameTextureSet),
	mUpscalerPrepassOutputSet(renderer.mUpscalerPrepassOutputSet),
	mTaaFrameTextureSet(renderer.mTaaFrameTextureSet),
	mTaaOutputSet(renderer.mTaaOutputSet),
	mRawPresentFrameTextureSet(renderer.mRawPresentFrameTextureSet),
	mRawPresentOutputSet(renderer.mRawPresentOutputSet),
	mFinalPresentFrameTextureSet(renderer.mFinalPresentFrameTextureSet),
	mFinalPresentOutputSet(renderer.mFinalPresentOutputSet),
	mFrameInputDescriptors(renderer.mFrameInputDescriptors),
	mOutputDescriptors(renderer.mOutputDescriptors),
	mExposure(renderer.mExposure),
	mTraceShaderStats(renderer.mTraceShaderStats),
	mNrd(renderer.mNrd),
	mUpscaler(renderer.mUpscaler),
	mPersistentVoxels(renderer.mPersistentVoxels),
	mBoundSceneInstances(renderer.mBoundSceneInstances),
	mSceneInstanceBuffer(renderer.mSceneInstanceBuffer),
	mDirectionalLightState(renderer.mDirectionalLightState),
	mNightVisionState(renderer.mNightVisionState),
	mLastPerfShellTraceStats(renderer.mLastPerfShellTraceStats),
	mLastPerfTraceShaderStats(renderer.mLastPerfTraceShaderStats),
	mLastAutoExposureSettings(renderer.mLastAutoExposureSettings),
	mFrameIndex(renderer.mFrameIndex),
	mRenderWidth(renderer.mRenderWidth),
	mRenderHeight(renderer.mRenderHeight),
	mOutputWidth(renderer.mOutputWidth),
	mOutputHeight(renderer.mOutputHeight),
	mTargetWidth(renderer.mTargetWidth),
	mTargetHeight(renderer.mTargetHeight),
	mSceneLeft(renderer.mSceneLeft),
	mSceneTop(renderer.mSceneTop),
	mCurrentCameraPos(renderer.mCurrentCameraPos),
	mCurrentCameraForward(renderer.mCurrentCameraForward),
	mCurrentCameraRight(renderer.mCurrentCameraRight),
	mCurrentCameraUp(renderer.mCurrentCameraUp),
	mPreviousCameraPos(renderer.mPreviousCameraPos),
	mPreviousCameraForward(renderer.mPreviousCameraForward),
	mPreviousCameraRight(renderer.mPreviousCameraRight),
	mPreviousCameraUp(renderer.mPreviousCameraUp),
	mCurrentTanHalfFovX(renderer.mCurrentTanHalfFovX),
	mCurrentTanHalfFovY(renderer.mCurrentTanHalfFovY),
	mPreviousTanHalfFovX(renderer.mPreviousTanHalfFovX),
	mPreviousTanHalfFovY(renderer.mPreviousTanHalfFovY),
	mCurrentJitter(renderer.mCurrentJitter),
	mPreviousJitter(renderer.mPreviousJitter),
	mCurrentViewToClip(renderer.mCurrentViewToClip),
	mPreviousViewToClip(renderer.mPreviousViewToClip),
	mCurrentWorldToView(renderer.mCurrentWorldToView),
	mPreviousWorldToView(renderer.mPreviousWorldToView),
	mSkyColor(renderer.mSkyColor),
	mGroundColor(renderer.mGroundColor),
	mGuiCaptureActive(renderer.mGuiCaptureActive),
	mResetHistory(renderer.mResetHistory),
	mHasAutoExposureSettingsState(renderer.mHasAutoExposureSettingsState),
	mUseUpscaledInFinal(renderer.mUseUpscaledInFinal),
	mUseDenoisedCompositionInputs(renderer.mUseDenoisedCompositionInputs),
	mUseSplitShadowDenoiser(renderer.mUseSplitShadowDenoiser),
	mBoundStaticPrimitiveCount(renderer.mBoundStaticPrimitiveCount),
	mBoundDynamicPrimitiveCount(renderer.mBoundDynamicPrimitiveCount),
	mBoundStaticMaterialCount(renderer.mBoundStaticMaterialCount),
	mBoundDynamicMaterialCount(renderer.mBoundDynamicMaterialCount),
	mBoundPortalCount(renderer.mBoundPortalCount),
	mBoundRuntimeLightCount(renderer.mBoundRuntimeLightCount),
	mBoundRuntimeLightTileCountX(renderer.mBoundRuntimeLightTileCountX),
	mBoundRuntimeLightTileCountY(renderer.mBoundRuntimeLightTileCountY),
	mHistoryInputSlot(renderer.mHistoryInputSlot),
	mHistoryOutputSlot(renderer.mHistoryOutputSlot),
	mUpscaledInputSlot(renderer.mUpscaledInputSlot),
	mRenderer(renderer)
{
}

NRITextureResource& NRIPassDispatchContext::GetFrameTexture(FrameTextureSlot slot) const
{
	return mRenderer.GetFrameTexture(slot);
}

nri::Pipeline* NRIPassDispatchContext::GetPipeline(PipelineSlot slot) const
{
	return mRenderer.GetPipeline(slot);
}

nri::DescriptorSet* NRIPassDispatchContext::GetCurrentSceneTextureSet() const
{
	return mRenderer.GetCurrentSceneTextureSet();
}

nri::DescriptorSet* NRIPassDispatchContext::GetCurrentSceneDataSet() const
{
	return mRenderer.GetCurrentSceneDataSet();
}

NRIResourceServices NRIPassDispatchContext::BuildResourceServices() const
{
	return mRenderer.BuildResourceServices();
}

bool NRIPassDispatchContext::UpdateReprojectionBuffer() const
{
	return NRISceneUploadManager::UpdateReprojectionBuffer(mRenderer, nullptr);
}

void NRIPassDispatchContext::ReadbackAutoExposureStats() const
{
	mRenderer.ReadbackAutoExposureStats();
}

bool NRIPassDispatchContext::EnsureAutoExposureResources(const NRIAutoExposureSettings& settings) const
{
	return mRenderer.EnsureAutoExposureResources(settings);
}

void NRIPassDispatchContext::RequestAutoExposureReset(const char* reason) const
{
	mRenderer.RequestAutoExposureReset(reason);
}

bool NRIPassDispatchContext::DispatchAutoExposure(FrameTextureSlot sourceSlot) const
{
	return mRenderer.DispatchAutoExposure(sourceSlot);
}

void NRIPassDispatchContext::ResetSelfTestRouteSnapshot() const
{
	mRenderer.ResetSelfTestRouteSnapshot();
}

void NRIPassDispatchContext::SetSelfTestRouteSnapshot(const char* routeName, const char* presenterName, const char* ownerName, const char* passListName, bool denoiserRun, bool upscalerRun, bool exposureRun) const
{
	mRenderer.SetSelfTestRouteSnapshot(routeName, presenterName, ownerName, passListName, denoiserRun, upscalerRun, exposureRun);
}

void NRIPassDispatchContext::CopyFinalToActiveTarget() const
{
	mRenderer.CopyFinalToActiveTarget();
}

void NRIPassDispatchContext::CopyTexture(NRITextureResource& source, NRITextureResource& destination) const
{
	mRenderer.CopyTexture(source, destination);
}

void NRIPassDispatchContext::TransitionTexture(NRITextureResource& texture, nri::AccessLayoutStage after) const
{
	mFrameBuffer->TransitionTexture(texture, after);
}

void NRIPassDispatchContext::BindSceneRootDescriptors() const
{
	mRenderer.BindSceneRootDescriptors();
}

bool NRIPassDispatchContext::UpdateFrameTextureSet() const
{
	return NRIDescriptorSetManager::UpdateFrameTextureSet(mRenderer);
}

bool NRIPassDispatchContext::UpdateFrameTextureSet(nri::DescriptorSet* descriptorSet, const std::array<nri::Descriptor*, 14>& descriptors) const
{
	return NRIDescriptorSetManager::UpdateFrameTextureSet(mRenderer, descriptorSet, descriptors);
}

bool NRIPassDispatchContext::UpdateOutputSet() const
{
	return NRIDescriptorSetManager::UpdateOutputSet(mRenderer);
}

bool NRIPassDispatchContext::UpdateOutputSet(nri::DescriptorSet* descriptorSet, const std::array<nri::Descriptor*, 15>& descriptors) const
{
	return NRIDescriptorSetManager::UpdateOutputSet(mRenderer, descriptorSet, descriptors);
}

NRIMainUpscalerKind NRIPassDispatchContext::ResolveMainUpscalerKind(bool logFallback) const
{
	return mRenderer.ResolveMainUpscalerKind(logFallback);
}

NRIPostSharpenKind NRIPassDispatchContext::ResolvePostSharpenKind(bool logFallback) const
{
	return mRenderer.ResolvePostSharpenKind(logFallback);
}

nri::UpscalerMode NRIPassDispatchContext::GetSelectedUpscalerMode() const
{
	return mRenderer.GetSelectedUpscalerMode();
}

bool NRIPassDispatchContext::ShouldRunAppTaaForFrameGraph(NRIMainUpscalerKind kind) const
{
	return mRenderer.ShouldRunAppTaaForFrameGraph(kind);
}

NRIPassDispatchContext::ExposureRoute NRIPassDispatchContext::ResolveExposureRoute(FrameTextureSlot inputSlot, const NRIPTOutputPolicy& outputPolicy, NRIMainUpscalerKind mainKind, NRIPostSharpenKind postSharpenKind) const
{
	return mRenderer.ResolveExposureRoute(inputSlot, outputPolicy, mainKind, postSharpenKind);
}

void NRIPassDispatchContext::TraceTemporalState(const char* stage, NRIMainUpscalerKind resolvedMainUpscaler, NRIPostSharpenKind resolvedPostSharpen, bool runAppTaa, FrameTextureSlot primarySlot, FrameTextureSlot secondarySlot) const
{
	mRenderer.TraceTemporalState(stage, resolvedMainUpscaler, resolvedPostSharpen, runAppTaa, primarySlot, secondarySlot);
}

uint32_t NRIPassDispatchContext::EstimatePersistentVoxelPrimitiveCountForInstanceOffset(uint32_t primitiveOffset) const
{
	return mPersistentVoxels.EstimatePrimitiveCountForInstanceOffset(primitiveOffset);
}
