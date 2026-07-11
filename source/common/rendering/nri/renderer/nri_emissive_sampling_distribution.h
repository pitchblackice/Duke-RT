#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct NRIEmissiveSamplingDistributionCandidate
{
	uint64_t stableKey = 0;
	uint64_t bindingKey = 0;
	float proposalWeight = 0.0f;
};

struct NRIEmissiveSamplingDistributionEntry
{
	size_t inputIndex = 0;
	uint64_t stableKey = 0;
	uint64_t bindingKey = 0;
	float proposalWeight = 0.0f;
	float selectionPdf = 0.0f;
};

struct NRIEmissiveSamplingDistributionStats
{
	uint32_t inputCount = 0;
	uint32_t uniqueCount = 0;
	uint32_t duplicateCount = 0;
	uint32_t cappedCount = 0;
};

class NRIEmissiveSamplingDistribution
{
public:
	void Reset();
	void Build(
		const std::vector<NRIEmissiveSamplingDistributionCandidate>& candidates,
		size_t maxCandidateCount,
		std::vector<NRIEmissiveSamplingDistributionEntry>& outEntries,
		std::vector<float>& outCdf,
		NRIEmissiveSamplingDistributionStats* outStats = nullptr) const;
};
