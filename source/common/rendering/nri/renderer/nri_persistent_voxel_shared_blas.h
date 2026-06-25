#pragma once

#include "nri_resources.h"

#include <cstring>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>

enum class NRIPersistentVoxelSharedBlasState : uint8_t
{
	Missing,
	Building,
	Resident,
	Failed
};

struct NRIPersistentVoxelSharedBlasFrameStats
{
	uint32_t activeActors = 0;
	uint32_t uniqueDesiredKeys = 0;
	uint32_t residentSharedAssets = 0;
	uint32_t queuedSharedAssets = 0;
	uint32_t eligibleBuildKeys = 0;
	uint32_t buildAttempts = 0;
	uint32_t buildSuccesses = 0;
	uint32_t buildFailures = 0;
	uint32_t cacheHits = 0;
	uint32_t cacheMisses = 0;
	uint32_t actorRefs = 0;
	uint32_t routedLegacy = 0;
	uint32_t routedShared = 0;
	uint32_t fallbackLastValid = 0;
	uint32_t activeReferencedAssets = 0;
	uint32_t unreferencedResidentAssets = 0;
	uint64_t residentBytes = 0;
	uint64_t activeReferencedBytes = 0;
	uint64_t unreferencedResidentBytes = 0;
	uint32_t routeEligibleActors = 0;
	uint32_t routeRejectMissingResident = 0;
	uint32_t routeRejectNonLocal = 0;
	uint32_t routeRejectTransformKeyed = 0;
	uint32_t routeRejectGeometryMismatch = 0;
	uint32_t rejectMissingKey = 0;
	uint32_t rejectDisabled = 0;
	uint32_t rejectNonLocal = 0;
	uint32_t rejectTransformKeyed = 0;
	uint32_t rejectMissingBuffers = 0;
	uint32_t rejectInvalidCounts = 0;
	uint32_t rejectBuildBudget = 0;
	uint32_t rejectGeometryMismatch = 0;
};

struct NRIPersistentVoxelSharedBlasEntry
{
	uint64_t key = 0;
	uint64_t geometrySignature = 0;
	uint32_t primitiveCount = 0;
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
	uint32_t lastUsedFrame = 0;
	uint32_t frameReferences = 0;
	uint64_t memoryEstimateBytes = 0;
	NRIPersistentVoxelSharedBlasState state = NRIPersistentVoxelSharedBlasState::Missing;
	NRIAccelerationStructureResource accelerationStructure;
};

class NRIPersistentVoxelSharedBlasCache
{
public:
	template <typename RetireAccelerationStructureFn>
	void RetireAll(RetireAccelerationStructureFn&& retireAccelerationStructure)
	{
		for (auto& pair : mEntries)
		{
			retireAccelerationStructure(pair.second.accelerationStructure);
		}
		mEntries.clear();
		ResetFrameStats();
	}

	void BeginFrame()
	{
		ResetFrameStats();
		for (auto& pair : mEntries)
		{
			pair.second.frameReferences = 0;
		}
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
		auto entryIt = mEntries.find(sharedBlasKey);
		if (entryIt != mEntries.end() && entryIt->second.state == NRIPersistentVoxelSharedBlasState::Resident)
		{
			entryIt->second.frameReferences++;
			mFrameStats.cacheHits++;
		}
		else
		{
			mFrameStats.cacheMisses++;
		}
	}

	void RecordSharedActor(uint64_t sharedBlasKey)
	{
		mFrameStats.activeActors++;
		mFrameStats.actorRefs++;
		mFrameStats.routedShared++;
		if (sharedBlasKey == 0)
		{
			mFrameStats.rejectMissingKey++;
			return;
		}
		mDesiredKeys.insert(sharedBlasKey);
		auto entryIt = mEntries.find(sharedBlasKey);
		if (entryIt != mEntries.end() && entryIt->second.state == NRIPersistentVoxelSharedBlasState::Resident)
		{
			entryIt->second.frameReferences++;
			mFrameStats.cacheHits++;
		}
		else
		{
			mFrameStats.cacheMisses++;
		}
	}

	void RecordRouteEligibleActor()
	{
		mFrameStats.routeEligibleActors++;
	}

	void RecordRouteFallback(uint64_t sharedBlasKey, const char* reason)
	{
		mFrameStats.fallbackLastValid++;
		if (sharedBlasKey == 0 || reason == nullptr)
		{
			return;
		}
		if (std::strcmp(reason, "missing-resident") == 0)
		{
			mFrameStats.routeRejectMissingResident++;
		}
		else if (std::strcmp(reason, "non-local") == 0)
		{
			mFrameStats.routeRejectNonLocal++;
		}
		else if (std::strcmp(reason, "transform-keyed") == 0)
		{
			mFrameStats.routeRejectTransformKeyed++;
		}
		else if (std::strcmp(reason, "geometry-mismatch") == 0)
		{
			mFrameStats.routeRejectGeometryMismatch++;
		}
	}

	void RecordBuildDisabled(uint64_t sharedBlasKey)
	{
		if (sharedBlasKey != 0 && mRejectedBuildKeys.insert(sharedBlasKey).second)
		{
			mFrameStats.rejectDisabled++;
		}
	}

	void RecordBuildReject(uint64_t sharedBlasKey, const char* reason)
	{
		if (sharedBlasKey == 0 || !mRejectedBuildKeys.insert(sharedBlasKey).second)
		{
			return;
		}
		if (reason == nullptr)
		{
			return;
		}
		if (std::strcmp(reason, "non-local") == 0)
		{
			mFrameStats.rejectNonLocal++;
		}
		else if (std::strcmp(reason, "transform-keyed") == 0)
		{
			mFrameStats.rejectTransformKeyed++;
		}
		else if (std::strcmp(reason, "missing-buffers") == 0)
		{
			mFrameStats.rejectMissingBuffers++;
		}
		else if (std::strcmp(reason, "invalid-counts") == 0)
		{
			mFrameStats.rejectInvalidCounts++;
		}
		else if (std::strcmp(reason, "build-budget") == 0)
		{
			mFrameStats.rejectBuildBudget++;
		}
		else if (std::strcmp(reason, "geometry-mismatch") == 0)
		{
			mFrameStats.rejectGeometryMismatch++;
		}
	}

	NRIPersistentVoxelSharedBlasEntry& PrepareBuild(
		uint64_t sharedBlasKey,
		uint64_t geometrySignature,
		uint32_t vertexCount,
		uint32_t indexCount,
		uint32_t primitiveCount,
		uint32_t frameIndex)
	{
		NRIPersistentVoxelSharedBlasEntry& entry = mEntries[sharedBlasKey];
		entry.key = sharedBlasKey;
		entry.geometrySignature = geometrySignature;
		entry.vertexCount = vertexCount;
		entry.indexCount = indexCount;
		entry.primitiveCount = primitiveCount;
		entry.lastUsedFrame = frameIndex;
		entry.state = NRIPersistentVoxelSharedBlasState::Building;
		mFrameStats.eligibleBuildKeys++;
		mFrameStats.buildAttempts++;
		return entry;
	}

	void MarkBuildSuccess(NRIPersistentVoxelSharedBlasEntry& entry, uint32_t frameIndex)
	{
		entry.lastUsedFrame = frameIndex;
		entry.memoryEstimateBytes = entry.accelerationStructure.memorySize;
		entry.state = NRIPersistentVoxelSharedBlasState::Resident;
		mFrameStats.buildSuccesses++;
	}

	void MarkBuildFailure(NRIPersistentVoxelSharedBlasEntry& entry)
	{
		entry.state = NRIPersistentVoxelSharedBlasState::Failed;
		entry.memoryEstimateBytes = 0;
		mFrameStats.buildFailures++;
	}

	const NRIPersistentVoxelSharedBlasEntry* Find(uint64_t sharedBlasKey) const
	{
		auto entryIt = mEntries.find(sharedBlasKey);
		return entryIt != mEntries.end() ? &entryIt->second : nullptr;
	}

	NRIPersistentVoxelSharedBlasFrameStats EndFrame()
	{
		mFrameStats.uniqueDesiredKeys = (uint32_t)mDesiredKeys.size();
		mFrameStats.residentSharedAssets = 0;
		mFrameStats.queuedSharedAssets = 0;
		for (const auto& pair : mEntries)
		{
			if (pair.second.state == NRIPersistentVoxelSharedBlasState::Resident)
			{
				mFrameStats.residentSharedAssets++;
				mFrameStats.residentBytes += pair.second.memoryEstimateBytes;
				if (pair.second.frameReferences != 0)
				{
					mFrameStats.activeReferencedAssets++;
					mFrameStats.activeReferencedBytes += pair.second.memoryEstimateBytes;
				}
				else
				{
					mFrameStats.unreferencedResidentAssets++;
					mFrameStats.unreferencedResidentBytes += pair.second.memoryEstimateBytes;
				}
			}
			else if (pair.second.state == NRIPersistentVoxelSharedBlasState::Building)
			{
				mFrameStats.queuedSharedAssets++;
			}
		}
		return mFrameStats;
	}

	const NRIPersistentVoxelSharedBlasFrameStats& LastFrameStats() const
	{
		return mFrameStats;
	}

private:
	void ResetFrameStats()
	{
		mDesiredKeys.clear();
		mRejectedBuildKeys.clear();
		mFrameStats = {};
	}

	std::unordered_map<uint64_t, NRIPersistentVoxelSharedBlasEntry> mEntries;
	std::unordered_set<uint64_t> mDesiredKeys;
	std::unordered_set<uint64_t> mRejectedBuildKeys;
	NRIPersistentVoxelSharedBlasFrameStats mFrameStats = {};
};
