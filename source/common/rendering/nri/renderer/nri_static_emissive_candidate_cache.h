#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

struct NRIStaticEmissiveSettingsIdentity
{
	uint32_t emissiveMinPowerBits = 0;
	uint32_t emissiveMinSurfaceBits = 0;
	uint32_t glowScaleBits = 0;
	uint32_t glowReachBits = 0;
	uint32_t glowFalloffBits = 0;
	uint32_t sectorEmissionSignalStrengthBits = 0;
	uint32_t sectorEmissionResponseMinBits = 0;
	uint32_t sectorEmissionResponseMaxBits = 0;

	bool operator==(const NRIStaticEmissiveSettingsIdentity& other) const
	{
		return emissiveMinPowerBits == other.emissiveMinPowerBits &&
			emissiveMinSurfaceBits == other.emissiveMinSurfaceBits &&
			glowScaleBits == other.glowScaleBits &&
			glowReachBits == other.glowReachBits &&
			glowFalloffBits == other.glowFalloffBits &&
			sectorEmissionSignalStrengthBits == other.sectorEmissionSignalStrengthBits &&
			sectorEmissionResponseMinBits == other.sectorEmissionResponseMinBits &&
			sectorEmissionResponseMaxBits == other.sectorEmissionResponseMaxBits;
	}
};

struct NRIStaticEmissiveCandidateIdentity
{
	uint32_t mapChunkIndex = UINT32_MAX;
	uint64_t geometryGeneration = 0;
	uint64_t materialGeneration = 0;
	uint32_t materialOffset = 0;
	uint32_t recordCount = 0;
	uint32_t resolvedLightOverlayGeneration = 0;
	uint64_t emissiveHeuristicGeneration = 0;
	NRIStaticEmissiveSettingsIdentity settings = {};
	bool active = false;
	bool runtimeMutationReplacement = false;

	bool CanReuse() const
	{
		return mapChunkIndex != UINT32_MAX &&
			geometryGeneration != 0 &&
			materialGeneration != 0 &&
			resolvedLightOverlayGeneration != 0 &&
			emissiveHeuristicGeneration != 0 &&
			active &&
			!runtimeMutationReplacement;
	}

	bool operator==(const NRIStaticEmissiveCandidateIdentity& other) const
	{
		return mapChunkIndex == other.mapChunkIndex &&
			geometryGeneration == other.geometryGeneration &&
			materialGeneration == other.materialGeneration &&
			materialOffset == other.materialOffset &&
			recordCount == other.recordCount &&
			resolvedLightOverlayGeneration == other.resolvedLightOverlayGeneration &&
			emissiveHeuristicGeneration == other.emissiveHeuristicGeneration &&
			settings == other.settings &&
			active == other.active &&
			runtimeMutationReplacement == other.runtimeMutationReplacement;
	}
};

template<typename Candidate>
class NRIStaticEmissiveCandidateCache
{
public:
	struct FrameStats
	{
		uint32_t probes = 0;
		uint32_t hits = 0;
		uint32_t rebuilds = 0;
		uint32_t liveFallbacks = 0;
		uint32_t quarantinedFallbacks = 0;
		uint32_t evaluatedRecords = 0;
		uint32_t reusedCandidates = 0;
		uint32_t validationChecks = 0;
		uint32_t validationMismatches = 0;
		uint32_t residentEntries = 0;
		uint32_t residentCandidates = 0;
		uint32_t quarantinedEntries = 0;
	};

	enum class ProbeResult : uint8_t
	{
		LiveFallback,
		QuarantinedFallback,
		Rebuild,
		Hit,
	};

	ProbeResult Probe(
		const NRIStaticEmissiveCandidateIdentity& identity,
		const std::vector<Candidate>*& outCandidates)
	{
		outCandidates = nullptr;
		mFrameStats.probes++;
		if (!identity.CanReuse())
		{
			mFrameStats.liveFallbacks++;
			return ProbeResult::LiveFallback;
		}
		const auto quarantineIt = mQuarantinedIdentities.find(identity.mapChunkIndex);
		if (quarantineIt != mQuarantinedIdentities.end())
		{
			if (quarantineIt->second == identity)
			{
				mFrameStats.quarantinedFallbacks++;
				return ProbeResult::QuarantinedFallback;
			}
			mQuarantinedIdentities.erase(quarantineIt);
			RefreshResidentStats();
		}

		const auto it = mEntries.find(identity.mapChunkIndex);
		if (it == mEntries.end() || !(it->second.identity == identity))
		{
			mFrameStats.rebuilds++;
			return ProbeResult::Rebuild;
		}

		mFrameStats.hits++;
		outCandidates = &it->second.candidates;
		return ProbeResult::Hit;
	}

	void Commit(
		const NRIStaticEmissiveCandidateIdentity& identity,
		std::vector<Candidate> candidates)
	{
		if (!identity.CanReuse())
		{
			return;
		}

		const auto existing = mEntries.find(identity.mapChunkIndex);
		if (existing != mEntries.end())
		{
			mResidentCandidateCount -= existing->second.candidates.size();
		}
		Entry& entry = mEntries[identity.mapChunkIndex];
		entry.identity = identity;
		entry.candidates = std::move(candidates);
		mResidentCandidateCount += entry.candidates.size();
		mQuarantinedIdentities.erase(identity.mapChunkIndex);
		RefreshResidentStats();
	}

	void InvalidateChunk(uint32_t mapChunkIndex)
	{
		const auto it = mEntries.find(mapChunkIndex);
		const bool hadQuarantine = mQuarantinedIdentities.erase(mapChunkIndex) != 0;
		if (it == mEntries.end())
		{
			if (hadQuarantine)
			{
				RefreshResidentStats();
			}
			return;
		}
		mResidentCandidateCount -= it->second.candidates.size();
		mEntries.erase(it);
		RefreshResidentStats();
	}

	template<typename Equal>
	bool ValidateAndQuarantine(
		const NRIStaticEmissiveCandidateIdentity& identity,
		const std::vector<Candidate>& reconstructed,
		Equal equal)
	{
		mFrameStats.validationChecks++;
		const auto it = mEntries.find(identity.mapChunkIndex);
		bool matches = it != mEntries.end() &&
			it->second.identity == identity &&
			it->second.candidates.size() == reconstructed.size();
		if (matches)
		{
			for (size_t index = 0; index < reconstructed.size(); ++index)
			{
				if (!equal(it->second.candidates[index], reconstructed[index]))
				{
					matches = false;
					break;
				}
			}
		}
		if (matches)
		{
			return true;
		}

		mQuarantinedIdentities[identity.mapChunkIndex] = identity;
		mFrameStats.validationMismatches++;
		RefreshResidentStats();
		return false;
	}

	void NoteEvaluatedRecords(uint32_t count)
	{
		mFrameStats.evaluatedRecords += count;
	}

	void NoteReusedCandidates(uint32_t count)
	{
		mFrameStats.reusedCandidates += count;
	}

	void BeginFrame()
	{
		mFrameStats = {};
		RefreshResidentStats();
	}

	void Reset()
	{
		mEntries.clear();
		mQuarantinedIdentities.clear();
		mResidentCandidateCount = 0;
		mFrameStats = {};
	}

	const FrameStats& GetFrameStats() const { return mFrameStats; }

private:
	struct Entry
	{
		NRIStaticEmissiveCandidateIdentity identity = {};
		std::vector<Candidate> candidates;
	};

	void RefreshResidentStats()
	{
		mFrameStats.residentEntries = (uint32_t)mEntries.size();
		mFrameStats.residentCandidates = (uint32_t)mResidentCandidateCount;
		mFrameStats.quarantinedEntries = (uint32_t)mQuarantinedIdentities.size();
	}

	std::unordered_map<uint32_t, Entry> mEntries;
	std::unordered_map<uint32_t, NRIStaticEmissiveCandidateIdentity> mQuarantinedIdentities;
	size_t mResidentCandidateCount = 0;
	FrameStats mFrameStats = {};
};
