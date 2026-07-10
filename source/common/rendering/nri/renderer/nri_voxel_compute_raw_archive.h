#pragma once

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

constexpr uint32_t NRI_VOXEL_RAW_ARCHIVE_INVALID_PAGE = UINT32_MAX;

enum class NRIVoxelComputeRawSourceState : uint8_t
{
	Missing,
	Pending,
	Scanning,
	Ready,
	Failed,
};

enum class NRIVoxelComputeRawSourceQueueResult : uint8_t
{
	Invalid,
	Queued,
	AlreadyPending,
	Ready,
	Failed,
	Full,
};

struct NRIVoxelComputeRawArchiveSourceRange
{
	uint32_t pageIndex = NRI_VOXEL_RAW_ARCHIVE_INVALID_PAGE;
	uint32_t slabOffset = 0;
	uint32_t slabCount = 0;
	uint32_t colorRunOffset = 0;
	uint32_t colorRunCount = 0;
	uint64_t serial = 0;
	uint64_t queuedFrame = 0;
	bool preloadRecorded = false;
};

struct NRIVoxelComputeRawArchivePagePlan
{
	uint32_t slabCapacity = 0;
	uint32_t colorRunCapacity = 0;
	uint32_t slabCount = 0;
	uint32_t colorRunCount = 0;
};

struct NRIVoxelComputeRawSourcePendingJob
{
	uint64_t sourceKey = 0;
	uint64_t queuedFrame = 0;
};

struct NRIVoxelComputeRawArchivePlanStats
{
	uint32_t pendingSources = 0;
	uint64_t queuedSources = 0;
	uint64_t dedupeHits = 0;
	uint64_t scansStarted = 0;
	uint64_t committedSources = 0;
	uint64_t failedSources = 0;
};

class NRIVoxelComputeRawArchivePlan
{
public:
	NRIVoxelComputeRawArchivePlan(uint32_t slabPageCapacity, uint32_t colorRunPageCapacity);

	NRIVoxelComputeRawSourceQueueResult QueueSource(uint64_t sourceKey, uint64_t frameNumber, uint32_t maxPendingSources);
	std::vector<NRIVoxelComputeRawSourcePendingJob> TakePendingSources(uint32_t maxSources);
	bool CommitSource(
		uint64_t sourceKey,
		uint32_t slabCount,
		uint32_t colorRunCount,
		bool preloadRecorded,
		NRIVoxelComputeRawArchiveSourceRange& outRange);
	bool FailSource(uint64_t sourceKey);

	NRIVoxelComputeRawSourceState GetSourceState(uint64_t sourceKey) const;
	const NRIVoxelComputeRawArchiveSourceRange* FindSource(uint64_t sourceKey) const;
	const NRIVoxelComputeRawArchivePagePlan* GetPage(uint32_t pageIndex) const;
	uint32_t GetPageCount() const;
	const NRIVoxelComputeRawArchivePlanStats& GetStats() const;

private:
	struct SourceRecord
	{
		NRIVoxelComputeRawSourceState state = NRIVoxelComputeRawSourceState::Missing;
		NRIVoxelComputeRawArchiveSourceRange range;
	};

	uint32_t FindOrAddPage(uint32_t slabCount, uint32_t colorRunCount);

	uint32_t m_slabPageCapacity = 0;
	uint32_t m_colorRunPageCapacity = 0;
	uint64_t m_nextSerial = 1;
	std::unordered_map<uint64_t, SourceRecord> m_sources;
	std::deque<uint64_t> m_pendingSources;
	std::vector<NRIVoxelComputeRawArchivePagePlan> m_pages;
	NRIVoxelComputeRawArchivePlanStats m_stats;
};
