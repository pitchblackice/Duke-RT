#include "nri_debug_reporters.h"

#include "nri_renderer.h"
#include "../framegen/nri_framegen.h"
#include "../scene/nri_hash.h"
#include "../scene/nri_scene_stats.h"
#include "../system/nri_renderdevice.h"
#include "nri_actor_sprite_diagnostics.h"
#include "nri_diagnostic_names.h"
#include "nri_frame_graph.h"
#include "nri_map_chunk_diagnostics.h"
#include "nri_renderer_settings.h"
#include "nri_shader_contracts.h"
#include "nri_surface_light_overlay.h"
#include "c_cvars.h"
#include "mapinfo.h"
#include "printf.h"

#include <unordered_map>
#include <unordered_set>

EXTERN_CVAR(Bool, nri_apivalidation)
EXTERN_CVAR(Bool, nri_denoise)
EXTERN_CVAR(Bool, nri_dred)
EXTERN_CVAR(Bool, nri_ptbootstrap)
EXTERN_CVAR(Bool, nri_ptceilingnudge)
EXTERN_CVAR(Bool, nri_ptdirectscene)
EXTERN_CVAR(Bool, nri_ptemissivefastshadow)
EXTERN_CVAR(Bool, nri_ptemissivetlas)
EXTERN_CVAR(Bool, nri_ptnightvision)
EXTERN_CVAR(Bool, nri_ptruntimelinktrace)
EXTERN_CVAR(Bool, nri_ptsectorlighting)
EXTERN_CVAR(Bool, nri_ptselftest)
EXTERN_CVAR(Bool, nri_ptscenestats)
EXTERN_CVAR(Bool, nri_validation)
EXTERN_CVAR(Float, nri_ptceilingnudgedistance)
EXTERN_CVAR(Float, nri_ptemissiveminpower)
EXTERN_CVAR(Float, nri_ptemissiveminsurface)
EXTERN_CVAR(Float, nri_ptfullbrightboost)
EXTERN_CVAR(Float, nri_ptglowblend)
EXTERN_CVAR(Float, nri_ptglowfalloff)
EXTERN_CVAR(Float, nri_ptglowreach)
EXTERN_CVAR(Float, nri_ptglowscale)
EXTERN_CVAR(Float, nri_ptmirrordynamicdistance)
EXTERN_CVAR(Float, nri_ptnightvisionblue)
EXTERN_CVAR(Float, nri_ptnightvisioncontrast)
EXTERN_CVAR(Float, nri_ptnightvisionexposure)
EXTERN_CVAR(Float, nri_ptnightvisiongreen)
EXTERN_CVAR(Float, nri_ptnightvisionred)
EXTERN_CVAR(Float, nri_ptnightvisionsaturation)
EXTERN_CVAR(Float, nri_ptsectorambientscale)
EXTERN_CVAR(Float, nri_ptsectorclamp)
EXTERN_CVAR(Float, nri_ptsectoremissionlightmax)
EXTERN_CVAR(Float, nri_ptsectoremissionlightmin)
EXTERN_CVAR(Float, nri_ptsectoremissionreachmax)
EXTERN_CVAR(Float, nri_ptsectoremissionreachmin)
EXTERN_CVAR(Float, nri_ptsectoremissionresponsemax)
EXTERN_CVAR(Float, nri_ptsectoremissionresponsemin)
EXTERN_CVAR(Float, nri_ptsectoremissionsignalstrength)
EXTERN_CVAR(Float, nri_ptsectorfogscale)
EXTERN_CVAR(Float, nri_ptsectorhemiscale)
EXTERN_CVAR(Float, nri_ptsectorpulseamount)
EXTERN_CVAR(Float, nri_renderscale)
EXTERN_CVAR(Float, nri_sharpness)
EXTERN_CVAR(Float, nri_voxelemissionboost)
EXTERN_CVAR(Int, nri_ptactorspritetrace)
EXTERN_CVAR(Int, nri_ptdebug)
EXTERN_CVAR(Int, nri_ptbootstrapmode)
EXTERN_CVAR(Int, nri_ptmutationtracechunk)
EXTERN_CVAR(Int, nri_ptmutationtracesector)
EXTERN_CVAR(Int, nri_ptsectorfilterlotag)
EXTERN_CVAR(Int, nri_ptsectorfiltermaxshade)
EXTERN_CVAR(Int, nri_ptsectorfilterminshade)
EXTERN_CVAR(Int, nri_ptsectorfilterpal)
EXTERN_CVAR(Int, nri_ptsectorpulseframes)
EXTERN_CVAR(Int, nri_ptsurfaceprobe)
EXTERN_CVAR(Int, nri_pttraceframes)

namespace
{
	namespace chunk_diag = nri_map_chunk_diag;

	static const char* YesNo(bool value)
	{
		return value ? "yes" : "no";
	}

	static const char* GetNightVisionModeName(NRIPTNightVisionMode mode)
	{
		switch (mode)
		{
		case NRIPTNightVisionMode::Duke: return "duke";
		default: return "none";
		}
	}

	static const char* GetUpscalerFamilyName(NRIMainUpscalerKind kind, bool runAppTaa)
	{
		switch (kind)
		{
		case NRIMainUpscalerKind::DLSR: return "vendor-sr";
		case NRIMainUpscalerKind::DLRR: return "vendor-rr";
		default: return runAppTaa ? "native-taa" : "native";
		}
	}

	static const char* GetDirectionalLightSourceName(const NRIDirectionalLightState& state)
	{
		if (!state.enabled)
		{
			return "off";
		}

		return state.fromOverlay ? "overlay" : "default";
	}

	static const char* GetNrdHitDistanceReconstructionModeName(uint32_t mode)
	{
		switch (mode)
		{
		case 1: return "area_3x3";
		case 2: return "area_5x5";
		default: return "off";
		}
	}

	static const char* GetNrdDenoiserModeName(NRINrdDenoiserMode mode)
	{
		switch (mode)
		{
		case NRINrdDenoiserMode::Relax: return "RELAX_DIFFUSE_SPECULAR";
		default: return "REBLUR_DIFFUSE_SPECULAR";
		}
	}

	static const char* GetNrdInputSplitModeName(uint32_t mode)
	{
		switch (mode)
		{
		case 1: return "raw_left_denoised_right";
		case 2: return "denoised_left_raw_right";
		default: return "off";
		}
	}

	static uint32_t GetEffectivePtDebugMode()
	{
		if (nri_ptdebug < 0 || nri_ptdebug > (int)nri_diag::PtDebugTaaPreExposedInput)
		{
			return 0u;
		}

		const uint32_t debugMode = (uint32_t)nri_ptdebug;
		return IsNRIFrameGraphSupportedDebugMode(debugMode) ? debugMode : 0u;
	}

	static uint32_t GetBootstrapMode()
	{
		return std::clamp((uint32_t)std::max(0, (int)nri_ptbootstrapmode), 0u, 32u);
	}

	static NRIPresentRouteInfo ResolvePresentRouteInfo(uint32_t debugMode, bool bootstrap)
	{
		NRIFrameRouteRequest request = {};
		request.debugMode = debugMode;
		request.bootstrap = bootstrap;
		request.bootstrapMode = bootstrap ? GetBootstrapMode() : 0u;
		return ResolveNRIFrameRoute(request);
	}

	static uint32_t CoherencyFloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	static uint64_t HashMaterialBridgeSummary(const nri_scene::MaterialBridgeData& materials)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, (uint64_t)materials.materials.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)materials.lightMetadata.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)materials.textures.size());
		for (size_t i = 0; i < materials.materials.size(); ++i)
		{
			const auto& material = materials.materials[i];
			hash = nri_scene::HashCombine64(hash, (uint64_t)material.textureIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)material.paletteIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)material.flags);
			hash = nri_scene::HashCombine64(hash, (uint64_t)material.lightingFlags);
			hash = nri_scene::HashCombine64(hash, (uint64_t)material.emissiveMode);
			hash = nri_scene::HashCombine64(hash, (uint64_t)material.emissiveTextureIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)CoherencyFloatBits(material.alpha));
		}

		for (const auto& metadata : materials.lightMetadata)
		{
			hash = nri_scene::HashCombine64(hash, metadata.materialKey);
			hash = nri_scene::HashCombine64(hash, (uint64_t)metadata.textureId);
			hash = nri_scene::HashCombine64(hash, (uint64_t)metadata.actorIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)metadata.textureIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)metadata.paletteIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)metadata.emissiveMode);
			hash = nri_scene::HashCombine64(hash, (uint64_t)metadata.emissiveTextureIndex);
		}

		for (const auto& texture : materials.textures)
		{
			hash = nri_scene::HashCombine64(hash, texture.key);
			hash = nri_scene::HashCombine64(hash, (uint64_t)texture.width);
			hash = nri_scene::HashCombine64(hash, (uint64_t)texture.height);
			hash = nri_scene::HashCombine64(hash, texture.indexed ? 1ull : 0ull);
		}

		return hash;
	}

	static bool IsChunkMarkedVisible(const std::vector<uint32_t>& visibleChunkWords, uint32_t chunkIndex)
	{
		const size_t wordIndex = (size_t)(chunkIndex >> 5u);
		if (wordIndex >= visibleChunkWords.size())
		{
			return false;
		}

		return (visibleChunkWords[wordIndex] & (1u << (chunkIndex & 31u))) != 0u;
	}

	static uint32_t GetFlatPlaneVisibilityIndex(int32_t sectorIndex, bool ceiling)
	{
		return (uint32_t)sectorIndex * 2u + (ceiling ? 1u : 0u);
	}

	static bool IsFlatPlaneMarkedVisible(const std::vector<uint32_t>& visibleFlatPlaneWords, int32_t sectorIndex, bool ceiling)
	{
		if (sectorIndex < 0)
		{
			return false;
		}

		const uint32_t flatPlaneIndex = GetFlatPlaneVisibilityIndex(sectorIndex, ceiling);
		const size_t wordIndex = (size_t)(flatPlaneIndex >> 5u);
		if (wordIndex >= visibleFlatPlaneWords.size())
		{
			return false;
		}

		return (visibleFlatPlaneWords[wordIndex] & (1u << (flatPlaneIndex & 31u))) != 0u;
	}

}

void NRIRendererDiagnostics::ResetSelfTestRouteSnapshot()
{
	mSelfTestRoute = {};
}

void NRIRendererDiagnostics::SetSelfTestRouteSnapshot(const char* routeName, const char* presenterName, const char* ownerName, const char* passes, bool denoiserRun, bool upscalerRun, bool exposureRun)
{
	if (!nri_ptselftest)
	{
		return;
	}

	mSelfTestRoute.routeName = routeName != nullptr ? routeName : "unknown";
	mSelfTestRoute.presenterName = presenterName != nullptr ? presenterName : "unknown";
	mSelfTestRoute.ownerName = ownerName != nullptr ? ownerName : "unknown";
	mSelfTestRoute.passes = passes != nullptr ? passes : "unknown";
	mSelfTestRoute.denoiserRun = denoiserRun;
	mSelfTestRoute.upscalerRun = upscalerRun;
	mSelfTestRoute.exposureRun = exposureRun;
}

void NRIRendererDiagnostics::EmitSelfTestSummary(const NRISelfTestSummarySnapshot& snapshot) const
{
	if (!nri_ptselftest)
	{
		return;
	}

	Printf("NRI PT selftest: frame=%u engine_frame=%u map=%s level=%s backend=nri api=%s world_active=%u menu_active=%s gameplay_frame=%u portal=%u drawmode=%d route=%s debug=%d passes=%s presenter=%s owner=%s denoiser_run=%u upscaler_run=%u exposure_run=%u present_kind=%s render_width=%u render_height=%u output_width=%u output_height=%u swapchain_format=%u hdr=%u prims=%u mats=%u scene_instances=%u static_instances=%u dynamic_instances=%u voxel_instances=%u emissive_instances=%u vertices=%u indices=%u vertex_bytes=%llu index_bytes=%llu primitive_bytes=%llu material_bytes=%llu instance_bytes=%llu scene_sig=0x%llx material_sig=0x%llx instance_sig=0x%llx sky_sig=0x%llx sky_mode=%s sky_source=%s sky_key=0x%llx sky_brightness=%.3f sky_action=%s auto_exposure=%u exposure_texture=%u exposure=%.6f target_exposure=%.6f adapted_exposure=%.6f metered_log_lum=%.6f exposure_stats_valid=%u exposure_stats_frame=%llu final_valid=%u final_nonzero=unknown final_nonzero_ratio=unknown final_luma_mean=unknown final_luma_min=unknown final_luma_max=unknown final_nan=unknown final_inf=unknown scene_reason=ok route_reason=ok exposure_reason=%s present_reason=ok\n",
		snapshot.traceFrameIndex,
		snapshot.engineFrameIndex,
		snapshot.mapName,
		snapshot.levelName,
		snapshot.graphicsApiName,
		snapshot.worldActive ? 1u : 0u,
		snapshot.menuActive ? "yes" : "no",
		snapshot.gameplayFrame ? 1u : 0u,
		snapshot.portal ? 1u : 0u,
		snapshot.drawmode,
		snapshot.route.routeName,
		snapshot.debugMode,
		snapshot.route.passes,
		snapshot.route.presenterName,
		snapshot.route.ownerName,
		snapshot.route.denoiserRun ? 1u : 0u,
		snapshot.route.upscalerRun ? 1u : 0u,
		snapshot.route.exposureRun ? 1u : 0u,
		snapshot.presentKind,
		snapshot.renderWidth,
		snapshot.renderHeight,
		snapshot.outputWidth,
		snapshot.outputHeight,
		snapshot.swapchainFormat,
		snapshot.hdr ? 1u : 0u,
		snapshot.primitiveCount,
		snapshot.materialCount,
		snapshot.sceneInstanceCount,
		snapshot.staticInstanceCount,
		snapshot.dynamicInstanceCount,
		snapshot.persistentVoxelInstanceCount,
		snapshot.emissiveInstanceCount,
		snapshot.vertexCount,
		snapshot.indexCount,
		(unsigned long long)snapshot.vertexBytes,
		(unsigned long long)snapshot.indexBytes,
		(unsigned long long)snapshot.primitiveBytes,
		(unsigned long long)snapshot.materialBytes,
		(unsigned long long)snapshot.instanceBytes,
		(unsigned long long)snapshot.sceneSignature,
		(unsigned long long)snapshot.materialSignature,
		(unsigned long long)snapshot.instanceSignature,
		(unsigned long long)snapshot.skySignature,
		snapshot.skyMode,
		snapshot.skySource,
		(unsigned long long)snapshot.skyKey,
		snapshot.skyBrightness,
		snapshot.skyAction,
		snapshot.autoExposure ? 1u : 0u,
		snapshot.exposureTexture ? 1u : 0u,
		snapshot.exposure,
		snapshot.targetExposure,
		snapshot.adaptedExposure,
		snapshot.meteredLogLuminance,
		snapshot.exposureStatsValid ? 1u : 0u,
		(unsigned long long)snapshot.exposureStatsFrame,
		snapshot.finalValid ? 1u : 0u,
		snapshot.exposureReason);
}

void NRIRenderer::ResetSelfTestRouteSnapshot()
{
	mDiagnostics.ResetSelfTestRouteSnapshot();
}

void NRIRenderer::SetSelfTestRouteSnapshot(const char* routeName, const char* presenterName, const char* ownerName, const char* passes, bool denoiserRun, bool upscalerRun, bool exposureRun)
{
	mDiagnostics.SetSelfTestRouteSnapshot(routeName, presenterName, ownerName, passes, denoiserRun, upscalerRun, exposureRun);
}

void NRIRenderer::PrintStatus()
{
	NRISyncLegacyUpscalerConfig(false);
	const NRIMainUpscalerKind requestedMain = GetSelectedMainUpscalerKind();
	const NRIMainUpscalerKind resolvedMain = GetResolvedMainUpscalerKindForStatus();
	const NRIPostSharpenKind requestedPost = GetSelectedPostSharpenKind();
	const NRIPostSharpenKind resolvedPost = GetResolvedPostSharpenKindForStatus();
	const nri::UpscalerMode requestedUpscalerMode = GetSelectedUpscalerMode();
	const nri::UpscalerMode resolvedUpscalerMode = NRIResolveUpscalerModeForMain(resolvedMain, requestedUpscalerMode);
	const bool runAppTaa = NRIShouldRunAppTaa(resolvedMain);
	const float requestedRenderScale = std::max(0.33f, std::min((float)nri_renderscale, 1.0f));
	const float resolvedRenderScale = NRIResolveRenderScaleForMain(resolvedMain, requestedUpscalerMode, requestedRenderScale);
	const uint32_t bootstrapMode = GetBootstrapMode();
	const NRITraceSettings traceSettings = BuildNRITraceSettingsFromCVars();
	const NRIDenoiserSettings denoiserSettings = BuildNRIDenoiserSettingsFromCVars();
	const NRITextureResource& srInput = GetFrameTexture(FrameTextureSlot::SrInput);
	const NRITextureResource& rrInput = GetFrameTexture(FrameTextureSlot::RrInput);
	const NRITextureResource& upscalerDepth = GetFrameTexture(FrameTextureSlot::UpscalerDepth);
	const NRITextureResource& vendorOutput = GetFrameTexture(FrameTextureSlot::VendorOutput);
	const NRITextureResource& postSharpenOutput = GetFrameTexture(FrameTextureSlot::PostSharpenOutput);
	const NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	const auto& frameGenPolicy = mFrameBuffer->mFrameGeneration.GetPolicy();
	const auto& frameGenPresentContract = mFrameBuffer->mFrameGeneration.GetPresentContract();
	const NRIPTOutputPolicy outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	const NRIPresentRouteInfo presentRoute = ResolvePresentRouteInfo(GetEffectivePtDebugMode(), !!nri_ptbootstrap);
	const nri::Format expectedFinalFormat = ResolveFinalSceneFormat();
	const bool hasFrameGenDesc = mFrameBuffer->mFrameGeneration.HasFrameDesc();
	const auto& frameGenDesc = mFrameBuffer->mFrameGeneration.GetFrameDesc();
	const auto& frameGenAudit = mFrameBuffer->mFrameGeneration.GetInputAudit();
	const auto& frameGenProvider = mFrameBuffer->mFrameGeneration.GetProviderState();
	const NRIAutoExposureSettings autoExposureSettings = GetNRIAutoExposureSettings(
		outputPolicy.exposure,
		IsNRIPTHdrOutputActive(outputPolicy));
	mExposure.SetSettings(autoExposureSettings);
	ReadbackAutoExposureStats();
	const NRIAutoExposureStatus& autoExposureStatus = mExposure.GetStatus();
	const NRIMainUpscalerKind autoExposureResolvedMain = GetResolvedMainUpscalerKindForStatus();
	const NRIPostSharpenKind autoExposureResolvedPost = GetResolvedPostSharpenKindForStatus();
	const FrameTextureSlot autoExposurePresentSlot = mUseUpscaledInFinal ? mUpscaledInputSlot : mHistoryOutputSlot;
	const ExposureRoute autoExposurePresentRoute = ResolveExposureRoute(
		autoExposurePresentSlot,
		outputPolicy,
		autoExposureResolvedMain,
		autoExposureResolvedPost);
	const NRITextureResource* autoExposureStateTexture = mExposure.GetExposureStateTexture(mFrameIndex & 1u);
	const bool autoExposureSceneHdrInput = autoExposurePresentRoute.inputDomain == ExposureDomain::SceneHDR;
	const bool autoExposureTextureValid =
		autoExposureStateTexture != nullptr &&
		autoExposureStateTexture->shaderView != nullptr;
	const bool autoExposurePresentEligible =
		autoExposureSettings.enabled &&
		autoExposureSceneHdrInput &&
		autoExposureTextureValid;
	const bool vendorExposurePath =
		autoExposureResolvedMain == NRIMainUpscalerKind::DLSR ||
		autoExposureResolvedMain == NRIMainUpscalerKind::DLRR;
	const bool vendorExposureEngine =
		vendorExposurePath &&
		autoExposureSettings.enabled &&
		autoExposureTextureValid;
	const char* vendorExposureMode =
		!vendorExposurePath ? "none" :
		vendorExposureEngine ? "engine" :
		"vendor-auto";

	Printf("NRI PT status: support=%s", mPathTracingSupported ? "available" : "raster-fallback");
	if (!mPathTracingSupported)
	{
		Printf(" (%s)", GetAvailabilityReason());
	}
	Printf("\n");
	Printf("NRI PT frame: index=%u fg_frame_id=%llu render=%ux%u output=%ux%u prev_camera=%s reset_history=%s\n",
		mFrameIndex,
		(unsigned long long)mFrameGenerationFrameId,
		mRenderWidth,
		mRenderHeight,
		mOutputWidth,
		mOutputHeight,
		mHasPreviousCameraState ? "yes" : "no",
		mResetHistory ? "yes" : "no");
	Printf("NRI PT output: requested_mode=%s resolved_mode=%s control_block=%s tonemap=%s exposure=%.3f contrast=%.3f saturation=%.3f shoulder=%.3f toe=%.3f paper_white=%.1f offscreen_hdr=%s hdr_swapchain=%s display_info=%s display_hdr=%s display_sdr_nits=%.1f display_max_nits=%.1f\n",
		GetNRIPTOutputModeName(outputPolicy.requestedMode),
		GetNRIPTOutputModeName(outputPolicy.resolvedMode),
		GetNRIPTOutputControlBlockName(outputPolicy),
		GetNRIPTTonemapModeName(outputPolicy.tonemapMode),
		outputPolicy.exposure,
		outputPolicy.contrast,
		outputPolicy.saturation,
		outputPolicy.shoulder,
		outputPolicy.toe,
		outputPolicy.paperWhiteNits,
		outputPolicy.offscreenHdrTarget ? "yes" : "no",
		outputPolicy.hdrSwapChainActive ? "yes" : "no",
		outputPolicy.displayInfoAvailable ? "yes" : "no",
		outputPolicy.displayHdrSupported ? "yes" : "no",
		outputPolicy.displaySdrLuminance,
		outputPolicy.displayMaxLuminance);
	Printf("NRI PT auto exposure: enabled=%s control_block=%s freeze=%s stats=%s resources=%s state_textures=%s meter_source=%s meter_mode=%s histogram_bins=%u sample_step=%u target=%.3f range=%.3f..%.3f bias=%.3f percentiles=%.2f..%.2f hist_log_range=%.1f..%.1f adapt=%.3f/%.3f fallback_manual=%.3f resource_render=%ux%u memory=%llu alloc_serial=%u reset_pending=%s reset_serial=%llu reset_request_frame=%llu reset_consumed_frame=%llu reset_reason=%s\n",
		YesNo(autoExposureSettings.enabled),
		autoExposureSettings.hdrControlsActive ? "hdr" : "sdr",
		YesNo(autoExposureSettings.freeze),
		YesNo(autoExposureSettings.stats),
		YesNo(autoExposureStatus.resourcesAllocated),
		autoExposureStatus.resourcesAllocated ? "allocated" : "not_allocated",
		GetFrameTextureSlotName(mAutoExposureInputSourceSlot),
		GetNRIAutoExposureMeteringModeName(autoExposureSettings.meteringMode),
		autoExposureSettings.histogramBinCount,
		autoExposureSettings.sampleStep,
		autoExposureSettings.targetLuminance,
		autoExposureSettings.minExposure,
		autoExposureSettings.maxExposure,
		autoExposureSettings.exposureBias,
		autoExposureSettings.lowPercentile,
		autoExposureSettings.highPercentile,
		NRI_EXPOSURE_LOG_LUMINANCE_MIN,
		NRI_EXPOSURE_LOG_LUMINANCE_MAX,
		autoExposureSettings.adaptUpSpeed,
		autoExposureSettings.adaptDownSpeed,
		autoExposureSettings.fallbackManualExposure,
		autoExposureStatus.renderWidth,
		autoExposureStatus.renderHeight,
		(unsigned long long)autoExposureStatus.memoryBytes,
		autoExposureStatus.allocationSerial,
		YesNo(autoExposureStatus.resetPending),
		(unsigned long long)autoExposureStatus.resetSerial,
		(unsigned long long)autoExposureStatus.resetRequestFrame,
		(unsigned long long)autoExposureStatus.resetConsumedFrame,
		autoExposureStatus.resetReason[0] != '\0' ? autoExposureStatus.resetReason : "none");
	Printf("NRI PT auto exposure stats: valid=%s readback=%s frame=%llu samples=%u bins=%u..%u log_lum=%.3f..%.3f metered_log_lum=%.3f target_exposure=%.3f adapted_exposure=%.3f target_ev=%.3f adapted_ev=%.3f\n",
		YesNo(autoExposureStatus.debugValid),
		YesNo(autoExposureStatus.debugReadbackAllocated),
		(unsigned long long)autoExposureStatus.debugFrameIndex,
		autoExposureStatus.sampleCount,
		autoExposureStatus.lowBin,
		autoExposureStatus.highBin,
		autoExposureStatus.lowLogLuminance,
		autoExposureStatus.highLogLuminance,
		autoExposureStatus.meteredLogLuminance,
		autoExposureStatus.targetExposure,
		autoExposureStatus.adaptedExposure,
		std::log2(std::max(autoExposureStatus.targetExposure, 1.0e-6f)),
		std::log2(std::max(autoExposureStatus.adaptedExposure, 1.0e-6f)));
	Printf("NRI PT auto exposure present: slot=%s domain=%s enabled=%s scene_hdr=%s texture_valid=%s apply=%s manual_fallback=%.3f\n",
		GetFrameTextureSlotName(autoExposurePresentSlot),
		GetExposureDomainName(autoExposurePresentRoute.inputDomain),
		YesNo(autoExposureSettings.enabled),
		YesNo(autoExposureSceneHdrInput),
		YesNo(autoExposureTextureValid),
		YesNo(autoExposurePresentEligible),
		autoExposurePresentRoute.presentExposure);
	Printf("NRI PT auto exposure vendor: main=%s mode=%s texture_valid=%s engine_enabled=%s recreate_on_policy_change=yes\n",
		NRIGetMainUpscalerName(autoExposureResolvedMain),
		vendorExposureMode,
		YesNo(autoExposureTextureValid),
		YesNo(autoExposureSettings.enabled));
	Printf("NRI PT nightvision: mode=%s view_eligible=%s active=%s presenter=%s strength=%.3f remaining_s=%.3f\n",
		GetNightVisionModeName(mNightVisionState.mode),
		YesNo(mNightVisionState.viewEligible),
		YesNo(mNightVisionState.enabled),
		nri_ptnightvision ? "on" : "off",
		mNightVisionState.strength01,
		mNightVisionState.remainingSeconds);
	Printf("NRI PT nightvision tuning: exposure=%.3f contrast=%.3f saturation=%.3f\n",
		(float)nri_ptnightvisionexposure,
		(float)nri_ptnightvisioncontrast,
		(float)nri_ptnightvisionsaturation);
	Printf("NRI PT nightvision tint: red=%.3f green=%.3f blue=%.3f\n",
		(float)nri_ptnightvisionred,
		(float)nri_ptnightvisiongreen,
		(float)nri_ptnightvisionblue);
	Printf("NRI PT material calibration: fullbright_boost=%.3f voxel_emission_boost=%.3f\n",
		(float)nri_ptfullbrightboost,
		(float)nri_voxelemissionboost);
	if (outputPolicy.hdrSwapChainActive)
	{
		const float safeDisplaySdr = std::max(outputPolicy.displaySdrLuminance, 1.0f);
		const float safeDisplayMax = std::max(outputPolicy.displayMaxLuminance, safeDisplaySdr);
		const float safePaperWhite = std::clamp(std::max(outputPolicy.paperWhiteNits, safeDisplaySdr), safeDisplaySdr, safeDisplayMax);
		const float hdrPaperWhiteScale = safePaperWhite / 80.0f;
		const float hdrHeadroom = std::max(safeDisplayMax / safePaperWhite, 1.0f);
		const float hdrMaxScale = hdrPaperWhiteScale * hdrHeadroom;
		Printf("NRI PT output hdr: paper_scale=%.3f headroom=%.3f max_scale=%.3f active_linear16=%s\n",
			hdrPaperWhiteScale,
			hdrHeadroom,
			hdrMaxScale,
			outputPolicy.resolvedMode == NRIPTOutputMode::HDRLinear16 ? "yes" : "no");
	}
	Printf("NRI PT routing: debug=%u route=%s presenter=%s owner=%s root_bytes=scene:%u temporal:%u present:%u\n",
		GetEffectivePtDebugMode(),
		presentRoute.routeName,
		presentRoute.presenterName,
		presentRoute.ownerName,
		(unsigned)sizeof(NRITraceSceneConstants),
		(unsigned)sizeof(NRITemporalConstants),
		(unsigned)sizeof(NRIPresentConstants));
	Printf("NRI PT features: bootstrap=%s denoise=%s validation=%s api_validation=%s dred=%s main_upscaler=%s->%s post_sharpen=%s->%s requested_mode=%s resolved_mode=%s requested_render_scale=%.3f resolved_render_scale=%.3f sharpness=%.3f\n",
		nri_ptbootstrap ? "on" : "off",
		nri_denoise ? "on" : "off",
		nri_validation ? "on" : "off",
		nri_apivalidation ? "on" : "off",
		nri_dred ? "on" : "off",
		NRIGetMainUpscalerName(requestedMain),
		NRIGetMainUpscalerName(resolvedMain),
		NRIGetPostSharpenName(requestedPost),
		NRIGetPostSharpenName(resolvedPost),
		NRIGetUpscalerModeName(requestedUpscalerMode),
		NRIGetUpscalerModeName(resolvedUpscalerMode),
		requestedRenderScale,
		resolvedRenderScale,
		(float)nri_sharpness);
	Printf("NRI PT framegen policy: requested=%s provider=%s resolved=%s output=%s->%s contract=%s scope=%s api=%s shader_model=%u.%u window=%s low_latency=%s->%s(avail=%s iface=%s swapchain=%s) async=%s->%s(avail=%s) ui=%s->%s swapchain=%s native=device:%s queue:%s swapchain:%s waitable=%s runtime=%s frame_desc=%s reason=%s\n",
		frameGenPolicy.requestedEnabled ? "on" : "off",
		NRIFrameGenerationContext::GetProviderName(frameGenPolicy.requestedProvider),
		NRIFrameGenerationContext::GetProviderName(frameGenPolicy.resolvedProvider),
		GetNRIPTOutputModeName(frameGenPolicy.requestedOutputMode),
		GetNRIPTOutputModeName(frameGenPolicy.resolvedOutputMode),
		NRIFrameGenerationContext::GetOutputContractName(frameGenPolicy.resolvedOutputContract),
		frameGenPolicy.outputContractScope,
		frameGenPolicy.selectedApiName,
		frameGenPolicy.shaderModel / 10u,
		frameGenPolicy.shaderModel % 10u,
		NRIFrameGenerationContext::GetWindowModeName(frameGenPolicy.fullscreenActive),
		frameGenPolicy.requestedLowLatency ? "on" : "off",
		frameGenPolicy.resolvedLowLatency ? "on" : "off",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencyAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencyInterfaceAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencySwapChainEnabled),
		frameGenPolicy.requestedAsync ? "on" : "off",
		frameGenPolicy.resolvedAsync ? "on" : "off",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.asyncWorkloadAvailable),
		NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.requestedUiMode),
		NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.resolvedUiMode),
		frameGenPolicy.swapChainReady ? "ready" : "cold",
		frameGenPolicy.nativeDeviceAvailable ? "ok" : "missing",
		frameGenPolicy.nativeGraphicsQueueAvailable ? "ok" : "missing",
		frameGenPolicy.nativeSwapChainAvailable ? "ok" : "missing",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.waitableSwapChainAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.providerRuntimeSupported),
		hasFrameGenDesc ? "captured" : "empty",
		frameGenPolicy.resolvedReason);
	Printf("NRI PT framegen present contract: output=%s->%s proxy=%s hdr_swapchain=%s swapchain=%s texture=%s active=%s dxgi=%s active_dxgi=%s transfer=%s luminance=%.3f..%.3f hdr_scale=%.3f reason=%s\n",
		GetNRIPTOutputModeName(frameGenPresentContract.requestedOutputMode),
		GetNRIPTOutputModeName(frameGenPresentContract.resolvedOutputMode),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPresentContract.proxyAllowed),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPresentContract.usesHdrSwapChain),
		NRIFrameGenerationContext::GetSwapChainFormatName(frameGenPresentContract.createdSwapChainFormat),
		NRIFrameGenerationContext::GetNriFormatName(frameGenPresentContract.resolvedTextureFormat),
		NRIFrameGenerationContext::GetNriFormatName(frameGenPresentContract.activePresentTargetFormat),
		frameGenPresentContract.resolvedDxgiFormatValid ? NRIFrameGenerationContext::GetDxgiFormatName(frameGenPresentContract.resolvedDxgiFormat) : "unknown",
		frameGenPresentContract.activePresentTargetDxgiFormatValid ? NRIFrameGenerationContext::GetDxgiFormatName(frameGenPresentContract.activePresentTargetDxgiFormat) : "unknown",
		NRIFrameGenerationContext::GetPresentTransferFunctionName(frameGenPresentContract.transferFunction),
		frameGenPresentContract.minLuminance,
		frameGenPresentContract.maxLuminance,
		frameGenPresentContract.hdrPaperWhiteScale,
		frameGenPresentContract.resolvedReason);
	Printf("NRI PT final surface: expected=%s allocated=%s contract=%s active=%s size=%ux%u\n",
		NRIFrameGenerationContext::GetNriFormatName(expectedFinalFormat),
		NRIFrameGenerationContext::GetNriFormatName(final.format),
		NRIFrameGenerationContext::GetNriFormatName(frameGenPresentContract.resolvedTextureFormat),
		NRIFrameGenerationContext::GetNriFormatName(frameGenPresentContract.activePresentTargetFormat),
		final.width,
		final.height);
	Printf("NRI PT framegen provider: runtime=%s funcs=%s context=%s swapctx=%s bridge=%s debug=%s no_swapchain_notify=%s cfg=%s prepare=%s fg_dispatch=%s ui_reg=%s camera=%s lib=%s version=%s dims=render:%ux%u display:%ux%u counts=cfg:%llu prep:%llu fg:%llu frames=%llu/%llu query=%s/%s create=%s/%s config=%s/%s prepare=%s dispatch=%s vram=fg:%s:%llu/%llu sc:%s:%llu/%llu resets=%llu last_reset=%s present=%s/%s count=%llu reason=%s\n",
		frameGenProvider.runtimeLoaded ? "yes" : "no",
		frameGenProvider.runtimeFunctionsLoaded ? "yes" : "no",
		frameGenProvider.contextCreated ? "yes" : "no",
		frameGenProvider.swapChainContextCreated ? "yes" : "no",
		frameGenProvider.presentBridgeReady ? "yes" : "no",
		frameGenProvider.debugConfigured ? "yes" : "no",
		frameGenProvider.noSwapChainNotify ? "yes" : "no",
		frameGenProvider.configuredThisFrame ? "yes" : "no",
		frameGenProvider.prepareDispatchedThisFrame ? "yes" : "no",
		frameGenProvider.frameGenerationDispatchedThisFrame ? "yes" : "no",
		frameGenProvider.uiResourceRegisteredThisFrame ? "yes" : "no",
		frameGenProvider.prepareCameraInfoProvided ? "yes" : "no",
		frameGenProvider.runtimeLibrary,
		frameGenProvider.providerVersion,
		frameGenProvider.contextRenderWidth,
		frameGenProvider.contextRenderHeight,
		frameGenProvider.contextDisplayWidth,
		frameGenProvider.contextDisplayHeight,
		(unsigned long long)frameGenProvider.configureCount,
		(unsigned long long)frameGenProvider.prepareCount,
		(unsigned long long)frameGenProvider.dispatchCount,
		(unsigned long long)frameGenProvider.lastConfiguredFrameId,
		(unsigned long long)frameGenProvider.lastPreparedFrameId,
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastQueryResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastSwapChainQueryResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastCreateResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastSwapChainCreateResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastConfigureResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastSwapChainConfigureResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastPrepareResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastDispatchResult),
		frameGenProvider.memoryUsageValid ? "yes" : "no",
		(unsigned long long)frameGenProvider.totalUsageBytes,
		(unsigned long long)frameGenProvider.aliasableUsageBytes,
		frameGenProvider.swapChainMemoryUsageValid ? "yes" : "no",
		(unsigned long long)frameGenProvider.swapChainTotalUsageBytes,
		(unsigned long long)frameGenProvider.swapChainAliasableUsageBytes,
		(unsigned long long)frameGenProvider.resetCount,
		frameGenProvider.lastResetReason,
		frameGenProvider.lastPresentMode,
		NRIFrameGenerationContext::GetPresentResultName(frameGenProvider.lastPresentResult),
		(unsigned long long)frameGenProvider.presentCount,
		frameGenProvider.lastStatusReason);
	Printf("NRI PT framegen present: current=%s bridge_active=%s generated=%s fallback_pending=%s last=%s result=%s\n",
		frameGenProvider.frameGenerationDispatchedThisFrame ? "generated" :
			(frameGenProvider.presentUsedBridgeThisFrame ? "passthrough" : "native"),
		frameGenProvider.presentBridgeReady ? "yes" : "no",
		frameGenProvider.frameGenerationDispatchedThisFrame ? "yes" : "no",
		frameGenProvider.nativeFallbackRequested ? "yes" : "no",
		frameGenProvider.lastPresentMode,
		NRIFrameGenerationContext::GetPresentResultName(frameGenProvider.lastPresentResult));
	if (hasFrameGenDesc)
	{
		Printf("NRI PT framegen inputs: frame_id=%llu hudless=%s:%ux%u ui=%ux%u motion=%ux%u depth=%ux%u render_rect=%u,%u+%ux%u output_rect=%u,%u+%ux%u reset=%s prev_camera=%s frame_time=%s frame_time_ms=%.3f\n",
			(unsigned long long)frameGenDesc.frameId,
			NRIFrameGenerationContext::GetColorSourceName(frameGenDesc.hudlessColorSource),
			frameGenDesc.hudlessColor != nullptr ? frameGenDesc.hudlessColor->width : 0u,
			frameGenDesc.hudlessColor != nullptr ? frameGenDesc.hudlessColor->height : 0u,
			frameGenDesc.uiTexture != nullptr ? frameGenDesc.uiTexture->width : 0u,
			frameGenDesc.uiTexture != nullptr ? frameGenDesc.uiTexture->height : 0u,
			frameGenDesc.motionVectors != nullptr ? frameGenDesc.motionVectors->width : 0u,
			frameGenDesc.motionVectors != nullptr ? frameGenDesc.motionVectors->height : 0u,
			frameGenDesc.depth != nullptr ? frameGenDesc.depth->width : 0u,
			frameGenDesc.depth != nullptr ? frameGenDesc.depth->height : 0u,
			frameGenDesc.renderRect.left,
			frameGenDesc.renderRect.top,
			frameGenDesc.renderRect.width,
			frameGenDesc.renderRect.height,
			frameGenDesc.outputRect.left,
			frameGenDesc.outputRect.top,
			frameGenDesc.outputRect.width,
			frameGenDesc.outputRect.height,
			frameGenDesc.resetReason[0] != '\0' ? frameGenDesc.resetReason : "none",
			frameGenDesc.hasPreviousCamera ? "yes" : "no",
			frameGenDesc.hasRealFrameTimeMs ? "captured" : "pending",
			frameGenDesc.realFrameTimeMs);
		Printf("NRI PT framegen contract: motion=%s/%s scale=%.3f,%.3f depth=%s inverted=%s infinite=%s jitter=current(%.3f,%.3f) prev(%.3f,%.3f) fsr3=motion:%s depth:%s prepare:%s adapter:%s reason=%s\n",
			NRIFrameGenerationContext::GetMotionVectorSpaceName(frameGenDesc.motionVectorSpace),
			NRIFrameGenerationContext::GetMotionVectorDirectionName(frameGenDesc.motionVectorDirection),
			frameGenDesc.motionVectorScale[0],
			frameGenDesc.motionVectorScale[1],
			NRIFrameGenerationContext::GetDepthTypeName(frameGenDesc.depthType),
			frameGenDesc.depthInverted ? "yes" : "no",
			frameGenDesc.depthInfinite ? "yes" : "no",
			frameGenDesc.cameraJitter[0],
			frameGenDesc.cameraJitter[1],
			frameGenDesc.previousCameraJitter[0],
			frameGenDesc.previousCameraJitter[1],
			frameGenAudit.fsr3MotionCompatible ? "yes" : "no",
			frameGenAudit.fsr3DepthCompatible ? "yes" : "no",
			frameGenAudit.fsr3PrepareInputsRequired ? "yes" : "no",
			NRIFrameGenerationContext::GetAdapterRequirementName(frameGenAudit.adapterRequirement),
			frameGenAudit.statusReason);
	}
	Printf("NRI PT resolution policy: policy=%s render=%ux%u output=%ux%u jitter=%s phases=%u\n",
		NRIGetRenderResolutionPolicyName(resolvedMain),
		mRenderWidth,
		mRenderHeight,
		mOutputWidth,
		mOutputHeight,
		NRIGetTemporalJitterModeName(resolvedMain, mGuiCaptureActive),
		NRIGetTemporalJitterPhaseCount(resolvedMain, resolvedUpscalerMode, mGuiCaptureActive));
	Printf("NRI PT output shell: family=%s sr_input=%ux%u rr_input=%ux%u guides=%ux%u vendor=%ux%u post_output=%ux%u post=%s active=%s last_reset_reason=%s\n",
		GetUpscalerFamilyName(resolvedMain, runAppTaa),
		srInput.width,
		srInput.height,
		rrInput.width,
		rrInput.height,
		upscalerDepth.width,
		upscalerDepth.height,
		vendorOutput.width,
		vendorOutput.height,
		postSharpenOutput.width,
		postSharpenOutput.height,
		NRIGetPostSharpenName(resolvedPost),
		resolvedPost == NRIPostSharpenKind::Off ? "pre-post" : "post-sharpen-output",
		mLastHistoryResetReason.c_str());
	Printf("NRI PT tracing: direct_scene_fallback=%s light_bounces=%u mirror_bounces=%u portal_depth=%u surface_probe=%d ceiling_nudge=%s ceiling_nudge_distance=%.4f\n",
		nri_ptdirectscene ? "on" : "off",
		traceSettings.lightBounceCount,
		traceSettings.mirrorBounceCount,
		traceSettings.portalDepth,
		(int)nri_ptsurfaceprobe,
		nri_ptceilingnudge ? "on" : "off",
		(float)nri_ptceilingnudgedistance);
	Printf("NRI PT lighting shell: directional=%s sector=%s\n",
		mDirectionalLightState.enabled ? "on" : "off",
		nri_ptsectorlighting ? "on" : "off");
	Printf("NRI PT directional light: source=%s shadow=%s rule=%u dir=(%.3f, %.3f, %.3f) color=(%.3f, %.3f, %.3f) angular=%.3f\n",
		GetDirectionalLightSourceName(mDirectionalLightState),
		mDirectionalLightState.enabled && mDirectionalLightState.shadow ? "on" : "off",
		mDirectionalLightState.ruleId,
		mDirectionalLightState.direction[0],
		mDirectionalLightState.direction[1],
		mDirectionalLightState.direction[2],
		mDirectionalLightState.color[0],
		mDirectionalLightState.color[1],
		mDirectionalLightState.color[2],
		mDirectionalLightState.angularSize);
	Printf("NRI PT transparent shell: trace_transparent=placeholder_noop\n");
	uint32_t emissiveBaseCount = 0;
	uint32_t emissiveConstantCount = 0;
	uint32_t emissiveGlowmapCount = 0;
	for (const auto& surface : mSceneLights.GetEmissiveSurfaces().activeSurfaces)
	{
		switch (surface.emissiveMode)
		{
		case nri_scene::MaterialEmissiveMode_UseBaseTexture: emissiveBaseCount++; break;
		case nri_scene::MaterialEmissiveMode_UseConstantColor: emissiveConstantCount++; break;
		case nri_scene::MaterialEmissiveMode_UseGlowmapTexture: emissiveGlowmapCount++; break;
		default: break;
		}
	}
	Printf("NRI PT NRD: integration=%s requested=%s validation_output=%s denoiser=%s motion=%s prev_position=%s extra_debugs=%s\n",
		mNrd.IsReady() ? "ready" : "cold",
		nri_denoise ? "on" : "off",
		nri_validation ? "expected" : "disabled",
		GetNrdDenoiserModeName(denoiserSettings.denoiserMode),
		"2.5D",
		"interpolated",
		"16=denoised_diff 17=denoised_spec 18=metalness 19=roughness 20=motion_z 21=live_raw_penumbra 22=live_raw_shadow 23=temporal_sigma_shadow 24=direct_lighting 25=direct_emission 26=analytic_direct 27=emissive_tags 28=emissive_direct 29=sector_ambient 30=emissive_uv 31=emissive_radiance 32=emissive_primitive 33=emissive_visibility 34=trace_transparent 35=sr_input 36=sr_depth 37=vendor_output 38=vendor_output_final 39=rr_input 40=rr_diffuse_albedo 41=rr_specular_albedo 42=rr_normal_roughness 43=rr_specular_hit_distance 44=post_sharpen_output 45=taa_pre_exposed_input");
	const char* shadowSplitMode =
		!mUseSplitShadowDenoiser ? "off" :
		(GetEffectivePtDebugMode() >= 21 && GetEffectivePtDebugMode() <= 23) ? "sigma-debug" :
		"sigma-beauty";
	Printf("NRI PT NRD settings: max_frames=%u fast_frames=%u stabilization_frames=%u anti_firefly=%s hit_recon=%s input_split=%s shadow_split=%s\n",
		denoiserSettings.maxAccumulatedFrameNum,
		denoiserSettings.maxFastAccumulatedFrameNum,
		denoiserSettings.maxStabilizedFrameNum,
		denoiserSettings.enableAntiFirefly ? "on" : "off",
		GetNrdHitDistanceReconstructionModeName(denoiserSettings.hitDistanceReconstructionMode),
		GetNrdInputSplitModeName(denoiserSettings.inputSplitMode),
		shadowSplitMode);
	Printf("NRI PT SIGMA tuning: stabilization_frames=%u plane_distance_sensitivity=%.3f\n",
		denoiserSettings.sigmaMaxStabilizedFrameNum,
		denoiserSettings.sigmaPlaneDistanceSensitivity);
	if (denoiserSettings.denoiserMode == NRINrdDenoiserMode::Relax)
	{
		Printf("NRI PT NRD tuning: fast_history_sigma=%.2f prepass=%.2f/%.2f material_floor=1/2 blur_radius=n/a_relax\n",
			denoiserSettings.fastHistoryClampingSigmaScale,
			denoiserSettings.diffusePrepassBlurRadius,
			denoiserSettings.specularPrepassBlurRadius);
	}
	else
	{
		Printf("NRI PT NRD tuning: fast_history_sigma=%.2f blur_radius=%.2f..%.2f prepass=%.2f/%.2f material_floor=1/2\n",
			denoiserSettings.fastHistoryClampingSigmaScale,
			denoiserSettings.minBlurRadius,
			denoiserSettings.maxBlurRadius,
			denoiserSettings.diffusePrepassBlurRadius,
			denoiserSettings.specularPrepassBlurRadius);
	}
	Printf("NRI PT NRD guides: diffuse_signal=primary_demodulated_radiance specular_signal=primary_demodulated_radiance hit_distance=%s roughness=material_hint metalness=material_hint material_id=semantic_class\n",
		denoiserSettings.denoiserMode == NRINrdDenoiserMode::Relax ? "secondary_transport_linear_hitdist" : "secondary_transport_reblur_norm");
	Printf("NRI PT scene stats: %s\n", nri_ptscenestats ? "on" : "off");
	Printf("NRI PT mutation trace: chunk=%d sector=%d\n",
		(int)nri_ptmutationtracechunk,
		(int)nri_ptmutationtracesector);
	Printf("NRI PT runtime link trace: %s\n", nri_ptruntimelinktrace ? "on" : "off");
	Printf("NRI PT analytic lights: active=%u manual=%u muzzle_slots=%u muzzle_active=%u rules=%u topo_changed=%s prop_changed=%s added=%u removed=%u rebound=%u limit=%u\n",
		(uint32_t)mSceneLights.GetAnalyticLights().activeLights.size(),
		(uint32_t)mSceneLights.GetAnalyticLights().manualLights.size(),
		mSceneLights.GetAnalyticLights().transientMuzzleSlotCount,
		mSceneLights.GetAnalyticLights().transientMuzzleActiveCount,
		(uint32_t)mSceneLights.GetAnalyticLights().spriteTileRules.size(),
		YesNo(mSceneLights.GetAnalyticLights().lastBuildTopologyChanged),
		YesNo(mSceneLights.GetAnalyticLights().lastBuildPropertiesChanged),
		(uint32_t)mSceneLights.GetAnalyticLights().addedTopologyKeys.size(),
		(uint32_t)mSceneLights.GetAnalyticLights().removedTopologyKeys.size(),
		(uint32_t)mSceneLights.GetAnalyticLights().reboundTopologyKeys.size(),
		NRI_MAX_RUNTIME_POINT_LIGHTS);
	Printf("NRI PT analytic clusters: tile=%u grid=%ux%u used_indices=%u max_occupancy=%u debug_mode=%u\n",
		mBoundRuntimeLightTileSize,
		mBoundRuntimeLightTileCountX,
		mBoundRuntimeLightTileCountY,
		mBoundRuntimeLightTileIndexCount,
		mBoundRuntimeLightMaxTileOccupancy,
		nri_diag::PtDebugAnalyticDirect);
	Printf("NRI PT emissive surfaces: active=%u rules=%u auto=%u explicit=%u overrides=%u override_matches=%u material_response_rules=%u material_response_matches=%u total_power=%.3f topo_changed=%s prop_changed=%s added=%u removed=%u rebound=%u debug_mode=%u/%u thresholds=area>=%.3f power>=%.3f light=[%.3f,%.3f] reach=[%.3f,%.3f] glow_scale=%.3f glow_reach=%.3f glow_falloff=%.3f glow_blend=%.3f\n",
		(uint32_t)mSceneLights.GetEmissiveSurfaces().activeSurfaces.size(),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().textureRules.size(),
		mSceneLights.GetEmissiveSurfaces().autoTaggedCount,
		mSceneLights.GetEmissiveSurfaces().explicitRuleMatchCount,
		mSceneLights.GetEmissiveSurfaces().overrideRuleCount,
		mSceneLights.GetEmissiveSurfaces().overrideMatchedSurfaceCount,
		mSceneLights.GetEmissiveSurfaces().materialResponseRuleCount,
		mSceneLights.GetEmissiveSurfaces().materialResponseMatchedSurfaceCount,
		mSceneLights.GetEmissiveSurfaces().totalPowerEstimate,
		YesNo(mSceneLights.GetEmissiveSurfaces().lastBuildTopologyChanged),
		YesNo(mSceneLights.GetEmissiveSurfaces().lastBuildPropertiesChanged),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().addedTopologyKeys.size(),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().removedTopologyKeys.size(),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().reboundTopologyKeys.size(),
		nri_diag::PtDebugEmissiveTags,
		nri_diag::PtDebugEmissiveDirect,
		(float)nri_ptemissiveminsurface,
		(float)nri_ptemissiveminpower,
		(float)nri_ptsectoremissionlightmin,
		(float)nri_ptsectoremissionlightmax,
		(float)nri_ptsectoremissionreachmin,
		(float)nri_ptsectoremissionreachmax,
		(float)nri_ptglowscale,
		(float)nri_ptglowreach,
		(float)nri_ptglowfalloff,
		(float)nri_ptglowblend);
	const auto& appendStats = mSceneLights.GetFrameAppendStats();
	Printf("NRI PT scene-light ingest: records=%u static=%u mutation=%u captured=%u dynamic=%u append_ms=static:%.3f mutation:%.3f captured:%.3f dynamic:%.3f rebuild_ms=analytic:%.3f emissive:%.3f sector:%.3f\n",
		appendStats.totalRecordCount,
		appendStats.staticRecordCount,
		appendStats.runtimeMutationRecordCount,
		appendStats.capturedRecordCount,
		appendStats.dynamicRecordCount,
		mLastPerfShellTraceStats.sceneLightStaticAppendMs,
		mLastPerfShellTraceStats.sceneLightRuntimeMutationAppendMs,
		mLastPerfShellTraceStats.sceneLightCapturedAppendMs,
		mLastPerfShellTraceStats.sceneLightDynamicAppendMs,
		mLastPerfShellTraceStats.sceneLightAnalyticMs,
		mLastPerfShellTraceStats.sceneLightEmissiveMs,
		mLastPerfShellTraceStats.sceneLightSectorMs);
	Printf("NRI PT emissive sources: base=%u glowmap=%u constant=%u\n",
		emissiveBaseCount,
		emissiveGlowmapCount,
		emissiveConstantCount);
	Printf("NRI PT emissive sampling: primitives=%u total_power=%.3f samples=%u dominant_tile=%u dominant_primitive=%u dominant_source=%s dominant_power=%.3f dominant_flags=0x%x debug_mode=%u\n",
		mBoundEmissivePrimitiveCount,
		mBoundEmissiveTotalPower,
		traceSettings.emissiveSampleCount,
		mBoundEmissiveDominantTile,
		mBoundEmissiveDominantPrimitive,
		nri_diag::GetSceneDataSourceName(mBoundEmissiveDominantDataSource),
		mBoundEmissiveDominantPower,
		mBoundEmissiveDominantFlags,
		nri_diag::PtDebugEmissiveSampleVisibility);
	Printf("NRI PT emissive query: tlas=%s fast_shadow=%s instances=%u static=%u dynamic=%u builds=%u\n",
		nri_ptemissivetlas ? "on" : "off",
		nri_ptemissivefastshadow ? "on" : "off",
		mEmissiveTlasInstanceCount,
		mEmissiveTlasStaticInstanceCount,
		mEmissiveTlasDynamicInstanceCount,
		mEmissiveTlasBuildCount);
	Printf("NRI PT sector lighting: enabled=%s active=%u raw_active=%u raw_nonneutral=%u response=boost:%u dim:%u neutral:%u eligible=%u fog=%u pulsing=%u debug_mode=%u multiplier=%.3f scales=ambient=%.3f hemi=%.3f fog=%.3f clamp=%.3f sector_response=%.3f/[%.3f,%.3f] intensity=[%.3f,%.3f] reach=[%.3f,%.3f] filter=pal=%d shade=[%d,%d] lotag=%d pulse=%d/%.3f\n",
		nri_ptsectorlighting ? "on" : "off",
		mSceneLights.GetSectorLighting().activeSectorCount,
		mSceneLights.GetSectorLighting().rawActiveSectorCount,
		mSceneLights.GetSectorLighting().rawNonNeutralSectorCount,
		mSceneLights.GetSectorLighting().responseBoostSectorCount,
		mSceneLights.GetSectorLighting().responseDimSectorCount,
		mSceneLights.GetSectorLighting().responseNeutralSectorCount,
		mSceneLights.GetSectorLighting().eligibleSectorCount,
		mSceneLights.GetSectorLighting().fogSectorCount,
		mSceneLights.GetSectorLighting().pulsingSectorCount,
		nri_diag::PtDebugSectorAmbient,
		NRIGetSectorLightMultiplier(),
		(float)nri_ptsectorambientscale,
		(float)nri_ptsectorhemiscale,
		(float)nri_ptsectorfogscale,
		(float)nri_ptsectorclamp,
		(float)nri_ptsectoremissionsignalstrength,
		(float)nri_ptsectoremissionresponsemin,
		(float)nri_ptsectoremissionresponsemax,
		(float)nri_ptsectoremissionlightmin,
		(float)nri_ptsectoremissionlightmax,
		(float)nri_ptsectoremissionreachmin,
		(float)nri_ptsectoremissionreachmax,
		(int)nri_ptsectorfilterpal,
		(int)nri_ptsectorfilterminshade,
		(int)nri_ptsectorfiltermaxshade,
		(int)nri_ptsectorfilterlotag,
		(int)nri_ptsectorpulseframes,
		(float)nri_ptsectorpulseamount);
	Printf("NRI PT sector buffer: sectors=%u active=%u pulsing=%u dominant_sector=%u dominant_contribution=%.3f\n",
		mBoundSectorLightSectorCount,
		mBoundSectorLightActiveCount,
		mBoundSectorLightPulsingCount,
		mBoundSectorLightDominantSector != UINT32_MAX ? mBoundSectorLightDominantSector : 0u,
		mBoundSectorLightDominantContribution);
	if (nri_ptbootstrap)
	{
		Printf("NRI PT bootstrap mode: %u\n", bootstrapMode);
	}

	if (mHasLoggedStats)
	{
		const auto& stats = mLastStats;
		Printf("NRI PT last scene: walls=%u flats=%u sprites=%u translucent=%u models=%u voxel_proxies=%u unsupported_models=%u voxel_cache=candidates:%u uncacheable:%u hits:%u misses:%u changes:%u split_stable:%u split_live:%u entries:%u surface_hits:%u stores:%u rebuilds:%u transform_rebakes:%u removes:%u not_captured:%u cached_prims:%u mirrors=%u skies=%u portal_views=%u portal_skips=%u approx_tris=%u materials=%u\n",
			stats.wallDrawItems,
			stats.flatDrawItems,
			stats.spriteDrawItems,
			stats.translucentDrawItems,
			stats.modelDrawItems,
			stats.voxelProxyDrawItems,
			stats.unsupportedModelDrawItems,
			stats.voxelStableCandidates,
			stats.voxelStableUncacheable,
			stats.voxelStableSignatureHits,
			stats.voxelStableSignatureMisses,
			stats.voxelStableSignatureChanges,
			stats.voxelStableSplitStable,
			stats.voxelStableSplitLive,
			stats.voxelCacheEntries,
			stats.voxelCacheSurfaceHits,
			stats.voxelCacheSurfaceStores,
			stats.voxelCacheSurfaceRebuilds,
			stats.voxelCacheTransformRebakes,
			stats.voxelCacheSurfaceRemoves,
			stats.voxelCacheNotCaptured,
			stats.voxelCachePrimitives,
			stats.mirrorSurfaces,
			stats.skySurfaces,
			stats.portalViews,
			stats.portalCapturesSkipped,
			stats.triangleEstimate,
			stats.materialRefs);
	}
	else
	{
		Printf("NRI PT last scene: no translated PT scene has been captured yet.\n");
	}

	PrintMapWorldStatus();
	PrintPortalTraversalStatus();
	PrintStaticMapSceneStatus();
	PrintResidentMapChunkRegistryStatus();
	PrintDynamicSceneStatus();
	PrintTemporalStatus();
	mRuntimeMutation.PrintStatus();
	PrintRuntimeSpaceLinkStatus();
	PrintSceneBufferStatus();
	PrintSurfaceProbeStatus();
}

void NRIRenderer::TraceActorSpriteMaterialAssignments(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& outMaterials, const char* traceLabel)
{
	mDescriptorCoherencyDebugStats.actorMaterialBuilds++;
	mDescriptorCoherencyDebugStats.lastMaterialBuildLabel = traceLabel != nullptr ? traceLabel : "unlabeled";
	mDescriptorCoherencyDebugStats.lastMaterialCount = (uint32_t)outMaterials.materials.size();
	mDescriptorCoherencyDebugStats.lastTextureCount = (uint32_t)outMaterials.textures.size();
	mDescriptorCoherencyDebugStats.lastMaterialBridgeHash = HashMaterialBridgeSummary(outMaterials);

	const uint32_t spriteMaterialBase = (uint32_t)(sceneView.opaqueWalls.size() + sceneView.opaqueFlats.size());
	uint32_t actorSurfaceCount = 0;
	uint64_t actorHash = 1469598103934665603ull;
	std::unordered_set<int32_t> actorIndices;
	actorIndices.reserve(sceneView.opaqueSprites.size());
	uint32_t printed = 0;

	for (uint32_t spriteIndex = 0; spriteIndex < (uint32_t)sceneView.opaqueSprites.size(); ++spriteIndex)
	{
		const auto& surface = sceneView.opaqueSprites[spriteIndex];
		if (surface.provenance.actorIndex < 0)
		{
			continue;
		}

		const uint32_t materialIndex = spriteMaterialBase + spriteIndex;
		if (materialIndex >= outMaterials.materials.size() || materialIndex >= outMaterials.lightMetadata.size())
		{
			continue;
		}

		const auto& material = outMaterials.materials[materialIndex];
		const auto& metadata = outMaterials.lightMetadata[materialIndex];
		actorSurfaceCount++;
		actorIndices.insert(surface.provenance.actorIndex);
		actorHash = nri_scene::HashCombine64(actorHash, (uint64_t)surface.provenance.actorIndex);
		actorHash = nri_scene::HashCombine64(actorHash, (uint64_t)(uint32_t)surface.provenance.sourceType);
		actorHash = nri_scene::HashCombine64(actorHash, (uint64_t)materialIndex);
		actorHash = nri_scene::HashCombine64(actorHash, (uint64_t)metadata.textureId);
		actorHash = nri_scene::HashCombine64(actorHash, (uint64_t)material.textureIndex);
		actorHash = nri_scene::HashCombine64(actorHash, (uint64_t)material.paletteIndex);
		actorHash = nri_scene::HashCombine64(actorHash, (uint64_t)material.emissiveMode);
		actorHash = nri_scene::HashCombine64(actorHash, (uint64_t)material.emissiveTextureIndex);
		actorHash = nri_scene::HashCombine64(actorHash, metadata.materialKey);

		if (nri_actor_sprite_diag::ShouldTraceVerbose((int)nri_ptactorspritetrace, (int)nri_pttraceframes) && printed < 32)
		{
			Printf("NRI PT actor-sprite material: frame=%u label=%s actor=%d source=%s material=%u tex_id=%u tex_index=%u emissive_mode=%u emissive_tex=%u palette=%u flags=0x%x light_flags=0x%x material_key=0x%llx tex_ptr=%p\n",
				mFrameIndex,
				traceLabel != nullptr ? traceLabel : "unlabeled",
				surface.provenance.actorIndex,
				nri_diag::GetSurfaceSourceTypeName(surface.provenance.sourceType),
				materialIndex,
				metadata.textureId,
				material.textureIndex,
				material.emissiveMode,
				material.emissiveTextureIndex,
				material.paletteIndex,
				material.flags,
				material.lightingFlags,
				(unsigned long long)metadata.materialKey,
				metadata.texture);
			printed++;
		}
	}

	mDescriptorCoherencyDebugStats.lastActorSpriteSurfaceCount = actorSurfaceCount;
	mDescriptorCoherencyDebugStats.lastActorSpriteActorCount = (uint32_t)actorIndices.size();
	mDescriptorCoherencyDebugStats.lastActorSpriteMaterialHash = actorHash;

	if (actorSurfaceCount == 0 || !nri_actor_sprite_diag::ShouldTraceCoherency((int)nri_ptactorspritetrace, (int)nri_pttraceframes))
	{
		return;
	}

	Printf("NRI PT actor-sprite materials: frame=%u label=%s materials=%u textures=%u actor_surfaces=%u actor_count=%u bridge_hash=0x%llx actor_hash=0x%llx qframe=%u outstanding_slots=%u\n",
		mFrameIndex,
		traceLabel != nullptr ? traceLabel : "unlabeled",
		(uint32_t)outMaterials.materials.size(),
		(uint32_t)outMaterials.textures.size(),
		actorSurfaceCount,
		(uint32_t)actorIndices.size(),
		(unsigned long long)mDescriptorCoherencyDebugStats.lastMaterialBridgeHash,
		(unsigned long long)actorHash,
		mFrameBuffer != nullptr ? mFrameBuffer->mCurrentQueuedFrameIndex : 0u,
		CountPotentialOutstandingQueuedFrames());
}

void NRIRenderer::TraceActorSpriteEvent(const PathTracingActorSpriteTraceEvent& event)
{
	if (!nri_actor_sprite_diag::ShouldTraceVerbose((int)nri_ptactorspritetrace, (int)nri_pttraceframes))
	{
		return;
	}

	if (event.hasVoxelKeys)
	{
		Printf("NRI PT actor-sprite %s: actor=%d stat=%d pic=%d base_tex=%d resolved_tex=%d pal=%d shade=%d cstat=0x%x cstat2=0x%x noanimate=%s fullbright=%s drawlist=%u tex_ptr=%p voxel_action=%s voxel_mesh_key=0x%llx voxel_mat_key=0x%llx voxel_inst_key=0x%llx voxel_surface_sig=0x%llx\n",
			nri_actor_sprite_diag::GetTraceStageName(event.stage),
			event.actorIndex,
			event.spriteStatnum,
			event.spritePicnum,
			event.baseTextureId,
			event.resolvedTextureId,
			event.palette,
			event.shade,
			event.cstat,
			event.cstat2,
			event.noAnimate ? "yes" : "no",
			event.fullbright ? "yes" : "no",
			event.drawListType,
			event.resolvedGameTexture,
			event.voxelAction != nullptr ? event.voxelAction : "unknown",
			(unsigned long long)event.voxelMeshKeyHash,
			(unsigned long long)event.voxelMaterialKeyHash,
			(unsigned long long)event.voxelInstanceKeyHash,
			(unsigned long long)event.voxelSurfaceSignature);
		return;
	}

	Printf("NRI PT actor-sprite %s: actor=%d stat=%d pic=%d base_tex=%d resolved_tex=%d pal=%d shade=%d cstat=0x%x cstat2=0x%x noanimate=%s fullbright=%s drawlist=%u tex_ptr=%p\n",
		nri_actor_sprite_diag::GetTraceStageName(event.stage),
		event.actorIndex,
		event.spriteStatnum,
		event.spritePicnum,
		event.baseTextureId,
		event.resolvedTextureId,
		event.palette,
		event.shade,
		event.cstat,
		event.cstat2,
		event.noAnimate ? "yes" : "no",
		event.fullbright ? "yes" : "no",
		event.drawListType,
		event.resolvedGameTexture);
}

void NRIRenderer::PrintSurfaceProbeStatus() const
{
	if (!mLastSurfaceProbe.valid)
	{
		Printf("NRI PT surface probe: no sampled center hit has been recorded yet.\n");
		return;
	}

	if (!mLastSurfaceProbe.hit)
	{
		Printf("NRI PT surface probe: last sampled center ray missed translated PT geometry.\n");
		return;
	}

	const SurfaceProbeEmissiveDiagnostics emissiveDiagnostics = BuildSurfaceProbeEmissiveDiagnostics(mLastSurfaceProbe);
	const uint32_t flags = mLastSurfaceProbe.primitiveFlags;
	const uint32_t lightingFlags = mLastSurfaceProbe.materialLightingFlags;
	const int32_t localSpaceIndex = mLastSurfaceProbe.provenance.mapChunkIndex >= 0 ? nri_scene::FindMapWorldLocalSpaceIndex(mMapWorld, (uint32_t)mLastSurfaceProbe.provenance.mapChunkIndex) : -1;
	const int32_t portalGraphIndex = nri_scene::FindMapWorldPortalIndex(mMapWorld, mLastSurfaceProbe.provenance);
	bool chunkResidentStatic = false;
	bool chunkStaticTlasInstanced = false;
	bool chunkStaticProbeIncluded = false;
	bool chunkVisibleGate = false;
	bool flatPlaneVisibilityRelevant = false;
	bool flatPlaneVisible = false;
	bool chunkReplaced = false;
	bool chunkSectorDirty = false;
	bool chunkDragged = false;
	bool chunkBlindSpot = false;
	uint32_t chunkReasonMask = 0;
	uint32_t chunkSectionDirtyCount = 0;
	uint32_t replacementSurfaceCount = 0;
	uint32_t replacementTriangleCount = 0;
	if (mLastSurfaceProbe.provenance.mapChunkIndex >= 0)
	{
		const uint32_t chunkIndex = (uint32_t)mLastSurfaceProbe.provenance.mapChunkIndex;
		chunkVisibleGate = IsChunkMarkedVisible(mCurrentVisibleChunkWords, chunkIndex);
		for (const auto& chunkCache : mStaticMapScene.chunks)
		{
			if (chunkCache.chunkIndex == chunkIndex)
			{
				chunkResidentStatic = true;
				chunkStaticTlasInstanced = true;
				chunkStaticProbeIncluded = true;
				break;
			}
		}
		if (const auto* replacement = mRuntimeMutation.FindReplacement(chunkIndex))
		{
			chunkReplaced = replacement->active;
			chunkSectorDirty = replacement->sectorDirty;
			chunkDragged = replacement->dragged;
			chunkBlindSpot = replacement->blindSpot;
			chunkReasonMask = replacement->reasonMask;
			chunkSectionDirtyCount = replacement->sectionDirtyCount;
			replacementSurfaceCount = replacement->surfaceCount;
			replacementTriangleCount = replacement->triangleCount;
		}
	}
	if ((flags & nri_scene::MaterialFlag_Flat) != 0 &&
		(flags & (nri_scene::MaterialFlag_Sprite | nri_scene::MaterialFlag_Mirror | nri_scene::MaterialFlag_Sky | nri_scene::MaterialFlag_Portal)) == 0 &&
		mLastSurfaceProbe.provenance.sectorIndex >= 0)
	{
		flatPlaneVisibilityRelevant = true;
		flatPlaneVisible = IsFlatPlaneMarkedVisible(mCurrentVisibleFlatPlaneWords, mLastSurfaceProbe.provenance.sectorIndex, mLastSurfaceProbe.normal[1] < 0.0f);
	}
	const std::string chunkReasons = GetRuntimeMapMutationReasonSummary(chunkReasonMask);
	Printf("NRI PT surface probe: source=%s drawlist=%s owner=%s data_source=%s chunk=%d gate_visible=%s flat_drawlist_visible=%s static_resident=%s static_tlas_instanced=%s static_probe_included=%s chunk_replaced=%s chunk_reasons=%s section_dirty=%u sector_dirty=%s dragged=%s blind_spot=%s replacement_surfaces=%u replacement_tris=%u local_space=%d portal_graph=%d sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x primitive=%u material=%u tile=%u material_tile=%u distance=%.2f pos=(%.2f, %.2f, %.2f) flags=0x%x indexed=%s fullbright=%s flat=%s sprite=%s mirror=%s sky=%s portal=%s facing_billboard=%s point_sampled=%s tex_fullbright=%s glowing=%s auto_glow=%s glowmap=%s normalmap=%s metallic=%s roughness=%s normal_tex=%u metallic_tex=%u roughness_tex=%u metalness_hint=%.3f roughness_hint=%.3f material_class=%u emissive_mode=%s emissive_tex=%u light_surface=%s light_mat=%u emissive_surface=%s emissive_prims=%u emissive_hit=%s emissive_flags=0x%x emissive_rule=%u emissive_override=%u emissive_sector=%d sector_scale=%.3f sector_reach=%.3f sector_applied=%s emissive_area=%.2f emissive_power=%.3f emissive_sample_weight=%.3f emissive_pdf=%.6f emissive_intensity=%.3f material_response=%s material_scale=%.3f light=%.3f alpha=%.3f avg=(%.2f, %.2f, %.2f) emissive=(%.2f, %.2f, %.2f) glow=(%.2f, %.2f, %.2f)\n",
		nri_diag::GetSurfaceSourceTypeName(mLastSurfaceProbe.provenance.sourceType),
		nri_diag::GetDrawListTypeName(mLastSurfaceProbe.provenance.drawListType),
		nri_diag::GetSurfaceProbeSceneOwnerName(mLastSurfaceProbe.sceneOwner),
		nri_diag::GetSceneDataSourceName(mLastSurfaceProbe.sceneDataSource),
		mLastSurfaceProbe.provenance.mapChunkIndex,
		YesNo(chunkVisibleGate),
		flatPlaneVisibilityRelevant ? YesNo(flatPlaneVisible) : "n/a",
		YesNo(chunkResidentStatic),
		YesNo(chunkStaticTlasInstanced),
		YesNo(chunkStaticProbeIncluded),
		YesNo(chunkReplaced),
		chunkReasons.c_str(),
		chunkSectionDirtyCount,
		YesNo(chunkSectorDirty),
		YesNo(chunkDragged),
		YesNo(chunkBlindSpot),
		replacementSurfaceCount,
		replacementTriangleCount,
		localSpaceIndex,
		portalGraphIndex,
		mLastSurfaceProbe.provenance.sectorIndex,
		mLastSurfaceProbe.provenance.wallIndex,
		mLastSurfaceProbe.provenance.nextSectorIndex,
		mLastSurfaceProbe.provenance.actorIndex,
		mLastSurfaceProbe.provenance.cstat,
		mLastSurfaceProbe.primitiveIndex,
		mLastSurfaceProbe.materialIndex,
		mLastSurfaceProbe.textureId,
		mLastSurfaceProbe.baseTextureId,
		mLastSurfaceProbe.distance,
		mLastSurfaceProbe.position[0],
		mLastSurfaceProbe.position[1],
		mLastSurfaceProbe.position[2],
		flags,
		YesNo((flags & nri_scene::MaterialFlag_Indexed) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Fullbright) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Flat) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Sprite) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Mirror) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Sky) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Portal) != 0),
		YesNo((flags & nri_scene::MaterialFlag_FacingBillboard) != 0),
		YesNo((flags & nri_scene::MaterialFlag_PointSampled) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureFullbright) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureGlowing) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureAutoGlowing) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_HasGlowmap) != 0),
		YesNo(mLastSurfaceProbe.normalTextureIndex != UINT32_MAX),
		YesNo(mLastSurfaceProbe.metallicTextureIndex != UINT32_MAX),
		YesNo(mLastSurfaceProbe.roughnessTextureIndex != UINT32_MAX),
		mLastSurfaceProbe.normalTextureIndex != UINT32_MAX ? mLastSurfaceProbe.normalTextureIndex : 0u,
		mLastSurfaceProbe.metallicTextureIndex != UINT32_MAX ? mLastSurfaceProbe.metallicTextureIndex : 0u,
		mLastSurfaceProbe.roughnessTextureIndex != UINT32_MAX ? mLastSurfaceProbe.roughnessTextureIndex : 0u,
		mLastSurfaceProbe.metalnessHint,
		mLastSurfaceProbe.roughnessHint,
		mLastSurfaceProbe.materialClass,
		nri_diag::GetMaterialEmissiveModeName(mLastSurfaceProbe.emissiveMode),
		mLastSurfaceProbe.emissiveTextureIndex != UINT32_MAX ? mLastSurfaceProbe.emissiveTextureIndex : 0u,
		YesNo(emissiveDiagnostics.sceneLightSurfaceMatch),
		emissiveDiagnostics.sceneLightMaterialIndex != UINT32_MAX ? emissiveDiagnostics.sceneLightMaterialIndex : 0u,
		YesNo(emissiveDiagnostics.activeEmissiveSurfaceMatch),
		emissiveDiagnostics.emissivePrimitiveMatchCount,
		YesNo(emissiveDiagnostics.exactEmissivePrimitiveMatch),
		emissiveDiagnostics.emissiveSourceFlags,
		emissiveDiagnostics.emissiveSourceRuleId,
		emissiveDiagnostics.emissiveOverrideRuleId,
		emissiveDiagnostics.emissiveSectorIndex,
		emissiveDiagnostics.sectorResponseScale,
		emissiveDiagnostics.sectorReachScale,
		YesNo(emissiveDiagnostics.sectorResponseApplied),
		emissiveDiagnostics.emissivePrimitiveArea,
		emissiveDiagnostics.emissivePowerEstimate,
		emissiveDiagnostics.emissiveSelectionWeight,
		emissiveDiagnostics.emissiveSelectionPdf,
		emissiveDiagnostics.emissiveIntensity,
		YesNo(emissiveDiagnostics.materialResponseEnabled),
		emissiveDiagnostics.materialResponseScale,
		mLastSurfaceProbe.lightLevel,
		mLastSurfaceProbe.alpha,
		mLastSurfaceProbe.averageColor[0],
		mLastSurfaceProbe.averageColor[1],
		mLastSurfaceProbe.averageColor[2],
		mLastSurfaceProbe.emissiveColor[0],
		mLastSurfaceProbe.emissiveColor[1],
		mLastSurfaceProbe.emissiveColor[2],
		mLastSurfaceProbe.glowColor[0],
		mLastSurfaceProbe.glowColor[1],
		mLastSurfaceProbe.glowColor[2]);
}

void NRIRenderer::PrintMapChunkDump(int32_t chunkIndex) const
{
	if (!mMapWorld.valid)
	{
		Printf("NRI PT chunk dump: no authoritative map world has been built yet.\n");
		return;
	}

	if (chunkIndex < 0)
	{
		if (mLastSurfaceProbe.valid && mLastSurfaceProbe.hit && mLastSurfaceProbe.provenance.mapChunkIndex >= 0)
		{
			chunkIndex = mLastSurfaceProbe.provenance.mapChunkIndex;
		}
		else
		{
			Printf("NRI PT chunk dump: no chunk was specified and the last surface probe hit did not resolve to a map chunk.\n");
			return;
		}
	}

	if (chunkIndex < 0 || (unsigned)chunkIndex >= mMapWorld.chunks.size())
	{
		Printf("NRI PT chunk dump: chunk %d is out of range [0,%u).\n", chunkIndex, (uint32_t)mMapWorld.chunks.size());
		return;
	}

	const auto& chunk = mMapWorld.chunks[(unsigned)chunkIndex];
	const uint32_t preferredChunkListIndex = FindPreferredStaticSceneChunkListIndex((uint32_t)chunkIndex);
	const uint32_t duplicateChunkSlotCount = CountStaticSceneChunkSlots((uint32_t)chunkIndex);
	const StaticMapSceneCache::ChunkCache* staticChunk =
		preferredChunkListIndex < mStaticMapScene.chunks.size() ?
		&mStaticMapScene.chunks[preferredChunkListIndex] :
		nullptr;
	const bool residentStatic = staticChunk != nullptr;
	const bool staticTlasInstanced =
		staticChunk != nullptr &&
		staticChunk->active &&
		staticChunk->accelerationStructure.accelerationStructure != nullptr;
	const bool staticProbeIncluded = staticChunk != nullptr && staticChunk->active;
	const auto* replacement = mRuntimeMutation.FindReplacement((uint32_t)chunkIndex);

	uint32_t portalSurfaceCount = 0;
	uint32_t skySurfaceCount = 0;
	uint32_t surfaceTriangleCount = 0;
	for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < chunk.surfaceCount; ++localSurfaceIndex)
	{
		const uint32_t surfaceIndex = chunk.firstSurface + localSurfaceIndex;
		if (surfaceIndex >= mMapWorld.surfaces.size())
		{
			break;
		}

		const auto& surface = mMapWorld.surfaces[surfaceIndex];
		surfaceTriangleCount += chunk_diag::CountSurfaceTriangles(surface.surface);
		if ((surface.surface.material.flags & (nri_scene::MaterialFlag_Portal | nri_scene::MaterialFlag_Mirror)) != 0)
		{
			portalSurfaceCount++;
		}
		if ((surface.surface.material.flags & nri_scene::MaterialFlag_Sky) != 0)
		{
			skySurfaceCount++;
		}
	}

	uint32_t sourcePortalCount = 0;
	for (const auto& portal : mMapWorld.portals)
	{
		if (portal.sourceChunkIndex == (uint32_t)chunkIndex)
		{
			sourcePortalCount++;
		}
	}

	Printf("NRI PT chunk dump: chunk=%d sector=%d local_space=%u surfaces=%u tris=%u portal_surfaces=%u sky_surfaces=%u source_portals=%u resident_static=%s static_tlas_instanced=%s static_probe_included=%s runtime_replaced=%s replacement_reasons=%s section_dirty=%u sector_dirty=%s dragged=%s blind_spot=%s replacement_surfaces=%u replacement_tris=%u\n",
		chunkIndex,
		chunk.sectorIndex,
		chunk.localSpaceIndex,
		chunk.surfaceCount,
		surfaceTriangleCount,
		portalSurfaceCount,
		skySurfaceCount,
		sourcePortalCount,
		YesNo(residentStatic),
		YesNo(staticTlasInstanced),
		YesNo(staticProbeIncluded),
		YesNo(replacement != nullptr && replacement->active),
		replacement != nullptr ? GetRuntimeMapMutationReasonSummary(replacement->reasonMask).c_str() : "none",
		replacement != nullptr ? replacement->sectionDirtyCount : 0u,
		YesNo(replacement != nullptr && replacement->sectorDirty),
		YesNo(replacement != nullptr && replacement->dragged),
		YesNo(replacement != nullptr && replacement->blindSpot),
		replacement != nullptr ? replacement->surfaceCount : 0u,
		replacement != nullptr ? replacement->triangleCount : 0u);

	if (duplicateChunkSlotCount > 1)
	{
		Printf("NRI PT chunk dump slots: chunk=%d duplicate_slots=%u preferred_slot=%u\n",
			chunkIndex,
			duplicateChunkSlotCount,
			preferredChunkListIndex);
	}

	if (staticChunk != nullptr)
	{
		Printf("NRI PT chunk dump static: primitive_offset=%u primitive_count=%u material_offset=%u material_count=%u as_ready=%s\n",
			staticChunk->primitiveOffset,
			staticChunk->primitiveCount,
			staticChunk->materialOffset,
			staticChunk->materialCount,
			YesNo(staticChunk->accelerationStructure.accelerationStructure != nullptr));
	}

	for (const auto& portal : mMapWorld.portals)
	{
		if (portal.sourceChunkIndex != (uint32_t)chunkIndex)
		{
			continue;
		}

		Printf("NRI PT chunk portal: portal=%u source_surface=%u source_sector=%d source_wall=%d source_plane=%d target_count=%u runtime_bound=%s delta=(%.2f, %.2f, %.2f)\n",
			portal.portalIndex,
			portal.sourceSurfaceIndex,
			portal.sourceSectorIndex,
			portal.sourceWallIndex,
			portal.sourcePlane,
			portal.targetCount,
			YesNo(portal.runtimeBoundTarget),
			(float)portal.delta[0],
			(float)portal.delta[1],
			(float)portal.delta[2]);
	}

	for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < chunk.surfaceCount; ++localSurfaceIndex)
	{
		const uint32_t surfaceIndex = chunk.firstSurface + localSurfaceIndex;
		if (surfaceIndex >= mMapWorld.surfaces.size())
		{
			break;
		}

		const auto& surface = mMapWorld.surfaces[surfaceIndex];
		const uint32_t flags = surface.surface.material.flags;
		const uint32_t textureId =
			surface.surface.material.texture != nullptr ?
			(uint32_t)surface.surface.material.texture->GetID().GetIndex() :
			0u;
		Printf("NRI PT chunk surface %u: kind=%s source=%s section=%d sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x flags=0x%x flat=%s sprite=%s mirror=%s sky=%s portal=%s one_way=%s facing_billboard=%s point_sampled=%s tile=%u pal=%d shade=%d alpha=%.3f verts=%u tris=%u\n",
			surfaceIndex,
			nri_diag::GetMapSurfaceKindName(surface.kind),
			nri_diag::GetSurfaceSourceTypeName(surface.surface.provenance.sourceType),
			surface.surface.provenance.sectionIndex,
			surface.surface.provenance.sectorIndex,
			surface.surface.provenance.wallIndex,
			surface.surface.provenance.nextSectorIndex,
			surface.surface.provenance.actorIndex,
			surface.surface.provenance.cstat,
			flags,
			YesNo((flags & nri_scene::MaterialFlag_Flat) != 0),
			YesNo((flags & nri_scene::MaterialFlag_Sprite) != 0),
			YesNo((flags & nri_scene::MaterialFlag_Mirror) != 0),
			YesNo((flags & nri_scene::MaterialFlag_Sky) != 0),
			YesNo((flags & nri_scene::MaterialFlag_Portal) != 0),
			YesNo((flags & nri_scene::MaterialFlag_OneWay) != 0),
			YesNo((flags & nri_scene::MaterialFlag_FacingBillboard) != 0),
			YesNo((flags & nri_scene::MaterialFlag_PointSampled) != 0),
			textureId,
			surface.surface.material.palette,
			surface.surface.material.shade,
			surface.surface.material.alpha,
			(uint32_t)surface.surface.vertices.size(),
			chunk_diag::CountSurfaceTriangles(surface.surface));
	}
}

void NRIRenderer::PrintMapChunkCompare(int32_t chunkIndex) const
{
	if (!mMapWorld.valid)
	{
		Printf("NRI PT chunk compare: no authoritative map world has been built yet.\n");
		return;
	}

	if (chunkIndex < 0)
	{
		if (mLastSurfaceProbe.valid && mLastSurfaceProbe.hit && mLastSurfaceProbe.provenance.mapChunkIndex >= 0)
		{
			chunkIndex = mLastSurfaceProbe.provenance.mapChunkIndex;
		}
		else
		{
			Printf("NRI PT chunk compare: no chunk was specified and the last surface probe hit did not resolve to a map chunk.\n");
			return;
		}
	}

	if (chunkIndex < 0 || (unsigned)chunkIndex >= mMapWorld.chunks.size())
	{
		Printf("NRI PT chunk compare: chunk %d is out of range [0,%u).\n", chunkIndex, (uint32_t)mMapWorld.chunks.size());
		return;
	}

	const auto& staticChunk = mMapWorld.chunks[(unsigned)chunkIndex];
	nri_scene::PTMapWorld liveWorld = {};
	nri_scene::PTMapWorldStats liveStats = {};
	if (!nri_scene::BuildLiveMapChunkWorld(staticChunk, liveWorld, &liveStats) ||
		liveWorld.chunks.empty())
	{
		Printf("NRI PT chunk compare: failed to build live runtime chunk %d.\n", chunkIndex);
		return;
	}

	const auto& liveChunk = liveWorld.chunks[0];
	const auto* replacement = mRuntimeMutation.FindReplacement((uint32_t)chunkIndex);

	std::vector<uint32_t> staticSurfaceIndices;
	std::vector<uint32_t> liveSurfaceIndices;
	staticSurfaceIndices.reserve(staticChunk.surfaceCount);
	liveSurfaceIndices.reserve(liveChunk.surfaceCount);

	for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < staticChunk.surfaceCount; ++localSurfaceIndex)
	{
		const uint32_t surfaceIndex = staticChunk.firstSurface + localSurfaceIndex;
		if (surfaceIndex >= mMapWorld.surfaces.size())
		{
			break;
		}
		staticSurfaceIndices.push_back(surfaceIndex);
	}

	for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < liveChunk.surfaceCount; ++localSurfaceIndex)
	{
		const uint32_t surfaceIndex = liveChunk.firstSurface + localSurfaceIndex;
		if (surfaceIndex >= liveWorld.surfaces.size())
		{
			break;
		}
		liveSurfaceIndices.push_back(surfaceIndex);
	}

	std::unordered_map<chunk_diag::SurfaceKey, std::vector<uint32_t>, chunk_diag::SurfaceKeyHash> liveSurfaceLookup;
	liveSurfaceLookup.reserve(liveSurfaceIndices.size());
	for (uint32_t liveLocalIndex = 0; liveLocalIndex < (uint32_t)liveSurfaceIndices.size(); ++liveLocalIndex)
	{
		const auto& liveSurface = liveWorld.surfaces[liveSurfaceIndices[liveLocalIndex]];
		liveSurfaceLookup[chunk_diag::BuildSurfaceKey(liveSurface)].push_back(liveLocalIndex);
	}

	std::vector<uint8_t> liveSurfaceUsed(liveSurfaceIndices.size(), 0u);
	std::vector<chunk_diag::MatchRecord> matches;
	std::vector<uint32_t> unmatchedStaticSurfaceIndices;
	std::vector<uint32_t> unmatchedLiveSurfaceIndices;
	matches.reserve(std::min(staticSurfaceIndices.size(), liveSurfaceIndices.size()));
	unmatchedStaticSurfaceIndices.reserve(staticSurfaceIndices.size());
	unmatchedLiveSurfaceIndices.reserve(liveSurfaceIndices.size());

	for (uint32_t staticSurfaceIndex : staticSurfaceIndices)
	{
		const auto& staticSurface = mMapWorld.surfaces[staticSurfaceIndex];
		const chunk_diag::SurfaceKey key = chunk_diag::BuildSurfaceKey(staticSurface);
		auto it = liveSurfaceLookup.find(key);
		if (it == liveSurfaceLookup.end())
		{
			unmatchedStaticSurfaceIndices.push_back(staticSurfaceIndex);
			continue;
		}

		uint32_t matchedLiveLocalIndex = UINT32_MAX;
		for (uint32_t candidate : it->second)
		{
			if (candidate < liveSurfaceUsed.size() && liveSurfaceUsed[candidate] == 0u)
			{
				matchedLiveLocalIndex = candidate;
				break;
			}
		}
		if (matchedLiveLocalIndex == UINT32_MAX)
		{
			unmatchedStaticSurfaceIndices.push_back(staticSurfaceIndex);
			continue;
		}

		liveSurfaceUsed[matchedLiveLocalIndex] = 1u;
		const uint32_t liveSurfaceIndex = liveSurfaceIndices[matchedLiveLocalIndex];
		const auto& liveSurface = liveWorld.surfaces[liveSurfaceIndex];

		chunk_diag::MatchRecord match = {};
		match.staticSurfaceIndex = staticSurfaceIndex;
		match.liveSurfaceIndex = liveSurfaceIndex;
		match.key = key;
		match.staticMetrics = chunk_diag::ComputeSurfaceMetrics(staticSurface);
		match.liveMetrics = chunk_diag::ComputeSurfaceMetrics(liveSurface);
		for (int axis = 0; axis < 3; ++axis)
		{
			match.delta[axis] = match.liveMetrics.centroid[axis] - match.staticMetrics.centroid[axis];
		}
		match.deltaDistance = chunk_diag::Distance3(match.liveMetrics.centroid, match.staticMetrics.centroid);
		if (match.staticMetrics.area > 0.0001f)
		{
			match.areaRatio = match.liveMetrics.area / match.staticMetrics.area;
		}
		else
		{
			match.areaRatio = match.liveMetrics.area > 0.0001f ? 9999.0f : 1.0f;
		}

		const float staticNormalLength = std::sqrt(chunk_diag::Dot3(match.staticMetrics.normal, match.staticMetrics.normal));
		const float liveNormalLength = std::sqrt(chunk_diag::Dot3(match.liveMetrics.normal, match.liveMetrics.normal));
		if (staticNormalLength > 0.0001f && liveNormalLength > 0.0001f)
		{
			match.normalDot = std::max(-1.0f, std::min(1.0f, chunk_diag::Dot3(match.staticMetrics.normal, match.liveMetrics.normal)));
		}
		else
		{
			match.normalDot = staticNormalLength <= 0.0001f && liveNormalLength <= 0.0001f ? 1.0f : 0.0f;
		}

		match.materialScore =
			(match.staticMetrics.textureId == match.liveMetrics.textureId ? 0.0f : 1.0f) +
			(match.staticMetrics.palette == match.liveMetrics.palette ? 0.0f : 1.0f) +
			(match.staticMetrics.shade == match.liveMetrics.shade ? 0.0f : 1.0f) +
			(match.staticMetrics.materialFlags == match.liveMetrics.materialFlags ? 0.0f : 1.0f) +
			(std::fabs(match.staticMetrics.alpha - match.liveMetrics.alpha) > 0.001f ? 1.0f : 0.0f);
		matches.push_back(match);
	}

	for (uint32_t liveLocalIndex = 0; liveLocalIndex < (uint32_t)liveSurfaceIndices.size(); ++liveLocalIndex)
	{
		if (liveSurfaceUsed[liveLocalIndex] == 0u)
		{
			unmatchedLiveSurfaceIndices.push_back(liveSurfaceIndices[liveLocalIndex]);
		}
	}

	float meanDelta[3] = {};
	for (const auto& match : matches)
	{
		meanDelta[0] += match.delta[0];
		meanDelta[1] += match.delta[1];
		meanDelta[2] += match.delta[2];
	}
	if (!matches.empty())
	{
		const float invMatchCount = 1.0f / (float)matches.size();
		meanDelta[0] *= invMatchCount;
		meanDelta[1] *= invMatchCount;
		meanDelta[2] *= invMatchCount;
	}

	std::unordered_map<int32_t, uint32_t> sectorChunkLookup;
	sectorChunkLookup.reserve(mMapWorld.chunks.size());
	for (const auto& mapChunk : mMapWorld.chunks)
	{
		if (mapChunk.sectorIndex >= 0)
		{
			sectorChunkLookup.emplace(mapChunk.sectorIndex, mapChunk.chunkIndex);
		}
	}

	uint32_t within1 = 0;
	uint32_t within4 = 0;
	uint32_t areaOutlierCount = 0;
	uint32_t normalOutlierCount = 0;
	uint32_t materialDiffCount = 0;
	uint32_t seamSurfaceCount = 0;
	uint32_t seamOutlierCount = 0;
	uint32_t seamAgainstStaticCount = 0;
	uint32_t seamAgainstReplacedCount = 0;
	for (auto& match : matches)
	{
		const float meanDeltaPoint[3] = { meanDelta[0], meanDelta[1], meanDelta[2] };
		match.deviationFromMean = chunk_diag::Distance3(match.delta, meanDeltaPoint);
		const float areaDelta = std::fabs(match.areaRatio - 1.0f);
		if (match.deviationFromMean <= 1.0f)
		{
			within1++;
		}
		if (match.deviationFromMean <= 4.0f)
		{
			within4++;
		}
		if (areaDelta > 0.05f)
		{
			areaOutlierCount++;
		}
		if (match.normalDot < 0.98f)
		{
			normalOutlierCount++;
		}
		if (match.materialScore > 0.0f)
		{
			materialDiffCount++;
		}
		match.score = match.deviationFromMean + areaDelta * 10.0f + (1.0f - match.normalDot) * 10.0f + match.materialScore;

		const auto& staticSurface = mMapWorld.surfaces[match.staticSurfaceIndex];
		if (staticSurface.surface.provenance.nextSectorIndex >= 0 &&
			staticSurface.kind != nri_scene::PTMapSurfaceKind::Floor &&
			staticSurface.kind != nri_scene::PTMapSurfaceKind::Ceiling &&
			staticSurface.kind != nri_scene::PTMapSurfaceKind::Portal)
		{
			seamSurfaceCount++;
			auto adjacentChunkIt = sectorChunkLookup.find(staticSurface.surface.provenance.nextSectorIndex);
			const bool adjacentReplaced =
				adjacentChunkIt != sectorChunkLookup.end() &&
				mRuntimeMutation.IsReplacementActive(adjacentChunkIt->second);
			if (adjacentReplaced)
			{
				seamAgainstReplacedCount++;
			}
			else
			{
				seamAgainstStaticCount++;
			}

			if (match.deviationFromMean > 0.5f)
			{
				seamOutlierCount++;
			}
		}
	}

	std::sort(matches.begin(), matches.end(), [](const chunk_diag::MatchRecord& a, const chunk_diag::MatchRecord& b)
	{
		return a.score > b.score;
	});

	const bool likelyCoherent =
		!matches.empty() &&
		unmatchedStaticSurfaceIndices.empty() &&
		unmatchedLiveSurfaceIndices.empty() &&
		within4 + std::max<uint32_t>(1u, (uint32_t)matches.size() / 10u) >= (uint32_t)matches.size() &&
		areaOutlierCount == 0 &&
		normalOutlierCount == 0;

	Printf("NRI PT chunk compare: chunk=%d sector=%d static_surfaces=%u live_surfaces=%u matched=%u unmatched_static=%u unmatched_live=%u reasons=%s dragged=%s replacement_active=%s mean_delta=(%.2f, %.2f, %.2f) within_1=%u within_4=%u area_outliers=%u normal_outliers=%u material_diffs=%u likely_coherent=%s live_tris=%u\n",
		chunkIndex,
		staticChunk.sectorIndex,
		(uint32_t)staticSurfaceIndices.size(),
		(uint32_t)liveSurfaceIndices.size(),
		(uint32_t)matches.size(),
		(uint32_t)unmatchedStaticSurfaceIndices.size(),
		(uint32_t)unmatchedLiveSurfaceIndices.size(),
		replacement != nullptr ? GetRuntimeMapMutationReasonSummary(replacement->reasonMask).c_str() : "none",
		YesNo(replacement != nullptr && replacement->dragged),
		YesNo(replacement != nullptr && replacement->active),
		meanDelta[0],
		meanDelta[1],
		meanDelta[2],
		within1,
		within4,
		areaOutlierCount,
		normalOutlierCount,
		materialDiffCount,
		YesNo(likelyCoherent),
		liveChunk.triangleCount);
	Printf("NRI PT chunk seam compare: chunk=%d border_surfaces=%u seam_outliers=%u adjacent_static=%u adjacent_replaced=%u\n",
		chunkIndex,
		seamSurfaceCount,
		seamOutlierCount,
		seamAgainstStaticCount,
		seamAgainstReplacedCount);

	const size_t outlierCount = std::min<size_t>(matches.size(), 8u);
	for (size_t i = 0; i < outlierCount; ++i)
	{
		const auto& match = matches[i];
		if (match.score <= 0.01f && likelyCoherent)
		{
			break;
		}

		const auto& staticSurface = mMapWorld.surfaces[match.staticSurfaceIndex];
		const auto& liveSurface = liveWorld.surfaces[match.liveSurfaceIndex];
		Printf("NRI PT chunk compare match: static_surface=%u live_surface=%u kind=%s source=%s sector=%d wall=%d section=%d nextsector=%d cstat=0x%x delta=(%.2f, %.2f, %.2f) dev=%.2f area_ratio=%.3f normal_dot=%.3f tile_static=%u tile_live=%u flags_static=0x%x flags_live=0x%x\n",
			match.staticSurfaceIndex,
			match.liveSurfaceIndex,
			nri_diag::GetMapSurfaceKindName(staticSurface.kind),
			nri_diag::GetSurfaceSourceTypeName(staticSurface.surface.provenance.sourceType),
			staticSurface.surface.provenance.sectorIndex,
			staticSurface.surface.provenance.wallIndex,
			staticSurface.surface.provenance.sectionIndex,
			staticSurface.surface.provenance.nextSectorIndex,
			staticSurface.surface.provenance.cstat,
			match.delta[0],
			match.delta[1],
			match.delta[2],
			match.deviationFromMean,
			match.areaRatio,
			match.normalDot,
			match.staticMetrics.textureId,
			match.liveMetrics.textureId,
			staticSurface.surface.material.flags,
			liveSurface.surface.material.flags);
	}

	size_t seamPrinted = 0;
	for (const auto& match : matches)
	{
		if (seamPrinted >= 8u)
		{
			break;
		}

		const auto& staticSurface = mMapWorld.surfaces[match.staticSurfaceIndex];
		if (staticSurface.surface.provenance.nextSectorIndex < 0 ||
			staticSurface.kind == nri_scene::PTMapSurfaceKind::Floor ||
			staticSurface.kind == nri_scene::PTMapSurfaceKind::Ceiling ||
			staticSurface.kind == nri_scene::PTMapSurfaceKind::Portal)
		{
			continue;
		}

		auto adjacentChunkIt = sectorChunkLookup.find(staticSurface.surface.provenance.nextSectorIndex);
		const int32_t adjacentChunkIndex = adjacentChunkIt != sectorChunkLookup.end() ? (int32_t)adjacentChunkIt->second : -1;
		const bool adjacentReplaced =
			adjacentChunkIndex >= 0 &&
			mRuntimeMutation.IsReplacementActive((uint32_t)adjacentChunkIndex);
		const bool seamOutlier = match.deviationFromMean > 0.5f;
		if (!seamOutlier && seamPrinted >= 4u)
		{
			continue;
		}

		Printf("NRI PT chunk seam match: static_surface=%u live_surface=%u kind=%s wall=%d nextsector=%d adjacent_chunk=%d adjacent_replaced=%s delta=(%.2f, %.2f, %.2f) dev=%.2f area_ratio=%.3f normal_dot=%.3f seam_outlier=%s\n",
			match.staticSurfaceIndex,
			match.liveSurfaceIndex,
			nri_diag::GetMapSurfaceKindName(staticSurface.kind),
			staticSurface.surface.provenance.wallIndex,
			staticSurface.surface.provenance.nextSectorIndex,
			adjacentChunkIndex,
			YesNo(adjacentReplaced),
			match.delta[0],
			match.delta[1],
			match.delta[2],
			match.deviationFromMean,
			match.areaRatio,
			match.normalDot,
			YesNo(seamOutlier));
		seamPrinted++;
	}

	const size_t unmatchedStaticCount = std::min<size_t>(unmatchedStaticSurfaceIndices.size(), 8u);
	for (size_t i = 0; i < unmatchedStaticCount; ++i)
	{
		const auto& surface = mMapWorld.surfaces[unmatchedStaticSurfaceIndices[i]];
		Printf("NRI PT chunk compare unmatched_static: surface=%u kind=%s source=%s sector=%d wall=%d section=%d nextsector=%d cstat=0x%x tile=%u flags=0x%x verts=%u tris=%u\n",
			unmatchedStaticSurfaceIndices[i],
			nri_diag::GetMapSurfaceKindName(surface.kind),
			nri_diag::GetSurfaceSourceTypeName(surface.surface.provenance.sourceType),
			surface.surface.provenance.sectorIndex,
			surface.surface.provenance.wallIndex,
			surface.surface.provenance.sectionIndex,
			surface.surface.provenance.nextSectorIndex,
			surface.surface.provenance.cstat,
			chunk_diag::GetSurfaceTextureId(surface),
			surface.surface.material.flags,
			(uint32_t)surface.surface.vertices.size(),
			chunk_diag::CountSurfaceTriangles(surface.surface));
	}

	const size_t unmatchedLiveCount = std::min<size_t>(unmatchedLiveSurfaceIndices.size(), 8u);
	for (size_t i = 0; i < unmatchedLiveCount; ++i)
	{
		const auto& surface = liveWorld.surfaces[unmatchedLiveSurfaceIndices[i]];
		Printf("NRI PT chunk compare unmatched_live: surface=%u kind=%s source=%s sector=%d wall=%d section=%d nextsector=%d cstat=0x%x tile=%u flags=0x%x verts=%u tris=%u\n",
			unmatchedLiveSurfaceIndices[i],
			nri_diag::GetMapSurfaceKindName(surface.kind),
			nri_diag::GetSurfaceSourceTypeName(surface.surface.provenance.sourceType),
			surface.surface.provenance.sectorIndex,
			surface.surface.provenance.wallIndex,
			surface.surface.provenance.sectionIndex,
			surface.surface.provenance.nextSectorIndex,
			surface.surface.provenance.cstat,
			chunk_diag::GetSurfaceTextureId(surface),
			surface.surface.material.flags,
			(uint32_t)surface.surface.vertices.size(),
			chunk_diag::CountSurfaceTriangles(surface.surface));
	}
}

void NRIRenderer::PrintSceneBufferStatus() const
{
	NRISceneBufferStatusSnapshot snapshot = {};
	const auto appendBuffer = [&snapshot](const NRIBufferResource& resource, const SceneBufferDebugStats& stats)
	{
		snapshot.buffers.push_back(BuildNRIBufferStatusSnapshot(resource, stats));
	};

	const NRIBufferResource& activeVertexBuffer = GetActiveVertexBuffer();
	const NRIBufferResource& activeIndexBuffer = GetActiveIndexBuffer();
	const NRIBufferResource& activePrimitiveBuffer = GetActivePrimitiveBuffer();
	const NRIBufferResource& activeMaterialBuffer = GetActiveMaterialBuffer();
	snapshot.totalUsedBytes = activeVertexBuffer.usedSize + activeIndexBuffer.usedSize + activePrimitiveBuffer.usedSize + activeMaterialBuffer.usedSize;
	snapshot.totalCapacityBytes = activeVertexBuffer.size + activeIndexBuffer.size + activePrimitiveBuffer.size + activeMaterialBuffer.size;
	snapshot.lastFrameUploadBytes =
		mVertexBufferStats.bytesUploadedLastFrame +
		mIndexBufferStats.bytesUploadedLastFrame +
		mPrimitiveBufferStats.bytesUploadedLastFrame +
		mMaterialBufferStats.bytesUploadedLastFrame;
	snapshot.lastFrameGrowEvents =
		mVertexBufferStats.growEventsLastFrame +
		mIndexBufferStats.growEventsLastFrame +
		mPrimitiveBufferStats.growEventsLastFrame +
		mMaterialBufferStats.growEventsLastFrame;
	snapshot.lastFrameOverwriteEvents =
		mVertexBufferStats.overwriteEventsLastFrame +
		mIndexBufferStats.overwriteEventsLastFrame +
		mPrimitiveBufferStats.overwriteEventsLastFrame +
		mMaterialBufferStats.overwriteEventsLastFrame;

	appendBuffer(activeVertexBuffer, mVertexBufferStats);
	appendBuffer(activeIndexBuffer, mIndexBufferStats);
	appendBuffer(activePrimitiveBuffer, mPrimitiveBufferStats);
	appendBuffer(activeMaterialBuffer, mMaterialBufferStats);
	appendBuffer(mPortalBuffer, mPortalBufferStats);
	appendBuffer(mRuntimeLightBuffer, mRuntimeLightBufferStats);
	appendBuffer(mRuntimeLightTileHeaderBuffer, mRuntimeLightTileHeaderBufferStats);
	appendBuffer(mRuntimeLightTileIndexBuffer, mRuntimeLightTileIndexBufferStats);
	appendBuffer(mEmissivePrimitiveHeaderBuffer, mEmissivePrimitiveHeaderBufferStats);
	appendBuffer(mEmissivePrimitiveBuffer, mEmissivePrimitiveBufferStats);
	appendBuffer(mEmissivePrimitiveCdfBuffer, mEmissivePrimitiveCdfBufferStats);
	appendBuffer(mEmissiveMaterialResponseBuffer, mEmissiveMaterialResponseBufferStats);
	appendBuffer(mSectorLightHeaderBuffer, mSectorLightHeaderBufferStats);
	appendBuffer(mSectorLightBuffer, mSectorLightBufferStats);
	PrintNRISceneBufferStatusSnapshot(snapshot);
}

void NRIRenderer::LogFallback(const char* reason)
{
	if (mHasLoggedFallback)
	{
		return;
	}

	Printf(TEXTCOLOR_ORANGE "NRI PT fallback: %s\n", reason != nullptr ? reason : "unknown reason");
	mHasLoggedFallback = true;
}

void NRIRenderer::LogBridgeStats(const nri_scene::SceneDebugStats& stats)
{
	if (!nri_ptscenestats)
	{
		mLastStats = stats;
		mHasLoggedStats = true;
		return;
	}

	if (!mHasLoggedStats || nri_scene::SceneDebugStatsDiffer(mLastStats, stats))
	{
		Printf("NRI PT scene: walls=%u flats=%u sprites=%u translucent=%u models=%u voxel_proxies=%u unsupported_models=%u voxel_cache=candidates:%u uncacheable:%u hits:%u misses:%u changes:%u split_stable:%u split_live:%u entries:%u surface_hits:%u stores:%u rebuilds:%u transform_rebakes:%u removes:%u not_captured:%u deferred:%u cached_prims:%u mirrors=%u skies=%u portal_views=%u portal_skips=%u approx_tris=%u materials=%u\n",
			stats.wallDrawItems,
			stats.flatDrawItems,
			stats.spriteDrawItems,
			stats.translucentDrawItems,
			stats.modelDrawItems,
			stats.voxelProxyDrawItems,
			stats.unsupportedModelDrawItems,
			stats.voxelStableCandidates,
			stats.voxelStableUncacheable,
			stats.voxelStableSignatureHits,
			stats.voxelStableSignatureMisses,
			stats.voxelStableSignatureChanges,
			stats.voxelStableSplitStable,
			stats.voxelStableSplitLive,
			stats.voxelCacheEntries,
			stats.voxelCacheSurfaceHits,
			stats.voxelCacheSurfaceStores,
			stats.voxelCacheSurfaceRebuilds,
			stats.voxelCacheTransformRebakes,
			stats.voxelCacheSurfaceRemoves,
			stats.voxelCacheNotCaptured,
			stats.voxelCacheDeferred,
			stats.voxelCachePrimitives,
			stats.mirrorSurfaces,
			stats.skySurfaces,
			stats.portalViews,
			stats.portalCapturesSkipped,
			stats.triangleEstimate,
			stats.materialRefs);
		mLastStats = stats;
		mHasLoggedStats = true;
	}
}

void NRIRenderer::PrintEmissiveSurfaceDump(float radius, uint32_t limit) const
{
	mSceneLights.PrintEmissiveSurfaceDump(mBoundEmissivePrimitiveRecords, mBoundEmissiveTotalPower, mCurrentCameraPos, radius, limit);
}

void NRIRenderer::PrintSceneLightDump(float radius, uint32_t limit) const
{
	mSceneLights.PrintSceneLightDump(mCurrentCameraPos, mMapWorld, mFrameIndex, radius, limit);
}

void NRIRenderer::PrintRuntimeSpaceLinkStatus() const
{
	Printf("NRI PT runtime links: active=%s geo_effect=%s query_attempted=%s query_rejected=%s candidate_sector=%d candidate_lotag=%d source_sector=%d reported_geo_count=%d view_roots=%u visible_sectors=%u providers=%u geo_providers=%u provider_groups=%u local_space_matches=%u visible_matches=%u links=%u translated_chunks=%u orphan_local_spaces=%u unresolved_runtime_portals=%u surfaces=%u tris=%u materials=%u\n",
		mRuntimeSpaceLinkLastFrame.active ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.geoEffectActive ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.queryAttempted ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.queryRejected ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.candidateSectorIndex,
		mRuntimeSpaceLinkLastFrame.candidateSectorLotag,
		mRuntimeSpaceLinkLastFrame.sourceSectorIndex,
		mRuntimeSpaceLinkLastFrame.reportedGeoCount,
		mRuntimeSpaceLinkLastFrame.viewRootSectorCount,
		mRuntimeSpaceLinkLastFrame.visibleSectorCount,
		mRuntimeSpaceLinkLastFrame.providerSectorCount,
		mRuntimeSpaceLinkLastFrame.geoProviderCount,
		mRuntimeSpaceLinkLastFrame.providerGroupCount,
		mRuntimeSpaceLinkLastFrame.localSpaceMatchedProviderCount,
		mRuntimeSpaceLinkLastFrame.visibleMatchedProviderCount,
		mRuntimeSpaceLinkLastFrame.linkCount,
		mRuntimeSpaceLinkLastFrame.translatedChunkCount,
		mRuntimeSpaceLinkLastFrame.orphanLocalSpaceCount,
		mRuntimeSpaceLinkLastFrame.unresolvedRuntimePortalCount,
		mRuntimeSpaceLinkLastFrame.surfaceCount,
		mRuntimeSpaceLinkLastFrame.triangleCount,
		mRuntimeSpaceLinkLastFrame.materialCount);
	Printf("NRI PT runtime link motion: prev_chunk_offsets=%u topology_changed=%s special_material_history=%s\n",
		(uint32_t)mRuntimeChunkTranslationHistory.size(),
		mRuntimeSpaceLinkLastFrame.topologyChanged ? "yes" : "no",
		"portal_mirror_raw_fallback");
}

void NRIRenderer::PrintMapWorldStatus() const
{
	if (!mMapWorld.valid)
	{
		Printf("NRI PT map world: no authoritative map world has been built yet.\n");
		return;
	}

	const auto& stats = mMapWorld.stats;
	Printf("NRI PT map world: level=%s build_serial=%llu chunks=%u local_spaces=%u sectors=%u sections=%u surfaces=%u walls=%u flats=%u portal_surfaces=%u portal_graph=%u portal_targets=%u wall_portals=%u sector_portals=%u mirror_portals=%u runtime_portals=%u skies=%u tris=%u\n",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)mMapWorld.buildSerial,
		stats.chunkCount,
		stats.localSpaceCount,
		stats.sectorCount,
		stats.sectionCount,
		stats.surfaceCount,
		stats.wallSurfaceCount,
		stats.flatSurfaceCount,
		stats.portalSurfaceCount,
		stats.portalCount,
		stats.portalTargetCount,
		stats.wallPortalCount,
		stats.sectorPortalCount,
		stats.mirrorPortalCount,
		stats.runtimePortalCount,
		stats.skySurfaceCount,
		stats.triangleCount);
}

void NRIRenderer::PrintDynamicSceneStatus() const
{
	const PersistentDynamicSurfaceStats persistentStats = mSceneLights.GatherPersistentDynamicEmissiveSurfaceStats();
	const PersistentDynamicEmissiveCache& persistentCache = mSceneLights.GetPersistentDynamicEmissiveCache();
	const SceneLightSystem::PersistentDynamicEmissiveHighWaterStats& persistentHighWater = mSceneLights.GetPersistentDynamicEmissiveHighWaterStats();
	const ActorSpriteDebugStats& actorSpriteDebugStats = mSceneLights.GetActorSpriteDebugStats();

	Printf("NRI PT dynamic scene: active=%s sprite_surfaces=%u tris=%u materials=%u models=%u unsupported_models=%u mirror_extended_surfaces=%u mirror_extended_tris=%u mirror_extended_materials=%u mirror_extended_models=%u mirror_extended_unsupported_models=%u mirror_player_surfaces=%u mirror_player_tris=%u mirror_player_materials=%u mirror_player_models=%u mirror_player_unsupported_models=%u mirror_distance=%.1f dynamic_as_builds=%u last_frame_as_build=%s active_tlas_instances=%u emissive_cache=%s cache_surfaces=%u cache_tris=%u cache_materials=%u\n",
		mUsedDynamicSceneLastFrame ? "yes" : "no",
		mDynamicSceneLastFrame.spriteSurfaceCount,
		mDynamicSceneLastFrame.primitiveCount,
		mDynamicSceneLastFrame.materialCount,
		mDynamicSceneLastFrame.modelCount,
		mDynamicSceneLastFrame.unsupportedModelCount,
		mDynamicSceneLastFrame.mirrorExtendedSurfaceCount,
		mDynamicSceneLastFrame.mirrorExtendedPrimitiveCount,
		mDynamicSceneLastFrame.mirrorExtendedMaterialCount,
		mDynamicSceneLastFrame.mirrorExtendedModelCount,
		mDynamicSceneLastFrame.mirrorExtendedUnsupportedModelCount,
		mDynamicSceneLastFrame.mirrorPlayerSurfaceCount,
		mDynamicSceneLastFrame.mirrorPlayerPrimitiveCount,
		mDynamicSceneLastFrame.mirrorPlayerMaterialCount,
		mDynamicSceneLastFrame.mirrorPlayerModelCount,
		mDynamicSceneLastFrame.mirrorPlayerUnsupportedModelCount,
		(double)nri_ptmirrordynamicdistance,
		mDynamicSceneLastFrame.asBuildCount,
		mBuiltDynamicSceneASLastFrame ? "yes" : "no",
		mActiveTlasInstanceCount,
		persistentCache.valid ? "yes" : "no",
		persistentCache.surfaceCount,
		persistentCache.primitiveCount,
		persistentCache.materialCount);
	Printf("NRI PT dynamic cache: actor_surfaces=%u non_actor_surfaces=%u walls=%u flats=%u sprites=%u actor_facing=%u actor_voxel=%u highwater=surfaces:%u tris:%u mats:%u actor:%u non_actor:%u walls:%u flats:%u sprites:%u actor_facing:%u actor_voxel:%u\n",
		persistentStats.actorSurfaceCount,
		persistentStats.nonActorSurfaceCount,
		persistentStats.wallSurfaceCount,
		persistentStats.flatSurfaceCount,
		persistentStats.spriteSurfaceCount,
		persistentStats.actorFacingSpriteCount,
		persistentStats.actorVoxelSpriteCount,
		persistentHighWater.surfaceCount,
		persistentHighWater.primitiveCount,
		persistentHighWater.materialCount,
		persistentHighWater.surfaceStats.actorSurfaceCount,
		persistentHighWater.surfaceStats.nonActorSurfaceCount,
		persistentHighWater.surfaceStats.wallSurfaceCount,
		persistentHighWater.surfaceStats.flatSurfaceCount,
		persistentHighWater.surfaceStats.spriteSurfaceCount,
		persistentHighWater.surfaceStats.actorFacingSpriteCount,
		persistentHighWater.surfaceStats.actorVoxelSpriteCount);
	Printf("NRI PT actor sprite diag: trace=%d cache_actor_facing=%u cache_actor_voxel=%u prune_checks=%u prune_matches=%u drop_missing_actor=%u drop_missing_actor_index=%u drop_null_live_texture=%u drop_texture_mismatch=%u drop_palette_mismatch=%u\n",
		(int)nri_ptactorspritetrace,
		persistentStats.actorFacingSpriteCount,
		persistentStats.actorVoxelSpriteCount,
		actorSpriteDebugStats.lastPruneChecks,
		actorSpriteDebugStats.lastPruneMatches,
		actorSpriteDebugStats.lastPruneDroppedMissingActor,
		actorSpriteDebugStats.lastPruneDroppedMissingActorIndex,
		actorSpriteDebugStats.lastPruneDroppedNullLiveTexture,
		actorSpriteDebugStats.lastPruneDroppedTextureMismatch,
		actorSpriteDebugStats.lastPruneDroppedPaletteMismatch);
	Printf("NRI PT scene texture overflow: textures=%u truncated=%u clamps=base:%u normal:%u metallic:%u roughness:%u emissive:%u builds=%llu warned=%s\n",
		mSceneTextures.OverflowStats().textureCountLastBuild,
		mSceneTextures.OverflowStats().truncatedTextureCountLastBuild,
		mSceneTextures.OverflowStats().baseTextureClampCountLastBuild,
		mSceneTextures.OverflowStats().normalTextureClampCountLastBuild,
		mSceneTextures.OverflowStats().metallicTextureClampCountLastBuild,
		mSceneTextures.OverflowStats().roughnessTextureClampCountLastBuild,
		mSceneTextures.OverflowStats().emissiveTextureClampCountLastBuild,
		(unsigned long long)mSceneTextures.OverflowStats().totalOverflowBuilds,
		mSceneTextures.OverflowStats().warningLogged ? "yes" : "no");
	Printf("NRI PT scene texture attribution: reason=%s requested=%u actor_materials=%u base=%u glow=%u normal=%u metallic=%u roughness=%u emissive=%u\n",
		mLastPerfShellTraceStats.sceneTextureReason.empty() ? "none" : mLastPerfShellTraceStats.sceneTextureReason.c_str(),
		mLastPerfShellTraceStats.sceneTextureRequestedCount,
		mLastPerfShellTraceStats.sceneTextureReferencedActorMaterialCount,
		mLastPerfShellTraceStats.sceneTextureReferencedBaseCount,
		mLastPerfShellTraceStats.sceneTextureReferencedGlowCount,
		mLastPerfShellTraceStats.sceneTextureReferencedNormalCount,
		mLastPerfShellTraceStats.sceneTextureReferencedMetallicCount,
		mLastPerfShellTraceStats.sceneTextureReferencedRoughnessCount,
		mLastPerfShellTraceStats.sceneTextureReferencedEmissiveCount);
	Printf("NRI PT actor overflow: materials=%u clamps=base:%u normal:%u metallic:%u roughness:%u emissive:%u omitted=%u\n",
		mLastPerfShellTraceStats.actorOverflowMaterialCount,
		mLastPerfShellTraceStats.actorOverflowBaseClampCount,
		mLastPerfShellTraceStats.actorOverflowNormalClampCount,
		mLastPerfShellTraceStats.actorOverflowMetallicClampCount,
		mLastPerfShellTraceStats.actorOverflowRoughnessClampCount,
		mLastPerfShellTraceStats.actorOverflowEmissiveClampCount,
		mLastPerfShellTraceStats.actorOverflowTraceOmittedCount);
	Printf("NRI PT scene texture cache: entries=%u highwater=%u misses=%u inserts=%u transitions=%u lookup_ms=%.3f realize_ms=%.3f descriptor_ms=%.3f transition_ms=%.3f\n",
		mSceneTextures.CacheStats().cacheEntriesLastBuild,
		mSceneTextures.CacheStats().cacheEntriesHighWater,
		mSceneTextures.CacheStats().lookupMissesLastBuild,
		mSceneTextures.CacheStats().insertCountLastBuild,
		mSceneTextures.CacheStats().transitionCountLastFrame,
		mSceneTextures.CacheStats().lookupMsLastBuild,
		mSceneTextures.CacheStats().realizeMsLastBuild,
		mSceneTextures.CacheStats().descriptorMsLastBuild,
		mSceneTextures.CacheStats().transitionMsLastFrame);
	Printf("NRI PT binding diag: label=%s materials=%u textures=%u actor_surfaces=%u actor_count=%u bridge_hash=0x%llx actor_hash=0x%llx scene_tex_updates=%llu scene_tex_hash=0x%llx scene_tex_reason=%s qframe=%u outstanding=%u scene_data_updates=%llu scene_data_hash=0x%llx scene_data_reason=%s qframe=%u outstanding=%u\n",
		mDescriptorCoherencyDebugStats.lastMaterialBuildLabel.empty() ? "none" : mDescriptorCoherencyDebugStats.lastMaterialBuildLabel.c_str(),
		mDescriptorCoherencyDebugStats.lastMaterialCount,
		mDescriptorCoherencyDebugStats.lastTextureCount,
		mDescriptorCoherencyDebugStats.lastActorSpriteSurfaceCount,
		mDescriptorCoherencyDebugStats.lastActorSpriteActorCount,
		(unsigned long long)mDescriptorCoherencyDebugStats.lastMaterialBridgeHash,
		(unsigned long long)mDescriptorCoherencyDebugStats.lastActorSpriteMaterialHash,
		(unsigned long long)mDescriptorCoherencyDebugStats.sceneTextureSetUpdates,
		(unsigned long long)mDescriptorCoherencyDebugStats.lastSceneTextureDescriptorHash,
		mDescriptorCoherencyDebugStats.lastSceneTextureReason.empty() ? "none" : mDescriptorCoherencyDebugStats.lastSceneTextureReason.c_str(),
		mDescriptorCoherencyDebugStats.lastSceneTextureQueuedFrameIndex,
		mDescriptorCoherencyDebugStats.lastSceneTextureOutstandingQueuedFrames,
		(unsigned long long)mDescriptorCoherencyDebugStats.sceneDataSetUpdates,
		(unsigned long long)mDescriptorCoherencyDebugStats.lastSceneDataDescriptorHash,
		mDescriptorCoherencyDebugStats.lastSceneDataReason.empty() ? "none" : mDescriptorCoherencyDebugStats.lastSceneDataReason.c_str(),
		mDescriptorCoherencyDebugStats.lastSceneDataQueuedFrameIndex,
		mDescriptorCoherencyDebugStats.lastSceneDataOutstandingQueuedFrames);
	Printf("NRI PT material builds: calls=%u override_builds=%u override_ms=%.3f material_ms=%.3f\n",
		mLastPerfShellTraceStats.materialBuildCalls,
		mLastPerfShellTraceStats.actorOverrideMapBuildCalls,
		mLastPerfShellTraceStats.actorOverrideMapBuildMs,
		mLastPerfShellTraceStats.materialBuildMs);
	Printf("NRI PT mutation detail: structural_ms=%.3f material_refresh_ms=%.3f structural=%u material_refresh=%u refresh_delta=%u refresh_delta_mask=0x%x refresh_hwcanvas=%u refresh_animated=%u struct_delta=%u struct_delta_mask=0x%x struct_view=%u struct_static_anim_flip=%u struct_excl_static_flip=%u struct_force_topology=%u struct_invalid=%u hwcanvas_chunks=%u\n",
		mLastPerfShellTraceStats.runtimeMutationStructuralRebuildMs,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshMs,
		mLastPerfShellTraceStats.runtimeMutationStructuralRebuildChunks,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshChunks,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReplacementDeltaChunks,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReasonMaskOr,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshHardwareCanvasChunks,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshAnimatedChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaReasonMaskOr,
		mLastPerfShellTraceStats.runtimeMutationStructuralReplacementViewChangedChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralStaticAnimatedModeFlipChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralExcludeStaticFlipChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralForcedTopologyChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralInvalidChunks,
		mLastPerfShellTraceStats.runtimeMutationHardwareCanvasChunkCount);
	for (size_t index = 0; index < NRIRenderer::MaterialBuildTraceSlotCount; ++index)
	{
		const auto& entry = mLastPerfShellTraceStats.materialBuildByLabel[index];
		if (entry.calls == 0 && entry.overrideBuildCalls == 0)
		{
			continue;
		}

		Printf("NRI PT material detail: label=%s calls=%u override_builds=%u override_ms=%.3f material_ms=%.3f\n",
			GetMaterialBuildTraceSlotName((MaterialBuildTraceSlot)index),
			entry.calls,
			entry.overrideBuildCalls,
			entry.overrideBuildMs,
			entry.materialBuildMs);
		Printf("NRI PT material textures: label=%s materials=%u actor_materials=%u textures=%u base=%u glow=%u normal=%u metallic=%u roughness=%u emissive=%u\n",
			GetMaterialBuildTraceSlotName((MaterialBuildTraceSlot)index),
			entry.materialCount,
			entry.actorMaterialCount,
			entry.textureCount,
			entry.baseTextureCount,
			entry.glowTextureCount,
			entry.normalTextureCount,
			entry.metallicTextureCount,
			entry.roughnessTextureCount,
			entry.emissiveTextureCount);
	}
}

NRIBufferStatusSnapshot BuildNRIBufferStatusSnapshot(const NRIBufferResource& resource, const SceneBufferDebugStats& stats)
{
	NRIBufferStatusSnapshot snapshot = {};
	snapshot.label = stats.label;
	snapshot.usedBytes = resource.usedSize;
	snapshot.capacityBytes = resource.size;
	snapshot.usedItems = resource.stride != 0 ? resource.usedSize / resource.stride : 0;
	snapshot.capacityItems = resource.stride != 0 ? resource.size / resource.stride : 0;
	snapshot.uploadCount = stats.uploadCount;
	snapshot.growthCount = stats.growthCount;
	snapshot.overwriteCount = stats.overwriteCount;
	snapshot.bytesUploadedLastFrame = stats.bytesUploadedLastFrame;
	snapshot.growEventsLastFrame = stats.growEventsLastFrame;
	snapshot.overwriteEventsLastFrame = stats.overwriteEventsLastFrame;
	snapshot.peakUsedBytes = stats.peakUsedBytes;
	return snapshot;
}

void PrintNRISceneBufferStatusSnapshot(const NRISceneBufferStatusSnapshot& snapshot)
{
	Printf("NRI PT scene buffers: used=%llu capacity=%llu last_frame_upload=%llu last_frame_grows=%u last_frame_overwrites=%u\n",
		(unsigned long long)snapshot.totalUsedBytes,
		(unsigned long long)snapshot.totalCapacityBytes,
		(unsigned long long)snapshot.lastFrameUploadBytes,
		snapshot.lastFrameGrowEvents,
		snapshot.lastFrameOverwriteEvents);

	for (const NRIBufferStatusSnapshot& buffer : snapshot.buffers)
	{
		Printf("NRI PT %s buffer: used=%llu/%llu bytes items=%llu/%llu uploads=%u grows=%u overwrites=%u last_frame_bytes=%llu last_frame_grows=%u last_frame_overwrites=%u peak_used=%llu\n",
			buffer.label,
			(unsigned long long)buffer.usedBytes,
			(unsigned long long)buffer.capacityBytes,
			(unsigned long long)buffer.usedItems,
			(unsigned long long)buffer.capacityItems,
			buffer.uploadCount,
			buffer.growthCount,
			buffer.overwriteCount,
			(unsigned long long)buffer.bytesUploadedLastFrame,
			buffer.growEventsLastFrame,
			buffer.overwriteEventsLastFrame,
			(unsigned long long)buffer.peakUsedBytes);
	}
}

void PrintNRITemporalStatusSnapshot(const NRITemporalStatusSnapshot& snapshot)
{
	Printf("NRI PT temporal: debug=%d requested_main=%s resolved_main=%s requested_post=%s resolved_post=%s taa=%s gui_capture=%s last_debug=%d last_main=%s last_post=%s reset=%s prev_camera=%s history_in=%s[%ux%u a=%u l=%u s=0x%x] history_out=%s[%ux%u a=%u l=%u s=0x%x] present=%s upscaled=%s use_upscaled=%s\n",
		snapshot.debugMode,
		snapshot.requestedMainUpscaler,
		snapshot.resolvedMainUpscaler,
		snapshot.requestedPostSharpen,
		snapshot.resolvedPostSharpen,
		snapshot.taa ? "on" : "off",
		snapshot.guiCapture ? "yes" : "no",
		snapshot.lastDebugMode,
		snapshot.lastMainUpscaler,
		snapshot.lastPostSharpen,
		snapshot.resetHistory ? "yes" : "no",
		snapshot.previousCamera ? "yes" : "no",
		snapshot.historyInput.slotName,
		snapshot.historyInput.width,
		snapshot.historyInput.height,
		snapshot.historyInput.access,
		snapshot.historyInput.layout,
		snapshot.historyInput.stages,
		snapshot.historyOutput.slotName,
		snapshot.historyOutput.width,
		snapshot.historyOutput.height,
		snapshot.historyOutput.access,
		snapshot.historyOutput.layout,
		snapshot.historyOutput.stages,
		snapshot.presentSlotName,
		snapshot.upscaledSlotName,
		snapshot.useUpscaled ? "yes" : "no");
	Printf("NRI PT beauty path: nrd_and_composition -> pre_exposed_hdr_temporal -> final_display_mapping inspect_scene=15 inspect_pre_exposed=45 inspect_post_taa=13 inspect_post_upscale=14\n");
	Printf("NRI PT temporal domain: history=%s present=%s temporal_exposure=%.3f present_exposure=%.3f exposure_stops=%.3f reset_threshold_stops=%.3f auto_exposure=%s exposure_texture=%s taa_apply=%s\n",
		snapshot.historyDomain,
		snapshot.presentDomain,
		snapshot.temporalExposure,
		snapshot.presentExposure,
		snapshot.exposureStops,
		snapshot.resetThresholdStops,
		snapshot.autoExposure ? "yes" : "no",
		snapshot.exposureTexture ? "yes" : "no",
		snapshot.taaApply ? "yes" : "no");
}

void PrintNRITemporalTraceSnapshot(const NRITemporalTraceSnapshot& snapshot)
{
	Printf("NRI PT temporal trace: stage=%s frame=%u debug=%d resolved_main=%s resolved_post=%s run_app_taa=%s gui_capture=%s primary_domain=%s secondary_domain=%s temporal_exposure=%.3f primary_present_exposure=%.3f secondary_present_exposure=%.3f reset=%s reset_reason=%s prev_camera=%s history_in=%s[%ux%u a=%u l=%u s=0x%x] history_out=%s[%ux%u a=%u l=%u s=0x%x] primary=%s[%ux%u a=%u l=%u s=0x%x] secondary=%s[%ux%u a=%u l=%u s=0x%x] use_upscaled=%s\n",
		snapshot.stage,
		snapshot.frameIndex,
		snapshot.debugMode,
		snapshot.resolvedMainUpscaler,
		snapshot.resolvedPostSharpen,
		snapshot.runAppTaa ? "yes" : "no",
		snapshot.guiCapture ? "yes" : "no",
		snapshot.primaryDomain,
		snapshot.secondaryDomain,
		snapshot.temporalExposure,
		snapshot.primaryPresentExposure,
		snapshot.secondaryPresentExposure,
		snapshot.resetHistory ? "yes" : "no",
		snapshot.resetReason,
		snapshot.previousCamera ? "yes" : "no",
		snapshot.historyInput.slotName,
		snapshot.historyInput.width,
		snapshot.historyInput.height,
		snapshot.historyInput.access,
		snapshot.historyInput.layout,
		snapshot.historyInput.stages,
		snapshot.historyOutput.slotName,
		snapshot.historyOutput.width,
		snapshot.historyOutput.height,
		snapshot.historyOutput.access,
		snapshot.historyOutput.layout,
		snapshot.historyOutput.stages,
		snapshot.primary.slotName,
		snapshot.primary.width,
		snapshot.primary.height,
		snapshot.primary.access,
		snapshot.primary.layout,
		snapshot.primary.stages,
		snapshot.secondary.slotName,
		snapshot.secondary.width,
		snapshot.secondary.height,
		snapshot.secondary.access,
		snapshot.secondary.layout,
		snapshot.secondary.stages,
		snapshot.useUpscaled ? "yes" : "no");
}

void PrintNRIPortalTraversalStatusSnapshot(const NRIPortalTraversalStatusSnapshot& snapshot)
{
	if (!snapshot.available)
	{
		Printf("NRI PT portal traversal: no authoritative portal graph is available.\n");
		return;
	}

	Printf("NRI PT portal traversal: depth=%u reflective=%u transfer=%u runtime_bound=%u hittable_surfaces=%u plane_portals_pending=%u\n",
		snapshot.depth,
		snapshot.reflective,
		snapshot.transfer,
		snapshot.runtimeBound,
		snapshot.hittableSurfaces,
		snapshot.pendingPlanePortals);
}

void PrintNRIResidentMapChunkRegistryStatusSnapshot(const NRIResidentMapChunkRegistryStatusSnapshot& snapshot)
{
	if (!snapshot.available)
	{
		Printf("NRI PT resident chunk registry: unavailable.\n");
		return;
	}

	Printf("NRI PT resident chunk registry: build_serial=%llu chunks=%u active=%u mapped=%u acceleration_resident=%u animated_candidates=%u animated_refresh_suppressed=%u\n",
		(unsigned long long)snapshot.buildSerial,
		snapshot.chunkCount,
		snapshot.activeChunkCount,
		snapshot.mappedChunkCount,
		snapshot.accelerationResidentChunkCount,
		snapshot.animatedCandidateChunkCount,
		snapshot.animatedRefreshSuppressedChunkCount);
	Printf("NRI PT map chunk bounds: chunks=%u valid=%u invalid=%u near_distance=%.1f visible=%u invisible_near=%u invisible_far=%u invisible_unknown=%u sample_chunk=%u center=(%.1f,%.1f,%.1f) radius=%.1f distance=%.1f tier=%s\n",
		snapshot.mapWorldChunkCount,
		snapshot.boundsValidCount,
		snapshot.boundsInvalidCount,
		(double)snapshot.nearDistance,
		snapshot.visibleCount,
		snapshot.invisibleNearCount,
		snapshot.invisibleFarCount,
		snapshot.invisibleUnknownCount,
		snapshot.sampleChunkIndex,
		(double)snapshot.sampleCenter[0],
		(double)snapshot.sampleCenter[1],
		(double)snapshot.sampleCenter[2],
		(double)snapshot.sampleRadius,
		(double)snapshot.sampleDistance,
		snapshot.sampleTier);
}
