#pragma once

#include "nri_resources.h"
#include "nri_scene_lights.h"

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_map_builder.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"

#include <cstdint>
#include <string>
#include <vector>

static constexpr size_t RuntimeMutationTopTraceCount = 8;

enum class RuntimeMutationTraceAction : uint8_t
{
	None,
	StructuralRebuild,
	MaterialRefresh,
	ResidentApply,
	ResidentNoopSkip,
	ResidentFallback,
	Held,
	SyncSkip,
	DeferredMaterialRefresh,
	DeferredStructuralRebuild,
	Failed
};

struct RuntimeMutationTopTraceEntry
{
	bool valid = false;
	uint32_t score = 0;
	uint32_t chunkIndex = UINT32_MAX;
	int32_t sectorIndex = -1;
	uint32_t reasonMask = 0;
	uint32_t sectionDirtyCount = 0;
	uint32_t surfaceCount = 0;
	uint32_t triangleCount = 0;
	uint32_t materialCount = 0;
	RuntimeMutationTraceAction action = RuntimeMutationTraceAction::None;
	bool forceTopology = false;
	bool residentMaterialDirty = false;
	bool residentGeometryDirty = false;
	bool recoveredEmpty = false;
};

enum RuntimeMutationWorklistCandidateSourceBits : uint32_t
{
	RuntimeMutationWorklistCandidateSource_ActiveReplacement = 1 << 0,
	RuntimeMutationWorklistCandidateSource_VisibleResidentValidation = 1 << 1,
	RuntimeMutationWorklistCandidateSource_StartupVisibleValidation = 1 << 2,
	RuntimeMutationWorklistCandidateSource_UnresolvedAuthoredTextures = 1 << 3,
	RuntimeMutationWorklistCandidateSource_StaticAnimatedSuppressed = 1 << 4,
	RuntimeMutationWorklistCandidateSource_SectorDirty = 1 << 5,
	RuntimeMutationWorklistCandidateSource_SectionDirty = 1 << 6,
	RuntimeMutationWorklistCandidateSource_Dragged = 1 << 7,
	RuntimeMutationWorklistCandidateSource_SignatureWatchlist = 1 << 8,
	RuntimeMutationWorklistCandidateSource_BackgroundSweep = 1 << 9,
	RuntimeMutationWorklistCandidateSource_DeferredMaterialRefresh = 1 << 10,
	RuntimeMutationWorklistCandidateSource_DeferredStructuralRebuild = 1 << 11,
};

const char* GetRuntimeMutationTraceActionName(RuntimeMutationTraceAction action);
uint32_t ScoreRuntimeMutationTopTraceEntry(const RuntimeMutationTopTraceEntry& entry);
std::string GetRuntimeMapMutationReasonSummary(uint32_t reasonMask);
std::string GetRuntimeMutationWorklistCandidateSourceSummary(uint32_t sourceMask);

struct RuntimeMutationCacheStats
{
	uint32_t activeChunkCount = 0;
	uint32_t validChunkCount = 0;
	uint32_t excludedStaticChunkCount = 0;
	uint32_t cachedSurfaceCount = 0;
	uint32_t cachedTriangleCount = 0;
	uint32_t cachedMaterialCount = 0;
	uint32_t cachedMaterialStateCount = 0;
};

struct RuntimeMapMutationCache
{
	struct ChunkReplacement
	{
		struct MaterialStateCacheEntry
		{
			uint64_t animatedGeometrySignature = 0;
			uint64_t animatedMaterialSignature = 0;
			uint32_t surfaceCount = 0;
			nri_scene::MaterialBridgeData materialBridge;
		};

		nri_scene::PTMapChunkMutationBaseline baseline;
		nri_scene::PTMapChunkMutationBaseline replacementBaseline;
		uint64_t baselineSignature = 0;
		uint64_t liveSignature = 0;
		uint64_t animatedMaterialSignature = 0;
		uint64_t lastTraceSignature = UINT64_MAX;
		uint64_t lastTraceAnimatedMaterialSignature = UINT64_MAX;
		uint32_t reasonMask = 0;
		uint32_t sectionDirtyCount = 0;
		uint32_t stableMutationFrameCount = 0;
		uint32_t lastTraceReasonMask = UINT32_MAX;
		uint32_t traceCount = 0;
		bool active = false;
		bool valid = false;
		bool residentAuthoritative = false;
		bool sectorDirty = false;
		bool dragged = false;
		bool blindSpot = false;
		bool excludeStaticChunk = false;
		bool staticAnimatedReplacement = false;
		bool lastTraceActive = false;
		bool lastTraceBlindSpot = false;
		bool animationOnlyRefreshed = false;
		bool lastTraceAnimationOnlyRefreshed = false;
		bool lastTraceStaticAnimatedReplacement = false;
		bool deferredMaterialRefresh = false;
		uint64_t deferredMaterialFrame = 0;
		bool deferredStructuralRebuild = false;
		uint64_t deferredStructuralFrame = 0;
		uint32_t surfaceCount = 0;
		uint32_t triangleCount = 0;
		SceneLightSystem::SurfaceIdentityOverrides lightIdentityOverrides;
		nri_scene::SceneView sceneView;
		nri_scene::GeometryData geometry;
		nri_scene::MaterialBridgeData materialBridge;
		std::vector<MaterialStateCacheEntry> materialStateCache;
	};

	std::vector<ChunkReplacement> chunks;
};

struct RuntimeMutationResidentApplyMode
{
	bool materialOnlyReplacement = false;
	bool exclusiveMaterialOnlyReplacement = false;
	bool fastResidentMaterialOnlyUpdate = false;
};

struct RuntimeMutationResidentUploadRange
{
	int uploadKind = 0;
	uint64_t byteOffset = 0;
	uint64_t size = 0;
	uint64_t dirtySize = 0;
};

struct NRIRuntimeMutationResidentUploadServices
{
	using StageGeometryRangesFn = bool (*)(void* user, const std::vector<RuntimeMutationResidentUploadRange>& ranges);
	using NoteUploadRangeFn = void (*)(void* user, int uploadKind, uint64_t size);
	using NoteCoalescedRangeFn = void (*)(void* user, const RuntimeMutationResidentUploadRange& range);
	using NoteCoalescedRejectFn = void (*)(void* user);

	void* user = nullptr;
	StageGeometryRangesFn stageGeometryRanges = nullptr;
	NoteUploadRangeFn noteUploadRange = nullptr;
	NoteCoalescedRangeFn noteCoalescedRange = nullptr;
	NoteCoalescedRejectFn noteCoalescedReject = nullptr;

	bool StageGeometryRanges(const std::vector<RuntimeMutationResidentUploadRange>& ranges) const;
	void NoteUploadRange(int uploadKind, uint64_t size) const;
	void NoteCoalescedRange(const RuntimeMutationResidentUploadRange& range) const;
	void NoteCoalescedReject() const;
};

struct RuntimeMapMutationFrameState
{
	bool active = false;
	uint32_t dirtyChunkCount = 0;
	uint32_t residentAppliedChunkCount = 0;
	uint32_t residentGeometryChunkCount = 0;
	uint32_t residentMaterialChunkCount = 0;
	uint32_t residentAtlasGrowCount = 0;
	uint32_t residentFallbackChunkCount = 0;
	uint32_t rebuiltChunkCount = 0;
	uint32_t heldChunkCount = 0;
	uint32_t blindSpotChunkCount = 0;
	uint32_t sectorGeometryChunkCount = 0;
	uint32_t sectorMaterialChunkCount = 0;
	uint32_t wallGeometryChunkCount = 0;
	uint32_t wallMaterialChunkCount = 0;
	uint32_t sectorDirtyChunkCount = 0;
	uint32_t sectionDirtyChunkCount = 0;
	uint32_t draggedChunkCount = 0;
	uint32_t animatedRefreshChunkCount = 0;
	uint32_t replacementSurfaceCount = 0;
	uint32_t replacementTriangleCount = 0;
	uint32_t materialCount = 0;
};

class NRIRuntimeMutationSystem
{
public:
	RuntimeMutationCacheStats GatherCacheStats() const;
	void PrintStatus() const;
	void ClearReplacementPayload(RuntimeMapMutationCache::ChunkReplacement& replacement, bool clearMaterialStateCache);
	void TraceChunk(
		const nri_scene::PTMapChunk& mapChunk,
		RuntimeMapMutationCache::ChunkReplacement& replacement,
		bool traceEnabled,
		int traceChunkIndex,
		int traceSectorIndex);
	void ClearResidentGeometryUploadRanges();
	bool QueueResidentGeometryUploadRange(
		int uploadKind,
		uint64_t byteOffset,
		uint64_t size,
		const NRIRuntimeMutationResidentUploadServices& services);
	bool FlushResidentGeometryUploadRanges(const NRIRuntimeMutationResidentUploadServices& services);
	void NoteResidentAtlasGrow();
	void ResetCacheAndFrame();
	void ResetCacheForStaticSceneBuild(uint32_t chunkCount);
	void InitializeStaticChunkReplacement(const nri_scene::PTMapChunk& chunk);

	RuntimeMapMutationCache cache;
	RuntimeMapMutationFrameState lastFrame = {};
	RuntimeMutationCacheStats cacheHighWaterStats = {};
	std::vector<RuntimeMutationResidentUploadRange> residentGeometryUploadRanges;
	std::vector<uint8_t> signatureWatchlist;
	uint64_t signatureWatchlistBuildSerial = 0;
	uint32_t worklistSweepCursor = 0;
};
