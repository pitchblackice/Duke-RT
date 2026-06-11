#include "nri_runtime_mutation.h"

#include "../scene/nri_hash.h"

#include "printf.h"

#include <algorithm>

namespace
{
	static void AppendMutationReasonToken(std::string& text, const char* token)
	{
		if (!text.empty())
		{
			text += "|";
		}
		text += token;
	}
}

bool NRIRuntimeMutationResidentUploadServices::StageGeometryRanges(const std::vector<RuntimeMutationResidentUploadRange>& ranges) const
{
	return stageGeometryRanges != nullptr && stageGeometryRanges(user, ranges);
}

void NRIRuntimeMutationResidentUploadServices::NoteUploadRange(int uploadKind, uint64_t size) const
{
	if (noteUploadRange != nullptr)
	{
		noteUploadRange(user, uploadKind, size);
	}
}

void NRIRuntimeMutationResidentUploadServices::NoteCoalescedRange(const RuntimeMutationResidentUploadRange& range) const
{
	if (noteCoalescedRange != nullptr)
	{
		noteCoalescedRange(user, range);
	}
}

void NRIRuntimeMutationResidentUploadServices::NoteCoalescedReject() const
{
	if (noteCoalescedReject != nullptr)
	{
		noteCoalescedReject(user);
	}
}

const char* GetRuntimeMutationTraceActionName(RuntimeMutationTraceAction action)
{
	switch (action)
	{
	case RuntimeMutationTraceAction::StructuralRebuild: return "rebuild";
	case RuntimeMutationTraceAction::MaterialRefresh: return "material-refresh";
	case RuntimeMutationTraceAction::ResidentApply: return "resident-apply";
	case RuntimeMutationTraceAction::ResidentNoopSkip: return "resident-noop-skip";
	case RuntimeMutationTraceAction::ResidentFallback: return "fallback";
	case RuntimeMutationTraceAction::Held: return "held";
	case RuntimeMutationTraceAction::SyncSkip: return "sync-skip";
	case RuntimeMutationTraceAction::DeferredMaterialRefresh: return "deferred-material-refresh";
	case RuntimeMutationTraceAction::DeferredStructuralRebuild: return "deferred-structural-rebuild";
	case RuntimeMutationTraceAction::Failed: return "failed";
	default: return "none";
	}
}

uint32_t ScoreRuntimeMutationTopTraceEntry(const RuntimeMutationTopTraceEntry& entry)
{
	uint32_t score = entry.triangleCount * 16u + entry.materialCount * 32u + entry.surfaceCount * 8u;
	if (entry.forceTopology)
	{
		score += 1u << 20;
	}
	if (entry.recoveredEmpty)
	{
		score += 1u << 19;
	}
	if (entry.residentGeometryDirty)
	{
		score += 1u << 18;
	}
	if (entry.residentMaterialDirty)
	{
		score += 1u << 17;
	}
	switch (entry.action)
	{
	case RuntimeMutationTraceAction::ResidentFallback:
		score += 1u << 16;
		break;
	case RuntimeMutationTraceAction::ResidentNoopSkip:
		score += 1u << 15;
		break;
	case RuntimeMutationTraceAction::ResidentApply:
		score += 1u << 14;
		break;
	case RuntimeMutationTraceAction::StructuralRebuild:
		score += 1u << 13;
		break;
	case RuntimeMutationTraceAction::MaterialRefresh:
		score += 1u << 12;
		break;
	case RuntimeMutationTraceAction::SyncSkip:
		score += 1u << 11;
		break;
	case RuntimeMutationTraceAction::DeferredMaterialRefresh:
		score += 1u << 10;
		break;
	default:
		break;
	}
	return score;
}

std::string GetRuntimeMapMutationReasonSummary(uint32_t reasonMask)
{
	std::string text;
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectorGeometry) != 0)
	{
		AppendMutationReasonToken(text, "sector_geom");
	}
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectorMaterial) != 0)
	{
		AppendMutationReasonToken(text, "sector_mat");
	}
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_WallGeometry) != 0)
	{
		AppendMutationReasonToken(text, "wall_geom");
	}
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_WallMaterial) != 0)
	{
		AppendMutationReasonToken(text, "wall_mat");
	}
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectorDirty) != 0)
	{
		AppendMutationReasonToken(text, "sector_dirty");
	}
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectionDirty) != 0)
	{
		AppendMutationReasonToken(text, "section_dirty");
	}
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_Dragged) != 0)
	{
		AppendMutationReasonToken(text, "dragged");
	}
	if (text.empty())
	{
		text = "none";
	}
	return text;
}

std::string GetRuntimeMutationWorklistCandidateSourceSummary(uint32_t sourceMask)
{
	std::string text;
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_ActiveReplacement) != 0)
	{
		AppendMutationReasonToken(text, "active_replacement");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_VisibleResidentValidation) != 0)
	{
		AppendMutationReasonToken(text, "visible_resident_validation");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_StartupVisibleValidation) != 0)
	{
		AppendMutationReasonToken(text, "startup_visible_validation");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_UnresolvedAuthoredTextures) != 0)
	{
		AppendMutationReasonToken(text, "unresolved_authored_textures");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_StaticAnimatedSuppressed) != 0)
	{
		AppendMutationReasonToken(text, "static_animated_suppressed");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_SectorDirty) != 0)
	{
		AppendMutationReasonToken(text, "sector_dirty");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_SectionDirty) != 0)
	{
		AppendMutationReasonToken(text, "section_dirty");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_Dragged) != 0)
	{
		AppendMutationReasonToken(text, "dragged");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_SignatureWatchlist) != 0)
	{
		AppendMutationReasonToken(text, "signature_watchlist");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_BackgroundSweep) != 0)
	{
		AppendMutationReasonToken(text, "background_sweep");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_DeferredMaterialRefresh) != 0)
	{
		AppendMutationReasonToken(text, "deferred_material_refresh");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_DeferredStructuralRebuild) != 0)
	{
		AppendMutationReasonToken(text, "deferred_structural_rebuild");
	}
	if (text.empty())
	{
		text = "none";
	}
	return text;
}

RuntimeMutationCacheStats NRIRuntimeMutationSystem::GatherCacheStats() const
{
	RuntimeMutationCacheStats stats = {};
	for (const auto& replacement : cache.chunks)
	{
		stats.cachedMaterialStateCount += (uint32_t)replacement.materialStateCache.size();
		if (!replacement.active)
		{
			continue;
		}

		stats.activeChunkCount++;
		if (!replacement.valid)
		{
			continue;
		}

		stats.validChunkCount++;
		if (replacement.excludeStaticChunk)
		{
			stats.excludedStaticChunkCount++;
		}

		stats.cachedSurfaceCount += replacement.surfaceCount;
		stats.cachedTriangleCount += replacement.triangleCount;
		stats.cachedMaterialCount += (uint32_t)replacement.materialBridge.materials.size();
	}

	return stats;
}

bool NRIRuntimeMutationSystem::IsCacheEmpty() const
{
	return cache.chunks.empty();
}

uint32_t NRIRuntimeMutationSystem::GetCacheChunkCount() const
{
	return (uint32_t)cache.chunks.size();
}

uint64_t NRIRuntimeMutationSystem::BuildFrameGenerationHash(bool hasRuntimeMutationOverlay) const
{
	if (!hasRuntimeMutationOverlay && !lastFrame.active)
	{
		return 0ull;
	}

	uint64_t hash = 1469598103934665603ull;
	hash = nri_scene::HashCombine64(hash, (uint64_t)lastFrame.dirtyChunkCount);
	hash = nri_scene::HashCombine64(hash, (uint64_t)lastFrame.residentAppliedChunkCount);
	hash = nri_scene::HashCombine64(hash, (uint64_t)lastFrame.residentGeometryChunkCount);
	hash = nri_scene::HashCombine64(hash, (uint64_t)lastFrame.residentMaterialChunkCount);
	hash = nri_scene::HashCombine64(hash, (uint64_t)lastFrame.rebuiltChunkCount);
	hash = nri_scene::HashCombine64(hash, (uint64_t)lastFrame.heldChunkCount);
	hash = nri_scene::HashCombine64(hash, (uint64_t)lastFrame.replacementTriangleCount);
	hash = nri_scene::HashCombine64(hash, (uint64_t)lastFrame.materialCount);
	return hash;
}

const RuntimeMapMutationCache::ChunkReplacement* NRIRuntimeMutationSystem::FindReplacement(uint32_t chunkIndex) const
{
	return chunkIndex < cache.chunks.size() ? &cache.chunks[chunkIndex] : nullptr;
}

RuntimeMapMutationCache::ChunkReplacement* NRIRuntimeMutationSystem::FindReplacement(uint32_t chunkIndex)
{
	return chunkIndex < cache.chunks.size() ? &cache.chunks[chunkIndex] : nullptr;
}

bool NRIRuntimeMutationSystem::IsReplacementActive(uint32_t chunkIndex) const
{
	const auto* replacement = FindReplacement(chunkIndex);
	return replacement != nullptr && replacement->active;
}

bool NRIRuntimeMutationSystem::IsReplacementActiveAndValid(uint32_t chunkIndex) const
{
	const auto* replacement = FindReplacement(chunkIndex);
	return replacement != nullptr && replacement->active && replacement->valid;
}

uint32_t NRIRuntimeMutationSystem::AppendSceneLightRecords(SceneLightSystem& sceneLights) const
{
	uint32_t runtimeMutationMaterialOffset = 0;
	for (const auto& replacement : cache.chunks)
	{
		if (!replacement.active || !replacement.valid)
		{
			continue;
		}

		sceneLights.AppendSceneView(
			replacement.sceneView,
			replacement.materialBridge,
			SceneLightRecordSource::RuntimeMutationScene,
			runtimeMutationMaterialOffset,
			0u,
			&replacement.lightIdentityOverrides);
		runtimeMutationMaterialOffset += (uint32_t)replacement.materialBridge.materials.size();
	}
	return runtimeMutationMaterialOffset;
}

void NRIRuntimeMutationSystem::PrintStatus() const
{
	const RuntimeMutationCacheStats cacheStats = GatherCacheStats();

	Printf("NRI PT runtime map: active=%s dirty_chunks=%u resident_applied=%u resident_geom=%u resident_mat=%u resident_atlas_grows=%u resident_fallback=%u rebuilt_chunks=%u held_chunks=%u animated_refreshes=%u blind_spots=%u sector_geom=%u sector_mat=%u wall_geom=%u wall_mat=%u sector_dirty=%u section_dirty=%u dragged=%u surfaces=%u tris=%u materials=%u\n",
		lastFrame.active ? "yes" : "no",
		lastFrame.dirtyChunkCount,
		lastFrame.residentAppliedChunkCount,
		lastFrame.residentGeometryChunkCount,
		lastFrame.residentMaterialChunkCount,
		lastFrame.residentAtlasGrowCount,
		lastFrame.residentFallbackChunkCount,
		lastFrame.rebuiltChunkCount,
		lastFrame.heldChunkCount,
		lastFrame.animatedRefreshChunkCount,
		lastFrame.blindSpotChunkCount,
		lastFrame.sectorGeometryChunkCount,
		lastFrame.sectorMaterialChunkCount,
		lastFrame.wallGeometryChunkCount,
		lastFrame.wallMaterialChunkCount,
		lastFrame.sectorDirtyChunkCount,
		lastFrame.sectionDirtyChunkCount,
		lastFrame.draggedChunkCount,
		lastFrame.replacementSurfaceCount,
		lastFrame.replacementTriangleCount,
		lastFrame.materialCount);
	Printf("NRI PT runtime map cache: active_chunks=%u valid_chunks=%u exclude_static=%u cached_surfaces=%u cached_tris=%u cached_materials=%u cached_states=%u highwater=active:%u valid:%u exclude_static:%u surfaces:%u tris:%u mats:%u states:%u\n",
		cacheStats.activeChunkCount,
		cacheStats.validChunkCount,
		cacheStats.excludedStaticChunkCount,
		cacheStats.cachedSurfaceCount,
		cacheStats.cachedTriangleCount,
		cacheStats.cachedMaterialCount,
		cacheStats.cachedMaterialStateCount,
		cacheHighWaterStats.activeChunkCount,
		cacheHighWaterStats.validChunkCount,
		cacheHighWaterStats.excludedStaticChunkCount,
		cacheHighWaterStats.cachedSurfaceCount,
		cacheHighWaterStats.cachedTriangleCount,
		cacheHighWaterStats.cachedMaterialCount,
		cacheHighWaterStats.cachedMaterialStateCount);
}

void NRIRuntimeMutationSystem::ClearReplacementPayload(RuntimeMapMutationCache::ChunkReplacement& replacement, bool clearMaterialStateCache)
{
	replacement.lightIdentityOverrides.Clear();
	replacement.sceneView = {};
	replacement.geometry = {};
	replacement.materialBridge = {};
	replacement.deferredMaterialRefresh = false;
	replacement.deferredMaterialFrame = 0;
	replacement.deferredStructuralRebuild = false;
	replacement.deferredStructuralFrame = 0;
	if (clearMaterialStateCache)
	{
		replacement.materialStateCache.clear();
	}
}

void NRIRuntimeMutationSystem::TraceChunk(
	const nri_scene::PTMapChunk& mapChunk,
	RuntimeMapMutationCache::ChunkReplacement& replacement,
	bool traceEnabled,
	int traceChunkIndex,
	int traceSectorIndex)
{
	if (!traceEnabled)
	{
		return;
	}

	const bool filterByChunk = traceChunkIndex >= 0;
	const bool filterBySector = traceSectorIndex >= 0;
	if (!filterByChunk && !filterBySector)
	{
		return;
	}

	if (filterByChunk && mapChunk.chunkIndex != (uint32_t)traceChunkIndex)
	{
		return;
	}

	if (filterBySector && mapChunk.sectorIndex != traceSectorIndex)
	{
		return;
	}

	const bool changed =
		replacement.traceCount == 0 ||
		replacement.lastTraceSignature != replacement.liveSignature ||
		replacement.lastTraceAnimatedMaterialSignature != replacement.animatedMaterialSignature ||
		replacement.lastTraceReasonMask != replacement.reasonMask ||
		replacement.lastTraceActive != replacement.active ||
		replacement.lastTraceBlindSpot != replacement.blindSpot ||
		replacement.lastTraceAnimationOnlyRefreshed != replacement.animationOnlyRefreshed ||
		replacement.lastTraceStaticAnimatedReplacement != replacement.staticAnimatedReplacement;
	if (!changed)
	{
		return;
	}

	const std::string reasons = GetRuntimeMapMutationReasonSummary(replacement.reasonMask);
	Printf("NRI PT runtime map trace: chunk=%u sector=%d active=%s blind_spot=%s static_anim=%s signature_changed=%s anim_refresh=%s baseline_sig=0x%llx live_sig=0x%llx anim_sig=0x%llx reasons=%s section_dirty=%u sector_dirty=%s dragged=%s surfaces=%u tris=%u materials=%u\n",
		mapChunk.chunkIndex,
		mapChunk.sectorIndex,
		replacement.active ? "yes" : "no",
		replacement.blindSpot ? "yes" : "no",
		replacement.staticAnimatedReplacement ? "yes" : "no",
		replacement.liveSignature != replacement.baselineSignature ? "yes" : "no",
		replacement.animationOnlyRefreshed ? "yes" : "no",
		(unsigned long long)replacement.baselineSignature,
		(unsigned long long)replacement.liveSignature,
		(unsigned long long)replacement.animatedMaterialSignature,
		reasons.c_str(),
		replacement.sectionDirtyCount,
		replacement.sectorDirty ? "yes" : "no",
		replacement.dragged ? "yes" : "no",
		replacement.surfaceCount,
		replacement.triangleCount,
		(uint32_t)replacement.materialBridge.materials.size());

	replacement.lastTraceSignature = replacement.liveSignature;
	replacement.lastTraceAnimatedMaterialSignature = replacement.animatedMaterialSignature;
	replacement.lastTraceReasonMask = replacement.reasonMask;
	replacement.lastTraceActive = replacement.active;
	replacement.lastTraceBlindSpot = replacement.blindSpot;
	replacement.lastTraceAnimationOnlyRefreshed = replacement.animationOnlyRefreshed;
	replacement.lastTraceStaticAnimatedReplacement = replacement.staticAnimatedReplacement;
	replacement.traceCount++;
}

void NRIRuntimeMutationSystem::ClearResidentGeometryUploadRanges()
{
	residentGeometryUploadRanges.clear();
}

bool NRIRuntimeMutationSystem::QueueResidentGeometryUploadRange(
	int uploadKind,
	uint64_t byteOffset,
	uint64_t size,
	const NRIRuntimeMutationResidentUploadServices& services)
{
	if (size == 0)
	{
		return true;
	}

	switch (uploadKind)
	{
	case 0:
	case 1:
	case 2:
		services.NoteUploadRange(uploadKind, size);
		break;
	default:
		return false;
	}

	residentGeometryUploadRanges.push_back({ uploadKind, byteOffset, size, size });
	return true;
}

bool NRIRuntimeMutationSystem::FlushResidentGeometryUploadRanges(const NRIRuntimeMutationResidentUploadServices& services)
{
	if (residentGeometryUploadRanges.empty())
	{
		return true;
	}

	constexpr uint64_t kResidentUploadCoalesceMaxGapBytes = 4ull * 1024ull;
	constexpr uint64_t kResidentUploadCoalesceMaxByteExpansion = 2;
	auto clearAndFail = [&]()
	{
		residentGeometryUploadRanges.clear();
		return false;
	};

	std::sort(
		residentGeometryUploadRanges.begin(),
		residentGeometryUploadRanges.end(),
		[](const RuntimeMutationResidentUploadRange& a, const RuntimeMutationResidentUploadRange& b)
		{
			if (a.uploadKind != b.uploadKind)
			{
				return a.uploadKind < b.uploadKind;
			}
			return a.byteOffset < b.byteOffset;
		});

	std::vector<RuntimeMutationResidentUploadRange> coalescedRanges;
	coalescedRanges.reserve(residentGeometryUploadRanges.size());
	for (const RuntimeMutationResidentUploadRange& range : residentGeometryUploadRanges)
	{
		if (coalescedRanges.empty() ||
			coalescedRanges.back().uploadKind != range.uploadKind)
		{
			coalescedRanges.push_back(range);
			continue;
		}

		RuntimeMutationResidentUploadRange& tail = coalescedRanges.back();
		const uint64_t tailEnd = tail.byteOffset + tail.size;
		const uint64_t rangeEnd = range.byteOffset + range.size;
		const uint64_t gapBytes = range.byteOffset > tailEnd ? range.byteOffset - tailEnd : 0;
		const uint64_t candidateSize = rangeEnd > tailEnd ? rangeEnd - tail.byteOffset : tail.size;
		const uint64_t candidateDirtySize = tail.dirtySize + range.size;
		const bool acceptableByteExpansion =
			candidateDirtySize > UINT64_MAX / kResidentUploadCoalesceMaxByteExpansion ||
			candidateSize <= candidateDirtySize * kResidentUploadCoalesceMaxByteExpansion;
		if (gapBytes <= kResidentUploadCoalesceMaxGapBytes && acceptableByteExpansion)
		{
			if (rangeEnd > tailEnd)
			{
				tail.size = rangeEnd - tail.byteOffset;
			}
			tail.dirtySize += range.size;
			continue;
		}

		services.NoteCoalescedReject();
		coalescedRanges.push_back(range);
	}

	if (!services.StageGeometryRanges(coalescedRanges))
	{
		return clearAndFail();
	}

	for (const RuntimeMutationResidentUploadRange& range : coalescedRanges)
	{
		services.NoteCoalescedRange(range);
	}

	residentGeometryUploadRanges.clear();
	return true;
}

void NRIRuntimeMutationSystem::NoteResidentAtlasGrow()
{
	lastFrame.residentAtlasGrowCount++;
}

void NRIRuntimeMutationSystem::ResetCacheAndFrame()
{
	cache.chunks.clear();
	lastFrame = {};
}

void NRIRuntimeMutationSystem::ResetCacheForStaticSceneBuild(uint32_t chunkCount)
{
	cache.chunks.clear();
	cache.chunks.resize(chunkCount);
}

void NRIRuntimeMutationSystem::InitializeStaticChunkReplacement(const nri_scene::PTMapChunk& chunk)
{
	if (chunk.chunkIndex >= cache.chunks.size())
	{
		return;
	}

	auto& replacement = cache.chunks[chunk.chunkIndex];
	nri_scene::CaptureMapChunkMutationBaseline(chunk, replacement.baseline);
	replacement.replacementBaseline = replacement.baseline;
	replacement.baselineSignature = replacement.baseline.signature;
	replacement.liveSignature = replacement.baselineSignature;
	replacement.animatedMaterialSignature = 0;
	replacement.reasonMask = 0;
	replacement.sectionDirtyCount = 0;
	replacement.stableMutationFrameCount = 0;
	replacement.sectorDirty = false;
	replacement.dragged = false;
	replacement.blindSpot = false;
	replacement.excludeStaticChunk = false;
	replacement.staticAnimatedReplacement = false;
	replacement.lastTraceSignature = UINT64_MAX;
	replacement.lastTraceAnimatedMaterialSignature = UINT64_MAX;
	replacement.lastTraceReasonMask = UINT32_MAX;
	replacement.lastTraceActive = false;
	replacement.lastTraceBlindSpot = false;
	replacement.animationOnlyRefreshed = false;
	replacement.lastTraceAnimationOnlyRefreshed = false;
	replacement.lastTraceStaticAnimatedReplacement = false;
	replacement.traceCount = 0;
	replacement.surfaceCount = 0;
	replacement.triangleCount = 0;
	replacement.residentAuthoritative = true;
	ClearReplacementPayload(replacement, true);
}

void NRIRuntimeMutationSystem::ResetWorklist()
{
	signatureWatchlist.clear();
	signatureWatchlistBuildSerial = 0;
	worklistSweepCursor = 0;
}

void NRIRuntimeMutationSystem::ResetFrameState()
{
	lastFrame = {};
}

void NRIRuntimeMutationSystem::ResetHighWaterStats()
{
	cacheHighWaterStats = {};
}

void NRIRuntimeMutationSystem::PrepareSignatureWatchlist(uint64_t buildSerial, uint32_t chunkCount)
{
	signatureWatchlist.clear();
	signatureWatchlist.resize(chunkCount, 0u);
	signatureWatchlistBuildSerial = buildSerial;
	worklistSweepCursor = 0;
}

bool NRIRuntimeMutationSystem::HasCacheChunkCount(uint32_t chunkCount) const
{
	return cache.chunks.size() == chunkCount;
}
