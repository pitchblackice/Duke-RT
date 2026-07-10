#include "nri_voxel_compute_raw_archive.h"

#include <algorithm>
#include <limits>

NRIVoxelComputeRawArchivePlan::NRIVoxelComputeRawArchivePlan(
	uint32_t slabPageCapacity,
	uint32_t colorRunPageCapacity)
	: m_slabPageCapacity(std::max(1u, slabPageCapacity))
	, m_colorRunPageCapacity(std::max(1u, colorRunPageCapacity))
{
}

NRIVoxelComputeRawSourceQueueResult NRIVoxelComputeRawArchivePlan::QueueSource(
	uint64_t sourceKey,
	uint64_t frameNumber,
	uint32_t maxPendingSources)
{
	if (sourceKey == 0)
	{
		return NRIVoxelComputeRawSourceQueueResult::Invalid;
	}

	auto found = m_sources.find(sourceKey);
	if (found != m_sources.end())
	{
		switch (found->second.state)
		{
		case NRIVoxelComputeRawSourceState::Ready:
			return NRIVoxelComputeRawSourceQueueResult::Ready;
		case NRIVoxelComputeRawSourceState::Failed:
			return NRIVoxelComputeRawSourceQueueResult::Failed;
		case NRIVoxelComputeRawSourceState::Pending:
		case NRIVoxelComputeRawSourceState::Scanning:
			m_stats.dedupeHits++;
			return NRIVoxelComputeRawSourceQueueResult::AlreadyPending;
		default:
			break;
		}
	}

	if (maxPendingSources == 0 || m_stats.pendingSources >= maxPendingSources)
	{
		return NRIVoxelComputeRawSourceQueueResult::Full;
	}

	SourceRecord& source = m_sources[sourceKey];
	source.state = NRIVoxelComputeRawSourceState::Pending;
	source.range.queuedFrame = frameNumber;
	m_pendingSources.push_back(sourceKey);
	m_stats.pendingSources++;
	m_stats.queuedSources++;
	return NRIVoxelComputeRawSourceQueueResult::Queued;
}

std::vector<NRIVoxelComputeRawSourcePendingJob> NRIVoxelComputeRawArchivePlan::TakePendingSources(uint32_t maxSources)
{
	std::vector<NRIVoxelComputeRawSourcePendingJob> jobs;
	jobs.reserve(std::min<size_t>(maxSources, m_pendingSources.size()));
	while (!m_pendingSources.empty() && jobs.size() < maxSources)
	{
		const uint64_t sourceKey = m_pendingSources.front();
		m_pendingSources.pop_front();
		auto found = m_sources.find(sourceKey);
		if (found == m_sources.end() || found->second.state != NRIVoxelComputeRawSourceState::Pending)
		{
			continue;
		}
		found->second.state = NRIVoxelComputeRawSourceState::Scanning;
		jobs.push_back({ sourceKey, found->second.range.queuedFrame });
		m_stats.scansStarted++;
	}
	return jobs;
}

bool NRIVoxelComputeRawArchivePlan::CommitSource(
	uint64_t sourceKey,
	uint32_t slabCount,
	uint32_t colorRunCount,
	bool preloadRecorded,
	NRIVoxelComputeRawArchiveSourceRange& outRange)
{
	outRange = {};
	if (sourceKey == 0 || slabCount == 0 || colorRunCount == 0)
	{
		return false;
	}

	SourceRecord& source = m_sources[sourceKey];
	if (source.state == NRIVoxelComputeRawSourceState::Ready)
	{
		source.range.preloadRecorded = source.range.preloadRecorded || preloadRecorded;
		outRange = source.range;
		return true;
	}
	if (source.state == NRIVoxelComputeRawSourceState::Failed)
	{
		return false;
	}
	const bool wasPending =
		source.state == NRIVoxelComputeRawSourceState::Pending ||
		source.state == NRIVoxelComputeRawSourceState::Scanning;

	const uint32_t pageIndex = FindOrAddPage(slabCount, colorRunCount);
	if (pageIndex == NRI_VOXEL_RAW_ARCHIVE_INVALID_PAGE)
	{
		return false;
	}

	NRIVoxelComputeRawArchivePagePlan& page = m_pages[pageIndex];
	source.range.pageIndex = pageIndex;
	source.range.slabOffset = page.slabCount;
	source.range.slabCount = slabCount;
	source.range.colorRunOffset = page.colorRunCount;
	source.range.colorRunCount = colorRunCount;
	source.range.serial = m_nextSerial++;
	source.range.preloadRecorded = preloadRecorded;
	page.slabCount += slabCount;
	page.colorRunCount += colorRunCount;
	source.state = NRIVoxelComputeRawSourceState::Ready;
	if (m_stats.pendingSources != 0 && wasPending)
	{
		m_stats.pendingSources--;
	}
	m_stats.committedSources++;
	outRange = source.range;
	return true;
}

bool NRIVoxelComputeRawArchivePlan::FailSource(uint64_t sourceKey)
{
	auto found = m_sources.find(sourceKey);
	if (found == m_sources.end() || found->second.state == NRIVoxelComputeRawSourceState::Ready ||
		found->second.state == NRIVoxelComputeRawSourceState::Failed)
	{
		return false;
	}
	if (m_stats.pendingSources != 0 &&
		(found->second.state == NRIVoxelComputeRawSourceState::Pending ||
			found->second.state == NRIVoxelComputeRawSourceState::Scanning))
	{
		m_stats.pendingSources--;
	}
	found->second.state = NRIVoxelComputeRawSourceState::Failed;
	m_stats.failedSources++;
	return true;
}

NRIVoxelComputeRawSourceState NRIVoxelComputeRawArchivePlan::GetSourceState(uint64_t sourceKey) const
{
	auto found = m_sources.find(sourceKey);
	return found != m_sources.end() ? found->second.state : NRIVoxelComputeRawSourceState::Missing;
}

const NRIVoxelComputeRawArchiveSourceRange* NRIVoxelComputeRawArchivePlan::FindSource(uint64_t sourceKey) const
{
	auto found = m_sources.find(sourceKey);
	return found != m_sources.end() && found->second.state == NRIVoxelComputeRawSourceState::Ready ?
		&found->second.range : nullptr;
}

const NRIVoxelComputeRawArchivePagePlan* NRIVoxelComputeRawArchivePlan::GetPage(uint32_t pageIndex) const
{
	return pageIndex < m_pages.size() ? &m_pages[pageIndex] : nullptr;
}

uint32_t NRIVoxelComputeRawArchivePlan::GetPageCount() const
{
	return (uint32_t)m_pages.size();
}

const NRIVoxelComputeRawArchivePlanStats& NRIVoxelComputeRawArchivePlan::GetStats() const
{
	return m_stats;
}

uint32_t NRIVoxelComputeRawArchivePlan::FindOrAddPage(uint32_t slabCount, uint32_t colorRunCount)
{
	if (!m_pages.empty())
	{
		const NRIVoxelComputeRawArchivePagePlan& page = m_pages.back();
		if (slabCount <= page.slabCapacity - page.slabCount &&
			colorRunCount <= page.colorRunCapacity - page.colorRunCount)
		{
			return (uint32_t)m_pages.size() - 1u;
		}
	}

	if (m_pages.size() >= std::numeric_limits<uint32_t>::max())
	{
		return NRI_VOXEL_RAW_ARCHIVE_INVALID_PAGE;
	}
	NRIVoxelComputeRawArchivePagePlan page = {};
	page.slabCapacity = std::max(m_slabPageCapacity, slabCount);
	page.colorRunCapacity = std::max(m_colorRunPageCapacity, colorRunCount);
	m_pages.push_back(page);
	return (uint32_t)m_pages.size() - 1u;
}
