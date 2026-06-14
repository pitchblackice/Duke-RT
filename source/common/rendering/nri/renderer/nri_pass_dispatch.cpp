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

bool NRIPassDispatcher::DispatchBootstrapView(NRIPassDispatchContext& context)
{
	Clocker clock(NriPTBootstrapDispatch);

	if (!context.UpdateReprojectionBuffer())
	{
		return false;
	}

	const uint32_t bootstrapMode = GetBootstrapMode();
	NRITraceSceneConstants constants = {};
	Copy3(context.mCurrentCameraPos, constants.CameraPos);
	Copy3(context.mCurrentCameraForward, constants.CameraForward);
	Copy3(context.mCurrentCameraRight, constants.CameraRight);
	Copy3(context.mCurrentCameraUp, constants.CameraUp);
	Copy3(context.mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(context.mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(context.mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(context.mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = context.mRenderWidth;
	constants.RenderHeight = context.mRenderHeight;
	constants.DisplayWidth = context.mOutputWidth;
	constants.DisplayHeight = context.mOutputHeight;
	constants.TanHalfFovX = context.mCurrentTanHalfFovX;
	constants.TanHalfFovY = context.mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = context.mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = context.mPreviousTanHalfFovY;
	constants.SceneInstanceCount = context.mSceneInstanceBuffer.stride != 0 ? (uint32_t)(context.mSceneInstanceBuffer.usedSize / context.mSceneInstanceBuffer.stride) : 0u;
	constants.StaticPrimitiveCount = context.mBoundStaticPrimitiveCount;
	constants.DynamicPrimitiveCount = context.mBoundDynamicPrimitiveCount;
	constants.FrameIndex = context.mFrameIndex;
	constants.Flags =
		NRI_FLAG_BOOTSTRAP_VIEW |
		(context.mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(context.mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(context.mDirectionalLightState.enabled && context.mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u);
	constants.StaticMaterialCount = context.mBoundStaticMaterialCount;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = context.mBoundDynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(0u, 0u, context.mDirectionalLightState.color);
	constants.ReservedTrace0 = (uint16_t)(int16_t)context.mSceneLeft | ((uint32_t)(uint16_t)(int16_t)context.mSceneTop << 16);
	Copy3(context.mSkyColor, constants.SkyColor);
	Copy3(context.mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(context.mDirectionalLightState, constants);

	NRITextureResource& history = context.GetFrameTexture(context.mHistoryOutputSlot);
	NRITextureResource& upscaled = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed);
	NRITextureResource& final = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Final);
	context.TransitionTexture(history, NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Validation), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular), NRIComputeShaderResourceState());
	context.TransitionTexture(upscaled, NRIComputeShaderResourceState());
	context.TransitionTexture(final, NRIComputeStorageState());

	context.mFrameInputDescriptors.fill(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed).shaderView);
	context.mFrameInputDescriptors[0] = history.shaderView;
	context.mFrameInputDescriptors[1] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion).shaderView;
	context.mFrameInputDescriptors[2] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ).shaderView;
	context.mFrameInputDescriptors[3] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness).shaderView;
	context.mFrameInputDescriptors[4] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness).shaderView;
	context.mFrameInputDescriptors[5] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed).shaderView;
	context.mFrameInputDescriptors[6] = upscaled.shaderView;
	context.mFrameInputDescriptors[7] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Validation).shaderView;
	context.mFrameInputDescriptors[8] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse).shaderView;
	context.mFrameInputDescriptors[9] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular).shaderView;
	context.mFrameInputDescriptors[10] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular).shaderView;
	context.UpdateFrameTextureSet(context.mUpscalerPrepassFrameTextureSet, context.mFrameInputDescriptors);

	context.mOutputDescriptors.fill(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::VendorOutput).storageView);
	context.mOutputDescriptors[2] = final.storageView;
	context.UpdateOutputSet(context.mUpscalerPrepassOutputSet, context.mOutputDescriptors);

	context.mCore->CmdSetPipelineLayout(*context.mCommandBuffer, nri::BindPoint::COMPUTE, *context.mPipelineLayout);
	context.mCore->CmdSetRootConstants(*context.mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	context.BindSceneRootDescriptors();
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 0, context.mSamplerSet, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 1, context.GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 2, context.GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 3, context.mFrameTextureSet, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 4, context.mOutputSet, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetPipeline(*context.mCommandBuffer, *context.GetPipeline(NRIRenderer::PipelineSlot::Final));
	context.mCore->CmdDispatch(*context.mCommandBuffer, { GetDispatchSize(context.mTargetWidth), GetDispatchSize(context.mTargetHeight), 1 });
	return true;
}



bool NRIPassDispatcher::DispatchFrameGraph(NRIPassDispatchContext& context, HWDrawInfo& di, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials, int)
{
	ScopedPtPerfTimer perfTimer(context.mLastPerfShellTraceStats.frameGraphMs);
	Clocker clock(NriPTFrameGraph);

	const int ptDebugMode = (int)GetEffectivePtDebugMode();
	NRIFrameGraphExecutionRequest request = {};
	request.ptDebugMode = ptDebugMode;
	request.denoise = !!nri_denoise;
	request.presentRoute = ResolvePresentRouteInfo((uint32_t)ptDebugMode, !!nri_ptbootstrap);
	return ExecuteNRIFrameGraph(context, di, geometry, materials, request);
}



bool NRIPassDispatcher::DispatchTraceOpaque(NRIPassDispatchContext& context, HWDrawInfo&, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials)
{
	Clocker clock(NriPTTraceOpaque);
	ScopedPtPerfTimer traceOpaqueTimer(context.mLastPerfShellTraceStats.traceOpaqueMs);
	{
		ScopedPtPerfTimer perfTimer(context.mLastPerfShellTraceStats.traceOpaqueReadbackMs);
		NRITraceShaderStatsReadbackInput input = {};
		input.enabled = (bool)nri_ptshaderstats;
		input.boundSceneInstances = &context.mBoundSceneInstances;
		input.staticPrimitiveCount = context.mBoundStaticPrimitiveCount;
		input.dynamicPrimitiveCount = context.mBoundDynamicPrimitiveCount;
		input.persistentVoxelPrimitiveCount = context.mPersistentVoxels.BoundPrimitiveCount();
		input.user = &context;
		input.estimatePersistentVoxelPrimitiveCount = [](void* user, uint32_t primitiveOffset) -> uint32_t
		{
			return static_cast<NRIPassDispatchContext*>(user)->EstimatePersistentVoxelPrimitiveCountForInstanceOffset(primitiveOffset);
		};
		context.mTraceShaderStats.Readback(context.BuildResourceServices(), input, context.mLastPerfTraceShaderStats);
		context.ReadbackAutoExposureStats();
	}

	if (!context.UpdateReprojectionBuffer())
	{
		return false;
	}

	NRITraceSceneConstants constants = {};
	const NRITraceSettings traceSettings = BuildNRITraceSettingsFromCVars();
	const NRIDenoiserSettings denoiserSettings = BuildNRIDenoiserSettingsFromCVars();
	const uint32_t bootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	const NRIMainUpscalerKind resolvedMainUpscaler = context.ResolveMainUpscalerKind(false);
	const nri::UpscalerMode resolvedUpscalerMode = NRIResolveUpscalerModeForMain(resolvedMainUpscaler, context.GetSelectedUpscalerMode());
	const uint32_t jitterPhaseCount = NRIGetTemporalJitterPhaseCount(resolvedMainUpscaler, resolvedUpscalerMode, context.mGuiCaptureActive);
	const bool directSceneTrace = (!nri_ptbootstrap && nri_ptdirectscene) || bootstrapMode == 11u || bootstrapMode == 12u;
	const bool useTemporalJitter =
		!nri_ptbootstrap &&
		!context.mGuiCaptureActive &&
		NRIShouldUseTemporalJitter(resolvedMainUpscaler);
	Copy3(context.mCurrentCameraPos, constants.CameraPos);
	Copy3(context.mCurrentCameraForward, constants.CameraForward);
	Copy3(context.mCurrentCameraRight, constants.CameraRight);
	Copy3(context.mCurrentCameraUp, constants.CameraUp);
	Copy3(context.mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(context.mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(context.mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(context.mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = context.mRenderWidth;
	constants.RenderHeight = context.mRenderHeight;
	constants.DisplayWidth = context.mOutputWidth;
	constants.DisplayHeight = context.mOutputHeight;
	constants.TanHalfFovX = context.mCurrentTanHalfFovX;
	constants.TanHalfFovY = context.mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = context.mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = context.mPreviousTanHalfFovY;
	constants.SceneInstanceCount = context.mSceneInstanceBuffer.stride != 0 ? (uint32_t)(context.mSceneInstanceBuffer.usedSize / context.mSceneInstanceBuffer.stride) : 0u;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.StaticPrimitiveCount = context.mBoundStaticPrimitiveCount;
	constants.FrameIndex = context.mFrameIndex;
	constants.DynamicPrimitiveCount = context.mBoundDynamicPrimitiveCount;
	constants.Flags =
		(context.mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(directSceneTrace ? NRI_FLAG_PRESENT_RAW_TRACE : 0u) |
		(context.mUseSplitShadowDenoiser && !directSceneTrace ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(context.mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(context.mDirectionalLightState.enabled && context.mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u) |
		(nri_ptemissivefastshadow ? NRI_FLAG_FAST_EMISSIVE_SHADOW : 0u) |
		(nri_ptvisiblechunkgate ? NRI_FLAG_GATE_PRIMARY_VISIBLE_CHUNKS : 0u) |
		(ShouldCollectTraceShaderStats() ? NRI_FLAG_TRACE_SHADER_STATS : 0u) |
		(useTemporalJitter ? NRI_FLAG_USE_JITTER : 0u) |
		NRIPackTemporalJitterPhaseCount(jitterPhaseCount);
	constants.StaticMaterialCount = context.mBoundStaticMaterialCount;
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = context.mBoundDynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(
		traceSettings.lightBounceCount,
		traceSettings.mirrorBounceCount,
		context.mDirectionalLightState.color);
	constants.PortalCount = context.mBoundPortalCount;
	constants.RuntimeLightCount = context.mBoundRuntimeLightCount;
	constants.PortalDepth = PackPortalDepthAndAmbientMultipliers(
		traceSettings.portalDepth,
		GetBaseAmbient(),
		GetMetalAmbient());
	constants.ReservedTrace0 = (context.mBoundRuntimeLightTileCountX & 0xffffu) | ((context.mBoundRuntimeLightTileCountY & 0xffffu) << 16u);
	constants.ReservedTrace1 = PackTraceAux1(
		(uint32_t)denoiserSettings.denoiserMode,
		traceSettings.emissiveSampleCount,
		context.mDirectionalLightState.angularSize);
	Copy3(context.mSkyColor, constants.SkyColor);
	Copy3(context.mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(context.mDirectionalLightState, constants);

	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse), NRIComputeStorageState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular), NRIComputeStorageState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredPenumbra), NRIComputeStorageState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectLighting), NRIComputeStorageState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectEmission), NRIComputeStorageState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion), NRIComputeStorageState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ), NRIComputeStorageState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness), NRIComputeStorageState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness), NRIComputeStorageState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::SrInput), NRIComputeStorageState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo), NRIComputeStorageState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideSpecularHitDistance), NRIComputeStorageState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Validation), NRIComputeStorageState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::VendorOutput), NRIComputeStorageState());

	const nri::Descriptor* defaultInput = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed).shaderView;
	context.mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	context.UpdateFrameTextureSet();

	const nri::Descriptor* defaultOutput = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Validation).storageView;
	context.mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	context.mOutputDescriptors[0] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse).storageView;
	context.mOutputDescriptors[3] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion).storageView;
	context.mOutputDescriptors[4] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ).storageView;
	context.mOutputDescriptors[5] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness).storageView;
	context.mOutputDescriptors[6] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness).storageView;
	context.mOutputDescriptors[9] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo).storageView;
	context.mOutputDescriptors[10] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular).storageView;
	context.mOutputDescriptors[11] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideSpecularHitDistance).storageView;
	context.mOutputDescriptors[12] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredPenumbra).storageView;
	context.mOutputDescriptors[13] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectLighting).storageView;
	context.mOutputDescriptors[14] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectEmission).storageView;
	context.UpdateOutputSet();

	context.mCore->CmdSetPipelineLayout(*context.mCommandBuffer, nri::BindPoint::COMPUTE, *context.mPipelineLayout);
	context.mCore->CmdSetRootConstants(*context.mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	context.BindSceneRootDescriptors();
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 0, context.mSamplerSet, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 1, context.GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 2, context.GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 3, context.mFrameTextureSet, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 4, context.mOutputSet, nri::BindPoint::COMPUTE });
	const uint32_t dispatchX = GetDispatchSize(context.mRenderWidth);
	const uint32_t dispatchY = GetDispatchSize(context.mRenderHeight);
	const uint32_t dispatchZ = 1;
	context.mLastPerfShellTraceStats.traceOpaqueDispatchX = dispatchX;
	context.mLastPerfShellTraceStats.traceOpaqueDispatchY = dispatchY;
	context.mLastPerfShellTraceStats.traceOpaqueDispatchZ = dispatchZ;
	{
		ScopedPtPerfTimer perfTimer(context.mLastPerfShellTraceStats.traceOpaqueCommandMs);
		context.mTraceShaderStats.ResetBuffer(context.BuildResourceServices(), ShouldCollectTraceShaderStats());
		context.mCore->CmdSetPipeline(*context.mCommandBuffer, *context.GetPipeline(NRIRenderer::PipelineSlot::TraceOpaque));
		context.mCore->CmdDispatch(*context.mCommandBuffer, { dispatchX, dispatchY, dispatchZ });
	}
	{
		ScopedPtPerfTimer perfTimer(context.mLastPerfShellTraceStats.traceOpaqueStatsCopyMs);
		context.mTraceShaderStats.CopyForReadback(context.BuildResourceServices(), ShouldCollectTraceShaderStats(), (uint64_t)context.mFrameIndex);
	}
	return true;
}



bool NRIPassDispatcher::DispatchDenoiser(NRIPassDispatchContext& context)
{
	Clocker clock(NriPTDenoiser);
	const NRIDenoiserSettings denoiserSettings = BuildNRIDenoiserSettingsFromCVars();

	if (!context.mNrd.EnsureReady(*context.mDevice, context.mRenderWidth, context.mRenderHeight, 1))
	{
		return false;
	}

	context.mNrd.NewFrame();

	NRINrdDispatchDesc desc = {};
	desc.commandBuffer = context.mCommandBuffer;
	desc.motion = &context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion);
	desc.viewZ = &context.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ);
	desc.normalRoughness = &context.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness);
	desc.baseColorMetalness = &context.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness);
	desc.unfilteredDiffuse = &context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse);
	desc.unfilteredSpecular = &context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular);
	desc.unfilteredPenumbra = &context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredPenumbra);
	desc.diffuse = &context.GetFrameTexture(NRIRenderer::FrameTextureSlot::DenoisedDiffuse);
	desc.specular = &context.GetFrameTexture(NRIRenderer::FrameTextureSlot::DenoisedSpecular);
	desc.shadow = &context.GetFrameTexture(NRIRenderer::FrameTextureSlot::DenoisedShadow);
	desc.validation = &context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Validation);
	desc.resourceWidth = context.mRenderWidth;
	desc.resourceHeight = context.mRenderHeight;
	desc.frameIndex = context.mFrameIndex;
	Copy2(context.mCurrentJitter, desc.cameraJitter);
	Copy2(context.mPreviousJitter, desc.cameraJitterPrev);
	std::memcpy(desc.viewToClipMatrix, context.mCurrentViewToClip, sizeof(desc.viewToClipMatrix));
	std::memcpy(desc.viewToClipMatrixPrev, context.mPreviousViewToClip, sizeof(desc.viewToClipMatrixPrev));
	std::memcpy(desc.worldToViewMatrix, context.mCurrentWorldToView, sizeof(desc.worldToViewMatrix));
	std::memcpy(desc.worldToViewMatrixPrev, context.mPreviousWorldToView, sizeof(desc.worldToViewMatrixPrev));
	desc.lightDirection[0] = context.mDirectionalLightState.direction[0];
	desc.lightDirection[1] = context.mDirectionalLightState.direction[1];
	desc.lightDirection[2] = context.mDirectionalLightState.direction[2];
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
	desc.resetHistory = context.mResetHistory;
	desc.enableAntiFirefly = denoiserSettings.enableAntiFirefly;
	desc.enableValidation = denoiserSettings.enableValidation;
	desc.enableSigmaShadow = context.mUseSplitShadowDenoiser;
	return context.mNrd.Denoise(desc);
}



bool NRIPassDispatcher::DispatchComposition(NRIPassDispatchContext& context, NRIRenderer::FrameTextureSlot outputSlot)
{
	Clocker clock(NriPTComposition);

	NRITraceSceneConstants constants = {};
	const NRIDenoiserSettings denoiserSettings = BuildNRIDenoiserSettingsFromCVars();
	Copy3(context.mCurrentCameraPos, constants.CameraPos);
	Copy3(context.mCurrentCameraForward, constants.CameraForward);
	Copy3(context.mCurrentCameraRight, constants.CameraRight);
	Copy3(context.mCurrentCameraUp, constants.CameraUp);
	Copy3(context.mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(context.mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(context.mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(context.mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = context.mRenderWidth;
	constants.RenderHeight = context.mRenderHeight;
	constants.DisplayWidth = context.mOutputWidth;
	constants.DisplayHeight = context.mOutputHeight;
	constants.TanHalfFovX = context.mCurrentTanHalfFovX;
	constants.TanHalfFovY = context.mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = context.mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = context.mPreviousTanHalfFovY;
	constants.FrameIndex = context.mFrameIndex;
	constants.Flags =
		(context.mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(context.mUseSplitShadowDenoiser ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(context.mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(context.mDirectionalLightState.enabled && context.mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u);
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.BootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	constants.BounceCounts = PackTraceBounceCounts(0u, 0u, context.mDirectionalLightState.color);
	constants.RuntimeLightCount = context.mBoundRuntimeLightCount;
	constants.ReservedTrace0 = denoiserSettings.inputSplitMode;
	constants.ReservedTrace1 = PackDenoiserAux1((uint32_t)denoiserSettings.denoiserMode, context.mDirectionalLightState.angularSize);
	Copy3(context.mSkyColor, constants.SkyColor);
	Copy3(context.mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(context.mDirectionalLightState, constants);

	NRITextureResource& diffuse = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse);
	NRITextureResource& specular = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular);
	NRITextureResource& viewZ = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ);
	NRITextureResource& normalRoughness = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness);
	NRITextureResource& baseColorMetalness = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness);
	NRITextureResource& rawShadow = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredPenumbra);
	NRITextureResource& directLighting = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectLighting);
	NRITextureResource& directEmission = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectEmission);
	const NRIRenderer::FrameTextureSlot filteredDiffuseSlot = context.mUseDenoisedCompositionInputs ? NRIRenderer::FrameTextureSlot::DenoisedDiffuse : NRIRenderer::FrameTextureSlot::UnfilteredDiffuse;
	const NRIRenderer::FrameTextureSlot filteredSpecularSlot = context.mUseDenoisedCompositionInputs ? NRIRenderer::FrameTextureSlot::DenoisedSpecular : NRIRenderer::FrameTextureSlot::UnfilteredSpecular;
	const NRIRenderer::FrameTextureSlot filteredShadowSlot = context.mUseDenoisedCompositionInputs ? NRIRenderer::FrameTextureSlot::DenoisedShadow : NRIRenderer::FrameTextureSlot::UnfilteredPenumbra;
	NRITextureResource& filteredDiffuse = context.GetFrameTexture(filteredDiffuseSlot);
	NRITextureResource& filteredSpecular = context.GetFrameTexture(filteredSpecularSlot);
	NRITextureResource& filteredShadow = context.GetFrameTexture(filteredShadowSlot);
	NRITextureResource& composed = context.GetFrameTexture(outputSlot);

	context.TransitionTexture(diffuse, NRIComputeShaderResourceState());
	context.TransitionTexture(specular, NRIComputeShaderResourceState());
	context.TransitionTexture(viewZ, NRIComputeShaderResourceState());
	context.TransitionTexture(normalRoughness, NRIComputeShaderResourceState());
	context.TransitionTexture(baseColorMetalness, NRIComputeShaderResourceState());
	context.TransitionTexture(rawShadow, NRIComputeShaderResourceState());
	context.TransitionTexture(directLighting, NRIComputeShaderResourceState());
	context.TransitionTexture(directEmission, NRIComputeShaderResourceState());
	context.TransitionTexture(filteredDiffuse, NRIComputeShaderResourceState());
	context.TransitionTexture(filteredSpecular, NRIComputeShaderResourceState());
	context.TransitionTexture(filteredShadow, NRIComputeShaderResourceState());
	context.TransitionTexture(composed, NRIComputeStorageState());

	const nri::Descriptor* defaultInput = diffuse.shaderView;
	context.mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	context.mFrameInputDescriptors[2] = viewZ.shaderView;
	context.mFrameInputDescriptors[3] = normalRoughness.shaderView;
	context.mFrameInputDescriptors[4] = baseColorMetalness.shaderView;
	context.mFrameInputDescriptors[5] = diffuse.shaderView;
	context.mFrameInputDescriptors[6] = specular.shaderView;
	context.mFrameInputDescriptors[8] = filteredDiffuse.shaderView;
	context.mFrameInputDescriptors[9] = filteredSpecular.shaderView;
	context.mFrameInputDescriptors[10] = rawShadow.shaderView;
	context.mFrameInputDescriptors[11] = filteredShadow.shaderView;
	context.mFrameInputDescriptors[12] = directLighting.shaderView;
	context.mFrameInputDescriptors[13] = directEmission.shaderView;
	context.UpdateFrameTextureSet(context.mCompositionFrameTextureSet, context.mFrameInputDescriptors);

	const nri::Descriptor* defaultOutput = composed.storageView;
	context.mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	context.mOutputDescriptors[1] = composed.storageView;
	context.UpdateOutputSet(context.mCompositionOutputSet, context.mOutputDescriptors);

	context.mCore->CmdSetPipelineLayout(*context.mCommandBuffer, nri::BindPoint::COMPUTE, *context.mPipelineLayout);
	context.mCore->CmdSetRootConstants(*context.mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	context.BindSceneRootDescriptors();
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 0, context.mSamplerSet, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 1, context.GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 2, context.GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 3, context.mCompositionFrameTextureSet, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 4, context.mCompositionOutputSet, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetPipeline(*context.mCommandBuffer, *context.GetPipeline(NRIRenderer::PipelineSlot::Composition));
	context.mCore->CmdDispatch(*context.mCommandBuffer, { GetDispatchSize(context.mRenderWidth), GetDispatchSize(context.mRenderHeight), 1 });
	return true;
}



bool NRIPassDispatcher::DispatchTraceTransparent(NRIPassDispatchContext& context)
{
	Clocker clock(NriPTComposition);

	NRITextureResource& composed = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed);
	NRITextureResource& transparentOutput = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::TraceTransparentOutput);
	context.CopyTexture(composed, transparentOutput);
	return true;
}



bool NRIPassDispatcher::DispatchUpscalerPrepass(NRIPassDispatchContext& context, NRIMainUpscalerKind mainKind)
{
	if (mainKind == NRIMainUpscalerKind::Off)
	{
		return false;
	}

	const NRIRenderer::FrameTextureSlot vendorInputSlot =
		mainKind == NRIMainUpscalerKind::DLSR ? NRIRenderer::FrameTextureSlot::SrInput :
		NRIRenderer::FrameTextureSlot::RrInput;
	NRITextureResource& vendorInput = context.GetFrameTexture(vendorInputSlot);
	NRITextureResource& upscalerDepth = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UpscalerDepth);
	NRITextureResource& rrGuideDiffuseAlbedo = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo);
	NRITextureResource& rrGuideSpecularAlbedo = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideSpecularAlbedo);
	NRITextureResource& rrGuideSpecularHitDistance = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideSpecularHitDistance);
	NRITextureResource& rrGuideNormalRoughness = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideNormalRoughness);
	const bool useSrPrepass = mainKind == NRIMainUpscalerKind::DLSR;

	// SR consumes the post-transparent composed signal, while RR now arrives with an
	// explicitly prepared noisy RrInput from the frame-graph path above.
	if (useSrPrepass)
	{
		context.CopyTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::TraceTransparentOutput), vendorInput);
	}
	context.TransitionTexture(vendorInput, NRIComputeShaderResourceState());

	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	context.TransitionTexture(upscalerDepth, NRIComputeStorageState());
	if (!useSrPrepass)
	{
		context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
		context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
		context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular), NRIComputeShaderResourceState());
		context.TransitionTexture(rrGuideDiffuseAlbedo, NRIComputeStorageState());
		context.TransitionTexture(rrGuideSpecularAlbedo, NRIComputeStorageState());
		context.TransitionTexture(rrGuideSpecularHitDistance, NRIComputeStorageState());
		context.TransitionTexture(rrGuideNormalRoughness, NRIComputeStorageState());
	}

	const nri::Descriptor* defaultInput = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ).shaderView;
	context.mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	context.mFrameInputDescriptors[2] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ).shaderView;
	if (!useSrPrepass)
	{
		context.mFrameInputDescriptors[3] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness).shaderView;
		context.mFrameInputDescriptors[4] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness).shaderView;
		context.mFrameInputDescriptors[6] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular).shaderView;
	}
	if (!context.UpdateFrameTextureSet(context.mUpscalerPrepassFrameTextureSet, context.mFrameInputDescriptors))
	{
		return false;
	}

	const nri::Descriptor* defaultOutput = upscalerDepth.storageView;
	context.mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	context.mOutputDescriptors[12] = upscalerDepth.storageView;
	if (!useSrPrepass)
	{
		context.mOutputDescriptors[5] = rrGuideNormalRoughness.storageView;
		context.mOutputDescriptors[9] = rrGuideDiffuseAlbedo.storageView;
		context.mOutputDescriptors[10] = rrGuideSpecularAlbedo.storageView;
		context.mOutputDescriptors[11] = rrGuideSpecularHitDistance.storageView;
	}
	if (!context.UpdateOutputSet(context.mUpscalerPrepassOutputSet, context.mOutputDescriptors))
	{
		return false;
	}

	NRITraceSceneConstants constants = {};
	constants.RenderWidth = context.mRenderWidth;
	constants.RenderHeight = context.mRenderHeight;
	constants.DisplayWidth = context.mOutputWidth;
	constants.DisplayHeight = context.mOutputHeight;
	constants.FrameIndex = context.mFrameIndex;
	constants.ReservedTrace0 =
		mainKind == NRIMainUpscalerKind::DLSR ? 1u :
		mainKind == NRIMainUpscalerKind::DLRR ? 2u :
		0u;
	constants.ReservedTrace1 = (uint32_t)GetSelectedNrdDenoiserMode();
	constants.Flags = context.mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u;
	context.mCore->CmdSetPipelineLayout(*context.mCommandBuffer, nri::BindPoint::COMPUTE, *context.mPipelineLayout);
	context.mCore->CmdSetRootConstants(*context.mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	context.BindSceneRootDescriptors();
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 0, context.mSamplerSet, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 1, context.GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 2, context.GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 3, context.mUpscalerPrepassFrameTextureSet, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 4, context.mUpscalerPrepassOutputSet, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetPipeline(*context.mCommandBuffer, *context.GetPipeline(useSrPrepass ? NRIRenderer::PipelineSlot::DlssSrBefore : NRIRenderer::PipelineSlot::DlssBefore));
	context.mCore->CmdDispatch(*context.mCommandBuffer, { GetDispatchSize(context.mRenderWidth), GetDispatchSize(context.mRenderHeight), 1 });
	return true;
}



bool NRIPassDispatcher::DispatchRawPresent(NRIPassDispatchContext& context, NRIRenderer::FrameTextureSlot inputSlot, NRIRenderer::FrameTextureSlot secondarySlot, NRIRenderer::FrameTextureSlot tertiarySlot)
{
	Clocker clock(NriPTRawPresent);

	NRIPresentConstants constants = {};
	ApplyOutputPolicyToPresentConstants(context.mFrameBuffer->GetPathTracingOutputPolicy(), constants);
	constants.DisplayWidth = context.mOutputWidth;
	constants.DisplayHeight = context.mOutputHeight;
	constants.FrameIndex = context.mFrameIndex;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.PackedSceneOrigin = PackPresentSceneOrigin(context.mSceneLeft, context.mSceneTop);
	constants.DenoiserMode = (uint32_t)GetSelectedNrdDenoiserMode();

	NRITextureResource& input = context.GetFrameTexture(inputSlot);
	constants.InputWidth = input.width;
	constants.InputHeight = input.height;
	const bool addSecondary = secondarySlot != NRIRenderer::FrameTextureSlot::Count;
	NRITextureResource& secondary = context.GetFrameTexture(addSecondary ? secondarySlot : inputSlot);
	const bool hasTertiary = tertiarySlot != NRIRenderer::FrameTextureSlot::Count;
	NRITextureResource& tertiary = context.GetFrameTexture(hasTertiary ? tertiarySlot : inputSlot);
	NRITextureResource& final = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Final);
	if (addSecondary)
	{
		constants.Flags |= NRI_FLAG_RAW_PRESENT_ADD_SECONDARY;
	}
	if (context.mUseSplitShadowDenoiser)
	{
		constants.Flags |= NRI_PRESENT_FLAG_SPLIT_SHADOW_DENOISER;
	}

	context.TransitionTexture(input, NRIComputeShaderResourceState());
	context.TransitionTexture(secondary, NRIComputeShaderResourceState());
	context.TransitionTexture(tertiary, NRIComputeShaderResourceState());
	context.TransitionTexture(final, NRIComputeStorageState());

	const nri::Descriptor* inputs[3] = {
		input.shaderView,
		secondary.shaderView,
		tertiary.shaderView
	};
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = context.mRawPresentFrameTextureSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputs;
	inputUpdate.descriptorNum = (uint32_t)std::size(inputs);
	context.mCore->UpdateDescriptorRanges(&inputUpdate, 1);

	const nri::Descriptor* outputs[1] = { final.storageView };
	nri::UpdateDescriptorRangeDesc outputUpdate = {};
	outputUpdate.descriptorSet = context.mRawPresentOutputSet;
	outputUpdate.rangeIndex = 0;
	outputUpdate.descriptors = outputs;
	outputUpdate.descriptorNum = (uint32_t)std::size(outputs);
	context.mCore->UpdateDescriptorRanges(&outputUpdate, 1);

	context.mCore->CmdSetPipelineLayout(*context.mCommandBuffer, nri::BindPoint::COMPUTE, *context.mPresentPipelineLayout);
	context.mCore->CmdSetRootConstants(*context.mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 0, context.mRawPresentFrameTextureSet, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 1, context.mRawPresentOutputSet, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetPipeline(*context.mCommandBuffer, *context.GetPipeline(NRIRenderer::PipelineSlot::RawPresent));
	context.mCore->CmdDispatch(*context.mCommandBuffer, { GetDispatchSize(context.mTargetWidth), GetDispatchSize(context.mTargetHeight), 1 });
	return true;
}



bool NRIPassDispatcher::DispatchFinalPresent(NRIPassDispatchContext& context, NRIRenderer::FrameTextureSlot inputSlot)
{
	Clocker clock(NriPTFinalPresent);

	const NRIPTOutputPolicy outputPolicy = context.mFrameBuffer->GetPathTracingOutputPolicy();
	const NRIMainUpscalerKind resolvedMain = context.ResolveMainUpscalerKind(false);
	const NRIPostSharpenKind resolvedPost = context.ResolvePostSharpenKind(false);
	const NRIRenderer::ExposureRoute exposureRoute = context.ResolveExposureRoute(inputSlot, outputPolicy, resolvedMain, resolvedPost);
	NRIPresentConstants constants = {};
	ApplyOutputPolicyToPresentConstants(outputPolicy, constants);
	ApplyNightVisionStateToPresentConstants(context.mNightVisionState, constants);
	constants.Exposure = exposureRoute.presentExposure;
	const bool finalPresentInputPreExposed = exposureRoute.inputDomain == NRIRenderer::ExposureDomain::PreExposedHDR;
	const bool finalPresentAutoExposureEligible =
		context.mExposure.GetSettings().enabled &&
		exposureRoute.inputDomain == NRIRenderer::ExposureDomain::SceneHDR;
	NRITextureResource* exposureStateTexture = nullptr;
	if (finalPresentAutoExposureEligible)
	{
		NRITextureResource& candidateExposureState = context.mExposure.GetMutableExposureStateTexture(context.mFrameIndex & 1u);
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
	constants.DisplayWidth = context.mOutputWidth;
	constants.DisplayHeight = context.mOutputHeight;
	constants.FrameIndex = context.mFrameIndex;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.PackedSceneOrigin = PackPresentSceneOrigin(context.mSceneLeft, context.mSceneTop);

	NRITextureResource& input = context.GetFrameTexture(inputSlot);
	NRITextureResource& final = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Final);
	constants.InputWidth = input.width;
	constants.InputHeight = input.height;

	context.TransitionTexture(input, NRIComputeShaderResourceState());
	if (exposureStateTextureValid)
	{
		context.TransitionTexture(*exposureStateTexture, NRIComputeShaderResourceState());
	}
	context.TransitionTexture(final, NRIComputeStorageState());

	const nri::Descriptor* inputs[3] = {
		input.shaderView,
		exposureStateTextureValid ? exposureStateTexture->shaderView : input.shaderView,
		input.shaderView
	};
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = context.mFinalPresentFrameTextureSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputs;
	inputUpdate.descriptorNum = (uint32_t)std::size(inputs);
	context.mCore->UpdateDescriptorRanges(&inputUpdate, 1);

	const nri::Descriptor* outputs[1] = { final.storageView };
	nri::UpdateDescriptorRangeDesc outputUpdate = {};
	outputUpdate.descriptorSet = context.mFinalPresentOutputSet;
	outputUpdate.rangeIndex = 0;
	outputUpdate.descriptors = outputs;
	outputUpdate.descriptorNum = (uint32_t)std::size(outputs);
	context.mCore->UpdateDescriptorRanges(&outputUpdate, 1);

	context.mCore->CmdSetPipelineLayout(*context.mCommandBuffer, nri::BindPoint::COMPUTE, *context.mPresentPipelineLayout);
	context.mCore->CmdSetRootConstants(*context.mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 0, context.mFinalPresentFrameTextureSet, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 1, context.mFinalPresentOutputSet, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetPipeline(*context.mCommandBuffer, *context.GetPipeline(NRIRenderer::PipelineSlot::FinalPresent));
	context.mCore->CmdDispatch(*context.mCommandBuffer, { GetDispatchSize(context.mTargetWidth), GetDispatchSize(context.mTargetHeight), 1 });
	return true;
}



bool NRIPassDispatcher::DispatchUpscaleChain(NRIPassDispatchContext& context)
{
	Clocker clock(NriPTUpscale);

	const NRIMainUpscalerKind mainKind = context.ResolveMainUpscalerKind(true);
	const NRIPostSharpenKind postSharpenKind = context.ResolvePostSharpenKind(true);
	const bool runAppTaa = NRIShouldRunAppTaa(mainKind);
	const bool useAppTaaJitter = runAppTaa && !context.mGuiCaptureActive;
	NRITextureResource& composed = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::TraceTransparentOutput);
	const NRIRenderer::FrameTextureSlot vendorSourceSlot =
		mainKind == NRIMainUpscalerKind::DLRR ? NRIRenderer::FrameTextureSlot::RrInput :
		NRIRenderer::FrameTextureSlot::TraceTransparentOutput;
	NRITextureResource& historyInput = context.GetFrameTexture(context.mHistoryInputSlot);
	NRITextureResource& historyOutput = context.GetFrameTexture(context.mHistoryOutputSlot);
	context.TraceTemporalState("upscale-entry", mainKind, postSharpenKind, runAppTaa, context.mHistoryOutputSlot, vendorSourceSlot);

	if (runAppTaa)
	{
		NRITemporalConstants constants = {};
		constants.RenderWidth = context.mRenderWidth;
		constants.RenderHeight = context.mRenderHeight;
		constants.FrameIndex = context.mFrameIndex;
		constants.Flags =
			(context.mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
			(useAppTaaJitter ? NRI_FLAG_USE_JITTER : 0u) |
			NRIPackTemporalJitterPhaseCount(NRIGetTemporalJitterPhaseCount(
				mainKind,
				NRIResolveUpscalerModeForMain(mainKind, context.GetSelectedUpscalerMode()),
				context.mGuiCaptureActive));
		constants.Exposure = GetTemporalExposure(context.mFrameBuffer->GetPathTracingOutputPolicy());
		NRITextureResource* exposureStateTexture = nullptr;
		if (context.mExposure.GetSettings().enabled)
		{
			NRITextureResource& candidateExposureState = context.mExposure.GetMutableExposureStateTexture(context.mFrameIndex & 1u);
			if (candidateExposureState.texture != nullptr)
			{
				exposureStateTexture = &candidateExposureState;
			}
		}
		const bool exposureStateTextureValid =
			exposureStateTexture != nullptr &&
			exposureStateTexture->shaderView != nullptr;
		constants.Flags |=
			(context.mExposure.GetSettings().enabled ? NRI_TEMPORAL_FLAG_AUTO_EXPOSURE : 0u) |
			(exposureStateTextureValid ? NRI_TEMPORAL_FLAG_EXPOSURE_TEXTURE_VALID : 0u);

		context.TransitionTexture(composed, NRIComputeShaderResourceState());
		context.TransitionTexture(historyInput, NRIComputeShaderResourceState());
		context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion), NRIComputeShaderResourceState());
		if (exposureStateTextureValid)
		{
			context.TransitionTexture(*exposureStateTexture, NRIComputeShaderResourceState());
		}
		context.TransitionTexture(historyOutput, NRIComputeStorageState());

		const nri::Descriptor* taaInputs[4] = {
			historyInput.shaderView,
			context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion).shaderView,
			composed.shaderView,
			exposureStateTextureValid ? exposureStateTexture->shaderView : composed.shaderView
		};
		nri::UpdateDescriptorRangeDesc taaInputUpdate = {};
		taaInputUpdate.descriptorSet = context.mTaaFrameTextureSet;
		taaInputUpdate.rangeIndex = 0;
		taaInputUpdate.descriptors = taaInputs;
		taaInputUpdate.descriptorNum = (uint32_t)std::size(taaInputs);
		context.mCore->UpdateDescriptorRanges(&taaInputUpdate, 1);

		const nri::Descriptor* taaOutputs[1] = { historyOutput.storageView };
		nri::UpdateDescriptorRangeDesc taaOutputUpdate = {};
		taaOutputUpdate.descriptorSet = context.mTaaOutputSet;
		taaOutputUpdate.rangeIndex = 0;
		taaOutputUpdate.descriptors = taaOutputs;
		taaOutputUpdate.descriptorNum = (uint32_t)std::size(taaOutputs);
		context.mCore->UpdateDescriptorRanges(&taaOutputUpdate, 1);

		context.mCore->CmdSetPipelineLayout(*context.mCommandBuffer, nri::BindPoint::COMPUTE, *context.mTaaPipelineLayout);
		context.mCore->CmdSetRootConstants(*context.mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
		context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 0, context.mTaaFrameTextureSet, nri::BindPoint::COMPUTE });
		context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 1, context.mTaaOutputSet, nri::BindPoint::COMPUTE });
		context.mCore->CmdSetPipeline(*context.mCommandBuffer, *context.GetPipeline(NRIRenderer::PipelineSlot::Taa));
		context.mCore->CmdDispatch(*context.mCommandBuffer, { GetDispatchSize(context.mRenderWidth), GetDispatchSize(context.mRenderHeight), 1 });
	}
	else if (mainKind == NRIMainUpscalerKind::Off)
	{
		context.CopyTexture(composed, historyOutput);
	}
	else if (mainKind == NRIMainUpscalerKind::DLSR)
	{
		// Keep ptdebug 13 context.meaningful even when app-TAA is intentionally bypassed for vendor SR.
		context.CopyTexture(composed, historyOutput);
	}
	else if (mainKind == NRIMainUpscalerKind::DLRR)
	{
		// Keep ptdebug 13 context.meaningful for RR as well by exposing the explicit noisy RR input.
		context.CopyTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrInput), historyOutput);
	}

	NRIRenderer::FrameTextureSlot resolvedInputSlot = context.mHistoryOutputSlot;

	if (mainKind != NRIMainUpscalerKind::Off)
	{
		const NRIRenderer::FrameTextureSlot vendorInputSlot =
			mainKind == NRIMainUpscalerKind::DLSR ? NRIRenderer::FrameTextureSlot::SrInput :
			NRIRenderer::FrameTextureSlot::RrInput;
		NRITextureResource& vendorInput = context.GetFrameTexture(vendorInputSlot);
		NRITextureResource& upscalerDepth = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UpscalerDepth);
		NRITextureResource& rrGuideDiffuseAlbedo = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo);
		NRITextureResource& rrGuideSpecularAlbedo = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideSpecularAlbedo);
		NRITextureResource& rrGuideSpecularHitDistance = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideSpecularHitDistance);
		NRITextureResource& rrGuideNormalRoughness = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideNormalRoughness);
		NRITextureResource& vendorOutput = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::VendorOutput);
		NRITextureResource* vendorExposure = nullptr;
		if (context.mExposure.GetSettings().enabled)
		{
			NRITextureResource& candidateExposureState = context.mExposure.GetMutableExposureStateTexture(context.mFrameIndex & 1u);
			if (candidateExposureState.texture != nullptr && candidateExposureState.shaderView != nullptr)
			{
				vendorExposure = &candidateExposureState;
			}
		}

		if (!NRIPassDispatcher::DispatchUpscalerPrepass(context, mainKind))
		{
			return false;
		}

		context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion), NRIComputeShaderResourceState());
		context.TransitionTexture(vendorInput, NRIComputeShaderResourceState());
		context.TransitionTexture(upscalerDepth, NRIComputeShaderResourceState());
		context.TransitionTexture(rrGuideDiffuseAlbedo, NRIComputeShaderResourceState());
		context.TransitionTexture(rrGuideSpecularAlbedo, NRIComputeShaderResourceState());
		context.TransitionTexture(rrGuideSpecularHitDistance, NRIComputeShaderResourceState());
		context.TransitionTexture(rrGuideNormalRoughness, NRIComputeShaderResourceState());
		if (vendorExposure != nullptr)
		{
			context.TransitionTexture(*vendorExposure, NRIComputeShaderResourceState());
		}
		context.TransitionTexture(vendorOutput, NRIComputeStorageState());

		const nri::UpscalerMode resolvedUpscalerMode = NRIResolveUpscalerModeForMain(mainKind, context.GetSelectedUpscalerMode());
		if (!context.mUpscaler.EnsureMainUpscaler(*context.mFrameBuffer, mainKind, resolvedUpscalerMode, context.mOutputWidth, context.mOutputHeight, vendorExposure != nullptr))
		{
			return false;
		}

		NRIUpscalerDispatchDesc upscalerDesc = {};
		upscalerDesc.commandBuffer = context.mCommandBuffer;
		upscalerDesc.input = &vendorInput;
		upscalerDesc.output = &vendorOutput;
		upscalerDesc.motion = &context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion);
		upscalerDesc.depth = &upscalerDepth;
		upscalerDesc.exposure = vendorExposure;
		upscalerDesc.normalRoughness = &rrGuideNormalRoughness;
		upscalerDesc.diffuseAlbedo = &rrGuideDiffuseAlbedo;
		upscalerDesc.specularAlbedo = &rrGuideSpecularAlbedo;
		upscalerDesc.specularHitDistance = &rrGuideSpecularHitDistance;
		upscalerDesc.currentWidth = context.mRenderWidth;
		upscalerDesc.currentHeight = context.mRenderHeight;
		Copy2(context.mCurrentJitter, upscalerDesc.cameraJitter);
		std::memcpy(upscalerDesc.viewToClipMatrix, context.mCurrentViewToClip, sizeof(upscalerDesc.viewToClipMatrix));
		std::memcpy(upscalerDesc.worldToViewMatrix, context.mCurrentWorldToView, sizeof(upscalerDesc.worldToViewMatrix));
		upscalerDesc.sharpness = Clamp01((float)nri_sharpness);
		upscalerDesc.resetHistory = context.mResetHistory;
		if (!context.mUpscaler.DispatchMainUpscaler(*context.mFrameBuffer, mainKind, upscalerDesc))
		{
			return false;
		}

		context.mUseUpscaledInFinal = true;
		context.mUpscaledInputSlot = NRIRenderer::FrameTextureSlot::VendorOutput;
		resolvedInputSlot = NRIRenderer::FrameTextureSlot::VendorOutput;
		context.TraceTemporalState("upscale-vendor", mainKind, postSharpenKind, runAppTaa, context.mUpscaledInputSlot, vendorSourceSlot);
	}
	else
	{
		context.mUseUpscaledInFinal = false;
		context.mUpscaledInputSlot = context.mHistoryOutputSlot;
		resolvedInputSlot = context.mHistoryOutputSlot;
		context.TraceTemporalState("upscale-native", mainKind, postSharpenKind, runAppTaa, resolvedInputSlot, context.mHistoryOutputSlot);
	}

	if (postSharpenKind == NRIPostSharpenKind::Off)
	{
		return true;
	}

	NRITextureResource& postInput = context.GetFrameTexture(resolvedInputSlot);
	NRITextureResource& postOutput = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::PostSharpenOutput);
	context.TransitionTexture(postInput, NRIComputeShaderResourceState());
	context.TransitionTexture(postOutput, NRIComputeStorageState());
	if (!context.mUpscaler.EnsurePostSharpen(*context.mFrameBuffer, postSharpenKind, context.mOutputWidth, context.mOutputHeight))
	{
		return false;
	}

	NRIUpscalerDispatchDesc postDesc = {};
	postDesc.commandBuffer = context.mCommandBuffer;
	postDesc.input = &postInput;
	postDesc.output = &postOutput;
	postDesc.currentWidth = postInput.width;
	postDesc.currentHeight = postInput.height;
	Copy2(context.mCurrentJitter, postDesc.cameraJitter);
	postDesc.sharpness = Clamp01((float)nri_sharpness);
	postDesc.resetHistory = context.mResetHistory;
	if (!context.mUpscaler.DispatchPostSharpen(*context.mFrameBuffer, postSharpenKind, postDesc))
	{
		return false;
	}

	context.mUseUpscaledInFinal = true;
	context.mUpscaledInputSlot = NRIRenderer::FrameTextureSlot::PostSharpenOutput;
	context.TraceTemporalState("upscale-post-sharpen", mainKind, postSharpenKind, runAppTaa, context.mUpscaledInputSlot, resolvedInputSlot);
	return true;
}



bool NRIPassDispatcher::DispatchFinal(NRIPassDispatchContext& context)
{
	Clocker clock(NriPTFinal);

	NRITraceSceneConstants constants = {};
	const uint32_t bootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	const bool presentRawTrace = (!nri_ptbootstrap && !context.mUseUpscaledInFinal) || bootstrapMode >= 13u;
	Copy3(context.mCurrentCameraPos, constants.CameraPos);
	Copy3(context.mCurrentCameraForward, constants.CameraForward);
	Copy3(context.mCurrentCameraRight, constants.CameraRight);
	Copy3(context.mCurrentCameraUp, constants.CameraUp);
	Copy3(context.mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(context.mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(context.mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(context.mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = context.mRenderWidth;
	constants.RenderHeight = context.mRenderHeight;
	constants.DisplayWidth = context.mOutputWidth;
	constants.DisplayHeight = context.mOutputHeight;
	constants.TanHalfFovX = context.mCurrentTanHalfFovX;
	constants.TanHalfFovY = context.mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = context.mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = context.mPreviousTanHalfFovY;
	constants.SceneInstanceCount = context.mSceneInstanceBuffer.stride != 0 ? (uint32_t)(context.mSceneInstanceBuffer.usedSize / context.mSceneInstanceBuffer.stride) : 0u;
	constants.StaticPrimitiveCount = context.mBoundStaticPrimitiveCount;
	constants.DynamicPrimitiveCount = context.mBoundDynamicPrimitiveCount;
	constants.FrameIndex = context.mFrameIndex;
	constants.Flags =
		(context.mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(context.mUseUpscaledInFinal ? NRI_FLAG_USE_UPSCALED : 0u) |
		(presentRawTrace ? NRI_FLAG_PRESENT_RAW_TRACE : 0u) |
		(context.mUseSplitShadowDenoiser ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(context.mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(context.mDirectionalLightState.enabled && context.mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u);
	constants.StaticMaterialCount = context.mBoundStaticMaterialCount;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = context.mBoundDynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(0u, 0u, context.mDirectionalLightState.color);
	constants.RuntimeLightCount = context.mBoundRuntimeLightCount;
	constants.ReservedTrace0 = (uint16_t)(int16_t)context.mSceneLeft | ((uint32_t)(uint16_t)(int16_t)context.mSceneTop << 16);
	constants.ReservedTrace1 = PackDenoiserAux1(0u, context.mDirectionalLightState.angularSize);
	Copy3(context.mSkyColor, constants.SkyColor);
	Copy3(context.mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(context.mDirectionalLightState, constants);

	NRITextureResource& history = context.GetFrameTexture(context.mHistoryOutputSlot);
	NRITextureResource& upscaled = context.GetFrameTexture(context.mUpscaledInputSlot);
	NRITextureResource& final = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Final);
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredPenumbra), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::DenoisedShadow), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectLighting), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectEmission), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Validation), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideSpecularAlbedo), NRIComputeShaderResourceState());
	context.TransitionTexture(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideSpecularHitDistance), NRIComputeShaderResourceState());
	context.TransitionTexture(history, NRIComputeShaderResourceState());
	context.TransitionTexture(upscaled, NRIComputeShaderResourceState());
	context.TransitionTexture(final, NRIComputeStorageState());

	context.mFrameInputDescriptors.fill(context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed).shaderView);
	context.mFrameInputDescriptors[0] = history.shaderView;
	context.mFrameInputDescriptors[1] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Motion).shaderView;
	context.mFrameInputDescriptors[2] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::ViewZ).shaderView;
	context.mFrameInputDescriptors[3] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::NormalRoughness).shaderView;
	context.mFrameInputDescriptors[4] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::BaseColorMetalness).shaderView;
	context.mFrameInputDescriptors[5] = presentRawTrace ? (context.mUseUpscaledInFinal ? upscaled.shaderView : context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed).shaderView) : context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Composed).shaderView;
	context.mFrameInputDescriptors[6] = upscaled.shaderView;
	context.mFrameInputDescriptors[7] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::Validation).shaderView;
	context.mFrameInputDescriptors[8] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo).shaderView;
	context.mFrameInputDescriptors[9] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrGuideSpecularAlbedo).shaderView;
	context.mFrameInputDescriptors[10] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredPenumbra).shaderView;
	context.mFrameInputDescriptors[11] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::DenoisedShadow).shaderView;
	context.mFrameInputDescriptors[12] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectLighting).shaderView;
	context.mFrameInputDescriptors[13] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::DirectEmission).shaderView;
	if (constants.DebugMode == 10)
	{
		context.mFrameInputDescriptors[5] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse).shaderView;
	}
	else if (constants.DebugMode == 11)
	{
		context.mFrameInputDescriptors[5] = context.GetFrameTexture(NRIRenderer::FrameTextureSlot::UnfilteredSpecular).shaderView;
	}
	context.UpdateFrameTextureSet();

	context.mOutputDescriptors.fill(final.storageView);
	context.mOutputDescriptors[2] = final.storageView;
	context.UpdateOutputSet();

	context.mCore->CmdSetPipelineLayout(*context.mCommandBuffer, nri::BindPoint::COMPUTE, *context.mPipelineLayout);
	context.mCore->CmdSetRootConstants(*context.mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	context.BindSceneRootDescriptors();
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 0, context.mSamplerSet, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 1, context.GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 2, context.GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 3, context.mFrameTextureSet, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetDescriptorSet(*context.mCommandBuffer, { 4, context.mOutputSet, nri::BindPoint::COMPUTE });
	context.mCore->CmdSetPipeline(*context.mCommandBuffer, *context.GetPipeline(NRIRenderer::PipelineSlot::Final));
	context.mCore->CmdDispatch(*context.mCommandBuffer, { GetDispatchSize(context.mTargetWidth), GetDispatchSize(context.mTargetHeight), 1 });
	return true;
}
