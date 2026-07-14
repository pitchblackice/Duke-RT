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

inline NRISceneInstanceVisibility ResolveNRIPersistentVoxelInstanceVisibility(bool indirectOnly)
{
	NRISceneInstanceVisibility result = {};
	if (indirectOnly)
	{
		result.tlasMask = NRI_TLAS_MASK_REFLECTION | NRI_TLAS_MASK_GI;
		result.metadataFlags = SceneInstanceMetadataFlag_IndirectOnly;
	}
	return result;
}
