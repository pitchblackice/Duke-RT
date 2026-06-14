#include "nri_debug_reporters.h"

#include "nri_renderer.h"
#include "../scene/nri_scene_stats.h"
#include "c_cvars.h"
#include "mapinfo.h"
#include "printf.h"

EXTERN_CVAR(Bool, nri_ptselftest)
EXTERN_CVAR(Bool, nri_ptscenestats)
EXTERN_CVAR(Float, nri_ptmirrordynamicdistance)
EXTERN_CVAR(Int, nri_ptactorspritetrace)

void NRIRendererDiagnostics::ResetSelfTestRouteSnapshot()
{
	mSelfTestRoute = {};
}

void NRIRendererDiagnostics::SetSelfTestRouteSnapshot(const char* routeName, const char* presenterName, const char* ownerName, const char* passes, bool denoiserRun, bool upscalerRun, bool exposureRun)
{
	if (!nri_ptselftest)
	{
		return;
	}

	mSelfTestRoute.routeName = routeName != nullptr ? routeName : "unknown";
	mSelfTestRoute.presenterName = presenterName != nullptr ? presenterName : "unknown";
	mSelfTestRoute.ownerName = ownerName != nullptr ? ownerName : "unknown";
	mSelfTestRoute.passes = passes != nullptr ? passes : "unknown";
	mSelfTestRoute.denoiserRun = denoiserRun;
	mSelfTestRoute.upscalerRun = upscalerRun;
	mSelfTestRoute.exposureRun = exposureRun;
}

void NRIRendererDiagnostics::EmitSelfTestSummary(const NRISelfTestSummarySnapshot& snapshot) const
{
	if (!nri_ptselftest)
	{
		return;
	}

	Printf("NRI PT selftest: frame=%u engine_frame=%u map=%s level=%s backend=nri api=%s world_active=%u menu_active=%s gameplay_frame=%u portal=%u drawmode=%d route=%s debug=%d passes=%s presenter=%s owner=%s denoiser_run=%u upscaler_run=%u exposure_run=%u present_kind=%s render_width=%u render_height=%u output_width=%u output_height=%u swapchain_format=%u hdr=%u prims=%u mats=%u scene_instances=%u static_instances=%u dynamic_instances=%u voxel_instances=%u emissive_instances=%u vertices=%u indices=%u vertex_bytes=%llu index_bytes=%llu primitive_bytes=%llu material_bytes=%llu instance_bytes=%llu scene_sig=0x%llx material_sig=0x%llx instance_sig=0x%llx sky_sig=0x%llx sky_mode=%s sky_source=%s sky_key=0x%llx sky_brightness=%.3f sky_action=%s auto_exposure=%u exposure_texture=%u exposure=%.6f target_exposure=%.6f adapted_exposure=%.6f metered_log_lum=%.6f exposure_stats_valid=%u exposure_stats_frame=%llu final_valid=%u final_nonzero=unknown final_nonzero_ratio=unknown final_luma_mean=unknown final_luma_min=unknown final_luma_max=unknown final_nan=unknown final_inf=unknown scene_reason=ok route_reason=ok exposure_reason=%s present_reason=ok\n",
		snapshot.traceFrameIndex,
		snapshot.engineFrameIndex,
		snapshot.mapName,
		snapshot.levelName,
		snapshot.graphicsApiName,
		snapshot.worldActive ? 1u : 0u,
		snapshot.menuActive ? "yes" : "no",
		snapshot.gameplayFrame ? 1u : 0u,
		snapshot.portal ? 1u : 0u,
		snapshot.drawmode,
		snapshot.route.routeName,
		snapshot.debugMode,
		snapshot.route.passes,
		snapshot.route.presenterName,
		snapshot.route.ownerName,
		snapshot.route.denoiserRun ? 1u : 0u,
		snapshot.route.upscalerRun ? 1u : 0u,
		snapshot.route.exposureRun ? 1u : 0u,
		snapshot.presentKind,
		snapshot.renderWidth,
		snapshot.renderHeight,
		snapshot.outputWidth,
		snapshot.outputHeight,
		snapshot.swapchainFormat,
		snapshot.hdr ? 1u : 0u,
		snapshot.primitiveCount,
		snapshot.materialCount,
		snapshot.sceneInstanceCount,
		snapshot.staticInstanceCount,
		snapshot.dynamicInstanceCount,
		snapshot.persistentVoxelInstanceCount,
		snapshot.emissiveInstanceCount,
		snapshot.vertexCount,
		snapshot.indexCount,
		(unsigned long long)snapshot.vertexBytes,
		(unsigned long long)snapshot.indexBytes,
		(unsigned long long)snapshot.primitiveBytes,
		(unsigned long long)snapshot.materialBytes,
		(unsigned long long)snapshot.instanceBytes,
		(unsigned long long)snapshot.sceneSignature,
		(unsigned long long)snapshot.materialSignature,
		(unsigned long long)snapshot.instanceSignature,
		(unsigned long long)snapshot.skySignature,
		snapshot.skyMode,
		snapshot.skySource,
		(unsigned long long)snapshot.skyKey,
		snapshot.skyBrightness,
		snapshot.skyAction,
		snapshot.autoExposure ? 1u : 0u,
		snapshot.exposureTexture ? 1u : 0u,
		snapshot.exposure,
		snapshot.targetExposure,
		snapshot.adaptedExposure,
		snapshot.meteredLogLuminance,
		snapshot.exposureStatsValid ? 1u : 0u,
		(unsigned long long)snapshot.exposureStatsFrame,
		snapshot.finalValid ? 1u : 0u,
		snapshot.exposureReason);
}

void NRIRenderer::ResetSelfTestRouteSnapshot()
{
	mDiagnostics.ResetSelfTestRouteSnapshot();
}

void NRIRenderer::SetSelfTestRouteSnapshot(const char* routeName, const char* presenterName, const char* ownerName, const char* passes, bool denoiserRun, bool upscalerRun, bool exposureRun)
{
	mDiagnostics.SetSelfTestRouteSnapshot(routeName, presenterName, ownerName, passes, denoiserRun, upscalerRun, exposureRun);
}

void NRIRenderer::PrintSceneBufferStatus() const
{
	NRISceneBufferStatusSnapshot snapshot = {};
	const auto appendBuffer = [&snapshot](const NRIBufferResource& resource, const SceneBufferDebugStats& stats)
	{
		snapshot.buffers.push_back(BuildNRIBufferStatusSnapshot(resource, stats));
	};

	const NRIBufferResource& activeVertexBuffer = GetActiveVertexBuffer();
	const NRIBufferResource& activeIndexBuffer = GetActiveIndexBuffer();
	const NRIBufferResource& activePrimitiveBuffer = GetActivePrimitiveBuffer();
	const NRIBufferResource& activeMaterialBuffer = GetActiveMaterialBuffer();
	snapshot.totalUsedBytes = activeVertexBuffer.usedSize + activeIndexBuffer.usedSize + activePrimitiveBuffer.usedSize + activeMaterialBuffer.usedSize;
	snapshot.totalCapacityBytes = activeVertexBuffer.size + activeIndexBuffer.size + activePrimitiveBuffer.size + activeMaterialBuffer.size;
	snapshot.lastFrameUploadBytes =
		mVertexBufferStats.bytesUploadedLastFrame +
		mIndexBufferStats.bytesUploadedLastFrame +
		mPrimitiveBufferStats.bytesUploadedLastFrame +
		mMaterialBufferStats.bytesUploadedLastFrame;
	snapshot.lastFrameGrowEvents =
		mVertexBufferStats.growEventsLastFrame +
		mIndexBufferStats.growEventsLastFrame +
		mPrimitiveBufferStats.growEventsLastFrame +
		mMaterialBufferStats.growEventsLastFrame;
	snapshot.lastFrameOverwriteEvents =
		mVertexBufferStats.overwriteEventsLastFrame +
		mIndexBufferStats.overwriteEventsLastFrame +
		mPrimitiveBufferStats.overwriteEventsLastFrame +
		mMaterialBufferStats.overwriteEventsLastFrame;

	appendBuffer(activeVertexBuffer, mVertexBufferStats);
	appendBuffer(activeIndexBuffer, mIndexBufferStats);
	appendBuffer(activePrimitiveBuffer, mPrimitiveBufferStats);
	appendBuffer(activeMaterialBuffer, mMaterialBufferStats);
	appendBuffer(mPortalBuffer, mPortalBufferStats);
	appendBuffer(mRuntimeLightBuffer, mRuntimeLightBufferStats);
	appendBuffer(mRuntimeLightTileHeaderBuffer, mRuntimeLightTileHeaderBufferStats);
	appendBuffer(mRuntimeLightTileIndexBuffer, mRuntimeLightTileIndexBufferStats);
	appendBuffer(mEmissivePrimitiveHeaderBuffer, mEmissivePrimitiveHeaderBufferStats);
	appendBuffer(mEmissivePrimitiveBuffer, mEmissivePrimitiveBufferStats);
	appendBuffer(mEmissivePrimitiveCdfBuffer, mEmissivePrimitiveCdfBufferStats);
	appendBuffer(mEmissiveMaterialResponseBuffer, mEmissiveMaterialResponseBufferStats);
	appendBuffer(mSectorLightHeaderBuffer, mSectorLightHeaderBufferStats);
	appendBuffer(mSectorLightBuffer, mSectorLightBufferStats);
	PrintNRISceneBufferStatusSnapshot(snapshot);
}

void NRIRenderer::LogFallback(const char* reason)
{
	if (mHasLoggedFallback)
	{
		return;
	}

	Printf(TEXTCOLOR_ORANGE "NRI PT fallback: %s\n", reason != nullptr ? reason : "unknown reason");
	mHasLoggedFallback = true;
}

void NRIRenderer::LogBridgeStats(const nri_scene::SceneDebugStats& stats)
{
	if (!nri_ptscenestats)
	{
		mLastStats = stats;
		mHasLoggedStats = true;
		return;
	}

	if (!mHasLoggedStats || nri_scene::SceneDebugStatsDiffer(mLastStats, stats))
	{
		Printf("NRI PT scene: walls=%u flats=%u sprites=%u translucent=%u models=%u voxel_proxies=%u unsupported_models=%u voxel_cache=candidates:%u uncacheable:%u hits:%u misses:%u changes:%u split_stable:%u split_live:%u entries:%u surface_hits:%u stores:%u rebuilds:%u transform_rebakes:%u removes:%u not_captured:%u deferred:%u cached_prims:%u mirrors=%u skies=%u portal_views=%u portal_skips=%u approx_tris=%u materials=%u\n",
			stats.wallDrawItems,
			stats.flatDrawItems,
			stats.spriteDrawItems,
			stats.translucentDrawItems,
			stats.modelDrawItems,
			stats.voxelProxyDrawItems,
			stats.unsupportedModelDrawItems,
			stats.voxelStableCandidates,
			stats.voxelStableUncacheable,
			stats.voxelStableSignatureHits,
			stats.voxelStableSignatureMisses,
			stats.voxelStableSignatureChanges,
			stats.voxelStableSplitStable,
			stats.voxelStableSplitLive,
			stats.voxelCacheEntries,
			stats.voxelCacheSurfaceHits,
			stats.voxelCacheSurfaceStores,
			stats.voxelCacheSurfaceRebuilds,
			stats.voxelCacheTransformRebakes,
			stats.voxelCacheSurfaceRemoves,
			stats.voxelCacheNotCaptured,
			stats.voxelCacheDeferred,
			stats.voxelCachePrimitives,
			stats.mirrorSurfaces,
			stats.skySurfaces,
			stats.portalViews,
			stats.portalCapturesSkipped,
			stats.triangleEstimate,
			stats.materialRefs);
		mLastStats = stats;
		mHasLoggedStats = true;
	}
}

void NRIRenderer::PrintEmissiveSurfaceDump(float radius, uint32_t limit) const
{
	mSceneLights.PrintEmissiveSurfaceDump(mBoundEmissivePrimitiveRecords, mBoundEmissiveTotalPower, mCurrentCameraPos, radius, limit);
}

void NRIRenderer::PrintSceneLightDump(float radius, uint32_t limit) const
{
	mSceneLights.PrintSceneLightDump(mCurrentCameraPos, mMapWorld, mFrameIndex, radius, limit);
}

void NRIRenderer::PrintRuntimeSpaceLinkStatus() const
{
	Printf("NRI PT runtime links: active=%s geo_effect=%s query_attempted=%s query_rejected=%s candidate_sector=%d candidate_lotag=%d source_sector=%d reported_geo_count=%d view_roots=%u visible_sectors=%u providers=%u geo_providers=%u provider_groups=%u local_space_matches=%u visible_matches=%u links=%u translated_chunks=%u orphan_local_spaces=%u unresolved_runtime_portals=%u surfaces=%u tris=%u materials=%u\n",
		mRuntimeSpaceLinkLastFrame.active ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.geoEffectActive ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.queryAttempted ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.queryRejected ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.candidateSectorIndex,
		mRuntimeSpaceLinkLastFrame.candidateSectorLotag,
		mRuntimeSpaceLinkLastFrame.sourceSectorIndex,
		mRuntimeSpaceLinkLastFrame.reportedGeoCount,
		mRuntimeSpaceLinkLastFrame.viewRootSectorCount,
		mRuntimeSpaceLinkLastFrame.visibleSectorCount,
		mRuntimeSpaceLinkLastFrame.providerSectorCount,
		mRuntimeSpaceLinkLastFrame.geoProviderCount,
		mRuntimeSpaceLinkLastFrame.providerGroupCount,
		mRuntimeSpaceLinkLastFrame.localSpaceMatchedProviderCount,
		mRuntimeSpaceLinkLastFrame.visibleMatchedProviderCount,
		mRuntimeSpaceLinkLastFrame.linkCount,
		mRuntimeSpaceLinkLastFrame.translatedChunkCount,
		mRuntimeSpaceLinkLastFrame.orphanLocalSpaceCount,
		mRuntimeSpaceLinkLastFrame.unresolvedRuntimePortalCount,
		mRuntimeSpaceLinkLastFrame.surfaceCount,
		mRuntimeSpaceLinkLastFrame.triangleCount,
		mRuntimeSpaceLinkLastFrame.materialCount);
	Printf("NRI PT runtime link motion: prev_chunk_offsets=%u topology_changed=%s special_material_history=%s\n",
		(uint32_t)mRuntimeChunkTranslationHistory.size(),
		mRuntimeSpaceLinkLastFrame.topologyChanged ? "yes" : "no",
		"portal_mirror_raw_fallback");
}

void NRIRenderer::PrintMapWorldStatus() const
{
	if (!mMapWorld.valid)
	{
		Printf("NRI PT map world: no authoritative map world has been built yet.\n");
		return;
	}

	const auto& stats = mMapWorld.stats;
	Printf("NRI PT map world: level=%s build_serial=%llu chunks=%u local_spaces=%u sectors=%u sections=%u surfaces=%u walls=%u flats=%u portal_surfaces=%u portal_graph=%u portal_targets=%u wall_portals=%u sector_portals=%u mirror_portals=%u runtime_portals=%u skies=%u tris=%u\n",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)mMapWorld.buildSerial,
		stats.chunkCount,
		stats.localSpaceCount,
		stats.sectorCount,
		stats.sectionCount,
		stats.surfaceCount,
		stats.wallSurfaceCount,
		stats.flatSurfaceCount,
		stats.portalSurfaceCount,
		stats.portalCount,
		stats.portalTargetCount,
		stats.wallPortalCount,
		stats.sectorPortalCount,
		stats.mirrorPortalCount,
		stats.runtimePortalCount,
		stats.skySurfaceCount,
		stats.triangleCount);
}

void NRIRenderer::PrintDynamicSceneStatus() const
{
	const PersistentDynamicSurfaceStats persistentStats = mSceneLights.GatherPersistentDynamicEmissiveSurfaceStats();
	const PersistentDynamicEmissiveCache& persistentCache = mSceneLights.GetPersistentDynamicEmissiveCache();
	const SceneLightSystem::PersistentDynamicEmissiveHighWaterStats& persistentHighWater = mSceneLights.GetPersistentDynamicEmissiveHighWaterStats();
	const ActorSpriteDebugStats& actorSpriteDebugStats = mSceneLights.GetActorSpriteDebugStats();

	Printf("NRI PT dynamic scene: active=%s sprite_surfaces=%u tris=%u materials=%u models=%u unsupported_models=%u mirror_extended_surfaces=%u mirror_extended_tris=%u mirror_extended_materials=%u mirror_extended_models=%u mirror_extended_unsupported_models=%u mirror_player_surfaces=%u mirror_player_tris=%u mirror_player_materials=%u mirror_player_models=%u mirror_player_unsupported_models=%u mirror_distance=%.1f dynamic_as_builds=%u last_frame_as_build=%s active_tlas_instances=%u emissive_cache=%s cache_surfaces=%u cache_tris=%u cache_materials=%u\n",
		mUsedDynamicSceneLastFrame ? "yes" : "no",
		mDynamicSceneLastFrame.spriteSurfaceCount,
		mDynamicSceneLastFrame.primitiveCount,
		mDynamicSceneLastFrame.materialCount,
		mDynamicSceneLastFrame.modelCount,
		mDynamicSceneLastFrame.unsupportedModelCount,
		mDynamicSceneLastFrame.mirrorExtendedSurfaceCount,
		mDynamicSceneLastFrame.mirrorExtendedPrimitiveCount,
		mDynamicSceneLastFrame.mirrorExtendedMaterialCount,
		mDynamicSceneLastFrame.mirrorExtendedModelCount,
		mDynamicSceneLastFrame.mirrorExtendedUnsupportedModelCount,
		mDynamicSceneLastFrame.mirrorPlayerSurfaceCount,
		mDynamicSceneLastFrame.mirrorPlayerPrimitiveCount,
		mDynamicSceneLastFrame.mirrorPlayerMaterialCount,
		mDynamicSceneLastFrame.mirrorPlayerModelCount,
		mDynamicSceneLastFrame.mirrorPlayerUnsupportedModelCount,
		(double)nri_ptmirrordynamicdistance,
		mDynamicSceneLastFrame.asBuildCount,
		mBuiltDynamicSceneASLastFrame ? "yes" : "no",
		mActiveTlasInstanceCount,
		persistentCache.valid ? "yes" : "no",
		persistentCache.surfaceCount,
		persistentCache.primitiveCount,
		persistentCache.materialCount);
	Printf("NRI PT dynamic cache: actor_surfaces=%u non_actor_surfaces=%u walls=%u flats=%u sprites=%u actor_facing=%u actor_voxel=%u highwater=surfaces:%u tris:%u mats:%u actor:%u non_actor:%u walls:%u flats:%u sprites:%u actor_facing:%u actor_voxel:%u\n",
		persistentStats.actorSurfaceCount,
		persistentStats.nonActorSurfaceCount,
		persistentStats.wallSurfaceCount,
		persistentStats.flatSurfaceCount,
		persistentStats.spriteSurfaceCount,
		persistentStats.actorFacingSpriteCount,
		persistentStats.actorVoxelSpriteCount,
		persistentHighWater.surfaceCount,
		persistentHighWater.primitiveCount,
		persistentHighWater.materialCount,
		persistentHighWater.surfaceStats.actorSurfaceCount,
		persistentHighWater.surfaceStats.nonActorSurfaceCount,
		persistentHighWater.surfaceStats.wallSurfaceCount,
		persistentHighWater.surfaceStats.flatSurfaceCount,
		persistentHighWater.surfaceStats.spriteSurfaceCount,
		persistentHighWater.surfaceStats.actorFacingSpriteCount,
		persistentHighWater.surfaceStats.actorVoxelSpriteCount);
	Printf("NRI PT actor sprite diag: trace=%d cache_actor_facing=%u cache_actor_voxel=%u prune_checks=%u prune_matches=%u drop_missing_actor=%u drop_missing_actor_index=%u drop_null_live_texture=%u drop_texture_mismatch=%u drop_palette_mismatch=%u\n",
		(int)nri_ptactorspritetrace,
		persistentStats.actorFacingSpriteCount,
		persistentStats.actorVoxelSpriteCount,
		actorSpriteDebugStats.lastPruneChecks,
		actorSpriteDebugStats.lastPruneMatches,
		actorSpriteDebugStats.lastPruneDroppedMissingActor,
		actorSpriteDebugStats.lastPruneDroppedMissingActorIndex,
		actorSpriteDebugStats.lastPruneDroppedNullLiveTexture,
		actorSpriteDebugStats.lastPruneDroppedTextureMismatch,
		actorSpriteDebugStats.lastPruneDroppedPaletteMismatch);
	Printf("NRI PT scene texture overflow: textures=%u truncated=%u clamps=base:%u normal:%u metallic:%u roughness:%u emissive:%u builds=%llu warned=%s\n",
		mSceneTextures.OverflowStats().textureCountLastBuild,
		mSceneTextures.OverflowStats().truncatedTextureCountLastBuild,
		mSceneTextures.OverflowStats().baseTextureClampCountLastBuild,
		mSceneTextures.OverflowStats().normalTextureClampCountLastBuild,
		mSceneTextures.OverflowStats().metallicTextureClampCountLastBuild,
		mSceneTextures.OverflowStats().roughnessTextureClampCountLastBuild,
		mSceneTextures.OverflowStats().emissiveTextureClampCountLastBuild,
		(unsigned long long)mSceneTextures.OverflowStats().totalOverflowBuilds,
		mSceneTextures.OverflowStats().warningLogged ? "yes" : "no");
	Printf("NRI PT scene texture attribution: reason=%s requested=%u actor_materials=%u base=%u glow=%u normal=%u metallic=%u roughness=%u emissive=%u\n",
		mLastPerfShellTraceStats.sceneTextureReason.empty() ? "none" : mLastPerfShellTraceStats.sceneTextureReason.c_str(),
		mLastPerfShellTraceStats.sceneTextureRequestedCount,
		mLastPerfShellTraceStats.sceneTextureReferencedActorMaterialCount,
		mLastPerfShellTraceStats.sceneTextureReferencedBaseCount,
		mLastPerfShellTraceStats.sceneTextureReferencedGlowCount,
		mLastPerfShellTraceStats.sceneTextureReferencedNormalCount,
		mLastPerfShellTraceStats.sceneTextureReferencedMetallicCount,
		mLastPerfShellTraceStats.sceneTextureReferencedRoughnessCount,
		mLastPerfShellTraceStats.sceneTextureReferencedEmissiveCount);
	Printf("NRI PT actor overflow: materials=%u clamps=base:%u normal:%u metallic:%u roughness:%u emissive:%u omitted=%u\n",
		mLastPerfShellTraceStats.actorOverflowMaterialCount,
		mLastPerfShellTraceStats.actorOverflowBaseClampCount,
		mLastPerfShellTraceStats.actorOverflowNormalClampCount,
		mLastPerfShellTraceStats.actorOverflowMetallicClampCount,
		mLastPerfShellTraceStats.actorOverflowRoughnessClampCount,
		mLastPerfShellTraceStats.actorOverflowEmissiveClampCount,
		mLastPerfShellTraceStats.actorOverflowTraceOmittedCount);
	Printf("NRI PT scene texture cache: entries=%u highwater=%u misses=%u inserts=%u transitions=%u lookup_ms=%.3f realize_ms=%.3f descriptor_ms=%.3f transition_ms=%.3f\n",
		mSceneTextures.CacheStats().cacheEntriesLastBuild,
		mSceneTextures.CacheStats().cacheEntriesHighWater,
		mSceneTextures.CacheStats().lookupMissesLastBuild,
		mSceneTextures.CacheStats().insertCountLastBuild,
		mSceneTextures.CacheStats().transitionCountLastFrame,
		mSceneTextures.CacheStats().lookupMsLastBuild,
		mSceneTextures.CacheStats().realizeMsLastBuild,
		mSceneTextures.CacheStats().descriptorMsLastBuild,
		mSceneTextures.CacheStats().transitionMsLastFrame);
	Printf("NRI PT binding diag: label=%s materials=%u textures=%u actor_surfaces=%u actor_count=%u bridge_hash=0x%llx actor_hash=0x%llx scene_tex_updates=%llu scene_tex_hash=0x%llx scene_tex_reason=%s qframe=%u outstanding=%u scene_data_updates=%llu scene_data_hash=0x%llx scene_data_reason=%s qframe=%u outstanding=%u\n",
		mDescriptorCoherencyDebugStats.lastMaterialBuildLabel.empty() ? "none" : mDescriptorCoherencyDebugStats.lastMaterialBuildLabel.c_str(),
		mDescriptorCoherencyDebugStats.lastMaterialCount,
		mDescriptorCoherencyDebugStats.lastTextureCount,
		mDescriptorCoherencyDebugStats.lastActorSpriteSurfaceCount,
		mDescriptorCoherencyDebugStats.lastActorSpriteActorCount,
		(unsigned long long)mDescriptorCoherencyDebugStats.lastMaterialBridgeHash,
		(unsigned long long)mDescriptorCoherencyDebugStats.lastActorSpriteMaterialHash,
		(unsigned long long)mDescriptorCoherencyDebugStats.sceneTextureSetUpdates,
		(unsigned long long)mDescriptorCoherencyDebugStats.lastSceneTextureDescriptorHash,
		mDescriptorCoherencyDebugStats.lastSceneTextureReason.empty() ? "none" : mDescriptorCoherencyDebugStats.lastSceneTextureReason.c_str(),
		mDescriptorCoherencyDebugStats.lastSceneTextureQueuedFrameIndex,
		mDescriptorCoherencyDebugStats.lastSceneTextureOutstandingQueuedFrames,
		(unsigned long long)mDescriptorCoherencyDebugStats.sceneDataSetUpdates,
		(unsigned long long)mDescriptorCoherencyDebugStats.lastSceneDataDescriptorHash,
		mDescriptorCoherencyDebugStats.lastSceneDataReason.empty() ? "none" : mDescriptorCoherencyDebugStats.lastSceneDataReason.c_str(),
		mDescriptorCoherencyDebugStats.lastSceneDataQueuedFrameIndex,
		mDescriptorCoherencyDebugStats.lastSceneDataOutstandingQueuedFrames);
	Printf("NRI PT material builds: calls=%u override_builds=%u override_ms=%.3f material_ms=%.3f\n",
		mLastPerfShellTraceStats.materialBuildCalls,
		mLastPerfShellTraceStats.actorOverrideMapBuildCalls,
		mLastPerfShellTraceStats.actorOverrideMapBuildMs,
		mLastPerfShellTraceStats.materialBuildMs);
	Printf("NRI PT mutation detail: structural_ms=%.3f material_refresh_ms=%.3f structural=%u material_refresh=%u refresh_delta=%u refresh_delta_mask=0x%x refresh_hwcanvas=%u refresh_animated=%u struct_delta=%u struct_delta_mask=0x%x struct_view=%u struct_static_anim_flip=%u struct_excl_static_flip=%u struct_force_topology=%u struct_invalid=%u hwcanvas_chunks=%u\n",
		mLastPerfShellTraceStats.runtimeMutationStructuralRebuildMs,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshMs,
		mLastPerfShellTraceStats.runtimeMutationStructuralRebuildChunks,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshChunks,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReplacementDeltaChunks,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReasonMaskOr,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshHardwareCanvasChunks,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshAnimatedChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaReasonMaskOr,
		mLastPerfShellTraceStats.runtimeMutationStructuralReplacementViewChangedChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralStaticAnimatedModeFlipChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralExcludeStaticFlipChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralForcedTopologyChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralInvalidChunks,
		mLastPerfShellTraceStats.runtimeMutationHardwareCanvasChunkCount);
	for (size_t index = 0; index < NRIRenderer::MaterialBuildTraceSlotCount; ++index)
	{
		const auto& entry = mLastPerfShellTraceStats.materialBuildByLabel[index];
		if (entry.calls == 0 && entry.overrideBuildCalls == 0)
		{
			continue;
		}

		Printf("NRI PT material detail: label=%s calls=%u override_builds=%u override_ms=%.3f material_ms=%.3f\n",
			GetMaterialBuildTraceSlotName((MaterialBuildTraceSlot)index),
			entry.calls,
			entry.overrideBuildCalls,
			entry.overrideBuildMs,
			entry.materialBuildMs);
		Printf("NRI PT material textures: label=%s materials=%u actor_materials=%u textures=%u base=%u glow=%u normal=%u metallic=%u roughness=%u emissive=%u\n",
			GetMaterialBuildTraceSlotName((MaterialBuildTraceSlot)index),
			entry.materialCount,
			entry.actorMaterialCount,
			entry.textureCount,
			entry.baseTextureCount,
			entry.glowTextureCount,
			entry.normalTextureCount,
			entry.metallicTextureCount,
			entry.roughnessTextureCount,
			entry.emissiveTextureCount);
	}
}

NRIBufferStatusSnapshot BuildNRIBufferStatusSnapshot(const NRIBufferResource& resource, const SceneBufferDebugStats& stats)
{
	NRIBufferStatusSnapshot snapshot = {};
	snapshot.label = stats.label;
	snapshot.usedBytes = resource.usedSize;
	snapshot.capacityBytes = resource.size;
	snapshot.usedItems = resource.stride != 0 ? resource.usedSize / resource.stride : 0;
	snapshot.capacityItems = resource.stride != 0 ? resource.size / resource.stride : 0;
	snapshot.uploadCount = stats.uploadCount;
	snapshot.growthCount = stats.growthCount;
	snapshot.overwriteCount = stats.overwriteCount;
	snapshot.bytesUploadedLastFrame = stats.bytesUploadedLastFrame;
	snapshot.growEventsLastFrame = stats.growEventsLastFrame;
	snapshot.overwriteEventsLastFrame = stats.overwriteEventsLastFrame;
	snapshot.peakUsedBytes = stats.peakUsedBytes;
	return snapshot;
}

void PrintNRISceneBufferStatusSnapshot(const NRISceneBufferStatusSnapshot& snapshot)
{
	Printf("NRI PT scene buffers: used=%llu capacity=%llu last_frame_upload=%llu last_frame_grows=%u last_frame_overwrites=%u\n",
		(unsigned long long)snapshot.totalUsedBytes,
		(unsigned long long)snapshot.totalCapacityBytes,
		(unsigned long long)snapshot.lastFrameUploadBytes,
		snapshot.lastFrameGrowEvents,
		snapshot.lastFrameOverwriteEvents);

	for (const NRIBufferStatusSnapshot& buffer : snapshot.buffers)
	{
		Printf("NRI PT %s buffer: used=%llu/%llu bytes items=%llu/%llu uploads=%u grows=%u overwrites=%u last_frame_bytes=%llu last_frame_grows=%u last_frame_overwrites=%u peak_used=%llu\n",
			buffer.label,
			(unsigned long long)buffer.usedBytes,
			(unsigned long long)buffer.capacityBytes,
			(unsigned long long)buffer.usedItems,
			(unsigned long long)buffer.capacityItems,
			buffer.uploadCount,
			buffer.growthCount,
			buffer.overwriteCount,
			(unsigned long long)buffer.bytesUploadedLastFrame,
			buffer.growEventsLastFrame,
			buffer.overwriteEventsLastFrame,
			(unsigned long long)buffer.peakUsedBytes);
	}
}

void PrintNRITemporalStatusSnapshot(const NRITemporalStatusSnapshot& snapshot)
{
	Printf("NRI PT temporal: debug=%d requested_main=%s resolved_main=%s requested_post=%s resolved_post=%s taa=%s gui_capture=%s last_debug=%d last_main=%s last_post=%s reset=%s prev_camera=%s history_in=%s[%ux%u a=%u l=%u s=0x%x] history_out=%s[%ux%u a=%u l=%u s=0x%x] present=%s upscaled=%s use_upscaled=%s\n",
		snapshot.debugMode,
		snapshot.requestedMainUpscaler,
		snapshot.resolvedMainUpscaler,
		snapshot.requestedPostSharpen,
		snapshot.resolvedPostSharpen,
		snapshot.taa ? "on" : "off",
		snapshot.guiCapture ? "yes" : "no",
		snapshot.lastDebugMode,
		snapshot.lastMainUpscaler,
		snapshot.lastPostSharpen,
		snapshot.resetHistory ? "yes" : "no",
		snapshot.previousCamera ? "yes" : "no",
		snapshot.historyInput.slotName,
		snapshot.historyInput.width,
		snapshot.historyInput.height,
		snapshot.historyInput.access,
		snapshot.historyInput.layout,
		snapshot.historyInput.stages,
		snapshot.historyOutput.slotName,
		snapshot.historyOutput.width,
		snapshot.historyOutput.height,
		snapshot.historyOutput.access,
		snapshot.historyOutput.layout,
		snapshot.historyOutput.stages,
		snapshot.presentSlotName,
		snapshot.upscaledSlotName,
		snapshot.useUpscaled ? "yes" : "no");
	Printf("NRI PT beauty path: nrd_and_composition -> pre_exposed_hdr_temporal -> final_display_mapping inspect_scene=15 inspect_pre_exposed=45 inspect_post_taa=13 inspect_post_upscale=14\n");
	Printf("NRI PT temporal domain: history=%s present=%s temporal_exposure=%.3f present_exposure=%.3f exposure_stops=%.3f reset_threshold_stops=%.3f auto_exposure=%s exposure_texture=%s taa_apply=%s\n",
		snapshot.historyDomain,
		snapshot.presentDomain,
		snapshot.temporalExposure,
		snapshot.presentExposure,
		snapshot.exposureStops,
		snapshot.resetThresholdStops,
		snapshot.autoExposure ? "yes" : "no",
		snapshot.exposureTexture ? "yes" : "no",
		snapshot.taaApply ? "yes" : "no");
}

void PrintNRITemporalTraceSnapshot(const NRITemporalTraceSnapshot& snapshot)
{
	Printf("NRI PT temporal trace: stage=%s frame=%u debug=%d resolved_main=%s resolved_post=%s run_app_taa=%s gui_capture=%s primary_domain=%s secondary_domain=%s temporal_exposure=%.3f primary_present_exposure=%.3f secondary_present_exposure=%.3f reset=%s reset_reason=%s prev_camera=%s history_in=%s[%ux%u a=%u l=%u s=0x%x] history_out=%s[%ux%u a=%u l=%u s=0x%x] primary=%s[%ux%u a=%u l=%u s=0x%x] secondary=%s[%ux%u a=%u l=%u s=0x%x] use_upscaled=%s\n",
		snapshot.stage,
		snapshot.frameIndex,
		snapshot.debugMode,
		snapshot.resolvedMainUpscaler,
		snapshot.resolvedPostSharpen,
		snapshot.runAppTaa ? "yes" : "no",
		snapshot.guiCapture ? "yes" : "no",
		snapshot.primaryDomain,
		snapshot.secondaryDomain,
		snapshot.temporalExposure,
		snapshot.primaryPresentExposure,
		snapshot.secondaryPresentExposure,
		snapshot.resetHistory ? "yes" : "no",
		snapshot.resetReason,
		snapshot.previousCamera ? "yes" : "no",
		snapshot.historyInput.slotName,
		snapshot.historyInput.width,
		snapshot.historyInput.height,
		snapshot.historyInput.access,
		snapshot.historyInput.layout,
		snapshot.historyInput.stages,
		snapshot.historyOutput.slotName,
		snapshot.historyOutput.width,
		snapshot.historyOutput.height,
		snapshot.historyOutput.access,
		snapshot.historyOutput.layout,
		snapshot.historyOutput.stages,
		snapshot.primary.slotName,
		snapshot.primary.width,
		snapshot.primary.height,
		snapshot.primary.access,
		snapshot.primary.layout,
		snapshot.primary.stages,
		snapshot.secondary.slotName,
		snapshot.secondary.width,
		snapshot.secondary.height,
		snapshot.secondary.access,
		snapshot.secondary.layout,
		snapshot.secondary.stages,
		snapshot.useUpscaled ? "yes" : "no");
}

void PrintNRIPortalTraversalStatusSnapshot(const NRIPortalTraversalStatusSnapshot& snapshot)
{
	if (!snapshot.available)
	{
		Printf("NRI PT portal traversal: no authoritative portal graph is available.\n");
		return;
	}

	Printf("NRI PT portal traversal: depth=%u reflective=%u transfer=%u runtime_bound=%u hittable_surfaces=%u plane_portals_pending=%u\n",
		snapshot.depth,
		snapshot.reflective,
		snapshot.transfer,
		snapshot.runtimeBound,
		snapshot.hittableSurfaces,
		snapshot.pendingPlanePortals);
}

void PrintNRIResidentMapChunkRegistryStatusSnapshot(const NRIResidentMapChunkRegistryStatusSnapshot& snapshot)
{
	if (!snapshot.available)
	{
		Printf("NRI PT resident chunk registry: unavailable.\n");
		return;
	}

	Printf("NRI PT resident chunk registry: build_serial=%llu chunks=%u active=%u mapped=%u acceleration_resident=%u animated_candidates=%u animated_refresh_suppressed=%u\n",
		(unsigned long long)snapshot.buildSerial,
		snapshot.chunkCount,
		snapshot.activeChunkCount,
		snapshot.mappedChunkCount,
		snapshot.accelerationResidentChunkCount,
		snapshot.animatedCandidateChunkCount,
		snapshot.animatedRefreshSuppressedChunkCount);
	Printf("NRI PT map chunk bounds: chunks=%u valid=%u invalid=%u near_distance=%.1f visible=%u invisible_near=%u invisible_far=%u invisible_unknown=%u sample_chunk=%u center=(%.1f,%.1f,%.1f) radius=%.1f distance=%.1f tier=%s\n",
		snapshot.mapWorldChunkCount,
		snapshot.boundsValidCount,
		snapshot.boundsInvalidCount,
		(double)snapshot.nearDistance,
		snapshot.visibleCount,
		snapshot.invisibleNearCount,
		snapshot.invisibleFarCount,
		snapshot.invisibleUnknownCount,
		snapshot.sampleChunkIndex,
		(double)snapshot.sampleCenter[0],
		(double)snapshot.sampleCenter[1],
		(double)snapshot.sampleCenter[2],
		(double)snapshot.sampleRadius,
		(double)snapshot.sampleDistance,
		snapshot.sampleTier);
}
