#pragma once

#include "nri_exposure.h"

#include <cstddef>
#include <cstdint>

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
