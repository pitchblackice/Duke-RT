#pragma once

#include <algorithm>
#include <cstdint>

constexpr uint32_t NRI_SMOKE_GRID_FIRST_USE_CORE_MINIMUM = 8u;
constexpr uint32_t NRI_SMOKE_GRID_FIRST_USE_CORE_DIVISOR = 16u;
constexpr uint32_t NRI_SMOKE_GRID_POLICY_BRICK_RESIDENT = 2u;
constexpr uint32_t NRI_SMOKE_GRID_POLICY_BRICK_CONTENT = 1u;
constexpr uint32_t NRI_SMOKE_GRID_POLICY_BRICK_HALO = 2u;
constexpr uint32_t NRI_SMOKE_GRID_POLICY_BRICK_BORROWED = 4u;

inline uint32_t NRISmokeGridFirstUseCoreCapacity(uint32_t brickCapacity)
{
	return std::min(brickCapacity,
		std::max(NRI_SMOKE_GRID_FIRST_USE_CORE_MINIMUM,
			brickCapacity / NRI_SMOKE_GRID_FIRST_USE_CORE_DIVISOR));
}

inline bool NRISmokeGridIsFirstUseClass(uint32_t sourceClass)
{
	return sourceClass != 0u;
}

inline bool NRISmokeGridAmbientBorrowsCore(uint32_t sourceClass,
	uint32_t freeBeforeAllocation, uint32_t brickCapacity)
{
	return !NRISmokeGridIsFirstUseClass(sourceClass) &&
		freeBeforeAllocation <= NRISmokeGridFirstUseCoreCapacity(brickCapacity);
}

inline bool NRISmokeGridBorrowedDormantCandidate(uint32_t state, uint32_t flags, uint32_t idleFrames)
{
	return state == NRI_SMOKE_GRID_POLICY_BRICK_RESIDENT &&
		(flags & NRI_SMOKE_GRID_POLICY_BRICK_BORROWED) != 0u &&
		(flags & NRI_SMOKE_GRID_POLICY_BRICK_HALO) != 0u &&
		(flags & NRI_SMOKE_GRID_POLICY_BRICK_CONTENT) == 0u && idleFrames > 0u;
}
