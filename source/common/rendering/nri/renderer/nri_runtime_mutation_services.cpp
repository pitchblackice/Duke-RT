#include "nri_renderer.h"

#include "../system/nri_renderdevice.h"
#include "c_cvars.h"

#include <chrono>

EXTERN_CVAR(Bool, nri_ptscenestats)
EXTERN_CVAR(Bool, nri_ptslowdowntrace)
EXTERN_CVAR(Bool, nri_pttemporaltrace)
EXTERN_CVAR(Int, nri_pttraceframes)
EXTERN_CVAR(Int, perf_looptraceframes)

namespace
{
    static bool ShouldCollectRuntimeMutationPerfTiming()
    {
        const bool perfLoopTraceActive = (int)perf_looptraceframes > 0;
        const bool temporalTraceActive = !!nri_pttemporaltrace && (int)nri_pttraceframes > 0;
        return perfLoopTraceActive || temporalTraceActive || (bool)nri_ptslowdowntrace || (bool)nri_ptscenestats;
    }

    static double RuntimeMutationDurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
    {
        return std::chrono::duration<double, std::milli>(end - start).count();
    }

    class ScopedPtPerfTimer
    {
    public:
        explicit ScopedPtPerfTimer(double& targetMs)
            : mTarget(ShouldCollectRuntimeMutationPerfTiming() ? &targetMs : nullptr)
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
                *mTarget += RuntimeMutationDurationMs(mStart, std::chrono::steady_clock::now());
            }
        }

    private:
        double* mTarget = nullptr;
        std::chrono::steady_clock::time_point mStart = {};
    };

    static nri::AccessStage NRIComputeShaderResourceAccess()
    {
        return { nri::AccessBits::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER };
    }

    static nri::AccessStage NRICopySourceAccess()
    {
        return { nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY };
    }

    static nri::AccessStage NRICopyDestinationAccess()
    {
        return { nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY };
    }

    static nri::AccessStage NRIAccelerationStructureBuildInputAccess()
    {
        return { nri::AccessBits::SHADER_RESOURCE, nri::StageBits::ALL_SHADERS };
    }
}


// Runtime mutation renderer integration extracted from nri_renderer.cpp.

bool NRIRenderer::StageRuntimeMutationResidentGeometryUploadRanges(const std::vector<RuntimeMutationResidentUploadRange>& ranges)
{
	if (ranges.empty())
	{
		return true;
	}

	if (mFrameBuffer == nullptr || mFrameBuffer->mCommandBuffer == nullptr)
	{
		return false;
	}

	constexpr uint64_t kResidentUploadScratchAlignment = 16u;
	auto& frameScratch = GetResidentUploadScratchFrame();

	struct UploadKindState
	{
		ResidentBufferUploadScratch* scratch = nullptr;
		NRIBufferResource* resource = nullptr;
		const uint8_t* data = nullptr;
		uint64_t availableBytes = 0;
		nri::AccessStage after = {};
		double* aggregateMs = nullptr;
		double* stageMs = nullptr;
		uint64_t requiredSize = 0;
		uint64_t mapStart = UINT64_MAX;
		uint64_t mapEnd = 0;
		bool used = false;
	};

	std::array<UploadKindState, 3> states = {};
	states[ResidentUploadKind_Vertex] = {
		&frameScratch.vertex,
		&mStaticVertexBuffer,
		reinterpret_cast<const uint8_t*>(mStaticMapScene.geometry.vertices.data()),
		(uint64_t)mStaticMapScene.geometry.vertices.size() * sizeof(nri_scene::SceneVertex),
		NRIAccelerationStructureBuildInputAccess(),
		&mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexIndexCopyMs,
		&mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexStageMs };
	states[ResidentUploadKind_Index] = {
		&frameScratch.index,
		&mStaticIndexBuffer,
		reinterpret_cast<const uint8_t*>(mStaticMapScene.geometry.indices.data()),
		(uint64_t)mStaticMapScene.geometry.indices.size() * sizeof(uint32_t),
		NRIAccelerationStructureBuildInputAccess(),
		&mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexIndexCopyMs,
		&mLastPerfShellTraceStats.runtimeMutationResidentApplyIndexStageMs };
	states[ResidentUploadKind_Primitive] = {
		&frameScratch.primitive,
		&mStaticPrimitiveBuffer,
		reinterpret_cast<const uint8_t*>(mStaticMapScene.geometry.primitives.data()),
		(uint64_t)mStaticMapScene.geometry.primitives.size() * sizeof(nri_scene::PrimitiveData),
		NRIComputeShaderResourceAccess(),
		&mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveRewriteMs,
		&mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveStageMs };

	for (UploadKindState& state : states)
	{
		state.requiredSize = state.scratch != nullptr ? state.scratch->cursor : 0;
	}

	for (const RuntimeMutationResidentUploadRange& range : ranges)
	{
		if (range.uploadKind < ResidentUploadKind_Vertex || range.uploadKind > ResidentUploadKind_Primitive)
		{
			return false;
		}
		UploadKindState& state = states[range.uploadKind];
		if (state.resource == nullptr ||
			state.resource->buffer == nullptr ||
			state.data == nullptr ||
			range.size == 0 ||
			range.byteOffset > state.availableBytes ||
			range.size > state.availableBytes - range.byteOffset)
		{
			return false;
		}

		state.used = true;
		state.requiredSize =
			(state.requiredSize + kResidentUploadScratchAlignment - 1u) &
			~(kResidentUploadScratchAlignment - 1u);
		state.requiredSize += range.size;
	}

	for (UploadKindState& state : states)
	{
		if (state.used && !EnsureResidentUploadScratchBuffer(*state.scratch, frameScratch, state.requiredSize))
		{
			return false;
		}
	}

	struct StagedCopy
	{
		UploadKindState* state = nullptr;
		uint64_t targetOffset = 0;
		uint64_t scratchOffset = 0;
		uint64_t size = 0;
		const uint8_t* data = nullptr;
	};

	std::vector<StagedCopy> stagedCopies;
	stagedCopies.reserve(ranges.size());
	for (const RuntimeMutationResidentUploadRange& range : ranges)
	{
		UploadKindState& state = states[range.uploadKind];
		const uint64_t scratchOffset =
			(state.scratch->cursor + kResidentUploadScratchAlignment - 1u) &
			~(kResidentUploadScratchAlignment - 1u);
		const uint64_t requiredSize = scratchOffset + range.size;
		if (requiredSize > state.scratch->buffer.size)
		{
			return false;
		}

		state.scratch->cursor = requiredSize;
		state.mapStart = std::min(state.mapStart, scratchOffset);
		state.mapEnd = std::max(state.mapEnd, requiredSize);
		stagedCopies.push_back({ &state, range.byteOffset, scratchOffset, range.size, state.data + range.byteOffset });
	}

	for (UploadKindState& state : states)
	{
		if (!state.used)
		{
			continue;
		}

		const uint64_t mapSize = state.mapEnd - state.mapStart;
		void* mapped = nullptr;
		{
			ScopedPtPerfTimer aggregateTimer(*state.aggregateMs);
			ScopedPtPerfTimer stageTimer(*state.stageMs);
			ScopedPtPerfTimer mapTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyStageMapMs);
			mapped = mFrameBuffer->mCore.MapBuffer(*state.scratch->buffer.buffer, state.mapStart, mapSize);
		}
		if (mapped == nullptr)
		{
			return false;
		}

		{
			ScopedPtPerfTimer aggregateTimer(*state.aggregateMs);
			ScopedPtPerfTimer stageTimer(*state.stageMs);
			ScopedPtPerfTimer memcpyTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyStageMemcpyMs);
			for (const StagedCopy& copy : stagedCopies)
			{
				if (copy.state == &state)
				{
					std::memcpy(static_cast<uint8_t*>(mapped) + (copy.scratchOffset - state.mapStart), copy.data, (size_t)copy.size);
				}
			}
		}

		{
			ScopedPtPerfTimer aggregateTimer(*state.aggregateMs);
			ScopedPtPerfTimer stageTimer(*state.stageMs);
			ScopedPtPerfTimer mapTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyStageMapMs);
			mFrameBuffer->mCore.UnmapBuffer(*state.scratch->buffer.buffer);
		}
	}

	std::vector<nri::BufferBarrierDesc> sourceBarriers;
	std::vector<nri::BufferBarrierDesc> beforeCopyBarriers;
	std::vector<nri::BufferBarrierDesc> afterCopyBarriers;
	sourceBarriers.reserve(states.size());
	beforeCopyBarriers.reserve(states.size());
	afterCopyBarriers.reserve(states.size());

	for (UploadKindState& state : states)
	{
		if (!state.used)
		{
			continue;
		}

		if (!state.scratch->copySourceActive)
		{
			nri::BufferBarrierDesc sourceBarrier = {};
			sourceBarrier.buffer = state.scratch->buffer.buffer;
			sourceBarrier.before = {};
			sourceBarrier.after = NRICopySourceAccess();
			sourceBarriers.push_back(sourceBarrier);
			state.scratch->copySourceActive = true;
		}

		nri::BufferBarrierDesc beforeCopyBarrier = {};
		beforeCopyBarrier.buffer = state.resource->buffer;
		beforeCopyBarrier.before = state.after;
		beforeCopyBarrier.after = NRICopyDestinationAccess();
		beforeCopyBarriers.push_back(beforeCopyBarrier);

		nri::BufferBarrierDesc afterCopyBarrier = {};
		afterCopyBarrier.buffer = state.resource->buffer;
		afterCopyBarrier.before = NRICopyDestinationAccess();
		afterCopyBarrier.after = state.after;
		afterCopyBarriers.push_back(afterCopyBarrier);
	}

	{
		ScopedPtPerfTimer commandTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyStageCommandMs);
		if (!sourceBarriers.empty())
		{
			nri::BarrierDesc sourceBarrierDesc = {};
			sourceBarrierDesc.buffers = sourceBarriers.data();
			sourceBarrierDesc.bufferNum = (uint32_t)sourceBarriers.size();
			mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, sourceBarrierDesc);
			mLastPerfShellTraceStats.runtimeMutationResidentApplyStageBarrierCommandCount++;
		}

		if (!beforeCopyBarriers.empty())
		{
			nri::BarrierDesc beforeCopyBarrierDesc = {};
			beforeCopyBarrierDesc.buffers = beforeCopyBarriers.data();
			beforeCopyBarrierDesc.bufferNum = (uint32_t)beforeCopyBarriers.size();
			mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, beforeCopyBarrierDesc);
			mLastPerfShellTraceStats.runtimeMutationResidentApplyStageBarrierCommandCount++;
		}

		for (const StagedCopy& copy : stagedCopies)
		{
			mFrameBuffer->mCore.CmdCopyBuffer(
				*mFrameBuffer->mCommandBuffer,
				*copy.state->resource->buffer,
				copy.targetOffset,
				*copy.state->scratch->buffer.buffer,
				copy.scratchOffset,
				copy.size);
			mLastPerfShellTraceStats.runtimeMutationResidentApplyStageCopyCommandCount++;
		}

		if (!afterCopyBarriers.empty())
		{
			nri::BarrierDesc afterCopyBarrierDesc = {};
			afterCopyBarrierDesc.buffers = afterCopyBarriers.data();
			afterCopyBarrierDesc.bufferNum = (uint32_t)afterCopyBarriers.size();
			mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, afterCopyBarrierDesc);
			mLastPerfShellTraceStats.runtimeMutationResidentApplyStageBarrierCommandCount++;
		}
	}

	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageBatchCount++;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageBatchRangeCount += (uint32_t)stagedCopies.size();
	return true;
}

NRIRuntimeMutationResidentUploadServices NRIRenderer::BuildRuntimeMutationResidentUploadServices()
{
	NRIRuntimeMutationResidentUploadServices services = {};
	services.user = this;
	services.stageGeometryRanges = [](void* user, const std::vector<RuntimeMutationResidentUploadRange>& ranges) -> bool
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		ScopedPtPerfTimer residentApplyPerfTimer(renderer->mLastPerfShellTraceStats.runtimeMutationResidentApplyMs);
		return renderer->StageRuntimeMutationResidentGeometryUploadRanges(ranges);
	};
	services.noteUploadRange = [](void* user, int uploadKind, uint64_t size)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		switch (uploadKind)
		{
		case ResidentUploadKind_Vertex:
			renderer->mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexStageRangeCount++;
			renderer->mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexStageBytes += size;
			break;
		case ResidentUploadKind_Index:
			renderer->mLastPerfShellTraceStats.runtimeMutationResidentApplyIndexStageRangeCount++;
			renderer->mLastPerfShellTraceStats.runtimeMutationResidentApplyIndexStageBytes += size;
			break;
		case ResidentUploadKind_Primitive:
			renderer->mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveStageRangeCount++;
			renderer->mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveStageBytes += size;
			break;
		default:
			break;
		}
	};
	services.noteCoalescedRange = [](void* user, const RuntimeMutationResidentUploadRange& range)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		renderer->mLastPerfShellTraceStats.runtimeMutationResidentApplyCoalescedStageRangeCount++;
		renderer->mLastPerfShellTraceStats.runtimeMutationResidentApplyCoalescedStageBytes += range.size;
		if (range.size > range.dirtySize)
		{
			renderer->mLastPerfShellTraceStats.runtimeMutationResidentApplyCoalescedStageGapBytes += range.size - range.dirtySize;
		}
	};
	services.noteCoalescedReject = [](void* user)
	{
		static_cast<NRIRenderer*>(user)->mLastPerfShellTraceStats.runtimeMutationResidentApplyCoalescedStageRejectCount++;
	};
	return services;
}

NRIRuntimeMutationOverlayServices NRIRenderer::BuildRuntimeMutationOverlayServices()
{
	NRIRuntimeMutationOverlayServices services = {};
	services.user = this;
	services.buildOverlay = [](void* user, nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials, bool* outResidentStaticSceneChanged) -> bool
	{
		return static_cast<NRIRenderer*>(user)->BuildRuntimeMapMutationOverlayImpl(
			outGeometry,
			outMaterials,
			outResidentStaticSceneChanged);
	};
	return services;
}

NRIRuntimeMutationResidentApplyServices NRIRenderer::BuildRuntimeMutationResidentApplyServices()
{
	NRIRuntimeMutationResidentApplyServices services = {};
	services.user = this;
	services.tryApplyChunk = [](void* user, const nri_scene::PTMapChunk& mapChunk, RuntimeMapMutationCache::ChunkReplacement& replacement, RuntimeMutationResidentApplyResult& outResult) -> bool
	{
		return static_cast<NRIRenderer*>(user)->TryApplyRuntimeMutationChunkToResidentSceneImpl(
			mapChunk,
			replacement,
			outResult);
	};
	return services;
}

NRIRuntimeMutationResidentSceneRefreshServices NRIRenderer::BuildRuntimeMutationResidentSceneRefreshServices()
{
	NRIRuntimeMutationResidentSceneRefreshServices services = {};
	services.user = this;
	services.refreshMaterialSlices = [](void* user, const std::vector<uint32_t>& chunkListIndices, const std::vector<uint32_t>& animatedChunkListIndices) -> bool
	{
		return static_cast<NRIRenderer*>(user)->RefreshResidentStaticMaterialSlices(
			chunkListIndices,
			"resident_runtime_mutation_static",
			&animatedChunkListIndices);
	};
	services.noteMaterialFallback = [](void* user, uint32_t chunkCount)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		renderer->mLastPerfResourceTraceStats.residentChunkBatchMaterialFallbackCount++;
		if (nri_ptscenestats)
		{
			Printf("NRI PT static scene trace: event=resident_material_refresh_fallback frame=%u chunks=%u\n",
				renderer->mFrameIndex,
				chunkCount);
		}
	};
	services.rebuildMaterialState = [](void* user, const char* reason) -> bool
	{
		return static_cast<NRIRenderer*>(user)->RebuildResidentStaticMaterialState(reason);
	};
	services.rebuildChunkBlases = [](void* user, const std::vector<uint32_t>& chunkListIndices) -> bool
	{
		return static_cast<NRIRenderer*>(user)->RebuildResidentStaticMapChunkBlases(chunkListIndices);
	};
	services.noteBlasRebuild = [](void* user, uint32_t chunkCount)
	{
		static_cast<NRIRenderer*>(user)->mLastPerfResourceTraceStats.residentChunkBatchBlasRebuildCount += chunkCount;
	};
	services.commitSuccess = [](void* user, const std::vector<uint32_t>& geometryChunkListIndices, bool materialDirty)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		renderer->mStaticMapScene.texturesResident = true;
		renderer->mStaticMapScene.buffersResident = true;
		renderer->mStaticMapScene.accelerationResident = true;
		renderer->mStaticMapScene.gpuUploadCount += materialDirty ? 1u : 0u;
		renderer->mStaticMapScene.accelerationBuildCount += (uint32_t)geometryChunkListIndices.size();
		renderer->SyncResidentMapChunkRegistryFromStaticScene();
		renderer->RefreshStateCommitCombinedGeometryStaticPrefixForResidentUpdate(geometryChunkListIndices);
	};
	services.recoverFailure = [](void* user, const char* reason) -> bool
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		renderer->DestroyStaticMapSceneCache(reason != nullptr ? reason : "runtime-mutation-resident-refresh-failed");
		renderer->mStaticMapScene = {};
		renderer->mStaticAccelerationBuildSerial = 0;
		renderer->mPreservedStaticMapSky = {};
		return renderer->EnsureStaticMapScene();
	};
	return services;
}

void NRIRenderer::NoteRuntimeMutationResidentAtlasGrow()
{
	mRuntimeMutation.NoteResidentAtlasGrow();
}

void NRIRenderer::ResetRuntimeMutationCacheAndFrame()
{
	mRuntimeMutation.ResetCacheAndFrame();
}


