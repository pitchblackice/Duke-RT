#include "nri_runtime_mutation.h"

#include "printf.h"

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
