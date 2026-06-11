#include "nri_runtime_mutation.h"

#include "../scene/nri_hash.h"

#include "printf.h"

#include <algorithm>
#include <cstddef>
#include <unordered_map>

namespace
{
	struct RuntimeMutationLightIdentitySurfaceKey
	{
		uint32_t kind = UINT32_MAX;
		uint32_t sourceType = (uint32_t)nri_scene::SurfaceSourceType::Unknown;
		int32_t sectorIndex = -1;
		int32_t wallIndex = -1;
		int32_t sectionIndex = -1;
		int32_t nextSectorIndex = -1;
		int32_t actorIndex = -1;
		uint32_t cstat = 0;
		uint32_t materialFlags = 0;
		uint32_t primaryKey = UINT32_MAX;
		uint32_t secondaryKey = UINT32_MAX;

		bool operator==(const RuntimeMutationLightIdentitySurfaceKey& other) const
		{
			return kind == other.kind &&
				sourceType == other.sourceType &&
				sectorIndex == other.sectorIndex &&
				wallIndex == other.wallIndex &&
				sectionIndex == other.sectionIndex &&
				nextSectorIndex == other.nextSectorIndex &&
				actorIndex == other.actorIndex &&
				cstat == other.cstat &&
				materialFlags == other.materialFlags &&
				primaryKey == other.primaryKey &&
				secondaryKey == other.secondaryKey;
		}
	};

	struct RuntimeMutationLightIdentitySurfaceKeyHash
	{
		size_t operator()(const RuntimeMutationLightIdentitySurfaceKey& key) const
		{
			size_t h = 1469598103934665603ull;
			const auto mix = [&h](uint64_t value)
			{
				h ^= (size_t)value;
				h *= 1099511628211ull;
			};
			mix(key.kind);
			mix(key.sourceType);
			mix((uint32_t)key.sectorIndex);
			mix((uint32_t)key.wallIndex);
			mix((uint32_t)key.sectionIndex);
			mix((uint32_t)key.nextSectorIndex);
			mix((uint32_t)key.actorIndex);
			mix(key.cstat);
			mix(key.materialFlags);
			mix(key.primaryKey);
			mix(key.secondaryKey);
			return h;
		}
	};

	static void AppendMutationReasonToken(std::string& text, const char* token)
	{
		if (!text.empty())
		{
			text += "|";
		}
		text += token;
	}

	static RuntimeMutationLightIdentitySurfaceKey BuildRuntimeMutationLightIdentitySurfaceKey(const nri_scene::PTMapSurface& surface)
	{
		RuntimeMutationLightIdentitySurfaceKey key = {};
		key.kind = (uint32_t)surface.kind;
		key.sourceType = (uint32_t)surface.surface.provenance.sourceType;
		key.sectorIndex = surface.surface.provenance.sectorIndex;
		key.wallIndex = surface.surface.provenance.wallIndex;
		key.sectionIndex = surface.surface.provenance.sectionIndex;
		key.nextSectorIndex = surface.surface.provenance.nextSectorIndex;
		key.actorIndex = surface.surface.provenance.actorIndex;
		key.cstat = surface.surface.provenance.cstat;
		key.materialFlags = surface.surface.provenance.materialFlags;
		key.primaryKey = surface.key.primary;
		key.secondaryKey = surface.key.secondary;
		return key;
	}

	static void ComputeRuntimeMutationSurfaceCenter(const nri_scene::SurfaceRef& surface, float outCenter[3])
	{
		outCenter[0] = 0.0f;
		outCenter[1] = 0.0f;
		outCenter[2] = 0.0f;
		if (surface.vertices.empty())
		{
			return;
		}

		for (const nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			outCenter[0] += vertex.position[0];
			outCenter[1] += vertex.position[1];
			outCenter[2] += vertex.position[2];
		}

		const float invCount = 1.0f / (float)surface.vertices.size();
		outCenter[0] *= invCount;
		outCenter[1] *= invCount;
		outCenter[2] *= invCount;
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

void NRIRuntimeMutationSystem::BuildLightIdentityOverrides(
	const nri_scene::PTMapWorld& staticWorld,
	const nri_scene::PTMapChunk& staticChunk,
	const nri_scene::PTMapWorld& liveWorld,
	const nri_scene::PTMapChunk& liveChunk,
	SceneLightSystem::SurfaceIdentityOverrides& outOverrides) const
{
	outOverrides.Clear();
	if (!staticWorld.valid || !liveWorld.valid)
	{
		return;
	}

	std::vector<uint32_t> staticSurfaceIndices;
	std::vector<uint32_t> liveSurfaceIndices;
	staticSurfaceIndices.reserve(staticChunk.surfaceCount);
	liveSurfaceIndices.reserve(liveChunk.surfaceCount);

	for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < staticChunk.surfaceCount; ++localSurfaceIndex)
	{
		const uint32_t surfaceIndex = staticChunk.firstSurface + localSurfaceIndex;
		if (surfaceIndex >= staticWorld.surfaces.size())
		{
			break;
		}
		staticSurfaceIndices.push_back(surfaceIndex);
	}

	for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < liveChunk.surfaceCount; ++localSurfaceIndex)
	{
		const uint32_t surfaceIndex = liveChunk.firstSurface + localSurfaceIndex;
		if (surfaceIndex >= liveWorld.surfaces.size())
		{
			break;
		}
		liveSurfaceIndices.push_back(surfaceIndex);
	}

	std::unordered_map<RuntimeMutationLightIdentitySurfaceKey, std::vector<uint32_t>, RuntimeMutationLightIdentitySurfaceKeyHash> liveSurfaceLookup;
	liveSurfaceLookup.reserve(liveSurfaceIndices.size());
	for (uint32_t liveLocalIndex = 0; liveLocalIndex < (uint32_t)liveSurfaceIndices.size(); ++liveLocalIndex)
	{
		const auto& liveSurface = liveWorld.surfaces[liveSurfaceIndices[liveLocalIndex]];
		liveSurfaceLookup[BuildRuntimeMutationLightIdentitySurfaceKey(liveSurface)].push_back(liveLocalIndex);
	}

	std::vector<uint8_t> liveSurfaceUsed(liveSurfaceIndices.size(), 0u);
	std::vector<uint64_t> inheritedIdentityKeys(liveSurfaceIndices.size(), 0ull);
	float staticSurfaceCenter[3] = {};
	for (uint32_t staticSurfaceIndex : staticSurfaceIndices)
	{
		const auto& staticSurface = staticWorld.surfaces[staticSurfaceIndex];
		const RuntimeMutationLightIdentitySurfaceKey key = BuildRuntimeMutationLightIdentitySurfaceKey(staticSurface);
		auto liveSurfaceIt = liveSurfaceLookup.find(key);
		if (liveSurfaceIt == liveSurfaceLookup.end())
		{
			continue;
		}

		uint32_t matchedLiveLocalIndex = UINT32_MAX;
		for (uint32_t candidate : liveSurfaceIt->second)
		{
			if (candidate < liveSurfaceUsed.size() && liveSurfaceUsed[candidate] == 0u)
			{
				matchedLiveLocalIndex = candidate;
				break;
			}
		}
		if (matchedLiveLocalIndex == UINT32_MAX)
		{
			continue;
		}

		liveSurfaceUsed[matchedLiveLocalIndex] = 1u;
		ComputeRuntimeMutationSurfaceCenter(staticSurface.surface, staticSurfaceCenter);
		inheritedIdentityKeys[matchedLiveLocalIndex] = SceneLightSystem::ComputeSurfaceIdentityKey(
			SceneLightRecordSource::StaticMapScene,
			staticSurface.surface.provenance,
			staticSurfaceCenter);
	}

	outOverrides.opaqueWalls.reserve(liveWorld.stats.wallSurfaceCount);
	outOverrides.opaqueFlats.reserve(liveWorld.stats.flatSurfaceCount);
	for (uint32_t liveLocalIndex = 0; liveLocalIndex < (uint32_t)liveSurfaceIndices.size(); ++liveLocalIndex)
	{
		const auto& liveSurface = liveWorld.surfaces[liveSurfaceIndices[liveLocalIndex]];
		if ((liveSurface.surface.material.flags & nri_scene::MaterialFlag_Sky) != 0 && liveSurface.surface.material.texture != nullptr)
		{
			continue;
		}

		switch (liveSurface.kind)
		{
		case nri_scene::PTMapSurfaceKind::Floor:
		case nri_scene::PTMapSurfaceKind::Ceiling:
			outOverrides.opaqueFlats.push_back(inheritedIdentityKeys[liveLocalIndex]);
			break;
		default:
			outOverrides.opaqueWalls.push_back(inheritedIdentityKeys[liveLocalIndex]);
			break;
		}
	}
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

void NRIRuntimeMutationSystem::EnsureSignatureWatchlist(uint64_t buildSerial, uint32_t chunkCount)
{
	if (signatureWatchlistBuildSerial == buildSerial && signatureWatchlist.size() == chunkCount)
	{
		return;
	}

	PrepareSignatureWatchlist(buildSerial, chunkCount);
}

bool NRIRuntimeMutationSystem::SeedSignatureWatchlist(uint32_t chunkIndex)
{
	if (chunkIndex >= signatureWatchlist.size() || signatureWatchlist[chunkIndex] != 0u)
	{
		return false;
	}

	signatureWatchlist[chunkIndex] = 1u;
	return true;
}

bool NRIRuntimeMutationSystem::IsSignatureWatchlistSeeded(uint32_t chunkIndex) const
{
	return chunkIndex < signatureWatchlist.size() && signatureWatchlist[chunkIndex] != 0u;
}

uint32_t NRIRuntimeMutationSystem::GetSignatureWatchlistSeedCount() const
{
	return (uint32_t)std::count(signatureWatchlist.begin(), signatureWatchlist.end(), (uint8_t)1u);
}

uint32_t NRIRuntimeMutationSystem::GetWorklistSweepChunkIndex(uint32_t sweepOffset, uint32_t chunkCount) const
{
	if (chunkCount == 0)
	{
		return 0;
	}
	return (worklistSweepCursor + sweepOffset) % chunkCount;
}

void NRIRuntimeMutationSystem::AdvanceWorklistSweepCursor(uint32_t sweepCount, uint32_t chunkCount)
{
	if (chunkCount == 0)
	{
		worklistSweepCursor = 0;
		return;
	}
	worklistSweepCursor = (worklistSweepCursor + sweepCount) % chunkCount;
}

void NRIRuntimeMutationSystem::FinalizeFrameActive()
{
	lastFrame.active =
		lastFrame.dirtyChunkCount > 0 ||
		lastFrame.residentAppliedChunkCount > 0 ||
		lastFrame.residentFallbackChunkCount > 0;
}

bool NRIRuntimeMutationSystem::HasStartupMaterialOnlyMutation() const
{
	return lastFrame.sectorMaterialChunkCount + lastFrame.wallMaterialChunkCount > 0;
}

uint32_t NRIRuntimeMutationSystem::GetDirtyChunkCount() const
{
	return lastFrame.dirtyChunkCount;
}

uint32_t NRIRuntimeMutationSystem::GetStartupMaterialOnlyDirtyChunkCount() const
{
	return lastFrame.sectorMaterialChunkCount + lastFrame.wallMaterialChunkCount;
}

void NRIRuntimeMutationSystem::MarkFrameInactive()
{
	lastFrame.active = false;
}

void NRIRuntimeMutationSystem::UpdateHighWaterStats(const RuntimeMutationCacheStats& cacheStats)
{
	cacheHighWaterStats.activeChunkCount = std::max(cacheHighWaterStats.activeChunkCount, cacheStats.activeChunkCount);
	cacheHighWaterStats.validChunkCount = std::max(cacheHighWaterStats.validChunkCount, cacheStats.validChunkCount);
	cacheHighWaterStats.excludedStaticChunkCount = std::max(cacheHighWaterStats.excludedStaticChunkCount, cacheStats.excludedStaticChunkCount);
	cacheHighWaterStats.cachedSurfaceCount = std::max(cacheHighWaterStats.cachedSurfaceCount, cacheStats.cachedSurfaceCount);
	cacheHighWaterStats.cachedTriangleCount = std::max(cacheHighWaterStats.cachedTriangleCount, cacheStats.cachedTriangleCount);
	cacheHighWaterStats.cachedMaterialCount = std::max(cacheHighWaterStats.cachedMaterialCount, cacheStats.cachedMaterialCount);
	cacheHighWaterStats.cachedMaterialStateCount = std::max(cacheHighWaterStats.cachedMaterialStateCount, cacheStats.cachedMaterialStateCount);
}
