#include "nri_pass_dispatch.h"

#include "nri_descriptor_sets.h"
#include "nri_exposure.h"
#include "nri_frame_graph.h"
#include "nri_renderer_settings.h"
#include "nri_scene_upload.h"
#include "nri_shader_contracts.h"
#include "../system/nri_renderdevice.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "printf.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

EXTERN_CVAR(Int, perf_looptraceframes)
EXTERN_CVAR(Int, nri_pttraceframes)
EXTERN_CVAR(Bool, nri_ptslowdowntrace)
EXTERN_CVAR(Bool, nri_denoise)
EXTERN_CVAR(Int, nri_nrddenoiser)
EXTERN_CVAR(Int, nri_ptdebug)
EXTERN_CVAR(Bool, nri_ptbootstrap)
EXTERN_CVAR(Int, nri_ptbootstrapmode)
EXTERN_CVAR(Bool, nri_ptdirectscene)
EXTERN_CVAR(Bool, nri_ptemissivefastshadow)
EXTERN_CVAR(Bool, nri_ptvisiblechunkgate)
EXTERN_CVAR(Bool, nri_ptshaderstats)
EXTERN_CVAR(Float, nri_sharpness)
EXTERN_CVAR(Float, nri_ptbaseambient)
EXTERN_CVAR(Float, nri_ptmetalambient)
EXTERN_CVAR(Bool, nri_ptnightvision)
EXTERN_CVAR(Float, nri_ptnightvisionexposure)
EXTERN_CVAR(Float, nri_ptnightvisioncontrast)
EXTERN_CVAR(Float, nri_ptnightvisionsaturation)
EXTERN_CVAR(Float, nri_ptnightvisionred)
EXTERN_CVAR(Float, nri_ptnightvisiongreen)
EXTERN_CVAR(Float, nri_ptnightvisionblue)

namespace
{
	static double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	}

	static bool ShouldTracePtPerf()
	{
		return (int)perf_looptraceframes > 0 || (int)nri_pttraceframes > 0;
	}

	static bool ShouldCollectPtPerfTiming()
	{
		return ShouldTracePtPerf() || (bool)nri_ptslowdowntrace;
	}

	static bool ShouldCollectTraceShaderStats()
	{
		return !!nri_ptshaderstats && ShouldTracePtPerf();
	}

	class ScopedPtPerfTimer
	{
	public:
		explicit ScopedPtPerfTimer(double& targetMs)
			: mTarget(ShouldCollectPtPerfTiming() ? &targetMs : nullptr)
		{
			if (mTarget != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedPtPerfTimer()
		{
			if (mTarget != nullptr)
			{
				*mTarget += DurationMs(mStart, std::chrono::steady_clock::now());
			}
		}

	private:
		double* mTarget = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};

	static uint32_t GetEffectivePtDebugMode()
	{
		return (uint32_t)std::max(0, (int)nri_ptdebug);
	}

	static uint32_t GetBootstrapMode()
	{
		return (uint32_t)std::max(0, std::min((int)nri_ptbootstrapmode, 13));
	}

	static NRIPresentRouteInfo ResolvePresentRouteInfo(uint32_t debugMode, bool bootstrap)
	{
		NRIFrameRouteRequest request = {};
		request.debugMode = debugMode;
		request.bootstrap = bootstrap;
		request.bootstrapMode = bootstrap ? GetBootstrapMode() : 0u;
		return ResolveNRIFrameRoute(request);
	}

	static NRINrdDenoiserMode GetSelectedNrdDenoiserMode()
	{
		return (NRINrdDenoiserMode)std::clamp((int)nri_nrddenoiser, 0, 1);
	}

	static uint32_t GetDispatchSize(uint32_t value)
	{
		return (value + 7u) / 8u;
	}

	static float Clamp01(float value)
	{
		return std::max(0.0f, std::min(value, 1.0f));
	}

	static float GetBaseAmbient()
	{
		return std::max(0.0f, (float)nri_ptbaseambient);
	}

	static float GetMetalAmbient()
	{
		return std::max(0.0f, (float)nri_ptmetalambient);
	}

	static uint32_t PackAmbientMultiplier12(float value)
	{
		return (uint32_t)std::min(4095.0f, std::max(0.0f, value) * 1024.0f + 0.5f);
	}

	static uint32_t PackPortalDepthAndAmbientMultipliers(uint32_t portalDepth, float baseAmbient, float metalAmbient)
	{
		return
			(portalDepth & 0xffu) |
			(PackAmbientMultiplier12(baseAmbient) << 8u) |
			(PackAmbientMultiplier12(metalAmbient) << 20u);
	}

	static float ClampDirectionalAngularSize(float angularSize)
	{
		return std::clamp(angularSize, 0.001f, 1.2f);
	}

	static uint32_t PackDirectionalLightColor24(const float color[3])
	{
		auto packChannel = [](float value) -> uint32_t
		{
			const float clamped = std::clamp(value, 0.0f, 8.0f);
			return (uint32_t)std::clamp((int)std::lround((double)(clamped * (255.0f / 8.0f))), 0, 255);
		};

		const uint32_t r = packChannel(color[0]);
		const uint32_t g = packChannel(color[1]);
		const uint32_t b = packChannel(color[2]);
		return r | (g << 8u) | (b << 16u);
	}

	static uint32_t PackDirectionalAngularSize16(float angularSize)
	{
		const float normalized = ClampDirectionalAngularSize(angularSize) / 1.2f;
		return (uint32_t)std::clamp((int)std::lround((double)(normalized * 65535.0f)), 0, 65535);
	}

	static uint32_t PackTraceBounceCounts(uint32_t lightBounceCount, uint32_t mirrorBounceCount, const float directionalColor[3])
	{
		return
			(lightBounceCount & 0xfu) |
			((mirrorBounceCount & 0xfu) << 4u) |
			(PackDirectionalLightColor24(directionalColor) << 8u);
	}

	static uint32_t PackTraceAux1(uint32_t denoiserMode, uint32_t emissiveSampleCount, float directionalAngularSize)
	{
		return
			(denoiserMode & 0xffu) |
			((emissiveSampleCount & 0xffu) << 8u) |
			(PackDirectionalAngularSize16(directionalAngularSize) << 16u);
	}

	static uint32_t PackDenoiserAux1(uint32_t denoiserMode, float directionalAngularSize)
	{
		return (denoiserMode & 0xffu) | (PackDirectionalAngularSize16(directionalAngularSize) << 16u);
	}

	static float GetTemporalExposure(const NRIPTOutputPolicy& outputPolicy)
	{
		return std::max(outputPolicy.exposure, 0.125f);
	}

	static uint32_t PackPresentSceneOrigin(int sceneLeft, int sceneTop)
	{
		return (uint16_t)(int16_t)sceneLeft | ((uint32_t)(uint16_t)(int16_t)sceneTop << 16);
	}

	static uint32_t PackNightVisionControls(float contrast, float saturation)
	{
		const uint32_t contrastBits = (uint32_t)std::lround(std::clamp(contrast, 0.0f, 2.0f) * (65535.0f / 2.0f));
		const uint32_t saturationBits = (uint32_t)std::lround(std::clamp(saturation, 0.0f, 2.0f) * (65535.0f / 2.0f));
		return contrastBits | (saturationBits << 16);
	}

	static uint32_t PackNightVisionModeAndTint(NRIPTNightVisionMode mode, float red, float green, float blue)
	{
		const uint32_t redBits = (uint32_t)std::lround(std::clamp(red, 0.0f, 2.0f) * (255.0f / 2.0f));
		const uint32_t greenBits = (uint32_t)std::lround(std::clamp(green, 0.0f, 2.0f) * (255.0f / 2.0f));
		const uint32_t blueBits = (uint32_t)std::lround(std::clamp(blue, 0.0f, 2.0f) * (255.0f / 2.0f));
		return (uint32_t)mode | (redBits << 8) | (greenBits << 16) | (blueBits << 24);
	}

	static void ApplyOutputPolicyToPresentConstants(const NRIPTOutputPolicy& policy, NRIPresentConstants& constants)
	{
		constants.OutputMode = (uint32_t)policy.resolvedMode;
		constants.TonemapMode = (uint32_t)policy.tonemapMode;
		constants.OutputFlags =
			(policy.displayInfoAvailable ? NRI_PRESENT_OUTPUT_FLAG_DISPLAY_INFO_AVAILABLE : 0u) |
			(policy.displayHdrSupported ? NRI_PRESENT_OUTPUT_FLAG_DISPLAY_HDR_SUPPORTED : 0u) |
			(policy.hdrSwapChainActive ? NRI_PRESENT_OUTPUT_FLAG_HDR_SWAPCHAIN_ACTIVE : 0u) |
			(policy.offscreenHdrTarget ? NRI_PRESENT_OUTPUT_FLAG_OFFSCREEN_HDR_TARGET : 0u);
		constants.Exposure = policy.exposure;
		constants.Contrast = policy.contrast;
		constants.Saturation = policy.saturation;
		constants.Shoulder = policy.shoulder;
		constants.Toe = policy.toe;
		constants.PaperWhiteNits = policy.paperWhiteNits;
		constants.DisplayMaxLuminance = policy.displayMaxLuminance;
		constants.DisplaySdrLuminance = policy.displaySdrLuminance;
	}

	static void ApplyNightVisionStateToPresentConstants(const NRIPTNightVisionState& state, NRIPresentConstants& constants)
	{
		constants.NightVisionPackedModeTint = PackNightVisionModeAndTint(
			state.mode,
			(float)nri_ptnightvisionred,
			(float)nri_ptnightvisiongreen,
			(float)nri_ptnightvisionblue);
		constants.NightVisionStrength = nri_ptnightvision ? state.strength01 : 0.0f;
		constants.NightVisionExposure = (float)nri_ptnightvisionexposure;
		constants.NightVisionPackedControls = PackNightVisionControls(
			(float)nri_ptnightvisioncontrast,
			(float)nri_ptnightvisionsaturation);
	}

	static void Copy3(const float* src, float* dst)
	{
		std::memcpy(dst, src, sizeof(float) * 3);
	}

	static void Copy2(const float* src, float* dst)
	{
		std::memcpy(dst, src, sizeof(float) * 2);
	}

	static void Normalize3(float v[3])
	{
		const float length = std::max(0.0001f, sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}

	static void ApplyDirectionalLightStateToConstants(const NRIDirectionalLightState& state, NRITraceSceneConstants& constants)
	{
		constants.LightDirection[0] = state.direction[0];
		constants.LightDirection[1] = state.direction[1];
		constants.LightDirection[2] = state.direction[2];
		Normalize3(constants.LightDirection);
	}
}

bool NRIPassDispatcher::DispatchBootstrapView(NRIRenderer& renderer)
{
	Clocker clock(NriPTBootstrapDispatch);

	if (!NRISceneUploadManager::UpdateReprojectionBuffer(renderer, nullptr))
	{
		return false;
	}

	const uint32_t bootstrapMode = GetBootstrapMode();
	NRITraceSceneConstants constants = {};
	Copy3(renderer.mCurrentCameraPos, constants.CameraPos);
	Copy3(renderer.mCurrentCameraForward, constants.CameraForward);
	Copy3(renderer.mCurrentCameraRight, constants.CameraRight);
	Copy3(renderer.mCurrentCameraUp, constants.CameraUp);
	Copy3(renderer.mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(renderer.mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(renderer.mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(renderer.mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = renderer.mRenderWidth;
	constants.RenderHeight = renderer.mRenderHeight;
	constants.DisplayWidth = renderer.mOutputWidth;
	constants.DisplayHeight = renderer.mOutputHeight;
	constants.TanHalfFovX = renderer.mCurrentTanHalfFovX;
	constants.TanHalfFovY = renderer.mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = renderer.mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = renderer.mPreviousTanHalfFovY;
	constants.SceneInstanceCount = renderer.mSceneInstanceBuffer.stride != 0 ? (uint32_t)(renderer.mSceneInstanceBuffer.usedSize / renderer.mSceneInstanceBuffer.stride) : 0u;
	constants.StaticPrimitiveCount = renderer.mBoundStaticPrimitiveCount;
	constants.DynamicPrimitiveCount = renderer.mBoundDynamicPrimitiveCount;
	constants.FrameIndex = renderer.mFrameIndex;
	constants.Flags =
		NRI_FLAG_BOOTSTRAP_VIEW |
		(renderer.mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(renderer.mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(renderer.mDirectionalLightState.enabled && renderer.mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u);
	constants.StaticMaterialCount = renderer.mBoundStaticMaterialCount;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = renderer.mBoundDynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(0u, 0u, renderer.mDirectionalLightState.color);
	constants.ReservedTrace0 = (uint16_t)(int16_t)renderer.mSceneLeft | ((uint32_t)(uint16_t)(int16_t)renderer.mSceneTop << 16);
	Copy3(renderer.mSkyColor, constants.SkyColor);
	Copy3(renderer.mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(renderer.mDirectionalLightState, constants);

	NRITextureResource& history = renderer.GetFrameTexture(renderer.mHistoryOutputSlot);
	NRITextureResource& upscaled = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed);
	NRITextureResource& final = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Final);
	renderer.mFrameBuffer->TransitionTexture(history, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Validation), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(upscaled, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	renderer.mFrameInputDescriptors.fill(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed).shaderView);
	renderer.mFrameInputDescriptors[0] = history.shaderView;
	renderer.mFrameInputDescriptors[1] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion).shaderView;
	renderer.mFrameInputDescriptors[2] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ).shaderView;
	renderer.mFrameInputDescriptors[3] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness).shaderView;
	renderer.mFrameInputDescriptors[4] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness).shaderView;
	renderer.mFrameInputDescriptors[5] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed).shaderView;
	renderer.mFrameInputDescriptors[6] = upscaled.shaderView;
	renderer.mFrameInputDescriptors[7] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Validation).shaderView;
	renderer.mFrameInputDescriptors[8] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse).shaderView;
	renderer.mFrameInputDescriptors[9] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular).shaderView;
	renderer.mFrameInputDescriptors[10] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular).shaderView;
	NRIDescriptorSetManager::UpdateFrameTextureSet(renderer, renderer.mUpscalerPrepassFrameTextureSet, renderer.mFrameInputDescriptors);

	renderer.mOutputDescriptors.fill(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::VendorOutput).storageView);
	renderer.mOutputDescriptors[2] = final.storageView;
	NRIDescriptorSetManager::UpdateOutputSet(renderer, renderer.mUpscalerPrepassOutputSet, renderer.mOutputDescriptors);

	renderer.mFrameBuffer->mCore.CmdSetPipelineLayout(*renderer.mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *renderer.mPipelineLayout);
	renderer.mFrameBuffer->mCore.CmdSetRootConstants(*renderer.mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	renderer.BindSceneRootDescriptors();
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 0, renderer.mSamplerSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 1, renderer.GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 2, renderer.GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 3, renderer.mFrameTextureSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 4, renderer.mOutputSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetPipeline(*renderer.mFrameBuffer->mCommandBuffer, *renderer.GetPipeline(NRIRenderer::PipelineSlot::Final));
	renderer.mFrameBuffer->mCore.CmdDispatch(*renderer.mFrameBuffer->mCommandBuffer, { GetDispatchSize(renderer.mTargetWidth), GetDispatchSize(renderer.mTargetHeight), 1 });
	return true;
}



bool NRIPassDispatcher::DispatchFrameGraph(NRIRenderer& renderer, HWDrawInfo& di, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials, int)
{
	ScopedPtPerfTimer perfTimer(renderer.mLastPerfShellTraceStats.frameGraphMs);
	Clocker clock(NriPTFrameGraph);

	const int ptDebugMode = (int)GetEffectivePtDebugMode();
	NRIFrameGraphExecutionRequest request = {};
	request.ptDebugMode = ptDebugMode;
	request.denoise = !!nri_denoise;
	request.presentRoute = ResolvePresentRouteInfo((uint32_t)ptDebugMode, !!nri_ptbootstrap);
	return ExecuteNRIFrameGraph(renderer, di, geometry, materials, request);
}



bool NRIPassDispatcher::DispatchTraceOpaque(NRIRenderer& renderer, HWDrawInfo&, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials)
{
	Clocker clock(NriPTTraceOpaque);
	ScopedPtPerfTimer traceOpaqueTimer(renderer.mLastPerfShellTraceStats.traceOpaqueMs);
	{
		ScopedPtPerfTimer perfTimer(renderer.mLastPerfShellTraceStats.traceOpaqueReadbackMs);
		NRITraceShaderStatsReadbackInput input = {};
		input.enabled = (bool)nri_ptshaderstats;
		input.boundSceneInstances = &renderer.mBoundSceneInstances;
		input.staticPrimitiveCount = renderer.mBoundStaticPrimitiveCount;
		input.dynamicPrimitiveCount = renderer.mBoundDynamicPrimitiveCount;
		input.persistentVoxelPrimitiveCount = renderer.mPersistentVoxels.BoundPrimitiveCount();
		input.user = &renderer;
		input.estimatePersistentVoxelPrimitiveCount = [](void* user, uint32_t primitiveOffset) -> uint32_t
		{
			return static_cast<NRIRenderer*>(user)->mPersistentVoxels.EstimatePrimitiveCountForInstanceOffset(primitiveOffset);
		};
		renderer.mTraceShaderStats.Readback(renderer.BuildResourceServices(), input, renderer.mLastPerfTraceShaderStats);
		renderer.ReadbackAutoExposureStats();
	}

	if (!NRISceneUploadManager::UpdateReprojectionBuffer(renderer, nullptr))
	{
		return false;
	}

	NRITraceSceneConstants constants = {};
	const NRITraceSettings traceSettings = BuildNRITraceSettingsFromCVars();
	const NRIDenoiserSettings denoiserSettings = BuildNRIDenoiserSettingsFromCVars();
	const uint32_t bootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	const NRIMainUpscalerKind resolvedMainUpscaler = renderer.ResolveMainUpscalerKind(false);
	const nri::UpscalerMode resolvedUpscalerMode = NRIResolveUpscalerModeForMain(resolvedMainUpscaler, renderer.GetSelectedUpscalerMode());
	const uint32_t jitterPhaseCount = NRIGetTemporalJitterPhaseCount(resolvedMainUpscaler, resolvedUpscalerMode, renderer.mGuiCaptureActive);
	const bool directSceneTrace = (!nri_ptbootstrap && nri_ptdirectscene) || bootstrapMode == 11u || bootstrapMode == 12u;
	const bool useTemporalJitter =
		!nri_ptbootstrap &&
		!renderer.mGuiCaptureActive &&
		NRIShouldUseTemporalJitter(resolvedMainUpscaler);
	Copy3(renderer.mCurrentCameraPos, constants.CameraPos);
	Copy3(renderer.mCurrentCameraForward, constants.CameraForward);
	Copy3(renderer.mCurrentCameraRight, constants.CameraRight);
	Copy3(renderer.mCurrentCameraUp, constants.CameraUp);
	Copy3(renderer.mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(renderer.mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(renderer.mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(renderer.mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = renderer.mRenderWidth;
	constants.RenderHeight = renderer.mRenderHeight;
	constants.DisplayWidth = renderer.mOutputWidth;
	constants.DisplayHeight = renderer.mOutputHeight;
	constants.TanHalfFovX = renderer.mCurrentTanHalfFovX;
	constants.TanHalfFovY = renderer.mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = renderer.mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = renderer.mPreviousTanHalfFovY;
	constants.SceneInstanceCount = renderer.mSceneInstanceBuffer.stride != 0 ? (uint32_t)(renderer.mSceneInstanceBuffer.usedSize / renderer.mSceneInstanceBuffer.stride) : 0u;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.StaticPrimitiveCount = renderer.mBoundStaticPrimitiveCount;
	constants.FrameIndex = renderer.mFrameIndex;
	constants.DynamicPrimitiveCount = renderer.mBoundDynamicPrimitiveCount;
	constants.Flags =
		(renderer.mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(directSceneTrace ? NRI_FLAG_PRESENT_RAW_TRACE : 0u) |
		(renderer.mUseSplitShadowDenoiser && !directSceneTrace ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(renderer.mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(renderer.mDirectionalLightState.enabled && renderer.mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u) |
		(nri_ptemissivefastshadow ? NRI_FLAG_FAST_EMISSIVE_SHADOW : 0u) |
		(nri_ptvisiblechunkgate ? NRI_FLAG_GATE_PRIMARY_VISIBLE_CHUNKS : 0u) |
		(ShouldCollectTraceShaderStats() ? NRI_FLAG_TRACE_SHADER_STATS : 0u) |
		(useTemporalJitter ? NRI_FLAG_USE_JITTER : 0u) |
		NRIPackTemporalJitterPhaseCount(jitterPhaseCount);
	constants.StaticMaterialCount = renderer.mBoundStaticMaterialCount;
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = renderer.mBoundDynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(
		traceSettings.lightBounceCount,
		traceSettings.mirrorBounceCount,
		renderer.mDirectionalLightState.color);
	constants.PortalCount = renderer.mBoundPortalCount;
	constants.RuntimeLightCount = renderer.mBoundRuntimeLightCount;
	constants.PortalDepth = PackPortalDepthAndAmbientMultipliers(
		traceSettings.portalDepth,
		GetBaseAmbient(),
		GetMetalAmbient());
	constants.ReservedTrace0 = (renderer.mBoundRuntimeLightTileCountX & 0xffffu) | ((renderer.mBoundRuntimeLightTileCountY & 0xffffu) << 16u);
	constants.ReservedTrace1 = PackTraceAux1(
		(uint32_t)denoiserSettings.denoiserMode,
		traceSettings.emissiveSampleCount,
		renderer.mDirectionalLightState.angularSize);
	Copy3(renderer.mSkyColor, constants.SkyColor);
	Copy3(renderer.mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(renderer.mDirectionalLightState, constants);

	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse), NRIComputeStorageState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular), NRIComputeStorageState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredPenumbra), NRIComputeStorageState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectLighting), NRIComputeStorageState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectEmission), NRIComputeStorageState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion), NRIComputeStorageState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ), NRIComputeStorageState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness), NRIComputeStorageState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness), NRIComputeStorageState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::SrInput), NRIComputeStorageState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo), NRIComputeStorageState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideSpecularHitDistance), NRIComputeStorageState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Validation), NRIComputeStorageState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::VendorOutput), NRIComputeStorageState());

	const nri::Descriptor* defaultInput = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed).shaderView;
	renderer.mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	NRIDescriptorSetManager::UpdateFrameTextureSet(renderer);

	const nri::Descriptor* defaultOutput = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Validation).storageView;
	renderer.mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	renderer.mOutputDescriptors[0] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse).storageView;
	renderer.mOutputDescriptors[3] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion).storageView;
	renderer.mOutputDescriptors[4] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ).storageView;
	renderer.mOutputDescriptors[5] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness).storageView;
	renderer.mOutputDescriptors[6] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness).storageView;
	renderer.mOutputDescriptors[9] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo).storageView;
	renderer.mOutputDescriptors[10] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular).storageView;
	renderer.mOutputDescriptors[11] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideSpecularHitDistance).storageView;
	renderer.mOutputDescriptors[12] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredPenumbra).storageView;
	renderer.mOutputDescriptors[13] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectLighting).storageView;
	renderer.mOutputDescriptors[14] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectEmission).storageView;
	NRIDescriptorSetManager::UpdateOutputSet(renderer);

	renderer.mFrameBuffer->mCore.CmdSetPipelineLayout(*renderer.mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *renderer.mPipelineLayout);
	renderer.mFrameBuffer->mCore.CmdSetRootConstants(*renderer.mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	renderer.BindSceneRootDescriptors();
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 0, renderer.mSamplerSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 1, renderer.GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 2, renderer.GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 3, renderer.mFrameTextureSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 4, renderer.mOutputSet, nri::BindPoint::COMPUTE });
	const uint32_t dispatchX = GetDispatchSize(renderer.mRenderWidth);
	const uint32_t dispatchY = GetDispatchSize(renderer.mRenderHeight);
	const uint32_t dispatchZ = 1;
	renderer.mLastPerfShellTraceStats.traceOpaqueDispatchX = dispatchX;
	renderer.mLastPerfShellTraceStats.traceOpaqueDispatchY = dispatchY;
	renderer.mLastPerfShellTraceStats.traceOpaqueDispatchZ = dispatchZ;
	{
		ScopedPtPerfTimer perfTimer(renderer.mLastPerfShellTraceStats.traceOpaqueCommandMs);
		renderer.mTraceShaderStats.ResetBuffer(renderer.BuildResourceServices(), ShouldCollectTraceShaderStats());
		renderer.mFrameBuffer->mCore.CmdSetPipeline(*renderer.mFrameBuffer->mCommandBuffer, *renderer.GetPipeline(NRIRenderer::PipelineSlot::TraceOpaque));
		renderer.mFrameBuffer->mCore.CmdDispatch(*renderer.mFrameBuffer->mCommandBuffer, { dispatchX, dispatchY, dispatchZ });
	}
	{
		ScopedPtPerfTimer perfTimer(renderer.mLastPerfShellTraceStats.traceOpaqueStatsCopyMs);
		renderer.mTraceShaderStats.CopyForReadback(renderer.BuildResourceServices(), ShouldCollectTraceShaderStats(), (uint64_t)renderer.mFrameIndex);
	}
	return true;
}



bool NRIPassDispatcher::DispatchDenoiser(NRIRenderer& renderer)
{
	Clocker clock(NriPTDenoiser);
	const NRIDenoiserSettings denoiserSettings = BuildNRIDenoiserSettingsFromCVars();

	if (!renderer.mNrd.EnsureReady(*renderer.mFrameBuffer->mDevice, renderer.mRenderWidth, renderer.mRenderHeight, 1))
	{
		return false;
	}

	renderer.mNrd.NewFrame();

	NRINrdDispatchDesc desc = {};
	desc.commandBuffer = renderer.mFrameBuffer->mCommandBuffer;
	desc.motion = &renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion);
	desc.viewZ = &renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ);
	desc.normalRoughness = &renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness);
	desc.baseColorMetalness = &renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness);
	desc.unfilteredDiffuse = &renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse);
	desc.unfilteredSpecular = &renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular);
	desc.unfilteredPenumbra = &renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredPenumbra);
	desc.diffuse = &renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::DenoisedDiffuse);
	desc.specular = &renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::DenoisedSpecular);
	desc.shadow = &renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::DenoisedShadow);
	desc.validation = &renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Validation);
	desc.resourceWidth = renderer.mRenderWidth;
	desc.resourceHeight = renderer.mRenderHeight;
	desc.frameIndex = renderer.mFrameIndex;
	Copy2(renderer.mCurrentJitter, desc.cameraJitter);
	Copy2(renderer.mPreviousJitter, desc.cameraJitterPrev);
	std::memcpy(desc.viewToClipMatrix, renderer.mCurrentViewToClip, sizeof(desc.viewToClipMatrix));
	std::memcpy(desc.viewToClipMatrixPrev, renderer.mPreviousViewToClip, sizeof(desc.viewToClipMatrixPrev));
	std::memcpy(desc.worldToViewMatrix, renderer.mCurrentWorldToView, sizeof(desc.worldToViewMatrix));
	std::memcpy(desc.worldToViewMatrixPrev, renderer.mPreviousWorldToView, sizeof(desc.worldToViewMatrixPrev));
	desc.lightDirection[0] = renderer.mDirectionalLightState.direction[0];
	desc.lightDirection[1] = renderer.mDirectionalLightState.direction[1];
	desc.lightDirection[2] = renderer.mDirectionalLightState.direction[2];
	Normalize3(desc.lightDirection);
	desc.denoiserMode = denoiserSettings.denoiserMode;
	desc.maxAccumulatedFrameNum = denoiserSettings.maxAccumulatedFrameNum;
	desc.maxFastAccumulatedFrameNum = denoiserSettings.maxFastAccumulatedFrameNum;
	desc.maxStabilizedFrameNum = denoiserSettings.maxStabilizedFrameNum;
	desc.hitDistanceReconstructionMode = denoiserSettings.hitDistanceReconstructionMode;
	desc.fastHistoryClampingSigmaScale = denoiserSettings.fastHistoryClampingSigmaScale;
	desc.diffusePrepassBlurRadius = denoiserSettings.diffusePrepassBlurRadius;
	desc.specularPrepassBlurRadius = denoiserSettings.specularPrepassBlurRadius;
	desc.minBlurRadius = denoiserSettings.minBlurRadius;
	desc.maxBlurRadius = denoiserSettings.maxBlurRadius;
	desc.sigmaMaxStabilizedFrameNum = denoiserSettings.sigmaMaxStabilizedFrameNum;
	desc.sigmaPlaneDistanceSensitivity = denoiserSettings.sigmaPlaneDistanceSensitivity;
	desc.resetHistory = renderer.mResetHistory;
	desc.enableAntiFirefly = denoiserSettings.enableAntiFirefly;
	desc.enableValidation = denoiserSettings.enableValidation;
	desc.enableSigmaShadow = renderer.mUseSplitShadowDenoiser;
	return renderer.mNrd.Denoise(desc);
}



bool NRIPassDispatcher::DispatchComposition(NRIRenderer& renderer, NRIRenderer::FrameTextureSlot outputSlot)
{
	Clocker clock(NriPTComposition);

	NRITraceSceneConstants constants = {};
	const NRIDenoiserSettings denoiserSettings = BuildNRIDenoiserSettingsFromCVars();
	Copy3(renderer.mCurrentCameraPos, constants.CameraPos);
	Copy3(renderer.mCurrentCameraForward, constants.CameraForward);
	Copy3(renderer.mCurrentCameraRight, constants.CameraRight);
	Copy3(renderer.mCurrentCameraUp, constants.CameraUp);
	Copy3(renderer.mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(renderer.mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(renderer.mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(renderer.mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = renderer.mRenderWidth;
	constants.RenderHeight = renderer.mRenderHeight;
	constants.DisplayWidth = renderer.mOutputWidth;
	constants.DisplayHeight = renderer.mOutputHeight;
	constants.TanHalfFovX = renderer.mCurrentTanHalfFovX;
	constants.TanHalfFovY = renderer.mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = renderer.mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = renderer.mPreviousTanHalfFovY;
	constants.FrameIndex = renderer.mFrameIndex;
	constants.Flags =
		(renderer.mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(renderer.mUseSplitShadowDenoiser ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(renderer.mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(renderer.mDirectionalLightState.enabled && renderer.mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u);
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.BootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	constants.BounceCounts = PackTraceBounceCounts(0u, 0u, renderer.mDirectionalLightState.color);
	constants.RuntimeLightCount = renderer.mBoundRuntimeLightCount;
	constants.ReservedTrace0 = denoiserSettings.inputSplitMode;
	constants.ReservedTrace1 = PackDenoiserAux1((uint32_t)denoiserSettings.denoiserMode, renderer.mDirectionalLightState.angularSize);
	Copy3(renderer.mSkyColor, constants.SkyColor);
	Copy3(renderer.mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(renderer.mDirectionalLightState, constants);

	NRITextureResource& diffuse = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse);
	NRITextureResource& specular = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular);
	NRITextureResource& viewZ = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ);
	NRITextureResource& normalRoughness = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness);
	NRITextureResource& baseColorMetalness = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness);
	NRITextureResource& rawShadow = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredPenumbra);
	NRITextureResource& directLighting = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectLighting);
	NRITextureResource& directEmission = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectEmission);
	const NRIRenderer::FrameTextureSlot filteredDiffuseSlot = renderer.mUseDenoisedCompositionInputs ? NRIRenderer::FrameTextureSlot::DenoisedDiffuse : NRIRenderer::FrameTextureSlot::UnfilteredDiffuse;
	const NRIRenderer::FrameTextureSlot filteredSpecularSlot = renderer.mUseDenoisedCompositionInputs ? NRIRenderer::FrameTextureSlot::DenoisedSpecular : NRIRenderer::FrameTextureSlot::UnfilteredSpecular;
	const NRIRenderer::FrameTextureSlot filteredShadowSlot = renderer.mUseDenoisedCompositionInputs ? NRIRenderer::FrameTextureSlot::DenoisedShadow : NRIRenderer::FrameTextureSlot::UnfilteredPenumbra;
	NRITextureResource& filteredDiffuse = renderer.GetFrameTexture(filteredDiffuseSlot);
	NRITextureResource& filteredSpecular = renderer.GetFrameTexture(filteredSpecularSlot);
	NRITextureResource& filteredShadow = renderer.GetFrameTexture(filteredShadowSlot);
	NRITextureResource& composed = renderer.GetFrameTexture(outputSlot);

	renderer.mFrameBuffer->TransitionTexture(diffuse, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(specular, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(viewZ, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(normalRoughness, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(baseColorMetalness, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(rawShadow, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(directLighting, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(directEmission, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(filteredDiffuse, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(filteredSpecular, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(filteredShadow, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(composed, NRIComputeStorageState());

	const nri::Descriptor* defaultInput = diffuse.shaderView;
	renderer.mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	renderer.mFrameInputDescriptors[2] = viewZ.shaderView;
	renderer.mFrameInputDescriptors[3] = normalRoughness.shaderView;
	renderer.mFrameInputDescriptors[4] = baseColorMetalness.shaderView;
	renderer.mFrameInputDescriptors[5] = diffuse.shaderView;
	renderer.mFrameInputDescriptors[6] = specular.shaderView;
	renderer.mFrameInputDescriptors[8] = filteredDiffuse.shaderView;
	renderer.mFrameInputDescriptors[9] = filteredSpecular.shaderView;
	renderer.mFrameInputDescriptors[10] = rawShadow.shaderView;
	renderer.mFrameInputDescriptors[11] = filteredShadow.shaderView;
	renderer.mFrameInputDescriptors[12] = directLighting.shaderView;
	renderer.mFrameInputDescriptors[13] = directEmission.shaderView;
	NRIDescriptorSetManager::UpdateFrameTextureSet(renderer, renderer.mCompositionFrameTextureSet, renderer.mFrameInputDescriptors);

	const nri::Descriptor* defaultOutput = composed.storageView;
	renderer.mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	renderer.mOutputDescriptors[1] = composed.storageView;
	NRIDescriptorSetManager::UpdateOutputSet(renderer, renderer.mCompositionOutputSet, renderer.mOutputDescriptors);

	renderer.mFrameBuffer->mCore.CmdSetPipelineLayout(*renderer.mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *renderer.mPipelineLayout);
	renderer.mFrameBuffer->mCore.CmdSetRootConstants(*renderer.mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	renderer.BindSceneRootDescriptors();
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 0, renderer.mSamplerSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 1, renderer.GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 2, renderer.GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 3, renderer.mCompositionFrameTextureSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 4, renderer.mCompositionOutputSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetPipeline(*renderer.mFrameBuffer->mCommandBuffer, *renderer.GetPipeline(NRIRenderer::PipelineSlot::Composition));
	renderer.mFrameBuffer->mCore.CmdDispatch(*renderer.mFrameBuffer->mCommandBuffer, { GetDispatchSize(renderer.mRenderWidth), GetDispatchSize(renderer.mRenderHeight), 1 });
	return true;
}



bool NRIPassDispatcher::DispatchTraceTransparent(NRIRenderer& renderer)
{
	Clocker clock(NriPTComposition);

	NRITextureResource& composed = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed);
	NRITextureResource& transparentOutput = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::TraceTransparentOutput);
	renderer.CopyTexture(composed, transparentOutput);
	return true;
}



bool NRIPassDispatcher::DispatchUpscalerPrepass(NRIRenderer& renderer, NRIMainUpscalerKind mainKind)
{
	if (mainKind == NRIMainUpscalerKind::Off)
	{
		return false;
	}

	const NRIRenderer::FrameTextureSlot vendorInputSlot =
		mainKind == NRIMainUpscalerKind::DLSR ? NRIRenderer::FrameTextureSlot::SrInput :
		NRIRenderer::FrameTextureSlot::RrInput;
	NRITextureResource& vendorInput = renderer.GetFrameTexture(vendorInputSlot);
	NRITextureResource& upscalerDepth = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UpscalerDepth);
	NRITextureResource& rrGuideDiffuseAlbedo = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo);
	NRITextureResource& rrGuideSpecularAlbedo = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideSpecularAlbedo);
	NRITextureResource& rrGuideSpecularHitDistance = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideSpecularHitDistance);
	NRITextureResource& rrGuideNormalRoughness = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideNormalRoughness);
	const bool useSrPrepass = mainKind == NRIMainUpscalerKind::DLSR;

	// SR consumes the post-transparent composed signal, while RR now arrives with an
	// explicitly prepared noisy RrInput from the frame-graph path above.
	if (useSrPrepass)
	{
		renderer.CopyTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::TraceTransparentOutput), vendorInput);
	}
	renderer.mFrameBuffer->TransitionTexture(vendorInput, NRIComputeShaderResourceState());

	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(upscalerDepth, NRIComputeStorageState());
	if (!useSrPrepass)
	{
		renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
		renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
		renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular), NRIComputeShaderResourceState());
		renderer.mFrameBuffer->TransitionTexture(rrGuideDiffuseAlbedo, NRIComputeStorageState());
		renderer.mFrameBuffer->TransitionTexture(rrGuideSpecularAlbedo, NRIComputeStorageState());
		renderer.mFrameBuffer->TransitionTexture(rrGuideSpecularHitDistance, NRIComputeStorageState());
		renderer.mFrameBuffer->TransitionTexture(rrGuideNormalRoughness, NRIComputeStorageState());
	}

	const nri::Descriptor* defaultInput = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ).shaderView;
	renderer.mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	renderer.mFrameInputDescriptors[2] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ).shaderView;
	if (!useSrPrepass)
	{
		renderer.mFrameInputDescriptors[3] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness).shaderView;
		renderer.mFrameInputDescriptors[4] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness).shaderView;
		renderer.mFrameInputDescriptors[6] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular).shaderView;
	}
	if (!NRIDescriptorSetManager::UpdateFrameTextureSet(renderer, renderer.mUpscalerPrepassFrameTextureSet, renderer.mFrameInputDescriptors))
	{
		return false;
	}

	const nri::Descriptor* defaultOutput = upscalerDepth.storageView;
	renderer.mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	renderer.mOutputDescriptors[12] = upscalerDepth.storageView;
	if (!useSrPrepass)
	{
		renderer.mOutputDescriptors[5] = rrGuideNormalRoughness.storageView;
		renderer.mOutputDescriptors[9] = rrGuideDiffuseAlbedo.storageView;
		renderer.mOutputDescriptors[10] = rrGuideSpecularAlbedo.storageView;
		renderer.mOutputDescriptors[11] = rrGuideSpecularHitDistance.storageView;
	}
	if (!NRIDescriptorSetManager::UpdateOutputSet(renderer, renderer.mUpscalerPrepassOutputSet, renderer.mOutputDescriptors))
	{
		return false;
	}

	NRITraceSceneConstants constants = {};
	constants.RenderWidth = renderer.mRenderWidth;
	constants.RenderHeight = renderer.mRenderHeight;
	constants.DisplayWidth = renderer.mOutputWidth;
	constants.DisplayHeight = renderer.mOutputHeight;
	constants.FrameIndex = renderer.mFrameIndex;
	constants.ReservedTrace0 =
		mainKind == NRIMainUpscalerKind::DLSR ? 1u :
		mainKind == NRIMainUpscalerKind::DLRR ? 2u :
		0u;
	constants.ReservedTrace1 = (uint32_t)GetSelectedNrdDenoiserMode();
	constants.Flags = renderer.mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u;
	renderer.mFrameBuffer->mCore.CmdSetPipelineLayout(*renderer.mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *renderer.mPipelineLayout);
	renderer.mFrameBuffer->mCore.CmdSetRootConstants(*renderer.mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	renderer.BindSceneRootDescriptors();
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 0, renderer.mSamplerSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 1, renderer.GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 2, renderer.GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 3, renderer.mUpscalerPrepassFrameTextureSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 4, renderer.mUpscalerPrepassOutputSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetPipeline(*renderer.mFrameBuffer->mCommandBuffer, *renderer.GetPipeline(useSrPrepass ? NRIRenderer::PipelineSlot::DlssSrBefore : NRIRenderer::PipelineSlot::DlssBefore));
	renderer.mFrameBuffer->mCore.CmdDispatch(*renderer.mFrameBuffer->mCommandBuffer, { GetDispatchSize(renderer.mRenderWidth), GetDispatchSize(renderer.mRenderHeight), 1 });
	return true;
}



bool NRIPassDispatcher::DispatchRawPresent(NRIRenderer& renderer, NRIRenderer::FrameTextureSlot inputSlot, NRIRenderer::FrameTextureSlot secondarySlot, NRIRenderer::FrameTextureSlot tertiarySlot)
{
	Clocker clock(NriPTRawPresent);

	NRIPresentConstants constants = {};
	ApplyOutputPolicyToPresentConstants(renderer.mFrameBuffer->GetPathTracingOutputPolicy(), constants);
	constants.DisplayWidth = renderer.mOutputWidth;
	constants.DisplayHeight = renderer.mOutputHeight;
	constants.FrameIndex = renderer.mFrameIndex;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.PackedSceneOrigin = PackPresentSceneOrigin(renderer.mSceneLeft, renderer.mSceneTop);
	constants.DenoiserMode = (uint32_t)GetSelectedNrdDenoiserMode();

	NRITextureResource& input = renderer.GetFrameTexture(inputSlot);
	constants.InputWidth = input.width;
	constants.InputHeight = input.height;
	const bool addSecondary = secondarySlot != NRIRenderer::FrameTextureSlot::Count;
	NRITextureResource& secondary = renderer.GetFrameTexture(addSecondary ? secondarySlot : inputSlot);
	const bool hasTertiary = tertiarySlot != NRIRenderer::FrameTextureSlot::Count;
	NRITextureResource& tertiary = renderer.GetFrameTexture(hasTertiary ? tertiarySlot : inputSlot);
	NRITextureResource& final = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Final);
	if (addSecondary)
	{
		constants.Flags |= NRI_FLAG_RAW_PRESENT_ADD_SECONDARY;
	}
	if (renderer.mUseSplitShadowDenoiser)
	{
		constants.Flags |= NRI_PRESENT_FLAG_SPLIT_SHADOW_DENOISER;
	}

	renderer.mFrameBuffer->TransitionTexture(input, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(secondary, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(tertiary, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	const nri::Descriptor* inputs[3] = {
		input.shaderView,
		secondary.shaderView,
		tertiary.shaderView
	};
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = renderer.mRawPresentFrameTextureSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputs;
	inputUpdate.descriptorNum = (uint32_t)std::size(inputs);
	renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(&inputUpdate, 1);

	const nri::Descriptor* outputs[1] = { final.storageView };
	nri::UpdateDescriptorRangeDesc outputUpdate = {};
	outputUpdate.descriptorSet = renderer.mRawPresentOutputSet;
	outputUpdate.rangeIndex = 0;
	outputUpdate.descriptors = outputs;
	outputUpdate.descriptorNum = (uint32_t)std::size(outputs);
	renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(&outputUpdate, 1);

	renderer.mFrameBuffer->mCore.CmdSetPipelineLayout(*renderer.mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *renderer.mPresentPipelineLayout);
	renderer.mFrameBuffer->mCore.CmdSetRootConstants(*renderer.mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 0, renderer.mRawPresentFrameTextureSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 1, renderer.mRawPresentOutputSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetPipeline(*renderer.mFrameBuffer->mCommandBuffer, *renderer.GetPipeline(NRIRenderer::PipelineSlot::RawPresent));
	renderer.mFrameBuffer->mCore.CmdDispatch(*renderer.mFrameBuffer->mCommandBuffer, { GetDispatchSize(renderer.mTargetWidth), GetDispatchSize(renderer.mTargetHeight), 1 });
	return true;
}



bool NRIPassDispatcher::DispatchFinalPresent(NRIRenderer& renderer, NRIRenderer::FrameTextureSlot inputSlot)
{
	Clocker clock(NriPTFinalPresent);

	const NRIPTOutputPolicy outputPolicy = renderer.mFrameBuffer->GetPathTracingOutputPolicy();
	const NRIMainUpscalerKind resolvedMain = renderer.ResolveMainUpscalerKind(false);
	const NRIPostSharpenKind resolvedPost = renderer.ResolvePostSharpenKind(false);
	const NRIRenderer::ExposureRoute exposureRoute = renderer.ResolveExposureRoute(inputSlot, outputPolicy, resolvedMain, resolvedPost);
	NRIPresentConstants constants = {};
	ApplyOutputPolicyToPresentConstants(outputPolicy, constants);
	ApplyNightVisionStateToPresentConstants(renderer.mNightVisionState, constants);
	constants.Exposure = exposureRoute.presentExposure;
	const bool finalPresentInputPreExposed = exposureRoute.inputDomain == NRIRenderer::ExposureDomain::PreExposedHDR;
	const bool finalPresentAutoExposureEligible =
		renderer.mExposure.GetSettings().enabled &&
		exposureRoute.inputDomain == NRIRenderer::ExposureDomain::SceneHDR;
	NRITextureResource* exposureStateTexture = nullptr;
	if (finalPresentAutoExposureEligible)
	{
		NRITextureResource& candidateExposureState = renderer.mExposure.GetMutableExposureStateTexture(renderer.mFrameIndex & 1u);
		if (candidateExposureState.texture != nullptr)
		{
			exposureStateTexture = &candidateExposureState;
		}
	}
	const bool exposureStateTextureValid =
		exposureStateTexture != nullptr &&
		exposureStateTexture->shaderView != nullptr;
	constants.OutputFlags |=
		(finalPresentAutoExposureEligible ? NRI_PRESENT_OUTPUT_FLAG_AUTO_EXPOSURE : 0u) |
		(exposureStateTextureValid ? NRI_PRESENT_OUTPUT_FLAG_EXPOSURE_TEXTURE_VALID : 0u) |
		(finalPresentInputPreExposed ? NRI_PRESENT_OUTPUT_FLAG_INPUT_PRE_EXPOSED : 0u);
	constants.DisplayWidth = renderer.mOutputWidth;
	constants.DisplayHeight = renderer.mOutputHeight;
	constants.FrameIndex = renderer.mFrameIndex;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.PackedSceneOrigin = PackPresentSceneOrigin(renderer.mSceneLeft, renderer.mSceneTop);

	NRITextureResource& input = renderer.GetFrameTexture(inputSlot);
	NRITextureResource& final = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Final);
	constants.InputWidth = input.width;
	constants.InputHeight = input.height;

	renderer.mFrameBuffer->TransitionTexture(input, NRIComputeShaderResourceState());
	if (exposureStateTextureValid)
	{
		renderer.mFrameBuffer->TransitionTexture(*exposureStateTexture, NRIComputeShaderResourceState());
	}
	renderer.mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	const nri::Descriptor* inputs[3] = {
		input.shaderView,
		exposureStateTextureValid ? exposureStateTexture->shaderView : input.shaderView,
		input.shaderView
	};
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = renderer.mFinalPresentFrameTextureSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputs;
	inputUpdate.descriptorNum = (uint32_t)std::size(inputs);
	renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(&inputUpdate, 1);

	const nri::Descriptor* outputs[1] = { final.storageView };
	nri::UpdateDescriptorRangeDesc outputUpdate = {};
	outputUpdate.descriptorSet = renderer.mFinalPresentOutputSet;
	outputUpdate.rangeIndex = 0;
	outputUpdate.descriptors = outputs;
	outputUpdate.descriptorNum = (uint32_t)std::size(outputs);
	renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(&outputUpdate, 1);

	renderer.mFrameBuffer->mCore.CmdSetPipelineLayout(*renderer.mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *renderer.mPresentPipelineLayout);
	renderer.mFrameBuffer->mCore.CmdSetRootConstants(*renderer.mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 0, renderer.mFinalPresentFrameTextureSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 1, renderer.mFinalPresentOutputSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetPipeline(*renderer.mFrameBuffer->mCommandBuffer, *renderer.GetPipeline(NRIRenderer::PipelineSlot::FinalPresent));
	renderer.mFrameBuffer->mCore.CmdDispatch(*renderer.mFrameBuffer->mCommandBuffer, { GetDispatchSize(renderer.mTargetWidth), GetDispatchSize(renderer.mTargetHeight), 1 });
	return true;
}



bool NRIPassDispatcher::DispatchUpscaleChain(NRIRenderer& renderer)
{
	Clocker clock(NriPTUpscale);

	const NRIMainUpscalerKind mainKind = renderer.ResolveMainUpscalerKind(true);
	const NRIPostSharpenKind postSharpenKind = renderer.ResolvePostSharpenKind(true);
	const bool runAppTaa = NRIShouldRunAppTaa(mainKind);
	const bool useAppTaaJitter = runAppTaa && !renderer.mGuiCaptureActive;
	NRITextureResource& composed = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::TraceTransparentOutput);
	const NRIRenderer::FrameTextureSlot vendorSourceSlot =
		mainKind == NRIMainUpscalerKind::DLRR ? NRIRenderer::FrameTextureSlot::RrInput :
		NRIRenderer::FrameTextureSlot::TraceTransparentOutput;
	NRITextureResource& historyInput = renderer.GetFrameTexture(renderer.mHistoryInputSlot);
	NRITextureResource& historyOutput = renderer.GetFrameTexture(renderer.mHistoryOutputSlot);
	renderer.TraceTemporalState("upscale-entry", mainKind, postSharpenKind, runAppTaa, renderer.mHistoryOutputSlot, vendorSourceSlot);

	if (runAppTaa)
	{
		NRITemporalConstants constants = {};
		constants.RenderWidth = renderer.mRenderWidth;
		constants.RenderHeight = renderer.mRenderHeight;
		constants.FrameIndex = renderer.mFrameIndex;
		constants.Flags =
			(renderer.mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
			(useAppTaaJitter ? NRI_FLAG_USE_JITTER : 0u) |
			NRIPackTemporalJitterPhaseCount(NRIGetTemporalJitterPhaseCount(
				mainKind,
				NRIResolveUpscalerModeForMain(mainKind, renderer.GetSelectedUpscalerMode()),
				renderer.mGuiCaptureActive));
		constants.Exposure = GetTemporalExposure(renderer.mFrameBuffer->GetPathTracingOutputPolicy());
		NRITextureResource* exposureStateTexture = nullptr;
		if (renderer.mExposure.GetSettings().enabled)
		{
			NRITextureResource& candidateExposureState = renderer.mExposure.GetMutableExposureStateTexture(renderer.mFrameIndex & 1u);
			if (candidateExposureState.texture != nullptr)
			{
				exposureStateTexture = &candidateExposureState;
			}
		}
		const bool exposureStateTextureValid =
			exposureStateTexture != nullptr &&
			exposureStateTexture->shaderView != nullptr;
		constants.Flags |=
			(renderer.mExposure.GetSettings().enabled ? NRI_TEMPORAL_FLAG_AUTO_EXPOSURE : 0u) |
			(exposureStateTextureValid ? NRI_TEMPORAL_FLAG_EXPOSURE_TEXTURE_VALID : 0u);

		renderer.mFrameBuffer->TransitionTexture(composed, NRIComputeShaderResourceState());
		renderer.mFrameBuffer->TransitionTexture(historyInput, NRIComputeShaderResourceState());
		renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion), NRIComputeShaderResourceState());
		if (exposureStateTextureValid)
		{
			renderer.mFrameBuffer->TransitionTexture(*exposureStateTexture, NRIComputeShaderResourceState());
		}
		renderer.mFrameBuffer->TransitionTexture(historyOutput, NRIComputeStorageState());

		const nri::Descriptor* taaInputs[4] = {
			historyInput.shaderView,
			renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion).shaderView,
			composed.shaderView,
			exposureStateTextureValid ? exposureStateTexture->shaderView : composed.shaderView
		};
		nri::UpdateDescriptorRangeDesc taaInputUpdate = {};
		taaInputUpdate.descriptorSet = renderer.mTaaFrameTextureSet;
		taaInputUpdate.rangeIndex = 0;
		taaInputUpdate.descriptors = taaInputs;
		taaInputUpdate.descriptorNum = (uint32_t)std::size(taaInputs);
		renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(&taaInputUpdate, 1);

		const nri::Descriptor* taaOutputs[1] = { historyOutput.storageView };
		nri::UpdateDescriptorRangeDesc taaOutputUpdate = {};
		taaOutputUpdate.descriptorSet = renderer.mTaaOutputSet;
		taaOutputUpdate.rangeIndex = 0;
		taaOutputUpdate.descriptors = taaOutputs;
		taaOutputUpdate.descriptorNum = (uint32_t)std::size(taaOutputs);
		renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(&taaOutputUpdate, 1);

		renderer.mFrameBuffer->mCore.CmdSetPipelineLayout(*renderer.mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *renderer.mTaaPipelineLayout);
		renderer.mFrameBuffer->mCore.CmdSetRootConstants(*renderer.mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
		renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 0, renderer.mTaaFrameTextureSet, nri::BindPoint::COMPUTE });
		renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 1, renderer.mTaaOutputSet, nri::BindPoint::COMPUTE });
		renderer.mFrameBuffer->mCore.CmdSetPipeline(*renderer.mFrameBuffer->mCommandBuffer, *renderer.GetPipeline(NRIRenderer::PipelineSlot::Taa));
		renderer.mFrameBuffer->mCore.CmdDispatch(*renderer.mFrameBuffer->mCommandBuffer, { GetDispatchSize(renderer.mRenderWidth), GetDispatchSize(renderer.mRenderHeight), 1 });
	}
	else if (mainKind == NRIMainUpscalerKind::Off)
	{
		renderer.CopyTexture(composed, historyOutput);
	}
	else if (mainKind == NRIMainUpscalerKind::DLSR)
	{
		// Keep ptdebug 13 renderer.meaningful even when app-TAA is intentionally bypassed for vendor SR.
		renderer.CopyTexture(composed, historyOutput);
	}
	else if (mainKind == NRIMainUpscalerKind::DLRR)
	{
		// Keep ptdebug 13 renderer.meaningful for RR as well by exposing the explicit noisy RR input.
		renderer.CopyTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrInput), historyOutput);
	}

	NRIRenderer::FrameTextureSlot resolvedInputSlot = renderer.mHistoryOutputSlot;

	if (mainKind != NRIMainUpscalerKind::Off)
	{
		const NRIRenderer::FrameTextureSlot vendorInputSlot =
			mainKind == NRIMainUpscalerKind::DLSR ? NRIRenderer::FrameTextureSlot::SrInput :
			NRIRenderer::FrameTextureSlot::RrInput;
		NRITextureResource& vendorInput = renderer.GetFrameTexture(vendorInputSlot);
		NRITextureResource& upscalerDepth = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UpscalerDepth);
		NRITextureResource& rrGuideDiffuseAlbedo = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo);
		NRITextureResource& rrGuideSpecularAlbedo = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideSpecularAlbedo);
		NRITextureResource& rrGuideSpecularHitDistance = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideSpecularHitDistance);
		NRITextureResource& rrGuideNormalRoughness = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideNormalRoughness);
		NRITextureResource& vendorOutput = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::VendorOutput);
		NRITextureResource* vendorExposure = nullptr;
		if (renderer.mExposure.GetSettings().enabled)
		{
			NRITextureResource& candidateExposureState = renderer.mExposure.GetMutableExposureStateTexture(renderer.mFrameIndex & 1u);
			if (candidateExposureState.texture != nullptr && candidateExposureState.shaderView != nullptr)
			{
				vendorExposure = &candidateExposureState;
			}
		}

		if (!NRIPassDispatcher::DispatchUpscalerPrepass(renderer, mainKind))
		{
			return false;
		}

		renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion), NRIComputeShaderResourceState());
		renderer.mFrameBuffer->TransitionTexture(vendorInput, NRIComputeShaderResourceState());
		renderer.mFrameBuffer->TransitionTexture(upscalerDepth, NRIComputeShaderResourceState());
		renderer.mFrameBuffer->TransitionTexture(rrGuideDiffuseAlbedo, NRIComputeShaderResourceState());
		renderer.mFrameBuffer->TransitionTexture(rrGuideSpecularAlbedo, NRIComputeShaderResourceState());
		renderer.mFrameBuffer->TransitionTexture(rrGuideSpecularHitDistance, NRIComputeShaderResourceState());
		renderer.mFrameBuffer->TransitionTexture(rrGuideNormalRoughness, NRIComputeShaderResourceState());
		if (vendorExposure != nullptr)
		{
			renderer.mFrameBuffer->TransitionTexture(*vendorExposure, NRIComputeShaderResourceState());
		}
		renderer.mFrameBuffer->TransitionTexture(vendorOutput, NRIComputeStorageState());

		const nri::UpscalerMode resolvedUpscalerMode = NRIResolveUpscalerModeForMain(mainKind, renderer.GetSelectedUpscalerMode());
		if (!renderer.mUpscaler.EnsureMainUpscaler(*renderer.mFrameBuffer, mainKind, resolvedUpscalerMode, renderer.mOutputWidth, renderer.mOutputHeight, vendorExposure != nullptr))
		{
			return false;
		}

		NRIUpscalerDispatchDesc upscalerDesc = {};
		upscalerDesc.commandBuffer = renderer.mFrameBuffer->mCommandBuffer;
		upscalerDesc.input = &vendorInput;
		upscalerDesc.output = &vendorOutput;
		upscalerDesc.motion = &renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion);
		upscalerDesc.depth = &upscalerDepth;
		upscalerDesc.exposure = vendorExposure;
		upscalerDesc.normalRoughness = &rrGuideNormalRoughness;
		upscalerDesc.diffuseAlbedo = &rrGuideDiffuseAlbedo;
		upscalerDesc.specularAlbedo = &rrGuideSpecularAlbedo;
		upscalerDesc.specularHitDistance = &rrGuideSpecularHitDistance;
		upscalerDesc.currentWidth = renderer.mRenderWidth;
		upscalerDesc.currentHeight = renderer.mRenderHeight;
		Copy2(renderer.mCurrentJitter, upscalerDesc.cameraJitter);
		std::memcpy(upscalerDesc.viewToClipMatrix, renderer.mCurrentViewToClip, sizeof(upscalerDesc.viewToClipMatrix));
		std::memcpy(upscalerDesc.worldToViewMatrix, renderer.mCurrentWorldToView, sizeof(upscalerDesc.worldToViewMatrix));
		upscalerDesc.sharpness = Clamp01((float)nri_sharpness);
		upscalerDesc.resetHistory = renderer.mResetHistory;
		if (!renderer.mUpscaler.DispatchMainUpscaler(*renderer.mFrameBuffer, mainKind, upscalerDesc))
		{
			return false;
		}

		renderer.mUseUpscaledInFinal = true;
		renderer.mUpscaledInputSlot = NRIRenderer::FrameTextureSlot::VendorOutput;
		resolvedInputSlot = NRIRenderer::FrameTextureSlot::VendorOutput;
		renderer.TraceTemporalState("upscale-vendor", mainKind, postSharpenKind, runAppTaa, renderer.mUpscaledInputSlot, vendorSourceSlot);
	}
	else
	{
		renderer.mUseUpscaledInFinal = false;
		renderer.mUpscaledInputSlot = renderer.mHistoryOutputSlot;
		resolvedInputSlot = renderer.mHistoryOutputSlot;
		renderer.TraceTemporalState("upscale-native", mainKind, postSharpenKind, runAppTaa, resolvedInputSlot, renderer.mHistoryOutputSlot);
	}

	if (postSharpenKind == NRIPostSharpenKind::Off)
	{
		return true;
	}

	NRITextureResource& postInput = renderer.GetFrameTexture(resolvedInputSlot);
	NRITextureResource& postOutput = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::PostSharpenOutput);
	renderer.mFrameBuffer->TransitionTexture(postInput, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(postOutput, NRIComputeStorageState());
	if (!renderer.mUpscaler.EnsurePostSharpen(*renderer.mFrameBuffer, postSharpenKind, renderer.mOutputWidth, renderer.mOutputHeight))
	{
		return false;
	}

	NRIUpscalerDispatchDesc postDesc = {};
	postDesc.commandBuffer = renderer.mFrameBuffer->mCommandBuffer;
	postDesc.input = &postInput;
	postDesc.output = &postOutput;
	postDesc.currentWidth = postInput.width;
	postDesc.currentHeight = postInput.height;
	Copy2(renderer.mCurrentJitter, postDesc.cameraJitter);
	postDesc.sharpness = Clamp01((float)nri_sharpness);
	postDesc.resetHistory = renderer.mResetHistory;
	if (!renderer.mUpscaler.DispatchPostSharpen(*renderer.mFrameBuffer, postSharpenKind, postDesc))
	{
		return false;
	}

	renderer.mUseUpscaledInFinal = true;
	renderer.mUpscaledInputSlot = NRIRenderer::FrameTextureSlot::PostSharpenOutput;
	renderer.TraceTemporalState("upscale-post-sharpen", mainKind, postSharpenKind, runAppTaa, renderer.mUpscaledInputSlot, resolvedInputSlot);
	return true;
}



bool NRIPassDispatcher::DispatchFinal(NRIRenderer& renderer)
{
	Clocker clock(NriPTFinal);

	NRITraceSceneConstants constants = {};
	const uint32_t bootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	const bool presentRawTrace = (!nri_ptbootstrap && !renderer.mUseUpscaledInFinal) || bootstrapMode >= 13u;
	Copy3(renderer.mCurrentCameraPos, constants.CameraPos);
	Copy3(renderer.mCurrentCameraForward, constants.CameraForward);
	Copy3(renderer.mCurrentCameraRight, constants.CameraRight);
	Copy3(renderer.mCurrentCameraUp, constants.CameraUp);
	Copy3(renderer.mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(renderer.mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(renderer.mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(renderer.mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = renderer.mRenderWidth;
	constants.RenderHeight = renderer.mRenderHeight;
	constants.DisplayWidth = renderer.mOutputWidth;
	constants.DisplayHeight = renderer.mOutputHeight;
	constants.TanHalfFovX = renderer.mCurrentTanHalfFovX;
	constants.TanHalfFovY = renderer.mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = renderer.mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = renderer.mPreviousTanHalfFovY;
	constants.SceneInstanceCount = renderer.mSceneInstanceBuffer.stride != 0 ? (uint32_t)(renderer.mSceneInstanceBuffer.usedSize / renderer.mSceneInstanceBuffer.stride) : 0u;
	constants.StaticPrimitiveCount = renderer.mBoundStaticPrimitiveCount;
	constants.DynamicPrimitiveCount = renderer.mBoundDynamicPrimitiveCount;
	constants.FrameIndex = renderer.mFrameIndex;
	constants.Flags =
		(renderer.mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(renderer.mUseUpscaledInFinal ? NRI_FLAG_USE_UPSCALED : 0u) |
		(presentRawTrace ? NRI_FLAG_PRESENT_RAW_TRACE : 0u) |
		(renderer.mUseSplitShadowDenoiser ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(renderer.mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(renderer.mDirectionalLightState.enabled && renderer.mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u);
	constants.StaticMaterialCount = renderer.mBoundStaticMaterialCount;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = renderer.mBoundDynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(0u, 0u, renderer.mDirectionalLightState.color);
	constants.RuntimeLightCount = renderer.mBoundRuntimeLightCount;
	constants.ReservedTrace0 = (uint16_t)(int16_t)renderer.mSceneLeft | ((uint32_t)(uint16_t)(int16_t)renderer.mSceneTop << 16);
	constants.ReservedTrace1 = PackDenoiserAux1(0u, renderer.mDirectionalLightState.angularSize);
	Copy3(renderer.mSkyColor, constants.SkyColor);
	Copy3(renderer.mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(renderer.mDirectionalLightState, constants);

	NRITextureResource& history = renderer.GetFrameTexture(renderer.mHistoryOutputSlot);
	NRITextureResource& upscaled = renderer.GetFrameTexture(renderer.mUpscaledInputSlot);
	NRITextureResource& final = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Final);
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredPenumbra), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::DenoisedShadow), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectLighting), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectEmission), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Validation), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideSpecularAlbedo), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideSpecularHitDistance), NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(history, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(upscaled, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	renderer.mFrameInputDescriptors.fill(renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed).shaderView);
	renderer.mFrameInputDescriptors[0] = history.shaderView;
	renderer.mFrameInputDescriptors[1] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion).shaderView;
	renderer.mFrameInputDescriptors[2] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ).shaderView;
	renderer.mFrameInputDescriptors[3] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness).shaderView;
	renderer.mFrameInputDescriptors[4] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness).shaderView;
	renderer.mFrameInputDescriptors[5] = presentRawTrace ? (renderer.mUseUpscaledInFinal ? upscaled.shaderView : renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed).shaderView) : renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed).shaderView;
	renderer.mFrameInputDescriptors[6] = upscaled.shaderView;
	renderer.mFrameInputDescriptors[7] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Validation).shaderView;
	renderer.mFrameInputDescriptors[8] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo).shaderView;
	renderer.mFrameInputDescriptors[9] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideSpecularAlbedo).shaderView;
	renderer.mFrameInputDescriptors[10] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredPenumbra).shaderView;
	renderer.mFrameInputDescriptors[11] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::DenoisedShadow).shaderView;
	renderer.mFrameInputDescriptors[12] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectLighting).shaderView;
	renderer.mFrameInputDescriptors[13] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectEmission).shaderView;
	if (constants.DebugMode == 10)
	{
		renderer.mFrameInputDescriptors[5] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse).shaderView;
	}
	else if (constants.DebugMode == 11)
	{
		renderer.mFrameInputDescriptors[5] = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular).shaderView;
	}
	NRIDescriptorSetManager::UpdateFrameTextureSet(renderer);

	renderer.mOutputDescriptors.fill(final.storageView);
	renderer.mOutputDescriptors[2] = final.storageView;
	NRIDescriptorSetManager::UpdateOutputSet(renderer);

	renderer.mFrameBuffer->mCore.CmdSetPipelineLayout(*renderer.mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *renderer.mPipelineLayout);
	renderer.mFrameBuffer->mCore.CmdSetRootConstants(*renderer.mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	renderer.BindSceneRootDescriptors();
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 0, renderer.mSamplerSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 1, renderer.GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 2, renderer.GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 3, renderer.mFrameTextureSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 4, renderer.mOutputSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetPipeline(*renderer.mFrameBuffer->mCommandBuffer, *renderer.GetPipeline(NRIRenderer::PipelineSlot::Final));
	renderer.mFrameBuffer->mCore.CmdDispatch(*renderer.mFrameBuffer->mCommandBuffer, { GetDispatchSize(renderer.mTargetWidth), GetDispatchSize(renderer.mTargetHeight), 1 });
	return true;
}
