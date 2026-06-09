#include "nri_frame_graph.h"

#include "nri_renderer.h"
#include "../system/nri_renderdevice.h"
#include "printf.h"

namespace
{
	constexpr uint32_t NRI_PTDEBUG_ANALYTIC_DIRECT_MODE = 26u;
	constexpr uint32_t NRI_PTDEBUG_SECTOR_AMBIENT_MODE = 29u;
	constexpr uint32_t NRI_PTDEBUG_EMISSIVE_SAMPLE_VISIBILITY_MODE = 33u;
	constexpr uint32_t NRI_PTDEBUG_UPSCALER_TRACE_TRANSPARENT_MODE = 34u;
	constexpr uint32_t NRI_PTDEBUG_TAA_PRE_EXPOSED_INPUT_MODE = 45u;

	constexpr uint32_t kSupportedDebugModes[] = {
		0u, 1u, 2u, 3u, 4u, 5u,
		9u, 10u, 11u, 12u,
		16u, 17u, 18u, 19u,
		21u, 22u, 24u, 25u,
		26u, 27u, 28u, 29u,
		33u, 34u, 45u
	};

	constexpr NRIPresentRouteInfo kBootstrapRawTraceRoute = {
		NRIPresentRouteKind::BootstrapFinal,
		"bootstrap_raw_trace",
		"Final",
		"bootstrap",
		"TraceOpaque,Final,CopyFinal",
		false,
		false,
		false,
		false,
		false
	};

	constexpr NRIPresentRouteInfo kBootstrapFallbackRoute = {
		NRIPresentRouteKind::FallbackFinal,
		"bootstrap_fallback",
		"Final",
		"bootstrap",
		"TraceOpaque,Final,CopyFinal",
		false,
		false,
		false,
		false,
		false
	};

	constexpr NRIPresentRouteInfo kFallbackFinalRoute = {
		NRIPresentRouteKind::FallbackFinal,
		"fallback_final",
		"Final",
		"fallback",
		"TraceOpaque,Final,CopyFinal",
		false,
		false,
		false,
		false,
		false
	};

	constexpr NRIPresentRouteInfo kResolvedBeautyRoute = {
		NRIPresentRouteKind::ResolvedBeauty,
		"resolved_beauty",
		"FinalPresent",
		"beauty",
		"TraceOpaque,Composition,TraceTransparent,Exposure,UpscaleChain,FinalPresent,CopyFinal",
		false,
		true,
		true,
		true,
		false
	};

	constexpr NRIPresentRouteInfo kComposedDebugRoute = {
		NRIPresentRouteKind::ComposedDebug,
		"taa_pre_exposed_probe",
		"FinalPresent",
		"debug-temporal",
		"TraceOpaque,Composition,TraceTransparent,Exposure,FinalPresent,CopyFinal",
		false,
		false,
		true,
		true,
		false
	};

	constexpr NRIPresentRouteInfo kUpscalerTraceTransparentProbeRoute = {
		NRIPresentRouteKind::UpscalerTraceTransparentProbe,
		"upscaler_trace_transparent",
		"RawPresent",
		"debug-upscaler",
		"TraceOpaque,Composition,TraceTransparent,Exposure,RawPresent,CopyFinal",
		false,
		false,
		true,
		true,
		false
	};

	constexpr NRIPresentRouteInfo kValidationRawRoute = {
		NRIPresentRouteKind::ValidationRaw,
		"validation_raw",
		"RawPresent",
		"debug-nrd",
		"TraceOpaque,Denoiser,RawPresent,CopyFinal",
		true,
		false,
		false,
		false,
		false
	};

	constexpr NRIPresentRouteInfo kDenoisedRawRoute = {
		NRIPresentRouteKind::DenoisedRaw,
		"denoised_raw",
		"RawPresent",
		"debug-nrd",
		"TraceOpaque,Denoiser,RawPresent,CopyFinal",
		true,
		false,
		false,
		false,
		false
	};

	constexpr NRIPresentRouteInfo kFinalDebugRoute = {
		NRIPresentRouteKind::FinalDebug,
		"final_debug",
		"Final",
		"debug-final",
		"TraceOpaque,Final,CopyFinal",
		false,
		false,
		false,
		false,
		false
	};

	constexpr NRIPresentRouteInfo kRawTraceDebugRoute = {
		NRIPresentRouteKind::RawTraceDebug,
		"raw_trace_debug",
		"RawPresent",
		"debug-trace",
		"TraceOpaque,RawPresent,CopyFinal",
		false,
		false,
		false,
		false,
		true
	};

	bool IsInRange(uint32_t value, uint32_t minValue, uint32_t maxValue)
	{
		return value >= minValue && value <= maxValue;
	}

	struct FrameGraphLogState
	{
		bool phaseBCompositionPath = false;
		bool phaseGResolvedPresentPath = false;
		bool phaseFDenoiserPath = false;
		bool phaseFDenoiserFallback = false;
		bool phaseFTraceTransparentPath = false;
		bool traceTransparentProbePath = false;
		bool rawTraceBypass = false;
		bool phaseHRrInputPath = false;
	};

	FrameGraphLogState& GetFrameGraphLogState()
	{
		static FrameGraphLogState state = {};
		return state;
	}
}

const char* GetNRIFramePassName(NRIFramePass pass)
{
	switch (pass)
	{
	case NRIFramePass::TraceOpaque: return "TraceOpaque";
	case NRIFramePass::Denoiser: return "Denoiser";
	case NRIFramePass::Composition: return "Composition";
	case NRIFramePass::TraceTransparent: return "TraceTransparent";
	case NRIFramePass::Exposure: return "Exposure";
	case NRIFramePass::UpscaleChain: return "UpscaleChain";
	case NRIFramePass::RawPresent: return "RawPresent";
	case NRIFramePass::FinalPresent: return "FinalPresent";
	case NRIFramePass::Final: return "Final";
	case NRIFramePass::CopyFinalToTarget: return "CopyFinal";
	default: return "Unknown";
	}
}

bool IsNRIFrameGraphSupportedDebugMode(uint32_t debugMode)
{
	for (uint32_t supportedMode : kSupportedDebugModes)
	{
		if (supportedMode == debugMode)
		{
			return true;
		}
	}
	return false;
}

bool IsNRIFrameGraphRawTraceDebugMode(uint32_t debugMode)
{
	return
		IsInRange(debugMode, 1u, 5u) ||
		IsInRange(debugMode, 10u, 12u) ||
		debugMode == 18u ||
		debugMode == 19u ||
		IsInRange(debugMode, 21u, 22u) ||
		IsInRange(debugMode, 24u, 25u) ||
		IsInRange(debugMode, NRI_PTDEBUG_ANALYTIC_DIRECT_MODE, NRI_PTDEBUG_SECTOR_AMBIENT_MODE) ||
		debugMode == NRI_PTDEBUG_EMISSIVE_SAMPLE_VISIBILITY_MODE;
}

bool IsNRIFrameGraphFinalShaderDebugMode(uint32_t)
{
	return false;
}

NRIPresentRouteInfo ResolveNRIFrameRoute(const NRIFrameRouteRequest& request)
{
	if (request.bootstrap)
	{
		if (request.bootstrapMode == 11u || request.bootstrapMode == 12u)
		{
			return kBootstrapRawTraceRoute;
		}
		return kBootstrapFallbackRoute;
	}

	if (request.debugMode == 0u)
	{
		return kResolvedBeautyRoute;
	}
	if (request.debugMode == NRI_PTDEBUG_TAA_PRE_EXPOSED_INPUT_MODE)
	{
		return kComposedDebugRoute;
	}
	if (request.debugMode == NRI_PTDEBUG_UPSCALER_TRACE_TRANSPARENT_MODE)
	{
		return kUpscalerTraceTransparentProbeRoute;
	}
	if (request.debugMode == 9u)
	{
		return kValidationRawRoute;
	}
	if (request.debugMode == 16u || request.debugMode == 17u)
	{
		return kDenoisedRawRoute;
	}
	if (IsNRIFrameGraphFinalShaderDebugMode(request.debugMode))
	{
		return kFinalDebugRoute;
	}
	if (IsNRIFrameGraphRawTraceDebugMode(request.debugMode))
	{
		return kRawTraceDebugRoute;
	}

	return kFallbackFinalRoute;
}

bool ExecuteNRIFrameGraph(
	NRIRenderer& renderer,
	HWDrawInfo& di,
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& materials,
	const NRIFrameGraphExecutionRequest& request)
{
	using FrameTextureSlot = NRIRenderer::FrameTextureSlot;

	FrameGraphLogState& logState = GetFrameGraphLogState();
	const int ptDebugMode = request.ptDebugMode;
	const bool denoise = request.denoise;
	const NRIPresentRouteInfo& presentRoute = request.presentRoute;
	renderer.ResetSelfTestRouteSnapshot();
	const bool bootstrapRawTracePresent = presentRoute.kind == NRIPresentRouteKind::BootstrapFinal;
	const bool useResolvedPresent = presentRoute.kind == NRIPresentRouteKind::ResolvedBeauty;
	const bool useComposedDebugPresent = presentRoute.kind == NRIPresentRouteKind::ComposedDebug;
	const bool useUpscalerTraceTransparentProbe = presentRoute.kind == NRIPresentRouteKind::UpscalerTraceTransparentProbe;
	const bool useCompositionPath = useResolvedPresent || useComposedDebugPresent || useUpscalerTraceTransparentProbe;
	const bool useValidationPresent = presentRoute.kind == NRIPresentRouteKind::ValidationRaw;
	const bool useDenoisedDebugPresent = presentRoute.kind == NRIPresentRouteKind::DenoisedRaw;
	const bool useShadowDebugPresent = presentRoute.kind == NRIPresentRouteKind::ShadowFinal;
	const bool useFinalDebugPresent = presentRoute.kind == NRIPresentRouteKind::FinalDebug || useShadowDebugPresent;
	const bool rawTraceDirectPresent = presentRoute.kind == NRIPresentRouteKind::RawTraceDebug;
	const bool useSplitShadowDebugProbe = rawTraceDirectPresent && ptDebugMode >= 21 && ptDebugMode <= 22;
	renderer.mHistoryInputSlot = (renderer.mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPing : FrameTextureSlot::TaaHistoryPong;
	renderer.mHistoryOutputSlot = (renderer.mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPong : FrameTextureSlot::TaaHistoryPing;
	renderer.mUpscaledInputSlot = FrameTextureSlot::PostSharpenOutput;
	renderer.mUseUpscaledInFinal = false;
	renderer.mUseDenoisedCompositionInputs = false;
	const bool directionalLightShadowEnabled = renderer.mDirectionalLightState.enabled && renderer.mDirectionalLightState.shadow;
	renderer.mUseSplitShadowDenoiser = directionalLightShadowEnabled && (useShadowDebugPresent || useSplitShadowDebugProbe || (useCompositionPath && denoise));
	const NRIPTOutputPolicy outputPolicy = renderer.mFrameBuffer->GetPathTracingOutputPolicy();
	const NRIAutoExposureSettings autoExposureSettings = GetNRIAutoExposureSettings(
		outputPolicy.exposure,
		IsNRIPTHdrOutputActive(outputPolicy));
	const char* autoExposureSettingsResetReason = nullptr;
	if (!renderer.mHasAutoExposureSettingsState)
	{
		renderer.mHasAutoExposureSettingsState = true;
	}
	else
	{
		autoExposureSettingsResetReason = GetNRIAutoExposureResetReasonForSettingsChange(renderer.mLastAutoExposureSettings, autoExposureSettings);
	}
	renderer.mLastAutoExposureSettings = autoExposureSettings;
	if (!renderer.EnsureAutoExposureResources(autoExposureSettings))
	{
		return false;
	}
	if (autoExposureSettingsResetReason != nullptr)
	{
		renderer.RequestAutoExposureReset(autoExposureSettingsResetReason);
	}

	if (!renderer.DispatchTraceOpaque(di, geometry, materials))
	{
		return false;
	}

	if (bootstrapRawTracePresent)
	{
		renderer.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, presentRoute.passListName, false, false, false);
		if (!renderer.DispatchFinal())
		{
			return false;
		}

		renderer.CopyFinalToActiveTarget();
		return true;
	}

	if (useValidationPresent)
	{
		renderer.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, presentRoute.passListName, true, false, false);
		if (!renderer.DispatchDenoiser())
		{
			return false;
		}

		if (!renderer.DispatchRawPresent(FrameTextureSlot::Validation))
		{
			return false;
		}

		renderer.CopyFinalToActiveTarget();
		return true;
	}

	if (useDenoisedDebugPresent)
	{
		renderer.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, presentRoute.passListName, true, false, false);
		if (!renderer.DispatchDenoiser())
		{
			return false;
		}

		const FrameTextureSlot denoisedSlot = ptDebugMode == 16 ? FrameTextureSlot::DenoisedDiffuse : FrameTextureSlot::DenoisedSpecular;
		if (!renderer.DispatchRawPresent(denoisedSlot))
		{
			return false;
		}

		renderer.CopyFinalToActiveTarget();
		return true;
	}

	if (useShadowDebugPresent)
	{
		renderer.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, denoise ? "TraceOpaque,Denoiser,Final,CopyFinal" : "TraceOpaque,Final,CopyFinal", denoise, false, false);
		if (denoise && !renderer.DispatchDenoiser())
		{
			return false;
		}

		renderer.mUseUpscaledInFinal = false;
		if (!renderer.DispatchFinal())
		{
			return false;
		}

		renderer.CopyFinalToActiveTarget();
		return true;
	}

	auto dispatchCompositionPath = [&]() -> bool
	{
		const NRIMainUpscalerKind resolvedMainKind = renderer.ResolveMainUpscalerKind(false);
		const bool buildRrInput = resolvedMainKind == NRIMainUpscalerKind::DLRR;
		const bool needStandardComposition =
			!buildRrInput || useComposedDebugPresent || useUpscalerTraceTransparentProbe;

		renderer.mUseDenoisedCompositionInputs = false;

		if (buildRrInput)
		{
			if (!logState.phaseHRrInputPath)
			{
				Printf("DLRR now builds a separate noisy RrInput before NRD and bypasses opaque denoising for the vendor RR branch.\n");
				logState.phaseHRrInputPath = true;
			}

			renderer.mUseSplitShadowDenoiser = false;
			if (!renderer.DispatchComposition(FrameTextureSlot::RrInput))
			{
				return false;
			}
		}

		if (!needStandardComposition)
		{
			if (!renderer.DispatchAutoExposure(FrameTextureSlot::RrInput))
			{
				return false;
			}
			return true;
		}

		if (!buildRrInput && denoise)
		{
			if (!logState.phaseFDenoiserPath)
			{
				Printf("The Composition-backed PT paths now route through NRD before Composition when nri_denoise is enabled.\n");
				logState.phaseFDenoiserPath = true;
			}

			if (!renderer.DispatchDenoiser())
			{
				if (!logState.phaseFDenoiserFallback)
				{
					Printf(TEXTCOLOR_ORANGE "NRD dispatch failed in the composition path; falling back to raw trace inputs for this frame.\n");
					logState.phaseFDenoiserFallback = true;
				}
			}
			else
			{
				renderer.mUseDenoisedCompositionInputs = true;
				renderer.mUseSplitShadowDenoiser = directionalLightShadowEnabled;
			}
		}

		if (!renderer.DispatchComposition(FrameTextureSlot::Composed))
		{
			return false;
		}

		if (!logState.phaseFTraceTransparentPath)
		{
			Printf("Composition-backed PT paths now pass through placeholder TraceTransparent before output-resolution dispatch.\n");
			logState.phaseFTraceTransparentPath = true;
		}

		if (!renderer.DispatchTraceTransparent())
		{
			return false;
		}

		if (!renderer.DispatchAutoExposure(FrameTextureSlot::TraceTransparentOutput))
		{
			return false;
		}

		return true;
	};

	if (useResolvedPresent)
	{
		renderer.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, presentRoute.passListName, denoise, true, true);
		if (!logState.phaseGResolvedPresentPath)
		{
			Printf("ptdebug 0 now routes through Composition, placeholder TraceTransparent, DispatchUpscaleChain, and the minimal FinalPresent presenter.\n");
			logState.phaseGResolvedPresentPath = true;
		}

		if (!dispatchCompositionPath())
		{
			return false;
		}

		if (!renderer.DispatchUpscaleChain())
		{
			return false;
		}

		const FrameTextureSlot resolvedPresentSlot = renderer.mUseUpscaledInFinal ? renderer.mUpscaledInputSlot : renderer.mHistoryOutputSlot;
		const NRIMainUpscalerKind resolvedMain = renderer.ResolveMainUpscalerKind(false);
		const NRIPostSharpenKind resolvedPost = renderer.ResolvePostSharpenKind(false);
		renderer.TraceTemporalState("resolved-present", resolvedMain, resolvedPost, renderer.ShouldRunAppTaaForFrameGraph(resolvedMain), resolvedPresentSlot, renderer.mHistoryOutputSlot);
		if (!renderer.DispatchFinalPresent(resolvedPresentSlot))
		{
			return false;
		}

		renderer.CopyFinalToActiveTarget();
		return true;
	}

	if (useComposedDebugPresent)
	{
		renderer.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, presentRoute.passListName, denoise, false, true);
		if (!logState.phaseBCompositionPath)
		{
			Printf("NRI Phase B: ptdebug 45 now routes through Composition, placeholder TraceTransparent, and the minimal FinalPresent presenter.\n");
			logState.phaseBCompositionPath = true;
		}

		if (!dispatchCompositionPath())
		{
			return false;
		}

		if (!renderer.DispatchFinalPresent(FrameTextureSlot::TraceTransparentOutput))
		{
			return false;
		}

		renderer.CopyFinalToActiveTarget();
		return true;
	}

	if (useUpscalerTraceTransparentProbe)
	{
		renderer.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, presentRoute.passListName, denoise, false, true);
		if (!logState.traceTransparentProbePath)
		{
			Printf("NRI Phase I instrumentation: ptdebug 34 now exposes TraceTransparentOutput before the upscaler chain.\n");
			logState.traceTransparentProbePath = true;
		}

		if (!dispatchCompositionPath())
		{
			return false;
		}

		if (!renderer.DispatchRawPresent(FrameTextureSlot::TraceTransparentOutput))
		{
			return false;
		}

		renderer.CopyFinalToActiveTarget();
		return true;
	}

	if (useFinalDebugPresent)
	{
		renderer.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, presentRoute.passListName, false, false, false);
		renderer.mUseUpscaledInFinal = false;
		if (!renderer.DispatchFinal())
		{
			return false;
		}

		renderer.CopyFinalToActiveTarget();
		return true;
	}

	if (rawTraceDirectPresent)
	{
		renderer.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, presentRoute.passListName, false, false, false);
		if (!logState.rawTraceBypass)
		{
			Printf("NRI frame-graph bypass: presenting raw TraceOpaque output through the direct present path for non-composition debug views.\n");
			logState.rawTraceBypass = true;
		}

		FrameTextureSlot rawPresentSlot = FrameTextureSlot::UnfilteredDiffuse;
		FrameTextureSlot rawPresentSecondarySlot = FrameTextureSlot::Count;
		FrameTextureSlot rawPresentTertiarySlot = FrameTextureSlot::Count;
		if (ptDebugMode == 11 || ptDebugMode == 12)
		{
			rawPresentSlot = FrameTextureSlot::UnfilteredSpecular;
		}
		else if (ptDebugMode == 18)
		{
			rawPresentSlot = FrameTextureSlot::BaseColorMetalness;
		}
		else if (ptDebugMode == 19)
		{
			rawPresentSlot = FrameTextureSlot::NormalRoughness;
		}
		else if (ptDebugMode == 21 || ptDebugMode == 22)
		{
			rawPresentSlot = FrameTextureSlot::UnfilteredPenumbra;
		}
		else if (ptDebugMode == 24)
		{
			rawPresentSlot = FrameTextureSlot::DirectLighting;
		}
		else if (ptDebugMode == 25)
		{
			rawPresentSlot = FrameTextureSlot::DirectEmission;
		}

		if (ptDebugMode == 12)
		{
			rawPresentSecondarySlot = FrameTextureSlot::ViewZ;
			rawPresentTertiarySlot = FrameTextureSlot::NormalRoughness;
		}

		if (!renderer.DispatchRawPresent(rawPresentSlot, rawPresentSecondarySlot, rawPresentTertiarySlot))
		{
			return false;
		}

		renderer.CopyFinalToActiveTarget();
		return true;
	}

	if (!logState.rawTraceBypass)
	{
		Printf("NRI frame-graph bypass: presenting raw TraceOpaque output until composition integration is stabilized.\n");
		logState.rawTraceBypass = true;
	}

	renderer.mUseUpscaledInFinal = false;
	renderer.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, presentRoute.passListName, false, false, false);
	if (!renderer.DispatchFinal())
	{
		return false;
	}

	renderer.CopyFinalToActiveTarget();
	return true;
}
