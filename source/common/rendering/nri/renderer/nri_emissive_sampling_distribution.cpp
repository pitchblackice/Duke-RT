#include "nri_emissive_sampling_distribution.h"

#include <algorithm>
#include <cmath>

namespace
{
	uint64_t HashCandidateIdentity(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u));
	}
}

void NRIEmissiveSamplingDistribution::Reset()
{
	mProposalRecords.clear();
}

void NRIEmissiveSamplingDistribution::Build(
	const std::vector<NRIEmissiveSamplingDistributionCandidate>& candidates,
	uint64_t frameIndex,
	size_t maxCandidateCount,
	std::vector<NRIEmissiveSamplingDistributionEntry>& outEntries,
	std::vector<float>& outCdf,
	NRIEmissiveSamplingDistributionStats* outStats)
{
	NRIEmissiveSamplingDistributionStats stats = {};
	stats.inputCount = (uint32_t)candidates.size();
	outEntries.clear();
	outCdf.clear();

	std::vector<size_t> orderedIndices(candidates.size());
	for (size_t i = 0; i < candidates.size(); ++i)
	{
		orderedIndices[i] = i;
	}
	std::sort(orderedIndices.begin(), orderedIndices.end(), [&](size_t aIndex, size_t bIndex)
	{
		const auto& a = candidates[aIndex];
		const auto& b = candidates[bIndex];
		if (a.stableKey != b.stableKey)
		{
			return a.stableKey < b.stableKey;
		}
		if (a.bindingKey != b.bindingKey)
		{
			return a.bindingKey < b.bindingKey;
		}
		if (a.tieBreakKey != b.tieBreakKey)
		{
			return a.tieBreakKey < b.tieBreakKey;
		}
		return aIndex < bIndex;
	});

	std::vector<size_t> uniqueIndices;
	uniqueIndices.reserve(orderedIndices.size());
	std::vector<uint64_t> resolvedStableKeys(candidates.size(), 0);
	for (size_t first = 0; first < orderedIndices.size();)
	{
		size_t last = first + 1u;
		while (last < orderedIndices.size() && candidates[orderedIndices[last]].stableKey == candidates[orderedIndices[first]].stableKey)
		{
			last++;
		}
		const bool duplicateGroup = last - first > 1u;
		if (duplicateGroup)
		{
			stats.duplicateCount += (uint32_t)(last - first - 1u);
		}
		for (size_t i = first; i < last; ++i)
		{
			const size_t index = orderedIndices[i];
			resolvedStableKeys[index] = duplicateGroup ?
				HashCandidateIdentity(candidates[index].stableKey, (uint64_t)(i - first)) :
				candidates[index].stableKey;
			uniqueIndices.push_back(index);
		}
		first = last;
	}
	stats.uniqueCount = (uint32_t)uniqueIndices.size();
	std::vector<uint64_t> liveKeys;
	liveKeys.reserve(uniqueIndices.size());
	for (size_t index : uniqueIndices)
	{
		liveKeys.push_back(resolvedStableKeys[index]);
	}
	std::sort(liveKeys.begin(), liveKeys.end());
	for (auto it = mProposalRecords.begin(); it != mProposalRecords.end();)
	{
		if (!std::binary_search(liveKeys.begin(), liveKeys.end(), it->first))
		{
			it = mProposalRecords.erase(it);
		}
		else
		{
			++it;
		}
	}

	std::vector<float> resolvedWeights(candidates.size(), 0.0f);
	constexpr float BoundAbsoluteTolerance = 1e-6f;
	constexpr float BoundRelativeTolerance = 1e-4f;
	for (size_t index : uniqueIndices)
	{
		const auto& candidate = candidates[index];
		const float currentWeight = std::isfinite(candidate.proposalWeight) ? std::max(candidate.proposalWeight, 0.0f) : 0.0f;
		const float authoredWeight =
			candidate.hasReferenceProposalWeight && std::isfinite(candidate.referenceProposalWeight) ?
				std::max(candidate.referenceProposalWeight, 0.0f) : 0.0f;
		const float initialWeight = std::max(currentWeight, authoredWeight);
		const uint64_t stableKey = resolvedStableKeys[index];
		auto [recordIt, inserted] = mProposalRecords.emplace(stableKey, ProposalRecord{});
		auto& record = recordIt->second;
		if (inserted || record.bindingKey != candidate.bindingKey)
		{
			record.bindingKey = candidate.bindingKey;
			record.referenceWeight = initialWeight;
			record.authored = candidate.hasReferenceProposalWeight;
		}
		else
		{
			const float requestedWeight = std::max(currentWeight, authoredWeight);
			const float growthTolerance = BoundAbsoluteTolerance + record.referenceWeight * BoundRelativeTolerance;
			if (requestedWeight > record.referenceWeight + growthTolerance)
			{
				stats.boundGrowthCount++;
				stats.lastBoundGrowthStableKey = stableKey;
				stats.lastBoundGrowthOldWeight = record.referenceWeight;
				stats.lastBoundGrowthNewWeight = requestedWeight;
				stats.lastBoundGrowthWasAuthored = authoredWeight >= currentWeight;
				record.referenceWeight = requestedWeight;
				record.authored = stats.lastBoundGrowthWasAuthored;
			}
		}
		if (currentWeight > 0.0f)
		{
			record.lastActiveFrame = frameIndex;
		}
		resolvedWeights[index] = record.referenceWeight;
	}

	if (uniqueIndices.size() > maxCandidateCount)
	{
		std::sort(uniqueIndices.begin(), uniqueIndices.end(), [&](size_t aIndex, size_t bIndex)
		{
			const float aWeight = resolvedWeights[aIndex];
			const float bWeight = resolvedWeights[bIndex];
			if (aWeight != bWeight)
			{
				return aWeight > bWeight;
			}
			return resolvedStableKeys[aIndex] < resolvedStableKeys[bIndex];
		});
		stats.cappedCount = (uint32_t)(uniqueIndices.size() - maxCandidateCount);
		uniqueIndices.resize(maxCandidateCount);
		std::sort(uniqueIndices.begin(), uniqueIndices.end(), [&](size_t aIndex, size_t bIndex)
		{
			return resolvedStableKeys[aIndex] < resolvedStableKeys[bIndex];
		});
	}

	float totalWeight = 0.0f;
	outEntries.reserve(uniqueIndices.size());
	for (size_t index : uniqueIndices)
	{
		const auto& candidate = candidates[index];
		NRIEmissiveSamplingDistributionEntry entry = {};
		entry.inputIndex = index;
		entry.stableKey = resolvedStableKeys[index];
		entry.bindingKey = candidate.bindingKey;
		entry.proposalWeight = resolvedWeights[index];
		totalWeight += entry.proposalWeight;
		outEntries.push_back(entry);
	}

	if (outEntries.empty())
	{
		outCdf.assign(1, 1.0f);
		if (outStats != nullptr)
		{
			*outStats = stats;
		}
		return;
	}

	const float uniformPdf = 1.0f / (float)outEntries.size();
	const float invTotalWeight = totalWeight > 0.0f ? 1.0f / totalWeight : 0.0f;
	float runningCdf = 0.0f;
	outCdf.reserve(outEntries.size());
	for (size_t i = 0; i < outEntries.size(); ++i)
	{
		auto& entry = outEntries[i];
		entry.selectionPdf = totalWeight > 0.0f ? entry.proposalWeight * invTotalWeight : uniformPdf;
		runningCdf += entry.selectionPdf;
		outCdf.push_back(i + 1u == outEntries.size() ? 1.0f : std::min(runningCdf, 1.0f));
	}

	if (outStats != nullptr)
	{
		*outStats = stats;
	}
}
