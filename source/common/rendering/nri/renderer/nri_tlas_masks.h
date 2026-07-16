#pragma once

#include <cstdint>

constexpr uint32_t NRI_TLAS_MASK_MAIN = 0x01u;
constexpr uint32_t NRI_TLAS_MASK_SHADOW = 0x02u;
constexpr uint32_t NRI_TLAS_MASK_REFLECTION = 0x04u;
constexpr uint32_t NRI_TLAS_MASK_GI = 0x08u;
constexpr uint32_t NRI_TLAS_MASK_EMISSIVE = 0x10u;
constexpr uint32_t NRI_TLAS_MASK_DEBUG = 0x20u;
constexpr uint32_t NRI_TLAS_MASK_ALL_WORKLOADS = 0xFFu;
