#include "nri_scene_upload.h"
#include "nri_cvars.h"

#include "nri_descriptor_sets.h"
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

	static bool StructuredBufferUpdateNeedsWait(const NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride)
	{
		return
			resource.buffer != nullptr &&
			data != nullptr &&
			size != 0 &&
			resource.stride == stride &&
			resource.size >= std::max<uint64_t>(size, stride);
	}
}
bool NRIRenderer::UpdateEmissiveSamplingBuffers(const EmissiveSamplingBuildContext& context, bool* ioWaitedForWrites)
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.emissiveUpdateMs);
	const uint64_t payloadHash = mSceneLights.BuildEmissiveSamplingPayloadHash(context);
	const uint64_t sectorResponsePayloadHash = mSceneLights.BuildEmissiveSectorResponsePayloadHash();
	const bool sectorResponseChanged =
		mEmissiveSectorResponsePayloadCacheValid &&
		mEmissiveSectorResponsePayloadHash != sectorResponsePayloadHash;
	if (mEmissiveSamplingPayloadCacheValid &&
		mEmissiveSamplingPayloadHash == payloadHash &&
		mEmissivePrimitiveHeaderBuffer.shaderView != nullptr &&
		mEmissivePrimitiveBuffer.shaderView != nullptr &&
		mEmissivePrimitiveCdfBuffer.shaderView != nullptr &&
		mEmissiveMaterialResponseBuffer.shaderView != nullptr)
	{
		if (!mEmissiveSectorResponsePayloadCacheValid)
		{
			mEmissiveSectorResponsePayloadCacheValid = true;
			mEmissiveSectorResponsePayloadHash = sectorResponsePayloadHash;
		}
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
	mLastPerfShellTraceStats.emissiveSamplingSurfaceDynamic = emissiveStats.surfaceDynamic;
	mLastPerfShellTraceStats.emissiveSamplingSurfacePersistentVoxel = emissiveStats.surfacePersistentVoxel;
	mLastPerfShellTraceStats.emissiveSamplingOutputStaticRecords = emissiveStats.outputStaticRecords;
	mLastPerfShellTraceStats.emissiveSamplingOutputDynamicRecords = emissiveStats.outputDynamicRecords;
	mLastPerfShellTraceStats.emissiveSamplingOutputPersistentVoxelRecords = emissiveStats.outputPersistentVoxelRecords;
	mLastPerfShellTraceStats.emissiveSamplingSkippedPersistentVoxelSurfaces = emissiveStats.skippedPersistentVoxelSurfaces;

	const auto ensureStructuredBufferBatched = [this, ioWaitedForWrites](NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after) -> bool
	{
		if (ioWaitedForWrites != nullptr &&
			!*ioWaitedForWrites &&
			StructuredBufferUpdateNeedsWait(resource, data, size, stride))
		{
			WaitForCommandsTracked("emissive_sampling_upload");
			*ioWaitedForWrites = true;
		}

		return EnsureStructuredBuffer(resource, stats, data, size, stride, usage, after, ioWaitedForWrites != nullptr && *ioWaitedForWrites, "emissive_sampling_upload");
	};

	if (!ensureStructuredBufferBatched(
		mEmissivePrimitiveHeaderBuffer,
		mEmissivePrimitiveHeaderBufferStats,
		&emissiveHeader,
		sizeof(emissiveHeader),
		sizeof(NRIEmissivePrimitiveHeaderGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!ensureStructuredBufferBatched(
		mEmissivePrimitiveBuffer,
		mEmissivePrimitiveBufferStats,
		emissivePrimitives.empty() ? nullptr : emissivePrimitives.data(),
		emissivePrimitives.empty() ? 0u : emissivePrimitives.size() * sizeof(NRIEmissivePrimitiveGpuData),
		sizeof(NRIEmissivePrimitiveGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!ensureStructuredBufferBatched(
		mEmissivePrimitiveCdfBuffer,
		mEmissivePrimitiveCdfBufferStats,
		emissiveCdf.data(),
		emissiveCdf.size() * sizeof(float),
		sizeof(float),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!ensureStructuredBufferBatched(
		mEmissiveMaterialResponseBuffer,
		mEmissiveMaterialResponseBufferStats,
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

	mSceneDataDescriptors[13] = mEmissivePrimitiveHeaderBuffer.shaderView;
	mSceneDataDescriptors[14] = mEmissivePrimitiveBuffer.shaderView;
	mSceneDataDescriptors[15] = mEmissivePrimitiveCdfBuffer.shaderView;
	mSceneDataDescriptors[25] = mEmissiveMaterialResponseBuffer.shaderView;

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
	mEmissiveSamplingPayloadCacheValid = true;
	mEmissiveSamplingPayloadHash = payloadHash;
	mEmissiveSectorResponsePayloadCacheValid = true;
	mEmissiveSectorResponsePayloadHash = sectorResponsePayloadHash;
	return true;
}
