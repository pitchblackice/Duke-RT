#pragma once

#include "nri_se29_floor_deformer_policy.h"
#include "../scene/nri_map_deformer_layout.h"

#include <cstdint>
#include <vector>

namespace nri_scene
{
struct PTMapChunk;
struct PTMapWorld;
}

class NRIMapMoverSystem;
struct ResidentMapChunkRegistry;
struct StaticMapChunkAtlas;
struct StaticMapSceneCache;

struct NRISE29FloorDeformerRouteInput
{
	const NRIMapMoverSystem* movers = nullptr;
	const nri_scene::PTMapWorld* mapWorld = nullptr;
	const StaticMapSceneCache* staticScene = nullptr;
	const StaticMapChunkAtlas* atlas = nullptr;
	const ResidentMapChunkRegistry* registry = nullptr;
	const nri_scene::PTMapChunk* mapChunk = nullptr;
	const nri_scene::GeometryData* exactCurrentGeometry = nullptr;
	uint64_t frameIndex = 0;
	bool rayVisible = false;
	bool required = false;
	bool enabled = false;
};

struct NRISE29FloorDeformerRouteResult
{
	struct ElementSpan
	{
		uint32_t firstElement = 0;
		uint32_t elementCount = 0;
	};

	bool candidate = false;
	bool admitted = false;
	uint64_t stableKey = 0;
	uint32_t layoutRejectMask = nri_scene::MapDeformerLayoutReject_None;
	uint32_t policyRejectMask = NRISE29FloorDeformerReject_None;
	nri_scene::GeometryData canonicalGeometry;
	std::vector<ElementSpan> vertexSpans;
	std::vector<ElementSpan> primitiveSpans;
};

struct NRISE29FloorDeformerRouteFrameStats
{
	uint64_t frameIndex = 0;
	uint32_t candidates = 0;
	uint32_t admitted = 0;
	uint32_t exactFallbacks = 0;
	uint32_t layoutRejectMaskOr = 0;
	uint32_t policyRejectMaskOr = 0;
	uint32_t residentRejects = 0;
	uint32_t dependencyGroups = 0;
	uint32_t scheduled = 0;
	uint32_t budgetDeferred = 0;
	uint32_t pending = 0;
	uint32_t pendingHighWater = 0;
	uint64_t maxPendingAge = 0;
	uint32_t vertexSpans = 0;
	uint32_t primitiveSpans = 0;
	uint64_t vertexBytes = 0;
	uint64_t primitiveBytes = 0;
	uint32_t plannedRefitChunks = 0;
	uint32_t partialUploadChunks = 0;
	uint32_t partialUploadVertexSpans = 0;
	uint32_t partialUploadPrimitiveSpans = 0;
	uint64_t partialUploadVertexBytes = 0;
	uint64_t partialUploadPrimitiveBytes = 0;
	uint32_t applyFailures = 0;
	uint32_t blasUpdated = 0;
	uint32_t blasRecreated = 0;
};

class NRISE29FloorDeformerRoute
{
public:
	void BeginFrame(uint64_t frameIndex, uint64_t buildSerial, uint64_t mapEpoch);
	NRISE29FloorDeformerRouteResult TryCanonicalize(const NRISE29FloorDeformerRouteInput& input);
	void NotePartialUpload(
		uint64_t stableKey,
		uint32_t vertexSpanCount,
		uint32_t primitiveSpanCount,
		uint64_t vertexBytes,
		uint64_t primitiveBytes);
	void NoteApplyFailure(uint64_t stableKey);
	void NoteBlasUpdated(uint64_t stableKey);
	void NoteBlasRecreated(uint64_t stableKey);
	void Reset();

	const NRISE29FloorDeformerRouteFrameStats& GetFrameStats() const { return m_frameStats; }

private:
	struct PendingAge
	{
		NRISE29FloorDeformerIdentity identity;
		uint64_t firstUnservedFrame = 0;
		uint64_t lastSeenFrame = 0;
	};

	NRISE29FloorDeformerPendingSet m_pending;
	NRISE29FloorDeformerBudget m_remainingBudget;
	std::vector<NRISE29FloorDeformerIdentity> m_scheduled;
	std::vector<PendingAge> m_pendingAges;
	NRISE29FloorDeformerRouteFrameStats m_frameStats;
	uint64_t m_buildSerial = 0;
	uint64_t m_mapEpoch = 0;
	uint64_t m_scheduledSinceFrame = 0;
	uint32_t m_pendingHighWater = 0;
};
