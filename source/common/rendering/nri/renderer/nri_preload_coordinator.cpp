#include "nri_preload_coordinator.h"
#include "nri_renderer.h"
#include "nri_cvars.h"
#include "nri_frame_resources.h"
#include "nri_sky_environment.h"
#include "../system/nri_renderdevice.h"

#include "mapinfo.h"
#include "printf.h"

namespace
{
	double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	class ScopedPreloadPerfTimer
	{
	public:
		explicit ScopedPreloadPerfTimer(double& targetMs)
			: mTarget(&targetMs)
			, mStart(std::chrono::steady_clock::now())
		{
		}

		~ScopedPreloadPerfTimer()
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

bool NRIPreloadCoordinator::HasFrameTarget(NRIRenderer& renderer, const Context& context)
{
	const bool hasCommandBuffer = renderer.mFrameBuffer != nullptr && renderer.mFrameBuffer->HasCurrentCommandBuffer();
	const bool hasRequiredTarget = context.standaloneContextUsed || (renderer.mFrameBuffer != nullptr && renderer.mFrameBuffer->HasActiveTarget());
	if (!hasCommandBuffer || !hasRequiredTarget)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=wait reason=%s framebuffer=%u command_buffer=%u active_target=%u standalone_context=%u output=%ux%u target=%ux%u\n",
				!hasCommandBuffer ? "command-buffer-not-ready" : "frame-target-not-ready",
				renderer.mFrameBuffer != nullptr ? 1u : 0u,
				renderer.mFrameBuffer != nullptr && renderer.mFrameBuffer->HasCurrentCommandBuffer() ? 1u : 0u,
				renderer.mFrameBuffer != nullptr && renderer.mFrameBuffer->HasActiveTarget() ? 1u : 0u,
				context.standaloneContextUsed ? 1u : 0u,
				context.outputWidth,
				context.outputHeight,
				context.targetWidth,
				context.targetHeight);
		}
		return false;
	}
	return true;
}

bool NRIPreloadCoordinator::ShouldSkipForUnsupportedPathTracing(NRIRenderer& renderer, const Context& context)
{
	if (!renderer.RefreshPathTracingAvailability() || !renderer.mPathTracingSupported)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=pt-unsupported output=%ux%u target=%ux%u\n",
				context.outputWidth,
				context.outputHeight,
				context.targetWidth,
				context.targetHeight);
		}
		return true;
	}
	return false;
}

void NRIPreloadCoordinator::TraceBegin(NRIRenderer& renderer, const Context& context)
{
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading gate: event=renderer-preload result=begin output=%ux%u target=%ux%u map_valid=%u static_valid=%u static_resident=%u\n",
			context.outputWidth,
			context.outputHeight,
			context.targetWidth,
			context.targetHeight,
			renderer.mMapWorld.valid ? 1u : 0u,
			renderer.mStaticMapScene.valid ? 1u : 0u,
			renderer.mStaticMapScene.valid && renderer.mStaticMapScene.texturesResident && renderer.mStaticMapScene.buffersResident && renderer.mStaticMapScene.accelerationResident ? 1u : 0u);
	}
}

bool NRIPreloadCoordinator::EnsureFrameResources(NRIRenderer& renderer, const Context& context)
{
	renderer.ResetPerfTraceStats();
	{
		ScopedPreloadPerfTimer initPerfTimer(renderer.mLastPerfShellTraceStats.initResourcesMs);
		if (!renderer.Initialize() || !NRIFrameResources::EnsureFrameResources(renderer, context.outputWidth, context.outputHeight, context.targetWidth, context.targetHeight))
		{
			renderer.LogFallback("PT preload frame resources or pipelines failed to initialize.");
			if ((int)nri_ptloadingtrace >= 1)
			{
				Printf("NRI PT loading gate: event=renderer-preload result=ready reason=init-failed ms=%.3f\n",
					DurationMs(context.start, std::chrono::steady_clock::now()));
			}
			return false;
		}
	}
	return true;
}

void NRIPreloadCoordinator::ResetSceneStats(NRIRenderer& renderer)
{
	renderer.ResetSceneBufferFrameStats();
	ResetRendererSkyPerfTraceStats();
	nri_scene::ResetAverageTextureColorCache();
	nri_scene::ResetSkyPerfStats();
}

NRIPreloadCoordinator::StepResult NRIPreloadCoordinator::PreloadStaticSceneAndStartupCorrection(NRIRenderer& renderer, const Context& context)
{
	renderer.RefreshMapWorld();
	if (!renderer.mMapWorld.valid)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=map-invalid ms=%.3f\n",
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return StepResult::Ready;
	}

	if (!renderer.PreloadStaticMapResources())
	{
		renderer.LogFallback("PT preload resident static scene build failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=static-map-failed ms=%.3f\n",
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return StepResult::Ready;
	}

	if (!renderer.ApplyStartupMapWorldCorrectionIfNeeded("renderer-preload"))
	{
		renderer.LogFallback("PT preload startup map-world correction failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=startup-correction-failed ms=%.3f\n",
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return StepResult::Ready;
	}
	if (renderer.mAllowStartupMapWorldCorrection)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=wait reason=startup-correction-pending ms=%.3f\n",
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return StepResult::Wait;
	}
	if (!renderer.mStaticMapScene.valid ||
		!renderer.mStaticMapScene.texturesResident ||
		!renderer.mStaticMapScene.buffersResident ||
		!renderer.mStaticMapScene.accelerationResident ||
		renderer.mStaticMapScene.buildSerial != renderer.mMapWorld.buildSerial)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=continue reason=startup-correction-rebuild static_valid=%u textures=%u buffers=%u acceleration=%u scene_build_serial=%llu map_build_serial=%llu ms=%.3f\n",
				renderer.mStaticMapScene.valid ? 1u : 0u,
				renderer.mStaticMapScene.texturesResident ? 1u : 0u,
				renderer.mStaticMapScene.buffersResident ? 1u : 0u,
				renderer.mStaticMapScene.accelerationResident ? 1u : 0u,
				(unsigned long long)renderer.mStaticMapScene.buildSerial,
				(unsigned long long)renderer.mMapWorld.buildSerial,
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		if (!renderer.PreloadStaticMapResources())
		{
			renderer.LogFallback("PT preload corrected resident static scene build failed.");
			if ((int)nri_ptloadingtrace >= 1)
			{
				Printf("NRI PT loading gate: event=renderer-preload result=ready reason=startup-correction-static-map-failed ms=%.3f\n",
					DurationMs(context.start, std::chrono::steady_clock::now()));
			}
			return StepResult::Ready;
		}
	}
	return StepResult::Continue;
}

void NRIPreloadCoordinator::RefreshStaticLighting(NRIRenderer& renderer, Context& context)
{
	renderer.RefreshSceneLightSystem(true, nullptr, nullptr, nullptr, nullptr, false);
	if (renderer.mGpuSceneHasDynamicOverlay)
	{
		return;
	}

	const bool needsResidentStaticLightRefresh =
		!renderer.mSceneLights.GetAnalyticLights().activeLights.empty() ||
		renderer.mBoundRuntimeLightCount != 0 ||
		renderer.mSceneLights.GetSectorLighting().activeSectorCount > 0 ||
		renderer.mBoundSectorLightActiveCount != 0;
	if (needsResidentStaticLightRefresh && !renderer.RefreshResidentStaticSceneDataSet())
	{
		context.staticLightRefreshReady = false;
		renderer.LogFallback("PT preload static scene light refresh failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=continue reason=static-light-refresh-failed analytic=%u runtime_bound=%u sector_active=%u sector_bound=%u ms=%.3f\n",
				renderer.mSceneLights.GetAnalyticLights().activeLights.empty() ? 0u : 1u,
				renderer.mBoundRuntimeLightCount,
				renderer.mSceneLights.GetSectorLighting().activeSectorCount,
				renderer.mBoundSectorLightActiveCount,
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
	}
}

NRIPreloadCoordinator::StepResult NRIPreloadCoordinator::PreloadResidentSceneResources(NRIRenderer& renderer, const Context& context)
{
	if (!renderer.PreloadPersistentVoxelResources())
	{
		if (renderer.mPersistentVoxels.HasPreloadPending())
		{
			if ((int)nri_ptloadingtrace >= 1)
			{
				uint32_t requiredPending = 0;
				uint32_t requiredReady = 0;
				uint32_t optionalPending = 0;
				uint32_t failed = 0;
				renderer.mPersistentVoxels.CountAdmissionWork(requiredPending, requiredReady, optionalPending, failed);
				Printf("NRI PT loading gate: event=renderer-preload result=wait reason=persistent-voxel-pending required_pending=%u required_ready=%u optional_pending=%u failed=%u ms=%.3f\n",
					requiredPending,
					requiredReady,
					optionalPending,
					failed,
					DurationMs(context.start, std::chrono::steady_clock::now()));
			}
			return StepResult::Wait;
		}
		renderer.LogFallback("PT preload persistent voxel resource admission failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=persistent-voxel-failed ms=%.3f\n",
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return StepResult::Ready;
	}
	if (!renderer.PreloadMaterialResources())
	{
		renderer.LogFallback("PT preload material warmup failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=material-failed ms=%.3f\n",
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return StepResult::Ready;
	}

	NRIRenderer::EmissiveSamplingBuildContext emissiveSamplingContext = {};
	emissiveSamplingContext.staticGeometry = &renderer.mStaticMapScene.geometry;
	if (!renderer.UpdateEmissiveSamplingBuffers(emissiveSamplingContext))
	{
		renderer.LogFallback("PT preload emissive primitive update failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=emissive-sampling-failed ms=%.3f\n",
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return StepResult::Ready;
	}
	if (!renderer.BuildEmissiveTopLevelAccelerationStructure())
	{
		renderer.LogFallback("PT preload emissive TLAS update failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=emissive-tlas-failed ms=%.3f\n",
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return StepResult::Ready;
	}
	if (!renderer.PreGrowLevelSceneResourcesForLoading())
	{
		renderer.LogFallback("PT preload scene resource pre-grow failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=pre-grow-failed ms=%.3f\n",
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return StepResult::Ready;
	}
	return StepResult::Continue;
}

bool NRIPreloadCoordinator::Finish(NRIRenderer& renderer, const Context& context)
{
	renderer.PrepareSceneTextureInputsForCompute();
	const bool staticReady =
		renderer.mStaticMapScene.valid &&
		renderer.mStaticMapScene.texturesResident &&
		renderer.mStaticMapScene.buffersResident &&
		renderer.mStaticMapScene.accelerationResident &&
		(!renderer.mMapWorld.valid || renderer.mStaticMapScene.buildSerial == renderer.mMapWorld.buildSerial);
	const NRIPersistentVoxelPreloadStatus voxelStatus = renderer.mPersistentVoxels.BuildPreloadStatusSnapshot();
	Printf("NRI PT loading summary: static_ready=%u startup_correction_pending=%u required_voxel_pending=%u required_voxel_ready=%u optional_voxel_pending=%u voxel_batch_ready=%u voxel_batch_pending=%u deferred_texture_prewarm=%u deferred_onboarding=%u frame_target_used=%u standalone_context_used=%u gpu_voxel_loading=%u static_light_refresh=%u\n",
		staticReady ? 1u : 0u,
		renderer.mAllowStartupMapWorldCorrection ? 1u : 0u,
		voxelStatus.requiredPending,
		voxelStatus.requiredReady,
		voxelStatus.optionalPending,
		voxelStatus.batchReady ? 1u : 0u,
		voxelStatus.batchPendingActors,
		voxelStatus.deferredTexturePrewarm,
		voxelStatus.deferredOnboarding,
		context.frameTargetUsed ? 1u : 0u,
		context.standaloneContextUsed ? 1u : 0u,
		voxelStatus.gpuLoadingEnabled ? 1u : 0u,
		context.staticLightRefreshReady ? 1u : 0u);
	Printf("NRI PT preload ready: level=%s build_serial=%llu chunks=%u tris=%u materials=%u\n",
		renderer.mMapWorld.level != nullptr ? renderer.mMapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)renderer.mMapWorld.buildSerial,
		(uint32_t)renderer.mStaticMapScene.chunks.size(),
		(uint32_t)renderer.mStaticMapScene.geometry.primitives.size(),
		(uint32_t)renderer.mStaticMapScene.gpuMaterials.size());
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading gate: event=renderer-preload result=ready reason=complete static_light_refresh=%u ms=%.3f\n",
			context.staticLightRefreshReady ? 1u : 0u,
			DurationMs(context.start, std::chrono::steady_clock::now()));
	}
	return true;
}

bool NRIPreloadCoordinator::Run(NRIRenderer& renderer, const NRIPreloadLevelSceneInputs& inputs)
{
	Context context = {};
	context.outputWidth = inputs.outputWidth;
	context.outputHeight = inputs.outputHeight;
	context.targetWidth = inputs.targetWidth;
	context.targetHeight = inputs.targetHeight;
	context.frameTargetUsed = inputs.frameTargetUsed;
	context.standaloneContextUsed = inputs.standaloneContextUsed;
	context.start = std::chrono::steady_clock::now();

	if (!HasFrameTarget(renderer, context))
	{
		return false;
	}
	if (ShouldSkipForUnsupportedPathTracing(renderer, context))
	{
		return true;
	}

	TraceBegin(renderer, context);
	if (!EnsureFrameResources(renderer, context))
	{
		return true;
	}
	ResetSceneStats(renderer);

	const StepResult staticSceneResult = PreloadStaticSceneAndStartupCorrection(renderer, context);
	if (staticSceneResult != StepResult::Continue)
	{
		return staticSceneResult != StepResult::Wait;
	}

	RefreshStaticLighting(renderer, context);
	const StepResult resourcesResult = PreloadResidentSceneResources(renderer, context);
	if (resourcesResult != StepResult::Continue)
	{
		return resourcesResult != StepResult::Wait;
	}

	return Finish(renderer, context);
}
