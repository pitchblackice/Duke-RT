#pragma once

#include "gamestruct.h"

#include <array>
#include <cstdint>

struct RuntimeSpaceLinkFrameState
{
	bool active = false;
	bool geoEffectActive = false;
	bool topologyChanged = false;
	bool queryAttempted = false;
	bool queryRejected = false;
	int32_t candidateSectorIndex = -1;
	int32_t candidateSectorLotag = -1;
	int32_t sourceSectorIndex = -1;
	int32_t reportedGeoCount = 0;
	uint32_t viewRootSectorCount = 0;
	uint32_t visibleSectorCount = 0;
	uint32_t providerSectorCount = 0;
	uint32_t geoProviderCount = 0;
	uint32_t providerGroupCount = 0;
	uint32_t localSpaceMatchedProviderCount = 0;
	uint32_t visibleMatchedProviderCount = 0;
	uint32_t linkCount = 0;
	uint32_t translatedChunkCount = 0;
	uint32_t orphanLocalSpaceCount = 0;
	uint32_t unresolvedRuntimePortalCount = 0;
	uint32_t surfaceCount = 0;
	uint32_t triangleCount = 0;
	uint32_t materialCount = 0;
};

struct RuntimeChunkTranslationState
{
	uint32_t chunkIndex = UINT32_MAX;
	float dx = 0.0f;
	float dz = 0.0f;
};

struct RuntimeLinkTraceState
{
	bool valid = false;
	int32_t candidateSectorIndex = -1;
	int32_t sourceSectorIndex = -1;
	bool geoEffectActive = false;
	uint32_t visibleTaggedSectorCount = 0;
	uint32_t visible848SectorCount = 0;
	uint32_t visibleTeleportSectorCount = 0;
	uint32_t taggedVisibleSectorStoredCount = 0;
	std::array<RuntimeTaggedSectorDebugInfo, 8> taggedVisibleSectors = {};
	uint32_t nearbyControlSectorStoredCount = 0;
	std::array<RuntimeTaggedSectorDebugInfo, 12> nearbyControlSectors = {};
	RuntimeLinkDebugState game = {};
};
