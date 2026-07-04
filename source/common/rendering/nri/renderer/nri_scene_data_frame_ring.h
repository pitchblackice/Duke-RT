#pragma once

#include "nri_frame_resources.h"

#include <algorithm>
#include <cstdint>

struct NRISceneDataFrameSlot
{
	NRIBufferResource reprojectionBuffer;
	NRIBufferResource visibleChunkBuffer;
	NRIBufferResource visibleFlatPlaneBuffer;
	NRIBufferResource sceneInstanceBuffer;
	NRIBufferResource portalBuffer;

	SceneBufferDebugStats reprojectionStats = { "SceneDataSlotReprojection" };
	SceneBufferDebugStats visibleChunkStats = { "SceneDataSlotVisibleChunk" };
	SceneBufferDebugStats visibleFlatPlaneStats = { "SceneDataSlotVisibleFlatPlane" };
	SceneBufferDebugStats sceneInstanceStats = { "SceneDataSlotSceneInstance" };
	SceneBufferDebugStats portalStats = { "SceneDataSlotPortal" };

	uint64_t UsedBytes() const
	{
		return
			reprojectionBuffer.usedSize +
			visibleChunkBuffer.usedSize +
			visibleFlatPlaneBuffer.usedSize +
			sceneInstanceBuffer.usedSize +
			portalBuffer.usedSize;
	}

	uint64_t CapacityBytes() const
	{
		return
			reprojectionBuffer.size +
			visibleChunkBuffer.size +
			visibleFlatPlaneBuffer.size +
			sceneInstanceBuffer.size +
			portalBuffer.size;
	}

	uint32_t GrowEventsLastFrame() const
	{
		return
			reprojectionStats.growEventsLastFrame +
			visibleChunkStats.growEventsLastFrame +
			visibleFlatPlaneStats.growEventsLastFrame +
			sceneInstanceStats.growEventsLastFrame +
			portalStats.growEventsLastFrame;
	}
};
