#pragma once

#include "nri_frame_resources.h"
#include "nri_resources.h"

#include <cstdint>
#include <vector>

class NRIRendererDiagnostics
{
};

struct NRIBufferStatusSnapshot
{
	const char* label = "";
	uint64_t usedBytes = 0;
	uint64_t capacityBytes = 0;
	uint64_t usedItems = 0;
	uint64_t capacityItems = 0;
	uint32_t uploadCount = 0;
	uint32_t growthCount = 0;
	uint32_t overwriteCount = 0;
	uint64_t bytesUploadedLastFrame = 0;
	uint32_t growEventsLastFrame = 0;
	uint32_t overwriteEventsLastFrame = 0;
	uint64_t peakUsedBytes = 0;
};

struct NRISceneBufferStatusSnapshot
{
	uint64_t totalUsedBytes = 0;
	uint64_t totalCapacityBytes = 0;
	uint64_t lastFrameUploadBytes = 0;
	uint32_t lastFrameGrowEvents = 0;
	uint32_t lastFrameOverwriteEvents = 0;
	std::vector<NRIBufferStatusSnapshot> buffers;
};

NRIBufferStatusSnapshot BuildNRIBufferStatusSnapshot(const NRIBufferResource& resource, const SceneBufferDebugStats& stats);
void PrintNRISceneBufferStatusSnapshot(const NRISceneBufferStatusSnapshot& snapshot);
