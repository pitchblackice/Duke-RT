#include "perf_capture.h"

#include "c_cvars.h"
#include "printf.h"

#include <algorithm>
#include <array>
#include <limits>

CUSTOM_CVAR(Int, perf_compactframes, 0, 0)
{
	if (self < 0) self = 0;
	else if (self > 2048) self = 2048;
}

namespace
{
	constexpr uint32_t MaxRecords = 4096;

	struct Record
	{
		PerfCompactOuterFrame outer;
		PerfCompactNriStats nri;
		PerfCompactBoundaryStats boundary;
		PerfCompactGpuTiming gpu;
		uint32_t expectedGpuSegments = 0;
		uint32_t resolvedGpuSegments = 0;
		uint32_t eligibleIndex = std::numeric_limits<uint32_t>::max();
		bool eligible = false;
	};

	enum class CaptureState : uint8_t { Idle, Active, Draining, Aborted };
	struct Capture
	{
		std::array<Record, MaxRecords> records = {};
		CaptureState state = CaptureState::Idle;
		uint64_t epoch = 0;
		uint32_t requested = 0, observed = 0, eligible = 0, pendingGpu = 0;
		PerfCompactCaptureToken current;
		const char* abortReason = "none";
	};
	Capture gCapture;

	bool TokenMatches(const PerfCompactCaptureToken& token)
	{
		return token && token.epoch == gCapture.epoch && token.recordIndex < gCapture.observed;
	}

	void ResetCapture()
	{
		const uint64_t epoch = gCapture.epoch;
		for (Record& record : gCapture.records) record = {};
		gCapture.state = CaptureState::Idle;
		gCapture.requested = 0;
		gCapture.observed = 0;
		gCapture.eligible = 0;
		gCapture.pendingGpu = 0;
		gCapture.current = {};
		gCapture.abortReason = "none";
		gCapture.epoch = epoch;
	}

	void FlushRecordOwners(const Record& record)
	{
		const auto& outer = record.outer;
		const auto& nri = record.nri;
		const auto& boundary = record.boundary;
		Printf("PERF render trace NRI: frame=%llu nri_frame=%llu total=%.3f init=%.3f res=%.3f state=%.3f capture=%.3f geo=%.3f mats=%.3f textures=%.3f buffers=%.3f as=%.3f graph=%.3f wait=%.3f wait_present=%.3f acquire=%.3f submit=%.3f present=%.3f trace=%.3f denoise=%.3f compose=%.3f upscale=%.3f final=%.3f compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)outer.traceFrame, (unsigned long long)nri.frame,
			outer.nriTotalMs, outer.nriInitializeMs,
			outer.nriFrameResourcesMs, outer.nriUpdateStateMs, outer.nriSceneCaptureMs,
			outer.nriGeometryBuildMs, outer.nriMaterialBuildMs, outer.nriSceneTexturesMs,
			outer.nriSceneBuffersMs, outer.nriAccelerationMs, outer.nriFrameGraphMs,
			boundary.waitMs, boundary.waitPresentMs, boundary.acquireMs, boundary.submitMs,
			boundary.presentMs, outer.nriTraceMs, outer.nriDenoiseMs, outer.nriComposeMs,
			outer.nriUpscaleMs, outer.nriFinalMs, (unsigned long long)gCapture.epoch,
			record.eligibleIndex);
		Printf("PERF pt shell trace NRI: frame=%llu nri_frame=%llu total=%.3f init=%.3f map=%.3f state=%.3f select=%.3f lights=%.3f frame_graph=%.3f post_diag=%.3f unattributed=%.3f active_prims=%u dynamic_prims=%u materials=%u instances=%u compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)outer.traceFrame, (unsigned long long)nri.frame,
			nri.totalMs, nri.initMs, nri.mapMs, nri.stateMs,
			nri.selectMs, nri.lightsMs, nri.frameGraphMs, nri.postDiagnosticsMs,
			nri.unattributedMs, nri.activePrimitives, nri.dynamicPrimitives,
			nri.activeMaterials, nri.sceneInstances, (unsigned long long)gCapture.epoch,
			record.eligibleIndex);
		Printf("PERF pt scene select accounting NRI: frame=%llu nri_frame=%llu total=%.3f mutation=%.3f mutation_discovery=%.3f mutation_budget=%.3f mutation_analyze=%.3f mutation_structural_ms=%.3f mutation_material_ms=%.3f mutation_resident_ms=%.3f mutation_commit=%.3f dynamic_capture=%.3f persistent_batch=%.3f material_bridge=%.3f textures=%.3f buffer_upload=%.3f persistent_voxel_as=%.3f dynamic_as=%.3f world_tlas=%.3f scene_data=%.3f state_commit=%.3f unaccounted=%.3f structural=%u material=%u resident=%u candidates=%u analyzed=%u sweep=%u candidate_active=%u candidate_visible=%u candidate_startup_visible=%u candidate_unresolved=%u candidate_static_animated=%u candidate_sector_dirty=%u candidate_section_dirty=%u candidate_dragged=%u candidate_signature_watch=%u candidate_deferred_material=%u candidate_deferred_structural=%u structural_chunk0=%u structural_reason0=0x%x structural_trigger0=0x%x structural_chunk1=%u structural_reason1=0x%x structural_trigger1=0x%x structural_chunk2=%u structural_reason2=0x%x structural_trigger2=0x%x structural_chunk3=%u structural_reason3=0x%x structural_trigger3=0x%x upload_bytes=%llu compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)outer.traceFrame, (unsigned long long)nri.frame,
			nri.selectMs, nri.mutationMs, nri.mutationDiscoveryMs, nri.mutationBudgetMs,
			nri.mutationAnalyzeMs, nri.mutationStructuralMs, nri.mutationMaterialMs,
			nri.mutationResidentMs, nri.mutationCommitMs, nri.dynamicCaptureMs,
			nri.persistentBatchMs, nri.materialBridgeMs, nri.texturesMs, nri.bufferUploadMs,
			nri.persistentVoxelAsMs, nri.dynamicAsMs, nri.worldTlasMs, nri.sceneDataMs,
			nri.stateCommitMs, nri.unattributedMs, nri.mutationStructural,
			nri.mutationMaterial, nri.mutationResident, nri.mutationCandidates,
			nri.mutationAnalyzed, nri.mutationSweep, nri.mutationCandidateActive,
			nri.mutationCandidateVisible, nri.mutationCandidateStartupVisible,
			nri.mutationCandidateUnresolved, nri.mutationCandidateStaticAnimated,
			nri.mutationCandidateSectorDirty, nri.mutationCandidateSectionDirty,
			nri.mutationCandidateDragged, nri.mutationCandidateSignatureWatch,
			nri.mutationCandidateDeferredMaterial, nri.mutationCandidateDeferredStructural,
			nri.mutationStructuralChunk[0], nri.mutationStructuralReason[0], nri.mutationStructuralTrigger[0],
			nri.mutationStructuralChunk[1], nri.mutationStructuralReason[1], nri.mutationStructuralTrigger[1],
			nri.mutationStructuralChunk[2], nri.mutationStructuralReason[2], nri.mutationStructuralTrigger[2],
			nri.mutationStructuralChunk[3], nri.mutationStructuralReason[3], nri.mutationStructuralTrigger[3],
			(unsigned long long)nri.sceneUploadBytes, (unsigned long long)gCapture.epoch,
			record.eligibleIndex);
		Printf("PERF pt voxel pressure compact NRI: frame=%llu nri_frame=%llu reason=0x%x flags=0x%x admission_rows=%u resource_rows=%u compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)outer.traceFrame, (unsigned long long)nri.frame,
			nri.voxelPressureReason, nri.voxelPressureFlags, nri.voxelPressureEntries,
			nri.voxelPressureResources,
			(unsigned long long)gCapture.epoch, record.eligibleIndex);
		Printf("PERF pt voxel batch compact NRI: frame=%llu nri_frame=%llu total=%.3f pump=%.3f cache_entries=%.3f sort=%.3f instance_sync=%.3f existing_actors=%.3f actor_loop=%.3f material_variant=%.3f mesh_admission=%.3f material_bridge=%.3f state=%.3f compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)outer.traceFrame, (unsigned long long)nri.frame,
			nri.persistentBatchMs, nri.voxelAdmissionPumpMs, nri.voxelBatchCacheEntryMs,
			nri.voxelBatchSortMs, nri.voxelBatchInstanceSyncMs, nri.voxelBatchExistingActorMapMs,
			nri.voxelBatchActorLoopMs, nri.voxelBatchMaterialVariantMs,
			nri.voxelBatchMeshAdmissionMs, nri.voxelBatchMaterialBridgeMs, nri.voxelBatchStateMs,
			(unsigned long long)gCapture.epoch, record.eligibleIndex);
		Printf("PERF pt resource waits NRI: frame=%llu nri_frame=%llu total=%u/%.3f compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)outer.traceFrame, (unsigned long long)nri.frame,
			nri.resourceWaitCalls, nri.resourceWaitMs,
			(unsigned long long)gCapture.epoch, record.eligibleIndex);
		Printf("PERF pt trace workload NRI: frame=%llu nri_frame=%llu renderer_frame=%llu schema=2 settings_key=%llu workload_key=%llu render_w=%u render_h=%u output_w=%u output_h=%u dispatch_x=%u dispatch_y=%u dispatch_z=%u light_bounces=%u mirror_bounces=%u portal_depth=%u emissive_samples=%u emissive_requested=%u emissive_budget=%u indirect_requested=%u indirect_effective=%u indirect_active=%u hit_recon=%u runtime_lights=%u light_tiles_x=%u light_tiles_y=%u light_tile_size=%u light_tile_indices=%u light_tile_max=%u emissive_prims=%u emissive_power=%.3f flags=%u debug=%u bootstrap=%u upscaler=%u upscaler_mode=%u denoiser=%u direct_scene=%u directional=%u directional_shadow=%u split_shadow=%u fast_emissive_shadow=%u visible_chunk_gate=%u compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)outer.traceFrame, (unsigned long long)nri.frame,
			(unsigned long long)nri.traceRendererFrame,
			(unsigned long long)nri.traceSettingsKey,
			(unsigned long long)nri.traceWorkloadKey,
			nri.traceRenderWidth, nri.traceRenderHeight, nri.traceOutputWidth, nri.traceOutputHeight,
			nri.traceDispatchX, nri.traceDispatchY, nri.traceDispatchZ,
			nri.traceLightBounces, nri.traceMirrorBounces, nri.tracePortalDepth, nri.traceEmissiveSamples,
			nri.traceEmissiveRequestedSamples, nri.traceEmissivePrimaryBudget,
			nri.traceIndirectSamplingRequestedMode,
			nri.traceIndirectSamplingEffectiveMode, nri.traceIndirectSamplingActiveMode,
			nri.traceHitDistanceReconstructionMode,
			nri.traceRuntimeLights, nri.traceRuntimeLightTilesX, nri.traceRuntimeLightTilesY,
			nri.traceRuntimeLightTileSize, nri.traceRuntimeLightTileIndices, nri.traceRuntimeLightMaxOccupancy,
			nri.traceEmissivePrimitiveCount, nri.traceEmissiveTotalPower,
			nri.traceFlags, nri.traceDebugMode, nri.traceBootstrapMode,
			nri.traceUpscalerKind, nri.traceUpscalerMode, nri.traceDenoiserMode,
			nri.traceDirectScene, nri.traceDirectional, nri.traceDirectionalShadow,
			nri.traceSplitShadow, nri.traceFastEmissiveShadow, nri.traceVisibleChunkGate,
			(unsigned long long)gCapture.epoch, record.eligibleIndex);
		Printf("PERF pt gpu timing NRI: frame=%llu nri_frame=%llu segment=%.3f scene=%.3f trace=%.3f trace_dispatch=%.3f denoise=%.3f compose=%.3f upscale=%.3f final=%.3f segments=%u invalid=%u dropped=%u resolved=%u expected=%u compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)outer.traceFrame, (unsigned long long)nri.frame,
			record.gpu.segmentMs, record.gpu.sceneMs,
			record.gpu.traceMs, record.gpu.traceDispatchMs, record.gpu.denoiseMs, record.gpu.compositionMs,
			record.gpu.upscaleMs, record.gpu.finalMs, record.gpu.segmentCount,
			record.gpu.invalidPairs, record.gpu.droppedScopes, record.resolvedGpuSegments,
			record.expectedGpuSegments, (unsigned long long)gCapture.epoch,
			record.eligibleIndex);
	}

	void FlushLoopRecord(const Record& record)
	{
		const auto& f = record.outer;
		Printf("PERF loop trace: frame=%llu presentation_gen=%llu simulation_gen=%llu engine_gen=%llu state=level gametic=%d startframe_ms=%.3f try_ms=%.3f try_traced_ms=%.3f display_ms=%.3f display_begin_ms=%.3f display_render_ms=%.3f display_overlay_ms=%.3f display_update_ms=%.3f starttic_ms=%.3f music_ms=%.3f frame_ms=%.3f do_wait=%d realtics=%d avail=%d counts=%d ticks=%d wait_loops=%d zero_return=%d wait_return=%d paused_return=%d display_skip=%d level_rendered=1 compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)f.traceFrame, (unsigned long long)f.presentationGeneration,
			(unsigned long long)f.simulationGeneration, (unsigned long long)f.engineGeneration,
			f.gametic, f.startFrameMs, f.tryMs, f.tryTracedMs, f.displayMs,
			f.displayBeginMs, f.displayRenderMs, f.displayOverlayMs, f.displayUpdateMs,
			f.startTicMs, f.musicMs, f.frameMs, f.doWait ? 1 : 0, f.realtics,
			f.availabletics, f.counts, f.ticks, f.waitLoops, f.zeroReturn ? 1 : 0,
			f.waitReturn ? 1 : 0, f.pausedReturn ? 1 : 0, f.displaySkipped ? 1 : 0,
			(unsigned long long)gCapture.epoch, record.eligibleIndex);
	}
}

void PerfCompactCaptureFlushIfReady()
{
	if (gCapture.state != CaptureState::Draining && gCapture.state != CaptureState::Aborted) return;
	if (gCapture.state == CaptureState::Draining && gCapture.pendingGpu != 0) return;
	if (gCapture.state == CaptureState::Draining)
	{
		for (uint32_t i = 0; i < gCapture.observed; ++i)
			if (gCapture.records[i].eligible) FlushRecordOwners(gCapture.records[i]);
		for (uint32_t i = 0; i < gCapture.observed; ++i)
			if (gCapture.records[i].eligible) FlushLoopRecord(gCapture.records[i]);
	}
	Printf("PERF compact capture complete: epoch=%llu status=%s requested=%u eligible=%u observed=%u pending_gpu=%u dropped=0 reason=%s\n",
		(unsigned long long)gCapture.epoch,
		gCapture.state == CaptureState::Draining ? "complete" : "aborted",
		gCapture.requested, gCapture.eligible, gCapture.observed, gCapture.pendingGpu,
		gCapture.abortReason);
	ResetCapture();
}

void PerfCompactCaptureBeginOuterFrame(uint64_t presentationGeneration)
{
	PerfCompactCaptureFlushIfReady();
	if (gCapture.state == CaptureState::Idle && (int)perf_compactframes > 0)
	{
		const uint32_t requested = (uint32_t)(int)perf_compactframes;
		perf_compactframes = 0;
		ResetCapture();
		if (++gCapture.epoch == 0) gCapture.epoch = 1;
		gCapture.requested = requested;
		gCapture.state = CaptureState::Active;
	}
	if (gCapture.state != CaptureState::Active)
	{
		gCapture.current = {};
		return;
	}
	if (gCapture.observed >= MaxRecords)
	{
		PerfCompactCaptureAbort("capacity-exhausted");
		return;
	}
	const uint32_t index = gCapture.observed++;
	gCapture.records[index] = {};
	gCapture.current = { gCapture.epoch, presentationGeneration, index };
}

bool PerfCompactCaptureTimingActive() { return gCapture.state == CaptureState::Active && (bool)gCapture.current; }
PerfCompactCaptureToken PerfCompactCaptureGetCurrentToken() { return PerfCompactCaptureTimingActive() ? gCapture.current : PerfCompactCaptureToken{}; }

void PerfCompactCaptureNoteNri(const PerfCompactCaptureToken& token, const PerfCompactNriStats& stats)
{
	if (TokenMatches(token)) gCapture.records[token.recordIndex].nri = stats;
}

void PerfCompactCaptureNoteBoundary(const PerfCompactCaptureToken& token, const PerfCompactBoundaryStats& stats)
{
	if (TokenMatches(token)) gCapture.records[token.recordIndex].boundary = stats;
}

void PerfCompactCaptureExpectGpuSegment(const PerfCompactCaptureToken& token)
{
	if (!TokenMatches(token)) return;
	gCapture.records[token.recordIndex].expectedGpuSegments++;
	gCapture.pendingGpu++;
}

void PerfCompactCaptureResolveGpuSegment(const PerfCompactCaptureToken& token, const PerfCompactGpuTiming& timing)
{
	if (!TokenMatches(token)) return;
	Record& r = gCapture.records[token.recordIndex];
	if (r.resolvedGpuSegments >= r.expectedGpuSegments) return;
	r.resolvedGpuSegments++;
	r.gpu.segmentMs += timing.segmentMs; r.gpu.sceneMs += timing.sceneMs;
	r.gpu.traceMs += timing.traceMs; r.gpu.traceDispatchMs += timing.traceDispatchMs;
	r.gpu.denoiseMs += timing.denoiseMs;
	r.gpu.compositionMs += timing.compositionMs; r.gpu.upscaleMs += timing.upscaleMs;
	r.gpu.finalMs += timing.finalMs; r.gpu.segmentCount += timing.segmentCount;
	r.gpu.invalidPairs += timing.invalidPairs; r.gpu.droppedScopes += timing.droppedScopes;
	if (gCapture.pendingGpu > 0) gCapture.pendingGpu--;
}

void PerfCompactCaptureEndOuterFrame(const PerfCompactOuterFrame& frame)
{
	const PerfCompactCaptureToken token = gCapture.current;
	if (!TokenMatches(token)) return;
	Record& r = gCapture.records[token.recordIndex];
	r.outer = frame;
	r.eligible = frame.stateIsLevel && frame.levelRendered && frame.nriActive && r.nri.valid &&
		r.nri.rendered && r.boundary.valid && r.boundary.pathTraced && r.boundary.presentOk &&
		r.nri.frame == r.boundary.frame;
	if (r.eligible) r.eligibleIndex = gCapture.eligible++;
	gCapture.current = {};
	if (gCapture.eligible >= gCapture.requested) gCapture.state = CaptureState::Draining;
}

void PerfCompactCaptureAbort(const char* reason)
{
	if (gCapture.state == CaptureState::Idle) return;
	gCapture.current = {};
	gCapture.abortReason = reason != nullptr ? reason : "unknown";
	gCapture.state = CaptureState::Aborted;
}
