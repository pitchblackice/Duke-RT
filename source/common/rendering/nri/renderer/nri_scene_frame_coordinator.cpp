#include "nri_renderer.h"
#include "nri_cvars.h"

#include "../framegen/nri_framegen.h"
#include "nri_acceleration.h"
#include "nri_diagnostic_names.h"
#include "nri_frame_resources.h"
#include "nri_material_policy.h"
#include "nri_pass_dispatch.h"
#include "nri_persistent_voxel_services.h"
#include "nri_render_geometry_helpers.h"
#include "nri_renderer_settings.h"
#include "nri_scene_frame_builder.h"
#include "nri_scene_frame_diagnostics.h"
#include "nri_scene_frame_coordinator_types.h"
#include "nri_scene_frame_mirrors.h"
#include "nri_scene_frame_overlay.h"
#include "nri_scene_frame_selection.h"
#include "nri_scene_frame_state.h"
#include "nri_scene_upload.h"
#include "nri_static_scene_geometry.h"
#include "nri_surface_light_overlay.h"
#include "nri_runtime_mutation_shared.h"
#include "nri_sky_environment.h"
#include "../scene/nri_hash.h"
#include "../scene/nri_scene_stats.h"
#include "../system/nri_renderdevice.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "coreactor.h"
#include "coreplayer.h"
#include "hw_voxels.h"
#include "gamecontrol.h"
#include "gamestate.h"
#include "hw_sections.h"
#include "lightoverlay.h"
#include "mapinfo.h"
#include "printf.h"
#include "gamestruct.h"
#include "hw_portal.h"
#include "texinfo.h"
#include "texturemanager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>


namespace
{
	static uint32_t GetBootstrapMode()
	{
		return (uint32_t)std::max(0, std::min((int)nri_ptbootstrapmode, 13));
	}


	static bool TryComputeCapturedSurfaceNormal(const nri_scene::SurfaceRef& surface, float outNormal[3])
	{
		if (surface.vertices.size() < 3)
		{
			return false;
		}

		const nri_scene::CapturedVertex& a = surface.vertices[0];
		const nri_scene::CapturedVertex& b = surface.vertices[1];
		const nri_scene::CapturedVertex& c = surface.vertices[2];
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float nx = aby * acz - abz * acy;
		const float ny = abz * acx - abx * acz;
		const float nz = abx * acy - aby * acx;
		const float lengthSq = nx * nx + ny * ny + nz * nz;
		if (lengthSq <= 1.0e-8f)
		{
			return false;
		}

		const float invLength = 1.0f / std::sqrt(lengthSq);
		outNormal[0] = nx * invLength;
		outNormal[1] = ny * invLength;
		outNormal[2] = nz * invLength;
		return true;
	}

	static void NudgeBlindSpotReplacementFlats(nri_scene::SceneView& sceneView)
	{
		static constexpr float kBlindSpotFlatDepthNudge = 0.01f;

		for (nri_scene::SurfaceRef& surface : sceneView.opaqueFlats)
		{
			if (surface.provenance.sourceType != nri_scene::SurfaceSourceType::MapFloorSection &&
				surface.provenance.sourceType != nri_scene::SurfaceSourceType::MapCeilingSection)
			{
				continue;
			}

			float normal[3] = {};
			if (!TryComputeCapturedSurfaceNormal(surface, normal))
			{
				continue;
			}

			for (nri_scene::CapturedVertex& vertex : surface.vertices)
			{
				vertex.position[0] += normal[0] * kBlindSpotFlatDepthNudge;
				vertex.position[1] += normal[1] * kBlindSpotFlatDepthNudge;
				vertex.position[2] += normal[2] * kBlindSpotFlatDepthNudge;
				vertex.prevPosition[0] += normal[0] * kBlindSpotFlatDepthNudge;
				vertex.prevPosition[1] += normal[1] * kBlindSpotFlatDepthNudge;
				vertex.prevPosition[2] += normal[2] * kBlindSpotFlatDepthNudge;
			}
		}
	}

	static void NudgeCapturedSurface(nri_scene::SurfaceRef& surface, float depthNudge)
	{
		float normal[3] = {};
		if (!TryComputeCapturedSurfaceNormal(surface, normal))
		{
			return;
		}

		for (nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			vertex.position[0] += normal[0] * depthNudge;
			vertex.position[1] += normal[1] * depthNudge;
			vertex.position[2] += normal[2] * depthNudge;
			vertex.prevPosition[0] += normal[0] * depthNudge;
			vertex.prevPosition[1] += normal[1] * depthNudge;
			vertex.prevPosition[2] += normal[2] * depthNudge;
		}
	}


	static void RebuildSceneViewStats(nri_scene::SceneView& sceneView)
	{
		const nri_scene::SceneDebugStats preservedStats = sceneView.stats;
		nri_scene::SceneDebugStats stats = {};
		stats.wallDrawItems = (uint32_t)sceneView.opaqueWalls.size();
		stats.flatDrawItems = (uint32_t)sceneView.opaqueFlats.size();
		stats.spriteDrawItems = (uint32_t)sceneView.opaqueSprites.size();

		for (const nri_scene::SurfaceRef& wall : sceneView.opaqueWalls)
		{
			stats.triangleEstimate += !wall.indices.empty() ? (uint32_t)(wall.indices.size() / 3u) : (wall.vertices.size() >= 3 ? (uint32_t)wall.vertices.size() - 2u : 0u);
			stats.materialRefs++;
			if (wall.provenance.sourceType == nri_scene::SurfaceSourceType::MirrorWall)
			{
				stats.mirrorSurfaces++;
			}
		}

		for (const nri_scene::SurfaceRef& flat : sceneView.opaqueFlats)
		{
			stats.triangleEstimate += !flat.indices.empty() ? (uint32_t)(flat.indices.size() / 3u) : (uint32_t)(flat.vertices.size() / 3u);
			stats.materialRefs++;
		}

		for (const nri_scene::SurfaceRef& sprite : sceneView.opaqueSprites)
		{
			stats.triangleEstimate += !sprite.indices.empty() ? (uint32_t)(sprite.indices.size() / 3u) : (sprite.vertices.size() >= 3 ? (uint32_t)sprite.vertices.size() - 2u : 0u);
			stats.materialRefs++;
			if (sprite.provenance.sourceType == nri_scene::SurfaceSourceType::VoxelProxySprite)
			{
				stats.modelDrawItems++;
				stats.voxelProxyDrawItems++;
			}
			else
			{
				stats.translucentDrawItems++;
			}
		}

		stats.totalDrawItems = stats.wallDrawItems + stats.flatDrawItems + stats.spriteDrawItems;
		stats.voxelStableCandidates = preservedStats.voxelStableCandidates;
		stats.voxelStableUncacheable = preservedStats.voxelStableUncacheable;
		stats.voxelStableSignatureHits = preservedStats.voxelStableSignatureHits;
		stats.voxelStableSignatureMisses = preservedStats.voxelStableSignatureMisses;
		stats.voxelStableSignatureChanges = preservedStats.voxelStableSignatureChanges;
		stats.voxelStableSplitStable = preservedStats.voxelStableSplitStable;
		stats.voxelStableSplitLive = preservedStats.voxelStableSplitLive;
		stats.voxelCacheEntries = preservedStats.voxelCacheEntries;
		stats.voxelCacheSurfaceHits = preservedStats.voxelCacheSurfaceHits;
		stats.voxelCacheSurfaceStores = preservedStats.voxelCacheSurfaceStores;
		stats.voxelCacheSurfaceRebuilds = preservedStats.voxelCacheSurfaceRebuilds;
		stats.voxelCacheTransformRebakes = preservedStats.voxelCacheTransformRebakes;
		stats.voxelCacheSurfaceRemoves = preservedStats.voxelCacheSurfaceRemoves;
		stats.voxelCacheNotCaptured = preservedStats.voxelCacheNotCaptured;
		stats.voxelCachePrimitives = preservedStats.voxelCachePrimitives;
		sceneView.stats = stats;
	}


	struct MaterialTextureAttributionCounts
	{
		uint32_t materialCount = 0;
		uint32_t actorMaterialCount = 0;
		uint32_t textureCount = 0;
		uint32_t baseTextureCount = 0;
		uint32_t glowTextureCount = 0;
		uint32_t normalTextureCount = 0;
		uint32_t metallicTextureCount = 0;
		uint32_t roughnessTextureCount = 0;
		uint32_t emissiveTextureCount = 0;
	};

	static bool ShouldTraceActorOverflow()
	{
		return (int)perf_looptraceframes > 0;
	}

	static bool ShouldTraceResidentGeometryOrderHash()
	{
		return (int)perf_looptraceframes > 0;
	}

	static bool ShouldTraceSceneBufferDirtyRanges()
	{
		return (int)perf_looptraceframes > 0;
	}


	static constexpr uint32_t NRI_MAX_ACTOR_OVERFLOW_TRACE_LINES = 16;

	static MaterialTextureAttributionCounts GatherMaterialTextureAttribution(
		const std::vector<nri_scene::MaterialData>& materials,
		const std::vector<nri_scene::MaterialLightingMetadata>& lightMetadata,
		size_t textureCount)
	{
		MaterialTextureAttributionCounts counts = {};
		counts.materialCount = (uint32_t)materials.size();
		counts.textureCount = (uint32_t)textureCount;

		std::unordered_set<uint32_t> baseTextures;
		std::unordered_set<uint32_t> glowTextures;
		std::unordered_set<uint32_t> normalTextures;
		std::unordered_set<uint32_t> metallicTextures;
		std::unordered_set<uint32_t> roughnessTextures;
		std::unordered_set<uint32_t> emissiveTextures;
		baseTextures.reserve(materials.size());
		glowTextures.reserve(lightMetadata.size());
		normalTextures.reserve(materials.size());
		metallicTextures.reserve(materials.size());
		roughnessTextures.reserve(materials.size());
		emissiveTextures.reserve(materials.size());

		const auto addTextureIndex = [textureCount](std::unordered_set<uint32_t>& destination, uint32_t textureIndex)
		{
			if (textureIndex != UINT32_MAX && (size_t)textureIndex < textureCount)
			{
				destination.insert(textureIndex);
			}
		};

		for (uint32_t materialIndex = 0; materialIndex < (uint32_t)materials.size(); ++materialIndex)
		{
			const auto& material = materials[materialIndex];
			addTextureIndex(baseTextures, material.textureIndex);
			addTextureIndex(normalTextures, material.normalTextureIndex);
			addTextureIndex(metallicTextures, material.metallicTextureIndex);
			addTextureIndex(roughnessTextures, material.roughnessTextureIndex);
			addTextureIndex(emissiveTextures, material.emissiveTextureIndex);
			if (materialIndex < lightMetadata.size())
			{
				const auto& metadata = lightMetadata[materialIndex];
				addTextureIndex(glowTextures, metadata.glowmapTextureIndex);
				if (metadata.actorIndex >= 0)
				{
					counts.actorMaterialCount++;
				}
			}
		}

		counts.baseTextureCount = (uint32_t)baseTextures.size();
		counts.glowTextureCount = (uint32_t)glowTextures.size();
		counts.normalTextureCount = (uint32_t)normalTextures.size();
		counts.metallicTextureCount = (uint32_t)metallicTextures.size();
		counts.roughnessTextureCount = (uint32_t)roughnessTextures.size();
		counts.emissiveTextureCount = (uint32_t)emissiveTextures.size();
		return counts;
	}

	static void AccumulateMaterialTextureAttribution(NRIRenderer::MaterialBuildTraceEntry& entry, const MaterialTextureAttributionCounts& counts)
	{
		entry.materialCount += counts.materialCount;
		entry.actorMaterialCount += counts.actorMaterialCount;
		entry.textureCount += counts.textureCount;
		entry.baseTextureCount += counts.baseTextureCount;
		entry.glowTextureCount += counts.glowTextureCount;
		entry.normalTextureCount += counts.normalTextureCount;
		entry.metallicTextureCount += counts.metallicTextureCount;
		entry.roughnessTextureCount += counts.roughnessTextureCount;
		entry.emissiveTextureCount += counts.emissiveTextureCount;
	}

static void Copy3(const float* src, float* dst)
{
	std::memcpy(dst, src, sizeof(float) * 3);
}

static void Copy2(const float* src, float* dst)
{
	std::memcpy(dst, src, sizeof(float) * 2);
}

static const char* YesNo(bool value)
{
	return value ? "yes" : "no";
}

static double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
{
	return std::chrono::duration<double, std::milli>(end - start).count();
}

static bool ShouldTracePtPerf()
{
	return PerfLoopTraceActive() || ShouldEmitRendererTemporalTraceLogs();
}

static bool ShouldCollectPtPerfTiming()
{
	return ShouldTracePtPerf() || (bool)nri_ptslowdowntrace;
}

class ScopedPtPerfTimer
{
public:
	explicit ScopedPtPerfTimer(double& targetMs)
		: mTarget(ShouldCollectPtPerfTiming() ? &targetMs : nullptr)
	{
		if (mTarget != nullptr)
		{
			mStart = std::chrono::steady_clock::now();
		}
	}

	~ScopedPtPerfTimer()
	{
		if (mTarget != nullptr)
		{
			*mTarget += DurationMs(mStart, std::chrono::steady_clock::now());
		}
	}

private:
	double* mTarget = nullptr;
	std::chrono::steady_clock::time_point mStart = {};
};

}


RenderSceneHistorySnapshot NRIRenderer::CaptureRenderSceneHistorySnapshot(bool preserveHistory) const
{
	RenderSceneHistorySnapshot snapshot = {};
	snapshot.frameIndex = mFrameIndex;
	snapshot.currentTanHalfFovX = mCurrentTanHalfFovX;
	snapshot.currentTanHalfFovY = mCurrentTanHalfFovY;
	snapshot.previousTanHalfFovX = mPreviousTanHalfFovX;
	snapshot.previousTanHalfFovY = mPreviousTanHalfFovY;
	snapshot.hasPreviousCameraState = mHasPreviousCameraState;
	snapshot.resetHistory = mResetHistory;
	if (preserveHistory)
	{
		Copy3(mCurrentCameraPos, snapshot.currentCameraPos);
		Copy3(mCurrentCameraForward, snapshot.currentCameraForward);
		Copy3(mCurrentCameraRight, snapshot.currentCameraRight);
		Copy3(mCurrentCameraUp, snapshot.currentCameraUp);
		Copy3(mPreviousCameraPos, snapshot.previousCameraPos);
		Copy3(mPreviousCameraForward, snapshot.previousCameraForward);
		Copy3(mPreviousCameraRight, snapshot.previousCameraRight);
		Copy3(mPreviousCameraUp, snapshot.previousCameraUp);
		Copy2(mCurrentJitter, snapshot.currentJitter);
		Copy2(mPreviousJitter, snapshot.previousJitter);
		std::memcpy(snapshot.currentViewToClip, mCurrentViewToClip, sizeof(snapshot.currentViewToClip));
		std::memcpy(snapshot.previousViewToClip, mPreviousViewToClip, sizeof(snapshot.previousViewToClip));
		std::memcpy(snapshot.currentWorldToView, mCurrentWorldToView, sizeof(snapshot.currentWorldToView));
		std::memcpy(snapshot.previousWorldToView, mPreviousWorldToView, sizeof(snapshot.previousWorldToView));
	}
	return snapshot;
}

void NRIRenderer::RestoreRenderSceneHistorySnapshot(const RenderSceneHistorySnapshot& snapshot)
{
	mFrameIndex = snapshot.frameIndex;
	Copy3(snapshot.currentCameraPos, mCurrentCameraPos);
	Copy3(snapshot.currentCameraForward, mCurrentCameraForward);
	Copy3(snapshot.currentCameraRight, mCurrentCameraRight);
	Copy3(snapshot.currentCameraUp, mCurrentCameraUp);
	Copy3(snapshot.previousCameraPos, mPreviousCameraPos);
	Copy3(snapshot.previousCameraForward, mPreviousCameraForward);
	Copy3(snapshot.previousCameraRight, mPreviousCameraRight);
	Copy3(snapshot.previousCameraUp, mPreviousCameraUp);
	Copy2(snapshot.currentJitter, mCurrentJitter);
	Copy2(snapshot.previousJitter, mPreviousJitter);
	std::memcpy(mCurrentViewToClip, snapshot.currentViewToClip, sizeof(mCurrentViewToClip));
	std::memcpy(mPreviousViewToClip, snapshot.previousViewToClip, sizeof(mPreviousViewToClip));
	std::memcpy(mCurrentWorldToView, snapshot.currentWorldToView, sizeof(mCurrentWorldToView));
	std::memcpy(mPreviousWorldToView, snapshot.previousWorldToView, sizeof(mPreviousWorldToView));
	mCurrentTanHalfFovX = snapshot.currentTanHalfFovX;
	mCurrentTanHalfFovY = snapshot.currentTanHalfFovY;
	mPreviousTanHalfFovX = snapshot.previousTanHalfFovX;
	mPreviousTanHalfFovY = snapshot.previousTanHalfFovY;
	mHasPreviousCameraState = snapshot.hasPreviousCameraState;
	mResetHistory = snapshot.resetHistory;
}

bool NRIRenderer::EnsureRenderSceneFrameResources(const NRIRendererFrameContext& frameContext, bool preserveHistory, const RenderSceneHistorySnapshot& history)
{
	bool ready = false;
	{
		ScopedPtPerfTimer initPerfTimer(mLastPerfShellTraceStats.initResourcesMs);
		ready =
			Initialize() &&
			NRIFrameResources::EnsureFrameResources(
				*this,
				frameContext.outputWidth,
				frameContext.outputHeight,
				frameContext.targetWidth,
				frameContext.targetHeight);
	}
	if (!ready)
	{
		LogFallback("PT frame resources or pipelines failed to initialize.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}
	return true;
}

bool NRIRenderer::BeginRenderSceneFrame(HWDrawInfo& di, const NRIRendererFrameContext& frameContext, bool preserveHistory, const RenderSceneHistorySnapshot& history)
{
	ResetSceneBufferFrameStats();
	ResetRendererSkyPerfTraceStats();
	nri_scene::ResetAverageTextureColorCache();
	nri_scene::ResetSkyPerfStats();
	mUsedStaticMapSceneLastFrame = false;
	mUsedDynamicSceneLastFrame = false;
	mHasVisibleMirrorPortalLastFrame = false;
	mUploadedStaticMapSceneLastFrame = false;
	mBuiltStaticMapSceneASLastFrame = false;
	mBuiltDynamicSceneASLastFrame = false;
	mDynamicSceneLastFrame = {};
	mRuntimeMutation.BeginFrameState();
	mRuntimeSpaceLinkLastFrame = {};
	if (!preserveHistory)
	{
		mPendingFrameGenerationTimestamp = std::chrono::steady_clock::now();
		mHasPendingFrameGenerationRealFrameTime = false;
		mPendingFrameGenerationRealFrameTimeMs = 0.0f;
		if (mHasFrameGenerationTimestamp)
		{
			const auto elapsed = mPendingFrameGenerationTimestamp - mLastFrameGenerationTimestamp;
			mPendingFrameGenerationRealFrameTimeMs = (float)std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(elapsed).count();
			mHasPendingFrameGenerationRealFrameTime = true;
			if (mPendingFrameGenerationRealFrameTimeMs > 250.0f)
			{
				RequestHistoryReset("cadence-break");
			}
		}
	}
	UpdateFrameGenerationHistoryPolicy(frameContext.debugMode, mFrameBuffer->mFrameGeneration.GetPolicy(), frameContext.preserveHistory);

	RefreshMapWorld();
	if (!ApplyStartupMapWorldCorrectionIfNeeded("render-frame-start"))
	{
		LogFallback("PT startup world correction failed.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}
	if (mPendingStaticMapLightingInvalidation)
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.lightingInvalidationsApplied++;
		}
		InvalidateStaticMapSceneForMaterialLighting();
		mPendingStaticMapLightingInvalidation = false;
	}
	UpdatePerFrameState(di);
	if (preserveHistory)
	{
		mResetHistory = true;
	}
	return true;
}

bool NRIRenderer::RenderSimpleBootstrapView(bool preserveHistory, const RenderSceneHistorySnapshot& history)
{
	mHistoryInputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPing : FrameTextureSlot::TaaHistoryPong;
	mHistoryOutputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPong : FrameTextureSlot::TaaHistoryPing;
	mUpscaledInputSlot = FrameTextureSlot::Composed;
	mUseUpscaledInFinal = false;
	NRIPassDispatchContext passContext = BuildPassDispatchContext();
	if (!NRIPassDispatcher::DispatchBootstrapView(passContext))
	{
		LogFallback("PT bootstrap view dispatch failed.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}

	CopyFinalToActiveTarget();
	if (!preserveHistory)
	{
		NoteSuccessfulRealFrame();
		++mFrameIndex;
		mHasPreviousCameraState = true;
		mResetHistory = false;
	}
	else
	{
		RestoreRenderSceneHistorySnapshot(history);
	}
	return true;
}

bool NRIRenderer::DispatchSelectedRenderScene(const RenderSceneDispatchInputs& inputs)
{
	if (inputs.bootstrapCapturedView)
	{
		mHistoryInputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPing : FrameTextureSlot::TaaHistoryPong;
		mHistoryOutputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPong : FrameTextureSlot::TaaHistoryPing;
		mUpscaledInputSlot = FrameTextureSlot::Composed;
		mUseUpscaledInFinal = false;
		NRIPassDispatchContext passContext = BuildPassDispatchContext();
		return inputs.buffersReady && NRIPassDispatcher::DispatchBootstrapView(passContext);
	}

	NRIPassDispatchContext passContext = BuildPassDispatchContext();
	return inputs.accelerationReady &&
		inputs.drawInfo != nullptr &&
		inputs.activeGeometry != nullptr &&
		inputs.activeGpuMaterials != nullptr &&
		NRIPassDispatcher::DispatchFrameGraph(passContext, *inputs.drawInfo, *inputs.activeGeometry, *inputs.activeGpuMaterials, inputs.drawmode);
}

void NRIRenderer::LogRenderSceneFailureReasons(bool paletteReady, bool texturesReady, bool buffersReady, bool accelerationReady, bool dispatched, bool bootstrapCapturedView)
{
	if (!paletteReady)
	{
		LogFallback("PT palette texture upload failed.");
	}
	else if (!texturesReady)
	{
		LogFallback("PT material texture upload failed.");
	}
	else if (!buffersReady)
	{
		LogFallback("PT scene buffer upload failed.");
	}
	else if (!accelerationReady)
	{
		LogFallback("PT acceleration structure build failed.");
	}
	else if (!dispatched)
	{
		LogFallback(bootstrapCapturedView ? "PT bootstrap captured-scene dispatch failed." : "PT frame graph dispatch failed.");
	}
}

void NRIRenderer::CommitRenderSceneResult(const RenderSceneCompletionInputs& inputs, const RenderSceneHistorySnapshot& history)
{
	if (inputs.success)
	{
		mHasLoggedFallback = false;
		if (inputs.bootstrapCapturedView)
		{
			CopyFinalToActiveTarget();
		}

		if (!inputs.preserveHistory)
		{
			NoteSuccessfulRealFrame();
			mFrameIndex++;
			mHasPreviousCameraState = true;
			mResetHistory = false;
		}
		else
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
	}
	else if (inputs.preserveHistory)
	{
		RestoreRenderSceneHistorySnapshot(history);
	}

	if (inputs.success)
	{
		RecordRenderSceneSuccessStats(inputs);
		EmitSelfTestSummary(inputs.traceFrameIndex, inputs.drawmode, inputs.portal);
	}
	EmitRenderSceneTemporalTrace(inputs.traceFrameIndex);
}

void NRIRenderer::RecordRenderSceneSuccessStats(const RenderSceneCompletionInputs& inputs)
{
	if (inputs.activeGeometry == nullptr || inputs.activeGpuMaterials == nullptr)
	{
		return;
	}

	mLastPerfShellTraceStats.activePrimitiveCount = (uint32_t)inputs.activeGeometry->primitives.size();
	mLastPerfShellTraceStats.dynamicPrimitiveCount = inputs.activeDynamicGeometry != nullptr ? (uint32_t)inputs.activeDynamicGeometry->primitives.size() : 0u;
	mLastPerfShellTraceStats.activeMaterialCount = (uint32_t)inputs.activeGpuMaterials->size();
	mLastPerfShellTraceStats.sceneInstanceCount = (uint32_t)mBoundSceneInstances.size();
	mLastPerfShellTraceStats.sceneInstanceStaticCount = 0;
	mLastPerfShellTraceStats.sceneInstanceDynamicCount = 0;
	mLastPerfShellTraceStats.sceneInstancePersistentVoxelCount = 0;
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneInstanceStatsMs);
		for (const SceneInstanceData& instance : mBoundSceneInstances)
		{
			if (instance.dataSource == nri_diag::SceneDataSourceStatic)
			{
				mLastPerfShellTraceStats.sceneInstanceStaticCount++;
			}
			else if (instance.dataSource == nri_diag::SceneDataSourceDynamic)
			{
				mLastPerfShellTraceStats.sceneInstanceDynamicCount++;
			}
			else if (instance.dataSource == nri_diag::SceneDataSourcePersistentVoxel)
			{
				mLastPerfShellTraceStats.sceneInstancePersistentVoxelCount++;
			}
		}
	}
	NRIPersistentVoxelStatusSnapshot persistentVoxelStatus = {};
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.persistentVoxelResourceStatsMs);
		mPersistentVoxels.FillResourceStatusSnapshot(persistentVoxelStatus);
	}
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.persistentVoxelBatchStatsMs);
		mPersistentVoxels.FillBatchStatusSnapshot(persistentVoxelStatus);
	}
	mLastPerfShellTraceStats.persistentVoxelMeshVariantResourceCount = persistentVoxelStatus.meshVariantResourceCount;
	mLastPerfShellTraceStats.persistentVoxelMaterialVariantResourceCount = persistentVoxelStatus.materialVariantResourceCount;
	mLastPerfShellTraceStats.persistentVoxelBatchActorCount = persistentVoxelStatus.batchActorCount;
	mLastPerfShellTraceStats.persistentVoxelInstanceRecordCount = persistentVoxelStatus.instanceRecordCount;
	mLastPerfShellTraceStats.persistentVoxelAdmissionQueueCount = persistentVoxelStatus.admissionQueueCount;
	mLastPerfShellTraceStats.persistentVoxelPendingInstanceCount = persistentVoxelStatus.pendingInstanceCount;
	mLastPerfShellTraceStats.persistentVoxelResidentResourceBytes = persistentVoxelStatus.residentResourceBytes;
	mLastPerfShellTraceStats.persistentVoxelZeroRefResourceBytes = persistentVoxelStatus.zeroRefResourceBytes;
	mLastPerfShellTraceStats.persistentVoxelZeroRefMeshResourceCount = persistentVoxelStatus.zeroRefMeshResourceCount;
	mLastPerfShellTraceStats.persistentVoxelZeroRefMaterialResourceCount = persistentVoxelStatus.zeroRefMaterialResourceCount;
	mLastPerfShellTraceStats.persistentVoxelInstanceActiveCount = persistentVoxelStatus.activeInstanceCount;
	mLastPerfShellTraceStats.persistentVoxelInstancePrimitiveCount = persistentVoxelStatus.instancePrimitiveCount;
	mLastPerfShellTraceStats.persistentVoxelInstanceMaterialCount = persistentVoxelStatus.instanceMaterialCount;
	mLastPerfShellTraceStats.persistentVoxelInstanceMinPrimitiveCount = persistentVoxelStatus.instanceMinPrimitiveCount;
	mLastPerfShellTraceStats.persistentVoxelInstanceMaxPrimitiveCount = persistentVoxelStatus.instanceMaxPrimitiveCount;
	mLastPerfShellTraceStats.usedStaticMapScene = mUsedStaticMapSceneLastFrame;
	mLastPerfShellTraceStats.usedDynamicOverlay = mGpuSceneHasDynamicOverlay;
	mLastPerfShellTraceStats.usedPersistentDynamicEmissiveCache = inputs.usingPersistentDynamicEmissiveCache;
	const double accountedMs =
		mLastPerfShellTraceStats.initResourcesMs +
		mLastPerfShellTraceStats.mapWorldMs +
		mLastPerfShellTraceStats.updateStateMs +
		mLastPerfShellTraceStats.sceneSelectMs +
		mLastPerfShellTraceStats.sceneLightsMs +
		mLastPerfShellTraceStats.residentLightRefreshMs +
		mLastPerfShellTraceStats.emissiveUpdateMs +
		mLastPerfShellTraceStats.emissiveTlasMs +
		mLastPerfShellTraceStats.surfaceProbeMs +
		mLastPerfShellTraceStats.frameGraphMs;
	mLastPerfShellTraceStats.otherMs = std::max(0.0, mLastPerfShellTraceStats.totalMs - accountedMs);
}

void NRIRenderer::EmitRenderSceneTemporalTrace(uint32_t traceFrameIndex)
{
	if (!ShouldEmitRendererTemporalTraceLogs())
	{
		return;
	}

	const auto& analyticLights = mSceneLights.GetAnalyticLights();
	const auto& emissiveSurfaces = mSceneLights.GetEmissiveSurfaces();
	Printf("NRI PT light trace: frame=%u analytic=%u topo=%s prop=%s added=%u removed=%u rebound=%u emissive=%u topo=%s prop=%s added=%u removed=%u rebound=%u reset=%s reason=%s\n",
		traceFrameIndex,
		(uint32_t)analyticLights.activeLights.size(),
		YesNo(analyticLights.lastBuildTopologyChanged),
		YesNo(analyticLights.lastBuildPropertiesChanged),
		(uint32_t)analyticLights.addedTopologyKeys.size(),
		(uint32_t)analyticLights.removedTopologyKeys.size(),
		(uint32_t)analyticLights.reboundTopologyKeys.size(),
		(uint32_t)emissiveSurfaces.activeSurfaces.size(),
		YesNo(emissiveSurfaces.lastBuildTopologyChanged),
		YesNo(emissiveSurfaces.lastBuildPropertiesChanged),
		(uint32_t)emissiveSurfaces.addedTopologyKeys.size(),
		(uint32_t)emissiveSurfaces.removedTopologyKeys.size(),
		(uint32_t)emissiveSurfaces.reboundTopologyKeys.size(),
		YesNo(mResetHistory),
		mResetHistory ? mLastHistoryResetReason.c_str() : "none");

	const nri_scene::SkyPerfStats sceneSkyPerf = nri_scene::ConsumeSkyPerfStats();
	Printf("NRI PT sky perf: frame=%u ensure_scene=%u preserve_scene=%u rebuild_scene=%u ensure_sky=%u preserve_hit=%u reuse_active=%u reuse_probe=%u probe=%u/%u face_probes=%u uploads=%u ensure_ms=%.3f probe_ms=%.3f face_ms=%.3f upload_ms=%.3f static_builds=%u overlay_builds=%u\n",
		traceFrameIndex,
		gRendererSkyPerfTraceStats.ensureSceneTexturesCalls,
		gRendererSkyPerfTraceStats.ensureSceneTexturesPreserveTrueCalls,
		gRendererSkyPerfTraceStats.ensureSceneTexturesPreserveFalseCalls,
		gRendererSkyPerfTraceStats.ensureSkyCalls,
		gRendererSkyPerfTraceStats.preserveExistingHits,
		gRendererSkyPerfTraceStats.reuseActiveCubemapHits + gRendererSkyPerfTraceStats.solidReuseHits,
		gRendererSkyPerfTraceStats.reuseActiveProbeHits,
		gRendererSkyPerfTraceStats.probeSuccesses,
		gRendererSkyPerfTraceStats.probeAttempts,
		gRendererSkyPerfTraceStats.probeFaceCalls,
		gRendererSkyPerfTraceStats.buildCubemapUploadCalls,
		(double)gRendererSkyPerfTraceStats.ensureSkyTimeUs / 1000.0,
		(double)gRendererSkyPerfTraceStats.probeCubemapTimeUs / 1000.0,
		(double)gRendererSkyPerfTraceStats.probeFaceTimeUs / 1000.0,
		(double)gRendererSkyPerfTraceStats.buildCubemapUploadTimeUs / 1000.0,
		gRendererSkyPerfTraceStats.residentStaticSceneTextureBuilds,
		gRendererSkyPerfTraceStats.combinedOverlayTextureBuilds);
	Printf("NRI PT sky scene: frame=%u updates=%u wall=%u flat=%u portal=%u inspects=%u cubemap_candidates=%u solid_candidates=%u inspect_faces=%u avg_base=%u avg_recursive=%u recursive_faces=%u avg_pixels=%llu update_ms=%.3f inspect_ms=%.3f avg_ms=%.3f\n",
		traceFrameIndex,
		sceneSkyPerf.updateCalls,
		sceneSkyPerf.wallUpdateCalls,
		sceneSkyPerf.flatUpdateCalls,
		sceneSkyPerf.portalUpdateCalls,
		sceneSkyPerf.inspectCalls,
		sceneSkyPerf.inspectCubemapCandidates,
		sceneSkyPerf.inspectSolidCandidates,
		sceneSkyPerf.inspectFaceWalks,
		sceneSkyPerf.averageColorBaseCalls,
		sceneSkyPerf.averageColorRecursiveCalls,
		sceneSkyPerf.recursiveSkyboxFaceSamples,
		(unsigned long long)sceneSkyPerf.averageColorPixels,
		(double)sceneSkyPerf.updateTimeUs / 1000.0,
		(double)sceneSkyPerf.inspectTimeUs / 1000.0,
		(double)sceneSkyPerf.averageColorTimeUs / 1000.0);
	Printf("NRI PT sky invalidation: frame=%u requests=%u applied=%u emissive_material_dirty=%u keep_last=%u hold_level=%u cached_cubemap=%u create_cubemap=%u cached_solid=%u create_solid=%u\n",
		traceFrameIndex,
		gRendererSkyPerfTraceStats.lightingInvalidationRequests,
		gRendererSkyPerfTraceStats.lightingInvalidationsApplied,
		gRendererSkyPerfTraceStats.emissiveMaterialDirtyEvents,
		gRendererSkyPerfTraceStats.keepLastCubemapHits,
		gRendererSkyPerfTraceStats.holdLevelCubemapHits,
		gRendererSkyPerfTraceStats.activateCachedCubemapHits,
		gRendererSkyPerfTraceStats.createCachedCubemapHits,
		gRendererSkyPerfTraceStats.solidActivateHits,
		gRendererSkyPerfTraceStats.solidCreateHits);
}

bool NRIRenderer::BuildRenderSceneFrame(HWDrawInfo& di, const RenderSceneFrameBuildInputs& inputs, const RenderSceneHistorySnapshot& history, RenderSceneFrameBuildResult& frame)
{
	const uint32_t bootstrapMode = inputs.bootstrapMode;
	const bool bootstrapCapturedView = inputs.bootstrapCapturedView;
	const bool bootstrapCapturedDiagnostics = inputs.bootstrapCapturedDiagnostics;
	const bool bootstrapCapturedFlat = inputs.bootstrapCapturedFlat;
	const bool bootstrapCapturedBaseColor = inputs.bootstrapCapturedBaseColor;
	const bool rawTraceDirectScene = inputs.rawTraceDirectScene;
	const bool preserveHistory = inputs.preserveHistory;
	const NRIPersistentVoxelSettings persistentVoxelSettings = BuildNRIPersistentVoxelSettingsFromCVars();
	const bool allowStaticMapScene = !bootstrapCapturedView && !rawTraceDirectScene && mMapWorld.valid;
	nri_scene::SceneView& capturedSceneView = frame.capturedSceneView;
	nri_scene::SceneView& dynamicSceneView = frame.dynamicSceneView;
	nri_scene::GeometryData& capturedGeometry = frame.capturedGeometry;
	NRIRuntimeMutationFrameOutput& runtimeMutationFrame = frame.runtimeMutationFrame;
	nri_scene::GeometryData& runtimeSpaceLinkGeometry = frame.runtimeSpaceLinkGeometry;
	nri_scene::GeometryData& dynamicGeometry = frame.dynamicGeometry;
	nri_scene::GeometryData& mirrorExtendedDynamicGeometry = frame.mirrorExtendedDynamicGeometry;
	nri_scene::GeometryData& mergedDynamicGeometry = frame.mergedDynamicGeometry;
	nri_scene::GeometryData& debugSphereGeometry = frame.debugSphereGeometry;
	nri_scene::GeometryData& surfaceLightGeometry = frame.surfaceLightGeometry;
	nri_scene::MaterialBridgeData& materialBridge = frame.materialBridge;
	nri_scene::MaterialBridgeData& runtimeSpaceLinkMaterialBridge = frame.runtimeSpaceLinkMaterialBridge;
	nri_scene::MaterialBridgeData& dynamicMaterialBridge = frame.dynamicMaterialBridge;
	nri_scene::MaterialBridgeData& mirrorExtendedDynamicMaterialBridge = frame.mirrorExtendedDynamicMaterialBridge;
	nri_scene::MaterialBridgeData& mirrorPlayerMaterialBridge = frame.mirrorPlayerMaterialBridge;
	nri_scene::MaterialBridgeData& sceneLightMergedDynamicMaterialBridge = frame.sceneLightMergedDynamicMaterialBridge;
	nri_scene::MaterialBridgeData& mergedDynamicMaterialBridge = frame.mergedDynamicMaterialBridge;
	nri_scene::MaterialBridgeData& debugSphereMaterialBridge = frame.debugSphereMaterialBridge;
	nri_scene::MaterialBridgeData& surfaceLightMaterialBridge = frame.surfaceLightMaterialBridge;
	nri_scene::GeometryData& overlayGeometry = mSelectOverlayGeometryScratch;
	nri_scene::MaterialBridgeData& overlayMaterialBridge = mSelectOverlayMaterialBridgeScratch;
	nri_scene::MaterialBridgeData& combinedMaterialBridge = frame.combinedMaterialBridge;
	auto& capturedGpuMaterials = mSelectCapturedGpuMaterialScratch;
	auto& dynamicGpuMaterials = mSelectDynamicGpuMaterialScratch;
	auto& persistentVoxelGpuMaterials = mSelectPersistentVoxelGpuMaterialScratch;
	auto& combinedGpuMaterials = mSelectCombinedGpuMaterialScratch;
	auto& refreshedCombinedGpuMaterials = mSelectRefreshedCombinedGpuMaterialScratch;
	capturedGpuMaterials.clear();
	dynamicGpuMaterials.clear();
	persistentVoxelGpuMaterials.clear();
	combinedGpuMaterials.clear();
	refreshedCombinedGpuMaterials.clear();
	nri_scene::ClearGeometryRetainingCapacity(mSelectMirrorPlayerGeometryScratch);
	nri_scene::ClearGeometryRetainingCapacity(mSelectOverlayGeometryScratch);
	nri_scene::ClearMaterialBridgeRetainingCapacity(mSelectOverlayMaterialBridgeScratch);
	mSelectTopLevelInstanceScratch.clear();
	mSelectSceneInstanceScratch.clear();
	mSelectCapturedTopLevelInstanceScratch.clear();
	mSelectCapturedSceneInstanceScratch.clear();
	const nri_scene::SceneView*& activeSceneView = frame.activeSceneView;
	const nri_scene::GeometryData*& activeGeometry = frame.activeGeometry;
	const std::vector<nri_scene::MaterialData>*& activeGpuMaterials = frame.activeGpuMaterials;
	const nri_scene::MaterialBridgeData*& activeMaterialBridge = frame.activeMaterialBridge;
	const nri_scene::SceneView* sceneLightCapturedView = nullptr;
	const nri_scene::MaterialBridgeData* sceneLightCapturedMaterials = nullptr;
	const nri_scene::SceneView* sceneLightDynamicView = nullptr;
	const nri_scene::MaterialBridgeData* sceneLightDynamicMaterials = nullptr;
	nri_scene::SceneView& mirrorExtendedDynamicSceneView = frame.mirrorExtendedDynamicSceneView;
	nri_scene::SceneView& mirrorPlayerSceneView = frame.mirrorPlayerSceneView;
	nri_scene::SceneView& sceneLightMergedDynamicSceneView = frame.sceneLightMergedDynamicSceneView;
	nri_scene::SceneView& mergedDynamicSceneView = frame.mergedDynamicSceneView;
	const nri_scene::SceneView*& activeDynamicSceneView = frame.activeDynamicSceneView;
	const nri_scene::GeometryData*& activeDynamicGeometry = frame.activeDynamicGeometry;
	const nri_scene::MaterialBridgeData*& activeDynamicMaterials = frame.activeDynamicMaterials;
	nri_scene::GeometryData& mirrorPlayerGeometry = mSelectMirrorPlayerGeometryScratch;
	NRIMirrorPlayerCaptureStats mirrorPlayerCaptureStats = {};
	nri_scene::GeometryBuildTraceStats mirrorPlayerGeometryTraceStats = {};
	std::vector<SceneBufferUploadDomainSpan> sceneUploadDomainSpans;
	uint32_t activeStaticProbePrimitiveCount = 0;
	EmissiveSamplingBuildContext emissiveSamplingContext = {};
	bool sceneLightUsesStaticMapScene = false;
	nri_scene::SceneDebugStats& activeStats = frame.activeStats;
	bool& paletteReady = frame.paletteReady;
	bool& texturesReady = frame.texturesReady;
	bool& buffersReady = frame.buffersReady;
	bool& accelerationReady = frame.accelerationReady;
	uint32_t combinedOverlayMaterialOffset = 0;
	bool& usingPersistentDynamicEmissiveCache = frame.usingPersistentDynamicEmissiveCache;
	bool liveDynamicHasEmissive = false;
	bool hasPersistentVoxelBatch = false;
	bool appendPersistentVoxelSceneLights = false;
	uint32_t selectedStaticSceneInstanceCount = 0;
	uint32_t selectedDynamicSceneInstanceCount = 0;
	uint32_t selectedPersistentVoxelSceneInstanceCount = 0;
	uint32_t selectedSceneInstanceCount = 0;
	uint32_t selectedTlasInstanceCount = 0;
	{
		ScopedPtPerfTimer sceneSelectTimer(mLastPerfShellTraceStats.sceneSelectMs);
		const bool staticMapSceneReady = allowStaticMapScene && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectStaticMapMs);
			return EnsureStaticMapScene();
		}();
		NRISceneFramePathSelectionInputs pathSelectionInputs = {};
		pathSelectionInputs.allowStaticMapScene = allowStaticMapScene;
		pathSelectionInputs.staticMapSceneReady = staticMapSceneReady;
		const NRISceneFramePathSelectionResult pathSelection = SelectNRISceneFramePath(pathSelectionInputs);
		const bool hasStaticMapScene = pathSelection.hasStaticMapScene;
		if (hasStaticMapScene)
		{
			sceneLightUsesStaticMapScene = true;
			emissiveSamplingContext.staticGeometry = &mStaticMapScene.geometry;
			mUsedStaticMapSceneLastFrame = true;
			activeSceneView = &mStaticMapScene.sceneView;
			activeGeometry = &mStaticMapScene.geometry;
			activeGpuMaterials = &mStaticMapScene.gpuMaterials;
			activeMaterialBridge = &mStaticMapScene.materialBridge;
			activeStaticProbePrimitiveCount = (uint32_t)mStaticMapScene.geometry.primitives.size();
			activeStats = mStaticMapScene.sceneView.stats;

			bool residentStaticWorldGeometryChanged = false;
			NRISceneFrameOverlayDeferralInputs deferralInputs = {};
			deferralInputs.uploadedStaticMapSceneLastFrame = mUploadedStaticMapSceneLastFrame;
			deferralInputs.builtStaticMapSceneASLastFrame = mBuiltStaticMapSceneASLastFrame;
			const NRISceneFrameOverlayDeferralResult deferral = SelectNRISceneFrameOverlayDeferral(deferralInputs);
			const bool deferOverlayThisFrame = deferral.deferOverlayThisFrame;
			const bool hasRuntimeSpaceLinkOverlay = !deferOverlayThisFrame && [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeSpaceLinkMs);
				return BuildRuntimeSpaceLinkOverlay(di, runtimeSpaceLinkGeometry, runtimeSpaceLinkMaterialBridge);
			}();
			mLastPerfShellTraceStats.runtimeSpaceLinkPrimitiveCount = (uint32_t)runtimeSpaceLinkGeometry.primitives.size();
			mLastPerfShellTraceStats.runtimeSpaceLinkMaterialCount = (uint32_t)runtimeSpaceLinkMaterialBridge.materials.size();
			const bool hasRuntimeMutationOverlay = [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationMs);
				const bool hasOverlay = mRuntimeMutation.BuildFrameOverlay(
					NRIRuntimeMutationSystem::BuildOverlayServices(*this),
					runtimeMutationFrame);
				residentStaticWorldGeometryChanged = runtimeMutationFrame.residentStaticSceneChanged;
				return hasOverlay;
			}();
			const bool hasDynamicScene = !deferOverlayThisFrame && [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.dynamicCaptureMs);
				(void)nri_scene::ConsumeDynamicCapturePerfStats();
				const bool captured = nri_scene::CaptureDynamicScene(di, dynamicSceneView);
				const nri_scene::DynamicCapturePerfStats captureStats = nri_scene::ConsumeDynamicCapturePerfStats();
				mLastPerfShellTraceStats.dynamicCaptureCalls += captureStats.calls;
				mLastPerfShellTraceStats.dynamicCaptureWallSurfaces += captureStats.wallSurfaces;
				mLastPerfShellTraceStats.dynamicCaptureFlatSurfaces += captureStats.flatSurfaces;
				mLastPerfShellTraceStats.dynamicCaptureSpriteSurfaces += captureStats.spriteSurfaces;
				mLastPerfShellTraceStats.dynamicCaptureVoxelProxySurfaces += captureStats.voxelProxySurfaces;
				mLastPerfShellTraceStats.dynamicCaptureUnsupportedModelSurfaces += captureStats.unsupportedModelSurfaces;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCacheStores += captureStats.voxelCacheStores;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCacheRebuilds += captureStats.voxelCacheRebuilds;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCacheDeferred += captureStats.voxelCacheDeferred;
				mLastPerfShellTraceStats.dynamicCaptureVoxelMeshBuilds += captureStats.voxelMeshCacheBuilds;
				mLastPerfShellTraceStats.dynamicCaptureVoxelMeshDeferred += captureStats.voxelMeshCacheDeferred;
				mLastPerfShellTraceStats.dynamicCaptureVoxelMeshInvalid += captureStats.voxelMeshCacheInvalid;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCanonicalSurfaceBuilds += captureStats.voxelCanonicalSurfaceBuilds;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCanonicalSurfaceHits += captureStats.voxelCanonicalSurfaceHits;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCanonicalSurfaceInvalid += captureStats.voxelCanonicalSurfaceInvalid;
				mLastPerfShellTraceStats.voxelCacheActorEntries = dynamicSceneView.stats.voxelCacheEntries;
				mLastPerfShellTraceStats.voxelCacheActorSurfaces = dynamicSceneView.stats.voxelCacheActorSurfaces;
				mLastPerfShellTraceStats.voxelCacheUniqueMeshKeys = dynamicSceneView.stats.voxelCacheUniqueMeshKeys;
				mLastPerfShellTraceStats.voxelCacheUniqueMaterialKeys = dynamicSceneView.stats.voxelCacheUniqueMaterialKeys;
				mLastPerfShellTraceStats.voxelCacheLocalSpaceSurfaces = dynamicSceneView.stats.voxelCacheLocalSpaceSurfaces;
				mLastPerfShellTraceStats.voxelCacheBakedTransformSurfaces = dynamicSceneView.stats.voxelCacheBakedTransformSurfaces;
				mLastPerfShellTraceStats.voxelCacheUnknownSpaceSurfaces = dynamicSceneView.stats.voxelCacheUnknownSpaceSurfaces;
				mLastPerfShellTraceStats.voxelCacheTransformKeyedSurfaces = dynamicSceneView.stats.voxelCacheTransformKeyedSurfaces;
				mLastPerfShellTraceStats.voxelCacheUniqueTransformBases = dynamicSceneView.stats.voxelCacheUniqueTransformBases;
				mLastPerfShellTraceStats.voxelCacheInvariantWarnings = dynamicSceneView.stats.voxelCacheInvariantWarnings;
				mLastPerfShellTraceStats.voxelCacheActorPrimitives = dynamicSceneView.stats.voxelCachePrimitives;
				mLastPerfShellTraceStats.voxelCacheDuplicatedVertexBytes = dynamicSceneView.stats.voxelCacheDuplicatedVertexBytes;
				mLastPerfShellTraceStats.voxelCacheDuplicatedIndexBytes = dynamicSceneView.stats.voxelCacheDuplicatedIndexBytes;
				mLastPerfShellTraceStats.voxelCacheDuplicatedPrimitiveBytes = dynamicSceneView.stats.voxelCacheDuplicatedPrimitiveBytes;
				mLastPerfShellTraceStats.voxelCacheDuplicatedTotalBytes = dynamicSceneView.stats.voxelCacheDuplicatedTotalBytes;
				mLastPerfShellTraceStats.voxelCacheDuplicateTopCount = dynamicSceneView.stats.voxelCacheDuplicateTopCount;
				mLastPerfShellTraceStats.voxelCacheDuplicateTopEntries = dynamicSceneView.stats.voxelCacheDuplicateTopEntries;
				mLastPerfShellTraceStats.dynamicVoxelEscapeActorCount = dynamicSceneView.stats.dynamicVoxelEscapeActorCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapeEligibleActorCount = dynamicSceneView.stats.dynamicVoxelEscapeEligibleActorCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapeForcedActorCount = dynamicSceneView.stats.dynamicVoxelEscapeForcedActorCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapePrimitiveCount = dynamicSceneView.stats.dynamicVoxelEscapePrimitiveCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapeVertexBytes = dynamicSceneView.stats.dynamicVoxelEscapeVertexBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapeIndexBytes = dynamicSceneView.stats.dynamicVoxelEscapeIndexBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapePrimitiveBytes = dynamicSceneView.stats.dynamicVoxelEscapePrimitiveBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapeMaterialBytes = dynamicSceneView.stats.dynamicVoxelEscapeMaterialBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapeTotalBytes = dynamicSceneView.stats.dynamicVoxelEscapeTotalBytes;
				mLastPerfShellTraceStats.dynamicVoxelExpectedEscapeActorCount = dynamicSceneView.stats.dynamicVoxelExpectedEscapeActorCount;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapeActorCount = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapeActorCount;
				mLastPerfShellTraceStats.dynamicVoxelExpectedEscapePrimitiveCount = dynamicSceneView.stats.dynamicVoxelExpectedEscapePrimitiveCount;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapePrimitiveCount = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapePrimitiveCount;
				mLastPerfShellTraceStats.dynamicVoxelExpectedEscapeTotalBytes = dynamicSceneView.stats.dynamicVoxelExpectedEscapeTotalBytes;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapeTotalBytes = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapeTotalBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapeTopCount = dynamicSceneView.stats.dynamicVoxelEscapeTopCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapeTopEntries = dynamicSceneView.stats.dynamicVoxelEscapeTopEntries;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapeTopCount = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapeTopCount;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapeTopEntries = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapeTopEntries;
				mLastPerfShellTraceStats.dynamicCaptureCountMs += captureStats.countMs;
				mLastPerfShellTraceStats.dynamicCaptureWallsMs += captureStats.wallsMs;
				mLastPerfShellTraceStats.dynamicCaptureFlatsMs += captureStats.flatsMs;
				mLastPerfShellTraceStats.dynamicCaptureFacingSpritesMs += captureStats.facingSpritesMs;
				mLastPerfShellTraceStats.dynamicCaptureModelSpritesMs += captureStats.modelSpritesMs;
				mLastPerfShellTraceStats.dynamicCaptureModelClassifyMs += captureStats.modelClassifyMs;
				mLastPerfShellTraceStats.dynamicCaptureModelMeshMs += captureStats.modelMeshMs;
				mLastPerfShellTraceStats.dynamicCaptureModelSurfaceMs += captureStats.modelSurfaceMs;
				mLastPerfShellTraceStats.dynamicCaptureModelStoreMs += captureStats.modelStoreMs;
				mLastPerfShellTraceStats.dynamicCaptureVoxelFrameMs += captureStats.voxelFrameMs;
				mLastPerfShellTraceStats.dynamicCaptureStatsMs += captureStats.statsMs;
				return captured;
			}();
			const int32_t preferredMirrorWallIndex =
			mLastSurfaceProbe.valid &&
			mLastSurfaceProbe.hit &&
			(mLastSurfaceProbe.primitiveFlags & nri_scene::MaterialFlag_Mirror) != 0 &&
			mLastSurfaceProbe.provenance.wallIndex >= 0 ?
				mLastSurfaceProbe.provenance.wallIndex :
				-1;
		const NRIMirrorPortalSelectionResult visibleMirrorPortalSelection = !deferOverlayThisFrame ? [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectMirrorPortalMs);
				NRIMirrorPortalSelectionRequest request = {};
				request.drawInfo = &di;
				request.preferredWallIndex = preferredMirrorWallIndex;
				return SelectNRIPrimaryMirrorPortal(request);
			}() :
			NRIMirrorPortalSelectionResult {};
		HWPortal* const visibleMirrorPortal = visibleMirrorPortalSelection.portal;
		const uint32_t visibleMirrorPortalCandidates = visibleMirrorPortalSelection.candidateCount;
		const int32_t selectedVisibleMirrorWallIndex = visibleMirrorPortalSelection.selectedWallIndex;
		mHasVisibleMirrorPortalLastFrame = visibleMirrorPortal != nullptr;
		const bool hasMirrorExtendedDynamicScene = !deferOverlayThisFrame && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectMirrorCaptureMs);
			NRIMirrorExtendedCaptureRequest request = {};
			request.drawInfo = &di;
			request.mirrorPortal = visibleMirrorPortal;
			request.selectedMirrorWallIndex = selectedVisibleMirrorWallIndex;
			request.baseDynamicSceneView = hasDynamicScene ? &dynamicSceneView : nullptr;
			request.frameIndex = mFrameIndex;
			request.rebuildSceneViewStats = RebuildSceneViewStats;
			const NRIMirrorExtendedCaptureResult result =
				CaptureNRIMirrorExtendedDynamicScene(request, mirrorExtendedDynamicSceneView);
			return result.captured;
		}();
		const bool hasMirrorPlayerScene = !deferOverlayThisFrame && IsNRIMirrorPlayerPreviewCaptureEnabled() && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectMirrorCaptureMs);
			ScopedPtPerfTimer mirrorPlayerTimer(mLastPerfShellTraceStats.mirrorPlayerCaptureMs);
			NRIMirrorPlayerCaptureRequest request = {};
			request.drawInfo = &di;
			request.mirrorPortal = visibleMirrorPortal;
			request.selectedMirrorWallIndex = selectedVisibleMirrorWallIndex;
			request.mirrorPortalCandidates = visibleMirrorPortalCandidates;
			request.rebuildSceneViewStats = RebuildSceneViewStats;
			const NRIMirrorPlayerCaptureResult result =
				CaptureNRIMirrorPlayerDynamicScene(request, mirrorPlayerSceneView);
			mirrorPlayerCaptureStats = result.stats;
			return result.captured;
		}();
		if (hasDynamicScene)
		{
			{
				Clocker clock(NriPTGeometryBuild);
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildDynamicLiveMs);
				nri_scene::BuildGeometry(dynamicSceneView, dynamicGeometry);
				AssignGeometryPortalIndices(mMapWorld, dynamicGeometry);
			}
			mLastPerfShellTraceStats.geometryBuildDynamicLivePrimitives += (uint32_t)dynamicGeometry.primitives.size();

			if (!dynamicGeometry.primitives.empty())
			{
				{
					Clocker clock(NriPTMaterialBuild);
					BuildMaterialsWithActorOverrides(dynamicSceneView, dynamicMaterialBridge, "dynamic_live");
				}
			}

			sceneLightDynamicView = &dynamicSceneView;
			sceneLightDynamicMaterials = &dynamicMaterialBridge;
			activeDynamicSceneView = &dynamicSceneView;
			activeDynamicGeometry = &dynamicGeometry;
			activeDynamicMaterials = &dynamicMaterialBridge;
			liveDynamicHasEmissive = [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.persistentDynamicMs);
				return mSceneLights.RebuildPersistentDynamicEmissiveCache(
					dynamicSceneView,
					dynamicMaterialBridge,
					BuildPersistentDynamicEmissiveCacheServices());
			}();
		}
		if (hasMirrorExtendedDynamicScene)
		{
			{
				Clocker clock(NriPTGeometryBuild);
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildMirrorExtendedMs);
				nri_scene::BuildGeometry(mirrorExtendedDynamicSceneView, mirrorExtendedDynamicGeometry);
				AssignGeometryPortalIndices(mMapWorld, mirrorExtendedDynamicGeometry);
			}

			if (!mirrorExtendedDynamicGeometry.primitives.empty())
			{
				Clocker clock(NriPTMaterialBuild);
				BuildMaterialsWithActorOverrides(mirrorExtendedDynamicSceneView, mirrorExtendedDynamicMaterialBridge, "mirror_extended");
			}

			if (hasDynamicScene)
			{
				ScopedPtPerfTimer mergePerfTimer(mLastPerfShellTraceStats.sceneSelectLightMergeMs);
				sceneLightMergedDynamicSceneView = dynamicSceneView;
				sceneLightMergedDynamicSceneView.opaqueWalls.insert(
					sceneLightMergedDynamicSceneView.opaqueWalls.end(),
					mirrorExtendedDynamicSceneView.opaqueWalls.begin(),
					mirrorExtendedDynamicSceneView.opaqueWalls.end());
				sceneLightMergedDynamicSceneView.opaqueFlats.insert(
					sceneLightMergedDynamicSceneView.opaqueFlats.end(),
					mirrorExtendedDynamicSceneView.opaqueFlats.begin(),
					mirrorExtendedDynamicSceneView.opaqueFlats.end());
				sceneLightMergedDynamicSceneView.opaqueSprites.insert(
					sceneLightMergedDynamicSceneView.opaqueSprites.end(),
					mirrorExtendedDynamicSceneView.opaqueSprites.begin(),
					mirrorExtendedDynamicSceneView.opaqueSprites.end());
				RebuildSceneViewStats(sceneLightMergedDynamicSceneView);
				BuildMaterialsWithActorOverrides(sceneLightMergedDynamicSceneView, sceneLightMergedDynamicMaterialBridge, "scene_light_merged_dynamic");
				sceneLightDynamicView = &sceneLightMergedDynamicSceneView;
				sceneLightDynamicMaterials = &sceneLightMergedDynamicMaterialBridge;
			}
			else
			{
				sceneLightDynamicView = &mirrorExtendedDynamicSceneView;
				sceneLightDynamicMaterials = &mirrorExtendedDynamicMaterialBridge;
			}
		}
		if (hasMirrorPlayerScene)
		{
			{
				Clocker clock(NriPTGeometryBuild);
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildMirrorPlayerMs);
				{
					ScopedPtPerfTimer buildTimer(mLastPerfShellTraceStats.mirrorPlayerGeometryBuildMs);
					nri_scene::BuildGeometry(mirrorPlayerSceneView, mirrorPlayerGeometry, &mirrorPlayerGeometryTraceStats, true);
				}
				{
					ScopedPtPerfTimer portalTimer(mLastPerfShellTraceStats.mirrorPlayerPortalAssignMs);
					AssignGeometryPortalIndices(mMapWorld, mirrorPlayerGeometry);
				}
				mLastPerfShellTraceStats.mirrorPlayerGeometryBuildWallMs = mirrorPlayerGeometryTraceStats.wallMs;
				mLastPerfShellTraceStats.mirrorPlayerGeometryBuildFlatMs = mirrorPlayerGeometryTraceStats.flatMs;
				mLastPerfShellTraceStats.mirrorPlayerGeometryBuildSpriteMs = mirrorPlayerGeometryTraceStats.spriteMs;
				mLastPerfShellTraceStats.mirrorPlayerCaptureRawFacingSprites = mirrorPlayerCaptureStats.rawFacingSprites;
				mLastPerfShellTraceStats.mirrorPlayerCaptureRawVoxelSprites = mirrorPlayerCaptureStats.rawVoxelSprites;
				mLastPerfShellTraceStats.mirrorPlayerCaptureSurfaces = mirrorPlayerCaptureStats.capturedSurfaceCount;
				mLastPerfShellTraceStats.mirrorPlayerCaptureMatchingActorSurfaces = mirrorPlayerCaptureStats.capturedMatchingActorSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerCaptureOtherActorSurfaces = mirrorPlayerCaptureStats.capturedOtherActorSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerCaptureActorlessSurfaces = mirrorPlayerCaptureStats.capturedActorlessSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerCaptureFilteredSurfaces = mirrorPlayerCaptureStats.filteredSurfaceCount;
				mLastPerfShellTraceStats.mirrorPlayerGeometryWallSurfaces = mirrorPlayerGeometryTraceStats.wallSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometryFlatSurfaces = mirrorPlayerGeometryTraceStats.flatSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometrySpriteSurfaces = mirrorPlayerGeometryTraceStats.spriteSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometryIndexedSurfaces = mirrorPlayerGeometryTraceStats.indexedSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometryTriangleFanSurfaces = mirrorPlayerGeometryTraceStats.triangleFanSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometrySpriteStripSurfaces = mirrorPlayerGeometryTraceStats.spriteStripSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometrySkippedSurfaces = mirrorPlayerGeometryTraceStats.skippedSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometrySourceVertices = mirrorPlayerGeometryTraceStats.sourceVertexCount;
				mLastPerfShellTraceStats.mirrorPlayerGeometrySourceIndices = mirrorPlayerGeometryTraceStats.sourceIndexCount;
				mLastPerfShellTraceStats.mirrorPlayerGeometryVertexGrowths = mirrorPlayerGeometryTraceStats.vertexCapacityGrowths;
				mLastPerfShellTraceStats.mirrorPlayerGeometryIndexGrowths = mirrorPlayerGeometryTraceStats.indexCapacityGrowths;
				mLastPerfShellTraceStats.mirrorPlayerGeometryPrimitiveGrowths = mirrorPlayerGeometryTraceStats.primitiveCapacityGrowths;
				mLastPerfShellTraceStats.mirrorPlayerGeometryProvenanceGrowths = mirrorPlayerGeometryTraceStats.provenanceCapacityGrowths;
			}

			if (!mirrorPlayerGeometry.primitives.empty())
			{
				Clocker clock(NriPTMaterialBuild);
				ScopedPtPerfTimer materialTimer(mLastPerfShellTraceStats.mirrorPlayerMaterialBuildMs);
				BuildMaterialsWithActorOverrides(mirrorPlayerSceneView, mirrorPlayerMaterialBridge, "mirror_player");
			}
		}

		hasPersistentVoxelBatch = !deferOverlayThisFrame && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectPersistentVoxelBatchMs);
			const MemoryTelemetry telemetry = GetMemoryTelemetry();
			mPersistentVoxels.PumpAdmissionQueue(
				"runtime",
				mMapWorld.buildSerial,
				mFrameIndex,
				persistentVoxelSettings,
				telemetry.totalTrackedBytes,
				mFrameBuffer != nullptr ? mFrameBuffer->GetAdapterLocalBudgetBytes() : 0ull,
				(int)nri_ptloadingtrace,
				(bool)nri_voxelstats,
				BuildNRIPersistentVoxelResetServices(*this),
				BuildNRIPersistentVoxelAdmissionServices(*this));
			return EnsurePersistentVoxelBatch();
		}();

		PersistentDynamicSurfaceStats persistentDynamicStats = {};
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectPersistentEmissiveMs);
			mSceneLights.PrunePersistentDynamicEmissiveCacheToLiveActors(BuildPersistentDynamicEmissiveCacheServices());
			persistentDynamicStats = mSceneLights.GatherPersistentDynamicEmissiveSurfaceStats();
			mSceneLights.UpdatePersistentDynamicEmissiveHighWaterStats(persistentDynamicStats);
		}
		mLastPerfShellTraceStats.persistentDynamicActorSurfaceCount = persistentDynamicStats.actorSurfaceCount;
		mLastPerfShellTraceStats.persistentDynamicNonActorSurfaceCount = persistentDynamicStats.nonActorSurfaceCount;
		mLastPerfShellTraceStats.persistentDynamicWallSurfaceCount = persistentDynamicStats.wallSurfaceCount;
		mLastPerfShellTraceStats.persistentDynamicFlatSurfaceCount = persistentDynamicStats.flatSurfaceCount;
		mLastPerfShellTraceStats.persistentDynamicSpriteSurfaceCount = persistentDynamicStats.spriteSurfaceCount;

		const PersistentDynamicEmissiveCache& persistentDynamicCache = mSceneLights.GetPersistentDynamicEmissiveCache();
		const bool shouldUsePersistentDynamicEmissive = persistentDynamicCache.valid;
		if (shouldUsePersistentDynamicEmissive)
		{
			usingPersistentDynamicEmissiveCache = true;
			if (hasDynamicScene)
			{
				ScopedPtPerfTimer mergePerfTimer(mLastPerfShellTraceStats.sceneSelectDynamicMergeMs);
				mergedDynamicSceneView = dynamicSceneView;
				mSceneLights.MergePersistentDynamicEmissiveCacheIntoSceneView(mergedDynamicSceneView);
				RebuildSceneViewStats(mergedDynamicSceneView);

				{
					Clocker clock(NriPTGeometryBuild);
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildMergedDynamicMs);
					nri_scene::BuildGeometry(mergedDynamicSceneView, mergedDynamicGeometry);
					AssignGeometryPortalIndices(mMapWorld, mergedDynamicGeometry);
				}
				{
					Clocker clock(NriPTMaterialBuild);
					BuildMaterialsWithActorOverrides(mergedDynamicSceneView, mergedDynamicMaterialBridge, "dynamic_with_persistent_emissive");
				}

				if (!mergedDynamicGeometry.primitives.empty())
				{
					activeDynamicSceneView = &mergedDynamicSceneView;
					activeDynamicGeometry = &mergedDynamicGeometry;
					activeDynamicMaterials = &mergedDynamicMaterialBridge;
				}
			}
			else
			{
				activeDynamicSceneView = &persistentDynamicCache.sceneView;
				activeDynamicGeometry = &persistentDynamicCache.geometry;
				activeDynamicMaterials = &persistentDynamicCache.materialBridge;
			}

			if (hasMirrorExtendedDynamicScene && activeDynamicSceneView != nullptr && activeDynamicMaterials != nullptr)
			{
				ScopedPtPerfTimer mergePerfTimer(mLastPerfShellTraceStats.sceneSelectLightMergeMs);
				sceneLightMergedDynamicSceneView = *activeDynamicSceneView;
				sceneLightMergedDynamicSceneView.opaqueWalls.insert(
					sceneLightMergedDynamicSceneView.opaqueWalls.end(),
					mirrorExtendedDynamicSceneView.opaqueWalls.begin(),
					mirrorExtendedDynamicSceneView.opaqueWalls.end());
				sceneLightMergedDynamicSceneView.opaqueFlats.insert(
					sceneLightMergedDynamicSceneView.opaqueFlats.end(),
					mirrorExtendedDynamicSceneView.opaqueFlats.begin(),
					mirrorExtendedDynamicSceneView.opaqueFlats.end());
				sceneLightMergedDynamicSceneView.opaqueSprites.insert(
					sceneLightMergedDynamicSceneView.opaqueSprites.end(),
					mirrorExtendedDynamicSceneView.opaqueSprites.begin(),
					mirrorExtendedDynamicSceneView.opaqueSprites.end());
				RebuildSceneViewStats(sceneLightMergedDynamicSceneView);
				BuildMaterialsWithActorOverrides(sceneLightMergedDynamicSceneView, sceneLightMergedDynamicMaterialBridge, "scene_light_merged_persistent");
				sceneLightDynamicView = &sceneLightMergedDynamicSceneView;
				sceneLightDynamicMaterials = &sceneLightMergedDynamicMaterialBridge;
			}
			else if (activeDynamicSceneView != nullptr && activeDynamicMaterials != nullptr)
			{
				sceneLightDynamicView = activeDynamicSceneView;
				sceneLightDynamicMaterials = activeDynamicMaterials;
			}
			else if (hasMirrorExtendedDynamicScene)
			{
				sceneLightDynamicView = &mirrorExtendedDynamicSceneView;
				sceneLightDynamicMaterials = &mirrorExtendedDynamicMaterialBridge;
			}
		}

		if (hasPersistentVoxelBatch && mPersistentVoxels.HasValidBatch())
		{
			appendPersistentVoxelSceneLights = true;
		}

		const bool runtimeDebugSphereBuilt = !deferOverlayThisFrame && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeDebugSphereMs);
			NRIDebugOverlayBuildTelemetry debugOverlayTelemetry = {};
			const bool built = mDebugOverlays.BuildRuntimeDebugSphereOverlay(
				debugSphereGeometry,
				debugSphereMaterialBridge,
				debugOverlayTelemetry,
				ShouldCollectPtPerfTiming());
			mLastPerfShellTraceStats.runtimeDebugSphereViewMs += debugOverlayTelemetry.runtimeDebugSphereViewMs;
			mLastPerfShellTraceStats.runtimeDebugSphereGeoMs += debugOverlayTelemetry.runtimeDebugSphereGeoMs;
			mLastPerfShellTraceStats.runtimeDebugSphereMaterialMs += debugOverlayTelemetry.runtimeDebugSphereMaterialMs;
			mLastPerfShellTraceStats.geometryBuildDebugSphereMs += debugOverlayTelemetry.geometryBuildDebugSphereMs;
			mLastPerfShellTraceStats.runtimeDebugSphereCount = debugOverlayTelemetry.runtimeDebugSphereCount;
			mLastPerfShellTraceStats.runtimeDebugSphereLongitudeSegments = debugOverlayTelemetry.runtimeDebugSphereLongitudeSegments;
			mLastPerfShellTraceStats.runtimeDebugSphereLatitudeSegments = debugOverlayTelemetry.runtimeDebugSphereLatitudeSegments;
			mLastPerfShellTraceStats.runtimeDebugSpherePrimitiveCount = debugOverlayTelemetry.runtimeDebugSpherePrimitiveCount;
			mLastPerfShellTraceStats.runtimeDebugSphereMaterialCount = debugOverlayTelemetry.runtimeDebugSphereMaterialCount;
			return built;
		}();
		const bool surfaceLightBuilt = !deferOverlayThisFrame &&
			BuildSurfaceLightOverlay(surfaceLightGeometry, surfaceLightMaterialBridge);
		NRISceneFrameOverlayEligibilityInputs overlayEligibilityInputs = {};
		overlayEligibilityInputs.deferOverlayThisFrame = deferOverlayThisFrame;
		overlayEligibilityInputs.runtimeSpaceLinkBuilt = hasRuntimeSpaceLinkOverlay;
		overlayEligibilityInputs.runtimeMutationBuilt = hasRuntimeMutationOverlay;
		overlayEligibilityInputs.hasPersistentVoxelBatch = hasPersistentVoxelBatch;
		overlayEligibilityInputs.persistentVoxelRenderable = hasPersistentVoxelBatch ? mPersistentVoxels.HasRenderableOverlay() : false;
		overlayEligibilityInputs.activeDynamicGeometry = activeDynamicGeometry;
		overlayEligibilityInputs.activeDynamicMaterials = activeDynamicMaterials;
		overlayEligibilityInputs.hasMirrorExtendedDynamicScene = hasMirrorExtendedDynamicScene;
		overlayEligibilityInputs.mirrorExtendedGeometry = &mirrorExtendedDynamicGeometry;
		overlayEligibilityInputs.mirrorExtendedMaterials = &mirrorExtendedDynamicMaterialBridge;
		overlayEligibilityInputs.hasMirrorPlayerScene = hasMirrorPlayerScene;
		overlayEligibilityInputs.mirrorPlayerGeometry = &mirrorPlayerGeometry;
		overlayEligibilityInputs.mirrorPlayerMaterials = &mirrorPlayerMaterialBridge;
		overlayEligibilityInputs.runtimeDebugSphereBuilt = runtimeDebugSphereBuilt;
		overlayEligibilityInputs.surfaceLightBuilt = surfaceLightBuilt;
		const NRISceneFrameOverlayEligibilityResult overlayEligibility =
			SelectNRISceneFrameOverlayEligibility(overlayEligibilityInputs);
		const bool hasRuntimeSpaceLinkOverlayForAssembly = overlayEligibility.hasRuntimeSpaceLinkOverlay;
		const bool hasRuntimeMutationOverlayForAssembly = overlayEligibility.hasRuntimeMutationOverlay;
		const bool hasPersistentVoxelOverlay = overlayEligibility.hasPersistentVoxelOverlay;
		const bool hasActiveDynamicOverlay = overlayEligibility.hasActiveDynamicOverlay;
		const bool hasMirrorExtendedDynamicOverlay = overlayEligibility.hasMirrorExtendedDynamicOverlay;
		const bool hasMirrorPlayerOverlay = overlayEligibility.hasMirrorPlayerOverlay;
		const bool hasRuntimeDebugSphereOverlay = overlayEligibility.hasRuntimeDebugSphereOverlay;
		const bool hasSurfaceLightOverlay = overlayEligibility.hasSurfaceLightOverlay;

		if (overlayEligibility.hasAnyOverlay)
		{
			NRIPersistentVoxelOverlayStats persistentVoxelOverlayStats = {};
			if (hasPersistentVoxelOverlay)
			{
				persistentVoxelOverlayStats = mPersistentVoxels.BuildOverlayStats();
			}

			NRISceneFrameOverlayBuildInputs overlayInputs = {};
			overlayInputs.collectTiming = ShouldCollectPtPerfTiming();
			overlayInputs.mapWorldBuildSerial = mMapWorld.buildSerial;
			overlayInputs.frameIndex = mFrameIndex;
			overlayInputs.stats = &mLastPerfShellTraceStats;
			overlayInputs.hasPersistentVoxelOverlay = hasPersistentVoxelOverlay;
			overlayInputs.persistentVoxelOverlayStats = hasPersistentVoxelOverlay ? &persistentVoxelOverlayStats : nullptr;

			overlayInputs.hasRuntimeSpaceLinkOverlay = hasRuntimeSpaceLinkOverlayForAssembly;
			overlayInputs.runtimeSpaceLinkGeometry = &runtimeSpaceLinkGeometry;
			overlayInputs.runtimeSpaceLinkMaterials = &runtimeSpaceLinkMaterialBridge;
			overlayInputs.runtimeSpaceLinkTelemetry = {
				&mLastPerfShellTraceStats.overlayRuntimeSpaceLinkMs,
				&mLastPerfShellTraceStats.overlayRuntimeSpaceLinkGeometryMs,
				&mLastPerfShellTraceStats.overlayRuntimeSpaceLinkMaterialMs,
				&mLastPerfShellTraceStats.overlayRuntimeSpaceLinkPrimitiveCount,
				&mLastPerfShellTraceStats.overlayRuntimeSpaceLinkMaterialCount,
				&mLastPerfShellTraceStats.overlayRuntimeSpaceLinkAppend };

			overlayInputs.hasRuntimeMutationOverlay = hasRuntimeMutationOverlayForAssembly;
			overlayInputs.runtimeMutationGeometry = &runtimeMutationFrame.geometry;
			overlayInputs.runtimeMutationMaterials = &runtimeMutationFrame.materialBridge;
			overlayInputs.runtimeMutationTelemetry = {
				&mLastPerfShellTraceStats.overlayRuntimeMutationMs,
				&mLastPerfShellTraceStats.overlayRuntimeMutationGeometryMs,
				&mLastPerfShellTraceStats.overlayRuntimeMutationMaterialMs,
				&mLastPerfShellTraceStats.overlayRuntimeMutationPrimitiveCount,
				&mLastPerfShellTraceStats.overlayRuntimeMutationMaterialCount,
				&mLastPerfShellTraceStats.overlayRuntimeMutationAppend };

			overlayInputs.hasActiveDynamicOverlay = hasActiveDynamicOverlay;
			overlayInputs.activeDynamicSceneView = activeDynamicSceneView;
			overlayInputs.activeDynamicGeometry = activeDynamicGeometry;
			overlayInputs.activeDynamicMaterials = activeDynamicMaterials;
			overlayInputs.activeDynamicTelemetry = {
				&mLastPerfShellTraceStats.overlayDynamicMs,
				&mLastPerfShellTraceStats.overlayDynamicGeometryMs,
				&mLastPerfShellTraceStats.overlayDynamicMaterialMs,
				&mLastPerfShellTraceStats.overlayDynamicPrimitiveCount,
				&mLastPerfShellTraceStats.overlayDynamicMaterialCount,
				&mLastPerfShellTraceStats.overlayDynamicAppend };

			overlayInputs.hasMirrorExtendedDynamicOverlay = hasMirrorExtendedDynamicOverlay;
			overlayInputs.mirrorExtendedSceneView = &mirrorExtendedDynamicSceneView;
			overlayInputs.mirrorExtendedGeometry = &mirrorExtendedDynamicGeometry;
			overlayInputs.mirrorExtendedMaterials = &mirrorExtendedDynamicMaterialBridge;
			overlayInputs.mirrorExtendedTelemetry = {
				&mLastPerfShellTraceStats.overlayMirrorExtendedMs,
				&mLastPerfShellTraceStats.overlayMirrorExtendedGeometryMs,
				&mLastPerfShellTraceStats.overlayMirrorExtendedMaterialMs,
				&mLastPerfShellTraceStats.overlayMirrorExtendedPrimitiveCount,
				&mLastPerfShellTraceStats.overlayMirrorExtendedMaterialCount,
				&mLastPerfShellTraceStats.overlayMirrorExtendedAppend };

			overlayInputs.hasMirrorPlayerOverlay = hasMirrorPlayerOverlay;
			overlayInputs.mirrorPlayerGeometry = &mirrorPlayerGeometry;
			overlayInputs.mirrorPlayerMaterials = &mirrorPlayerMaterialBridge;
			overlayInputs.mirrorPlayerTelemetry = {
				&mLastPerfShellTraceStats.overlayMirrorPlayerMs,
				&mLastPerfShellTraceStats.overlayMirrorPlayerGeometryMs,
				&mLastPerfShellTraceStats.overlayMirrorPlayerMaterialMs,
				&mLastPerfShellTraceStats.overlayMirrorPlayerPrimitiveCount,
				&mLastPerfShellTraceStats.overlayMirrorPlayerMaterialCount,
				&mLastPerfShellTraceStats.overlayMirrorPlayerAppend };

			overlayInputs.hasRuntimeDebugSphereOverlay = hasRuntimeDebugSphereOverlay;
			overlayInputs.runtimeDebugSphereGeometry = &debugSphereGeometry;
			overlayInputs.runtimeDebugSphereMaterials = &debugSphereMaterialBridge;
			overlayInputs.runtimeDebugSphereTelemetry = {
				&mLastPerfShellTraceStats.overlayDebugSphereMs,
				&mLastPerfShellTraceStats.overlayDebugSphereGeometryMs,
				&mLastPerfShellTraceStats.overlayDebugSphereMaterialMs,
				&mLastPerfShellTraceStats.overlayDebugSpherePrimitiveCount,
				&mLastPerfShellTraceStats.overlayDebugSphereMaterialCount,
				&mLastPerfShellTraceStats.overlayDebugSphereAppend };

			double surfaceLightOverlayMs = 0.0;
			double surfaceLightGeometryMs = 0.0;
			double surfaceLightMaterialMs = 0.0;
			uint32_t surfaceLightPrimitiveCount = 0;
			uint32_t surfaceLightMaterialCount = 0;
			PerfShellTraceStats::OverlayAppendSourceTraceEntry surfaceLightAppend = {};
			overlayInputs.hasSurfaceLightOverlay = hasSurfaceLightOverlay;
			overlayInputs.surfaceLightGeometry = &surfaceLightGeometry;
			overlayInputs.surfaceLightMaterials = &surfaceLightMaterialBridge;
			overlayInputs.surfaceLightTelemetry = {
				&surfaceLightOverlayMs,
				&surfaceLightGeometryMs,
				&surfaceLightMaterialMs,
				&surfaceLightPrimitiveCount,
				&surfaceLightMaterialCount,
				&surfaceLightAppend };

			NRISceneFrameOverlayBuildOutputs overlayOutputs = {};
			overlayOutputs.overlayGeometry = &overlayGeometry;
			overlayOutputs.overlayMaterialBridge = &overlayMaterialBridge;
			overlayOutputs.uploadSpans = &sceneUploadDomainSpans;
			BuildNRISceneFrameOverlay(overlayInputs, overlayOutputs);

			auto& instances = mSelectTopLevelInstanceScratch;
			auto& sceneInstances = mSelectSceneInstanceScratch;
			instances.clear();
			sceneInstances.clear();
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectStaticInstancesMs);
				BuildStaticMapInstances(instances, sceneInstances);
			}
			const uint32_t staticSceneInstanceBaselineCount = (uint32_t)sceneInstances.size();
			selectedStaticSceneInstanceCount = staticSceneInstanceBaselineCount;
			selectedSceneInstanceCount = (uint32_t)sceneInstances.size();
			selectedTlasInstanceCount = (uint32_t)instances.size();
			bool selectedSceneHasDynamicOverlay = false;

			if (overlayGeometry.primitives.empty() && !hasPersistentVoxelOverlay)
			{
				accelerationReady =
					BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static) &&
					NRISceneUploadManager::UpdateSceneDataSet(*this,
						mStaticVertexBuffer,
						mStaticIndexBuffer,
						mStaticPrimitiveBuffer,
						mStaticMaterialBuffer,
						mStaticVertexBuffer,
						mStaticIndexBuffer,
						mStaticPrimitiveBuffer,
						mStaticMaterialBuffer,
						sceneInstances,
						(uint32_t)mStaticMapScene.geometry.primitives.size(),
						0u,
						(uint32_t)mStaticMapScene.gpuMaterials.size(),
						0u,
						"static_only_scene");
				if (accelerationReady && hasRuntimeMutationOverlay)
				{
					mBuiltStaticMapSceneASLastFrame = false;
				}
			}
			else
			{
				{
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectMaterialBridgeMs);
					combinedMaterialBridge = mStaticMapScene.materialBridge;
					combinedOverlayMaterialOffset = (uint32_t)combinedMaterialBridge.materials.size();
					if (hasPersistentVoxelOverlay)
					{
						mPersistentVoxels.AppendMaterialBridgeTo(combinedMaterialBridge);
						combinedOverlayMaterialOffset = (uint32_t)combinedMaterialBridge.materials.size();
					}
					nri_scene::AppendMaterialBridge(overlayMaterialBridge, combinedMaterialBridge);
				}
				paletteReady = [&]()
				{
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectPaletteMs);
					return EnsurePaletteTexture(combinedMaterialBridge);
				}();
				if (ShouldTraceSkyPerf())
				{
					gRendererSkyPerfTraceStats.combinedOverlayTextureBuilds++;
				}
				texturesReady = paletteReady && [&]()
				{
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectTexturesMs);
					return EnsureSceneTextures(mStaticMapScene.sceneView, combinedMaterialBridge, combinedGpuMaterials, false, "static_map_overlay_combined");
				}();
				dynamicGpuMaterials.clear();
				persistentVoxelGpuMaterials.clear();
				if (texturesReady)
				{
					{
						ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectMaterialSplitMs);
						const size_t staticMaterialCount = mStaticMapScene.gpuMaterials.size();
						const size_t persistentVoxelMaterialCount = hasPersistentVoxelOverlay ? mPersistentVoxels.OverlayMaterialCount() : 0u;
						if (combinedGpuMaterials.size() < staticMaterialCount + persistentVoxelMaterialCount)
						{
							texturesReady = false;
						}
						else
						{
							persistentVoxelGpuMaterials.assign(
								combinedGpuMaterials.begin() + staticMaterialCount,
								combinedGpuMaterials.begin() + staticMaterialCount + persistentVoxelMaterialCount);
							dynamicGpuMaterials.assign(combinedGpuMaterials.begin() + staticMaterialCount + persistentVoxelMaterialCount, combinedGpuMaterials.end());
						}
					}
				}
				buffersReady = texturesReady && [&]()
				{
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadMs);
					return UploadSceneBuffers(overlayGeometry, dynamicGpuMaterials, &sceneUploadDomainSpans) &&
						(!hasPersistentVoxelOverlay || UploadPersistentVoxelArenaMaterialBuffers(persistentVoxelGpuMaterials));
				}();
				accelerationReady = false;
				const uint32_t liveOverlayPrimitiveCount = (uint32_t)overlayGeometry.primitives.size();
				const uint32_t liveOverlayIndexOffset = 0u;
				const uint32_t liveOverlayIndexCount = (uint32_t)overlayGeometry.indices.size();
				NRIAccelerationStructureResource& dynamicBottomLevelAS = GetCurrentDynamicBottomLevelAS();
				if (buffersReady)
				{
					bool persistentVoxelAsReady = true;
					bool dynamicAsReady = true;
					if (hasPersistentVoxelOverlay)
					{
						ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.persistentVoxelAsMs);
						NRIPersistentVoxelAccelerationBuildStats persistentVoxelAsStats = {};
						persistentVoxelAsReady = mPersistentVoxels.BuildAccelerationStructures(
							mFrameIndex,
							(bool)nri_voxelstats,
							BuildNRIPersistentVoxelResetServices(*this),
							BuildNRIPersistentVoxelAccelerationServices(*this),
							persistentVoxelAsStats);
						mLastPerfShellTraceStats.persistentVoxelAsCalls += persistentVoxelAsStats.calls;
						mLastPerfShellTraceStats.persistentVoxelAsBuilds += persistentVoxelAsStats.builds;
						mLastPerfShellTraceStats.persistentVoxelAsUniqueMeshBuilds += persistentVoxelAsStats.uniqueMeshBuilds;
						mLastPerfShellTraceStats.persistentVoxelAsInstances += persistentVoxelAsStats.instances;
					}
					if (liveOverlayPrimitiveCount > 0)
					{
						mLastPerfShellTraceStats.dynamicAsRuntimeSpaceLinkPrimitives = mLastPerfShellTraceStats.overlayRuntimeSpaceLinkAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsRuntimeMutationPrimitives = mLastPerfShellTraceStats.overlayRuntimeMutationAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsDynamicPrimitives = mLastPerfShellTraceStats.overlayDynamicAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsMirrorExtendedPrimitives = mLastPerfShellTraceStats.overlayMirrorExtendedAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsMirrorPlayerPrimitives = mLastPerfShellTraceStats.overlayMirrorPlayerAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsDebugSpherePrimitives = mLastPerfShellTraceStats.overlayDebugSphereAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsRuntimeSpaceLinkBytes = mLastPerfShellTraceStats.overlayRuntimeSpaceLinkAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsRuntimeMutationBytes = mLastPerfShellTraceStats.overlayRuntimeMutationAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsDynamicBytes = mLastPerfShellTraceStats.overlayDynamicAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsMirrorExtendedBytes = mLastPerfShellTraceStats.overlayMirrorExtendedAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsMirrorPlayerBytes = mLastPerfShellTraceStats.overlayMirrorPlayerAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsDebugSphereBytes = mLastPerfShellTraceStats.overlayDebugSphereAppend.byteCount;
						dynamicAsReady =
							BuildDynamicAccelerationStructure(
								overlayGeometry,
								liveOverlayIndexOffset,
								liveOverlayIndexCount,
								liveOverlayPrimitiveCount,
								dynamicBottomLevelAS,
								true) &&
							dynamicBottomLevelAS.accelerationStructure != nullptr;
					}
					else
					{
						mLastPerfShellTraceStats.dynamicAsPrimitiveCount = 0;
						mLastPerfShellTraceStats.dynamicAsVertexCount = 0;
						mLastPerfShellTraceStats.dynamicAsIndexCount = 0;
					}
					accelerationReady = persistentVoxelAsReady && dynamicAsReady;
				}
				emissiveSamplingContext.runtimeMutationGeometry = hasRuntimeMutationOverlay ? &runtimeMutationFrame.geometry : nullptr;
				emissiveSamplingContext.runtimeMutationPrimitiveBaseOffset = (uint32_t)runtimeSpaceLinkGeometry.primitives.size();
				emissiveSamplingContext.dynamicGeometry = hasActiveDynamicOverlay ? activeDynamicGeometry : nullptr;
				emissiveSamplingContext.dynamicPrimitiveBaseOffset = (uint32_t)(runtimeSpaceLinkGeometry.primitives.size() + runtimeMutationFrame.geometry.primitives.size());
				if (accelerationReady)
				{
					if (hasPersistentVoxelOverlay)
					{
						ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectInstanceHandlesMs);
						ScopedPtPerfTimer persistentVoxelTlasTimer(mLastPerfShellTraceStats.persistentVoxelTlasInstanceMs);
						NRIPersistentVoxelTlasServices persistentVoxelTlasServices = {};
						persistentVoxelTlasServices.user = this;
						persistentVoxelTlasServices.getAccelerationStructureHandle = [](void* user, const NRIAccelerationStructureResource& resource) -> uint64_t
						{
							NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
							return resource.accelerationStructure != nullptr ?
								renderer->mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*resource.accelerationStructure) :
								0ull;
						};
						NRIPersistentVoxelTlasBuildStats persistentVoxelTlasStats = {};
						if (!mPersistentVoxels.AppendTlasInstances(
							instances,
							sceneInstances,
							mFrameIndex,
							persistentVoxelSettings,
							(bool)nri_voxelstats,
							persistentVoxelTlasServices,
							persistentVoxelTlasStats))
						{
							accelerationReady = false;
						}
						mLastPerfShellTraceStats.persistentVoxelSharedMeshResources = persistentVoxelTlasStats.sharedMeshResourceCount;
						mLastPerfShellTraceStats.persistentVoxelTlasInstances += persistentVoxelTlasStats.instanceCount;
						mLastPerfShellTraceStats.persistentVoxelBakedFallbackInstances += persistentVoxelTlasStats.bakedFallbackInstanceCount;
					}

					if (liveOverlayPrimitiveCount > 0 && dynamicBottomLevelAS.accelerationStructure != nullptr)
					{
						ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectInstanceHandlesMs);
						nri::TopLevelInstance dynamicInstance = {};
						dynamicInstance.transform[0][0] = 1.0f;
						dynamicInstance.transform[1][1] = 1.0f;
						dynamicInstance.transform[2][2] = 1.0f;
						dynamicInstance.instanceId = (uint32_t)sceneInstances.size();
						dynamicInstance.mask = 0xFF;
						dynamicInstance.shaderBindingTableLocalOffset = 0;
						dynamicInstance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
						dynamicInstance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*dynamicBottomLevelAS.accelerationStructure);
						instances.push_back(dynamicInstance);
						sceneInstances.push_back({ 0u, nri_diag::SceneDataSourceDynamic, 0u, UINT32_MAX });
					}

					selectedStaticSceneInstanceCount = 0;
					selectedDynamicSceneInstanceCount = 0;
					selectedPersistentVoxelSceneInstanceCount = 0;
					for (const SceneInstanceData& sceneInstance : sceneInstances)
					{
						if (sceneInstance.dataSource == nri_diag::SceneDataSourceStatic)
						{
							selectedStaticSceneInstanceCount++;
						}
						else if (sceneInstance.dataSource == nri_diag::SceneDataSourceDynamic)
						{
							selectedDynamicSceneInstanceCount++;
						}
						else if (sceneInstance.dataSource == nri_diag::SceneDataSourcePersistentVoxel)
						{
							selectedPersistentVoxelSceneInstanceCount++;
						}
					}
					selectedSceneInstanceCount = (uint32_t)sceneInstances.size();
					selectedTlasInstanceCount = (uint32_t)instances.size();
					const bool hasEffectiveOverlayInstances = sceneInstances.size() > staticSceneInstanceBaselineCount;
					selectedSceneHasDynamicOverlay =
						liveOverlayPrimitiveCount > 0 ||
						selectedDynamicSceneInstanceCount > 0 ||
						selectedPersistentVoxelSceneInstanceCount > 0 ||
						hasEffectiveOverlayInstances;
					if (selectedSceneHasDynamicOverlay)
					{
						accelerationReady =
							BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static | SceneDataBufferMask_Dynamic) &&
							NRISceneUploadManager::UpdateSceneDataSet(*this,
								mStaticVertexBuffer,
								mStaticIndexBuffer,
								mStaticPrimitiveBuffer,
								mStaticMaterialBuffer,
								GetCurrentDynamicVertexBuffer(),
								GetCurrentDynamicIndexBuffer(),
								GetCurrentDynamicPrimitiveBuffer(),
								GetCurrentDynamicMaterialBuffer(),
								sceneInstances,
								(uint32_t)mStaticMapScene.geometry.primitives.size(),
								(uint32_t)overlayGeometry.primitives.size(),
								(uint32_t)mStaticMapScene.gpuMaterials.size(),
								(uint32_t)dynamicGpuMaterials.size(),
								"static_plus_overlay_scene");
					}
					else
					{
						accelerationReady =
							BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static) &&
							NRISceneUploadManager::UpdateSceneDataSet(*this,
								mStaticVertexBuffer,
								mStaticIndexBuffer,
								mStaticPrimitiveBuffer,
								mStaticMaterialBuffer,
								mStaticVertexBuffer,
								mStaticIndexBuffer,
								mStaticPrimitiveBuffer,
								mStaticMaterialBuffer,
								sceneInstances,
								(uint32_t)mStaticMapScene.geometry.primitives.size(),
								0u,
								(uint32_t)mStaticMapScene.gpuMaterials.size(),
								0u,
								"static_only_effective_scene");
					}
				}
			}

			if (overlayGeometry.primitives.empty() || texturesReady)
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectTexturePrepMs);
				PrepareSceneTextureInputsForCompute();
			}

			if (paletteReady && texturesReady && buffersReady && accelerationReady)
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectStateCommitMs);
				{
					ScopedPtPerfTimer stateFlagsTimer(mLastPerfShellTraceStats.sceneSelectStateCommitFlagsMs);
					mUsedDynamicSceneLastFrame = selectedSceneHasDynamicOverlay;
					mGpuSceneHasDynamicOverlay = selectedSceneHasDynamicOverlay;
					mLastPerfShellTraceStats.sceneSelectStateCommitSelectedDynamic = selectedSceneHasDynamicOverlay ? 1u : 0u;
				}
				{
					NRISceneFrameDynamicStateBuildRequest dynamicStateRequest = {};
					dynamicStateRequest.activeDynamicSceneView = activeDynamicSceneView;
					dynamicStateRequest.activeDynamicGeometry = activeDynamicGeometry;
					dynamicStateRequest.activeDynamicMaterials = activeDynamicMaterials;
					dynamicStateRequest.hasMirrorExtendedDynamicScene = hasMirrorExtendedDynamicScene;
					dynamicStateRequest.mirrorExtendedSceneView = &mirrorExtendedDynamicSceneView;
					dynamicStateRequest.mirrorExtendedGeometry = &mirrorExtendedDynamicGeometry;
					dynamicStateRequest.mirrorExtendedMaterials = &mirrorExtendedDynamicMaterialBridge;
					dynamicStateRequest.hasMirrorPlayerScene = hasMirrorPlayerScene;
					dynamicStateRequest.mirrorPlayerSceneView = &mirrorPlayerSceneView;
					dynamicStateRequest.mirrorPlayerGeometry = &mirrorPlayerGeometry;
					dynamicStateRequest.mirrorPlayerMaterials = &mirrorPlayerMaterialBridge;
					dynamicStateRequest.totalMs = &mLastPerfShellTraceStats.sceneSelectStateCommitDynamicStateMs;
					dynamicStateRequest.dynamicCoreMs = &mLastPerfShellTraceStats.sceneSelectStateCommitDynamicCoreMs;
					dynamicStateRequest.mirrorExtendedMs = &mLastPerfShellTraceStats.sceneSelectStateCommitDynamicMirrorExtendedMs;
					dynamicStateRequest.mirrorPlayerMs = &mLastPerfShellTraceStats.sceneSelectStateCommitDynamicMirrorPlayerMs;
					const NRISceneFrameDynamicStateInputs dynamicStateInputs =
						MakeNRISceneFrameDynamicStateInputs(dynamicStateRequest);
					mDynamicSceneLastFrame = BuildNRISceneFrameDynamicState(dynamicStateInputs, mDynamicSceneLastFrame, mLastPerfShellTraceStats);
				}
				{
					NRISceneFrameGeometrySelectionInputs geometrySelectionInputs = {};
					geometrySelectionInputs.staticBuildSerial = mStaticMapScene.buildSerial;
					geometrySelectionInputs.staticGeometry = &mStaticMapScene.geometry;
					geometrySelectionInputs.staticMaterialBridge = &mStaticMapScene.materialBridge;
					geometrySelectionInputs.staticGpuMaterials = &mStaticMapScene.gpuMaterials;
					geometrySelectionInputs.overlayGeometry = &overlayGeometry;
					geometrySelectionInputs.overlayMaterialOffset = combinedOverlayMaterialOffset;
					geometrySelectionInputs.combinedMaterialBridge = &combinedMaterialBridge;
					geometrySelectionInputs.combinedGpuMaterials = &combinedGpuMaterials;
					geometrySelectionInputs.totalMs = &mLastPerfShellTraceStats.sceneSelectStateCommitGeometryStateMs;
					geometrySelectionInputs.staticCopyMs = &mLastPerfShellTraceStats.sceneSelectStateCommitGeometryStaticCopyMs;
					geometrySelectionInputs.overlayAppendMs = &mLastPerfShellTraceStats.sceneSelectStateCommitGeometryAppendMs;
					geometrySelectionInputs.selectMs = &mLastPerfShellTraceStats.sceneSelectStateCommitGeometrySelectMs;
					const NRISceneFrameGeometrySelection geometrySelection = mSceneFrameGeometry.SelectActiveGeometry(geometrySelectionInputs);
					if (geometrySelection.usedCombinedGeometry)
					{
						mLastPerfShellTraceStats.sceneSelectStateCommitGeometryCombined = 1;
					}
					if (geometrySelection.usedStaticOnlyGeometry)
					{
						mLastPerfShellTraceStats.sceneSelectStateCommitGeometryStaticOnly = 1;
					}
					activeStaticProbePrimitiveCount = geometrySelection.staticProbePrimitiveCount;
					activeGeometry = geometrySelection.geometry;
					activeGpuMaterials = geometrySelection.gpuMaterials;
					activeMaterialBridge = geometrySelection.materialBridge;
					mLastPerfShellTraceStats.sceneSelectStateCommitCombinedPrimitiveCount = geometrySelection.combinedPrimitiveCount;
					mLastPerfShellTraceStats.sceneSelectStateCommitCombinedMaterialCount = geometrySelection.combinedMaterialCount;
				}

				{
					ScopedPtPerfTimer statsTimer(mLastPerfShellTraceStats.sceneSelectStateCommitStatsMs);
					nri_scene::SceneDebugStats persistentVoxelOverlayStats;
					if (hasPersistentVoxelOverlay)
					{
						ScopedPtPerfTimer persistentVoxelStatsTimer(mLastPerfShellTraceStats.sceneSelectStateCommitStatsPersistentVoxelMs);
						persistentVoxelOverlayStats = mPersistentVoxels.BuildOverlayDebugStats();
					}
					NRISceneFrameDebugStatsBuildRequest debugStatsRequest = {};
					debugStatsRequest.staticMapStats = &mStaticMapScene.sceneView.stats;
					debugStatsRequest.deferOverlayThisFrame = deferOverlayThisFrame;
					debugStatsRequest.deferredDynamicSceneView = &dynamicSceneView;
					debugStatsRequest.activeDynamicSceneView = activeDynamicSceneView;
					debugStatsRequest.persistentVoxelStats = hasPersistentVoxelOverlay ? &persistentVoxelOverlayStats : nullptr;
					debugStatsRequest.hasMirrorExtendedDynamicScene = hasMirrorExtendedDynamicScene;
					debugStatsRequest.mirrorExtendedSceneView = &mirrorExtendedDynamicSceneView;
					debugStatsRequest.hasMirrorPlayerScene = hasMirrorPlayerScene;
					debugStatsRequest.mirrorPlayerSceneView = &mirrorPlayerSceneView;
					debugStatsRequest.baseMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsBaseMs;
					debugStatsRequest.persistentVoxelMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsPersistentVoxelMs;
					debugStatsRequest.mirrorExtendedMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsMirrorExtendedMs;
					debugStatsRequest.mirrorPlayerMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsMirrorPlayerMs;
					debugStatsRequest.mergeMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsMergeMs;
					const NRISceneFrameDebugStatsInputs debugStatsInputs =
						MakeNRISceneFrameDebugStatsInputs(debugStatsRequest);
					activeStats = BuildNRISceneFrameDebugStats(debugStatsInputs, mLastPerfShellTraceStats);
				}

				{
					NRISceneFrameGenerationBuildRequest generationRequest = {};
					generationRequest.staticMapBuildSerial = mStaticMapScene.buildSerial;
					generationRequest.runtimeMutationGeneration = mRuntimeMutation.BuildFrameGenerationHash(hasRuntimeMutationOverlay);
					generationRequest.persistentVoxelGeneration = hasPersistentVoxelOverlay ? mPersistentVoxels.BuildSceneGenerationHash() : 0ull;
					generationRequest.frameIndex = mFrameIndex;
					generationRequest.staticAccelerationBuildSerial = mStaticAccelerationBuildSerial;
					generationRequest.renderWidth = mRenderWidth;
					generationRequest.renderHeight = mRenderHeight;
					generationRequest.currentCameraPos = mCurrentCameraPos;
					generationRequest.currentCameraForward = mCurrentCameraForward;
					generationRequest.currentCameraRight = mCurrentCameraRight;
					generationRequest.currentCameraUp = mCurrentCameraUp;
					generationRequest.currentTanHalfFovX = mCurrentTanHalfFovX;
					generationRequest.currentTanHalfFovY = mCurrentTanHalfFovY;
					generationRequest.selectedSceneHasDynamicOverlay = selectedSceneHasDynamicOverlay;
					generationRequest.activeDynamicSceneView = activeDynamicSceneView;
					generationRequest.activeDynamicGeometry = activeDynamicGeometry;
					generationRequest.activeDynamicMaterials = activeDynamicMaterials;
					generationRequest.hasMirrorPlayerScene = hasMirrorPlayerScene;
					generationRequest.mirrorPlayerGeometry = &mirrorPlayerGeometry;
					generationRequest.mirrorPlayerMaterials = &mirrorPlayerMaterialBridge;
					generationRequest.activeMaterialBridge = activeMaterialBridge;
					generationRequest.activeGpuMaterials = activeGpuMaterials;
					generationRequest.sceneTextureCacheCount = mSceneTextures.CacheCount();
					generationRequest.selectedTlasInstanceCount = selectedTlasInstanceCount;
					generationRequest.selectedSceneInstanceCount = selectedSceneInstanceCount;
					generationRequest.selectedStaticSceneInstanceCount = selectedStaticSceneInstanceCount;
					generationRequest.selectedDynamicSceneInstanceCount = selectedDynamicSceneInstanceCount;
					generationRequest.selectedPersistentVoxelSceneInstanceCount = selectedPersistentVoxelSceneInstanceCount;
					const NRISceneFrameGenerationInputs generationInputs =
						MakeNRISceneFrameGenerationInputs(generationRequest);
					const NRISceneFrameGenerationResult generationResult =
						BuildNRISceneFrameGenerationResult(generationInputs, mLastStateCommitDomainGenerations, mHasLastStateCommitDomainGenerations);
					WriteNRISceneFrameGenerationTraceStats(generationResult, mLastPerfShellTraceStats);
					mLastStateCommitDomainGenerations = generationResult.current;
					mHasLastStateCommitDomainGenerations = true;
				}
			}
			else
			{
				LogFallback("PT runtime/dynamic overlay update failed; tracing the resident static world only.");
				if (mGpuSceneHasDynamicOverlay)
				{
					RestoreStaticTopLevelScene();
				}
				paletteReady = true;
				texturesReady = true;
				buffersReady = true;
				accelerationReady = true;
			}
		}
		else if (mGpuSceneHasDynamicOverlay || residentStaticWorldGeometryChanged)
		{
			if (!RestoreStaticTopLevelScene())
			{
				LogFallback("PT static scene restore failed after dynamic overlay or resident chunk rebuild.");
				if (preserveHistory)
				{
					RestoreRenderSceneHistorySnapshot(history);
				}
				return false;
			}

			mGpuSceneHasDynamicOverlay = false;
			mUsedStaticMapSceneLastFrame = true;
			activeSceneView = &mStaticMapScene.sceneView;
			activeGeometry = &mStaticMapScene.geometry;
			activeGpuMaterials = &mStaticMapScene.gpuMaterials;
			activeMaterialBridge = &mStaticMapScene.materialBridge;
			activeStats = mStaticMapScene.sceneView.stats;
		}
		else if (deferOverlayThisFrame)
		{
			Printf("NRI PT dynamic scene deferred: skipping non-map dynamic overlay on the same frame that rebuilt resident static map assets.\n");
		}
		else
		{
			mGpuSceneHasDynamicOverlay = false;
		}
	}
	else
	{
		ResetPersistentDynamicEmissiveCache();
		Clocker clock(NriPTSceneCapture);
		if (!nri_scene::CaptureScene(di, capturedSceneView))
		{
			LogFallback("PT scene capture failed.");
			if (preserveHistory)
			{
				RestoreRenderSceneHistorySnapshot(history);
			}
			return false;
		}

		activeSceneView = &capturedSceneView;
		activeMaterialBridge = &materialBridge;
		sceneLightCapturedView = &capturedSceneView;
		activeStats = capturedSceneView.stats;

		{
			Clocker clock(NriPTGeometryBuild);
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildCapturedMs);
			nri_scene::BuildGeometry(capturedSceneView, capturedGeometry);
			AssignGeometryPortalIndices(mMapWorld, capturedGeometry);
		}

		{
			Clocker clock(NriPTMaterialBuild);
			BuildMaterialsWithActorOverrides(capturedSceneView, materialBridge, "captured_scene");
		}
		sceneLightCapturedMaterials = &materialBridge;

		const bool needsFallbackMaterials = bootstrapCapturedDiagnostics || bootstrapCapturedFlat;
		const bool needsRealTextures = !nri_ptbootstrap || bootstrapCapturedBaseColor || bootstrapMode >= 13u;
		paletteReady = needsRealTextures ? EnsurePaletteTexture(materialBridge) : true;
		texturesReady = needsFallbackMaterials ? UseFallbackSceneTextures(preserveHistory, "captured_scene_fallback") : (needsRealTextures ? (paletteReady && EnsureSceneTextures(capturedSceneView, materialBridge, capturedGpuMaterials, preserveHistory, "captured_scene")) : EnsureSkyTexture(capturedSceneView, preserveHistory));
		if (needsFallbackMaterials)
		{
			capturedGpuMaterials = materialBridge.materials;
			for (auto& material : capturedGpuMaterials)
			{
				material.textureIndex = 0;
				material.paletteIndex = 0;
				material.flags = 0;
				material.normalTextureIndex = UINT32_MAX;
				material.metallicTextureIndex = UINT32_MAX;
				material.roughnessTextureIndex = UINT32_MAX;
				material.emissiveTextureIndex = UINT32_MAX;
				material.lightLevel = 1.0f;
				material.alpha = 1.0f;
			}
		}
		else if (!needsRealTextures)
		{
			capturedGpuMaterials = materialBridge.materials;
		}

		buffersReady = texturesReady && UploadSceneBuffers(capturedGeometry, capturedGpuMaterials);
		auto& sceneInstances = mSelectCapturedSceneInstanceScratch;
		sceneInstances.clear();
		if (buffersReady)
		{
			sceneInstances.push_back({ 0u, nri_diag::SceneDataSourceDynamic, 0u, UINT32_MAX });
			buffersReady = NRISceneUploadManager::UpdateSceneDataSet(*this,
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				sceneInstances,
				0u,
				(uint32_t)capturedGeometry.primitives.size(),
				0u,
				(uint32_t)capturedGpuMaterials.size(),
				"captured_scene");
		}
		if (texturesReady)
		{
			PrepareSceneTextureInputsForCompute();
		}
		if (bootstrapCapturedView || rawTraceDirectScene)
		{
			accelerationReady = true;
		}
		else if (buffersReady)
		{
			NRIAccelerationStructureResource& dynamicBottomLevelAS = GetCurrentDynamicBottomLevelAS();
			accelerationReady =
				BuildDynamicAccelerationStructure(capturedGeometry) &&
				dynamicBottomLevelAS.accelerationStructure != nullptr;
			if (accelerationReady)
			{
				nri::TopLevelInstance instance = {};
				instance.transform[0][0] = 1.0f;
				instance.transform[1][1] = 1.0f;
				instance.transform[2][2] = 1.0f;
				instance.instanceId = 0;
				instance.mask = 0xFF;
				instance.shaderBindingTableLocalOffset = 0;
				instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
				instance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*dynamicBottomLevelAS.accelerationStructure);

				auto& instances = mSelectCapturedTopLevelInstanceScratch;
				instances.clear();
				instances.push_back(instance);
				accelerationReady = BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Dynamic);
			}
		}
		else
		{
			accelerationReady = false;
		}
		activeGeometry = &capturedGeometry;
		activeGpuMaterials = &capturedGpuMaterials;
		emissiveSamplingContext.capturedGeometry = &capturedGeometry;
		}
	}

	if (activeSceneView == nullptr || activeGeometry == nullptr || activeGpuMaterials == nullptr || activeMaterialBridge == nullptr)
	{
		LogFallback("PT scene selection failed.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}

	RefreshSceneLightSystem(
		sceneLightUsesStaticMapScene,
		sceneLightCapturedView,
		sceneLightCapturedMaterials,
		sceneLightDynamicView,
		sceneLightDynamicMaterials,
		appendPersistentVoxelSceneLights);

	bool refreshedSceneDataAfterLightRebuild = false;
	if (mGpuSceneHasDynamicOverlay &&
		activeMaterialBridge == &combinedMaterialBridge &&
		!overlayGeometry.primitives.empty())
	{
		refreshedCombinedGpuMaterials = combinedMaterialBridge.materials;
		ApplyEmissiveMaterialOverrides(combinedMaterialBridge, refreshedCombinedGpuMaterials);
		ApplyActorShadowMaterialOverrides(combinedMaterialBridge, refreshedCombinedGpuMaterials);
		if (!nri_material_policy::MaterialDataVectorEqual(refreshedCombinedGpuMaterials, combinedGpuMaterials))
		{
			const size_t staticMaterialCount = mStaticMapScene.gpuMaterials.size();
			if (refreshedCombinedGpuMaterials.size() < staticMaterialCount)
			{
				LogFallback("PT runtime overlay material refresh produced an invalid material slice.");
				if (preserveHistory)
				{
					RestoreRenderSceneHistorySnapshot(history);
				}
				return false;
			}

			combinedGpuMaterials.swap(refreshedCombinedGpuMaterials);
			dynamicGpuMaterials.assign(combinedGpuMaterials.begin() + staticMaterialCount, combinedGpuMaterials.end());
			if (!UploadSceneBuffers(overlayGeometry, dynamicGpuMaterials) ||
				!NRISceneUploadManager::UpdateSceneDataSet(*this,
					mStaticVertexBuffer,
					mStaticIndexBuffer,
					mStaticPrimitiveBuffer,
					mStaticMaterialBuffer,
					GetCurrentDynamicVertexBuffer(),
					GetCurrentDynamicIndexBuffer(),
					GetCurrentDynamicPrimitiveBuffer(),
					GetCurrentDynamicMaterialBuffer(),
					mBoundSceneInstances,
					(uint32_t)mStaticMapScene.geometry.primitives.size(),
					(uint32_t)overlayGeometry.primitives.size(),
					(uint32_t)mStaticMapScene.gpuMaterials.size(),
					(uint32_t)dynamicGpuMaterials.size(),
					"resident_overlay_material_refresh"))
			{
				LogFallback("PT runtime overlay material refresh failed after scene-light rebuild.");
				if (preserveHistory)
				{
					RestoreRenderSceneHistorySnapshot(history);
				}
				return false;
			}

			activeGpuMaterials = &combinedGpuMaterials;
			refreshedSceneDataAfterLightRebuild = true;
		}
	}

	if (mRuntimeLightSceneDataDirty && !refreshedSceneDataAfterLightRebuild)
	{
		if (mGpuSceneHasDynamicOverlay)
		{
			if (!NRISceneUploadManager::UpdateSceneDataSet(*this,
				mStaticVertexBuffer,
				mStaticIndexBuffer,
				mStaticPrimitiveBuffer,
				mStaticMaterialBuffer,
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				mBoundSceneInstances,
				(uint32_t)mStaticMapScene.geometry.primitives.size(),
				(uint32_t)overlayGeometry.primitives.size(),
				(uint32_t)mStaticMapScene.gpuMaterials.size(),
				(uint32_t)dynamicGpuMaterials.size(),
				"runtime_overlay_light_refresh"))
			{
				LogFallback("PT runtime overlay light refresh failed after scene-light rebuild.");
				if (preserveHistory)
				{
					RestoreRenderSceneHistorySnapshot(history);
				}
				return false;
			}
		}
		else if (!sceneLightUsesStaticMapScene)
		{
			if (!NRISceneUploadManager::UpdateSceneDataSet(*this,
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				mBoundSceneInstances,
				0u,
				(uint32_t)capturedGeometry.primitives.size(),
				0u,
				(uint32_t)capturedGpuMaterials.size(),
				"captured_scene_light_refresh"))
			{
				LogFallback("PT captured scene light refresh failed after scene-light rebuild.");
				if (preserveHistory)
				{
					RestoreRenderSceneHistorySnapshot(history);
				}
				return false;
			}
		}
	}

	if (sceneLightUsesStaticMapScene && !mGpuSceneHasDynamicOverlay)
	{
		const bool needsResidentStaticLightRefresh =
			mRuntimeLightSceneDataDirty ||
			!mSceneLights.GetAnalyticLights().activeLights.empty() ||
			mBoundRuntimeLightCount != 0 ||
			mSceneLights.GetSectorLighting().activeSectorCount > 0 ||
			mBoundSectorLightActiveCount != 0;
		if (needsResidentStaticLightRefresh)
		{
			if (!RefreshResidentStaticSceneDataSet())
			{
				LogFallback("PT static scene light refresh failed.");
				if (preserveHistory)
				{
					RestoreRenderSceneHistorySnapshot(history);
				}
				return false;
			}
		}
	}

	if (!UpdateEmissiveSamplingBuffers(emissiveSamplingContext))
	{
		LogFallback("PT emissive primitive update failed.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}
	if (!BuildEmissiveTopLevelAccelerationStructure())
	{
		LogFallback("PT emissive TLAS update failed.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}

	TraceRuntimeLinkEvents(di);
	LogBridgeStats(activeStats);
	if (activeStats.unsupportedModelDrawItems > 0)
	{
		LogFallback("generic GLDL_MODELS content is unsupported in the PT bridge; rendering the supported PT scene without those model draws.");
	}

	Copy3(activeSceneView->skyColor, mSkyColor);
	Copy3(activeSceneView->groundColor, mGroundColor);
	NRISceneSurfaceProbeFrameBuildRequest surfaceProbeFrameRequest = {};
	surfaceProbeFrameRequest.usesStaticMapScene = mUsedStaticMapSceneLastFrame;
	surfaceProbeFrameRequest.activeStaticProbePrimitiveCount = activeStaticProbePrimitiveCount;
	surfaceProbeFrameRequest.runtimeSpaceLinkGeometry = &runtimeSpaceLinkGeometry;
	surfaceProbeFrameRequest.runtimeMutationGeometry = &runtimeMutationFrame.geometry;
	surfaceProbeFrameRequest.overlayGeometry = &overlayGeometry;
	surfaceProbeFrameRequest.activeDynamicGeometry = activeDynamicGeometry;
	const NRISceneSurfaceProbeFrameInputs surfaceProbeFrameInputs =
		MakeNRISceneSurfaceProbeFrameInputs(surfaceProbeFrameRequest);
	mSurfaceProbeFrame = BuildNRISceneSurfaceProbeFrameState(surfaceProbeFrameInputs);

	if (!preserveHistory)
	{
		UpdateSurfaceProbe(*activeGeometry, activeMaterialBridge, true);
	}
	if (activeGeometry->primitives.empty())
	{
		LogFallback("PT scene path produced no supported opaque geometry.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}

	if (mUsedStaticMapSceneLastFrame)
	{
		PrepareSceneTextureInputsForCompute();
	}


	return true;
}

bool NRIRenderer::RenderScene(HWDrawInfo& di, int drawmode, bool portal)
{
	if ((drawmode != DM_MAINVIEW && drawmode != DM_OFFSCREEN) || portal || mFrameBuffer == nullptr ||
		mFrameBuffer->mCommandBuffer == nullptr || mFrameBuffer->mActiveTarget == nullptr)
	{
		return false;
	}

	if (!mPathTracingSupported)
	{
		LogFallback(GetAvailabilityReason());
		return false;
	}

	ResetPerfTraceStats();
	ScopedPtPerfTimer totalPerfTimer(mLastPerfShellTraceStats.totalMs);
	Clocker totalClock(NriPTAll);

	const uint32_t bootstrapMode = GetBootstrapMode();
	const bool bootstrapSimpleView = nri_ptbootstrap && bootstrapMode <= 3u;
	const bool bootstrapCapturedView = nri_ptbootstrap && bootstrapMode >= 4u && bootstrapMode <= 12u;
	const bool bootstrapCapturedDiagnostics = nri_ptbootstrap && bootstrapMode >= 4u && bootstrapMode <= 10u;
	const bool bootstrapCapturedFlat = nri_ptbootstrap && bootstrapMode == 11u;
	const bool bootstrapCapturedBaseColor = nri_ptbootstrap && bootstrapMode == 12u;
	const bool rawTraceDirectScene = !nri_ptbootstrap && nri_ptdirectscene;
	const int debugMode = (int)nri_ptdebug;

	const bool preserveHistory = drawmode != DM_MAINVIEW;
	const NRIRendererFrameContext frameContext = BuildFrameContext(drawmode, portal, debugMode, preserveHistory);
	const uint32_t traceFrameIndex = frameContext.frameIndex;
	const RenderSceneHistorySnapshot history = CaptureRenderSceneHistorySnapshot(preserveHistory);
	if (!EnsureRenderSceneFrameResources(frameContext, preserveHistory, history) ||
		!BeginRenderSceneFrame(di, frameContext, preserveHistory, history))
	{
		return false;
	}

	if (bootstrapSimpleView)
	{
		return RenderSimpleBootstrapView(preserveHistory, history);
	}

	RenderSceneFrameBuildInputs sceneFrameInputs = {};
	sceneFrameInputs.bootstrapMode = bootstrapMode;
	sceneFrameInputs.bootstrapCapturedView = bootstrapCapturedView;
	sceneFrameInputs.bootstrapCapturedDiagnostics = bootstrapCapturedDiagnostics;
	sceneFrameInputs.bootstrapCapturedFlat = bootstrapCapturedFlat;
	sceneFrameInputs.bootstrapCapturedBaseColor = bootstrapCapturedBaseColor;
	sceneFrameInputs.rawTraceDirectScene = rawTraceDirectScene;
	sceneFrameInputs.preserveHistory = preserveHistory;
	RenderSceneFrameBuildResult sceneFrame;
	if (!BuildRenderSceneFrame(di, sceneFrameInputs, history, sceneFrame))
	{
		return false;
	}
	RenderSceneDispatchInputs dispatchInputs = {};
	dispatchInputs.bootstrapCapturedView = bootstrapCapturedView;
	dispatchInputs.buffersReady = sceneFrame.buffersReady;
	dispatchInputs.accelerationReady = sceneFrame.accelerationReady;
	dispatchInputs.drawInfo = &di;
	dispatchInputs.activeGeometry = sceneFrame.activeGeometry;
	dispatchInputs.activeGpuMaterials = sceneFrame.activeGpuMaterials;
	dispatchInputs.drawmode = drawmode;
	const bool dispatched = DispatchSelectedRenderScene(dispatchInputs);
	const bool success = sceneFrame.paletteReady && sceneFrame.texturesReady && sceneFrame.buffersReady && sceneFrame.accelerationReady && dispatched;
	LogRenderSceneFailureReasons(sceneFrame.paletteReady, sceneFrame.texturesReady, sceneFrame.buffersReady, sceneFrame.accelerationReady, dispatched, bootstrapCapturedView);

	RenderSceneCompletionInputs completionInputs = {};
	completionInputs.success = success;
	completionInputs.preserveHistory = preserveHistory;
	completionInputs.bootstrapCapturedView = bootstrapCapturedView;
	completionInputs.traceFrameIndex = traceFrameIndex;
	completionInputs.drawmode = drawmode;
	completionInputs.portal = portal;
	completionInputs.activeGeometry = sceneFrame.activeGeometry;
	completionInputs.activeGpuMaterials = sceneFrame.activeGpuMaterials;
	completionInputs.activeDynamicGeometry = sceneFrame.activeDynamicGeometry;
	completionInputs.usingPersistentDynamicEmissiveCache = sceneFrame.usingPersistentDynamicEmissiveCache;
	CommitRenderSceneResult(completionInputs, history);

	return success;
}
