#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

struct NRIEmissiveSamplingDistributionCandidate
{
	uint64_t stableKey = 0;
	uint64_t bindingKey = 0;
	uint64_t tieBreakKey = 0;
	float proposalWeight = 0.0f;
	float referenceProposalWeight = 0.0f;
	bool hasReferenceProposalWeight = false;
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
	uint32_t boundGrowthCount = 0;
	uint64_t lastBoundGrowthStableKey = 0;
	float lastBoundGrowthOldWeight = 0.0f;
	float lastBoundGrowthNewWeight = 0.0f;
	bool lastBoundGrowthWasAuthored = false;
};

class NRIEmissiveSamplingDistribution
{
public:
	void Reset();
	void Build(
		const std::vector<NRIEmissiveSamplingDistributionCandidate>& candidates,
		uint64_t frameIndex,
		size_t maxCandidateCount,
		std::vector<NRIEmissiveSamplingDistributionEntry>& outEntries,
		std::vector<float>& outCdf,
		NRIEmissiveSamplingDistributionStats* outStats = nullptr);

private:
	struct ProposalRecord
	{
		uint64_t bindingKey = 0;
		uint64_t lastActiveFrame = 0;
		float referenceWeight = 0.0f;
		bool authored = false;
	};

	std::unordered_map<uint64_t, ProposalRecord> mProposalRecords;
};
