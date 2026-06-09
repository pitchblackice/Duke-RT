#pragma once

#include <cstdint>

enum class NRIFramePass : uint32_t
{
	TraceOpaque,
	Denoiser,
	Composition,
	TraceTransparent,
	Exposure,
	UpscaleChain,
	RawPresent,
	FinalPresent,
	Final,
	CopyFinalToTarget,
};

enum class NRIPresentRouteKind : uint32_t
{
	BootstrapFinal,
	ResolvedBeauty,
	ComposedDebug,
	UpscalerTraceTransparentProbe,
	ValidationRaw,
	DenoisedRaw,
	ShadowFinal,
	FinalDebug,
	RawTraceDebug,
	FallbackFinal,
};

struct NRIPresentRouteInfo
{
	NRIPresentRouteKind kind = NRIPresentRouteKind::FallbackFinal;
	const char* routeName = "fallback_final";
	const char* presenterName = "Final";
	const char* ownerName = "fallback";
	const char* passListName = "TraceOpaque,Final,CopyFinal";
	bool denoiserRun = false;
	bool upscalerRun = false;
	bool exposureRun = false;
	bool compositionPath = false;
	bool rawTraceDebug = false;
};

struct NRIFrameRouteRequest
{
	uint32_t debugMode = 0;
	bool bootstrap = false;
	uint32_t bootstrapMode = 0;
};

const char* GetNRIFramePassName(NRIFramePass pass);
bool IsNRIFrameGraphSupportedDebugMode(uint32_t debugMode);
bool IsNRIFrameGraphRawTraceDebugMode(uint32_t debugMode);
bool IsNRIFrameGraphFinalShaderDebugMode(uint32_t debugMode);
NRIPresentRouteInfo ResolveNRIFrameRoute(const NRIFrameRouteRequest& request);
