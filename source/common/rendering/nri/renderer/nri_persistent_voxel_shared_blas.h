#pragma once

#include <cstdint>
#include <unordered_set>

struct NRIPersistentVoxelSharedBlasFrameStats
{
	uint32_t activeActors = 0;
	uint32_t uniqueDesiredKeys = 0;
	uint32_t residentSharedAssets = 0;
	uint32_t queuedSharedAssets = 0;
	uint32_t buildAttempts = 0;
	uint32_t buildSuccesses = 0;
	uint32_t buildFailures = 0;
	uint32_t cacheHits = 0;
	uint32_t cacheMisses = 0;
	uint32_t actorRefs = 0;
	uint32_t routedLegacy = 0;
	uint32_t routedShared = 0;
	uint32_t fallbackLastValid = 0;
	uint32_t rejectMissingKey = 0;
};

class NRIPersistentVoxelSharedBlasCache
{
public:
	void Reset()
	{
		mDesiredKeys.clear();
		mFrameStats = {};
	}

	void BeginFrame()
	{
		mDesiredKeys.clear();
		mFrameStats = {};
	}

	void RecordLegacyActor(uint64_t sharedBlasKey)
	{
		mFrameStats.activeActors++;
		mFrameStats.actorRefs++;
		mFrameStats.routedLegacy++;
		if (sharedBlasKey == 0)
		{
			mFrameStats.rejectMissingKey++;
			return;
		}
		mDesiredKeys.insert(sharedBlasKey);
	}

	NRIPersistentVoxelSharedBlasFrameStats EndFrame()
	{
		mFrameStats.uniqueDesiredKeys = (uint32_t)mDesiredKeys.size();
		return mFrameStats;
	}

	const NRIPersistentVoxelSharedBlasFrameStats& LastFrameStats() const
	{
		return mFrameStats;
	}

private:
	std::unordered_set<uint64_t> mDesiredKeys;
	NRIPersistentVoxelSharedBlasFrameStats mFrameStats = {};
};
