#include "nri_debug_reporters.h"

#include "printf.h"

NRIBufferStatusSnapshot BuildNRIBufferStatusSnapshot(const NRIBufferResource& resource, const SceneBufferDebugStats& stats)
{
	NRIBufferStatusSnapshot snapshot = {};
	snapshot.label = stats.label;
	snapshot.usedBytes = resource.usedSize;
	snapshot.capacityBytes = resource.size;
	snapshot.usedItems = resource.stride != 0 ? resource.usedSize / resource.stride : 0;
	snapshot.capacityItems = resource.stride != 0 ? resource.size / resource.stride : 0;
	snapshot.uploadCount = stats.uploadCount;
	snapshot.growthCount = stats.growthCount;
	snapshot.overwriteCount = stats.overwriteCount;
	snapshot.bytesUploadedLastFrame = stats.bytesUploadedLastFrame;
	snapshot.growEventsLastFrame = stats.growEventsLastFrame;
	snapshot.overwriteEventsLastFrame = stats.overwriteEventsLastFrame;
	snapshot.peakUsedBytes = stats.peakUsedBytes;
	return snapshot;
}

void PrintNRISceneBufferStatusSnapshot(const NRISceneBufferStatusSnapshot& snapshot)
{
	Printf("NRI PT scene buffers: used=%llu capacity=%llu last_frame_upload=%llu last_frame_grows=%u last_frame_overwrites=%u\n",
		(unsigned long long)snapshot.totalUsedBytes,
		(unsigned long long)snapshot.totalCapacityBytes,
		(unsigned long long)snapshot.lastFrameUploadBytes,
		snapshot.lastFrameGrowEvents,
		snapshot.lastFrameOverwriteEvents);

	for (const NRIBufferStatusSnapshot& buffer : snapshot.buffers)
	{
		Printf("NRI PT %s buffer: used=%llu/%llu bytes items=%llu/%llu uploads=%u grows=%u overwrites=%u last_frame_bytes=%llu last_frame_grows=%u last_frame_overwrites=%u peak_used=%llu\n",
			buffer.label,
			(unsigned long long)buffer.usedBytes,
			(unsigned long long)buffer.capacityBytes,
			(unsigned long long)buffer.usedItems,
			(unsigned long long)buffer.capacityItems,
			buffer.uploadCount,
			buffer.growthCount,
			buffer.overwriteCount,
			(unsigned long long)buffer.bytesUploadedLastFrame,
			buffer.growEventsLastFrame,
			buffer.overwriteEventsLastFrame,
			(unsigned long long)buffer.peakUsedBytes);
	}
}
