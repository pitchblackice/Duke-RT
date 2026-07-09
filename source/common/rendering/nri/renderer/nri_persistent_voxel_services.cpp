#include "nri_persistent_voxel_services.h"
#include "nri_renderer.h"
#include "nri_cvars.h"
#include "nri_render_geometry_helpers.h"
#include "nri_voxel_compute_preload.h"
#include "nri_voxel_compute_meshing.h"
#include "../system/nri_renderdevice.h"
#include "hw_voxels.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "mapinfo.h"

#include <algorithm>

class NRIPersistentVoxelServiceFactory
{
public:
	static NRIPersistentVoxelResetServices BuildResetServices(NRIRenderer& renderer)
	{
		NRIPersistentVoxelResetServices services = {};
		services.user = &renderer;
		services.retireBuffer = [](void* user, NRIBufferResource& resource)
		{
			static_cast<NRIRenderer*>(user)->RetireResidentBufferResource(resource);
		};
		services.retireAccelerationStructure = [](void* user, NRIAccelerationStructureResource& resource)
		{
			static_cast<NRIRenderer*>(user)->RetireResidentAccelerationStructure(resource);
		};
		services.invalidateSceneDataDescriptors = [](void* user)
		{
			static_cast<NRIRenderer*>(user)->SetCurrentSceneDataDescriptorsInitialized(false);
		};
		return services;
	}

	static NRIPersistentVoxelAdmissionServices BuildAdmissionServices(NRIRenderer& renderer)
	{
		NRIPersistentVoxelAdmissionServices services = {};
		services.user = &renderer;
		services.admitVariantResource = [](
			void* user,
			PersistentVoxelAdmissionEntry& entry,
			uint64_t byteBudget,
			uint32_t& blasBudget,
			uint64_t& outUploadBytes,
			bool& outReusedMesh,
			bool& outReusedMaterial,
			bool& outInProgress,
			bool isolateBlasBuild,
			const char*& outFailureReason,
			PersistentVoxelAdmissionStats* outStats) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mPersistentVoxels.AdmitVariantResource(
				entry,
				byteBudget,
				blasBudget,
				outUploadBytes,
				outReusedMesh,
				outReusedMaterial,
				outInProgress,
				isolateBlasBuild,
				outFailureReason,
				outStats,
				renderer->mFrameIndex,
				(int)nri_ptloadingtrace,
				(bool)nri_voxelstats,
				BuildAdmissionServices(*renderer));
		};
		services.submitWaitAndRestart = [](void* user, const char* reason) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			if (renderer->mFrameBuffer == nullptr || !renderer->mFrameBuffer->SubmitWaitAndRestartCommandList(reason))
			{
				return false;
			}
			renderer->ResetResidentUploadScratchFrame(reason);
			return true;
		};
		services.isSubmitBudgetHit = [](void* user) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mFrameBuffer != nullptr && renderer->mFrameBuffer->IsPreloadSubmitBudgetHit();
		};
		services.retireBuffer = [](void* user, NRIBufferResource& resource)
		{
			static_cast<NRIRenderer*>(user)->RetireResidentBufferResource(resource);
		};
		services.retireAccelerationStructure = [](void* user, NRIAccelerationStructureResource& resource)
		{
			static_cast<NRIRenderer*>(user)->RetireResidentAccelerationStructure(resource);
		};
		services.buildMaterials = [](void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label)
		{
			Clocker materialClock(NriPTMaterialBuild);
			static_cast<NRIRenderer*>(user)->BuildMaterialsWithActorOverrides(sceneView, materials, label);
		};
		services.prewarmTexture = [](void* user, const nri_scene::TextureUpload& upload) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			if (upload.width == 0 || upload.height == 0)
			{
				return true;
			}
			if (renderer->mFrameBuffer != nullptr &&
				renderer->mFrameBuffer->mActiveCanvasSourceTexture != nullptr &&
				upload.sourceTexture == renderer->mFrameBuffer->mActiveCanvasSourceTexture)
			{
				return true;
			}
			if (upload.sourceTexture != nullptr && upload.sourceTexture->isHardwareCanvas())
			{
				return true;
			}
			return renderer->EnsureSceneTextureCacheEntry(upload);
		};
		services.assignGeometryPortalIndices = [](void* user, nri_scene::GeometryData& geometry)
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			AssignGeometryPortalIndices(renderer->mMapWorld, geometry);
		};
		services.createStructuredBufferNoUpload = [](void* user, NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			if (renderer->mFrameBuffer == nullptr ||
				!renderer->CreateBufferWithoutViewAtLocation(resource, size, stride, usage, nri::MemoryLocation::DEVICE))
			{
				return false;
			}
			nri::BufferViewDesc viewDesc = {};
			viewDesc.buffer = resource.buffer;
			viewDesc.type = nri::BufferView::STRUCTURED_BUFFER;
			viewDesc.offset = 0;
			viewDesc.size = nri::WHOLE_SIZE;
			viewDesc.structureStride = stride;
			if (renderer->mFrameBuffer->mCore.CreateBufferView(viewDesc, resource.shaderView) != nri::Result::SUCCESS)
			{
				renderer->DestroyBufferResource(resource);
				return false;
			}
			resource.usedSize = size;
			return true;
		};
		services.ensureArenaBuffer = [](void* user, NRIBufferResource& resource, uint64_t requiredSize, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after) -> bool
		{
			return static_cast<NRIRenderer*>(user)->EnsureResidentArenaBuffer(resource, requiredSize, stride, usage, after);
		};
		services.stageBufferCopyRange = [](void* user, NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after, int uploadKind) -> bool
		{
			return static_cast<NRIRenderer*>(user)->StageResidentBufferCopyRange(resource, byteOffset, data, size, after, uploadKind);
		};
		services.noteBufferUpload = [](void* user, int uploadKind, uint64_t size, const char* reason)
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			SceneBufferDebugStats* stats =
				uploadKind == ResidentUploadKind_Index ? &renderer->mIndexBufferStats :
				(uploadKind == ResidentUploadKind_Primitive ? &renderer->mPrimitiveBufferStats : &renderer->mVertexBufferStats);
			renderer->NotePerfBufferUpload(stats, size, false, reason, uploadKind);
		};
		services.buildBottomLevel = [](
			void* user,
			const NRIBufferResource& vertexBuffer,
			const NRIBufferResource& indexBuffer,
			uint32_t vertexOffset,
			uint32_t vertexCount,
			uint32_t indexOffset,
			uint32_t indexCount,
			uint32_t primitiveCount,
			NRIAccelerationStructureResource& outAccelerationStructure) -> bool
		{
			return static_cast<NRIRenderer*>(user)->BuildBottomLevelAccelerationStructure(
				vertexBuffer,
				indexBuffer,
				vertexOffset,
				vertexCount,
				indexOffset,
				indexCount,
				primitiveCount,
				outAccelerationStructure,
				false);
		};
		services.barrierBuildInputs = [](void* user, const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer) -> bool
		{
			return BarrierBuildInputs(static_cast<NRIRenderer&>(*static_cast<NRIRenderer*>(user)), vertexBuffer, indexBuffer);
		};
		services.barrierComputeToBuildInputs = [](void* user, const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer) -> bool
		{
			return BarrierComputeToBuildInputs(static_cast<NRIRenderer&>(*static_cast<NRIRenderer*>(user)), vertexBuffer, indexBuffer);
		};
		return services;
	}

	static NRIPersistentVoxelAccelerationServices BuildAccelerationServices(NRIRenderer& renderer)
	{
		NRIPersistentVoxelAccelerationServices services = {};
		services.user = &renderer;
		services.buildBottomLevel = [](
			void* user,
			const NRIBufferResource& vertexBuffer,
			const NRIBufferResource& indexBuffer,
			uint32_t vertexOffset,
			uint32_t vertexCount,
			uint32_t indexOffset,
			uint32_t indexCount,
			uint32_t primitiveCount,
			NRIAccelerationStructureResource& outAccelerationStructure) -> bool
		{
			return static_cast<NRIRenderer*>(user)->BuildBottomLevelAccelerationStructure(
				vertexBuffer,
				indexBuffer,
				vertexOffset,
				vertexCount,
				indexOffset,
				indexCount,
				primitiveCount,
				outAccelerationStructure,
				false);
		};
		services.barrierBuildInputs = [](void* user, const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer) -> bool
		{
			return BarrierBuildInputs(static_cast<NRIRenderer&>(*static_cast<NRIRenderer*>(user)), vertexBuffer, indexBuffer);
		};
		return services;
	}

	static bool PreloadResources(NRIRenderer& renderer)
	{
		std::vector<nri_scene::PrecachedVoxelVariantView> variants;
		std::vector<nri_scene::PrecachedVoxelRawManifestView> rawVariants;
		nri_scene::PrecachedVoxelRawManifestStats rawManifestStats = {};
		std::vector<nri_scene::PersistentVoxelCacheEntryView> cacheEntries;
		const bool gpuLoadingEnabled = (bool)nri_ptloadingvoxelgpu;
		bool hasCacheEntries = false;
		nri_scene::PreloadLiveActorVoxelRawSources();
		const NRIVoxelComputePreloadSettings computePreloadSettings = BuildNRIVoxelComputePreloadSettingsFromCVars();
		const bool computePreloadPlanningEnabled =
			computePreloadSettings.enabled || computePreloadSettings.traceLevel >= 1 || (int)nri_ptloadingtrace >= 1;
		if (computePreloadPlanningEnabled)
		{
			nri_scene::BuildPrecachedVoxelRawManifestViews(rawVariants, &rawManifestStats);
		}
		if (gpuLoadingEnabled)
		{
			nri_scene::BuildPrecachedVoxelVariantViews(variants);
			hasCacheEntries = nri_scene::BuildPersistentVoxelCacheEntries(cacheEntries);
		}

		struct ComputePreloadTimeline
		{
			uint64_t buildSerial = 0;
			uint32_t maxRawVariants = 0;
			uint32_t maxLegacyVariants = 0;
		};
		static ComputePreloadTimeline sComputePreloadTimeline = {};
		const bool newTimeline = sComputePreloadTimeline.buildSerial != renderer.mMapWorld.buildSerial;
		if (newTimeline)
		{
			sComputePreloadTimeline = {};
			sComputePreloadTimeline.buildSerial = renderer.mMapWorld.buildSerial;
		}
		const bool improvedTimeline =
			(uint32_t)rawVariants.size() > sComputePreloadTimeline.maxRawVariants ||
			(uint32_t)variants.size() > sComputePreloadTimeline.maxLegacyVariants;
		const bool shouldPlanComputePreload =
			computePreloadPlanningEnabled &&
			(newTimeline || improvedTimeline || computePreloadSettings.traceLevel >= 2);
		if (shouldPlanComputePreload)
		{
			const char* timelineStage = newTimeline ? "first" : (improvedTimeline ? "max" : "progress");
			PlanNRIVoxelComputePreload(
				variants,
				rawVariants,
				rawManifestStats,
				renderer.mPersistentVoxels,
				computePreloadSettings,
				renderer.mMapWorld.level != nullptr ? renderer.mMapWorld.level->labelName.GetChars() : nullptr,
				renderer.mMapWorld.buildSerial,
				renderer.mFrameIndex,
				timelineStage);
			sComputePreloadTimeline.maxRawVariants = std::max(sComputePreloadTimeline.maxRawVariants, (uint32_t)rawVariants.size());
			sComputePreloadTimeline.maxLegacyVariants = std::max(sComputePreloadTimeline.maxLegacyVariants, (uint32_t)variants.size());
		}
		if (computePreloadSettings.enabled && !computePreloadSettings.dryRun)
		{
			std::vector<nri_scene::PrecachedVoxelVariantView> directPreloadVariants;
			BuildNRIVoxelComputePreloadDirectVariants(rawVariants, computePreloadSettings, directPreloadVariants);
			if (!directPreloadVariants.empty())
			{
				if ((int)nri_ptloadingtrace >= 1 || computePreloadSettings.traceLevel >= 1 || (int)nri_ptvoxelcomputetrace >= 1)
				{
					Printf("NRI PT voxel compute preload: event=admit-source level=%s build_serial=%llu frame=%u direct_variants=%u legacy_variants=%u dry_run=0\n",
						renderer.mMapWorld.level != nullptr ? renderer.mMapWorld.level->labelName.GetChars() : "unknown",
						(unsigned long long)renderer.mMapWorld.buildSerial,
						renderer.mFrameIndex,
						(uint32_t)directPreloadVariants.size(),
						(uint32_t)variants.size());
				}
				variants.insert(variants.end(), directPreloadVariants.begin(), directPreloadVariants.end());
			}
		}

		const NRIPersistentVoxelSettings persistentVoxelSettings = BuildNRIPersistentVoxelSettingsFromCVars();
		if (gpuLoadingEnabled)
		{
			renderer.ResetResidentUploadScratchFrame("voxel-preload-start");
		}
		NRIPersistentVoxelPreloadServices preloadServices = {};
		preloadServices.user = &renderer;
		preloadServices.pumpAdmissionQueue = [](void* user, const char* phase) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			const NRIPersistentVoxelSettings settings = BuildNRIPersistentVoxelSettingsFromCVars();
			const NRIRenderer::MemoryTelemetry telemetry = renderer->GetMemoryTelemetry();
			return renderer->mPersistentVoxels.PumpAdmissionQueue(
				phase,
				renderer->mMapWorld.buildSerial,
				renderer->mFrameIndex,
				settings,
				telemetry.totalTrackedBytes,
				renderer->mFrameBuffer != nullptr ? renderer->mFrameBuffer->GetAdapterLocalBudgetBytes() : 0ull,
				(int)nri_ptloadingtrace,
				(bool)nri_voxelstats,
				BuildResetServices(*renderer),
				BuildAdmissionServices(*renderer));
		};
		preloadServices.pumpComputeJobs = [](void* user, uint32_t frameIndex) -> void
		{
			DispatchNRIVoxelComputeMeshingDiagnostics(*static_cast<NRIRenderer*>(user), frameIndex);
		};
		preloadServices.ensureBatch = [](void* user, NRIPersistentVoxelBatchStats* outStats) -> bool
		{
			return EnsureBatch(static_cast<NRIRenderer&>(*static_cast<NRIRenderer*>(user)), outStats);
		};
		preloadServices.warmSharedBlas = [](void* user, const std::vector<nri_scene::PrecachedVoxelVariantView>& variants, uint32_t frameIndex) -> bool
		{
			NRIRenderer& renderer = *static_cast<NRIRenderer*>(user);
			if (renderer.mPersistentVoxels.HasRenderableOverlay())
			{
				NRIPersistentVoxelAccelerationBuildStats batchAccelerationStats = {};
				if (!renderer.mPersistentVoxels.BuildAccelerationStructures(
						frameIndex,
						BuildNRIPersistentVoxelSettingsFromCVars(),
						(bool)nri_voxelstats,
						BuildResetServices(renderer),
						BuildAccelerationServices(renderer),
						batchAccelerationStats))
				{
					return false;
				}
				if ((int)nri_ptloadingtrace >= 1)
				{
					Printf("NRI PT loading voxel acceleration: event=warmup active_actors=%u calls=%u builds=%u unique_mesh_builds=%u\n",
						batchAccelerationStats.instances,
						batchAccelerationStats.calls,
						batchAccelerationStats.builds,
						batchAccelerationStats.uniqueMeshBuilds);
				}
			}
			return renderer.mPersistentVoxels.WarmSharedBlasForLoading(
				variants,
				frameIndex,
				BuildNRIPersistentVoxelSettingsFromCVars(),
				(int)nri_ptloadingtrace,
				(bool)nri_voxelstats,
				BuildResetServices(renderer),
				BuildAccelerationServices(renderer));
		};
		preloadServices.isSubmitBudgetHit = [](void* user) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mFrameBuffer != nullptr && renderer->mFrameBuffer->IsPreloadSubmitBudgetHit();
		};
		return renderer.mPersistentVoxels.PreloadResources(
			variants,
			cacheEntries,
			hasCacheEntries,
			gpuLoadingEnabled,
			renderer.mMapWorld.buildSerial,
			renderer.mMapWorld.level != nullptr ? renderer.mMapWorld.level->labelName.GetChars() : nullptr,
			renderer.mFrameIndex,
			persistentVoxelSettings,
			(int)nri_ptloadingtrace,
			(bool)nri_voxelstats,
			BuildResetServices(renderer),
			preloadServices);
	}

	static bool EnsureBatch(NRIRenderer& renderer, NRIPersistentVoxelBatchStats* outStats = nullptr)
	{
		NRIPersistentVoxelBatchServices batchServices = {};
		batchServices.user = &renderer;
		batchServices.buildMaterials = [](void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label)
		{
			static_cast<NRIRenderer*>(user)->BuildMaterialsWithActorOverrides(sceneView, materials, label);
		};
		batchServices.isTextureCached = [](void* user, const nri_scene::TextureUpload& upload) -> bool
		{
			return static_cast<NRIRenderer*>(user)->FindSceneTextureCacheIndex(upload.key) != UINT32_MAX;
		};
		batchServices.prewarmTexture = [](void* user, const nri_scene::TextureUpload& upload, double* outMs) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			if (upload.width == 0 || upload.height == 0)
			{
				return true;
			}
			if (renderer->mFrameBuffer != nullptr &&
				renderer->mFrameBuffer->mActiveCanvasSourceTexture != nullptr &&
				upload.sourceTexture == renderer->mFrameBuffer->mActiveCanvasSourceTexture)
			{
				return true;
			}
			if (upload.sourceTexture != nullptr && upload.sourceTexture->isHardwareCanvas())
			{
				return true;
			}
			return renderer->EnsureSceneTextureCacheEntry(upload, outMs);
		};
		batchServices.assignGeometryPortalIndices = [](void* user, nri_scene::GeometryData& geometry)
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			AssignGeometryPortalIndices(renderer->mMapWorld, geometry);
		};
		batchServices.ensureStructuredBuffer = [](void* user, NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, const char* reason, int uploadKind) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			SceneBufferDebugStats* stats =
				uploadKind == ResidentUploadKind_Index ? &renderer->mIndexBufferStats :
				(uploadKind == ResidentUploadKind_Primitive ? &renderer->mPrimitiveBufferStats : &renderer->mVertexBufferStats);
			return renderer->EnsureResidentStructuredBuffer(resource, *stats, data, size, stride, usage, after, reason, uploadKind);
		};
		batchServices.ensureArenaBuffer = [](void* user, NRIBufferResource& resource, uint64_t requiredSize, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after) -> bool
		{
			return static_cast<NRIRenderer*>(user)->EnsureResidentArenaBuffer(resource, requiredSize, stride, usage, after);
		};
		batchServices.stageBufferCopyRange = [](void* user, NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after, int uploadKind) -> bool
		{
			return static_cast<NRIRenderer*>(user)->StageResidentBufferCopyRange(resource, byteOffset, data, size, after, uploadKind);
		};
		batchServices.noteBufferUpload = [](void* user, int uploadKind, uint64_t size, const char* reason)
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			SceneBufferDebugStats* stats =
				uploadKind == ResidentUploadKind_Index ? &renderer->mIndexBufferStats :
				(uploadKind == ResidentUploadKind_Primitive ? &renderer->mPrimitiveBufferStats : &renderer->mVertexBufferStats);
			renderer->NotePerfBufferUpload(stats, size, false, reason, uploadKind);
		};
		batchServices.retireAccelerationStructure = [](void* user, NRIAccelerationStructureResource& resource)
		{
			static_cast<NRIRenderer*>(user)->RetireResidentAccelerationStructure(resource);
		};
		batchServices.materialWouldEmit = [](void* user, const nri_scene::MaterialLightingMetadata& metadata) -> bool
		{
			return static_cast<NRIRenderer*>(user)->mSceneLights.MaterialWouldEmit(metadata);
		};
		batchServices.buildSurfaceRecord = [](void* user, const nri_scene::SurfaceRef& surface, const nri_scene::MaterialBridgeData& materials, SceneLightRecordSource source, uint32_t materialIndex, uint32_t primitiveIndex) -> SceneLightSystem::SurfaceRecord
		{
			return static_cast<NRIRenderer*>(user)->mSceneLights.BuildSurfaceRecord(surface, materials, source, materialIndex, primitiveIndex);
		};

		NRIPersistentVoxelBatchStats batchStats = {};
		const bool result = renderer.mPersistentVoxels.EnsureBatch(
			renderer.mMapWorld.buildSerial,
			renderer.mFrameIndex,
			BuildNRIPersistentVoxelSettingsFromCVars(),
			(int)nri_ptloadingtrace,
			(bool)nri_voxelstats,
			BuildResetServices(renderer),
			batchServices,
			batchStats);
		if (outStats != nullptr)
		{
			*outStats = batchStats;
		}

		renderer.mLastPerfShellTraceStats.persistentVoxelBatchCacheEntryMs += batchStats.persistentVoxelBatchCacheEntryMs;
		renderer.mLastPerfShellTraceStats.persistentVoxelBatchSortMs += batchStats.persistentVoxelBatchSortMs;
		renderer.mLastPerfShellTraceStats.persistentVoxelBatchInstanceSyncMs += batchStats.persistentVoxelBatchInstanceSyncMs;
		renderer.mLastPerfShellTraceStats.persistentVoxelBatchExistingActorMapMs += batchStats.persistentVoxelBatchExistingActorMapMs;
		renderer.mLastPerfShellTraceStats.persistentVoxelBatchActorLoopMs += batchStats.persistentVoxelBatchActorLoopMs;
		renderer.mLastPerfShellTraceStats.persistentVoxelBatchMaterialVariantMs += batchStats.persistentVoxelBatchMaterialVariantMs;
		renderer.mLastPerfShellTraceStats.persistentVoxelBatchMeshAdmissionMs += batchStats.persistentVoxelBatchMeshAdmissionMs;
		renderer.mLastPerfShellTraceStats.persistentVoxelBatchMaterialBridgeMs += batchStats.persistentVoxelBatchMaterialBridgeMs;
		renderer.mLastPerfShellTraceStats.persistentVoxelBatchStateMs += batchStats.persistentVoxelBatchStateMs;
		renderer.mLastPerfShellTraceStats.geometryBuildPersistentVoxelVariantMs += batchStats.geometryBuildPersistentVoxelVariantMs;
		renderer.mLastPerfShellTraceStats.geometryBuildPersistentVoxelAppendMs += batchStats.geometryBuildPersistentVoxelAppendMs;
		renderer.mLastPerfShellTraceStats.geometryBuildPersistentVoxelRebuildMs += batchStats.geometryBuildPersistentVoxelRebuildMs;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmMs += batchStats.persistentVoxelTexturePrewarmMs;
		renderer.mLastPerfShellTraceStats.geometryBuildPersistentVoxelVariantCalls += batchStats.geometryBuildPersistentVoxelVariantCalls;
		renderer.mLastPerfShellTraceStats.geometryBuildPersistentVoxelVariantPrimitives += batchStats.geometryBuildPersistentVoxelVariantPrimitives;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmHitCount += batchStats.persistentVoxelTexturePrewarmHitCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmQueuedCount += batchStats.persistentVoxelTexturePrewarmQueuedCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmMissCount += batchStats.persistentVoxelTexturePrewarmMissCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmDeferredCount += batchStats.persistentVoxelTexturePrewarmDeferredCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmProcessedCount += batchStats.persistentVoxelTexturePrewarmProcessedCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmByteBudget = batchStats.persistentVoxelTexturePrewarmByteBudget;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmEstimatedBytes += batchStats.persistentVoxelTexturePrewarmEstimatedBytes;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmDeferredBytes += batchStats.persistentVoxelTexturePrewarmDeferredBytes;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmProcessedBytes += batchStats.persistentVoxelTexturePrewarmProcessedBytes;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingCandidateCount += batchStats.persistentVoxelOnboardingCandidateCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingDeferredCount += batchStats.persistentVoxelOnboardingDeferredCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingPrimitiveBudgetHits += batchStats.persistentVoxelOnboardingPrimitiveBudgetHits;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingByteBudgetHits += batchStats.persistentVoxelOnboardingByteBudgetHits;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingActorBudgetHits += batchStats.persistentVoxelOnboardingActorBudgetHits;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingAdmittedCount += batchStats.persistentVoxelOnboardingAdmittedCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingTextureBudgetHits += batchStats.persistentVoxelOnboardingTextureBudgetHits;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingAdmissionPendingCount += batchStats.persistentVoxelOnboardingAdmissionPendingCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingTexturePrewarmDeferredCount += batchStats.persistentVoxelOnboardingTexturePrewarmDeferredCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingMaterialInvalidCount += batchStats.persistentVoxelOnboardingMaterialInvalidCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingBudgetDeferredCount += batchStats.persistentVoxelOnboardingBudgetDeferredCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingEstimatedBytes += batchStats.persistentVoxelOnboardingEstimatedBytes;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingDeferredBytes += batchStats.persistentVoxelOnboardingDeferredBytes;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingAdmittedBytes += batchStats.persistentVoxelOnboardingAdmittedBytes;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingByteBudget = batchStats.persistentVoxelOnboardingByteBudget;
		renderer.mLastPerfShellTraceStats.persistentVoxelInstanceTransformUpdates += batchStats.persistentVoxelInstanceTransformUpdates;
		return result;
	}

private:
	static bool BarrierBuildInputs(NRIRenderer& renderer, const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer)
	{
		if (renderer.mFrameBuffer == nullptr || renderer.mFrameBuffer->mCommandBuffer == nullptr)
		{
			return false;
		}
		nri::BufferBarrierDesc inputBarriers[2] = {};
		inputBarriers[0].buffer = vertexBuffer.buffer;
		inputBarriers[0].before = NRIResourceAccelerationStructureBuildInputAccess();
		inputBarriers[0].after = NRIResourceComputeShaderResourceAccess();
		inputBarriers[1].buffer = indexBuffer.buffer;
		inputBarriers[1].before = NRIResourceAccelerationStructureBuildInputAccess();
		inputBarriers[1].after = NRIResourceComputeShaderResourceAccess();
		nri::BarrierDesc inputBarrierDesc = {};
		inputBarrierDesc.buffers = inputBarriers;
		inputBarrierDesc.bufferNum = 2;
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, inputBarrierDesc);
		return true;
	}

	static bool BarrierComputeToBuildInputs(NRIRenderer& renderer, const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer)
	{
		if (renderer.mFrameBuffer == nullptr || renderer.mFrameBuffer->mCommandBuffer == nullptr)
		{
			return false;
		}
		nri::BufferBarrierDesc inputBarriers[2] = {};
		inputBarriers[0].buffer = vertexBuffer.buffer;
		inputBarriers[0].before = NRIResourceComputeShaderResourceAccess();
		inputBarriers[0].after = NRIResourceAccelerationStructureBuildInputAccess();
		inputBarriers[1].buffer = indexBuffer.buffer;
		inputBarriers[1].before = NRIResourceComputeShaderResourceAccess();
		inputBarriers[1].after = NRIResourceAccelerationStructureBuildInputAccess();
		nri::BarrierDesc inputBarrierDesc = {};
		inputBarrierDesc.buffers = inputBarriers;
		inputBarrierDesc.bufferNum = 2;
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, inputBarrierDesc);
		return true;
	}
};

bool NRIRenderer::PreloadPersistentVoxelResources()
{
	return NRIPersistentVoxelServiceFactory::PreloadResources(*this);
}

bool NRIRenderer::EnsurePersistentVoxelBatch()
{
	return NRIPersistentVoxelServiceFactory::EnsureBatch(*this);
}

NRIPersistentVoxelResetServices BuildNRIPersistentVoxelResetServices(NRIRenderer& renderer)
{
	return NRIPersistentVoxelServiceFactory::BuildResetServices(renderer);
}

NRIPersistentVoxelAdmissionServices BuildNRIPersistentVoxelAdmissionServices(NRIRenderer& renderer)
{
	return NRIPersistentVoxelServiceFactory::BuildAdmissionServices(renderer);
}

NRIPersistentVoxelAccelerationServices BuildNRIPersistentVoxelAccelerationServices(NRIRenderer& renderer)
{
	return NRIPersistentVoxelServiceFactory::BuildAccelerationServices(renderer);
}
