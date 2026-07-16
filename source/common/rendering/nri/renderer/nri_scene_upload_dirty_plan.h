#pragma once

#include "nri_scene_upload_identity.h"

#include <cstdint>
#include <vector>

struct NRISceneUploadDirtyPlan
{
	std::vector<SceneUploadDirtyRange> ranges;
	uint64_t changedBytes = 0;
	uint64_t uploadBytes = 0;
	uint64_t gapBytes = 0;
	uint32_t rawRanges = 0;
	uint32_t rejectedCoalesces = 0;
	bool typed = false;
	bool forceFull = false;
};

NRISceneUploadDirtyPlan BuildNRISceneUploadDirtyPlan(
	const std::vector<NRISceneBufferUploadDomainSpan>& currentSpans,
	const std::vector<NRISceneBufferUploadDomainSpan>& previousSpans,
	NRISceneUploadBufferKind kind,
	uint64_t payloadSize,
	uint32_t stride,
	uint64_t currentExtraIdentity,
	uint64_t previousExtraIdentity,
	uint64_t maxGapBytes);
