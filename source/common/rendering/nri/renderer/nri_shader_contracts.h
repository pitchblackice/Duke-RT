#pragma once

#include "nri_exposure.h"

#include <cstddef>
#include <cstdint>

constexpr uint32_t NRI_MAX_SCENE_TEXTURES = 512;
constexpr uint32_t NRI_SCENE_DESCRIPTOR_NUM = 2 + NRI_MAX_SCENE_TEXTURES;
constexpr uint32_t NRI_SCENE_DATA_DESCRIPTOR_NUM = 26;
constexpr uint32_t NRI_INPUT_DESCRIPTOR_NUM = 14;
constexpr uint32_t NRI_OUTPUT_DESCRIPTOR_NUM = 15;
constexpr uint32_t NRI_TRACE_SHADER_STATS_DESCRIPTOR_NUM = 1;
constexpr uint32_t NRI_SAMPLER_DESCRIPTOR_NUM = 4;

constexpr uint32_t NRI_FLAG_RESET_HISTORY = 0x1u;
constexpr uint32_t NRI_FLAG_USE_UPSCALED = 0x2u;
constexpr uint32_t NRI_FLAG_BOOTSTRAP_VIEW = 0x4u;
constexpr uint32_t NRI_FLAG_PRESENT_RAW_TRACE = 0x8u;
constexpr uint32_t NRI_FLAG_RAW_PRESENT_ADD_SECONDARY = 0x10u;
constexpr uint32_t NRI_FLAG_SPLIT_SHADOW_DENOISER = 0x20u;
constexpr uint32_t NRI_FLAG_USE_JITTER = 0x40u;
constexpr uint32_t NRI_FLAG_DIRECTIONAL_LIGHT = 0x80u;
constexpr uint32_t NRI_FLAG_FAST_EMISSIVE_SHADOW = 0x100u;
constexpr uint32_t NRI_FLAG_GATE_PRIMARY_VISIBLE_CHUNKS = 0x200u;
constexpr uint32_t NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW = 0x400u;
constexpr uint32_t NRI_FLAG_TRACE_SHADER_STATS = 0x800u;

constexpr uint32_t NRI_PRESENT_FLAG_SPLIT_SHADOW_DENOISER = NRI_FLAG_SPLIT_SHADOW_DENOISER;
constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_DISPLAY_INFO_AVAILABLE = 0x1u;
constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_DISPLAY_HDR_SUPPORTED = 0x2u;
constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_HDR_SWAPCHAIN_ACTIVE = 0x4u;
constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_OFFSCREEN_HDR_TARGET = 0x8u;
constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_AUTO_EXPOSURE = 0x10u;
constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_EXPOSURE_TEXTURE_VALID = 0x20u;
constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_INPUT_PRE_EXPOSED = 0x40u;

constexpr uint32_t NRI_TEMPORAL_FLAG_AUTO_EXPOSURE = 0x1000u;
constexpr uint32_t NRI_TEMPORAL_FLAG_EXPOSURE_TEXTURE_VALID = 0x2000u;
constexpr uint32_t NRI_JITTER_PHASE_SHIFT = 16u;
constexpr uint32_t NRI_TAA_JITTER_PHASE_COUNT = 8u;

constexpr uint32_t NRIPackTemporalJitterPhaseCount(uint32_t jitterPhaseCount)
{
	return ((jitterPhaseCount > 255u ? 255u : jitterPhaseCount) & 0xffu) << NRI_JITTER_PHASE_SHIFT;
}

constexpr uint32_t NRI_RUNTIME_LIGHT_TILE_SIZE = 64u;
constexpr uint32_t NRI_PORTAL_FLAG_RUNTIME_BOUND = 0x1u;
constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_NONE = 0u;
constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_REFLECTIVE = 1u;
constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER = 2u;
constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_RUNTIME_BOUND = 3u;

// Mirrors shaders/Include/TraceConstants.hlsli.
struct NRITraceSceneConstants
{
	float CameraPos[3] = {};
	uint32_t RenderWidth = 0;
	float CameraForward[3] = {};
	uint32_t RenderHeight = 0;
	float CameraRight[3] = {};
	float TanHalfFovX = 1.0f;
	float CameraUp[3] = {};
	float TanHalfFovY = 1.0f;
	float PrevCameraPos[3] = {};
	uint32_t DisplayWidth = 0;
	float PrevCameraForward[3] = {};
	uint32_t DisplayHeight = 0;
	float PrevCameraRight[3] = {};
	float PrevTanHalfFovX = 1.0f;
	float PrevCameraUp[3] = {};
	float PrevTanHalfFovY = 1.0f;
	float LightDirection[3] = { 0.3f, 0.85f, -0.4f };
	uint32_t SceneInstanceCount = 0;
	float SkyColor[3] = { 0.38f, 0.48f, 0.65f };
	uint32_t DebugMode = 0;
	float GroundColor[3] = { 0.08f, 0.08f, 0.08f };
	uint32_t StaticPrimitiveCount = 0;
	uint32_t FrameIndex = 0;
	uint32_t DynamicPrimitiveCount = 0;
	uint32_t Flags = 0;
	uint32_t StaticMaterialCount = 0;
	uint32_t BootstrapMode = 0;
	uint32_t DynamicMaterialCount = 0;
	uint32_t BounceCounts = 0;
	uint32_t PortalCount = 0;
	uint32_t RuntimeLightCount = 0;
	uint32_t PortalDepth = 0;
	uint32_t ReservedTrace0 = 0;
	uint32_t ReservedTrace1 = 0;
};

// Mirrors shaders/Include/TemporalConstants.hlsli.
struct NRITemporalConstants
{
	uint32_t RenderWidth = 0;
	uint32_t RenderHeight = 0;
	uint32_t FrameIndex = 0;
	uint32_t Flags = 0;
	float Exposure = 1.0f;
};

// Mirrors shaders/Include/PresentConstants.hlsli.
struct NRIPresentConstants
{
	uint32_t InputWidth = 0;
	uint32_t InputHeight = 0;
	uint32_t DisplayWidth = 0;
	uint32_t DisplayHeight = 0;
	uint32_t PackedSceneOrigin = 0;
	uint32_t FrameIndex = 0;
	uint32_t DebugMode = 0;
	uint32_t Flags = 0;
	uint32_t DenoiserMode = 0;
	uint32_t OutputMode = 0;
	uint32_t TonemapMode = 0;
	uint32_t OutputFlags = 0;
	float Exposure = 1.0f;
	float Contrast = 1.0f;
	float Saturation = 1.0f;
	float Shoulder = 1.0f;
	float Toe = 1.0f;
	float PaperWhiteNits = 200.0f;
	float DisplayMaxLuminance = 80.0f;
	float DisplaySdrLuminance = 80.0f;
	uint32_t NightVisionPackedModeTint = 0;
	float NightVisionStrength = 0.0f;
	float NightVisionExposure = 1.0f;
	uint32_t NightVisionPackedControls = 0;
};

static_assert(sizeof(NRITraceSceneConstants) <= 224, "NRITraceSceneConstants must stay within the validated shared root-constant budget.");
static_assert(sizeof(NRITemporalConstants) <= 32, "NRITemporalConstants must stay compact.");
static_assert(sizeof(NRIPresentConstants) <= 96, "NRIPresentConstants must stay compact.");

// Mirrors shaders/Include/ExposureConstants.hlsli.
constexpr uint32_t NRI_EXPOSURE_SET_INPUTS = 0;
constexpr uint32_t NRI_EXPOSURE_SET_OUTPUTS = 1;
constexpr uint32_t NRI_EXPOSURE_SET_ROOT = 2;
constexpr uint32_t NRI_EXPOSURE_INPUT_BASE_REGISTER = 0;
constexpr uint32_t NRI_EXPOSURE_INPUT_DESCRIPTOR_NUM = 2;
constexpr uint32_t NRI_EXPOSURE_OUTPUT_TEXTURE_DESCRIPTOR_NUM = 1;
constexpr uint32_t NRI_EXPOSURE_OUTPUT_BUFFER_DESCRIPTOR_NUM = 2;
constexpr uint32_t NRI_EXPOSURE_OUTPUT_TEXTURE_BASE_REGISTER = 0;
constexpr uint32_t NRI_EXPOSURE_OUTPUT_BUFFER_BASE_REGISTER = 1;
constexpr uint32_t NRI_EXPOSURE_ROOT_REGISTER = 0;
constexpr uint32_t NRI_EXPOSURE_FLAG_FREEZE = 0x1u;
constexpr uint32_t NRI_EXPOSURE_FLAG_RESET = 0x2u;
constexpr uint32_t NRI_EXPOSURE_METERING_FULL = 0u;
constexpr uint32_t NRI_EXPOSURE_METERING_CENTER_WEIGHTED = 1u;
constexpr uint32_t NRI_EXPOSURE_METERING_BRIGHT_TAIL_SUPPRESSED = 2u;
constexpr uint32_t NRI_EXPOSURE_DEBUG_MAGIC = 0x45585033u;
constexpr uint32_t NRI_EXPOSURE_MAX_HISTOGRAM_BINS = 256u;
constexpr uint32_t NRI_EXPOSURE_DEBUG_WORD_COUNT = 16u;
constexpr float NRI_EXPOSURE_LOG_LUMINANCE_MIN = -12.0f;
constexpr float NRI_EXPOSURE_LOG_LUMINANCE_MAX = 12.0f;

struct NRIExposureConstants
{
	uint32_t RenderWidth = 0;
	uint32_t RenderHeight = 0;
	uint32_t FrameIndex = 0;
	uint32_t Flags = 0;
	uint32_t HistogramBinCount = 0;
	uint32_t SampleStep = 1;
	float DeltaTimeSeconds = 1.0f / 60.0f;
	uint32_t MeteringMode = 0;
	float LogLuminanceMin = NRI_EXPOSURE_LOG_LUMINANCE_MIN;
	float LogLuminanceMax = NRI_EXPOSURE_LOG_LUMINANCE_MAX;
	float InvLogLuminanceRange = 1.0f / 24.0f;
	float TargetLuminance = 0.18f;
	float MinExposure = 0.125f;
	float MaxExposure = 8.0f;
	float ExposureBias = 1.0f;
	float LowPercentile = 1.0f;
	float HighPercentile = 99.0f;
	float FallbackManualExposure = 1.0f;
	float AdaptUpSpeed = 3.0f;
	float AdaptDownSpeed = 1.0f;
};

static_assert(NRI_AUTO_EXPOSURE_MAX_HISTOGRAM_BINS == NRI_EXPOSURE_MAX_HISTOGRAM_BINS, "Exposure histogram capacity must match the shader contract.");
static_assert(NRI_AUTO_EXPOSURE_DEBUG_WORD_COUNT == NRI_EXPOSURE_DEBUG_WORD_COUNT, "Exposure debug readback size must match the shader contract.");
static_assert((uint32_t)NRIAutoExposureMeteringMode::FullFrame == NRI_EXPOSURE_METERING_FULL, "Exposure metering enum must match HLSL.");
static_assert((uint32_t)NRIAutoExposureMeteringMode::CenterWeighted == NRI_EXPOSURE_METERING_CENTER_WEIGHTED, "Exposure metering enum must match HLSL.");
static_assert((uint32_t)NRIAutoExposureMeteringMode::BrightTailSuppressed == NRI_EXPOSURE_METERING_BRIGHT_TAIL_SUPPRESSED, "Exposure metering enum must match HLSL.");
static_assert(sizeof(NRIExposureConstants) == 80, "NRIExposureConstants must match ExposureConstants.hlsli.");
static_assert(alignof(NRIExposureConstants) == 4, "NRIExposureConstants must remain scalar-aligned for HLSL root constants.");
static_assert(offsetof(NRIExposureConstants, RenderWidth) == 0, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, RenderHeight) == 4, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, FrameIndex) == 8, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, Flags) == 12, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, HistogramBinCount) == 16, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, SampleStep) == 20, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, DeltaTimeSeconds) == 24, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, MeteringMode) == 28, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, LogLuminanceMin) == 32, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, LogLuminanceMax) == 36, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, InvLogLuminanceRange) == 40, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, TargetLuminance) == 44, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, MinExposure) == 48, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, MaxExposure) == 52, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, ExposureBias) == 56, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, LowPercentile) == 60, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, HighPercentile) == 64, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, FallbackManualExposure) == 68, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, AdaptUpSpeed) == 72, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, AdaptDownSpeed) == 76, "NRIExposureConstants layout mismatch.");
