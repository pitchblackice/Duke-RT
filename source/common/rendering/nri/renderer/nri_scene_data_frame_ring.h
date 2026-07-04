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
	NRIBufferResource runtimeLightBuffer;
	NRIBufferResource runtimeLightTileHeaderBuffer;
	NRIBufferResource runtimeLightTileIndexBuffer;
	NRIBufferResource sectorLightHeaderBuffer;
	NRIBufferResource sectorLightBuffer;

	SceneBufferDebugStats reprojectionStats = { "SceneDataSlotReprojection" };
	SceneBufferDebugStats visibleChunkStats = { "SceneDataSlotVisibleChunk" };
	SceneBufferDebugStats visibleFlatPlaneStats = { "SceneDataSlotVisibleFlatPlane" };
	SceneBufferDebugStats sceneInstanceStats = { "SceneDataSlotSceneInstance" };
	SceneBufferDebugStats portalStats = { "SceneDataSlotPortal" };
	SceneBufferDebugStats runtimeLightStats = { "SceneDataSlotRuntimeLight" };
	SceneBufferDebugStats runtimeLightTileHeaderStats = { "SceneDataSlotRuntimeLightTileHeader" };
	SceneBufferDebugStats runtimeLightTileIndexStats = { "SceneDataSlotRuntimeLightTileIndex" };
	SceneBufferDebugStats sectorLightHeaderStats = { "SceneDataSlotSectorLightHeader" };
	SceneBufferDebugStats sectorLightStats = { "SceneDataSlotSectorLight" };

	uint64_t snapshotGeneration = 0;
	uint64_t sceneInstanceHash = 0;
	uint64_t tlasInstanceHash = 0;
	uint64_t portalHash = 0;
	uint32_t sceneInstanceCount = 0;
	uint32_t tlasInstanceCount = 0;
	uint32_t portalCount = 0;

	uint64_t UsedBytes() const
	{
		return
			reprojectionBuffer.usedSize +
			visibleChunkBuffer.usedSize +
			visibleFlatPlaneBuffer.usedSize +
			sceneInstanceBuffer.usedSize +
			portalBuffer.usedSize +
			runtimeLightBuffer.usedSize +
			runtimeLightTileHeaderBuffer.usedSize +
			runtimeLightTileIndexBuffer.usedSize +
			sectorLightHeaderBuffer.usedSize +
			sectorLightBuffer.usedSize;
	}

	uint64_t CapacityBytes() const
	{
		return
			reprojectionBuffer.size +
			visibleChunkBuffer.size +
			visibleFlatPlaneBuffer.size +
			sceneInstanceBuffer.size +
			portalBuffer.size +
			runtimeLightBuffer.size +
			runtimeLightTileHeaderBuffer.size +
			runtimeLightTileIndexBuffer.size +
			sectorLightHeaderBuffer.size +
			sectorLightBuffer.size;
	}

	uint32_t GrowEventsLastFrame() const
	{
		return
			reprojectionStats.growEventsLastFrame +
			visibleChunkStats.growEventsLastFrame +
			visibleFlatPlaneStats.growEventsLastFrame +
			sceneInstanceStats.growEventsLastFrame +
			portalStats.growEventsLastFrame +
			runtimeLightStats.growEventsLastFrame +
			runtimeLightTileHeaderStats.growEventsLastFrame +
			runtimeLightTileIndexStats.growEventsLastFrame +
			sectorLightHeaderStats.growEventsLastFrame +
			sectorLightStats.growEventsLastFrame;
	}
};
