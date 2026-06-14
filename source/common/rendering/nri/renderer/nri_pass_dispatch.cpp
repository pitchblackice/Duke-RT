#include "nri_renderer.h"

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

bool NRIRenderer::DispatchBootstrapView()
{
	Clocker clock(NriPTBootstrapDispatch);

	if (!NRISceneUploadManager::UpdateReprojectionBuffer(*this, nullptr))
	{
		return false;
	}

	const uint32_t bootstrapMode = GetBootstrapMode();
	NRITraceSceneConstants constants = {};
	Copy3(mCurrentCameraPos, constants.CameraPos);
	Copy3(mCurrentCameraForward, constants.CameraForward);
	Copy3(mCurrentCameraRight, constants.CameraRight);
	Copy3(mCurrentCameraUp, constants.CameraUp);
	Copy3(mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = mRenderWidth;
	constants.RenderHeight = mRenderHeight;
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.TanHalfFovX = mCurrentTanHalfFovX;
	constants.TanHalfFovY = mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = mPreviousTanHalfFovY;
	constants.SceneInstanceCount = mSceneInstanceBuffer.stride != 0 ? (uint32_t)(mSceneInstanceBuffer.usedSize / mSceneInstanceBuffer.stride) : 0u;
	constants.StaticPrimitiveCount = mBoundStaticPrimitiveCount;
	constants.DynamicPrimitiveCount = mBoundDynamicPrimitiveCount;
	constants.FrameIndex = mFrameIndex;
	constants.Flags =
		NRI_FLAG_BOOTSTRAP_VIEW |
		(mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(mDirectionalLightState.enabled && mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u);
	constants.StaticMaterialCount = mBoundStaticMaterialCount;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = mBoundDynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(0u, 0u, mDirectionalLightState.color);
	constants.ReservedTrace0 = (uint16_t)(int16_t)mSceneLeft | ((uint32_t)(uint16_t)(int16_t)mSceneTop << 16);
	Copy3(mSkyColor, constants.SkyColor);
	Copy3(mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(mDirectionalLightState, constants);

	NRITextureResource& history = GetFrameTexture(mHistoryOutputSlot);
	NRITextureResource& upscaled = GetFrameTexture(FrameTextureSlot::Composed);
	NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	mFrameBuffer->TransitionTexture(history, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Validation), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredSpecular), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(upscaled, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	mFrameInputDescriptors.fill(GetFrameTexture(FrameTextureSlot::Composed).shaderView);
	mFrameInputDescriptors[0] = history.shaderView;
	mFrameInputDescriptors[1] = GetFrameTexture(FrameTextureSlot::Motion).shaderView;
	mFrameInputDescriptors[2] = GetFrameTexture(FrameTextureSlot::ViewZ).shaderView;
	mFrameInputDescriptors[3] = GetFrameTexture(FrameTextureSlot::NormalRoughness).shaderView;
	mFrameInputDescriptors[4] = GetFrameTexture(FrameTextureSlot::BaseColorMetalness).shaderView;
	mFrameInputDescriptors[5] = GetFrameTexture(FrameTextureSlot::Composed).shaderView;
	mFrameInputDescriptors[6] = upscaled.shaderView;
	mFrameInputDescriptors[7] = GetFrameTexture(FrameTextureSlot::Validation).shaderView;
	mFrameInputDescriptors[8] = GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse).shaderView;
	mFrameInputDescriptors[9] = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular).shaderView;
	mFrameInputDescriptors[10] = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular).shaderView;
	NRIDescriptorSetManager::UpdateFrameTextureSet(*this, mUpscalerPrepassFrameTextureSet, mFrameInputDescriptors);

	mOutputDescriptors.fill(GetFrameTexture(FrameTextureSlot::VendorOutput).storageView);
	mOutputDescriptors[2] = final.storageView;
	NRIDescriptorSetManager::UpdateOutputSet(*this, mUpscalerPrepassOutputSet, mOutputDescriptors);

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	BindSceneRootDescriptors();
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mSamplerSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 2, GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 3, mFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 4, mOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::Final));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mTargetWidth), GetDispatchSize(mTargetHeight), 1 });
	return true;
}



bool NRIRenderer::DispatchFrameGraph(HWDrawInfo& di, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials, int)
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.frameGraphMs);
	Clocker clock(NriPTFrameGraph);

	const int ptDebugMode = (int)GetEffectivePtDebugMode();
	NRIFrameGraphExecutionRequest request = {};
	request.ptDebugMode = ptDebugMode;
	request.denoise = !!nri_denoise;
	request.presentRoute = ResolvePresentRouteInfo((uint32_t)ptDebugMode, !!nri_ptbootstrap);
	return ExecuteNRIFrameGraph(*this, di, geometry, materials, request);
}



bool NRIRenderer::DispatchTraceOpaque(HWDrawInfo&, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials)
{
	Clocker clock(NriPTTraceOpaque);
	ScopedPtPerfTimer traceOpaqueTimer(mLastPerfShellTraceStats.traceOpaqueMs);
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.traceOpaqueReadbackMs);
		NRITraceShaderStatsReadbackInput input = {};
		input.enabled = (bool)nri_ptshaderstats;
		input.boundSceneInstances = &mBoundSceneInstances;
		input.staticPrimitiveCount = mBoundStaticPrimitiveCount;
		input.dynamicPrimitiveCount = mBoundDynamicPrimitiveCount;
		input.persistentVoxelPrimitiveCount = mPersistentVoxels.BoundPrimitiveCount();
		input.user = this;
		input.estimatePersistentVoxelPrimitiveCount = [](void* user, uint32_t primitiveOffset) -> uint32_t
		{
			return static_cast<NRIRenderer*>(user)->mPersistentVoxels.EstimatePrimitiveCountForInstanceOffset(primitiveOffset);
		};
		mTraceShaderStats.Readback(BuildResourceServices(), input, mLastPerfTraceShaderStats);
		ReadbackAutoExposureStats();
	}

	if (!NRISceneUploadManager::UpdateReprojectionBuffer(*this, nullptr))
	{
		return false;
	}

	NRITraceSceneConstants constants = {};
	const NRITraceSettings traceSettings = BuildNRITraceSettingsFromCVars();
	const NRIDenoiserSettings denoiserSettings = BuildNRIDenoiserSettingsFromCVars();
	const uint32_t bootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	const NRIMainUpscalerKind resolvedMainUpscaler = ResolveMainUpscalerKind(false);
	const nri::UpscalerMode resolvedUpscalerMode = NRIResolveUpscalerModeForMain(resolvedMainUpscaler, GetSelectedUpscalerMode());
	const uint32_t jitterPhaseCount = NRIGetTemporalJitterPhaseCount(resolvedMainUpscaler, resolvedUpscalerMode, mGuiCaptureActive);
	const bool directSceneTrace = (!nri_ptbootstrap && nri_ptdirectscene) || bootstrapMode == 11u || bootstrapMode == 12u;
	const bool useTemporalJitter =
		!nri_ptbootstrap &&
		!mGuiCaptureActive &&
		NRIShouldUseTemporalJitter(resolvedMainUpscaler);
	Copy3(mCurrentCameraPos, constants.CameraPos);
	Copy3(mCurrentCameraForward, constants.CameraForward);
	Copy3(mCurrentCameraRight, constants.CameraRight);
	Copy3(mCurrentCameraUp, constants.CameraUp);
	Copy3(mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = mRenderWidth;
	constants.RenderHeight = mRenderHeight;
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.TanHalfFovX = mCurrentTanHalfFovX;
	constants.TanHalfFovY = mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = mPreviousTanHalfFovY;
	constants.SceneInstanceCount = mSceneInstanceBuffer.stride != 0 ? (uint32_t)(mSceneInstanceBuffer.usedSize / mSceneInstanceBuffer.stride) : 0u;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.StaticPrimitiveCount = mBoundStaticPrimitiveCount;
	constants.FrameIndex = mFrameIndex;
	constants.DynamicPrimitiveCount = mBoundDynamicPrimitiveCount;
	constants.Flags =
		(mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(directSceneTrace ? NRI_FLAG_PRESENT_RAW_TRACE : 0u) |
		(mUseSplitShadowDenoiser && !directSceneTrace ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(mDirectionalLightState.enabled && mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u) |
		(nri_ptemissivefastshadow ? NRI_FLAG_FAST_EMISSIVE_SHADOW : 0u) |
		(nri_ptvisiblechunkgate ? NRI_FLAG_GATE_PRIMARY_VISIBLE_CHUNKS : 0u) |
		(ShouldCollectTraceShaderStats() ? NRI_FLAG_TRACE_SHADER_STATS : 0u) |
		(useTemporalJitter ? NRI_FLAG_USE_JITTER : 0u) |
		NRIPackTemporalJitterPhaseCount(jitterPhaseCount);
	constants.StaticMaterialCount = mBoundStaticMaterialCount;
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = mBoundDynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(
		traceSettings.lightBounceCount,
		traceSettings.mirrorBounceCount,
		mDirectionalLightState.color);
	constants.PortalCount = mBoundPortalCount;
	constants.RuntimeLightCount = mBoundRuntimeLightCount;
	constants.PortalDepth = PackPortalDepthAndAmbientMultipliers(
		traceSettings.portalDepth,
		GetBaseAmbient(),
		GetMetalAmbient());
	constants.ReservedTrace0 = (mBoundRuntimeLightTileCountX & 0xffffu) | ((mBoundRuntimeLightTileCountY & 0xffffu) << 16u);
	constants.ReservedTrace1 = PackTraceAux1(
		(uint32_t)denoiserSettings.denoiserMode,
		traceSettings.emissiveSampleCount,
		mDirectionalLightState.angularSize);
	Copy3(mSkyColor, constants.SkyColor);
	Copy3(mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(mDirectionalLightState, constants);

	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredSpecular), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DirectLighting), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DirectEmission), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::ViewZ), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::NormalRoughness), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::BaseColorMetalness), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::SrInput), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::RrGuideSpecularHitDistance), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Validation), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::VendorOutput), NRIComputeStorageState());

	const nri::Descriptor* defaultInput = GetFrameTexture(FrameTextureSlot::Composed).shaderView;
	mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	NRIDescriptorSetManager::UpdateFrameTextureSet(*this);

	const nri::Descriptor* defaultOutput = GetFrameTexture(FrameTextureSlot::Validation).storageView;
	mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	mOutputDescriptors[0] = GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse).storageView;
	mOutputDescriptors[3] = GetFrameTexture(FrameTextureSlot::Motion).storageView;
	mOutputDescriptors[4] = GetFrameTexture(FrameTextureSlot::ViewZ).storageView;
	mOutputDescriptors[5] = GetFrameTexture(FrameTextureSlot::NormalRoughness).storageView;
	mOutputDescriptors[6] = GetFrameTexture(FrameTextureSlot::BaseColorMetalness).storageView;
	mOutputDescriptors[9] = GetFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo).storageView;
	mOutputDescriptors[10] = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular).storageView;
	mOutputDescriptors[11] = GetFrameTexture(FrameTextureSlot::RrGuideSpecularHitDistance).storageView;
	mOutputDescriptors[12] = GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra).storageView;
	mOutputDescriptors[13] = GetFrameTexture(FrameTextureSlot::DirectLighting).storageView;
	mOutputDescriptors[14] = GetFrameTexture(FrameTextureSlot::DirectEmission).storageView;
	NRIDescriptorSetManager::UpdateOutputSet(*this);

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	BindSceneRootDescriptors();
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mSamplerSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 2, GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 3, mFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 4, mOutputSet, nri::BindPoint::COMPUTE });
	const uint32_t dispatchX = GetDispatchSize(mRenderWidth);
	const uint32_t dispatchY = GetDispatchSize(mRenderHeight);
	const uint32_t dispatchZ = 1;
	mLastPerfShellTraceStats.traceOpaqueDispatchX = dispatchX;
	mLastPerfShellTraceStats.traceOpaqueDispatchY = dispatchY;
	mLastPerfShellTraceStats.traceOpaqueDispatchZ = dispatchZ;
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.traceOpaqueCommandMs);
		mTraceShaderStats.ResetBuffer(BuildResourceServices(), ShouldCollectTraceShaderStats());
		mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::TraceOpaque));
		mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { dispatchX, dispatchY, dispatchZ });
	}
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.traceOpaqueStatsCopyMs);
		mTraceShaderStats.CopyForReadback(BuildResourceServices(), ShouldCollectTraceShaderStats(), (uint64_t)mFrameIndex);
	}
	return true;
}



bool NRIRenderer::DispatchDenoiser()
{
	Clocker clock(NriPTDenoiser);
	const NRIDenoiserSettings denoiserSettings = BuildNRIDenoiserSettingsFromCVars();

	if (!mNrd.EnsureReady(*mFrameBuffer->mDevice, mRenderWidth, mRenderHeight, 1))
	{
		return false;
	}

	mNrd.NewFrame();

	NRINrdDispatchDesc desc = {};
	desc.commandBuffer = mFrameBuffer->mCommandBuffer;
	desc.motion = &GetFrameTexture(FrameTextureSlot::Motion);
	desc.viewZ = &GetFrameTexture(FrameTextureSlot::ViewZ);
	desc.normalRoughness = &GetFrameTexture(FrameTextureSlot::NormalRoughness);
	desc.baseColorMetalness = &GetFrameTexture(FrameTextureSlot::BaseColorMetalness);
	desc.unfilteredDiffuse = &GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse);
	desc.unfilteredSpecular = &GetFrameTexture(FrameTextureSlot::UnfilteredSpecular);
	desc.unfilteredPenumbra = &GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra);
	desc.diffuse = &GetFrameTexture(FrameTextureSlot::DenoisedDiffuse);
	desc.specular = &GetFrameTexture(FrameTextureSlot::DenoisedSpecular);
	desc.shadow = &GetFrameTexture(FrameTextureSlot::DenoisedShadow);
	desc.validation = &GetFrameTexture(FrameTextureSlot::Validation);
	desc.resourceWidth = mRenderWidth;
	desc.resourceHeight = mRenderHeight;
	desc.frameIndex = mFrameIndex;
	Copy2(mCurrentJitter, desc.cameraJitter);
	Copy2(mPreviousJitter, desc.cameraJitterPrev);
	std::memcpy(desc.viewToClipMatrix, mCurrentViewToClip, sizeof(desc.viewToClipMatrix));
	std::memcpy(desc.viewToClipMatrixPrev, mPreviousViewToClip, sizeof(desc.viewToClipMatrixPrev));
	std::memcpy(desc.worldToViewMatrix, mCurrentWorldToView, sizeof(desc.worldToViewMatrix));
	std::memcpy(desc.worldToViewMatrixPrev, mPreviousWorldToView, sizeof(desc.worldToViewMatrixPrev));
	desc.lightDirection[0] = mDirectionalLightState.direction[0];
	desc.lightDirection[1] = mDirectionalLightState.direction[1];
	desc.lightDirection[2] = mDirectionalLightState.direction[2];
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
	desc.resetHistory = mResetHistory;
	desc.enableAntiFirefly = denoiserSettings.enableAntiFirefly;
	desc.enableValidation = denoiserSettings.enableValidation;
	desc.enableSigmaShadow = mUseSplitShadowDenoiser;
	return mNrd.Denoise(desc);
}



bool NRIRenderer::DispatchComposition(FrameTextureSlot outputSlot)
{
	Clocker clock(NriPTComposition);

	NRITraceSceneConstants constants = {};
	const NRIDenoiserSettings denoiserSettings = BuildNRIDenoiserSettingsFromCVars();
	Copy3(mCurrentCameraPos, constants.CameraPos);
	Copy3(mCurrentCameraForward, constants.CameraForward);
	Copy3(mCurrentCameraRight, constants.CameraRight);
	Copy3(mCurrentCameraUp, constants.CameraUp);
	Copy3(mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = mRenderWidth;
	constants.RenderHeight = mRenderHeight;
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.TanHalfFovX = mCurrentTanHalfFovX;
	constants.TanHalfFovY = mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = mPreviousTanHalfFovY;
	constants.FrameIndex = mFrameIndex;
	constants.Flags =
		(mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(mUseSplitShadowDenoiser ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(mDirectionalLightState.enabled && mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u);
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.BootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	constants.BounceCounts = PackTraceBounceCounts(0u, 0u, mDirectionalLightState.color);
	constants.RuntimeLightCount = mBoundRuntimeLightCount;
	constants.ReservedTrace0 = denoiserSettings.inputSplitMode;
	constants.ReservedTrace1 = PackDenoiserAux1((uint32_t)denoiserSettings.denoiserMode, mDirectionalLightState.angularSize);
	Copy3(mSkyColor, constants.SkyColor);
	Copy3(mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(mDirectionalLightState, constants);

	NRITextureResource& diffuse = GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse);
	NRITextureResource& specular = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular);
	NRITextureResource& viewZ = GetFrameTexture(FrameTextureSlot::ViewZ);
	NRITextureResource& normalRoughness = GetFrameTexture(FrameTextureSlot::NormalRoughness);
	NRITextureResource& baseColorMetalness = GetFrameTexture(FrameTextureSlot::BaseColorMetalness);
	NRITextureResource& rawShadow = GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra);
	NRITextureResource& directLighting = GetFrameTexture(FrameTextureSlot::DirectLighting);
	NRITextureResource& directEmission = GetFrameTexture(FrameTextureSlot::DirectEmission);
	const FrameTextureSlot filteredDiffuseSlot = mUseDenoisedCompositionInputs ? FrameTextureSlot::DenoisedDiffuse : FrameTextureSlot::UnfilteredDiffuse;
	const FrameTextureSlot filteredSpecularSlot = mUseDenoisedCompositionInputs ? FrameTextureSlot::DenoisedSpecular : FrameTextureSlot::UnfilteredSpecular;
	const FrameTextureSlot filteredShadowSlot = mUseDenoisedCompositionInputs ? FrameTextureSlot::DenoisedShadow : FrameTextureSlot::UnfilteredPenumbra;
	NRITextureResource& filteredDiffuse = GetFrameTexture(filteredDiffuseSlot);
	NRITextureResource& filteredSpecular = GetFrameTexture(filteredSpecularSlot);
	NRITextureResource& filteredShadow = GetFrameTexture(filteredShadowSlot);
	NRITextureResource& composed = GetFrameTexture(outputSlot);

	mFrameBuffer->TransitionTexture(diffuse, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(specular, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(viewZ, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(normalRoughness, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(baseColorMetalness, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(rawShadow, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(directLighting, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(directEmission, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(filteredDiffuse, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(filteredSpecular, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(filteredShadow, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(composed, NRIComputeStorageState());

	const nri::Descriptor* defaultInput = diffuse.shaderView;
	mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	mFrameInputDescriptors[2] = viewZ.shaderView;
	mFrameInputDescriptors[3] = normalRoughness.shaderView;
	mFrameInputDescriptors[4] = baseColorMetalness.shaderView;
	mFrameInputDescriptors[5] = diffuse.shaderView;
	mFrameInputDescriptors[6] = specular.shaderView;
	mFrameInputDescriptors[8] = filteredDiffuse.shaderView;
	mFrameInputDescriptors[9] = filteredSpecular.shaderView;
	mFrameInputDescriptors[10] = rawShadow.shaderView;
	mFrameInputDescriptors[11] = filteredShadow.shaderView;
	mFrameInputDescriptors[12] = directLighting.shaderView;
	mFrameInputDescriptors[13] = directEmission.shaderView;
	NRIDescriptorSetManager::UpdateFrameTextureSet(*this, mCompositionFrameTextureSet, mFrameInputDescriptors);

	const nri::Descriptor* defaultOutput = composed.storageView;
	mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	mOutputDescriptors[1] = composed.storageView;
	NRIDescriptorSetManager::UpdateOutputSet(*this, mCompositionOutputSet, mOutputDescriptors);

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	BindSceneRootDescriptors();
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mSamplerSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 2, GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 3, mCompositionFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 4, mCompositionOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::Composition));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mRenderWidth), GetDispatchSize(mRenderHeight), 1 });
	return true;
}



bool NRIRenderer::DispatchTraceTransparent()
{
	Clocker clock(NriPTComposition);

	NRITextureResource& composed = GetFrameTexture(FrameTextureSlot::Composed);
	NRITextureResource& transparentOutput = GetFrameTexture(FrameTextureSlot::TraceTransparentOutput);
	CopyTexture(composed, transparentOutput);
	return true;
}



bool NRIRenderer::DispatchUpscalerPrepass(NRIMainUpscalerKind mainKind)
{
	if (mainKind == NRIMainUpscalerKind::Off)
	{
		return false;
	}

	const FrameTextureSlot vendorInputSlot =
		mainKind == NRIMainUpscalerKind::DLSR ? FrameTextureSlot::SrInput :
		FrameTextureSlot::RrInput;
	NRITextureResource& vendorInput = GetFrameTexture(vendorInputSlot);
	NRITextureResource& upscalerDepth = GetFrameTexture(FrameTextureSlot::UpscalerDepth);
	NRITextureResource& rrGuideDiffuseAlbedo = GetFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo);
	NRITextureResource& rrGuideSpecularAlbedo = GetFrameTexture(FrameTextureSlot::RrGuideSpecularAlbedo);
	NRITextureResource& rrGuideSpecularHitDistance = GetFrameTexture(FrameTextureSlot::RrGuideSpecularHitDistance);
	NRITextureResource& rrGuideNormalRoughness = GetFrameTexture(FrameTextureSlot::RrGuideNormalRoughness);
	const bool useSrPrepass = mainKind == NRIMainUpscalerKind::DLSR;

	// SR consumes the post-transparent composed signal, while RR now arrives with an
	// explicitly prepared noisy RrInput from the frame-graph path above.
	if (useSrPrepass)
	{
		CopyTexture(GetFrameTexture(FrameTextureSlot::TraceTransparentOutput), vendorInput);
	}
	mFrameBuffer->TransitionTexture(vendorInput, NRIComputeShaderResourceState());

	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(upscalerDepth, NRIComputeStorageState());
	if (!useSrPrepass)
	{
		mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredSpecular), NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(rrGuideDiffuseAlbedo, NRIComputeStorageState());
		mFrameBuffer->TransitionTexture(rrGuideSpecularAlbedo, NRIComputeStorageState());
		mFrameBuffer->TransitionTexture(rrGuideSpecularHitDistance, NRIComputeStorageState());
		mFrameBuffer->TransitionTexture(rrGuideNormalRoughness, NRIComputeStorageState());
	}

	const nri::Descriptor* defaultInput = GetFrameTexture(FrameTextureSlot::ViewZ).shaderView;
	mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	mFrameInputDescriptors[2] = GetFrameTexture(FrameTextureSlot::ViewZ).shaderView;
	if (!useSrPrepass)
	{
		mFrameInputDescriptors[3] = GetFrameTexture(FrameTextureSlot::NormalRoughness).shaderView;
		mFrameInputDescriptors[4] = GetFrameTexture(FrameTextureSlot::BaseColorMetalness).shaderView;
		mFrameInputDescriptors[6] = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular).shaderView;
	}
	if (!NRIDescriptorSetManager::UpdateFrameTextureSet(*this, mUpscalerPrepassFrameTextureSet, mFrameInputDescriptors))
	{
		return false;
	}

	const nri::Descriptor* defaultOutput = upscalerDepth.storageView;
	mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	mOutputDescriptors[12] = upscalerDepth.storageView;
	if (!useSrPrepass)
	{
		mOutputDescriptors[5] = rrGuideNormalRoughness.storageView;
		mOutputDescriptors[9] = rrGuideDiffuseAlbedo.storageView;
		mOutputDescriptors[10] = rrGuideSpecularAlbedo.storageView;
		mOutputDescriptors[11] = rrGuideSpecularHitDistance.storageView;
	}
	if (!NRIDescriptorSetManager::UpdateOutputSet(*this, mUpscalerPrepassOutputSet, mOutputDescriptors))
	{
		return false;
	}

	NRITraceSceneConstants constants = {};
	constants.RenderWidth = mRenderWidth;
	constants.RenderHeight = mRenderHeight;
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.FrameIndex = mFrameIndex;
	constants.ReservedTrace0 =
		mainKind == NRIMainUpscalerKind::DLSR ? 1u :
		mainKind == NRIMainUpscalerKind::DLRR ? 2u :
		0u;
	constants.ReservedTrace1 = (uint32_t)GetSelectedNrdDenoiserMode();
	constants.Flags = mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u;
	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	BindSceneRootDescriptors();
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mSamplerSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 2, GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 3, mUpscalerPrepassFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 4, mUpscalerPrepassOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(useSrPrepass ? PipelineSlot::DlssSrBefore : PipelineSlot::DlssBefore));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mRenderWidth), GetDispatchSize(mRenderHeight), 1 });
	return true;
}



bool NRIRenderer::DispatchRawPresent(FrameTextureSlot inputSlot, FrameTextureSlot secondarySlot, FrameTextureSlot tertiarySlot)
{
	Clocker clock(NriPTRawPresent);

	NRIPresentConstants constants = {};
	ApplyOutputPolicyToPresentConstants(mFrameBuffer->GetPathTracingOutputPolicy(), constants);
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.FrameIndex = mFrameIndex;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.PackedSceneOrigin = PackPresentSceneOrigin(mSceneLeft, mSceneTop);
	constants.DenoiserMode = (uint32_t)GetSelectedNrdDenoiserMode();

	NRITextureResource& input = GetFrameTexture(inputSlot);
	constants.InputWidth = input.width;
	constants.InputHeight = input.height;
	const bool addSecondary = secondarySlot != FrameTextureSlot::Count;
	NRITextureResource& secondary = GetFrameTexture(addSecondary ? secondarySlot : inputSlot);
	const bool hasTertiary = tertiarySlot != FrameTextureSlot::Count;
	NRITextureResource& tertiary = GetFrameTexture(hasTertiary ? tertiarySlot : inputSlot);
	NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	if (addSecondary)
	{
		constants.Flags |= NRI_FLAG_RAW_PRESENT_ADD_SECONDARY;
	}
	if (mUseSplitShadowDenoiser)
	{
		constants.Flags |= NRI_PRESENT_FLAG_SPLIT_SHADOW_DENOISER;
	}

	mFrameBuffer->TransitionTexture(input, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(secondary, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(tertiary, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	const nri::Descriptor* inputs[3] = {
		input.shaderView,
		secondary.shaderView,
		tertiary.shaderView
	};
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = mRawPresentFrameTextureSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputs;
	inputUpdate.descriptorNum = (uint32_t)std::size(inputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&inputUpdate, 1);

	const nri::Descriptor* outputs[1] = { final.storageView };
	nri::UpdateDescriptorRangeDesc outputUpdate = {};
	outputUpdate.descriptorSet = mRawPresentOutputSet;
	outputUpdate.rangeIndex = 0;
	outputUpdate.descriptors = outputs;
	outputUpdate.descriptorNum = (uint32_t)std::size(outputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&outputUpdate, 1);

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPresentPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mRawPresentFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mRawPresentOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::RawPresent));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mTargetWidth), GetDispatchSize(mTargetHeight), 1 });
	return true;
}



bool NRIRenderer::DispatchFinalPresent(FrameTextureSlot inputSlot)
{
	Clocker clock(NriPTFinalPresent);

	const NRIPTOutputPolicy outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	const NRIMainUpscalerKind resolvedMain = ResolveMainUpscalerKind(false);
	const NRIPostSharpenKind resolvedPost = ResolvePostSharpenKind(false);
	const ExposureRoute exposureRoute = ResolveExposureRoute(inputSlot, outputPolicy, resolvedMain, resolvedPost);
	NRIPresentConstants constants = {};
	ApplyOutputPolicyToPresentConstants(outputPolicy, constants);
	ApplyNightVisionStateToPresentConstants(mNightVisionState, constants);
	constants.Exposure = exposureRoute.presentExposure;
	const bool finalPresentInputPreExposed = exposureRoute.inputDomain == ExposureDomain::PreExposedHDR;
	const bool finalPresentAutoExposureEligible =
		mExposure.GetSettings().enabled &&
		exposureRoute.inputDomain == ExposureDomain::SceneHDR;
	NRITextureResource* exposureStateTexture = nullptr;
	if (finalPresentAutoExposureEligible)
	{
		NRITextureResource& candidateExposureState = mExposure.GetMutableExposureStateTexture(mFrameIndex & 1u);
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
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.FrameIndex = mFrameIndex;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.PackedSceneOrigin = PackPresentSceneOrigin(mSceneLeft, mSceneTop);

	NRITextureResource& input = GetFrameTexture(inputSlot);
	NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	constants.InputWidth = input.width;
	constants.InputHeight = input.height;

	mFrameBuffer->TransitionTexture(input, NRIComputeShaderResourceState());
	if (exposureStateTextureValid)
	{
		mFrameBuffer->TransitionTexture(*exposureStateTexture, NRIComputeShaderResourceState());
	}
	mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	const nri::Descriptor* inputs[3] = {
		input.shaderView,
		exposureStateTextureValid ? exposureStateTexture->shaderView : input.shaderView,
		input.shaderView
	};
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = mFinalPresentFrameTextureSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputs;
	inputUpdate.descriptorNum = (uint32_t)std::size(inputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&inputUpdate, 1);

	const nri::Descriptor* outputs[1] = { final.storageView };
	nri::UpdateDescriptorRangeDesc outputUpdate = {};
	outputUpdate.descriptorSet = mFinalPresentOutputSet;
	outputUpdate.rangeIndex = 0;
	outputUpdate.descriptors = outputs;
	outputUpdate.descriptorNum = (uint32_t)std::size(outputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&outputUpdate, 1);

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPresentPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mFinalPresentFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mFinalPresentOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::FinalPresent));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mTargetWidth), GetDispatchSize(mTargetHeight), 1 });
	return true;
}



bool NRIRenderer::DispatchUpscaleChain()
{
	Clocker clock(NriPTUpscale);

	const NRIMainUpscalerKind mainKind = ResolveMainUpscalerKind(true);
	const NRIPostSharpenKind postSharpenKind = ResolvePostSharpenKind(true);
	const bool runAppTaa = NRIShouldRunAppTaa(mainKind);
	const bool useAppTaaJitter = runAppTaa && !mGuiCaptureActive;
	NRITextureResource& composed = GetFrameTexture(FrameTextureSlot::TraceTransparentOutput);
	const FrameTextureSlot vendorSourceSlot =
		mainKind == NRIMainUpscalerKind::DLRR ? FrameTextureSlot::RrInput :
		FrameTextureSlot::TraceTransparentOutput;
	NRITextureResource& historyInput = GetFrameTexture(mHistoryInputSlot);
	NRITextureResource& historyOutput = GetFrameTexture(mHistoryOutputSlot);
	TraceTemporalState("upscale-entry", mainKind, postSharpenKind, runAppTaa, mHistoryOutputSlot, vendorSourceSlot);

	if (runAppTaa)
	{
		NRITemporalConstants constants = {};
		constants.RenderWidth = mRenderWidth;
		constants.RenderHeight = mRenderHeight;
		constants.FrameIndex = mFrameIndex;
		constants.Flags =
			(mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
			(useAppTaaJitter ? NRI_FLAG_USE_JITTER : 0u) |
			NRIPackTemporalJitterPhaseCount(NRIGetTemporalJitterPhaseCount(
				mainKind,
				NRIResolveUpscalerModeForMain(mainKind, GetSelectedUpscalerMode()),
				mGuiCaptureActive));
		constants.Exposure = GetTemporalExposure(mFrameBuffer->GetPathTracingOutputPolicy());
		NRITextureResource* exposureStateTexture = nullptr;
		if (mExposure.GetSettings().enabled)
		{
			NRITextureResource& candidateExposureState = mExposure.GetMutableExposureStateTexture(mFrameIndex & 1u);
			if (candidateExposureState.texture != nullptr)
			{
				exposureStateTexture = &candidateExposureState;
			}
		}
		const bool exposureStateTextureValid =
			exposureStateTexture != nullptr &&
			exposureStateTexture->shaderView != nullptr;
		constants.Flags |=
			(mExposure.GetSettings().enabled ? NRI_TEMPORAL_FLAG_AUTO_EXPOSURE : 0u) |
			(exposureStateTextureValid ? NRI_TEMPORAL_FLAG_EXPOSURE_TEXTURE_VALID : 0u);

		mFrameBuffer->TransitionTexture(composed, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(historyInput, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeShaderResourceState());
		if (exposureStateTextureValid)
		{
			mFrameBuffer->TransitionTexture(*exposureStateTexture, NRIComputeShaderResourceState());
		}
		mFrameBuffer->TransitionTexture(historyOutput, NRIComputeStorageState());

		const nri::Descriptor* taaInputs[4] = {
			historyInput.shaderView,
			GetFrameTexture(FrameTextureSlot::Motion).shaderView,
			composed.shaderView,
			exposureStateTextureValid ? exposureStateTexture->shaderView : composed.shaderView
		};
		nri::UpdateDescriptorRangeDesc taaInputUpdate = {};
		taaInputUpdate.descriptorSet = mTaaFrameTextureSet;
		taaInputUpdate.rangeIndex = 0;
		taaInputUpdate.descriptors = taaInputs;
		taaInputUpdate.descriptorNum = (uint32_t)std::size(taaInputs);
		mFrameBuffer->mCore.UpdateDescriptorRanges(&taaInputUpdate, 1);

		const nri::Descriptor* taaOutputs[1] = { historyOutput.storageView };
		nri::UpdateDescriptorRangeDesc taaOutputUpdate = {};
		taaOutputUpdate.descriptorSet = mTaaOutputSet;
		taaOutputUpdate.rangeIndex = 0;
		taaOutputUpdate.descriptors = taaOutputs;
		taaOutputUpdate.descriptorNum = (uint32_t)std::size(taaOutputs);
		mFrameBuffer->mCore.UpdateDescriptorRanges(&taaOutputUpdate, 1);

		mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mTaaPipelineLayout);
		mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mTaaFrameTextureSet, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mTaaOutputSet, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::Taa));
		mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mRenderWidth), GetDispatchSize(mRenderHeight), 1 });
	}
	else if (mainKind == NRIMainUpscalerKind::Off)
	{
		CopyTexture(composed, historyOutput);
	}
	else if (mainKind == NRIMainUpscalerKind::DLSR)
	{
		// Keep ptdebug 13 meaningful even when app-TAA is intentionally bypassed for vendor SR.
		CopyTexture(composed, historyOutput);
	}
	else if (mainKind == NRIMainUpscalerKind::DLRR)
	{
		// Keep ptdebug 13 meaningful for RR as well by exposing the explicit noisy RR input.
		CopyTexture(GetFrameTexture(FrameTextureSlot::RrInput), historyOutput);
	}

	FrameTextureSlot resolvedInputSlot = mHistoryOutputSlot;

	if (mainKind != NRIMainUpscalerKind::Off)
	{
		const FrameTextureSlot vendorInputSlot =
			mainKind == NRIMainUpscalerKind::DLSR ? FrameTextureSlot::SrInput :
			FrameTextureSlot::RrInput;
		NRITextureResource& vendorInput = GetFrameTexture(vendorInputSlot);
		NRITextureResource& upscalerDepth = GetFrameTexture(FrameTextureSlot::UpscalerDepth);
		NRITextureResource& rrGuideDiffuseAlbedo = GetFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo);
		NRITextureResource& rrGuideSpecularAlbedo = GetFrameTexture(FrameTextureSlot::RrGuideSpecularAlbedo);
		NRITextureResource& rrGuideSpecularHitDistance = GetFrameTexture(FrameTextureSlot::RrGuideSpecularHitDistance);
		NRITextureResource& rrGuideNormalRoughness = GetFrameTexture(FrameTextureSlot::RrGuideNormalRoughness);
		NRITextureResource& vendorOutput = GetFrameTexture(FrameTextureSlot::VendorOutput);
		NRITextureResource* vendorExposure = nullptr;
		if (mExposure.GetSettings().enabled)
		{
			NRITextureResource& candidateExposureState = mExposure.GetMutableExposureStateTexture(mFrameIndex & 1u);
			if (candidateExposureState.texture != nullptr && candidateExposureState.shaderView != nullptr)
			{
				vendorExposure = &candidateExposureState;
			}
		}

		if (!DispatchUpscalerPrepass(mainKind))
		{
			return false;
		}

		mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(vendorInput, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(upscalerDepth, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(rrGuideDiffuseAlbedo, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(rrGuideSpecularAlbedo, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(rrGuideSpecularHitDistance, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(rrGuideNormalRoughness, NRIComputeShaderResourceState());
		if (vendorExposure != nullptr)
		{
			mFrameBuffer->TransitionTexture(*vendorExposure, NRIComputeShaderResourceState());
		}
		mFrameBuffer->TransitionTexture(vendorOutput, NRIComputeStorageState());

		const nri::UpscalerMode resolvedUpscalerMode = NRIResolveUpscalerModeForMain(mainKind, GetSelectedUpscalerMode());
		if (!mUpscaler.EnsureMainUpscaler(*mFrameBuffer, mainKind, resolvedUpscalerMode, mOutputWidth, mOutputHeight, vendorExposure != nullptr))
		{
			return false;
		}

		NRIUpscalerDispatchDesc upscalerDesc = {};
		upscalerDesc.commandBuffer = mFrameBuffer->mCommandBuffer;
		upscalerDesc.input = &vendorInput;
		upscalerDesc.output = &vendorOutput;
		upscalerDesc.motion = &GetFrameTexture(FrameTextureSlot::Motion);
		upscalerDesc.depth = &upscalerDepth;
		upscalerDesc.exposure = vendorExposure;
		upscalerDesc.normalRoughness = &rrGuideNormalRoughness;
		upscalerDesc.diffuseAlbedo = &rrGuideDiffuseAlbedo;
		upscalerDesc.specularAlbedo = &rrGuideSpecularAlbedo;
		upscalerDesc.specularHitDistance = &rrGuideSpecularHitDistance;
		upscalerDesc.currentWidth = mRenderWidth;
		upscalerDesc.currentHeight = mRenderHeight;
		Copy2(mCurrentJitter, upscalerDesc.cameraJitter);
		std::memcpy(upscalerDesc.viewToClipMatrix, mCurrentViewToClip, sizeof(upscalerDesc.viewToClipMatrix));
		std::memcpy(upscalerDesc.worldToViewMatrix, mCurrentWorldToView, sizeof(upscalerDesc.worldToViewMatrix));
		upscalerDesc.sharpness = Clamp01((float)nri_sharpness);
		upscalerDesc.resetHistory = mResetHistory;
		if (!mUpscaler.DispatchMainUpscaler(*mFrameBuffer, mainKind, upscalerDesc))
		{
			return false;
		}

		mUseUpscaledInFinal = true;
		mUpscaledInputSlot = FrameTextureSlot::VendorOutput;
		resolvedInputSlot = FrameTextureSlot::VendorOutput;
		TraceTemporalState("upscale-vendor", mainKind, postSharpenKind, runAppTaa, mUpscaledInputSlot, vendorSourceSlot);
	}
	else
	{
		mUseUpscaledInFinal = false;
		mUpscaledInputSlot = mHistoryOutputSlot;
		resolvedInputSlot = mHistoryOutputSlot;
		TraceTemporalState("upscale-native", mainKind, postSharpenKind, runAppTaa, resolvedInputSlot, mHistoryOutputSlot);
	}

	if (postSharpenKind == NRIPostSharpenKind::Off)
	{
		return true;
	}

	NRITextureResource& postInput = GetFrameTexture(resolvedInputSlot);
	NRITextureResource& postOutput = GetFrameTexture(FrameTextureSlot::PostSharpenOutput);
	mFrameBuffer->TransitionTexture(postInput, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(postOutput, NRIComputeStorageState());
	if (!mUpscaler.EnsurePostSharpen(*mFrameBuffer, postSharpenKind, mOutputWidth, mOutputHeight))
	{
		return false;
	}

	NRIUpscalerDispatchDesc postDesc = {};
	postDesc.commandBuffer = mFrameBuffer->mCommandBuffer;
	postDesc.input = &postInput;
	postDesc.output = &postOutput;
	postDesc.currentWidth = postInput.width;
	postDesc.currentHeight = postInput.height;
	Copy2(mCurrentJitter, postDesc.cameraJitter);
	postDesc.sharpness = Clamp01((float)nri_sharpness);
	postDesc.resetHistory = mResetHistory;
	if (!mUpscaler.DispatchPostSharpen(*mFrameBuffer, postSharpenKind, postDesc))
	{
		return false;
	}

	mUseUpscaledInFinal = true;
	mUpscaledInputSlot = FrameTextureSlot::PostSharpenOutput;
	TraceTemporalState("upscale-post-sharpen", mainKind, postSharpenKind, runAppTaa, mUpscaledInputSlot, resolvedInputSlot);
	return true;
}



bool NRIRenderer::DispatchFinal()
{
	Clocker clock(NriPTFinal);

	NRITraceSceneConstants constants = {};
	const uint32_t bootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	const bool presentRawTrace = (!nri_ptbootstrap && !mUseUpscaledInFinal) || bootstrapMode >= 13u;
	Copy3(mCurrentCameraPos, constants.CameraPos);
	Copy3(mCurrentCameraForward, constants.CameraForward);
	Copy3(mCurrentCameraRight, constants.CameraRight);
	Copy3(mCurrentCameraUp, constants.CameraUp);
	Copy3(mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = mRenderWidth;
	constants.RenderHeight = mRenderHeight;
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.TanHalfFovX = mCurrentTanHalfFovX;
	constants.TanHalfFovY = mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = mPreviousTanHalfFovY;
	constants.SceneInstanceCount = mSceneInstanceBuffer.stride != 0 ? (uint32_t)(mSceneInstanceBuffer.usedSize / mSceneInstanceBuffer.stride) : 0u;
	constants.StaticPrimitiveCount = mBoundStaticPrimitiveCount;
	constants.DynamicPrimitiveCount = mBoundDynamicPrimitiveCount;
	constants.FrameIndex = mFrameIndex;
	constants.Flags =
		(mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(mUseUpscaledInFinal ? NRI_FLAG_USE_UPSCALED : 0u) |
		(presentRawTrace ? NRI_FLAG_PRESENT_RAW_TRACE : 0u) |
		(mUseSplitShadowDenoiser ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(mDirectionalLightState.enabled && mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u);
	constants.StaticMaterialCount = mBoundStaticMaterialCount;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = mBoundDynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(0u, 0u, mDirectionalLightState.color);
	constants.RuntimeLightCount = mBoundRuntimeLightCount;
	constants.ReservedTrace0 = (uint16_t)(int16_t)mSceneLeft | ((uint32_t)(uint16_t)(int16_t)mSceneTop << 16);
	constants.ReservedTrace1 = PackDenoiserAux1(0u, mDirectionalLightState.angularSize);
	Copy3(mSkyColor, constants.SkyColor);
	Copy3(mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(mDirectionalLightState, constants);

	NRITextureResource& history = GetFrameTexture(mHistoryOutputSlot);
	NRITextureResource& upscaled = GetFrameTexture(mUpscaledInputSlot);
	NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredSpecular), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DenoisedShadow), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DirectLighting), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DirectEmission), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Validation), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::RrGuideSpecularAlbedo), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::RrGuideSpecularHitDistance), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(history, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(upscaled, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	mFrameInputDescriptors.fill(GetFrameTexture(FrameTextureSlot::Composed).shaderView);
	mFrameInputDescriptors[0] = history.shaderView;
	mFrameInputDescriptors[1] = GetFrameTexture(FrameTextureSlot::Motion).shaderView;
	mFrameInputDescriptors[2] = GetFrameTexture(FrameTextureSlot::ViewZ).shaderView;
	mFrameInputDescriptors[3] = GetFrameTexture(FrameTextureSlot::NormalRoughness).shaderView;
	mFrameInputDescriptors[4] = GetFrameTexture(FrameTextureSlot::BaseColorMetalness).shaderView;
	mFrameInputDescriptors[5] = presentRawTrace ? (mUseUpscaledInFinal ? upscaled.shaderView : GetFrameTexture(FrameTextureSlot::Composed).shaderView) : GetFrameTexture(FrameTextureSlot::Composed).shaderView;
	mFrameInputDescriptors[6] = upscaled.shaderView;
	mFrameInputDescriptors[7] = GetFrameTexture(FrameTextureSlot::Validation).shaderView;
	mFrameInputDescriptors[8] = GetFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo).shaderView;
	mFrameInputDescriptors[9] = GetFrameTexture(FrameTextureSlot::RrGuideSpecularAlbedo).shaderView;
	mFrameInputDescriptors[10] = GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra).shaderView;
	mFrameInputDescriptors[11] = GetFrameTexture(FrameTextureSlot::DenoisedShadow).shaderView;
	mFrameInputDescriptors[12] = GetFrameTexture(FrameTextureSlot::DirectLighting).shaderView;
	mFrameInputDescriptors[13] = GetFrameTexture(FrameTextureSlot::DirectEmission).shaderView;
	if (constants.DebugMode == 10)
	{
		mFrameInputDescriptors[5] = GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse).shaderView;
	}
	else if (constants.DebugMode == 11)
	{
		mFrameInputDescriptors[5] = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular).shaderView;
	}
	NRIDescriptorSetManager::UpdateFrameTextureSet(*this);

	mOutputDescriptors.fill(final.storageView);
	mOutputDescriptors[2] = final.storageView;
	NRIDescriptorSetManager::UpdateOutputSet(*this);

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	BindSceneRootDescriptors();
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mSamplerSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, GetCurrentSceneTextureSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 2, GetCurrentSceneDataSet(), nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 3, mFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 4, mOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::Final));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mTargetWidth), GetDispatchSize(mTargetHeight), 1 });
	return true;
}
