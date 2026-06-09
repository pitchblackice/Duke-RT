#include "nri_frame_graph.h"

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
