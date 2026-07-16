#pragma once

#include <algorithm>
#include <cstdint>

inline bool ShouldSampleNRIPeriodicDiagnostic(
	uint64_t presentationGeneration,
	uint32_t interval)
{
	const uint64_t normalizedInterval = (uint64_t)(std::max)(1u, interval);
	return presentationGeneration == 1 ||
		(presentationGeneration != 0 && presentationGeneration % normalizedInterval == 0);
}
