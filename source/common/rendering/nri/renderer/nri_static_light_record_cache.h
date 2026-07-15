#pragma once

#include "../scene/nri_scene_surface_types.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

class NRIStaticLightRecordCache
{
public:
	struct Skeleton
	{
		uint64_t identityKey = 0;
		uint32_t localMaterialOrdinal = 0;
		float center[3] = {};
		float boundsRadius = 0.0f;
		float surfaceArea = 0.0f;
		nri_scene::SurfaceProvenance provenance = {};
	};

	struct Entry
	{
		uint64_t geometryGeneration = 0;
		uint64_t materialGeneration = 0;
		std::vector<Skeleton> skeletons;
	};

	struct FrameStats
	{
		uint32_t probes = 0;
		uint32_t stableHits = 0;
		uint32_t materialRefreshHits = 0;
		uint32_t rebuilds = 0;
		uint32_t legacyFallbacks = 0;
		uint32_t excludedInactive = 0;
		uint32_t excludedReplacement = 0;
		uint32_t appendedSkeletons = 0;
		uint32_t residentEntries = 0;
		uint32_t residentSkeletons = 0;
	};

	enum class ProbeResult : uint8_t
	{
		Excluded,
		LegacyFallback,
		Rebuild,
		StableHit,
		MaterialRefreshHit,
	};

	ProbeResult Probe(
		uint32_t chunkIndex,
		uint64_t geometryGeneration,
		uint64_t materialGeneration,
		bool active,
		bool runtimeReplacementActive)
	{
		mFrameStats.probes++;
		if (!active)
		{
			mFrameStats.excludedInactive++;
			return ProbeResult::Excluded;
		}
		if (runtimeReplacementActive)
		{
			mFrameStats.excludedReplacement++;
			return ProbeResult::Excluded;
		}
		if (geometryGeneration == 0 || materialGeneration == 0)
		{
			mFrameStats.legacyFallbacks++;
			return ProbeResult::LegacyFallback;
		}

		const auto it = mEntries.find(chunkIndex);
		if (it == mEntries.end() || it->second.geometryGeneration != geometryGeneration)
		{
			mFrameStats.rebuilds++;
			return ProbeResult::Rebuild;
		}
		if (it->second.materialGeneration != materialGeneration)
		{
			mFrameStats.materialRefreshHits++;
			return ProbeResult::MaterialRefreshHit;
		}

		mFrameStats.stableHits++;
		return ProbeResult::StableHit;
	}

	void Commit(
		uint32_t chunkIndex,
		uint64_t geometryGeneration,
		uint64_t materialGeneration,
		std::vector<Skeleton> skeletons)
	{
		const auto existing = mEntries.find(chunkIndex);
		if (existing != mEntries.end())
		{
			mResidentSkeletonCount -= existing->second.skeletons.size();
		}
		Entry& entry = mEntries[chunkIndex];
		entry.geometryGeneration = geometryGeneration;
		entry.materialGeneration = materialGeneration;
		entry.skeletons = std::move(skeletons);
		mResidentSkeletonCount += entry.skeletons.size();
		RefreshResidentStats();
	}

	void CommitMaterialGeneration(uint32_t chunkIndex, uint64_t materialGeneration)
	{
		const auto it = mEntries.find(chunkIndex);
		if (it != mEntries.end())
		{
			it->second.materialGeneration = materialGeneration;
		}
	}

	const Entry* Find(uint32_t chunkIndex) const
	{
		const auto it = mEntries.find(chunkIndex);
		return it != mEntries.end() ? &it->second : nullptr;
	}

	static bool RebaseMaterialIndex(uint32_t materialOffset, uint32_t localMaterialOrdinal, uint32_t& outMaterialIndex)
	{
		if (localMaterialOrdinal > UINT32_MAX - materialOffset)
		{
			return false;
		}
		outMaterialIndex = materialOffset + localMaterialOrdinal;
		return true;
	}

	void BeginFrame()
	{
		mFrameStats = {};
		RefreshResidentStats();
	}

	void NoteAppendedSkeletons(uint32_t count)
	{
		mFrameStats.appendedSkeletons += count;
	}

	void Reset()
	{
		mEntries.clear();
		mResidentSkeletonCount = 0;
		mFrameStats = {};
	}

	const FrameStats& GetFrameStats() const { return mFrameStats; }
	size_t Size() const { return mEntries.size(); }

private:
	void RefreshResidentStats()
	{
		mFrameStats.residentEntries = (uint32_t)mEntries.size();
		mFrameStats.residentSkeletons = (uint32_t)mResidentSkeletonCount;
	}

	std::unordered_map<uint32_t, Entry> mEntries;
	size_t mResidentSkeletonCount = 0;
	FrameStats mFrameStats = {};
};
