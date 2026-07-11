#include "nri_emissive_sampling_distribution.h"

#include <algorithm>
#include <cmath>

void NRIEmissiveSamplingDistribution::Reset()
{
}

void NRIEmissiveSamplingDistribution::Build(
	const std::vector<NRIEmissiveSamplingDistributionCandidate>& candidates,
	size_t maxCandidateCount,
	std::vector<NRIEmissiveSamplingDistributionEntry>& outEntries,
	std::vector<float>& outCdf,
	NRIEmissiveSamplingDistributionStats* outStats) const
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
		return aIndex < bIndex;
	});

	std::vector<size_t> uniqueIndices;
	uniqueIndices.reserve(orderedIndices.size());
	for (size_t index : orderedIndices)
	{
		if (!uniqueIndices.empty() && candidates[uniqueIndices.back()].stableKey == candidates[index].stableKey)
		{
			stats.duplicateCount++;
			continue;
		}
		uniqueIndices.push_back(index);
	}
	stats.uniqueCount = (uint32_t)uniqueIndices.size();

	if (uniqueIndices.size() > maxCandidateCount)
	{
		std::sort(uniqueIndices.begin(), uniqueIndices.end(), [&](size_t aIndex, size_t bIndex)
		{
			const float aWeight = std::isfinite(candidates[aIndex].proposalWeight) ? std::max(candidates[aIndex].proposalWeight, 0.0f) : 0.0f;
			const float bWeight = std::isfinite(candidates[bIndex].proposalWeight) ? std::max(candidates[bIndex].proposalWeight, 0.0f) : 0.0f;
			if (aWeight != bWeight)
			{
				return aWeight > bWeight;
			}
			return candidates[aIndex].stableKey < candidates[bIndex].stableKey;
		});
		stats.cappedCount = (uint32_t)(uniqueIndices.size() - maxCandidateCount);
		uniqueIndices.resize(maxCandidateCount);
		std::sort(uniqueIndices.begin(), uniqueIndices.end(), [&](size_t aIndex, size_t bIndex)
		{
			return candidates[aIndex].stableKey < candidates[bIndex].stableKey;
		});
	}

	float totalWeight = 0.0f;
	outEntries.reserve(uniqueIndices.size());
	for (size_t index : uniqueIndices)
	{
		const auto& candidate = candidates[index];
		NRIEmissiveSamplingDistributionEntry entry = {};
		entry.inputIndex = index;
		entry.stableKey = candidate.stableKey;
		entry.bindingKey = candidate.bindingKey;
		entry.proposalWeight = std::isfinite(candidate.proposalWeight) ? std::max(candidate.proposalWeight, 0.0f) : 0.0f;
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
