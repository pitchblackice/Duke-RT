#include "nri_pass_dispatch.h"

#include "nri_descriptor_sets.h"
#include "nri_scene_upload.h"
#include "nri_smoke.h"
#include "../system/nri_renderdevice.h"

#include <algorithm>

NRIPassDispatchContext NRIRenderer::BuildPassDispatchContext(bool mainViewEligible)
{
	NRIPassDispatchContext::Init init = {};
	NRIRenderDevice* const frameBuffer = mFrameBuffer;

	auto buildTextureService = [&]()
	{
		NRIPassDispatchContext::TextureService service = {};
		service.user = this;
		service.getFrameTexture = [](void* user, FrameTextureSlot slot) -> NRITextureResource&
		{
			return static_cast<NRIRenderer*>(user)->GetFrameTexture(slot);
		};
		return service;
	};

	auto buildPipelineService = [&]()
	{
		NRIPassDispatchContext::PipelineService service = {};
		service.user = this;
		service.getPipeline = [](void* user, PipelineSlot slot) -> nri::Pipeline*
		{
			return static_cast<NRIRenderer*>(user)->GetPipeline(slot);
		};
		return service;
	};

	auto buildDescriptorService = [&]()
	{
		NRIPassDispatchContext::DescriptorService service = {};
		service.user = this;
		service.updateFrameTextureSet = [](void* user) -> bool
		{
			return NRIDescriptorSetManager::UpdateFrameTextureSet(*static_cast<NRIRenderer*>(user));
		};
		service.updateFrameTextureSetWithDescriptors = [](void* user, nri::DescriptorSet* descriptorSet, const std::array<nri::Descriptor*, 14>& descriptors) -> bool
		{
			return NRIDescriptorSetManager::UpdateFrameTextureSet(*static_cast<NRIRenderer*>(user), descriptorSet, descriptors);
		};
		service.updateOutputSet = [](void* user) -> bool
		{
			return NRIDescriptorSetManager::UpdateOutputSet(*static_cast<NRIRenderer*>(user));
		};
		service.updateOutputSetWithDescriptors = [](void* user, nri::DescriptorSet* descriptorSet, const std::array<nri::Descriptor*, 15>& descriptors) -> bool
		{
			return NRIDescriptorSetManager::UpdateOutputSet(*static_cast<NRIRenderer*>(user), descriptorSet, descriptors);
		};
		return service;
	};

	auto buildResourceService = [&]()
	{
		NRIPassDispatchContext::ResourceService service = {};
		service.user = this;
		service.frameBuffer = frameBuffer;
		service.core = frameBuffer != nullptr ? frameBuffer->GetCoreInterface() : nullptr;
		service.device = frameBuffer != nullptr ? frameBuffer->GetDevice() : nullptr;
		service.commandBuffer = frameBuffer != nullptr ? frameBuffer->GetCurrentCommandBuffer() : nullptr;
		service.buildResourceServices = [](void* user) -> NRIResourceServices
		{
			return static_cast<NRIRenderer*>(user)->BuildResourceServices();
		};
		service.updateReprojectionBuffer = [](void* user) -> bool
		{
			return NRISceneUploadManager::UpdateReprojectionBuffer(*static_cast<NRIRenderer*>(user), nullptr);
		};
		service.getOutputPolicy = [](void* user) -> NRIPTOutputPolicy
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mFrameBuffer->GetPathTracingOutputPolicy();
		};
		service.copyFinalToActiveTarget = [](void* user)
		{
			static_cast<NRIRenderer*>(user)->CopyFinalToActiveTarget();
		};
		service.copyTexture = [](void* user, NRITextureResource& source, NRITextureResource& destination)
		{
			static_cast<NRIRenderer*>(user)->CopyTexture(source, destination);
		};
		service.transitionTexture = [](void* user, NRITextureResource& texture, nri::AccessLayoutStage after)
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			renderer->mFrameBuffer->TransitionTexture(texture, after);
		};
		return service;
	};

	auto buildCommandService = [&]()
	{
		NRIPassDispatchContext::CommandService service = {};
		service.core = frameBuffer != nullptr ? frameBuffer->GetCoreInterface() : nullptr;
		service.commandBuffer = frameBuffer != nullptr ? frameBuffer->GetCurrentCommandBuffer() : nullptr;
		return service;
	};

	auto buildSceneBindingService = [&]()
	{
		NRIPassDispatchContext::SceneBindingService service = {};
		service.user = this;
		service.getCurrentSceneTextureSet = [](void* user) -> nri::DescriptorSet*
		{
			return static_cast<NRIRenderer*>(user)->GetCurrentSceneTextureSet();
		};
		service.getCurrentSceneDataSet = [](void* user) -> nri::DescriptorSet*
		{
			return static_cast<NRIRenderer*>(user)->GetCurrentSceneDataSet();
		};
		service.bindSceneRootDescriptors = [](void* user)
		{
			static_cast<NRIRenderer*>(user)->BindSceneRootDescriptors();
		};
		return service;
	};

	auto buildExposureService = [&]()
	{
		NRIPassDispatchContext::ExposureService service = {};
		service.user = this;
		service.readbackAutoExposureStats = [](void* user)
		{
			static_cast<NRIRenderer*>(user)->ReadbackAutoExposureStats();
		};
		service.ensureAutoExposureResources = [](void* user, const NRIAutoExposureSettings& settings) -> bool
		{
			return static_cast<NRIRenderer*>(user)->EnsureAutoExposureResources(settings);
		};
		service.requestAutoExposureReset = [](void* user, const char* reason)
		{
			static_cast<NRIRenderer*>(user)->RequestAutoExposureReset(reason);
		};
		service.dispatchAutoExposure = [](void* user, FrameTextureSlot sourceSlot) -> bool
		{
			return static_cast<NRIRenderer*>(user)->DispatchAutoExposure(sourceSlot);
		};
		service.resolveExposureRoute = [](void* user, FrameTextureSlot inputSlot, const NRIPTOutputPolicy& outputPolicy, NRIMainUpscalerKind mainKind, NRIPostSharpenKind postSharpenKind) -> ExposureRoute
		{
			return static_cast<NRIRenderer*>(user)->ResolveExposureRoute(inputSlot, outputPolicy, mainKind, postSharpenKind);
		};
		return service;
	};

	auto buildUpscalerService = [&]()
	{
		NRIPassDispatchContext::UpscalerService service = {};
		service.user = this;
		service.resolveMainUpscalerKind = [](void* user, bool logFallback) -> NRIMainUpscalerKind
		{
			return static_cast<NRIRenderer*>(user)->ResolveMainUpscalerKind(logFallback);
		};
		service.resolvePostSharpenKind = [](void* user, bool logFallback) -> NRIPostSharpenKind
		{
			return static_cast<NRIRenderer*>(user)->ResolvePostSharpenKind(logFallback);
		};
		service.getSelectedUpscalerMode = [](void* user) -> nri::UpscalerMode
		{
			return static_cast<NRIRenderer*>(user)->GetSelectedUpscalerMode();
		};
		service.shouldRunAppTaaForFrameGraph = [](void* user, NRIMainUpscalerKind kind) -> bool
		{
			return static_cast<NRIRenderer*>(user)->ShouldRunAppTaaForFrameGraph(kind);
		};
		service.traceTemporalState = [](void* user, const char* stage, NRIMainUpscalerKind resolvedMainUpscaler, NRIPostSharpenKind resolvedPostSharpen, bool runAppTaa, FrameTextureSlot primarySlot, FrameTextureSlot secondarySlot)
		{
			static_cast<NRIRenderer*>(user)->TraceTemporalState(stage, resolvedMainUpscaler, resolvedPostSharpen, runAppTaa, primarySlot, secondarySlot);
		};
		service.ensureMainUpscaler = [](void* user, NRIMainUpscalerKind kind, nri::UpscalerMode mode, uint32_t outputWidth, uint32_t outputHeight, bool exposure) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mUpscaler.EnsureMainUpscaler(*renderer->mFrameBuffer, kind, mode, outputWidth, outputHeight, exposure);
		};
		service.dispatchMainUpscaler = [](void* user, NRIMainUpscalerKind kind, const NRIUpscalerDispatchDesc& desc) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mUpscaler.DispatchMainUpscaler(*renderer->mFrameBuffer, kind, desc);
		};
		service.ensurePostSharpen = [](void* user, NRIPostSharpenKind kind, uint32_t outputWidth, uint32_t outputHeight) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mUpscaler.EnsurePostSharpen(*renderer->mFrameBuffer, kind, outputWidth, outputHeight);
		};
		service.dispatchPostSharpen = [](void* user, NRIPostSharpenKind kind, const NRIUpscalerDispatchDesc& desc) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mUpscaler.DispatchPostSharpen(*renderer->mFrameBuffer, kind, desc);
		};
		return service;
	};

	auto buildSelfTestService = [&]()
	{
		NRIPassDispatchContext::SelfTestService service = {};
		service.user = this;
		service.resetSelfTestRouteSnapshot = [](void* user)
		{
			static_cast<NRIRenderer*>(user)->ResetSelfTestRouteSnapshot();
		};
		service.setSelfTestRouteSnapshot = [](void* user, const char* routeName, const char* presenterName, const char* ownerName, const char* passListName, bool denoiserRun, bool upscalerRun, bool exposureRun)
		{
			static_cast<NRIRenderer*>(user)->SetSelfTestRouteSnapshot(routeName, presenterName, ownerName, passListName, denoiserRun, upscalerRun, exposureRun);
		};
		return service;
	};

	auto buildSmokeService = [&]()
	{
		NRIPassDispatchContext::SmokeService service = {};
		service.user = this;
		service.prepareFrame = [](void* user, bool eligible) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mSmoke == nullptr || renderer->mSmoke->PrepareFrame(*renderer, eligible);
		};
		service.dispatchRoute = [](void* user, const NRISmokeRouteDesc& route) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mSmoke == nullptr || renderer->mSmoke->DispatchRoute(*renderer, route);
		};
		return service;
	};

	init.textures = buildTextureService();
	init.pipelines = buildPipelineService();
	init.descriptors = buildDescriptorService();
	init.resources = buildResourceService();
	init.commands = buildCommandService();
	init.sceneBinding = buildSceneBindingService();
	init.exposureService = buildExposureService();
	init.upscalerService = buildUpscalerService();
	init.selfTest = buildSelfTestService();
	init.smokeService = buildSmokeService();
	init.pipelineLayout = &mPipelineLayout;
	init.taaPipelineLayout = &mTaaPipelineLayout;
	init.presentPipelineLayout = &mPresentPipelineLayout;
	init.bloomPipelineLayout = &mBloomPipelineLayout;
	init.samplerSet = &mSamplerSet;
	init.frameTextureSet = &mFrameTextureSet;
	init.outputSet = &mOutputSet;
	init.compositionFrameTextureSet = &mCompositionFrameTextureSet;
	init.compositionOutputSet = &mCompositionOutputSet;
	init.upscalerPrepassFrameTextureSet = &mUpscalerPrepassFrameTextureSet;
	init.upscalerPrepassOutputSet = &mUpscalerPrepassOutputSet;
	init.taaFrameTextureSet = &mTaaFrameTextureSet;
	init.taaOutputSet = &mTaaOutputSet;
	init.rawPresentFrameTextureSet = &mRawPresentFrameTextureSet;
	init.rawPresentOutputSet = &mRawPresentOutputSet;
	init.finalPresentFrameTextureSet = &mFinalPresentFrameTextureSet;
	init.finalPresentOutputSet = &mFinalPresentOutputSet;
	init.bloomInputSets = &mBloomInputSets;
	init.bloomOutputSets = &mBloomOutputSets;
	init.frameInputDescriptors = &mFrameInputDescriptors;
	init.outputDescriptors = &mOutputDescriptors;
	init.exposure = &mExposure;
	init.traceShaderStats = &mTraceShaderStats;
	init.nrd = &mNrd;
	init.upscaler = &mUpscaler;
	init.persistentVoxels = &mPersistentVoxels;
	init.boundSceneInstances = &mBoundSceneInstances;
	init.directionalLightState = &mDirectionalLightState;
	init.nightVisionState = &mNightVisionState;
	init.lastPerfShellTraceStats = &mLastPerfShellTraceStats;
	init.lastPerfTraceShaderStats = &mLastPerfTraceShaderStats;
	init.lastAutoExposureSettings = &mLastAutoExposureSettings;
	init.frame.frameIndex = mFrameIndex;
	init.frame.renderWidth = mRenderWidth;
	init.frame.renderHeight = mRenderHeight;
	init.frame.outputWidth = mOutputWidth;
	init.frame.outputHeight = mOutputHeight;
	init.frame.targetWidth = mTargetWidth;
	init.frame.targetHeight = mTargetHeight;
	init.frame.sceneLeft = mSceneLeft;
	init.frame.sceneTop = mSceneTop;
	std::copy(mCurrentCameraPos, mCurrentCameraPos + 3, init.frame.currentCameraPos.begin());
	std::copy(mCurrentCameraForward, mCurrentCameraForward + 3, init.frame.currentCameraForward.begin());
	std::copy(mCurrentCameraRight, mCurrentCameraRight + 3, init.frame.currentCameraRight.begin());
	std::copy(mCurrentCameraUp, mCurrentCameraUp + 3, init.frame.currentCameraUp.begin());
	std::copy(mPreviousCameraPos, mPreviousCameraPos + 3, init.frame.previousCameraPos.begin());
	std::copy(mPreviousCameraForward, mPreviousCameraForward + 3, init.frame.previousCameraForward.begin());
	std::copy(mPreviousCameraRight, mPreviousCameraRight + 3, init.frame.previousCameraRight.begin());
	std::copy(mPreviousCameraUp, mPreviousCameraUp + 3, init.frame.previousCameraUp.begin());
	init.frame.currentTanHalfFovX = mCurrentTanHalfFovX;
	init.frame.currentTanHalfFovY = mCurrentTanHalfFovY;
	init.frame.previousTanHalfFovX = mPreviousTanHalfFovX;
	init.frame.previousTanHalfFovY = mPreviousTanHalfFovY;
	std::copy(mCurrentJitter, mCurrentJitter + 2, init.frame.currentJitter.begin());
	std::copy(mPreviousJitter, mPreviousJitter + 2, init.frame.previousJitter.begin());
	std::copy(mCurrentViewToClip, mCurrentViewToClip + 16, init.frame.currentViewToClip.begin());
	std::copy(mPreviousViewToClip, mPreviousViewToClip + 16, init.frame.previousViewToClip.begin());
	std::copy(mCurrentWorldToView, mCurrentWorldToView + 16, init.frame.currentWorldToView.begin());
	std::copy(mPreviousWorldToView, mPreviousWorldToView + 16, init.frame.previousWorldToView.begin());
	std::copy(mSkyColor, mSkyColor + 3, init.frame.skyColor.begin());
	std::copy(mGroundColor, mGroundColor + 3, init.frame.groundColor.begin());
	init.frame.guiCaptureActive = mGuiCaptureActive;
	init.frame.resetHistory = mResetHistory;
	init.frame.mainViewEligible = mainViewEligible;
	init.sceneStats.sceneInstanceCount = (uint32_t)mBoundSceneInstances.size();
	init.sceneStats.staticPrimitiveCount = mBoundStaticPrimitiveCount;
	init.sceneStats.dynamicPrimitiveCount = mBoundDynamicPrimitiveCount;
	init.sceneStats.staticMaterialCount = mBoundStaticMaterialCount;
	init.sceneStats.dynamicMaterialCount = mBoundDynamicMaterialCount;
	init.sceneStats.portalCount = mBoundPortalCount;
	init.sceneStats.runtimeLightCount = mBoundRuntimeLightCount;
	init.sceneStats.runtimeLightTileCountX = mBoundRuntimeLightTileCountX;
	init.sceneStats.runtimeLightTileCountY = mBoundRuntimeLightTileCountY;
	init.hasAutoExposureSettingsState = &mHasAutoExposureSettingsState;
	init.useUpscaledInFinal = &mUseUpscaledInFinal;
	init.useDenoisedCompositionInputs = &mUseDenoisedCompositionInputs;
	init.useSplitShadowDenoiser = &mUseSplitShadowDenoiser;
	init.historyInputSlot = &mHistoryInputSlot;
	init.historyOutputSlot = &mHistoryOutputSlot;
	init.upscaledInputSlot = &mUpscaledInputSlot;
	return NRIPassDispatchContext(init);
}
