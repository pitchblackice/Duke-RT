#pragma once

#include "nri_tlas_masks.h"

#include <cstdint>

enum SceneInstanceMetadataFlags : uint32_t
{
	SceneInstanceMetadataFlag_None = 0u,
	SceneInstanceMetadataFlag_IndirectOnly = 1u << 0,
};

struct NRISceneInstanceVisibility
{
	uint8_t tlasMask = NRI_TLAS_MASK_ALL_WORKLOADS;
	uint32_t metadataFlags = SceneInstanceMetadataFlag_None;
};

struct NRISceneInstanceVisibilityContext
{
	int32_t localPlayerActorIndex = -1;
	bool localPlayerPrimaryVisible = false;
};

inline bool IsNRILocalPlayerPrimaryVisibleFromViewpoint(int32_t viewpointActorIndex, int32_t localPlayerActorIndex)
{
	return viewpointActorIndex >= 0 &&
		localPlayerActorIndex >= 0 &&
		viewpointActorIndex != localPlayerActorIndex;
}

inline NRISceneInstanceVisibility ResolveNRIPersistentVoxelInstanceVisibility(
	bool indirectOnly,
	int32_t actorIndex,
	const NRISceneInstanceVisibilityContext& context)
{
	NRISceneInstanceVisibility result = {};
	const bool directVisibleLocalPlayer =
		indirectOnly &&
		context.localPlayerPrimaryVisible &&
		actorIndex == context.localPlayerActorIndex;
	if (indirectOnly && !directVisibleLocalPlayer)
	{
		result.tlasMask = NRI_TLAS_MASK_REFLECTION | NRI_TLAS_MASK_GI;
		result.metadataFlags = SceneInstanceMetadataFlag_IndirectOnly;
	}
	return result;
}
