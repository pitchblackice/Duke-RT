#include "nri_scene_upload.h"
#include "nri_cvars.h"

#include "nri_descriptor_sets.h"
#include "nri_emissive_sampling_upload_policy.h"
#include "nri_renderer.h"
#include "nri_runtime_mutation_trace.h"
#include "nri_scene_lights.h"
#include "printf.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace
{
	static double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	}

	static bool ShouldCollectSceneDataTiming()
	{
		return (int)nri_pttraceframes > 0 || (int)perf_looptraceframes > 0 || (bool)nri_ptslowdowntrace;
	}

	class ScopedPtPerfTimer
	{
	public:
		explicit ScopedPtPerfTimer(double& targetMs)
			: mTarget(ShouldCollectSceneDataTiming() ? &targetMs : nullptr)
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

	static NRIEmissiveSamplingUploadResourceInput BuildUploadResourceInput(
		const NRIBufferResource& resource,
		uint64_t payloadBytes,
		uint32_t payloadStride)
	{
		NRIEmissiveSamplingUploadResourceInput input = {};
		input.hasBuffer = resource.buffer != nullptr;
		input.hasShaderView = resource.shaderView != nullptr;
		input.capacityBytes = resource.size;
		input.currentStride = resource.stride;
		input.payloadBytes = payloadBytes;
		input.payloadStride = payloadStride;
		return input;
	}
}
bool NRIRenderer::UpdateEmissiveSamplingBuffers(
	const EmissiveSamplingBuildContext& context,
	bool* ioWaitedForWrites,
	bool allowSceneDataFrameSlot)
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.emissiveUpdateMs);
	const uint64_t payloadHash = mSceneLights.BuildEmissiveSamplingPayloadHash(context);
	const uint64_t sectorResponsePayloadHash = mSceneLights.BuildEmissiveSectorResponsePayloadHash();
	const bool sectorResponseChanged =
		mEmissiveSectorResponsePayloadCacheValid &&
		mEmissiveSectorResponsePayloadHash != sectorResponsePayloadHash;
	NRISceneDataFrameSlot* frameSlot =
		allowSceneDataFrameSlot && ShouldUseSceneDataFrameRing() ? &GetCurrentSceneDataFrameSlot() : nullptr;
	NRIBufferResource& emissivePrimitiveHeaderBuffer =
		frameSlot != nullptr ? frameSlot->emissivePrimitiveHeaderBuffer : mEmissivePrimitiveHeaderBuffer;
	NRIBufferResource& emissivePrimitiveBuffer =
		frameSlot != nullptr ? frameSlot->emissivePrimitiveBuffer : mEmissivePrimitiveBuffer;
	NRIBufferResource& emissivePrimitiveCdfBuffer =
		frameSlot != nullptr ? frameSlot->emissivePrimitiveCdfBuffer : mEmissivePrimitiveCdfBuffer;
	NRIBufferResource& emissiveMaterialResponseBuffer =
		frameSlot != nullptr ? frameSlot->emissiveMaterialResponseBuffer : mEmissiveMaterialResponseBuffer;
	SceneBufferDebugStats& emissivePrimitiveHeaderStats =
		frameSlot != nullptr ? frameSlot->emissivePrimitiveHeaderStats : mEmissivePrimitiveHeaderBufferStats;
	SceneBufferDebugStats& emissivePrimitiveStats =
		frameSlot != nullptr ? frameSlot->emissivePrimitiveStats : mEmissivePrimitiveBufferStats;
	SceneBufferDebugStats& emissivePrimitiveCdfStats =
		frameSlot != nullptr ? frameSlot->emissivePrimitiveCdfStats : mEmissivePrimitiveCdfBufferStats;
	SceneBufferDebugStats& emissiveMaterialResponseStats =
		frameSlot != nullptr ? frameSlot->emissiveMaterialResponseStats : mEmissiveMaterialResponseBufferStats;
	const bool destinationCacheValid =
		frameSlot != nullptr ?
			frameSlot->emissiveSamplingPayloadValid && frameSlot->emissiveSamplingPayloadHash == payloadHash :
			mEmissiveSamplingPayloadCacheValid && mEmissiveSamplingPayloadHash == payloadHash;

	const auto commitEmissiveDescriptors = [&]()
	{
		mSceneDataDescriptors[13] = emissivePrimitiveHeaderBuffer.shaderView;
		mSceneDataDescriptors[14] = emissivePrimitiveBuffer.shaderView;
		mSceneDataDescriptors[15] = emissivePrimitiveCdfBuffer.shaderView;
		mSceneDataDescriptors[25] = emissiveMaterialResponseBuffer.shaderView;

		bool descriptorsReady = IsCurrentSceneDataDescriptorsInitialized() && GetCurrentSceneDataSet() != nullptr;
		if (descriptorsReady)
		{
			for (const nri::Descriptor* descriptor : mSceneDataDescriptors)
			{
				if (descriptor == nullptr)
				{
					descriptorsReady = false;
					break;
				}
			}
		}

		if (descriptorsReady)
		{
			NRIDescriptorSetManager::CommitSceneDataDescriptors(*this, "emissive_sampling_refresh");
		}
	};

	if (destinationCacheValid &&
		emissivePrimitiveHeaderBuffer.shaderView != nullptr &&
		emissivePrimitiveBuffer.shaderView != nullptr &&
		emissivePrimitiveCdfBuffer.shaderView != nullptr &&
		emissiveMaterialResponseBuffer.shaderView != nullptr)
	{
		commitEmissiveDescriptors();
		if (frameSlot != nullptr)
		{
			mBoundEmissivePrimitiveCount = frameSlot->emissivePrimitiveCount;
			mBoundEmissiveDominantPrimitive = frameSlot->emissiveDominantPrimitive;
			mBoundEmissiveDominantTile = frameSlot->emissiveDominantTile;
			mBoundEmissiveDominantFlags = frameSlot->emissiveDominantFlags;
			mBoundEmissiveDominantDataSource = frameSlot->emissiveDominantDataSource;
			mBoundEmissiveTotalPower = frameSlot->emissiveTotalPower;
			mBoundEmissiveDominantPower = frameSlot->emissiveDominantPower;
			mBoundEmissivePrimitiveRecords = frameSlot->emissivePrimitiveDebugRecords;
			mEmissiveSamplingPayloadCacheValid = false;
			mEmissiveSamplingPayloadHash = 0;
		}
		mEmissiveSectorResponsePayloadCacheValid = true;
		mEmissiveSectorResponsePayloadHash = sectorResponsePayloadHash;
		return true;
	}

	NRIEmissivePrimitiveHeaderGpuData emissiveHeader = {};
	std::vector<NRIEmissivePrimitiveGpuData> emissivePrimitives;
	std::vector<float> emissiveCdf;
	std::vector<NRIEmissiveMaterialResponseGpuData> emissiveMaterialResponses;
	std::vector<NRIEmissivePrimitiveDebugRecord> emissiveDebugRecords;
	SceneLightSystem::EmissiveSamplingUploadStats emissiveStats = {};
	mSceneLights.BuildEmissiveSamplingUpload(context, emissiveHeader, emissivePrimitives, emissiveCdf, emissiveMaterialResponses, emissiveDebugRecords, &emissiveStats);
	mLastPerfShellTraceStats.emissiveSamplingSurfaceStatic = emissiveStats.surfaceStatic;
	mLastPerfShellTraceStats.emissiveSamplingSurfaceCaptured = emissiveStats.surfaceCaptured;
	mLastPerfShellTraceStats.emissiveSamplingSurfaceRuntimeMutation = emissiveStats.surfaceRuntimeMutation;
	mLastPerfShellTraceStats.emissiveSamplingSurfaceDynamic = emissiveStats.surfaceDynamic + emissiveStats.surfaceLightOverlay;
	mLastPerfShellTraceStats.emissiveSamplingSurfacePersistentVoxel = emissiveStats.surfacePersistentVoxel;
	mLastPerfShellTraceStats.emissiveSamplingOutputStaticRecords = emissiveStats.outputStaticRecords;
	mLastPerfShellTraceStats.emissiveSamplingOutputDynamicRecords = emissiveStats.outputDynamicRecords;
	mLastPerfShellTraceStats.emissiveSamplingOutputPersistentVoxelRecords = emissiveStats.outputPersistentVoxelRecords;
	mLastPerfShellTraceStats.emissiveSamplingSkippedPersistentVoxelSurfaces = emissiveStats.skippedPersistentVoxelSurfaces;

	const uint64_t headerBytes = sizeof(emissiveHeader);
	const uint64_t primitiveBytes = emissivePrimitives.size() * sizeof(NRIEmissivePrimitiveGpuData);
	const uint64_t cdfBytes = emissiveCdf.size() * sizeof(float);
	const uint64_t materialResponseBytes = emissiveMaterialResponses.size() * sizeof(NRIEmissiveMaterialResponseGpuData);
	const NRIEmissiveSamplingUploadResourceInput uploadInputs[] = {
		BuildUploadResourceInput(emissivePrimitiveHeaderBuffer, headerBytes, sizeof(NRIEmissivePrimitiveHeaderGpuData)),
		BuildUploadResourceInput(emissivePrimitiveBuffer, primitiveBytes, sizeof(NRIEmissivePrimitiveGpuData)),
		BuildUploadResourceInput(emissivePrimitiveCdfBuffer, cdfBytes, sizeof(float)),
		BuildUploadResourceInput(emissiveMaterialResponseBuffer, materialResponseBytes, sizeof(NRIEmissiveMaterialResponseGpuData)),
	};
	bool writesQuiesced = frameSlot != nullptr || (ioWaitedForWrites != nullptr && *ioWaitedForWrites);
	const NRIEmissiveSamplingUploadBatchDecision uploadDecision = NRIPlanEmissiveSamplingUploadBatch(
		uploadInputs,
		sizeof(uploadInputs) / sizeof(uploadInputs[0]),
		frameSlot != nullptr,
		writesQuiesced);
	uint32_t waitCount = 0;
	if (uploadDecision.waitRequired)
	{
		WaitForCommandsTracked("emissive_sampling_upload");
		writesQuiesced = true;
		waitCount = 1;
		if (ioWaitedForWrites != nullptr)
		{
			*ioWaitedForWrites = true;
		}
	}

	const auto ensureStructuredBufferBatched = [this, writesQuiesced](NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after) -> bool
	{
		return EnsureStructuredBuffer(resource, stats, data, size, stride, usage, after, writesQuiesced, "emissive_sampling_upload");
	};

	if (!ensureStructuredBufferBatched(
		emissivePrimitiveHeaderBuffer,
		emissivePrimitiveHeaderStats,
		&emissiveHeader,
		sizeof(emissiveHeader),
		sizeof(NRIEmissivePrimitiveHeaderGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!ensureStructuredBufferBatched(
		emissivePrimitiveBuffer,
		emissivePrimitiveStats,
		emissivePrimitives.empty() ? nullptr : emissivePrimitives.data(),
		emissivePrimitives.empty() ? 0u : emissivePrimitives.size() * sizeof(NRIEmissivePrimitiveGpuData),
		sizeof(NRIEmissivePrimitiveGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!ensureStructuredBufferBatched(
		emissivePrimitiveCdfBuffer,
		emissivePrimitiveCdfStats,
		emissiveCdf.data(),
		emissiveCdf.size() * sizeof(float),
		sizeof(float),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!ensureStructuredBufferBatched(
		emissiveMaterialResponseBuffer,
		emissiveMaterialResponseStats,
		emissiveMaterialResponses.empty() ? nullptr : emissiveMaterialResponses.data(),
		emissiveMaterialResponses.empty() ? 0u : emissiveMaterialResponses.size() * sizeof(NRIEmissiveMaterialResponseGpuData),
		sizeof(NRIEmissiveMaterialResponseGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess()))
	{
		return false;
	}

	mBoundEmissivePrimitiveCount = emissiveHeader.activeCount;
	mBoundEmissiveTotalPower = emissiveHeader.totalPower;
	mBoundEmissiveDominantPrimitive = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissiveDebugRecords.size() ? emissiveDebugRecords[emissiveHeader.dominantIndex].primitiveIndex : UINT32_MAX;
	mBoundEmissiveDominantTile = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].textureId : 0u;
	mBoundEmissiveDominantFlags = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].sourceFlags : 0u;
	mBoundEmissiveDominantDataSource = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].dataSource : 0u;
	mBoundEmissiveDominantPower = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].powerEstimate : 0.0f;
	mBoundEmissivePrimitiveRecords = std::move(emissiveDebugRecords);

	commitEmissiveDescriptors();
	if (frameSlot != nullptr)
	{
		frameSlot->emissiveSamplingPayloadValid = true;
		frameSlot->emissiveSamplingPayloadHash = payloadHash;
		frameSlot->emissivePrimitiveCount = mBoundEmissivePrimitiveCount;
		frameSlot->emissiveDominantPrimitive = mBoundEmissiveDominantPrimitive;
		frameSlot->emissiveDominantTile = mBoundEmissiveDominantTile;
		frameSlot->emissiveDominantFlags = mBoundEmissiveDominantFlags;
		frameSlot->emissiveDominantDataSource = mBoundEmissiveDominantDataSource;
		frameSlot->emissiveTotalPower = mBoundEmissiveTotalPower;
		frameSlot->emissiveDominantPower = mBoundEmissiveDominantPower;
		frameSlot->emissivePrimitiveDebugRecords = mBoundEmissivePrimitiveRecords;
	}

	if (ShouldCollectSceneDataTiming())
	{
		const uint32_t growCount =
			emissivePrimitiveHeaderStats.growEventsLastFrame +
			emissivePrimitiveStats.growEventsLastFrame +
			emissivePrimitiveCdfStats.growEventsLastFrame +
			emissiveMaterialResponseStats.growEventsLastFrame;
		const uint32_t replaceCount =
			emissivePrimitiveHeaderStats.overwriteEventsLastFrame +
			emissivePrimitiveStats.overwriteEventsLastFrame +
			emissivePrimitiveCdfStats.overwriteEventsLastFrame +
			emissiveMaterialResponseStats.overwriteEventsLastFrame;
		const uint64_t payloadBytes = headerBytes + primitiveBytes + cdfBytes + materialResponseBytes;
		Printf("NRI PT emissive sampling upload: frame=%u destination=%s slot=%u payload_hash=0x%016llx bytes=%llu grow=%u replace=%u wait=%u wait_reason=%s source=static:%u,captured:%u,mutation:%u,dynamic:%u,persistent_voxel:%u output=static:%u,dynamic:%u,persistent_voxel:%u skipped_persistent=%u\n",
			mFrameIndex,
			frameSlot != nullptr ? "frame-slot" : "shared",
			frameSlot != nullptr ? GetCurrentQueuedFrameIndex() : UINT32_MAX,
			(unsigned long long)payloadHash,
			(unsigned long long)payloadBytes,
			growCount,
			replaceCount,
			waitCount,
			waitCount != 0 ? "shared-in-flight" : "none",
			emissiveStats.surfaceStatic,
			emissiveStats.surfaceCaptured,
			emissiveStats.surfaceRuntimeMutation,
			emissiveStats.surfaceDynamic + emissiveStats.surfaceLightOverlay,
			emissiveStats.surfacePersistentVoxel,
			emissiveStats.outputStaticRecords,
			emissiveStats.outputDynamicRecords,
			emissiveStats.outputPersistentVoxelRecords,
			emissiveStats.skippedPersistentVoxelSurfaces);
	}
	if (sectorResponseChanged && nri_runtime_mutation::ShouldTracePtPerf())
	{
		const auto& sectorRegistry = mSceneLights.GetSectorLighting();
		Printf("NRI PT emissive sampling refresh: frame=%u reason=sector-response-change primitives=%u total_power=%.3f dominant_primitive=%u dominant_tile=%u sector_response_hash=0x%016llx->0x%016llx response=boost:%u dim:%u neutral:%u\n",
			mFrameIndex,
			mBoundEmissivePrimitiveCount,
			mBoundEmissiveTotalPower,
			mBoundEmissiveDominantPrimitive,
			mBoundEmissiveDominantTile,
			(unsigned long long)mEmissiveSectorResponsePayloadHash,
			(unsigned long long)sectorResponsePayloadHash,
			sectorRegistry.responseBoostSectorCount,
			sectorRegistry.responseDimSectorCount,
			sectorRegistry.responseNeutralSectorCount);
	}
	mEmissiveSamplingPayloadCacheValid = frameSlot == nullptr;
	mEmissiveSamplingPayloadHash = frameSlot == nullptr ? payloadHash : 0;
	mEmissiveSectorResponsePayloadCacheValid = true;
	mEmissiveSectorResponsePayloadHash = sectorResponsePayloadHash;
	return true;
}
