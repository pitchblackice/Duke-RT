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

void NRIRenderer::NoteRuntimeMutationResidentAtlasGrow()
{
	mRuntimeMutation.NoteResidentAtlasGrow();
}

void NRIRenderer::ResetRuntimeMutationCacheAndFrame()
{
	mRuntimeMutation.ResetCacheAndFrame();
}

bool NRIRenderer::BuildRuntimeMapMutationOverlay(nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials, bool* outResidentStaticSceneChanged)
{
	outGeometry = {};
	outMaterials = {};
	if (outResidentStaticSceneChanged != nullptr)
	{
		*outResidentStaticSceneChanged = false;
	}
	mRuntimeMutation.lastFrame = {};
	bool residentStaticSceneChanged = false;
	bool residentMaterialDirty = false;
	bool residentGeometryDirty = false;
	bool startupMaterialOnlyMutationDetected = false;
	std::vector<uint32_t> residentMaterialChunkListIndices;
	std::vector<uint32_t> animatedResidentApplyMaterialChunkListIndices;
	std::vector<uint32_t> residentGeometryChunkListIndices;
	mRuntimeMutation.ClearResidentGeometryUploadRanges();
	const NRIRuntimeMutationSettings runtimeMutationSettings = BuildNRIRuntimeMutationSettingsFromCVars();
	const bool tracePtPerf = ShouldTracePtPerf();
	const bool collectRuntimeMutationCacheStats = tracePtPerf || (bool)nri_ptslowdowntrace;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildVisibleMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildInvisibleMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildNearMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildFarMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildUnknownDistanceMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshVisibleMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshInvisibleMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshNearMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshFarMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshUnknownDistanceMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyVisibleMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyInvisibleMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyNearMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyFarMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyUnknownDistanceMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationNearDistance = runtimeMutationSettings.nearDistance;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyLiveBuildMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryBuildMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialBuildMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBaselineCaptureMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyAtlasMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyAtlasBookkeepingMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexIndexCopyMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexCpuCopyMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyIndexCpuCopyMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexStageMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyIndexStageMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveRewriteMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveCpuRewriteMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveStageMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryOrderHashMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasSetupMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasFilterMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasCreateMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasScratchMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasBarrierMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasBuildMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationCandidateChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationAnalyzedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationBackgroundSweepChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationDirtyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationRebuiltChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationHeldChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateVisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateInvisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateNearChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateFarChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateUnknownDistanceChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateBoundsValidChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateBoundsInvalidChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateActiveReplacementChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateVisibleResidentValidationChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateStartupVisibleValidationChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateUnresolvedTextureChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateStaticAnimatedSuppressedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateSectorDirtyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateSectionDirtyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateDraggedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateSignatureWatchChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateBackgroundSweepSourceChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateDeferredStructuralChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationDirtyVisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationDirtyInvisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationDirtyNearChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationDirtyFarChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationDirtyUnknownDistanceChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationDirtyActiveReplacementChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationDirtyBackgroundSweepChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildVisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildInvisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildNearChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildFarChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildUnknownDistanceChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildActiveReplacementChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildBackgroundSweepChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredCoalescedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFlushedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredPromotedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredPendingChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearCoalescedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearFlushedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearPendingChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearBudget =
		runtimeMutationSettings.nearInvisibleStructuralBudget;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarCoalescedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarFlushedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarPendingChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarBudget =
		runtimeMutationSettings.farStructuralBudget;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredBudget =
		mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearBudget +
		mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarBudget;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshVisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshInvisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshNearChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshFarChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshUnknownDistanceChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredCoalescedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredFlushedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredPendingChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredNearChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredNearCoalescedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredNearFlushedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredNearPendingChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredNearBudget =
		runtimeMutationSettings.nearInvisibleMaterialBudget;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshActiveReplacementChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshBackgroundSweepChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyVisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyInvisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyNearChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyFarChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyUnknownDistanceChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyActiveReplacementChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBackgroundSweepChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshAnimatedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReplacementDeltaChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshHardwareCanvasChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralReplacementViewChangedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralStaticAnimatedModeFlipChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralExcludeStaticFlipChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralForcedTopologyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralInvalidChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralMaterialOnlyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralSectorMaterialOnlyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralWallMaterialOnlyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralMixedMaterialOnlyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralGeometryOrDirtyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtySectorGeometryOnlyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtyWallGeometryOnlyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtySectorWallGeometryChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtyDirtyOnlyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtyGeometryDirtyMixedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtyForceTopologyOnlyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtyRealCountChangeChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtyWallsOnlyChangedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtyFlatsOnlyChangedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtyWallsAndFlatsChangedChunks = 0;
	mLastPerfShellTraceStats.runtimeRecurringChunkTrackedCount = 0;
	mLastPerfShellTraceStats.runtimeRecurringChunkRecurringCount = 0;
	mLastPerfShellTraceStats.runtimeRecurringChunkVisitCount = 0;
	mLastPerfShellTraceStats.runtimeRecurringChunkUniqueStateCount = 0;
	mLastPerfShellTraceStats.runtimeRecurringChunkTransitionCount = 0;
	mLastPerfShellTraceStats.runtimeRecurringChunkRepeatedStateHitCount = 0;
	mLastPerfShellTraceStats.runtimeRecurringChunkAbaRecurrenceCount = 0;
	mLastPerfShellTraceStats.runtimeRecurringChunkMaxUniqueStateCount = 0;
	mLastPerfShellTraceStats.runtimeMutationHardwareCanvasChunkCount = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaReasonMaskOr = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReasonMaskOr = 0;
	mLastPerfShellTraceStats.runtimeMutationInvalidForceTopologyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationInvalidAppliedCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopSkipCount = 0;
	mLastPerfShellTraceStats.runtimeMutationInvalidFailedCount = 0;
	mLastPerfShellTraceStats.runtimeMutationInvalidSyncSkipCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopCandidateCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopCandidateReasonMaskOr = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockNotAuthoritativeCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockResidentUnavailableCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockReplacementInvalidCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockExcludeStaticCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockSurfaceCountMismatch = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockMaterialCountMismatch = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockPrimitiveCountMismatch = 0;
	mLastPerfShellTraceStats.runtimeMutationValidMaterialCount = 0;
	mLastPerfShellTraceStats.runtimeMutationValidStructuralCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStructuralCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyFastMaterialOnlyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplySlowMaterialOnlyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyExclusiveCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyNoResidentChunkCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyInvalidReplacementCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyMaterialCountMismatchCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashCheckCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashSkipCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashMissCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashRejectCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashCheckCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashSkipCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashMissCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashRejectCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashBlasSkipCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderCheckCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderEquivalentCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderMissCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderRejectCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexStageRangeCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyIndexStageRangeCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveStageRangeCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexStageBytes = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyIndexStageBytes = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveStageBytes = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyCoalescedStageRangeCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyCoalescedStageRejectCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyCoalescedStageBytes = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyCoalescedStageGapBytes = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageMapMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageMemcpyMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageCommandMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageBatchCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageBatchRangeCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageCopyCommandCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageBarrierCommandCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageScratchGrowCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageScratchGrowBytes = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyPreserveGeometryCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyPreserveIndexCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyPreservePrimitiveCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasReuseCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasUpdateCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitOnlyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitProbeCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectNoPreviousAsCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectIndexCountMismatchCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectPrimitiveCountMismatchCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectZeroIndexCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectZeroPrimitiveCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateNoPreviousAsCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateRecoveredEmptyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateSliceMovedCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateTopologyChangedCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateForceTopologyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchQueryCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchCacheHitCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchCacheMissCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchGrowCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasBuildCommandCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchBarrierCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyKeepGeometrySliceCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyKeepMaterialSliceCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyEmptyRemovalCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyRecoverAttemptCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyRecoverSuccessCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyAtlasGrowCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchRefreshCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchRebuildCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchFilteredWallOnlyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchFilteredFlatOnlyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchFilteredMixedCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchResidentWallOnlyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchResidentFlatOnlyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchResidentMixedCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedSuppressedActiveCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedSuppressionEmitCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedUniqueTouchedCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedMaterialRefreshCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedAttemptCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedSuppressedAttemptCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedUnsuppressedAttemptCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedResidentApplyCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedSuppressedResidentApplyCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedUnsuppressedResidentApplyCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedSyncSkipCount = 0;
	mLastPerfShellTraceStats.runtimeMutationActiveChunkCount = 0;
	mLastPerfShellTraceStats.runtimeMutationValidChunkCount = 0;
	mLastPerfShellTraceStats.runtimeMutationExcludedStaticChunkCount = 0;
	mLastPerfShellTraceStats.runtimeMutationCachedSurfaceCount = 0;
	mLastPerfShellTraceStats.runtimeMutationCachedTriangleCount = 0;
	mLastPerfShellTraceStats.runtimeMutationCachedMaterialCount = 0;
	mLastPerfShellTraceStats.runtimeMutationCachedMaterialStateCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialCacheHitCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialCacheMissCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialCacheStoreCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentSliceCacheEntryCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentSliceCacheHitCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentSliceCacheMissCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentSliceCacheStoreCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentSliceApplyHitCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentSliceApplyMissCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentSliceSyncSkipHitCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentGpuPayloadCacheHitCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentGpuPayloadCacheMissCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentGpuPayloadCacheStoreCount = 0;
	mLastPerfShellTraceStats.runtimeMutationPrimitiveCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialCount = 0;
	mLastPerfShellTraceStats.runtimeMutationTopEntries = {};
	mLastPerfShellTraceStats.runtimeSectorDirtyTruthEntries = {};
	mLastPerfShellTraceStats.runtimeAnimatedChurnEntries = {};
	mLastPerfShellTraceStats.runtimeMaterialOnlyMismatchEntries = {};
	mLastPerfShellTraceStats.runtimeResidentBlasRecreateEntries = {};
	mLastPerfShellTraceStats.runtimeResidentBlasRefitRejectEntries = {};
	mLastPerfShellTraceStats.runtimeStructuralRebuildEntries = {};
	mLastPerfShellTraceStats.runtimeGeometryDirtyEntries = {};
	mLastPerfShellTraceStats.runtimeRecurringChunkEntries = {};
	if (mRuntimeRecurringChunkTrackerBuildSerial != mMapWorld.buildSerial)
	{
		mRuntimeRecurringChunkTrackerBuildSerial = mMapWorld.buildSerial;
		mRuntimeRecurringChunkTrackers.clear();
	}
	if (mRuntimeRecurringChunkTrackers.size() != mMapWorld.chunks.size())
	{
		mRuntimeRecurringChunkTrackers.resize(mMapWorld.chunks.size());
	}
	struct RuntimeAnimatedFrameTraceStats
	{
		bool touched = false;
		bool suppressed = false;
		uint32_t materialRefreshes = 0;
		uint32_t runtimeAttempts = 0;
		uint32_t residentApplies = 0;
		uint32_t syncSkips = 0;
	};
	std::vector<RuntimeAnimatedFrameTraceStats> runtimeAnimatedFrameStats;
	if (tracePtPerf)
	{
		runtimeAnimatedFrameStats.resize(mResidentMapChunkRegistry.entries.size());
	}
	const auto recordRuntimeAnimatedFrame =
		[this, tracePtPerf, &runtimeAnimatedFrameStats](uint32_t chunkIndex,
			bool suppressed,
			bool materialRefresh,
			bool attempted,
			bool residentApply,
			bool syncSkip)
	{
		if (!tracePtPerf)
		{
			return;
		}
		if (chunkIndex >= runtimeAnimatedFrameStats.size())
		{
			return;
		}

		auto& entry = runtimeAnimatedFrameStats[chunkIndex];
		if (!entry.touched)
		{
			entry.touched = true;
			mLastPerfShellTraceStats.runtimeAnimatedUniqueTouchedCount++;
		}
		entry.suppressed = entry.suppressed || suppressed;
		if (materialRefresh)
		{
			entry.materialRefreshes++;
			mLastPerfShellTraceStats.runtimeAnimatedMaterialRefreshCount++;
		}
		if (attempted)
		{
			entry.runtimeAttempts++;
			mLastPerfShellTraceStats.runtimeAnimatedAttemptCount++;
			if (suppressed)
			{
				mLastPerfShellTraceStats.runtimeAnimatedSuppressedAttemptCount++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeAnimatedUnsuppressedAttemptCount++;
			}
		}
		if (residentApply)
		{
			entry.residentApplies++;
			mLastPerfShellTraceStats.runtimeAnimatedResidentApplyCount++;
			if (suppressed)
			{
				mLastPerfShellTraceStats.runtimeAnimatedSuppressedResidentApplyCount++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeAnimatedUnsuppressedResidentApplyCount++;
			}
		}
		if (syncSkip)
		{
			entry.syncSkips++;
			mLastPerfShellTraceStats.runtimeAnimatedSyncSkipCount++;
		}
	};
	const auto hasCachedResidentAnimatedSlice =
		[this](uint32_t chunkListIndex,
			uint64_t animatedGeometrySignature,
			uint64_t animatedMaterialSignature) -> bool
	{
		if (chunkListIndex >= mStaticMapScene.chunks.size())
		{
			return false;
		}

		const auto& chunkCache = mStaticMapScene.chunks[chunkListIndex];
		if (!chunkCache.active || chunkCache.materialCount == 0)
		{
			return false;
		}

		const uint64_t materialBridgeHash = HashMaterialBridgeSummary(chunkCache.materialBridge);
		for (const auto& cacheEntry : chunkCache.residentMaterialSliceCache)
		{
			if (cacheEntry.animatedGeometrySignature == animatedGeometrySignature &&
				cacheEntry.animatedMaterialSignature == animatedMaterialSignature &&
				cacheEntry.materialBridgeHash == materialBridgeHash &&
				cacheEntry.materialCount == chunkCache.materialCount)
			{
				return true;
			}
		}

		return false;
	};

	const auto recordRuntimeMutationTopEntry =
		[this](uint32_t chunkIndex,
			int32_t sectorIndex,
			uint32_t reasonMask,
			uint32_t sectionDirtyCount,
			uint32_t surfaceCount,
			uint32_t triangleCount,
			uint32_t materialCount,
			RuntimeMutationTraceAction action,
			bool forceTopology,
			bool residentMaterialDirty,
			bool residentGeometryDirty,
			bool recoveredEmpty)
	{
		if (!ShouldTracePtPerf())
		{
			return;
		}

		RuntimeMutationTopTraceEntry entry = {};
		entry.valid = true;
		entry.chunkIndex = chunkIndex;
		entry.sectorIndex = sectorIndex;
		entry.reasonMask = reasonMask;
		entry.sectionDirtyCount = sectionDirtyCount;
		entry.surfaceCount = surfaceCount;
		entry.triangleCount = triangleCount;
		entry.materialCount = materialCount;
		entry.action = action;
		entry.forceTopology = forceTopology;
		entry.residentMaterialDirty = residentMaterialDirty;
		entry.residentGeometryDirty = residentGeometryDirty;
		entry.recoveredEmpty = recoveredEmpty;
		InsertRankedTraceEntry(
			mLastPerfShellTraceStats.runtimeMutationTopEntries,
			entry,
			ScoreRuntimeMutationTopTraceEntry);
	};

	const auto recordSectorDirtyTruthEntry =
		[this](uint32_t chunkIndex,
			int32_t sectorIndex,
			uint32_t reasonMask,
			RuntimeSectorDirtyTruthTraceEntry::PreviousStateSource previousStateSource,
			bool forceTopology,
			bool baselineChanged,
			bool geometryChanged,
			bool materialChanged,
			uint32_t previousSurfaceCount,
			uint32_t liveSurfaceCount,
			uint32_t previousTriangleCount,
			uint32_t liveTriangleCount)
	{
		if (!ShouldTracePtPerf())
		{
			return;
		}

		RuntimeSectorDirtyTruthTraceEntry entry = {};
		entry.valid = true;
		entry.chunkIndex = chunkIndex;
		entry.sectorIndex = sectorIndex;
		entry.reasonMask = reasonMask;
		entry.previousStateSource = previousStateSource;
		entry.forceTopology = forceTopology;
		entry.baselineChanged = baselineChanged;
		entry.geometryChanged = geometryChanged;
		entry.materialChanged = materialChanged;
		entry.previousSurfaceCount = previousSurfaceCount;
		entry.liveSurfaceCount = liveSurfaceCount;
		entry.previousTriangleCount = previousTriangleCount;
		entry.liveTriangleCount = liveTriangleCount;
		InsertRankedTraceEntry(
			mLastPerfShellTraceStats.runtimeSectorDirtyTruthEntries,
			entry,
			ScoreRuntimeSectorDirtyTruthEntry);
	};
	const auto recordMaterialOnlyMismatchEntry =
		[this](uint32_t chunkIndex,
			int32_t sectorIndex,
			bool refreshPath,
			uint32_t reasonMask,
			uint32_t filteredSurfaceCount,
			uint32_t filteredMaterialCount,
			uint32_t residentMaterialCount,
			uint32_t filteredWallCount,
			uint32_t filteredFlatCount,
			uint32_t residentWallCount,
			uint32_t residentFlatCount)
	{
		if (!ShouldTracePtPerf())
		{
			return;
		}

		RuntimeMaterialOnlyMismatchTraceEntry entry = {};
		entry.valid = true;
		entry.chunkIndex = chunkIndex;
		entry.sectorIndex = sectorIndex;
		entry.refreshPath = refreshPath;
		entry.reasonMask = reasonMask;
		entry.filteredSurfaceCount = filteredSurfaceCount;
		entry.filteredMaterialCount = filteredMaterialCount;
		entry.residentMaterialCount = residentMaterialCount;
		entry.filteredWallCount = filteredWallCount;
		entry.filteredFlatCount = filteredFlatCount;
		entry.residentWallCount = residentWallCount;
		entry.residentFlatCount = residentFlatCount;
		InsertRankedTraceEntry(
			mLastPerfShellTraceStats.runtimeMaterialOnlyMismatchEntries,
			entry,
			ScoreRuntimeMaterialOnlyMismatchTraceEntry);
	};
	const auto recordStructuralRebuildEntry =
		[this](uint32_t chunkIndex,
			int32_t sectorIndex,
			uint32_t reasonMask,
			uint32_t triggerMask,
			uint32_t surfaceCount,
			uint32_t triangleCount,
			uint32_t materialCount,
			RuntimeMutationTraceAction action,
			bool materialOnly,
			bool sectorMaterialOnly,
			bool wallMaterialOnly,
			bool mixedMaterialOnly,
			bool geometryOrDirty)
	{
		if (!ShouldTracePtPerf())
		{
			return;
		}

		RuntimeStructuralRebuildTraceEntry entry = {};
		entry.valid = true;
		entry.chunkIndex = chunkIndex;
		entry.sectorIndex = sectorIndex;
		entry.reasonMask = reasonMask;
		entry.triggerMask = triggerMask;
		entry.surfaceCount = surfaceCount;
		entry.triangleCount = triangleCount;
		entry.materialCount = materialCount;
		entry.action = action;
		entry.materialOnly = materialOnly;
		entry.sectorMaterialOnly = sectorMaterialOnly;
		entry.wallMaterialOnly = wallMaterialOnly;
		entry.mixedMaterialOnly = mixedMaterialOnly;
		entry.geometryOrDirty = geometryOrDirty;
		InsertRankedTraceEntry(
			mLastPerfShellTraceStats.runtimeStructuralRebuildEntries,
			entry,
			ScoreRuntimeStructuralRebuildTraceEntry);
	};
	const auto recordGeometryDirtyEntry =
		[this](uint32_t chunkIndex,
			int32_t sectorIndex,
			uint32_t reasonMask,
			uint32_t previousWallCount,
			uint32_t liveWallCount,
			uint32_t previousFlatCount,
			uint32_t liveFlatCount,
			uint32_t previousTriangleCount,
			uint32_t liveTriangleCount,
			uint32_t previousMaterialCount,
			uint32_t liveMaterialCount,
			bool forceTopology)
	{
		const bool sectorGeometry = (reasonMask & nri_scene::PTMapChunkMutationReason_SectorGeometry) != 0;
		const bool wallGeometry = (reasonMask & nri_scene::PTMapChunkMutationReason_WallGeometry) != 0;
		const bool dirtyOnlyBits =
			(reasonMask &
				(nri_scene::PTMapChunkMutationReason_SectorDirty |
				 nri_scene::PTMapChunkMutationReason_SectionDirty |
				 nri_scene::PTMapChunkMutationReason_Dragged)) != 0;
		const bool wallsChanged = previousWallCount != liveWallCount;
		const bool flatsChanged = previousFlatCount != liveFlatCount;
		const bool countChanged =
			wallsChanged ||
			flatsChanged ||
			previousTriangleCount != liveTriangleCount ||
			previousMaterialCount != liveMaterialCount;

		uint32_t familyMask = 0;
		if (sectorGeometry && !wallGeometry && !dirtyOnlyBits)
		{
			familyMask |= RuntimeGeometryDirtyFamily_SectorGeometryOnly;
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtySectorGeometryOnlyChunks++;
		}
		else if (!sectorGeometry && wallGeometry && !dirtyOnlyBits)
		{
			familyMask |= RuntimeGeometryDirtyFamily_WallGeometryOnly;
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtyWallGeometryOnlyChunks++;
		}
		else if (sectorGeometry && wallGeometry && !dirtyOnlyBits)
		{
			familyMask |= RuntimeGeometryDirtyFamily_SectorWallGeometry;
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtySectorWallGeometryChunks++;
		}
		else if (!sectorGeometry && !wallGeometry && dirtyOnlyBits)
		{
			familyMask |= RuntimeGeometryDirtyFamily_DirtyOnly;
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtyDirtyOnlyChunks++;
		}
		else
		{
			familyMask |= RuntimeGeometryDirtyFamily_GeometryDirtyMixed;
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtyGeometryDirtyMixedChunks++;
		}

		if (forceTopology && !countChanged)
		{
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtyForceTopologyOnlyChunks++;
		}
		if (countChanged)
		{
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtyRealCountChangeChunks++;
		}
		if (wallsChanged && !flatsChanged)
		{
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtyWallsOnlyChangedChunks++;
		}
		else if (!wallsChanged && flatsChanged)
		{
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtyFlatsOnlyChangedChunks++;
		}
		else if (wallsChanged && flatsChanged)
		{
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtyWallsAndFlatsChangedChunks++;
		}

		if (countChanged &&
			chunkIndex < mRuntimeRecurringChunkTrackers.size())
		{
			auto& tracker = mRuntimeRecurringChunkTrackers[chunkIndex];
			const uint64_t stateSignature = ComputeRecurringChunkStateSignature(
				reasonMask,
				liveWallCount,
				liveFlatCount,
				liveTriangleCount,
				liveMaterialCount);
			if (!tracker.valid)
			{
				tracker.valid = true;
				tracker.chunkIndex = chunkIndex;
				tracker.sectorIndex = sectorIndex;
			}
			tracker.lastReasonMask = reasonMask;
			tracker.visitCount++;
			tracker.lastWallCount = liveWallCount;
			tracker.lastFlatCount = liveFlatCount;
			tracker.lastTriangleCount = liveTriangleCount;
			tracker.lastMaterialCount = liveMaterialCount;

			bool seenBefore = false;
			for (uint32_t i = 0; i < tracker.uniqueStateCount; ++i)
			{
				if (tracker.seenStateSignatures[i] == stateSignature)
				{
					seenBefore = true;
					break;
				}
			}
			if (!seenBefore && tracker.uniqueStateCount < tracker.seenStateSignatures.size())
			{
				tracker.seenStateSignatures[tracker.uniqueStateCount++] = stateSignature;
			}
			if (tracker.lastStateSignature != 0 &&
				tracker.lastStateSignature != stateSignature)
			{
				tracker.transitionCount++;
				if (seenBefore)
				{
					tracker.repeatedStateHitCount++;
				}
				if (tracker.previousStateSignature != 0 &&
					tracker.previousStateSignature == stateSignature)
				{
					tracker.abaRecurrenceCount++;
				}
			}
			tracker.previousStateSignature = tracker.lastStateSignature;
			tracker.lastStateSignature = stateSignature;
		}

		if (!ShouldTracePtPerf())
		{
			return;
		}

		RuntimeGeometryDirtyTraceEntry entry = {};
		entry.valid = true;
		entry.chunkIndex = chunkIndex;
		entry.sectorIndex = sectorIndex;
		entry.reasonMask = reasonMask;
		entry.familyMask = familyMask;
		entry.previousWallCount = previousWallCount;
		entry.liveWallCount = liveWallCount;
		entry.previousFlatCount = previousFlatCount;
		entry.liveFlatCount = liveFlatCount;
		entry.previousTriangleCount = previousTriangleCount;
		entry.liveTriangleCount = liveTriangleCount;
		entry.previousMaterialCount = previousMaterialCount;
		entry.liveMaterialCount = liveMaterialCount;
		entry.forceTopology = forceTopology;
		entry.countChanged = countChanged;
		entry.wallsChanged = wallsChanged;
		entry.flatsChanged = flatsChanged;
		InsertRankedTraceEntry(
			mLastPerfShellTraceStats.runtimeGeometryDirtyEntries,
			entry,
			ScoreRuntimeGeometryDirtyTraceEntry);
	};
	if (!mStaticMapScene.valid ||
		mRuntimeMutation.cache.chunks.size() != mMapWorld.chunks.size())
	{
		return false;
	}
	static constexpr uint8_t kVisibleResidentValidationWindow = 8;
	static constexpr uint8_t kVisibleResidentUnresolvedTextureValidationWindow = 64;
	static constexpr uint8_t kStartupVisibleResidentValidationWindow = 64;
	if (mRuntimeMutation.signatureWatchlistBuildSerial != mMapWorld.buildSerial ||
		mRuntimeMutation.signatureWatchlist.size() != mMapWorld.chunks.size())
	{
		mRuntimeMutation.signatureWatchlist.assign(mMapWorld.chunks.size(), 0u);
		mRuntimeMutation.signatureWatchlistBuildSerial = mMapWorld.buildSerial;
		mRuntimeMutation.worklistSweepCursor = 0;
	}
	const auto isVisibleSuppressedStaticAnimatedChunk = [&](uint32_t mapChunkIndex) -> bool
	{
		if (!IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunkIndex))
		{
			return false;
		}

		if (mapChunkIndex < mResidentMapChunkRegistry.entries.size())
		{
			const auto& entry = mResidentMapChunkRegistry.entries[mapChunkIndex];
			return
				entry.valid &&
				entry.hasAnimatedTextureCandidates &&
				entry.animatedRefreshSuppressed;
		}

		return false;
	};
	for (const auto& mapChunk : mMapWorld.chunks)
	{
		if (mapChunk.chunkIndex < mPendingStartupVisibleChunkValidation.size() &&
			mPendingStartupVisibleChunkValidation[mapChunk.chunkIndex] != 0u &&
			!IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunk.chunkIndex))
		{
			mPendingStartupVisibleChunkValidation[mapChunk.chunkIndex] = 0u;
		}
	}
	const bool runtimeMutationWorklistEnabled = runtimeMutationSettings.worklistEnabled;
	std::vector<uint32_t> runtimeMutationCandidateSourceMasks(mMapWorld.chunks.size(), 0u);
	uint32_t runtimeMutationCandidateCount = 0;
	uint32_t runtimeMutationSignatureWatchlistCandidateCount = 0;
	uint32_t runtimeMutationBackgroundSweepCandidateCount = 0;
	enum class RuntimeMutationDistanceTier : uint8_t
	{
		Unknown,
		Near,
		Far,
	};
	const float runtimeMutationNearDistance = runtimeMutationSettings.nearDistance;
	const float runtimeMutationNearDistanceSquared = runtimeMutationNearDistance * runtimeMutationNearDistance;
	const auto getRuntimeMutationDistanceTier = [&](uint32_t chunkListIndex, bool chunkVisibleNow) -> RuntimeMutationDistanceTier
	{
		if (chunkVisibleNow)
		{
			return RuntimeMutationDistanceTier::Near;
		}
		if (chunkListIndex >= mMapWorld.chunks.size())
		{
			return RuntimeMutationDistanceTier::Unknown;
		}

		const auto& bounds = mMapWorld.chunks[chunkListIndex].bounds;
		if (!bounds.valid)
		{
			return RuntimeMutationDistanceTier::Unknown;
		}

		float distanceSquared = 0.0f;
		for (int axis = 0; axis < 3; ++axis)
		{
			float distance = 0.0f;
			if (mCurrentCameraPos[axis] < bounds.min[axis])
			{
				distance = bounds.min[axis] - mCurrentCameraPos[axis];
			}
			else if (mCurrentCameraPos[axis] > bounds.max[axis])
			{
				distance = mCurrentCameraPos[axis] - bounds.max[axis];
			}
			distanceSquared += distance * distance;
		}
		return distanceSquared <= runtimeMutationNearDistanceSquared ?
			RuntimeMutationDistanceTier::Near :
			RuntimeMutationDistanceTier::Far;
	};
	const auto markWorklistCandidate =
		[&](uint32_t chunkListIndex, uint32_t sourceMask)
	{
		if (chunkListIndex >= runtimeMutationCandidateSourceMasks.size() || sourceMask == 0)
		{
			return;
		}
		if (runtimeMutationCandidateSourceMasks[chunkListIndex] == 0)
		{
			runtimeMutationCandidateCount++;
			const uint32_t mapChunkIndex = chunkListIndex < mMapWorld.chunks.size() ? mMapWorld.chunks[chunkListIndex].chunkIndex : chunkListIndex;
			const bool chunkVisibleNow = IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunkIndex);
			const RuntimeMutationDistanceTier distanceTier = getRuntimeMutationDistanceTier(chunkListIndex, chunkVisibleNow);
			if (chunkVisibleNow)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateVisibleChunks++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateInvisibleChunks++;
				switch (distanceTier)
				{
				case RuntimeMutationDistanceTier::Near:
					mLastPerfShellTraceStats.runtimeMutationCandidateNearChunks++;
					break;
				case RuntimeMutationDistanceTier::Far:
					mLastPerfShellTraceStats.runtimeMutationCandidateFarChunks++;
					break;
				default:
					mLastPerfShellTraceStats.runtimeMutationCandidateUnknownDistanceChunks++;
					break;
				}
			}
			if (chunkListIndex < mMapWorld.chunks.size() && mMapWorld.chunks[chunkListIndex].bounds.valid)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateBoundsValidChunks++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateBoundsInvalidChunks++;
			}
		}
		if ((runtimeMutationCandidateSourceMasks[chunkListIndex] & sourceMask) == 0)
		{
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_ActiveReplacement) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateActiveReplacementChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_VisibleResidentValidation) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateVisibleResidentValidationChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_StartupVisibleValidation) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateStartupVisibleValidationChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_UnresolvedAuthoredTextures) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateUnresolvedTextureChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_StaticAnimatedSuppressed) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateStaticAnimatedSuppressedChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_SectorDirty) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateSectorDirtyChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_SectionDirty) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateSectionDirtyChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_Dragged) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateDraggedChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_SignatureWatchlist) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateSignatureWatchChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_BackgroundSweep) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateBackgroundSweepSourceChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_DeferredStructuralRebuild) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateDeferredStructuralChunks++;
			}
		}
		runtimeMutationCandidateSourceMasks[chunkListIndex] |= sourceMask;
	};
	struct RuntimeMutationWorklistValidationState
	{
		bool enabled = false;
		int verbosity = 0;
		uint32_t candidateCount = 0;
		uint32_t fullDirtyCount = 0;
		uint32_t falseNegativeCount = 0;
		uint32_t falsePositiveCount = 0;
		uint32_t falseNegativeTraceCount = 0;
		uint32_t signatureWatchlistCandidateCount = 0;
		uint32_t signatureWatchlistSeedCount = 0;
		uint32_t backgroundSweepCandidateCount = 0;
		std::array<uint32_t, 128> falseNegativeReasonMaskCounts = {};
		std::vector<uint32_t> sourceMasks;
		std::vector<uint8_t> fullDirty;
	};
	RuntimeMutationWorklistValidationState worklistValidation = {};
	worklistValidation.verbosity = (int)nri_ptmutationworklistvalidate;
	worklistValidation.enabled = worklistValidation.verbosity > 0;
	const auto seedRuntimeMutationSignatureWatchlist = [&](size_t chunkIndex) -> bool
	{
		if (chunkIndex >= mRuntimeMutation.signatureWatchlist.size() ||
			mRuntimeMutation.signatureWatchlist[chunkIndex] != 0u)
		{
			return false;
		}

		mRuntimeMutation.signatureWatchlist[chunkIndex] = 1u;
		return true;
	};
	for (uint32_t candidateListIndex = 0; candidateListIndex < (uint32_t)mMapWorld.chunks.size(); ++candidateListIndex)
	{
		const auto& mapChunk = mMapWorld.chunks[candidateListIndex];
		const auto& replacement = mRuntimeMutation.cache.chunks[candidateListIndex];
		if (replacement.active || (replacement.valid && !replacement.residentAuthoritative))
		{
			markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_ActiveReplacement);
		}

		const bool chunkVisibleNow = IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunk.chunkIndex);
		const bool startupVisibleValidationPending =
			mapChunk.chunkIndex < mPendingStartupVisibleChunkValidation.size() &&
			mPendingStartupVisibleChunkValidation[mapChunk.chunkIndex] != 0u;
		const bool chunkHasUnresolvedAuthoredTextures =
			chunkVisibleNow &&
			ChunkHasUnresolvedAuthoredTextures(mapChunk);
		const ResidentMapChunkRegistry::Entry* residentEntry =
			mapChunk.chunkIndex < mResidentMapChunkRegistry.entries.size() ?
			&mResidentMapChunkRegistry.entries[mapChunk.chunkIndex] :
			nullptr;
		if (residentEntry != nullptr &&
			residentEntry->valid &&
			chunkVisibleNow &&
			(!residentEntry->wasVisibleLastFrame || residentEntry->visibleValidationFramesRemaining > 0))
		{
			markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_VisibleResidentValidation);
		}
		if (chunkVisibleNow && startupVisibleValidationPending)
		{
			markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_StartupVisibleValidation);
		}
		if (chunkHasUnresolvedAuthoredTextures)
		{
			markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_UnresolvedAuthoredTextures);
		}
		if (isVisibleSuppressedStaticAnimatedChunk(mapChunk.chunkIndex))
		{
			markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_StaticAnimatedSuppressed);
		}
		if (replacement.deferredMaterialRefresh)
		{
			markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_DeferredMaterialRefresh);
		}
		if (replacement.deferredStructuralRebuild)
		{
			markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_DeferredStructuralRebuild);
		}
		if (candidateListIndex < mRuntimeMutation.signatureWatchlist.size() &&
			mRuntimeMutation.signatureWatchlist[candidateListIndex] != 0u)
		{
			const uint64_t liveSignature = nri_scene::ComputeMapChunkGeometrySignature(mapChunk);
			if (liveSignature != replacement.baselineSignature)
			{
				markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_SignatureWatchlist);
				runtimeMutationSignatureWatchlistCandidateCount++;
			}
		}
		if (mapChunk.sectorIndex >= 0 && (unsigned)mapChunk.sectorIndex < sector.Size())
		{
			const auto& sec = sector[(unsigned)mapChunk.sectorIndex];
			if (sec.dirty != 0)
			{
				markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_SectorDirty);
			}
			if ((sec.exflags & SECTOREX_DRAGGED) != 0)
			{
				markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_Dragged);
			}
		}
		for (int sectionIndex : replacement.baseline.sectionIndices)
		{
			if ((unsigned)sectionIndex < sections.Size() && sections[sectionIndex].dirty != 0)
			{
				markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_SectionDirty);
				break;
			}
		}
	}

	if (runtimeMutationWorklistEnabled && !mMapWorld.chunks.empty())
	{
		const uint32_t sweepBudget = runtimeMutationSettings.worklistSweepBudget;
		const uint32_t chunkCount = (uint32_t)mMapWorld.chunks.size();
		const uint32_t sweepCount = std::min(sweepBudget, chunkCount);
		for (uint32_t sweepOffset = 0; sweepOffset < sweepCount; ++sweepOffset)
		{
			const uint32_t chunkListIndex = (mRuntimeMutation.worklistSweepCursor + sweepOffset) % chunkCount;
			if ((runtimeMutationCandidateSourceMasks[chunkListIndex] & RuntimeMutationWorklistCandidateSource_BackgroundSweep) == 0)
			{
				runtimeMutationBackgroundSweepCandidateCount++;
			}
			markWorklistCandidate(chunkListIndex, RuntimeMutationWorklistCandidateSource_BackgroundSweep);
		}
		mRuntimeMutation.worklistSweepCursor = (mRuntimeMutation.worklistSweepCursor + sweepCount) % chunkCount;
	}
	mLastPerfShellTraceStats.runtimeMutationCandidateChunks = runtimeMutationCandidateCount;
	mLastPerfShellTraceStats.runtimeMutationBackgroundSweepChunks = runtimeMutationBackgroundSweepCandidateCount;
	if (worklistValidation.enabled)
	{
		worklistValidation.candidateCount = runtimeMutationCandidateCount;
		worklistValidation.signatureWatchlistCandidateCount = runtimeMutationSignatureWatchlistCandidateCount;
		worklistValidation.backgroundSweepCandidateCount = runtimeMutationBackgroundSweepCandidateCount;
		worklistValidation.sourceMasks = runtimeMutationCandidateSourceMasks;
		worklistValidation.fullDirty.resize(mMapWorld.chunks.size(), 0u);
	}
	const auto getRuntimeMutationCandidateSourceMask = [&](size_t chunkIndex) -> uint32_t
	{
		return chunkIndex < runtimeMutationCandidateSourceMasks.size() ? runtimeMutationCandidateSourceMasks[chunkIndex] : 0u;
	};
	const auto recordRuntimeMutationDirtyTier = [&](bool chunkVisibleNow, RuntimeMutationDistanceTier distanceTier, uint32_t sourceMask)
	{
		if (chunkVisibleNow)
		{
			mLastPerfShellTraceStats.runtimeMutationDirtyVisibleChunks++;
		}
		else
		{
			mLastPerfShellTraceStats.runtimeMutationDirtyInvisibleChunks++;
			switch (distanceTier)
			{
			case RuntimeMutationDistanceTier::Near:
				mLastPerfShellTraceStats.runtimeMutationDirtyNearChunks++;
				break;
			case RuntimeMutationDistanceTier::Far:
				mLastPerfShellTraceStats.runtimeMutationDirtyFarChunks++;
				break;
			default:
				mLastPerfShellTraceStats.runtimeMutationDirtyUnknownDistanceChunks++;
				break;
			}
		}
		if ((sourceMask & RuntimeMutationWorklistCandidateSource_ActiveReplacement) != 0)
		{
			mLastPerfShellTraceStats.runtimeMutationDirtyActiveReplacementChunks++;
		}
		if ((sourceMask & RuntimeMutationWorklistCandidateSource_BackgroundSweep) != 0)
		{
			mLastPerfShellTraceStats.runtimeMutationDirtyBackgroundSweepChunks++;
		}
	};
	const auto recordRuntimeMutationStructuralTier = [&](bool chunkVisibleNow, RuntimeMutationDistanceTier distanceTier, uint32_t sourceMask)
	{
		if (chunkVisibleNow)
		{
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildVisibleChunks++;
		}
		else
		{
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildInvisibleChunks++;
			switch (distanceTier)
			{
			case RuntimeMutationDistanceTier::Near:
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildNearChunks++;
				break;
			case RuntimeMutationDistanceTier::Far:
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildFarChunks++;
				break;
			default:
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildUnknownDistanceChunks++;
				break;
			}
		}
		if ((sourceMask & RuntimeMutationWorklistCandidateSource_ActiveReplacement) != 0)
		{
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildActiveReplacementChunks++;
		}
		if ((sourceMask & RuntimeMutationWorklistCandidateSource_BackgroundSweep) != 0)
		{
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildBackgroundSweepChunks++;
		}
	};
	const auto recordRuntimeMutationMaterialTier = [&](bool chunkVisibleNow, RuntimeMutationDistanceTier distanceTier, uint32_t sourceMask)
	{
		if (chunkVisibleNow)
		{
			mLastPerfShellTraceStats.runtimeMutationMaterialRefreshVisibleChunks++;
		}
		else
		{
			mLastPerfShellTraceStats.runtimeMutationMaterialRefreshInvisibleChunks++;
			switch (distanceTier)
			{
			case RuntimeMutationDistanceTier::Near:
				mLastPerfShellTraceStats.runtimeMutationMaterialRefreshNearChunks++;
				break;
			case RuntimeMutationDistanceTier::Far:
				mLastPerfShellTraceStats.runtimeMutationMaterialRefreshFarChunks++;
				break;
			default:
				mLastPerfShellTraceStats.runtimeMutationMaterialRefreshUnknownDistanceChunks++;
				break;
			}
		}
		if ((sourceMask & RuntimeMutationWorklistCandidateSource_ActiveReplacement) != 0)
		{
			mLastPerfShellTraceStats.runtimeMutationMaterialRefreshActiveReplacementChunks++;
		}
		if ((sourceMask & RuntimeMutationWorklistCandidateSource_BackgroundSweep) != 0)
		{
			mLastPerfShellTraceStats.runtimeMutationMaterialRefreshBackgroundSweepChunks++;
		}
	};
	const auto recordRuntimeMutationResidentApplyTier = [&](bool chunkVisibleNow, RuntimeMutationDistanceTier distanceTier, uint32_t sourceMask)
	{
		if (chunkVisibleNow)
		{
			mLastPerfShellTraceStats.runtimeMutationResidentApplyVisibleChunks++;
		}
		else
		{
			mLastPerfShellTraceStats.runtimeMutationResidentApplyInvisibleChunks++;
			switch (distanceTier)
			{
			case RuntimeMutationDistanceTier::Near:
				mLastPerfShellTraceStats.runtimeMutationResidentApplyNearChunks++;
				break;
			case RuntimeMutationDistanceTier::Far:
				mLastPerfShellTraceStats.runtimeMutationResidentApplyFarChunks++;
				break;
			default:
				mLastPerfShellTraceStats.runtimeMutationResidentApplyUnknownDistanceChunks++;
				break;
			}
		}
		if ((sourceMask & RuntimeMutationWorklistCandidateSource_ActiveReplacement) != 0)
		{
			mLastPerfShellTraceStats.runtimeMutationResidentApplyActiveReplacementChunks++;
		}
		if ((sourceMask & RuntimeMutationWorklistCandidateSource_BackgroundSweep) != 0)
		{
			mLastPerfShellTraceStats.runtimeMutationResidentApplyBackgroundSweepChunks++;
		}
	};
	const bool deferNearInvisibleStructuralRebuilds = runtimeMutationSettings.deferNearInvisibleStructuralRebuilds;
	const bool deferFarInvisibleStructuralRebuilds = runtimeMutationSettings.deferFarStructuralRebuilds;
	const uint32_t nearInvisibleStructuralBudget = runtimeMutationSettings.nearInvisibleStructuralBudget;
	const uint32_t farStructuralBudget = runtimeMutationSettings.farStructuralBudget;
	uint32_t nearInvisibleStructuralBudgetReserved = 0;
	uint32_t farStructuralBudgetReserved = 0;
	std::vector<uint8_t> nearInvisibleStructuralBudgetAllowed(mMapWorld.chunks.size(), 0u);
	std::vector<uint8_t> farStructuralBudgetAllowed(mMapWorld.chunks.size(), 0u);
	if ((deferNearInvisibleStructuralRebuilds && nearInvisibleStructuralBudget > 0) ||
		(deferFarInvisibleStructuralRebuilds && farStructuralBudget > 0))
	{
		struct DeferredStructuralBudgetCandidate
		{
			uint64_t deferredFrame = 0;
			uint32_t chunkListIndex = 0;
		};
		std::vector<DeferredStructuralBudgetCandidate> deferredNearStructuralCandidates;
		std::vector<DeferredStructuralBudgetCandidate> deferredFarStructuralCandidates;
		for (uint32_t candidateListIndex = 0; candidateListIndex < (uint32_t)mMapWorld.chunks.size(); ++candidateListIndex)
		{
			const auto& replacement = mRuntimeMutation.cache.chunks[candidateListIndex];
			if (!replacement.deferredStructuralRebuild)
			{
				continue;
			}
			const auto& mapChunk = mMapWorld.chunks[candidateListIndex];
			const bool chunkVisibleNow = IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunk.chunkIndex);
			if (chunkVisibleNow)
			{
				continue;
			}

			const RuntimeMutationDistanceTier distanceTier = getRuntimeMutationDistanceTier(candidateListIndex, false);
			if (distanceTier == RuntimeMutationDistanceTier::Near && deferNearInvisibleStructuralRebuilds)
			{
				deferredNearStructuralCandidates.push_back({ replacement.deferredStructuralFrame, candidateListIndex });
			}
			else if (distanceTier == RuntimeMutationDistanceTier::Far && deferFarInvisibleStructuralRebuilds)
			{
				deferredFarStructuralCandidates.push_back({ replacement.deferredStructuralFrame, candidateListIndex });
			}
		}
		const auto sortDeferredStructuralCandidates =
			[](std::vector<DeferredStructuralBudgetCandidate>& candidates)
			{
				std::sort(
					candidates.begin(),
					candidates.end(),
					[](const DeferredStructuralBudgetCandidate& left, const DeferredStructuralBudgetCandidate& right)
					{
						if (left.deferredFrame != right.deferredFrame)
						{
							return left.deferredFrame < right.deferredFrame;
						}
						return left.chunkListIndex < right.chunkListIndex;
					});
			};
		sortDeferredStructuralCandidates(deferredNearStructuralCandidates);
		sortDeferredStructuralCandidates(deferredFarStructuralCandidates);
		for (const auto& candidate : deferredNearStructuralCandidates)
		{
			if (nearInvisibleStructuralBudgetReserved >= nearInvisibleStructuralBudget)
			{
				break;
			}
			nearInvisibleStructuralBudgetAllowed[candidate.chunkListIndex] = 1u;
			nearInvisibleStructuralBudgetReserved++;
		}
		for (const auto& candidate : deferredFarStructuralCandidates)
		{
			if (farStructuralBudgetReserved >= farStructuralBudget)
			{
				break;
			}
			farStructuralBudgetAllowed[candidate.chunkListIndex] = 1u;
			farStructuralBudgetReserved++;
		}
	}
	const auto reserveInvisibleStructuralBudget =
		[&](size_t chunkIndex, RuntimeMutationDistanceTier distanceTier) -> bool
	{
		if (distanceTier == RuntimeMutationDistanceTier::Near)
		{
			if (!deferNearInvisibleStructuralRebuilds || nearInvisibleStructuralBudget == 0)
			{
				return false;
			}
			if (chunkIndex < nearInvisibleStructuralBudgetAllowed.size() &&
				nearInvisibleStructuralBudgetAllowed[chunkIndex] != 0u)
			{
				return true;
			}
			if (nearInvisibleStructuralBudgetReserved >= nearInvisibleStructuralBudget ||
				chunkIndex >= nearInvisibleStructuralBudgetAllowed.size())
			{
				return false;
			}

			nearInvisibleStructuralBudgetAllowed[chunkIndex] = 1u;
			nearInvisibleStructuralBudgetReserved++;
			return true;
		}
		if (distanceTier == RuntimeMutationDistanceTier::Far)
		{
			if (!deferFarInvisibleStructuralRebuilds || farStructuralBudget == 0)
			{
				return false;
			}
			if (chunkIndex < farStructuralBudgetAllowed.size() &&
				farStructuralBudgetAllowed[chunkIndex] != 0u)
			{
				return true;
			}
			if (farStructuralBudgetReserved >= farStructuralBudget ||
				chunkIndex >= farStructuralBudgetAllowed.size())
			{
				return false;
			}

			farStructuralBudgetAllowed[chunkIndex] = 1u;
			farStructuralBudgetReserved++;
			return true;
		}

		return false;
	};
	const bool deferNearInvisibleMaterialRefreshes = runtimeMutationSettings.deferNearInvisibleMaterialRefreshes;
	const uint32_t nearInvisibleMaterialBudget = runtimeMutationSettings.nearInvisibleMaterialBudget;
	uint32_t nearInvisibleMaterialBudgetReserved = 0;
	std::vector<uint8_t> nearInvisibleMaterialBudgetAllowed(mMapWorld.chunks.size(), 0u);
	if (deferNearInvisibleMaterialRefreshes && nearInvisibleMaterialBudget > 0)
	{
		struct DeferredMaterialBudgetCandidate
		{
			uint64_t deferredFrame = 0;
			uint32_t chunkListIndex = 0;
		};
		std::vector<DeferredMaterialBudgetCandidate> deferredNearMaterialCandidates;
		for (uint32_t candidateListIndex = 0; candidateListIndex < (uint32_t)mMapWorld.chunks.size(); ++candidateListIndex)
		{
			const auto& replacement = mRuntimeMutation.cache.chunks[candidateListIndex];
			if (!replacement.deferredMaterialRefresh)
			{
				continue;
			}
			const auto& mapChunk = mMapWorld.chunks[candidateListIndex];
			const bool chunkVisibleNow = IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunk.chunkIndex);
			if (chunkVisibleNow ||
				getRuntimeMutationDistanceTier(candidateListIndex, false) != RuntimeMutationDistanceTier::Near)
			{
				continue;
			}

			deferredNearMaterialCandidates.push_back({ replacement.deferredMaterialFrame, candidateListIndex });
		}
		std::sort(
			deferredNearMaterialCandidates.begin(),
			deferredNearMaterialCandidates.end(),
			[](const DeferredMaterialBudgetCandidate& left, const DeferredMaterialBudgetCandidate& right)
			{
				if (left.deferredFrame != right.deferredFrame)
				{
					return left.deferredFrame < right.deferredFrame;
				}
				return left.chunkListIndex < right.chunkListIndex;
			});
		for (const auto& candidate : deferredNearMaterialCandidates)
		{
			if (nearInvisibleMaterialBudgetReserved >= nearInvisibleMaterialBudget)
			{
				break;
			}
			nearInvisibleMaterialBudgetAllowed[candidate.chunkListIndex] = 1u;
			nearInvisibleMaterialBudgetReserved++;
		}
	}
	const auto reserveNearInvisibleMaterialBudget =
		[&](size_t chunkIndex) -> bool
	{
		if (!deferNearInvisibleMaterialRefreshes || nearInvisibleMaterialBudget == 0)
		{
			return false;
		}
		if (chunkIndex < nearInvisibleMaterialBudgetAllowed.size() &&
			nearInvisibleMaterialBudgetAllowed[chunkIndex] != 0u)
		{
			return true;
		}
		if (nearInvisibleMaterialBudgetReserved >= nearInvisibleMaterialBudget ||
			chunkIndex >= nearInvisibleMaterialBudgetAllowed.size())
		{
			return false;
		}

		nearInvisibleMaterialBudgetAllowed[chunkIndex] = 1u;
		nearInvisibleMaterialBudgetReserved++;
		return true;
	};
	const auto recordDeferredMaterialTier =
		[&](RuntimeMutationDistanceTier distanceTier, bool coalesced)
	{
		if (coalesced)
		{
			mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredCoalescedChunks++;
		}
		else
		{
			mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredChunks++;
		}
		if (distanceTier == RuntimeMutationDistanceTier::Near)
		{
			if (coalesced)
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredNearCoalescedChunks++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredNearChunks++;
			}
		}
	};
	const auto recordFlushedDeferredMaterialTier =
		[&](RuntimeMutationDistanceTier distanceTier)
	{
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredFlushedChunks++;
		if (distanceTier == RuntimeMutationDistanceTier::Near)
		{
			mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredNearFlushedChunks++;
		}
	};
	const auto recordDeferredStructuralTier =
		[&](RuntimeMutationDistanceTier distanceTier, bool coalesced)
	{
		if (coalesced)
		{
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredCoalescedChunks++;
		}
		else
		{
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredChunks++;
		}
		switch (distanceTier)
		{
		case RuntimeMutationDistanceTier::Near:
			if (coalesced)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearCoalescedChunks++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearChunks++;
			}
			break;
		case RuntimeMutationDistanceTier::Far:
			if (coalesced)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarCoalescedChunks++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarChunks++;
			}
			break;
		default:
			break;
		}
	};
	const auto recordFlushedDeferredStructuralTier =
		[&](RuntimeMutationDistanceTier distanceTier)
	{
		mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFlushedChunks++;
		switch (distanceTier)
		{
		case RuntimeMutationDistanceTier::Near:
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearFlushedChunks++;
			break;
		case RuntimeMutationDistanceTier::Far:
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarFlushedChunks++;
			break;
		default:
			break;
		}
	};
	double ignoredRuntimeMutationDistanceTierMs = 0.0;

	for (size_t chunkIndex = 0; chunkIndex < mMapWorld.chunks.size(); ++chunkIndex)
	{
		const auto& mapChunk = mMapWorld.chunks[chunkIndex];
		auto& replacement = mRuntimeMutation.cache.chunks[chunkIndex];
		const bool chunkVisibleNow = IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunk.chunkIndex);
		bool startupVisibleValidationPending =
			mapChunk.chunkIndex < mPendingStartupVisibleChunkValidation.size() &&
			mPendingStartupVisibleChunkValidation[mapChunk.chunkIndex] != 0u;
		if (!chunkVisibleNow && startupVisibleValidationPending)
		{
			mPendingStartupVisibleChunkValidation[mapChunk.chunkIndex] = 0u;
			startupVisibleValidationPending = false;
		}
		const bool chunkHasUnresolvedAuthoredTextures =
			chunkVisibleNow &&
			ChunkHasUnresolvedAuthoredTextures(mapChunk);
		ResidentMapChunkRegistry::Entry* residentEntry =
			mapChunk.chunkIndex < mResidentMapChunkRegistry.entries.size() ?
			&mResidentMapChunkRegistry.entries[mapChunk.chunkIndex] :
			nullptr;
		if (residentEntry != nullptr && residentEntry->valid)
		{
			if (!chunkVisibleNow)
			{
				if (mapChunk.chunkIndex < mPendingStartupVisibleChunkValidation.size())
				{
					mPendingStartupVisibleChunkValidation[mapChunk.chunkIndex] = 0u;
				}
				residentEntry->wasVisibleLastFrame = false;
				residentEntry->visibleValidationFramesRemaining = 0;
				residentEntry->visibleValidationTraceEmitted = false;
			}
			else
			{
				if (!residentEntry->wasVisibleLastFrame)
				{
					residentEntry->visibleValidationFramesRemaining = kVisibleResidentValidationWindow;
				}
				if (startupVisibleValidationPending)
				{
					residentEntry->visibleValidationFramesRemaining = std::max<uint8_t>(
						residentEntry->visibleValidationFramesRemaining,
						kStartupVisibleResidentValidationWindow);
				}
				if (chunkHasUnresolvedAuthoredTextures)
				{
					residentEntry->visibleValidationFramesRemaining = std::max<uint8_t>(
						residentEntry->visibleValidationFramesRemaining,
						kVisibleResidentUnresolvedTextureValidationWindow);
				}
				residentEntry->wasVisibleLastFrame = true;
			}
		}
		const bool processChunk =
			!runtimeMutationWorklistEnabled ||
			worklistValidation.enabled ||
			(chunkIndex < runtimeMutationCandidateSourceMasks.size() &&
				runtimeMutationCandidateSourceMasks[chunkIndex] != 0);
		if (!processChunk)
		{
			continue;
		}
		const uint32_t runtimeMutationCandidateSourceMask = getRuntimeMutationCandidateSourceMask(chunkIndex);
		const RuntimeMutationDistanceTier runtimeMutationDistanceTier =
			getRuntimeMutationDistanceTier((uint32_t)chunkIndex, chunkVisibleNow);

		const uint32_t previousReasonMask = replacement.reasonMask;
		nri_scene::PTMapChunkMutationAnalysis analysis = {};
		const bool analyzed = [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationAnalyzeMs);
			mLastPerfShellTraceStats.runtimeMutationAnalyzedChunks++;
			return nri_scene::AnalyzeMapChunkMutation(mapChunk, replacement.baseline, analysis);
		}();
		if (!analyzed)
		{
			replacement.active = false;
			replacement.reasonMask = nri_scene::PTMapChunkMutationReason_None;
			replacement.sectionDirtyCount = 0;
			replacement.stableMutationFrameCount = 0;
			replacement.sectorDirty = false;
			replacement.dragged = false;
			replacement.blindSpot = false;
			replacement.excludeStaticChunk = false;
			replacement.staticAnimatedReplacement = false;
			replacement.animationOnlyRefreshed = false;
			replacement.animatedMaterialSignature = 0;
			mRuntimeMutation.TraceChunk(mapChunk, replacement, ShouldEmitTemporalTraceLogs(), (int)nri_ptmutationtracechunk, (int)nri_ptmutationtracesector);
			mRuntimeMutation.ClearReplacementPayload(replacement, replacement.residentAuthoritative);
			continue;
		}

		replacement.liveSignature = analysis.signature;
		uint32_t normalizedReasonMask = analysis.reasonMask;
		uint32_t normalizedSectionDirtyCount = analysis.sectionDirtyCount;
		bool normalizedSectorDirty = analysis.sectorDirty;
		bool normalizedDragged = analysis.dragged;
		const uint32_t residentNoiseReasonMask =
			nri_scene::PTMapChunkMutationReason_SectorDirty |
			nri_scene::PTMapChunkMutationReason_SectionDirty |
			nri_scene::PTMapChunkMutationReason_Dragged;
		if (replacement.residentAuthoritative &&
			!analysis.signatureChanged &&
			analysis.signature == replacement.baselineSignature &&
			(normalizedReasonMask & ~residentNoiseReasonMask) == 0)
		{
			normalizedReasonMask = nri_scene::PTMapChunkMutationReason_None;
			normalizedSectionDirtyCount = 0;
			normalizedSectorDirty = false;
			normalizedDragged = false;
		}
		replacement.reasonMask = normalizedReasonMask;
		replacement.sectionDirtyCount = normalizedSectionDirtyCount;
		replacement.sectorDirty = normalizedSectorDirty;
		replacement.dragged = normalizedDragged;
		replacement.staticAnimatedReplacement = false;
		replacement.blindSpot = normalizedReasonMask != nri_scene::PTMapChunkMutationReason_None && !analysis.signatureChanged;
		if (normalizedReasonMask != nri_scene::PTMapChunkMutationReason_None &&
			!chunkVisibleNow &&
			chunkIndex < runtimeMutationCandidateSourceMasks.size() &&
			(runtimeMutationCandidateSourceMasks[chunkIndex] & RuntimeMutationWorklistCandidateSource_BackgroundSweep) != 0)
		{
			seedRuntimeMutationSignatureWatchlist(chunkIndex);
		}
		if (worklistValidation.enabled && chunkIndex < worklistValidation.sourceMasks.size())
		{
			const bool fullScanSelectedChunk = normalizedReasonMask != nri_scene::PTMapChunkMutationReason_None;
			if (fullScanSelectedChunk)
			{
				worklistValidation.fullDirty[chunkIndex] = 1u;
				worklistValidation.fullDirtyCount++;
				if (worklistValidation.sourceMasks[chunkIndex] == 0)
				{
					worklistValidation.falseNegativeCount++;
					if (normalizedReasonMask < worklistValidation.falseNegativeReasonMaskCounts.size())
					{
						worklistValidation.falseNegativeReasonMaskCounts[normalizedReasonMask]++;
					}
					if (seedRuntimeMutationSignatureWatchlist(chunkIndex))
					{
						worklistValidation.signatureWatchlistSeedCount++;
					}
					if (worklistValidation.verbosity >= 2 && worklistValidation.falseNegativeTraceCount < 16u)
					{
						Printf(
							"PERF pt mutation worklist miss NRI: frame=%llu chunk=%u sector=%d reason_mask=0x%x reasons=%s source_mask=0x%x sources=%s previous_reason_mask=0x%x active=%u valid=%u resident_authoritative=%u visible=%u startup_visible=%u unresolved_textures=%u visible_suppressed_anim=%u signature_changed=%u\n",
							(unsigned long long)mFrameIndex,
							mapChunk.chunkIndex,
							mapChunk.sectorIndex,
							normalizedReasonMask,
							GetRuntimeMapMutationReasonSummary(normalizedReasonMask).c_str(),
							worklistValidation.sourceMasks[chunkIndex],
							GetRuntimeMutationWorklistCandidateSourceSummary(worklistValidation.sourceMasks[chunkIndex]).c_str(),
							previousReasonMask,
							replacement.active ? 1u : 0u,
							replacement.valid ? 1u : 0u,
							replacement.residentAuthoritative ? 1u : 0u,
							chunkVisibleNow ? 1u : 0u,
							startupVisibleValidationPending ? 1u : 0u,
							chunkHasUnresolvedAuthoredTextures ? 1u : 0u,
							isVisibleSuppressedStaticAnimatedChunk(mapChunk.chunkIndex) ? 1u : 0u,
							analysis.signatureChanged ? 1u : 0u);
						worklistValidation.falseNegativeTraceCount++;
					}
				}
			}
		}
		// Section dirty alone is too broad for PT runtime replacement because
		// the raster path can mark transient warped sections dirty during draw
		// prep without producing a stable gameplay map mutation. Keep explicit
		// forced invalidation for sector-dirty, but do not let the sticky
		// dragged-sector ownership bit force perpetual rebuilds once PT already
		// has a valid replacement baseline for the chunk.
		const bool forceTopologyInvalidation =
			(normalizedReasonMask & nri_scene::PTMapChunkMutationReason_SectorDirty) != 0;
		RuntimeMutationTraceAction chunkTraceAction = RuntimeMutationTraceAction::None;
		bool chunkTraceResidentMaterialDirty = false;
		bool chunkTraceResidentGeometryDirty = false;
		bool chunkTraceRecoveredEmpty = false;
		uint32_t chunkTraceSurfaceCount = replacement.surfaceCount;
		uint32_t chunkTraceTriangleCount = replacement.triangleCount;
		uint32_t chunkTraceMaterialCount = (uint32_t)replacement.materialBridge.materials.size();
		bool sectorDirtyTruthCaptured = false;
		const uint32_t previousReplacementSurfaceCount = replacement.surfaceCount;
		const uint32_t previousReplacementTriangleCount = replacement.triangleCount;
		auto emitChunkTrace = [&]()
		{
			recordRuntimeMutationTopEntry(
				mapChunk.chunkIndex,
				mapChunk.sectorIndex,
				normalizedReasonMask,
				replacement.sectionDirtyCount,
				chunkTraceSurfaceCount,
				chunkTraceTriangleCount,
				chunkTraceMaterialCount,
				chunkTraceAction,
				forceTopologyInvalidation,
				chunkTraceResidentMaterialDirty,
				chunkTraceResidentGeometryDirty,
				chunkTraceRecoveredEmpty);
		};
		if ((normalizedReasonMask & nri_scene::PTMapChunkMutationReason_SectorGeometry) != 0)
		{
			mRuntimeMutation.lastFrame.sectorGeometryChunkCount++;
		}
		if ((normalizedReasonMask & nri_scene::PTMapChunkMutationReason_SectorMaterial) != 0)
		{
			mRuntimeMutation.lastFrame.sectorMaterialChunkCount++;
		}
		if ((normalizedReasonMask & nri_scene::PTMapChunkMutationReason_WallGeometry) != 0)
		{
			mRuntimeMutation.lastFrame.wallGeometryChunkCount++;
		}
		if ((normalizedReasonMask & nri_scene::PTMapChunkMutationReason_WallMaterial) != 0)
		{
			mRuntimeMutation.lastFrame.wallMaterialChunkCount++;
		}
		if ((normalizedReasonMask & nri_scene::PTMapChunkMutationReason_SectorDirty) != 0)
		{
			mRuntimeMutation.lastFrame.sectorDirtyChunkCount++;
		}
		if ((normalizedReasonMask & nri_scene::PTMapChunkMutationReason_SectionDirty) != 0)
		{
			mRuntimeMutation.lastFrame.sectionDirtyChunkCount++;
		}
		if ((normalizedReasonMask & nri_scene::PTMapChunkMutationReason_Dragged) != 0)
		{
			mRuntimeMutation.lastFrame.draggedChunkCount++;
		}
		const uint32_t startupMaterialOnlyReasonMask =
			nri_scene::PTMapChunkMutationReason_SectorMaterial |
			nri_scene::PTMapChunkMutationReason_WallMaterial;
		if (mAllowStartupMutationRebaseline &&
			mFrameIndex <= mStartupMutationRebaselineDeadlineFrame &&
			(normalizedReasonMask & startupMaterialOnlyReasonMask) != 0 &&
			(normalizedReasonMask & ~startupMaterialOnlyReasonMask) == 0)
		{
			startupMaterialOnlyMutationDetected = true;
		}

		const bool useStaticAnimatedReplacement =
			normalizedReasonMask == nri_scene::PTMapChunkMutationReason_None &&
			isVisibleSuppressedStaticAnimatedChunk(mapChunk.chunkIndex);
		if (normalizedReasonMask == nri_scene::PTMapChunkMutationReason_None &&
			!useStaticAnimatedReplacement &&
			replacement.residentAuthoritative &&
			chunkVisibleNow &&
			residentEntry != nullptr &&
			residentEntry->valid &&
			residentEntry->visibleValidationFramesRemaining > 0)
		{
			uint64_t liveVisibleGeometrySignature = 0;
			uint64_t liveVisibleMaterialSignature = 0;
			nri_scene::SceneView liveVisibleChunkView;
			nri_scene::PTMapWorldStats ignoredVisibleStats = {};
			if (nri_scene::BuildLiveMapChunkSceneView(mapChunk, liveVisibleChunkView, &ignoredVisibleStats))
			{
				if (nri_ptceilingnudge)
				{
					NudgeMapCeilingSections(liveVisibleChunkView, (float)nri_ptceilingnudgedistance);
				}
				liveVisibleGeometrySignature = ComputeExactGeometrySignature(liveVisibleChunkView);
				liveVisibleMaterialSignature = ComputeAnimatedMaterialSignature(liveVisibleChunkView);
				if (liveVisibleGeometrySignature != residentEntry->exactGeometrySignature)
				{
					normalizedReasonMask |= nri_scene::PTMapChunkMutationReason_SectionDirty;
				}
				else if (liveVisibleMaterialSignature != residentEntry->animatedMaterialSignature)
				{
					normalizedReasonMask |= nri_scene::PTMapChunkMutationReason_SectorMaterial;
				}
				else if (!chunkHasUnresolvedAuthoredTextures)
				{
					if (startupVisibleValidationPending &&
						mapChunk.chunkIndex < mPendingStartupVisibleChunkValidation.size())
					{
						mPendingStartupVisibleChunkValidation[mapChunk.chunkIndex] = 0u;
					}
					residentEntry->visibleValidationFramesRemaining = 0;
				}
			}

			if (residentEntry->visibleValidationFramesRemaining > 0 && !chunkHasUnresolvedAuthoredTextures)
			{
				residentEntry->visibleValidationFramesRemaining--;
			}
			if (normalizedReasonMask != nri_scene::PTMapChunkMutationReason_None &&
				ShouldTracePtPerf() &&
				!residentEntry->visibleValidationTraceEmitted)
			{
				Printf("NRI PT visible resident validation: chunk=%u sector=%d reason_mask=0x%x unresolved_textures=%s startup_pending=%s validation_frames=%u static_geom_sig=0x%llx live_geom_sig=0x%llx static_mat_sig=0x%llx live_mat_sig=0x%llx\n",
					mapChunk.chunkIndex,
					mapChunk.sectorIndex,
					normalizedReasonMask,
					YesNo(chunkHasUnresolvedAuthoredTextures),
					YesNo(startupVisibleValidationPending),
					(uint32_t)residentEntry->visibleValidationFramesRemaining,
					(unsigned long long)residentEntry->exactGeometrySignature,
					(unsigned long long)liveVisibleGeometrySignature,
					(unsigned long long)residentEntry->animatedMaterialSignature,
					(unsigned long long)liveVisibleMaterialSignature);
				PrintMapChunkCompare((int32_t)mapChunk.chunkIndex);
				residentEntry->visibleValidationTraceEmitted = true;
			}
		}
		if (normalizedReasonMask == nri_scene::PTMapChunkMutationReason_None && !useStaticAnimatedReplacement)
		{
			replacement.active = false;
			replacement.excludeStaticChunk = false;
			replacement.staticAnimatedReplacement = false;
			replacement.animationOnlyRefreshed = false;
			if (!replacement.residentAuthoritative)
			{
				replacement.animatedMaterialSignature = 0;
			}
			replacement.stableMutationFrameCount = 0;
			chunkTraceAction = RuntimeMutationTraceAction::None;
			mRuntimeMutation.TraceChunk(mapChunk, replacement, ShouldEmitTemporalTraceLogs(), (int)nri_ptmutationtracechunk, (int)nri_ptmutationtracesector);
			emitChunkTrace();
			mRuntimeMutation.ClearReplacementPayload(replacement, true);
			continue;
		}

		if (!useStaticAnimatedReplacement)
		{
			mRuntimeMutation.lastFrame.dirtyChunkCount++;
			mLastPerfShellTraceStats.runtimeMutationDirtyChunks++;
			recordRuntimeMutationDirtyTier(chunkVisibleNow, runtimeMutationDistanceTier, runtimeMutationCandidateSourceMask);
			if (replacement.blindSpot)
			{
				mRuntimeMutation.lastFrame.blindSpotChunkCount++;
			}
		}

		const bool materialOnlyReplacement = IsMaterialOnlyChunkReplacement(normalizedReasonMask);
		nri_scene::PTMapChunkMutationAnalysis replacementDelta = {};
		const bool haveReplacementBaseline = replacement.valid || replacement.residentAuthoritative;
		const bool analyzedReplacementDelta =
			haveReplacementBaseline &&
			nri_scene::AnalyzeMapChunkMutation(mapChunk, replacement.replacementBaseline, replacementDelta);
		const uint32_t structuralReasonMask =
			nri_scene::PTMapChunkMutationReason_SectorGeometry |
			nri_scene::PTMapChunkMutationReason_WallGeometry |
			nri_scene::PTMapChunkMutationReason_SectorDirty |
			nri_scene::PTMapChunkMutationReason_SectionDirty;
		const uint32_t replacementViewReasonMask =
			nri_scene::PTMapChunkMutationReason_SectorGeometry |
			nri_scene::PTMapChunkMutationReason_SectorMaterial |
			nri_scene::PTMapChunkMutationReason_WallGeometry |
			nri_scene::PTMapChunkMutationReason_WallMaterial |
			nri_scene::PTMapChunkMutationReason_SectorDirty |
			nri_scene::PTMapChunkMutationReason_SectionDirty |
			nri_scene::PTMapChunkMutationReason_Dragged;
		const uint32_t replacementRefreshReasonMask =
			replacementDelta.reasonMask & ~nri_scene::PTMapChunkMutationReason_Dragged;
		const bool useOverlayReplacementState = replacement.valid;
		const bool replacementViewChanged =
			useOverlayReplacementState &&
			(previousReasonMask & replacementViewReasonMask) !=
			(normalizedReasonMask & replacementViewReasonMask);
		replacement.animationOnlyRefreshed = false;
		nri_scene::PTMapWorld liveWorld = {};
		nri_scene::SceneView liveChunkView;
		nri_scene::PTMapWorldStats liveStats = {};
		bool havePreparedLiveChunkView = false;
		bool attemptedResidentComparableLiveChunkView = false;
		bool haveResidentComparableLiveChunkView = false;
		nri_scene::SceneView residentComparableLiveChunkView;
		const auto prepareLiveChunkView = [&]() -> bool
		{
			if (havePreparedLiveChunkView)
			{
				return true;
			}

			if (!nri_scene::BuildLiveMapChunkWorld(mapChunk, liveWorld, &liveStats))
			{
				return false;
			}

			nri_scene::BuildMapChunkSceneView(liveWorld, liveWorld.chunks[0], liveChunkView);
			const bool exclusiveMaterialOnlyReplacement =
				materialOnlyReplacement &&
				RequiresExclusiveMaterialOnlyChunkReplacement(normalizedReasonMask);
			const bool blindSpotReplacementNudged = replacement.blindSpot && replacement.dragged;
			if (blindSpotReplacementNudged)
			{
				NudgeBlindSpotReplacementFlats(liveChunkView);
			}
			else if (nri_ptceilingnudge)
			{
				NudgeMapCeilingSections(liveChunkView, (float)nri_ptceilingnudgedistance);
			}
			if (materialOnlyReplacement && !exclusiveMaterialOnlyReplacement)
			{
				FilterMaterialOnlyReplacementSceneView(liveChunkView, normalizedReasonMask);
			}

			havePreparedLiveChunkView = true;
			return true;
		};
		const auto tryResolveResidentChunkState =
			[&](uint32_t& outResidentChunkListIndex,
				const StaticMapSceneCache::ChunkCache*& outResidentChunkCache,
				const nri_scene::SceneView*& outResidentChunkView) -> bool
		{
			outResidentChunkListIndex = UINT32_MAX;
			outResidentChunkCache = nullptr;
			outResidentChunkView = nullptr;
			if (residentEntry == nullptr || !residentEntry->valid)
			{
				return false;
			}

			outResidentChunkListIndex = residentEntry->staticSceneChunkListIndex;
			const bool residentChunkListIndexValid =
				outResidentChunkListIndex < mStaticMapScene.chunks.size() &&
				mStaticMapScene.chunks[outResidentChunkListIndex].chunkIndex == mapChunk.chunkIndex;
			if (!residentChunkListIndexValid)
			{
				outResidentChunkListIndex = FindPreferredStaticSceneChunkListIndex(mapChunk.chunkIndex);
			}
			if (outResidentChunkListIndex >= mStaticMapScene.chunks.size() ||
				outResidentChunkListIndex >= mStaticMapScene.lightChunkViews.size() ||
				!mStaticMapScene.chunks[outResidentChunkListIndex].active)
			{
				return false;
			}

			outResidentChunkCache = &mStaticMapScene.chunks[outResidentChunkListIndex];
			outResidentChunkView = &mStaticMapScene.lightChunkViews[outResidentChunkListIndex];
			return true;
		};
		const auto tryMergeSectorMaterialOnlyPreparedMaterials =
			[&](const nri_scene::SceneView& preparedLiveChunkView,
				nri_scene::MaterialBridgeData& inOutPreparedLiveMaterials) -> bool
		{
			if (!IsPureSectorMaterialOnlyChunkReplacement(normalizedReasonMask))
			{
				return false;
			}

			uint32_t residentChunkListIndex = UINT32_MAX;
			const StaticMapSceneCache::ChunkCache* residentChunkCache = nullptr;
			const nri_scene::SceneView* residentChunkView = nullptr;
			if (!tryResolveResidentChunkState(residentChunkListIndex, residentChunkCache, residentChunkView) ||
				residentChunkCache == nullptr ||
				residentChunkView == nullptr)
			{
				return false;
			}

			nri_scene::MaterialBridgeData mergedMaterials;
			if (!TryBuildMergedSectorMaterialOnlyBridge(
				*residentChunkView,
				residentChunkCache->materialBridge,
				preparedLiveChunkView,
				inOutPreparedLiveMaterials,
				mergedMaterials))
			{
				return false;
			}

			inOutPreparedLiveMaterials = std::move(mergedMaterials);
			return true;
		};
		const auto getResidentComparableLiveChunkView =
			[&]() -> const nri_scene::SceneView*
		{
			if (attemptedResidentComparableLiveChunkView)
			{
				return haveResidentComparableLiveChunkView ? &residentComparableLiveChunkView : nullptr;
			}

			attemptedResidentComparableLiveChunkView = true;
			haveResidentComparableLiveChunkView = false;
			if (!IsPureSectorMaterialOnlyChunkReplacement(normalizedReasonMask))
			{
				return nullptr;
			}

			uint32_t residentChunkListIndex = UINT32_MAX;
			const StaticMapSceneCache::ChunkCache* residentChunkCache = nullptr;
			const nri_scene::SceneView* residentChunkView = nullptr;
			if (!tryResolveResidentChunkState(residentChunkListIndex, residentChunkCache, residentChunkView) ||
				residentChunkView == nullptr)
			{
				return nullptr;
			}
			// Pure sector-material chunks can still animate sector-driven wall bands.
			// Replacing the live wall set with resident walls hides those updates and
			// can make the chunk look synchronized after a single frame.
			if (SceneViewHasSectorDrivenWallBands(*residentChunkView))
			{
				return nullptr;
			}

			if (!TryBuildMergedSectorMaterialOnlySceneView(
				*residentChunkView,
				liveChunkView,
				residentComparableLiveChunkView))
			{
				return nullptr;
			}

			haveResidentComparableLiveChunkView = true;
			return &residentComparableLiveChunkView;
		};
		const auto recordPreparedLiveChunkMaterialOnlyMismatch =
			[&](const nri_scene::SceneView& preparedLiveChunkView,
				const nri_scene::MaterialBridgeData& preparedLiveMaterials,
				bool refreshPath)
		{
			const bool exclusiveMaterialOnlyReplacement =
				materialOnlyReplacement &&
				RequiresExclusiveMaterialOnlyChunkReplacement(normalizedReasonMask);
			if (exclusiveMaterialOnlyReplacement)
			{
				return;
			}

			const uint32_t filteredMaterialCount = (uint32_t)preparedLiveMaterials.materials.size();
			uint32_t residentChunkListIndex = UINT32_MAX;
			const StaticMapSceneCache::ChunkCache* residentChunkCache = nullptr;
			const nri_scene::SceneView* residentChunkView = nullptr;
			if (!tryResolveResidentChunkState(residentChunkListIndex, residentChunkCache, residentChunkView) ||
				residentChunkCache == nullptr ||
				residentChunkView == nullptr)
			{
				return;
			}

			const uint32_t residentMaterialCount = residentChunkCache->materialCount;
			if (filteredMaterialCount == residentMaterialCount)
			{
				return;
			}

			const uint32_t residentWallCount = (uint32_t)residentChunkView->opaqueWalls.size();
			const uint32_t residentFlatCount = (uint32_t)residentChunkView->opaqueFlats.size();
			const uint32_t filteredWallCount = (uint32_t)preparedLiveChunkView.opaqueWalls.size();
			const uint32_t filteredFlatCount = (uint32_t)preparedLiveChunkView.opaqueFlats.size();
			mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchCount++;
			if (refreshPath)
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchRefreshCount++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchRebuildCount++;
			}
			if (filteredWallCount != 0 && filteredFlatCount == 0)
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchFilteredWallOnlyCount++;
			}
			else if (filteredFlatCount != 0 && filteredWallCount == 0)
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchFilteredFlatOnlyCount++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchFilteredMixedCount++;
			}

			if (residentWallCount != 0 && residentFlatCount == 0)
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchResidentWallOnlyCount++;
			}
			else if (residentFlatCount != 0 && residentWallCount == 0)
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchResidentFlatOnlyCount++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchResidentMixedCount++;
			}

			recordMaterialOnlyMismatchEntry(
				mapChunk.chunkIndex,
				mapChunk.sectorIndex,
				refreshPath,
				normalizedReasonMask,
				CountSceneViewSurfaces(preparedLiveChunkView),
				filteredMaterialCount,
				residentMaterialCount,
				filteredWallCount,
				filteredFlatCount,
				residentWallCount,
				residentFlatCount);
		};
		static constexpr size_t kRuntimeMutationMaterialStateCacheCapacity = 8;
		const auto tryGetCachedRuntimeMutationMaterials =
			[&replacement](uint64_t animatedGeometrySignature,
				uint64_t animatedMaterialSignature,
				uint32_t surfaceCount,
				nri_scene::MaterialBridgeData& outMaterials) -> bool
		{
			auto& cache = replacement.materialStateCache;
			for (size_t index = 0; index < cache.size(); ++index)
			{
				const auto& entry = cache[index];
				if (entry.animatedGeometrySignature != animatedGeometrySignature ||
					entry.animatedMaterialSignature != animatedMaterialSignature ||
					entry.surfaceCount != surfaceCount)
				{
					continue;
				}

				if (index != 0)
				{
					auto hotEntry = std::move(cache[index]);
					cache.erase(cache.begin() + (ptrdiff_t)index);
					cache.insert(cache.begin(), std::move(hotEntry));
				}
				outMaterials = cache.front().materialBridge;
				return true;
			}

			return false;
		};
		const auto storeCachedRuntimeMutationMaterials =
			[&replacement](uint64_t animatedGeometrySignature,
				uint64_t animatedMaterialSignature,
				uint32_t surfaceCount,
				const nri_scene::MaterialBridgeData& materials)
		{
			if (materials.materials.empty())
			{
				return;
			}

			auto& cache = replacement.materialStateCache;
			for (size_t index = 0; index < cache.size(); ++index)
			{
				auto& entry = cache[index];
				if (entry.animatedGeometrySignature != animatedGeometrySignature ||
					entry.animatedMaterialSignature != animatedMaterialSignature ||
					entry.surfaceCount != surfaceCount)
				{
					continue;
				}

				entry.materialBridge = materials;
				if (index != 0)
				{
					auto hotEntry = std::move(entry);
					cache.erase(cache.begin() + (ptrdiff_t)index);
					cache.insert(cache.begin(), std::move(hotEntry));
				}
				return;
			}

			RuntimeMapMutationCache::ChunkReplacement::MaterialStateCacheEntry entry = {};
			entry.animatedGeometrySignature = animatedGeometrySignature;
			entry.animatedMaterialSignature = animatedMaterialSignature;
			entry.surfaceCount = surfaceCount;
			entry.materialBridge = materials;
			cache.insert(cache.begin(), std::move(entry));
			if (cache.size() > kRuntimeMutationMaterialStateCacheCapacity)
			{
				cache.pop_back();
			}
		};
		const auto captureSectorDirtyTruth = [&]()
		{
			if (!ShouldTracePtPerf() ||
				sectorDirtyTruthCaptured ||
				(normalizedReasonMask & nri_scene::PTMapChunkMutationReason_SectorDirty) == 0 ||
				!havePreparedLiveChunkView)
			{
				return;
			}

			uint64_t liveBaselineSignature = 0;
			nri_scene::PTMapChunkMutationBaseline truthBaseline;
			if (nri_scene::CaptureMapChunkMutationBaseline(mapChunk, truthBaseline))
			{
				liveBaselineSignature = truthBaseline.signature;
			}

			uint64_t previousGeometrySignature = 0;
			uint64_t previousMaterialSignature = 0;
			RuntimeSectorDirtyTruthTraceEntry::PreviousStateSource previousStateSource =
				RuntimeSectorDirtyTruthTraceEntry::PreviousStateSource::None;
			bool havePreviousSignatures = false;
			uint32_t previousSurfaceCount = 0;
			uint32_t previousTriangleCount = 0;
			uint32_t liveBuiltTriangleCount = liveStats.triangleCount;
			if (replacement.valid)
			{
				previousGeometrySignature = ComputeExactGeometrySignature(replacement.sceneView);
				previousMaterialSignature = replacement.animatedMaterialSignature;
				previousStateSource = RuntimeSectorDirtyTruthTraceEntry::PreviousStateSource::Replacement;
				havePreviousSignatures = true;
				previousSurfaceCount = previousReplacementSurfaceCount;
				previousTriangleCount = previousReplacementTriangleCount;
			}
			else if (residentEntry != nullptr && residentEntry->valid)
			{
				previousGeometrySignature = residentEntry->exactGeometrySignature;
				previousMaterialSignature = residentEntry->animatedMaterialSignature;
				previousStateSource = RuntimeSectorDirtyTruthTraceEntry::PreviousStateSource::Resident;
				havePreviousSignatures = true;
				previousSurfaceCount = residentEntry->materialCount;
				previousTriangleCount = residentEntry->primitiveCount;
			}

			const bool liveGeometryChanged =
				!havePreviousSignatures ||
				ComputeExactGeometrySignature(liveChunkView) != previousGeometrySignature;
			const bool liveMaterialChanged =
				!havePreviousSignatures ||
				ComputeAnimatedMaterialSignature(liveChunkView) != previousMaterialSignature;
			nri_scene::GeometryData liveTruthGeometry;
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildRuntimeMutationTruthMs);
				nri_scene::BuildGeometry(liveChunkView, liveTruthGeometry);
			}
			liveBuiltTriangleCount = (uint32_t)liveTruthGeometry.primitives.size();
			mLastPerfShellTraceStats.geometryBuildRuntimeMutationTruthCalls++;
			mLastPerfShellTraceStats.geometryBuildRuntimeMutationPrimitives += liveBuiltTriangleCount;
			if (forceTopologyInvalidation &&
				previousStateSource == RuntimeSectorDirtyTruthTraceEntry::PreviousStateSource::Resident &&
				!liveGeometryChanged &&
				!liveMaterialChanged)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentNoopCandidateCount++;
				mLastPerfShellTraceStats.runtimeMutationResidentNoopCandidateReasonMaskOr |= normalizedReasonMask;
			}

			recordSectorDirtyTruthEntry(
				mapChunk.chunkIndex,
				mapChunk.sectorIndex,
				normalizedReasonMask,
				previousStateSource,
				forceTopologyInvalidation,
				liveBaselineSignature != replacement.replacementBaseline.signature,
				liveGeometryChanged,
				liveMaterialChanged,
				previousSurfaceCount,
				CountSceneViewSurfaces(liveChunkView),
				previousTriangleCount,
				liveBuiltTriangleCount);
			sectorDirtyTruthCaptured = true;
		};
		const auto rebuildReplacementFromPreparedLiveChunk = [&](bool countAsStructuralRebuild) -> bool
		{
			const bool exclusiveMaterialOnlyReplacement =
				materialOnlyReplacement &&
				RequiresExclusiveMaterialOnlyChunkReplacement(normalizedReasonMask);
			const bool excludeStaticChunk =
				useStaticAnimatedReplacement ||
				!materialOnlyReplacement ||
				exclusiveMaterialOnlyReplacement;
			const nri_scene::SceneView* residentComparableView = getResidentComparableLiveChunkView();
			const nri_scene::SceneView& animatedSignatureSceneView =
				residentComparableView != nullptr ? *residentComparableView : liveChunkView;
			const uint64_t liveAnimatedGeometrySignature =
				ComputeAnimatedGeometrySignature(animatedSignatureSceneView);
			const uint64_t liveAnimatedMaterialSignature =
				ComputeAnimatedMaterialSignature(animatedSignatureSceneView);
			const uint32_t liveSurfaceCount = CountSceneViewSurfaces(liveChunkView);
			mRuntimeMutation.BuildLightIdentityOverrides(
				mMapWorld,
				mapChunk,
				liveWorld,
				liveWorld.chunks[0],
				replacement.lightIdentityOverrides);

			nri_scene::GeometryData liveGeometry;
			nri_scene::MaterialBridgeData liveMaterials;
			{
				Clocker clock(NriPTGeometryBuild);
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildRuntimeMutationRebuildMs);
				nri_scene::BuildGeometry(liveChunkView, liveGeometry);
				AssignGeometryPortalIndices(mMapWorld, liveGeometry);
			}
			mLastPerfShellTraceStats.geometryBuildRuntimeMutationRebuildCalls++;
			mLastPerfShellTraceStats.geometryBuildRuntimeMutationPrimitives += (uint32_t)liveGeometry.primitives.size();
			if (tryGetCachedRuntimeMutationMaterials(
				liveAnimatedGeometrySignature,
				liveAnimatedMaterialSignature,
				liveSurfaceCount,
				liveMaterials))
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialCacheHitCount++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialCacheMissCount++;
				{
					Clocker clock(NriPTMaterialBuild);
					BuildMaterialsWithActorOverrides(liveChunkView, liveMaterials, "runtime_mutation_chunk");
				}
				storeCachedRuntimeMutationMaterials(
					liveAnimatedGeometrySignature,
					liveAnimatedMaterialSignature,
					liveSurfaceCount,
					liveMaterials);
				mLastPerfShellTraceStats.runtimeMutationMaterialCacheStoreCount++;
			}
			tryMergeSectorMaterialOnlyPreparedMaterials(liveChunkView, liveMaterials);
			recordPreparedLiveChunkMaterialOnlyMismatch(liveChunkView, liveMaterials, false);
			nri_scene::PTMapChunkMutationBaseline liveBaseline;
			if (!nri_scene::CaptureMapChunkMutationBaseline(mapChunk, liveBaseline))
			{
				return false;
			}

			replacement.sceneView = liveChunkView;
			replacement.geometry = std::move(liveGeometry);
			replacement.materialBridge = std::move(liveMaterials);
			replacement.replacementBaseline = std::move(liveBaseline);
			replacement.surfaceCount = liveSurfaceCount;
			replacement.triangleCount = (uint32_t)replacement.geometry.primitives.size();
			replacement.animatedMaterialSignature = liveAnimatedMaterialSignature;
			replacement.valid = true;
			replacement.active = true;
			replacement.excludeStaticChunk = excludeStaticChunk;
			replacement.staticAnimatedReplacement = useStaticAnimatedReplacement;
			if (countAsStructuralRebuild)
			{
				mRuntimeMutation.lastFrame.rebuiltChunkCount++;
				mLastPerfShellTraceStats.runtimeMutationRebuiltChunks++;
			}
			return true;
		};
		const auto refreshReplacementMaterialsFromPreparedLiveChunk = [&]() -> bool
		{
			const bool exclusiveMaterialOnlyReplacement =
				materialOnlyReplacement &&
				RequiresExclusiveMaterialOnlyChunkReplacement(normalizedReasonMask);
			const bool excludeStaticChunk =
				useStaticAnimatedReplacement ||
				!materialOnlyReplacement ||
				exclusiveMaterialOnlyReplacement;
			const nri_scene::SceneView* residentComparableView = getResidentComparableLiveChunkView();
			const nri_scene::SceneView& animatedSignatureSceneView =
				residentComparableView != nullptr ? *residentComparableView : liveChunkView;
			const uint64_t liveAnimatedGeometrySignature =
				ComputeAnimatedGeometrySignature(animatedSignatureSceneView);
			const uint64_t liveAnimatedMaterialSignature =
				ComputeAnimatedMaterialSignature(animatedSignatureSceneView);
			const uint32_t liveSurfaceCount = CountSceneViewSurfaces(liveChunkView);
			nri_scene::GeometryData liveGeometry;
			if (exclusiveMaterialOnlyReplacement)
			{
				Clocker clock(NriPTGeometryBuild);
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildRuntimeMutationMaterialOnlyMs);
				nri_scene::BuildGeometry(liveChunkView, liveGeometry);
				AssignGeometryPortalIndices(mMapWorld, liveGeometry);
				mLastPerfShellTraceStats.geometryBuildRuntimeMutationMaterialOnlyCalls++;
				mLastPerfShellTraceStats.geometryBuildRuntimeMutationPrimitives += (uint32_t)liveGeometry.primitives.size();
			}
			nri_scene::MaterialBridgeData liveMaterials;
			if (tryGetCachedRuntimeMutationMaterials(
				liveAnimatedGeometrySignature,
				liveAnimatedMaterialSignature,
				liveSurfaceCount,
				liveMaterials))
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialCacheHitCount++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialCacheMissCount++;
				{
					Clocker clock(NriPTMaterialBuild);
					BuildMaterialsWithActorOverrides(liveChunkView, liveMaterials, "runtime_mutation_chunk");
				}
				storeCachedRuntimeMutationMaterials(
					liveAnimatedGeometrySignature,
					liveAnimatedMaterialSignature,
					liveSurfaceCount,
					liveMaterials);
				mLastPerfShellTraceStats.runtimeMutationMaterialCacheStoreCount++;
			}
			tryMergeSectorMaterialOnlyPreparedMaterials(liveChunkView, liveMaterials);
			recordPreparedLiveChunkMaterialOnlyMismatch(liveChunkView, liveMaterials, true);
			nri_scene::PTMapChunkMutationBaseline liveBaseline;
			if (!nri_scene::CaptureMapChunkMutationBaseline(mapChunk, liveBaseline))
			{
				return false;
			}

			if (exclusiveMaterialOnlyReplacement)
			{
				mRuntimeMutation.BuildLightIdentityOverrides(
					mMapWorld,
					mapChunk,
					liveWorld,
					liveWorld.chunks[0],
					replacement.lightIdentityOverrides);
				replacement.geometry = std::move(liveGeometry);
				replacement.triangleCount = (uint32_t)replacement.geometry.primitives.size();
			}
			replacement.sceneView = liveChunkView;
			replacement.materialBridge = std::move(liveMaterials);
			replacement.replacementBaseline = std::move(liveBaseline);
			replacement.surfaceCount = liveSurfaceCount;
			replacement.animatedMaterialSignature = liveAnimatedMaterialSignature;
			replacement.valid = true;
			replacement.active = true;
			replacement.excludeStaticChunk = excludeStaticChunk;
			replacement.staticAnimatedReplacement = useStaticAnimatedReplacement;
			return true;
		};
		const bool exclusiveMaterialOnlyReplacement =
			materialOnlyReplacement &&
			RequiresExclusiveMaterialOnlyChunkReplacement(normalizedReasonMask);
		const bool desiredExcludeStaticChunk =
			useStaticAnimatedReplacement ||
			!materialOnlyReplacement ||
			exclusiveMaterialOnlyReplacement;
		const bool structuralInvalid = !replacement.valid && !replacement.residentAuthoritative;
		const bool structuralReplacementDelta =
			!analyzedReplacementDelta ||
			(replacementDelta.reasonMask & structuralReasonMask) != 0;
		const bool structuralStaticAnimatedModeFlip =
			replacement.staticAnimatedReplacement != useStaticAnimatedReplacement;
		const bool structuralExcludeStaticFlip =
			useOverlayReplacementState &&
			replacement.excludeStaticChunk != desiredExcludeStaticChunk;
		const bool needsStructuralRebuild =
			structuralInvalid ||
			forceTopologyInvalidation ||
			structuralReplacementDelta ||
			replacementViewChanged ||
			structuralExcludeStaticFlip ||
			structuralStaticAnimatedModeFlip;
		const bool hadDeferredStructuralRebuild = replacement.deferredStructuralRebuild;
		if (forceTopologyInvalidation)
		{
			mLastPerfShellTraceStats.runtimeMutationInvalidForceTopologyCount++;
		}
		if (needsStructuralRebuild)
		{
			replacement.deferredMaterialRefresh = false;
			replacement.deferredMaterialFrame = 0;
		}
		const bool activeReplacementStructuralCandidate =
			(runtimeMutationCandidateSourceMask & RuntimeMutationWorklistCandidateSource_ActiveReplacement) != 0;
		const bool deferInvisibleStructuralRebuild =
			needsStructuralRebuild &&
			!useStaticAnimatedReplacement &&
			!chunkVisibleNow &&
			!activeReplacementStructuralCandidate &&
			((runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Near && deferNearInvisibleStructuralRebuilds) ||
				(runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Far && deferFarInvisibleStructuralRebuilds));
		if (deferInvisibleStructuralRebuild &&
			!reserveInvisibleStructuralBudget(chunkIndex, runtimeMutationDistanceTier))
		{
			recordDeferredStructuralTier(runtimeMutationDistanceTier, hadDeferredStructuralRebuild);
			if (!hadDeferredStructuralRebuild)
			{
				replacement.deferredStructuralFrame = mFrameIndex;
			}
			replacement.deferredStructuralRebuild = true;
			replacement.animationOnlyRefreshed = false;
			seedRuntimeMutationSignatureWatchlist(chunkIndex);
			chunkTraceAction = RuntimeMutationTraceAction::DeferredStructuralRebuild;
			mRuntimeMutation.TraceChunk(mapChunk, replacement, ShouldEmitTemporalTraceLogs(), (int)nri_ptmutationtracechunk, (int)nri_ptmutationtracesector);
			emitChunkTrace();
			continue;
		}
		if (needsStructuralRebuild && hadDeferredStructuralRebuild)
		{
			recordFlushedDeferredStructuralTier(runtimeMutationDistanceTier);
			if (!deferInvisibleStructuralRebuild)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredPromotedChunks++;
			}
			replacement.deferredStructuralRebuild = false;
			replacement.deferredStructuralFrame = 0;
		}

		if (needsStructuralRebuild)
		{
			const bool structuralMaterialOnly = materialOnlyReplacement;
			const bool structuralSectorMaterialOnly =
				normalizedReasonMask == nri_scene::PTMapChunkMutationReason_SectorMaterial;
			const bool structuralWallMaterialOnly =
				normalizedReasonMask == nri_scene::PTMapChunkMutationReason_WallMaterial;
			const bool structuralMixedMaterialOnly =
				structuralMaterialOnly &&
				!structuralSectorMaterialOnly &&
				!structuralWallMaterialOnly;
			const bool structuralGeometryOrDirty =
				(normalizedReasonMask & structuralReasonMask) != 0;
			uint32_t previousGeometryDirtyWallCount = 0;
			uint32_t previousGeometryDirtyFlatCount = 0;
			uint32_t previousGeometryDirtyTriangleCount = 0;
			uint32_t previousGeometryDirtyMaterialCount = 0;
			if (structuralGeometryOrDirty)
			{
				if (replacement.valid)
				{
					previousGeometryDirtyWallCount = (uint32_t)replacement.sceneView.opaqueWalls.size();
					previousGeometryDirtyFlatCount = (uint32_t)replacement.sceneView.opaqueFlats.size();
					previousGeometryDirtyTriangleCount = replacement.triangleCount;
					previousGeometryDirtyMaterialCount = (uint32_t)replacement.materialBridge.materials.size();
				}
				else
				{
					uint32_t residentChunkListIndex = UINT32_MAX;
					const StaticMapSceneCache::ChunkCache* residentChunkCache = nullptr;
					const nri_scene::SceneView* residentChunkView = nullptr;
					if (tryResolveResidentChunkState(residentChunkListIndex, residentChunkCache, residentChunkView) &&
						residentChunkCache != nullptr &&
						residentChunkView != nullptr)
					{
						previousGeometryDirtyWallCount = (uint32_t)residentChunkView->opaqueWalls.size();
						previousGeometryDirtyFlatCount = (uint32_t)residentChunkView->opaqueFlats.size();
						previousGeometryDirtyTriangleCount = residentChunkCache->primitiveCount;
						previousGeometryDirtyMaterialCount = residentChunkCache->materialCount;
					}
				}
			}
			uint32_t structuralTriggerMask = 0;
			if (structuralReplacementDelta)
			{
				structuralTriggerMask |= RuntimeStructuralRebuildTrigger_ReplacementDelta;
			}
			if (replacementViewChanged)
			{
				structuralTriggerMask |= RuntimeStructuralRebuildTrigger_ViewChanged;
			}
			if (structuralStaticAnimatedModeFlip)
			{
				structuralTriggerMask |= RuntimeStructuralRebuildTrigger_StaticAnimatedFlip;
			}
			if (structuralExcludeStaticFlip)
			{
				structuralTriggerMask |= RuntimeStructuralRebuildTrigger_ExcludeStaticFlip;
			}
			if (forceTopologyInvalidation)
			{
				structuralTriggerMask |= RuntimeStructuralRebuildTrigger_ForceTopology;
			}
			if (structuralInvalid)
			{
				structuralTriggerMask |= RuntimeStructuralRebuildTrigger_Invalid;
			}
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildChunks++;
			recordRuntimeMutationStructuralTier(chunkVisibleNow, runtimeMutationDistanceTier, runtimeMutationCandidateSourceMask);
			if (structuralReplacementDelta)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaChunks++;
				if (analyzedReplacementDelta)
				{
					mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaReasonMaskOr |= replacementDelta.reasonMask;
				}
			}
			if (replacementViewChanged)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralReplacementViewChangedChunks++;
			}
			if (structuralStaticAnimatedModeFlip)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralStaticAnimatedModeFlipChunks++;
			}
			if (structuralExcludeStaticFlip)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralExcludeStaticFlipChunks++;
			}
			if (forceTopologyInvalidation)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralForcedTopologyChunks++;
			}
			if (structuralInvalid)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralInvalidChunks++;
			}
			if (structuralMaterialOnly)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralMaterialOnlyChunks++;
			}
			if (structuralSectorMaterialOnly)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralSectorMaterialOnlyChunks++;
			}
			if (structuralWallMaterialOnly)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralWallMaterialOnlyChunks++;
			}
			if (structuralMixedMaterialOnly)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralMixedMaterialOnlyChunks++;
			}
			if (structuralGeometryOrDirty)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralGeometryOrDirtyChunks++;
			}
			const bool builtChunk = [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationRebuildMs);
				ScopedPtPerfTimer structuralPerfTimer(mLastPerfShellTraceStats.runtimeMutationStructuralRebuildMs);
				ScopedPtPerfTimer structuralTierPerfTimer(
					chunkVisibleNow ?
						mLastPerfShellTraceStats.runtimeMutationStructuralRebuildVisibleMs :
						mLastPerfShellTraceStats.runtimeMutationStructuralRebuildInvisibleMs);
				double& structuralDistanceTierMs =
					chunkVisibleNow ?
						ignoredRuntimeMutationDistanceTierMs :
						(runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Near ?
							mLastPerfShellTraceStats.runtimeMutationStructuralRebuildNearMs :
							(runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Far ?
								mLastPerfShellTraceStats.runtimeMutationStructuralRebuildFarMs :
								mLastPerfShellTraceStats.runtimeMutationStructuralRebuildUnknownDistanceMs));
				ScopedPtPerfTimer structuralDistanceTierPerfTimer(structuralDistanceTierMs);
				if (!prepareLiveChunkView())
				{
					return false;
				}
				captureSectorDirtyTruth();
				return rebuildReplacementFromPreparedLiveChunk(true);
			}();
			if (!builtChunk && replacement.valid)
			{
				replacement.active = true;
				mRuntimeMutation.lastFrame.heldChunkCount++;
				mLastPerfShellTraceStats.runtimeMutationHeldChunks++;
				chunkTraceAction = RuntimeMutationTraceAction::Held;
				recordStructuralRebuildEntry(
					mapChunk.chunkIndex,
					mapChunk.sectorIndex,
					normalizedReasonMask,
					structuralTriggerMask,
					replacement.surfaceCount,
					replacement.triangleCount,
					(uint32_t)replacement.materialBridge.materials.size(),
					chunkTraceAction,
					structuralMaterialOnly,
					structuralSectorMaterialOnly,
					structuralWallMaterialOnly,
					structuralMixedMaterialOnly,
					structuralGeometryOrDirty);
			}
			else if (!builtChunk)
			{
				replacement.active = false;
				replacement.excludeStaticChunk = false;
				replacement.staticAnimatedReplacement = false;
				replacement.animationOnlyRefreshed = false;
				replacement.animatedMaterialSignature = 0;
				replacement.stableMutationFrameCount = 0;
				mLastPerfShellTraceStats.runtimeMutationInvalidFailedCount++;
				chunkTraceAction = RuntimeMutationTraceAction::Failed;
				recordStructuralRebuildEntry(
					mapChunk.chunkIndex,
					mapChunk.sectorIndex,
					normalizedReasonMask,
					structuralTriggerMask,
					0,
					0,
					0,
					chunkTraceAction,
					structuralMaterialOnly,
					structuralSectorMaterialOnly,
					structuralWallMaterialOnly,
					structuralMixedMaterialOnly,
					structuralGeometryOrDirty);
				mRuntimeMutation.TraceChunk(mapChunk, replacement, ShouldEmitTemporalTraceLogs(), (int)nri_ptmutationtracechunk, (int)nri_ptmutationtracesector);
				emitChunkTrace();
				mRuntimeMutation.ClearReplacementPayload(replacement, replacement.residentAuthoritative);
				continue;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationValidStructuralCount++;
				chunkTraceAction = RuntimeMutationTraceAction::StructuralRebuild;
				chunkTraceSurfaceCount = replacement.surfaceCount;
				chunkTraceTriangleCount = replacement.triangleCount;
				chunkTraceMaterialCount = (uint32_t)replacement.materialBridge.materials.size();
				recordStructuralRebuildEntry(
					mapChunk.chunkIndex,
					mapChunk.sectorIndex,
					normalizedReasonMask,
					structuralTriggerMask,
					replacement.surfaceCount,
					replacement.triangleCount,
					(uint32_t)replacement.materialBridge.materials.size(),
					chunkTraceAction,
					structuralMaterialOnly,
					structuralSectorMaterialOnly,
					structuralWallMaterialOnly,
					structuralMixedMaterialOnly,
					structuralGeometryOrDirty);
				if (structuralGeometryOrDirty)
				{
					recordGeometryDirtyEntry(
						mapChunk.chunkIndex,
						mapChunk.sectorIndex,
						normalizedReasonMask,
						previousGeometryDirtyWallCount,
						(uint32_t)replacement.sceneView.opaqueWalls.size(),
						previousGeometryDirtyFlatCount,
						(uint32_t)replacement.sceneView.opaqueFlats.size(),
						previousGeometryDirtyTriangleCount,
						replacement.triangleCount,
						previousGeometryDirtyMaterialCount,
						(uint32_t)replacement.materialBridge.materials.size(),
						forceTopologyInvalidation);
				}
			}
		}
		else
		{
			if (hadDeferredStructuralRebuild)
			{
				replacement.deferredStructuralRebuild = false;
				replacement.deferredStructuralFrame = 0;
			}
			const bool activeReplacementMaterialCandidate =
				(runtimeMutationCandidateSourceMask & RuntimeMutationWorklistCandidateSource_ActiveReplacement) != 0;
			const bool deferNearInvisibleMaterialRefresh =
				deferNearInvisibleMaterialRefreshes &&
				materialOnlyReplacement &&
				!useStaticAnimatedReplacement &&
				!chunkVisibleNow &&
				!activeReplacementMaterialCandidate &&
				runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Near;
			const bool deferFarInvisibleMaterialRefresh =
				runtimeMutationSettings.deferFarMaterialRefreshes &&
				materialOnlyReplacement &&
				!useStaticAnimatedReplacement &&
				!chunkVisibleNow &&
				runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Far;
			const bool deferInvisibleMaterialRefresh =
				deferFarInvisibleMaterialRefresh ||
				(deferNearInvisibleMaterialRefresh && !reserveNearInvisibleMaterialBudget(chunkIndex));
			if (deferInvisibleMaterialRefresh)
			{
				const bool hadDeferredMaterialRefresh = replacement.deferredMaterialRefresh;
				recordDeferredMaterialTier(runtimeMutationDistanceTier, hadDeferredMaterialRefresh);
				if (!hadDeferredMaterialRefresh)
				{
					replacement.deferredMaterialFrame = mFrameIndex;
				}
				replacement.deferredMaterialRefresh = true;
				replacement.animationOnlyRefreshed = false;
				seedRuntimeMutationSignatureWatchlist(chunkIndex);
				chunkTraceAction = RuntimeMutationTraceAction::DeferredMaterialRefresh;
				mRuntimeMutation.TraceChunk(mapChunk, replacement, ShouldEmitTemporalTraceLogs(), (int)nri_ptmutationtracechunk, (int)nri_ptmutationtracesector);
				emitChunkTrace();
				continue;
			}
			replacement.active = true;
			const bool hadDeferredMaterialRefresh = replacement.deferredMaterialRefresh;
			bool activeHardwareCanvasChunk = false;
			bool suppressedAnimatedResidentAlreadySynchronized = false;
			bool unsuppressedAnimatedResidentAlreadySynchronized = false;
			// Build tile animation can change the resolved PT texture binding
			// without mutating the authored wall/sector fields tracked above.
			const bool refreshedAnimatedChunk = [&]()
			{
				const bool forceReplacementMaterialRefresh =
					useOverlayReplacementState &&
					analyzedReplacementDelta &&
					replacementRefreshReasonMask != nri_scene::PTMapChunkMutationReason_None;
				if (!prepareLiveChunkView())
				{
					return false;
				}

				activeHardwareCanvasChunk = SceneViewUsesHardwareCanvasTexture(liveChunkView);
				const bool forceHardwareCanvasRefresh =
					activeHardwareCanvasChunk &&
					IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunk.chunkIndex);
				const nri_scene::SceneView* residentComparableView = getResidentComparableLiveChunkView();
				const nri_scene::SceneView& animatedSignatureSceneView =
					residentComparableView != nullptr ? *residentComparableView : liveChunkView;
				const uint64_t liveAnimatedGeometrySignature =
					ComputeAnimatedGeometrySignature(animatedSignatureSceneView);
				const uint64_t liveAnimatedMaterialSignature =
					ComputeAnimatedMaterialSignature(animatedSignatureSceneView);
				captureSectorDirtyTruth();
				const bool residentAnimatedAlreadySynchronized =
					!forceReplacementMaterialRefresh &&
					!forceHardwareCanvasRefresh &&
					residentEntry != nullptr &&
					residentEntry->valid &&
					residentEntry->animatedGeometrySignature == liveAnimatedGeometrySignature &&
					residentEntry->animatedMaterialSignature == liveAnimatedMaterialSignature;
				suppressedAnimatedResidentAlreadySynchronized =
					useStaticAnimatedReplacement &&
					residentAnimatedAlreadySynchronized &&
					residentEntry->animatedRefreshSuppressed;
				unsuppressedAnimatedResidentAlreadySynchronized =
					!useStaticAnimatedReplacement &&
					residentAnimatedAlreadySynchronized;
				if (useStaticAnimatedReplacement &&
					residentEntry != nullptr &&
					residentEntry->valid &&
					residentEntry->animatedRefreshSuppressed)
				{
					residentEntry->runtimeAnimatedAttemptCount++;
					recordRuntimeAnimatedFrame(mapChunk.chunkIndex, true, false, true, false, false);
				}
				else if (unsuppressedAnimatedResidentAlreadySynchronized &&
					residentEntry != nullptr &&
					residentEntry->valid)
				{
					residentEntry->runtimeAnimatedAttemptCount++;
					recordRuntimeAnimatedFrame(mapChunk.chunkIndex, false, false, true, false, false);
				}
				if (suppressedAnimatedResidentAlreadySynchronized ||
					unsuppressedAnimatedResidentAlreadySynchronized)
				{
					return true;
				}
				if (!forceReplacementMaterialRefresh &&
					!forceHardwareCanvasRefresh &&
					liveAnimatedMaterialSignature == replacement.animatedMaterialSignature)
				{
					return true;
				}

				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationRebuildMs);
				ScopedPtPerfTimer materialRefreshPerfTimer(mLastPerfShellTraceStats.runtimeMutationMaterialRefreshMs);
				ScopedPtPerfTimer materialRefreshTierPerfTimer(
					chunkVisibleNow ?
						mLastPerfShellTraceStats.runtimeMutationMaterialRefreshVisibleMs :
						mLastPerfShellTraceStats.runtimeMutationMaterialRefreshInvisibleMs);
				double& materialRefreshDistanceTierMs =
					chunkVisibleNow ?
						ignoredRuntimeMutationDistanceTierMs :
						(runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Near ?
							mLastPerfShellTraceStats.runtimeMutationMaterialRefreshNearMs :
							(runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Far ?
								mLastPerfShellTraceStats.runtimeMutationMaterialRefreshFarMs :
								mLastPerfShellTraceStats.runtimeMutationMaterialRefreshUnknownDistanceMs));
				ScopedPtPerfTimer materialRefreshDistanceTierPerfTimer(materialRefreshDistanceTierMs);
				if (!refreshReplacementMaterialsFromPreparedLiveChunk())
				{
					return false;
				}

				mLastPerfShellTraceStats.runtimeMutationMaterialRefreshChunks++;
				if (hadDeferredMaterialRefresh)
				{
					recordFlushedDeferredMaterialTier(runtimeMutationDistanceTier);
				}
				recordRuntimeMutationMaterialTier(chunkVisibleNow, runtimeMutationDistanceTier, runtimeMutationCandidateSourceMask);
				if (forceReplacementMaterialRefresh)
				{
					mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReplacementDeltaChunks++;
					mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReasonMaskOr |= replacementRefreshReasonMask;
				}
				if (forceHardwareCanvasRefresh)
				{
					mLastPerfShellTraceStats.runtimeMutationMaterialRefreshHardwareCanvasChunks++;
				}
				if (!forceReplacementMaterialRefresh && !forceHardwareCanvasRefresh)
				{
					mLastPerfShellTraceStats.runtimeMutationMaterialRefreshAnimatedChunks++;
					replacement.animationOnlyRefreshed = true;
					mRuntimeMutation.lastFrame.animatedRefreshChunkCount++;
				}
				recordRuntimeAnimatedFrame(
					mapChunk.chunkIndex,
					useStaticAnimatedReplacement,
					true,
					!useStaticAnimatedReplacement,
					false,
					false);
				return true;
			}();
			if (!refreshedAnimatedChunk)
			{
				replacement.active = true;
				mRuntimeMutation.lastFrame.heldChunkCount++;
				mLastPerfShellTraceStats.runtimeMutationHeldChunks++;
				chunkTraceAction = RuntimeMutationTraceAction::Held;
			}
			else
			{
				replacement.deferredMaterialRefresh = false;
				replacement.deferredMaterialFrame = 0;
				if (suppressedAnimatedResidentAlreadySynchronized ||
					unsuppressedAnimatedResidentAlreadySynchronized)
				{
					replacement.active = false;
					replacement.excludeStaticChunk = false;
					replacement.staticAnimatedReplacement = false;
					replacement.animationOnlyRefreshed = false;
					replacement.stableMutationFrameCount = 0;
					mLastPerfShellTraceStats.runtimeMutationInvalidSyncSkipCount++;
					if (residentEntry != nullptr && residentEntry->valid)
					{
						residentEntry->runtimeAnimatedSyncSkipCount++;
						if (residentEntry->mappedInStaticScene &&
							hasCachedResidentAnimatedSlice(
								residentEntry->staticSceneChunkListIndex,
								residentEntry->animatedGeometrySignature,
								residentEntry->animatedMaterialSignature))
						{
							mLastPerfShellTraceStats.staticAnimatedResidentSliceSyncSkipHitCount++;
						}
					}
					recordRuntimeAnimatedFrame(
						mapChunk.chunkIndex,
						suppressedAnimatedResidentAlreadySynchronized,
						false,
						false,
						false,
						true);
					chunkTraceAction = RuntimeMutationTraceAction::SyncSkip;
					chunkTraceSurfaceCount = CountSceneViewSurfaces(liveChunkView);
					chunkTraceTriangleCount = liveStats.triangleCount;
					chunkTraceMaterialCount = (uint32_t)replacement.materialBridge.materials.size();
					mRuntimeMutation.TraceChunk(mapChunk, replacement, ShouldEmitTemporalTraceLogs(), (int)nri_ptmutationtracechunk, (int)nri_ptmutationtracesector);
					emitChunkTrace();
					continue;
				}
				else if (replacement.valid)
				{
					mLastPerfShellTraceStats.runtimeMutationValidMaterialCount++;
					chunkTraceAction = RuntimeMutationTraceAction::MaterialRefresh;
					chunkTraceSurfaceCount = replacement.surfaceCount;
					chunkTraceTriangleCount = replacement.triangleCount;
					chunkTraceMaterialCount = (uint32_t)replacement.materialBridge.materials.size();
				}
			}
		}

		if (replacement.active &&
			SceneViewUsesHardwareCanvasTexture(replacement.sceneView))
		{
			mLastPerfShellTraceStats.runtimeMutationHardwareCanvasChunkCount++;
		}

		if (replacement.active && replacement.valid)
		{
			uint32_t residentChunkListIndex = UINT32_MAX;
			bool residentChunkMaterialDirty = false;
			bool residentChunkGeometryDirty = false;
			uint32_t residentChunkSurfaceCount = 0;
			uint32_t residentChunkTriangleCount = 0;
			uint32_t residentChunkMaterialCount = 0;
			bool residentChunkRecoveredEmpty = false;
			const uint32_t appliedReasonMask = replacement.reasonMask;
			const bool attemptedStaticAnimatedReplacement = replacement.staticAnimatedReplacement;
			const bool attemptedAnimationOnlyRefresh = replacement.animationOnlyRefreshed;
			const bool attemptedSuppressedAnimatedResidentApply =
				attemptedStaticAnimatedReplacement &&
				residentEntry != nullptr &&
				residentEntry->valid &&
				residentEntry->animatedRefreshSuppressed;
			const auto settleResidentNoopReplacement = [&]()
			{
				const nri_scene::PTMapChunkMutationBaseline appliedBaseline = replacement.replacementBaseline;
				replacement.baseline = appliedBaseline;
				replacement.replacementBaseline = appliedBaseline;
				replacement.baselineSignature = appliedBaseline.signature;
				replacement.liveSignature = appliedBaseline.signature;
				replacement.reasonMask = nri_scene::PTMapChunkMutationReason_None;
				replacement.sectionDirtyCount = 0;
				replacement.stableMutationFrameCount = 0;
				replacement.sectorDirty = false;
				replacement.dragged = false;
				replacement.blindSpot = false;
				replacement.excludeStaticChunk = false;
				replacement.staticAnimatedReplacement = false;
				replacement.active = false;
				replacement.valid = false;
				replacement.residentAuthoritative = true;
				replacement.animationOnlyRefreshed = false;
				replacement.animatedMaterialSignature =
					residentEntry != nullptr ? residentEntry->animatedMaterialSignature : replacement.animatedMaterialSignature;
				replacement.surfaceCount = 0;
				replacement.triangleCount = 0;
				mRuntimeMutation.ClearReplacementPayload(replacement, true);
				if (residentEntry != nullptr)
				{
					residentEntry->appliedBaseline = appliedBaseline;
					residentEntry->baselineSignature = appliedBaseline.signature;
					residentEntry->liveSignature = appliedBaseline.signature;
					residentEntry->visibleValidationFramesRemaining = 0;
				}
			};
			const bool residentNoopResidentAvailable =
				residentEntry != nullptr &&
				residentEntry->valid &&
				residentEntry->active &&
				residentEntry->mappedInStaticScene;
			const bool residentNoopIgnoreExcludeStatic = forceTopologyInvalidation;
			const bool residentNoopSignatureCandidate =
				forceTopologyInvalidation &&
				replacement.residentAuthoritative &&
				residentEntry != nullptr &&
				residentEntry->valid &&
				replacement.valid &&
				ComputeExactGeometrySignature(replacement.sceneView) == residentEntry->exactGeometrySignature &&
				replacement.animatedMaterialSignature == residentEntry->animatedMaterialSignature;
			const bool residentNoopExactMatch =
				replacement.residentAuthoritative &&
				residentNoopResidentAvailable &&
				replacement.valid &&
				(!replacement.excludeStaticChunk || residentNoopIgnoreExcludeStatic) &&
				replacement.surfaceCount == residentEntry->materialCount &&
				replacement.triangleCount == residentEntry->primitiveCount &&
				(uint32_t)replacement.materialBridge.materials.size() == residentEntry->materialCount &&
				ComputeExactGeometrySignature(replacement.sceneView) == residentEntry->exactGeometrySignature &&
				replacement.animatedMaterialSignature == residentEntry->animatedMaterialSignature;
			if (residentNoopSignatureCandidate && !residentNoopExactMatch)
			{
				if (!replacement.residentAuthoritative)
				{
					mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockNotAuthoritativeCount++;
				}
				if (!residentNoopResidentAvailable)
				{
					mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockResidentUnavailableCount++;
				}
				if (!replacement.valid)
				{
					mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockReplacementInvalidCount++;
				}
				if (replacement.excludeStaticChunk)
				{
					mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockExcludeStaticCount++;
				}
				if (residentEntry != nullptr)
				{
					if (replacement.surfaceCount != residentEntry->materialCount)
					{
						mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockSurfaceCountMismatch++;
					}
					if ((uint32_t)replacement.materialBridge.materials.size() != residentEntry->materialCount)
					{
						mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockMaterialCountMismatch++;
					}
					if (replacement.triangleCount != residentEntry->primitiveCount)
					{
						mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockPrimitiveCountMismatch++;
					}
				}
			}
			if (residentNoopExactMatch)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentNoopSkipCount++;
				mLastPerfShellTraceStats.runtimeMutationInvalidSyncSkipCount++;
				if ((attemptedStaticAnimatedReplacement || attemptedAnimationOnlyRefresh) &&
					residentEntry != nullptr &&
					residentEntry->valid)
				{
					residentEntry->runtimeAnimatedSyncSkipCount++;
					if (residentEntry->mappedInStaticScene &&
						hasCachedResidentAnimatedSlice(
							residentEntry->staticSceneChunkListIndex,
							residentEntry->animatedGeometrySignature,
							residentEntry->animatedMaterialSignature))
					{
						mLastPerfShellTraceStats.staticAnimatedResidentSliceSyncSkipHitCount++;
					}
					recordRuntimeAnimatedFrame(
						mapChunk.chunkIndex,
						attemptedSuppressedAnimatedResidentApply,
						false,
						false,
						false,
						true);
				}
				settleResidentNoopReplacement();
				chunkTraceAction = RuntimeMutationTraceAction::ResidentNoopSkip;
				chunkTraceSurfaceCount = residentEntry != nullptr ? residentEntry->materialCount : 0;
				chunkTraceTriangleCount = residentEntry != nullptr ? residentEntry->primitiveCount : 0;
				chunkTraceMaterialCount = residentEntry != nullptr ? residentEntry->materialCount : 0;
				mRuntimeMutation.TraceChunk(mapChunk, replacement, ShouldEmitTemporalTraceLogs(), (int)nri_ptmutationtracechunk, (int)nri_ptmutationtracesector);
				emitChunkTrace();
				continue;
			}
			recordRuntimeMutationResidentApplyTier(chunkVisibleNow, runtimeMutationDistanceTier, runtimeMutationCandidateSourceMask);
			const bool appliedResidentChunk = [&]()
			{
				ScopedPtPerfTimer residentApplyTierPerfTimer(
					chunkVisibleNow ?
						mLastPerfShellTraceStats.runtimeMutationResidentApplyVisibleMs :
						mLastPerfShellTraceStats.runtimeMutationResidentApplyInvisibleMs);
				double& residentApplyDistanceTierMs =
					chunkVisibleNow ?
						ignoredRuntimeMutationDistanceTierMs :
						(runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Near ?
							mLastPerfShellTraceStats.runtimeMutationResidentApplyNearMs :
							(runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Far ?
								mLastPerfShellTraceStats.runtimeMutationResidentApplyFarMs :
								mLastPerfShellTraceStats.runtimeMutationResidentApplyUnknownDistanceMs));
				ScopedPtPerfTimer residentApplyDistanceTierPerfTimer(residentApplyDistanceTierMs);
				return TryApplyRuntimeMutationChunkToResidentScene(
					mapChunk,
					replacement,
					residentChunkListIndex,
					residentChunkMaterialDirty,
					residentChunkGeometryDirty,
					residentChunkSurfaceCount,
					residentChunkTriangleCount,
					residentChunkMaterialCount,
					residentChunkRecoveredEmpty);
			}();
			if (appliedResidentChunk)
			{
				mLastPerfShellTraceStats.runtimeMutationInvalidAppliedCount++;
				mLastPerfResourceTraceStats.residentChunkBatchChunkCount++;
				if (residentChunkMaterialDirty)
				{
					mLastPerfResourceTraceStats.residentChunkBatchMaterialDirtyCount++;
				}
				if (residentChunkGeometryDirty)
				{
					mLastPerfResourceTraceStats.residentChunkBatchGeometryDirtyCount++;
				}
				if (residentChunkRecoveredEmpty)
				{
					mLastPerfResourceTraceStats.residentChunkBatchRecoverEmptyCount++;
				}
				if ((attemptedStaticAnimatedReplacement || attemptedAnimationOnlyRefresh) &&
					residentEntry != nullptr &&
					residentEntry->valid)
				{
					residentEntry->runtimeAnimatedResidentApplyCount++;
				}
				if (attemptedStaticAnimatedReplacement || attemptedAnimationOnlyRefresh)
				{
					recordRuntimeAnimatedFrame(
						mapChunk.chunkIndex,
						attemptedSuppressedAnimatedResidentApply,
						false,
						false,
						true,
						false);
				}
				if (residentEntry != nullptr)
				{
					residentEntry->visibleValidationFramesRemaining = 0;
				}
				residentStaticSceneChanged = true;
				mRuntimeMutation.lastFrame.residentAppliedChunkCount++;
				if (residentChunkMaterialDirty)
				{
					residentMaterialDirty = true;
					mRuntimeMutation.lastFrame.residentMaterialChunkCount++;
					if (residentChunkListIndex != UINT32_MAX)
					{
						residentMaterialChunkListIndices.push_back(residentChunkListIndex);
						if (attemptedStaticAnimatedReplacement || attemptedAnimationOnlyRefresh)
						{
							animatedResidentApplyMaterialChunkListIndices.push_back(residentChunkListIndex);
						}
					}
				}
				if (residentChunkGeometryDirty)
				{
					residentGeometryDirty = true;
					mRuntimeMutation.lastFrame.residentGeometryChunkCount++;
					if (residentChunkListIndex != UINT32_MAX)
					{
						residentGeometryChunkListIndices.push_back(residentChunkListIndex);
					}
				}

				chunkTraceAction = RuntimeMutationTraceAction::ResidentApply;
				chunkTraceResidentMaterialDirty = residentChunkMaterialDirty;
				chunkTraceResidentGeometryDirty = residentChunkGeometryDirty;
				chunkTraceRecoveredEmpty = residentChunkRecoveredEmpty;
				chunkTraceSurfaceCount = residentChunkSurfaceCount;
				chunkTraceTriangleCount = residentChunkTriangleCount;
				chunkTraceMaterialCount = residentChunkMaterialCount;
				mRuntimeMutation.TraceChunk(mapChunk, replacement, ShouldEmitTemporalTraceLogs(), (int)nri_ptmutationtracechunk, (int)nri_ptmutationtracesector);
				emitChunkTrace();
				continue;
			}

			if (appliedReasonMask != nri_scene::PTMapChunkMutationReason_None)
			{
				mRuntimeMutation.lastFrame.residentFallbackChunkCount++;
			}
			replacement.active = false;
			replacement.excludeStaticChunk = false;
			replacement.stableMutationFrameCount = 0;
			mLastPerfShellTraceStats.runtimeMutationInvalidFailedCount++;
			chunkTraceAction = RuntimeMutationTraceAction::ResidentFallback;
			chunkTraceSurfaceCount = replacement.surfaceCount;
			chunkTraceTriangleCount = replacement.triangleCount;
			chunkTraceMaterialCount = (uint32_t)replacement.materialBridge.materials.size();
			mRuntimeMutation.TraceChunk(mapChunk, replacement, ShouldEmitTemporalTraceLogs(), (int)nri_ptmutationtracechunk, (int)nri_ptmutationtracesector);
			emitChunkTrace();
			continue;
		}
	}

	if (worklistValidation.enabled)
	{
		uint32_t falsePositiveTraceCount = 0;
		for (uint32_t chunkListIndex = 0; chunkListIndex < (uint32_t)worklistValidation.sourceMasks.size(); ++chunkListIndex)
		{
			if (worklistValidation.sourceMasks[chunkListIndex] == 0 ||
				worklistValidation.fullDirty[chunkListIndex] != 0)
			{
				continue;
			}

			worklistValidation.falsePositiveCount++;
			if (worklistValidation.verbosity >= 2 && falsePositiveTraceCount < 8u)
			{
				const auto& mapChunk = mMapWorld.chunks[chunkListIndex];
				Printf(
					"PERF pt mutation worklist extra NRI: frame=%llu chunk=%u sector=%d source_mask=0x%x sources=%s\n",
					(unsigned long long)mFrameIndex,
					mapChunk.chunkIndex,
					mapChunk.sectorIndex,
					worklistValidation.sourceMasks[chunkListIndex],
					GetRuntimeMutationWorklistCandidateSourceSummary(worklistValidation.sourceMasks[chunkListIndex]).c_str());
				falsePositiveTraceCount++;
			}
		}
		if (worklistValidation.falseNegativeCount > 0 || worklistValidation.verbosity >= 2)
		{
			Printf(
				"PERF pt mutation worklist validate NRI: frame=%llu candidates=%u full_dirty=%u false_negatives=%u false_positives=%u chunks=%u signature_watch_candidates=%u signature_watch_seeds=%u signature_watch_total=%u background_sweep_candidates=%u verbosity=%d\n",
				(unsigned long long)mFrameIndex,
				worklistValidation.candidateCount,
				worklistValidation.fullDirtyCount,
				worklistValidation.falseNegativeCount,
				worklistValidation.falsePositiveCount,
				(uint32_t)mMapWorld.chunks.size(),
				worklistValidation.signatureWatchlistCandidateCount,
				worklistValidation.signatureWatchlistSeedCount,
				(uint32_t)std::count(mRuntimeMutation.signatureWatchlist.begin(), mRuntimeMutation.signatureWatchlist.end(), (uint8_t)1u),
				worklistValidation.backgroundSweepCandidateCount,
				worklistValidation.verbosity);
			for (uint32_t reasonMask = 0; reasonMask < (uint32_t)worklistValidation.falseNegativeReasonMaskCounts.size(); ++reasonMask)
			{
				const uint32_t count = worklistValidation.falseNegativeReasonMaskCounts[reasonMask];
				if (count == 0)
				{
					continue;
				}

				Printf(
					"PERF pt mutation worklist reason NRI: frame=%llu reason_mask=0x%x reasons=%s false_negatives=%u\n",
					(unsigned long long)mFrameIndex,
					reasonMask,
					GetRuntimeMapMutationReasonSummary(reasonMask).c_str(),
					count);
			}
		}
	}
	for (const auto& replacement : mRuntimeMutation.cache.chunks)
	{
		if (replacement.deferredMaterialRefresh)
		{
			mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredPendingChunks++;
		}
		if (replacement.deferredStructuralRebuild)
		{
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredPendingChunks++;
		}
	}
	for (uint32_t chunkListIndex = 0; chunkListIndex < (uint32_t)mRuntimeMutation.cache.chunks.size(); ++chunkListIndex)
	{
		const bool deferredMaterialRefresh = mRuntimeMutation.cache.chunks[chunkListIndex].deferredMaterialRefresh;
		const bool deferredStructuralRebuild = mRuntimeMutation.cache.chunks[chunkListIndex].deferredStructuralRebuild;
		if ((!deferredMaterialRefresh && !deferredStructuralRebuild) ||
			chunkListIndex >= mMapWorld.chunks.size())
		{
			continue;
		}

		const auto& mapChunk = mMapWorld.chunks[chunkListIndex];
		const bool chunkVisibleNow = IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunk.chunkIndex);
		if (chunkVisibleNow)
		{
			continue;
		}

		switch (getRuntimeMutationDistanceTier(chunkListIndex, false))
		{
		case RuntimeMutationDistanceTier::Near:
			if (deferredMaterialRefresh)
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredNearPendingChunks++;
			}
			if (deferredStructuralRebuild)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearPendingChunks++;
			}
			break;
		case RuntimeMutationDistanceTier::Far:
			if (deferredStructuralRebuild)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarPendingChunks++;
			}
			break;
		default:
			break;
		}
	}

	if (tracePtPerf)
	{
		for (const auto& tracker : mRuntimeRecurringChunkTrackers)
		{
			if (!tracker.valid)
			{
				continue;
			}

			mLastPerfShellTraceStats.runtimeRecurringChunkTrackedCount++;
			mLastPerfShellTraceStats.runtimeRecurringChunkVisitCount += tracker.visitCount;
			mLastPerfShellTraceStats.runtimeRecurringChunkUniqueStateCount += tracker.uniqueStateCount;
			mLastPerfShellTraceStats.runtimeRecurringChunkTransitionCount += tracker.transitionCount;
			mLastPerfShellTraceStats.runtimeRecurringChunkRepeatedStateHitCount += tracker.repeatedStateHitCount;
			mLastPerfShellTraceStats.runtimeRecurringChunkAbaRecurrenceCount += tracker.abaRecurrenceCount;
			mLastPerfShellTraceStats.runtimeRecurringChunkMaxUniqueStateCount =
				std::max(mLastPerfShellTraceStats.runtimeRecurringChunkMaxUniqueStateCount, tracker.uniqueStateCount);
			if (tracker.uniqueStateCount > 1 || tracker.repeatedStateHitCount > 0 || tracker.abaRecurrenceCount > 0)
			{
				mLastPerfShellTraceStats.runtimeRecurringChunkRecurringCount++;
			}

			RuntimeRecurringChunkTraceEntry entry = {};
			entry.valid = true;
			entry.chunkIndex = tracker.chunkIndex;
			entry.sectorIndex = tracker.sectorIndex;
			entry.lastReasonMask = tracker.lastReasonMask;
			entry.visitCount = tracker.visitCount;
			entry.uniqueStateCount = tracker.uniqueStateCount;
			entry.transitionCount = tracker.transitionCount;
			entry.repeatedStateHitCount = tracker.repeatedStateHitCount;
			entry.abaRecurrenceCount = tracker.abaRecurrenceCount;
			entry.lastWallCount = tracker.lastWallCount;
			entry.lastFlatCount = tracker.lastFlatCount;
			entry.lastTriangleCount = tracker.lastTriangleCount;
			entry.lastMaterialCount = tracker.lastMaterialCount;
			entry.previousStateSignature = tracker.previousStateSignature;
			entry.lastStateSignature = tracker.lastStateSignature;
			InsertRankedTraceEntry(
				mLastPerfShellTraceStats.runtimeRecurringChunkEntries,
				entry,
				ScoreRuntimeRecurringChunkTraceEntry);
		}
	}

	mRuntimeMutation.lastFrame.active =
		mRuntimeMutation.lastFrame.dirtyChunkCount > 0 ||
		mRuntimeMutation.lastFrame.residentAppliedChunkCount > 0 ||
		mRuntimeMutation.lastFrame.residentFallbackChunkCount > 0;
	if (startupMaterialOnlyMutationDetected && !mPendingStartupMutationRebaseline)
	{
		mPendingStartupMutationRebaseline = true;
		Printf("NRI PT startup mutation rebaseline queued: level=%s frame=%u dirty_material_chunks=%u replaced_chunks=%u\n",
			currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)",
			mFrameIndex,
			mRuntimeMutation.lastFrame.sectorMaterialChunkCount + mRuntimeMutation.lastFrame.wallMaterialChunkCount,
			mRuntimeMutation.lastFrame.dirtyChunkCount);
	}
	if (mAllowStartupMutationRebaseline && mFrameIndex > mStartupMutationRebaselineDeadlineFrame)
	{
		mAllowStartupMutationRebaseline = false;
	}
	if (collectRuntimeMutationCacheStats)
	{
		const RuntimeMutationCacheStats cacheStats = mRuntimeMutation.GatherCacheStats();
		mLastPerfShellTraceStats.runtimeMutationActiveChunkCount = cacheStats.activeChunkCount;
		mLastPerfShellTraceStats.runtimeMutationValidChunkCount = cacheStats.validChunkCount;
		mLastPerfShellTraceStats.runtimeMutationExcludedStaticChunkCount = cacheStats.excludedStaticChunkCount;
		mLastPerfShellTraceStats.runtimeMutationCachedSurfaceCount = cacheStats.cachedSurfaceCount;
		mLastPerfShellTraceStats.runtimeMutationCachedTriangleCount = cacheStats.cachedTriangleCount;
		mLastPerfShellTraceStats.runtimeMutationCachedMaterialCount = cacheStats.cachedMaterialCount;
		mLastPerfShellTraceStats.runtimeMutationCachedMaterialStateCount = cacheStats.cachedMaterialStateCount;
		mRuntimeMutation.cacheHighWaterStats.activeChunkCount = std::max(mRuntimeMutation.cacheHighWaterStats.activeChunkCount, cacheStats.activeChunkCount);
		mRuntimeMutation.cacheHighWaterStats.validChunkCount = std::max(mRuntimeMutation.cacheHighWaterStats.validChunkCount, cacheStats.validChunkCount);
		mRuntimeMutation.cacheHighWaterStats.excludedStaticChunkCount = std::max(mRuntimeMutation.cacheHighWaterStats.excludedStaticChunkCount, cacheStats.excludedStaticChunkCount);
		mRuntimeMutation.cacheHighWaterStats.cachedSurfaceCount = std::max(mRuntimeMutation.cacheHighWaterStats.cachedSurfaceCount, cacheStats.cachedSurfaceCount);
		mRuntimeMutation.cacheHighWaterStats.cachedTriangleCount = std::max(mRuntimeMutation.cacheHighWaterStats.cachedTriangleCount, cacheStats.cachedTriangleCount);
		mRuntimeMutation.cacheHighWaterStats.cachedMaterialCount = std::max(mRuntimeMutation.cacheHighWaterStats.cachedMaterialCount, cacheStats.cachedMaterialCount);
		mRuntimeMutation.cacheHighWaterStats.cachedMaterialStateCount = std::max(mRuntimeMutation.cacheHighWaterStats.cachedMaterialStateCount, cacheStats.cachedMaterialStateCount);
	}
	if (tracePtPerf)
	{
		for (const auto& chunk : mStaticMapScene.chunks)
		{
			mLastPerfShellTraceStats.staticAnimatedResidentSliceCacheEntryCount +=
				(uint32_t)chunk.residentMaterialSliceCache.size();
		}
		for (uint32_t chunkIndex = 0; chunkIndex < (uint32_t)mResidentMapChunkRegistry.entries.size(); ++chunkIndex)
		{
			const auto& entry = mResidentMapChunkRegistry.entries[chunkIndex];
			if (!entry.valid)
			{
				continue;
			}

			if (entry.animatedRefreshSuppressed)
			{
				mLastPerfShellTraceStats.runtimeAnimatedSuppressedActiveCount++;
			}
			if (chunkIndex >= runtimeAnimatedFrameStats.size())
			{
				continue;
			}

			const auto& frameEntry = runtimeAnimatedFrameStats[chunkIndex];
			if (!frameEntry.touched && !entry.animatedRefreshSuppressed)
			{
				continue;
			}

			RuntimeAnimatedChurnTraceEntry animatedEntry = {};
			animatedEntry.valid = true;
			animatedEntry.chunkIndex = chunkIndex;
			animatedEntry.sectorIndex =
				chunkIndex < mMapWorld.chunks.size() ? mMapWorld.chunks[chunkIndex].sectorIndex : -1;
			animatedEntry.suppressed = entry.animatedRefreshSuppressed || frameEntry.suppressed;
			animatedEntry.materialRefreshes = frameEntry.materialRefreshes;
			animatedEntry.runtimeAttempts = frameEntry.runtimeAttempts;
			animatedEntry.residentApplies = frameEntry.residentApplies;
			animatedEntry.syncSkips = frameEntry.syncSkips;
			InsertRankedTraceEntry(
				mLastPerfShellTraceStats.runtimeAnimatedChurnEntries,
				animatedEntry,
				ScoreRuntimeAnimatedChurnTraceEntry);
		}
	}
	mLastPerfShellTraceStats.runtimeMutationPrimitiveCount = (uint32_t)outGeometry.primitives.size();
	mLastPerfShellTraceStats.runtimeMutationMaterialCount = (uint32_t)outMaterials.materials.size();
	if (residentStaticSceneChanged)
	{
		bool residentRefreshOkay = true;
		const char* residentRefreshFailureReason = nullptr;
		if (!mRuntimeMutation.FlushResidentGeometryUploadRanges(BuildRuntimeMutationResidentUploadServices()))
		{
			residentRefreshOkay = false;
			residentRefreshFailureReason = "runtime-mutation-resident-geometry-upload-failed";
		}
		if (residentRefreshOkay && residentMaterialDirty)
		{
			std::sort(residentMaterialChunkListIndices.begin(), residentMaterialChunkListIndices.end());
			residentMaterialChunkListIndices.erase(
				std::unique(residentMaterialChunkListIndices.begin(), residentMaterialChunkListIndices.end()),
				residentMaterialChunkListIndices.end());
			std::sort(animatedResidentApplyMaterialChunkListIndices.begin(), animatedResidentApplyMaterialChunkListIndices.end());
			animatedResidentApplyMaterialChunkListIndices.erase(
				std::unique(animatedResidentApplyMaterialChunkListIndices.begin(), animatedResidentApplyMaterialChunkListIndices.end()),
				animatedResidentApplyMaterialChunkListIndices.end());
			residentRefreshOkay = RefreshResidentStaticMaterialSlices(
				residentMaterialChunkListIndices,
				"resident_runtime_mutation_static",
				&animatedResidentApplyMaterialChunkListIndices);
			if (!residentRefreshOkay)
			{
				mLastPerfResourceTraceStats.residentChunkBatchMaterialFallbackCount++;
				if (nri_ptscenestats)
				{
					Printf("NRI PT static scene trace: event=resident_material_refresh_fallback frame=%u chunks=%u\n",
						mFrameIndex,
						(uint32_t)residentMaterialChunkListIndices.size());
				}
				residentRefreshOkay = RebuildResidentStaticMaterialState("resident_runtime_mutation_static_recover");
				if (!residentRefreshOkay)
				{
					residentRefreshFailureReason = "runtime-mutation-resident-material-refresh-failed";
				}
			}
		}
		if (residentRefreshOkay && residentGeometryDirty)
		{
			std::sort(residentGeometryChunkListIndices.begin(), residentGeometryChunkListIndices.end());
			residentGeometryChunkListIndices.erase(
				std::unique(residentGeometryChunkListIndices.begin(), residentGeometryChunkListIndices.end()),
				residentGeometryChunkListIndices.end());
			residentRefreshOkay = RebuildResidentStaticMapChunkBlases(residentGeometryChunkListIndices);
			mLastPerfResourceTraceStats.residentChunkBatchBlasRebuildCount += (uint32_t)residentGeometryChunkListIndices.size();
			if (!residentRefreshOkay)
			{
				residentRefreshFailureReason = "runtime-mutation-resident-blas-refresh-failed";
			}
		}

		if (residentRefreshOkay)
		{
			mStaticMapScene.texturesResident = true;
			mStaticMapScene.buffersResident = true;
			mStaticMapScene.accelerationResident = true;
			mStaticMapScene.gpuUploadCount += residentMaterialDirty ? 1u : 0u;
			mStaticMapScene.accelerationBuildCount += (uint32_t)residentGeometryChunkListIndices.size();
			SyncResidentMapChunkRegistryFromStaticScene();
			RefreshStateCommitCombinedGeometryStaticPrefixForResidentUpdate(residentGeometryChunkListIndices);
			if (outResidentStaticSceneChanged != nullptr)
			{
				*outResidentStaticSceneChanged = residentGeometryDirty;
			}
		}
		else
		{
			DestroyStaticMapSceneCache(
				residentRefreshFailureReason != nullptr ?
					residentRefreshFailureReason :
					"runtime-mutation-resident-refresh-failed");
			mStaticMapScene = {};
			mStaticAccelerationBuildSerial = 0;
			mPreservedStaticMapSky = {};
			if (!EnsureStaticMapScene())
			{
				return false;
			}
			outGeometry = {};
			outMaterials = {};
			mRuntimeMutation.lastFrame.active = false;
			if (outResidentStaticSceneChanged != nullptr)
			{
				*outResidentStaticSceneChanged = false;
			}
			return false;
		}
	}
	return !outGeometry.primitives.empty() || !outMaterials.materials.empty();
}

bool NRIRenderer::BuildRuntimeSpaceLinkOverlay(HWDrawInfo& di, nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials)
{
	outGeometry = {};
	outMaterials = {};
	mRuntimeSpaceLinkLastFrame = {};
	mRuntimeSpaceLinkLastFrame.orphanLocalSpaceCount = CountOrphanLocalSpaces(mMapWorld);
	mRuntimeSpaceLinkLastFrame.unresolvedRuntimePortalCount = mMapWorld.stats.runtimePortalCount;

	const auto deactivateRuntimeLinkHistory = [&]()
	{
		if (!mRuntimeChunkTranslationHistory.empty())
		{
			mRuntimeSpaceLinkLastFrame.topologyChanged = true;
			RequestHistoryReset("runtime-link-deactivated", false, true);
		}
	};

	if (!mMapWorld.valid)
	{
		deactivateRuntimeLinkHistory();
		return false;
	}

	int effectSectorIndex = -1;
	if (di.Viewpoint.SectNums != nullptr)
	{
		if (di.Viewpoint.SectCount > 0)
		{
			effectSectorIndex = di.Viewpoint.SectNums[0];
			mRuntimeSpaceLinkLastFrame.viewRootSectorCount = (uint32_t)di.Viewpoint.SectCount;
		}
	}
	else
	{
		effectSectorIndex = di.Viewpoint.SectCount;
		mRuntimeSpaceLinkLastFrame.viewRootSectorCount = 1;
	}

	if (effectSectorIndex < 0 || (unsigned)effectSectorIndex >= sector.Size())
	{
		deactivateRuntimeLinkHistory();
		return false;
	}

	const BitArray& visibleSectors = di.GetVisibleSectors();
	for (unsigned sectorIndex = 0; sectorIndex < visibleSectors.Size(); ++sectorIndex)
	{
		if (visibleSectors.Check(sectorIndex))
		{
			mRuntimeSpaceLinkLastFrame.visibleSectorCount++;
		}
	}

	mRuntimeSpaceLinkLastFrame.candidateSectorIndex = effectSectorIndex;
	mRuntimeSpaceLinkLastFrame.candidateSectorLotag = sector[(unsigned)effectSectorIndex].lotag;
	mRuntimeSpaceLinkLastFrame.queryAttempted = true;

	GeoEffect effect = {};
	int providerSectorIndex = -1;
	if (gi != nullptr && gi->GetGeoEffect(&effect, &sector[effectSectorIndex]))
	{
		providerSectorIndex = effectSectorIndex;
	}
	else
	{
		mRuntimeSpaceLinkLastFrame.queryRejected = true;
	}

	const auto getLocalSpaceIndex = [&](int sectorIndex) -> uint32_t
	{
		if (sectorIndex < 0 || (unsigned)sectorIndex >= mMapWorld.chunks.size())
		{
			return UINT32_MAX;
		}

		return mMapWorld.chunks[(unsigned)sectorIndex].localSpaceIndex;
	};

	const uint32_t candidateLocalSpaceIndex = getLocalSpaceIndex(effectSectorIndex);
	const auto sectorMatchesVisibleSet = [&](int sectorIndex) -> bool
	{
		return sectorIndex >= 0 &&
			(unsigned)sectorIndex < visibleSectors.Size() &&
			visibleSectors.Check((unsigned)sectorIndex);
	};
	auto groupMatchesCandidate = [&](const GeoEffect& candidateEffect, int groupIndex) -> bool
	{
		auto matchesSector = [&](sectortype* sect) -> bool
		{
			if (sect == nullptr)
			{
				return false;
			}

			const int sectorIndex = sector.IndexOf(sect);
			if (sectorIndex < 0)
			{
				return false;
			}

			if (sectorIndex == effectSectorIndex)
			{
				return true;
			}

			if (candidateLocalSpaceIndex == UINT32_MAX)
			{
				return false;
			}

			return getLocalSpaceIndex(sectorIndex) == candidateLocalSpaceIndex;
		};

		return
			matchesSector(candidateEffect.geosector != nullptr ? candidateEffect.geosector[groupIndex] : nullptr) ||
			matchesSector(candidateEffect.geosectorwarp != nullptr ? candidateEffect.geosectorwarp[groupIndex] : nullptr) ||
			matchesSector(candidateEffect.geosectorwarp2 != nullptr ? candidateEffect.geosectorwarp2[groupIndex] : nullptr);
	};
	auto groupMatchesVisibleSectors = [&](const GeoEffect& candidateEffect, int groupIndex) -> bool
	{
		auto matchesVisible = [&](sectortype* sect) -> bool
		{
			if (sect == nullptr)
			{
				return false;
			}

			const int sectorIndex = sector.IndexOf(sect);
			return sectorMatchesVisibleSet(sectorIndex);
		};

		return
			matchesVisible(candidateEffect.geosector != nullptr ? candidateEffect.geosector[groupIndex] : nullptr) ||
			matchesVisible(candidateEffect.geosectorwarp != nullptr ? candidateEffect.geosectorwarp[groupIndex] : nullptr) ||
			matchesVisible(candidateEffect.geosectorwarp2 != nullptr ? candidateEffect.geosectorwarp2[groupIndex] : nullptr);
	};

	if (gi != nullptr)
	{
		for (unsigned sectorIndex = 0; sectorIndex < sector.Size(); ++sectorIndex)
		{
			if (sector[sectorIndex].lotag != 848)
			{
				continue;
			}

			mRuntimeSpaceLinkLastFrame.providerSectorCount++;

			GeoEffect candidateEffect = {};
			if (!gi->GetGeoEffect(&candidateEffect, &sector[sectorIndex]) || candidateEffect.geocnt <= 0)
			{
				continue;
			}

			mRuntimeSpaceLinkLastFrame.geoProviderCount++;
			mRuntimeSpaceLinkLastFrame.providerGroupCount += (uint32_t)candidateEffect.geocnt;

			bool matched = false;
			bool visibleMatched = false;
			for (int i = 0; i < candidateEffect.geocnt; ++i)
			{
				if (groupMatchesCandidate(candidateEffect, i))
				{
					matched = true;
				}
				if (groupMatchesVisibleSectors(candidateEffect, i))
				{
					visibleMatched = true;
				}
			}

			if (matched)
			{
				mRuntimeSpaceLinkLastFrame.localSpaceMatchedProviderCount++;
			}
			if (visibleMatched)
			{
				mRuntimeSpaceLinkLastFrame.visibleMatchedProviderCount++;
			}

			if (providerSectorIndex >= 0 || !matched)
			{
				continue;
			}

			effect = candidateEffect;
			providerSectorIndex = (int)sectorIndex;
			break;
		}
	}

	if (providerSectorIndex < 0)
	{
		mRuntimeSpaceLinkLastFrame.queryRejected = true;
		deactivateRuntimeLinkHistory();
		return false;
	}

	mRuntimeSpaceLinkLastFrame.sourceSectorIndex = providerSectorIndex;
	mRuntimeSpaceLinkLastFrame.reportedGeoCount = effect.geocnt;
	if (effect.geocnt <= 0)
	{
		mRuntimeSpaceLinkLastFrame.queryRejected = true;
		deactivateRuntimeLinkHistory();
		return false;
	}

	struct RuntimeGeoLink
	{
		uint32_t chunkIndex = UINT32_MAX;
		float dx = 0.0f;
		float dz = 0.0f;
		float prevDx = 0.0f;
		float prevDz = 0.0f;
	};

	std::vector<RuntimeGeoLink> links;
	links.reserve((size_t)effect.geocnt * 2u);

	auto appendLink = [&](sectortype* warpedSector, double mapDx, double mapDy)
	{
		if (warpedSector == nullptr)
		{
			return;
		}

		const int32_t sectorIndex = sector.IndexOf(warpedSector);
		if (sectorIndex < 0 || (unsigned)sectorIndex >= mMapWorld.chunks.size())
		{
			return;
		}

		RuntimeGeoLink link = {};
		link.chunkIndex = (uint32_t)sectorIndex;
		link.dx = (float)mapDx;
		link.dz = (float)-mapDy;
		for (const RuntimeGeoLink& existing : links)
		{
			if (existing.chunkIndex == link.chunkIndex &&
				fabs(existing.dx - link.dx) < 0.001f &&
				fabs(existing.dz - link.dz) < 0.001f)
			{
				return;
			}
		}

		links.push_back(link);
	};

	for (int i = 0; i < effect.geocnt; ++i)
	{
		if (!groupMatchesCandidate(effect, i))
		{
			continue;
		}

		appendLink(effect.geosectorwarp != nullptr ? effect.geosectorwarp[i] : nullptr,
			effect.geox != nullptr ? effect.geox[i] : 0.0,
			effect.geoy != nullptr ? effect.geoy[i] : 0.0);
		appendLink(effect.geosectorwarp2 != nullptr ? effect.geosectorwarp2[i] : nullptr,
			effect.geox2 != nullptr ? effect.geox2[i] : 0.0,
			effect.geoy2 != nullptr ? effect.geoy2[i] : 0.0);
	}

	if (links.empty())
	{
		deactivateRuntimeLinkHistory();
		return false;
	}

	const auto findPreviousTranslation = [&](uint32_t chunkIndex, float& outPrevDx, float& outPrevDz) -> bool
	{
		for (const RuntimeChunkTranslationState& previous : mRuntimeChunkTranslationHistory)
		{
			if (previous.chunkIndex == chunkIndex)
			{
				outPrevDx = previous.dx;
				outPrevDz = previous.dz;
				return true;
			}
		}

		return false;
	};

	for (RuntimeGeoLink& link : links)
	{
		findPreviousTranslation(link.chunkIndex, link.prevDx, link.prevDz);
	}

	const auto runtimeLinkTopologyChanged = [&]() -> bool
	{
		if (links.size() != mRuntimeChunkTranslationHistory.size())
		{
			return true;
		}

		for (const RuntimeGeoLink& link : links)
		{
			bool found = false;
			for (const RuntimeChunkTranslationState& previous : mRuntimeChunkTranslationHistory)
			{
				if (previous.chunkIndex == link.chunkIndex)
				{
					found = true;
					break;
				}
			}

			if (!found)
			{
				return true;
			}
		}

		return false;
	};

	mRuntimeSpaceLinkLastFrame.geoEffectActive = true;
	mRuntimeSpaceLinkLastFrame.linkCount = (uint32_t)links.size();
	mRuntimeSpaceLinkLastFrame.topologyChanged = runtimeLinkTopologyChanged();
	if (mRuntimeSpaceLinkLastFrame.topologyChanged)
	{
		RequestHistoryReset("runtime-link-topology");
	}

	std::vector<RuntimeChunkTranslationState> nextRuntimeChunkTranslationHistory;
	nextRuntimeChunkTranslationHistory.reserve(links.size());

	for (const RuntimeGeoLink& link : links)
	{
		if (link.chunkIndex >= mMapWorld.chunks.size())
		{
			continue;
		}

		nri_scene::SceneView liveChunkView;
		nri_scene::PTMapWorldStats liveStats = {};
		if (!nri_scene::BuildLiveMapChunkSceneView(mMapWorld.chunks[link.chunkIndex], liveChunkView, &liveStats))
		{
			continue;
		}
		if (nri_ptceilingnudge)
		{
			NudgeMapCeilingSections(liveChunkView, (float)nri_ptceilingnudgedistance);
		}

		nri_scene::GeometryData chunkGeometry;
		nri_scene::MaterialBridgeData chunkMaterials;
		{
			Clocker clock(NriPTGeometryBuild);
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildRuntimeSpaceLinkMs);
			nri_scene::BuildGeometry(liveChunkView, chunkGeometry);
			AssignGeometryPortalIndices(mMapWorld, chunkGeometry);
			TranslateGeometry(chunkGeometry, link.dx, 0.0f, link.dz, link.prevDx, 0.0f, link.prevDz);
		}
		mLastPerfShellTraceStats.geometryBuildRuntimeSpaceLinkCalls++;
		mLastPerfShellTraceStats.geometryBuildRuntimeSpaceLinkPrimitives += (uint32_t)chunkGeometry.primitives.size();
		{
			Clocker clock(NriPTMaterialBuild);
			BuildMaterialsWithActorOverrides(liveChunkView, chunkMaterials, "runtime_space_link_chunk");
		}

		if (!chunkGeometry.primitives.empty())
		{
			AppendGeometry(chunkGeometry, (uint32_t)outMaterials.materials.size(), outGeometry);
		}
		nri_scene::AppendMaterialBridge(chunkMaterials, outMaterials);

		mRuntimeSpaceLinkLastFrame.translatedChunkCount++;
		mRuntimeSpaceLinkLastFrame.surfaceCount += liveStats.surfaceCount;
		mRuntimeSpaceLinkLastFrame.triangleCount += liveStats.triangleCount;
		mRuntimeSpaceLinkLastFrame.materialCount += (uint32_t)chunkMaterials.materials.size();
		nextRuntimeChunkTranslationHistory.push_back({ link.chunkIndex, link.dx, link.dz });
	}

	mRuntimeChunkTranslationHistory = std::move(nextRuntimeChunkTranslationHistory);
	mRuntimeSpaceLinkLastFrame.active = !outGeometry.primitives.empty();
	return mRuntimeSpaceLinkLastFrame.active;
}

bool NRIRenderer::RestoreStaticTopLevelScene()
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.restoreStaticSceneMs);
	std::vector<nri::TopLevelInstance> instances;
	std::vector<SceneInstanceData> sceneInstances;
	BuildStaticMapInstances(instances, sceneInstances);
	return
		BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static) &&
		UpdateSceneDataSet(
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
			0u,
			(uint32_t)mStaticMapScene.gpuMaterials.size(),
			0u,
			"restore_static_scene");
}

bool NRIRenderer::RefreshResidentStaticSceneDataSet()
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.residentLightRefreshMs);
	std::vector<SceneInstanceData> sceneInstances;
	std::vector<nri::TopLevelInstance> ignoredInstances;
	BuildStaticMapInstances(ignoredInstances, sceneInstances);
	return UpdateSceneDataSet(
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
		0u,
		(uint32_t)mStaticMapScene.gpuMaterials.size(),
			0u,
			"refresh_resident_static_scene");
}

bool NRIRenderer::RefreshResidentStaticMaterialSlices(
	const std::vector<uint32_t>& chunkListIndices,
	const char* reason,
	const std::vector<uint32_t>* animatedApplyChunkListIndices)
{
	static constexpr size_t ResidentAnimatedMaterialSliceCacheLimit = 4;

	if (chunkListIndices.empty())
	{
		return true;
	}
	if (!mStaticMapScene.valid ||
		!mStaticMapScene.buffersResident ||
		!mStaticMapChunkAtlas.valid)
	{
		if (nri_ptscenestats)
		{
			Printf("NRI PT static scene trace: event=resident_material_refresh_failed reason=precondition scene_valid=%s buffers=%s atlas=%s chunks=%u\n",
				YesNo(mStaticMapScene.valid),
				YesNo(mStaticMapScene.buffersResident),
				YesNo(mStaticMapChunkAtlas.valid),
				(uint32_t)chunkListIndices.size());
		}
		return false;
	}

	bool textureTableGrew = false;
	for (uint32_t chunkListIndex : chunkListIndices)
	{
		if (chunkListIndex >= mStaticMapScene.chunks.size() ||
			chunkListIndex >= mStaticMapChunkAtlas.chunks.size())
		{
			if (nri_ptscenestats)
			{
				Printf("NRI PT static scene trace: event=resident_material_refresh_failed reason=chunk-index-oob chunk_list_index=%u scene_chunks=%u atlas_chunks=%u\n",
					chunkListIndex,
					(uint32_t)mStaticMapScene.chunks.size(),
					(uint32_t)mStaticMapChunkAtlas.chunks.size());
			}
			return false;
		}

		auto& chunkCache = mStaticMapScene.chunks[chunkListIndex];
		const auto& atlasChunk = mStaticMapChunkAtlas.chunks[chunkListIndex];
		if (!chunkCache.active || !atlasChunk.valid || atlasChunk.materialCount == 0)
		{
			continue;
		}
		if (atlasChunk.materialOffset + atlasChunk.materialCount > mStaticMapScene.materialBridge.materials.size() ||
			atlasChunk.materialOffset + atlasChunk.materialCount > mStaticMapScene.materialBridge.lightMetadata.size() ||
			atlasChunk.materialOffset + atlasChunk.materialCount > mStaticMapScene.gpuMaterials.size())
		{
			if (nri_ptscenestats)
			{
				Printf("NRI PT static scene trace: event=resident_material_refresh_failed reason=atlas-range-oob chunk=%u chunk_list_index=%u material_offset=%u material_count=%u bridge=%u light_meta=%u gpu=%u\n",
					chunkCache.chunkIndex,
					chunkListIndex,
					atlasChunk.materialOffset,
					atlasChunk.materialCount,
					(uint32_t)mStaticMapScene.materialBridge.materials.size(),
					(uint32_t)mStaticMapScene.materialBridge.lightMetadata.size(),
					(uint32_t)mStaticMapScene.gpuMaterials.size());
			}
			return false;
		}

		nri_scene::MaterialBridgeData remappedChunkBridge;
		std::vector<nri_scene::MaterialData> remappedGpuMaterials;
		const uint64_t materialBridgeHash = HashMaterialBridgeSummary(chunkCache.materialBridge);
		uint64_t actorOverrideHash = 0;
		uint64_t emissiveOverrideHash = 0;
		bool chunkTextureTableGrew = false;
		bool reusedCachedRemap = false;
		bool reusedCachedGpuPayload = false;
		const StaticMapSceneCache::ChunkCache::ResidentMaterialSliceCacheEntry* remapCacheEntry = nullptr;
		size_t gpuPayloadCacheEntryIndex = chunkCache.residentMaterialSliceCache.size();
		for (const auto& cacheEntry : chunkCache.residentMaterialSliceCache)
		{
			if (cacheEntry.animatedGeometrySignature != chunkCache.animatedGeometrySignature ||
				cacheEntry.animatedMaterialSignature != chunkCache.animatedMaterialSignature ||
				cacheEntry.materialBridgeHash != materialBridgeHash ||
				cacheEntry.materialCount != atlasChunk.materialCount)
			{
				continue;
			}

			if (remapCacheEntry == nullptr)
			{
				remapCacheEntry = &cacheEntry;
			}
		}
		if (remapCacheEntry != nullptr)
		{
			remappedChunkBridge = remapCacheEntry->remappedMaterialBridge;
			reusedCachedRemap = true;
		}
		const bool animatedApplyChunk =
			animatedApplyChunkListIndices != nullptr &&
			std::binary_search(
				animatedApplyChunkListIndices->begin(),
				animatedApplyChunkListIndices->end(),
				chunkListIndex);
		if (reusedCachedRemap)
		{
			mLastPerfShellTraceStats.staticAnimatedResidentSliceCacheHitCount++;
			if (animatedApplyChunk)
			{
				mLastPerfShellTraceStats.staticAnimatedResidentSliceApplyHitCount++;
			}
		}
		else
		{
			mLastPerfShellTraceStats.staticAnimatedResidentSliceCacheMissCount++;
			if (animatedApplyChunk)
			{
				mLastPerfShellTraceStats.staticAnimatedResidentSliceApplyMissCount++;
			}
			RemapMaterialBridgeAgainstTextureTable(
				chunkCache.materialBridge,
				mStaticMapScene.materialBridge,
				remappedChunkBridge,
				&chunkTextureTableGrew);
		}
		actorOverrideHash = ComputeChunkActorOverrideHash(remappedChunkBridge);
		emissiveOverrideHash = ComputeChunkEmissiveOverrideHash(remappedChunkBridge);
		for (size_t cacheIndex = 0; cacheIndex < chunkCache.residentMaterialSliceCache.size(); ++cacheIndex)
		{
			const auto& cacheEntry = chunkCache.residentMaterialSliceCache[cacheIndex];
			if (cacheEntry.animatedGeometrySignature != chunkCache.animatedGeometrySignature ||
				cacheEntry.animatedMaterialSignature != chunkCache.animatedMaterialSignature ||
				cacheEntry.materialBridgeHash != materialBridgeHash ||
				cacheEntry.materialCount != atlasChunk.materialCount ||
				cacheEntry.actorOverrideHash != actorOverrideHash ||
				cacheEntry.emissiveOverrideHash != emissiveOverrideHash ||
				cacheEntry.gpuMaterials.size() != atlasChunk.materialCount)
			{
				continue;
			}

			gpuPayloadCacheEntryIndex = cacheIndex;
			break;
		}
		if (gpuPayloadCacheEntryIndex < chunkCache.residentMaterialSliceCache.size())
		{
			remappedGpuMaterials = chunkCache.residentMaterialSliceCache[gpuPayloadCacheEntryIndex].gpuMaterials;
			reusedCachedGpuPayload = true;
		}
		if (reusedCachedGpuPayload)
		{
			mLastPerfShellTraceStats.staticAnimatedResidentGpuPayloadCacheHitCount++;
		}
		else
		{
			mLastPerfShellTraceStats.staticAnimatedResidentGpuPayloadCacheMissCount++;
		}
		textureTableGrew = textureTableGrew || chunkTextureTableGrew;
		if ((uint32_t)remappedChunkBridge.materials.size() != atlasChunk.materialCount ||
			(uint32_t)remappedChunkBridge.lightMetadata.size() != atlasChunk.materialCount)
		{
			if (nri_ptscenestats)
			{
				Printf("NRI PT static scene trace: event=resident_material_refresh_failed reason=material-count-mismatch chunk=%u chunk_list_index=%u remapped_materials=%u remapped_light=%u atlas_materials=%u\n",
					chunkCache.chunkIndex,
					chunkListIndex,
					(uint32_t)remappedChunkBridge.materials.size(),
					(uint32_t)remappedChunkBridge.lightMetadata.size(),
					atlasChunk.materialCount);
			}
			return false;
		}
		std::copy_n(
			remappedChunkBridge.materials.data(),
			atlasChunk.materialCount,
			mStaticMapScene.materialBridge.materials.data() + atlasChunk.materialOffset);
		std::copy_n(
			remappedChunkBridge.lightMetadata.data(),
			atlasChunk.materialCount,
			mStaticMapScene.materialBridge.lightMetadata.data() + atlasChunk.materialOffset);

		if (!reusedCachedGpuPayload)
		{
			remappedGpuMaterials = remappedChunkBridge.materials;
			ApplyEmissiveMaterialOverrides(remappedChunkBridge, remappedGpuMaterials);
			ApplyActorShadowMaterialOverrides(remappedChunkBridge, remappedGpuMaterials);
		}
		std::copy_n(
			remappedGpuMaterials.data(),
			atlasChunk.materialCount,
			mStaticMapScene.gpuMaterials.data() + atlasChunk.materialOffset);
		if (!reusedCachedRemap || !reusedCachedGpuPayload)
		{
			StaticMapSceneCache::ChunkCache::ResidentMaterialSliceCacheEntry cacheEntry = {};
			cacheEntry.animatedGeometrySignature = chunkCache.animatedGeometrySignature;
			cacheEntry.animatedMaterialSignature = chunkCache.animatedMaterialSignature;
			cacheEntry.materialBridgeHash = materialBridgeHash;
			cacheEntry.actorOverrideHash = actorOverrideHash;
			cacheEntry.emissiveOverrideHash = emissiveOverrideHash;
			cacheEntry.materialCount = atlasChunk.materialCount;
			cacheEntry.remappedMaterialBridge = remappedChunkBridge;
			cacheEntry.gpuMaterials = remappedGpuMaterials;
			if (chunkCache.residentMaterialSliceCache.size() >= ResidentAnimatedMaterialSliceCacheLimit)
			{
				chunkCache.residentMaterialSliceCache.erase(chunkCache.residentMaterialSliceCache.begin());
			}
			chunkCache.residentMaterialSliceCache.push_back(std::move(cacheEntry));
			if (!reusedCachedRemap)
			{
				mLastPerfShellTraceStats.staticAnimatedResidentSliceCacheStoreCount++;
			}
			if (!reusedCachedGpuPayload)
			{
				mLastPerfShellTraceStats.staticAnimatedResidentGpuPayloadCacheStoreCount++;
			}
		}

		if (!textureTableGrew &&
			!StageResidentBufferCopyRange(
				mStaticMaterialBuffer,
				(uint64_t)atlasChunk.materialOffset * sizeof(nri_scene::MaterialData),
				remappedGpuMaterials.data(),
				(uint64_t)atlasChunk.materialCount * sizeof(nri_scene::MaterialData),
				NRIComputeShaderResourceAccess(),
				ResidentUploadKind_Material))
		{
			if (nri_ptscenestats)
			{
				Printf("NRI PT static scene trace: event=resident_material_refresh_failed reason=buffer-update chunk=%u chunk_list_index=%u material_offset=%u material_count=%u\n",
					chunkCache.chunkIndex,
					chunkListIndex,
					atlasChunk.materialOffset,
					atlasChunk.materialCount);
			}
			return false;
		}
		if (!textureTableGrew)
		{
			mLastPerfResourceTraceStats.residentChunkBatchMaterialBytes +=
				(uint64_t)atlasChunk.materialCount * sizeof(nri_scene::MaterialData);
		}
	}

	if (!textureTableGrew)
	{
		return true;
	}

	const bool rebuiltOkay =
		EnsurePaletteTexture(mStaticMapScene.materialBridge) &&
		EnsureSceneTextures(
			mStaticMapScene.sceneView,
			mStaticMapScene.materialBridge,
			mStaticMapScene.gpuMaterials,
			false,
			reason != nullptr ? reason : "resident_runtime_mutation_static") &&
		UploadStaticMapChunkMaterialAtlas(
			mStaticMaterialBuffer,
			mStaticMapChunkAtlas,
			mStaticMapScene,
			mStaticMapScene.gpuMaterials);
	if (!rebuiltOkay)
	{
		if (nri_ptscenestats)
		{
			Printf("NRI PT static scene trace: event=resident_material_refresh_failed reason=texture-table-rebuild\n");
		}
	}
	return rebuiltOkay;
}

bool NRIRenderer::RebuildResidentStaticMaterialState(const char* reason)
{
	const nri_scene::SceneView* preservedSkyView =
		(mPreservedStaticMapSky.valid && mPreservedStaticMapSky.buildSerial == mMapWorld.buildSerial) ?
		&mPreservedStaticMapSky.sceneView :
		nullptr;
	nri_scene::BuildMapSceneView(mMapWorld, mStaticMapScene.sceneView, preservedSkyView);
	if (!RebuildResidentStaticMaterialBridgeFromChunks())
	{
		return false;
	}

	return
		EnsurePaletteTexture(mStaticMapScene.materialBridge) &&
		EnsureSceneTextures(
			mStaticMapScene.sceneView,
			mStaticMapScene.materialBridge,
			mStaticMapScene.gpuMaterials,
			false,
			reason != nullptr ? reason : "resident_runtime_mutation_static") &&
		UploadStaticMapChunkMaterialAtlas(
			mStaticMaterialBuffer,
			mStaticMapChunkAtlas,
			mStaticMapScene,
			mStaticMapScene.gpuMaterials) &&
		RefreshResidentStaticSceneDataSet();
}

bool NRIRenderer::RebuildResidentStaticMapChunkBlases(const std::vector<uint32_t>& chunkListIndices)
{
	ScopedPtPerfTimer totalPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasMs);
	if (chunkListIndices.empty())
	{
		return true;
	}

	auto& activeChunkListIndices = mResidentStaticBlasActiveChunkListIndices;
	activeChunkListIndices.clear();
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasSetupMs);
		{
			ScopedPtPerfTimer filterPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasFilterMs);
			activeChunkListIndices.reserve(chunkListIndices.size());
			for (uint32_t chunkListIndex : chunkListIndices)
			{
				if (chunkListIndex >= mStaticMapScene.chunks.size() ||
					chunkListIndex >= mStaticMapChunkAtlas.chunks.size())
				{
					return false;
				}

				const auto& chunk = mStaticMapScene.chunks[chunkListIndex];
				const auto& atlasChunk = mStaticMapChunkAtlas.chunks[chunkListIndex];
				if (!chunk.active || !atlasChunk.valid || atlasChunk.indexCount == 0 || atlasChunk.primitiveCount == 0)
				{
					continue;
				}

				activeChunkListIndices.push_back(chunkListIndex);
			}
		}

		if (activeChunkListIndices.empty())
		{
			return true;
		}

		uint64_t maxScratchSize = 0;
		for (uint32_t chunkListIndex : activeChunkListIndices)
		{
			auto& chunk = mStaticMapScene.chunks[chunkListIndex];
			const auto& atlasChunk = mStaticMapChunkAtlas.chunks[chunkListIndex];
			const bool hadAccelerationStructure = chunk.accelerationStructure.accelerationStructure != nullptr;

			const bool canUpdateResidentBlas =
				chunk.blasUpdateEligible &&
				hadAccelerationStructure;
			chunk.blasUpdateEligible = canUpdateResidentBlas;
			auto* accelerationStructure = chunk.accelerationStructure.accelerationStructure;
			if (chunk.residentBlasScratchSizeCacheKey != accelerationStructure)
			{
				chunk.residentBlasScratchSizeCacheKey = accelerationStructure;
				chunk.residentBlasBuildScratchSize = 0;
				chunk.residentBlasUpdateScratchSize = 0;
			}
			if (canUpdateResidentBlas)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasReuseCount++;
				mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasUpdateCount++;
				uint64_t scratchSize = chunk.residentBlasUpdateScratchSize;
				if (scratchSize != 0)
				{
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchCacheHitCount++;
				}
				else
				{
					ScopedPtPerfTimer scratchPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasScratchMs);
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchCacheMissCount++;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchQueryCount++;
					scratchSize = mFrameBuffer->mRayTracing.GetAccelerationStructureUpdateScratchBufferSize(*accelerationStructure);
					chunk.residentBlasUpdateScratchSize = scratchSize;
				}
				maxScratchSize = std::max(maxScratchSize, scratchSize);
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateCount++;
				uint32_t fallbackMask = 0;
				if (!hadAccelerationStructure)
				{
					fallbackMask |= RuntimeResidentBlasRecreateFallback_NoPreviousAs;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateNoPreviousAsCount++;
				}
				if (chunk.lastResidentBlasRecoveredEmpty)
				{
					fallbackMask |= RuntimeResidentBlasRecreateFallback_RecoveredEmpty;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateRecoveredEmptyCount++;
				}
				if (!chunk.lastResidentBlasKeptGeometrySlice)
				{
					fallbackMask |= RuntimeResidentBlasRecreateFallback_SliceMoved;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateSliceMovedCount++;
				}
				if (chunk.lastResidentBlasTopologyChanged)
				{
					fallbackMask |= RuntimeResidentBlasRecreateFallback_TopologyChanged;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateTopologyChangedCount++;
				}
				if (chunk.lastResidentBlasForceTopology)
				{
					fallbackMask |= RuntimeResidentBlasRecreateFallback_ForceTopology;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateForceTopologyCount++;
				}
				if (ShouldTracePtPerf())
				{
					RuntimeResidentBlasRecreateTraceEntry entry = {};
					entry.valid = true;
					entry.chunkIndex = chunk.chunkIndex;
					entry.sectorIndex =
						chunk.chunkIndex < mMapWorld.chunks.size() ?
						mMapWorld.chunks[chunk.chunkIndex].sectorIndex :
						-1;
					entry.reasonMask = chunk.lastResidentBlasReasonMask;
					entry.fallbackMask = fallbackMask;
					entry.surfaceCount = chunk.lastResidentBlasSurfaceCount;
					entry.triangleCount = chunk.lastResidentBlasTriangleCount;
					entry.materialCount = chunk.lastResidentBlasMaterialCount;
					entry.forceTopology = chunk.lastResidentBlasForceTopology;
					entry.recoveredEmpty = chunk.lastResidentBlasRecoveredEmpty;
					entry.keptGeometrySlice = chunk.lastResidentBlasKeptGeometrySlice;
					entry.topologyChanged = chunk.lastResidentBlasTopologyChanged;
					entry.hadAccelerationStructure = hadAccelerationStructure;
					InsertRankedTraceEntry(
						mLastPerfShellTraceStats.runtimeResidentBlasRecreateEntries,
						entry,
						ScoreRuntimeResidentBlasRecreateTraceEntry);
				}
				{
					ScopedPtPerfTimer createPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasCreateMs);
					RetireResidentAccelerationStructure(chunk.accelerationStructure);
					chunk.residentBlasScratchSizeCacheKey = nullptr;
					chunk.residentBlasBuildScratchSize = 0;
					chunk.residentBlasUpdateScratchSize = 0;

					nri::BottomLevelGeometryDesc geometryDesc = {};
					geometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
					geometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
					geometryDesc.triangles.vertexBuffer = mStaticVertexBuffer.buffer;
					geometryDesc.triangles.vertexOffset = 0;
					geometryDesc.triangles.vertexNum = mStaticMapChunkAtlas.vertexCount;
					geometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
					geometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
					geometryDesc.triangles.indexBuffer = mStaticIndexBuffer.buffer;
					geometryDesc.triangles.indexOffset = (uint64_t)atlasChunk.indexOffset * sizeof(uint32_t);
					geometryDesc.triangles.indexNum = atlasChunk.indexCount;
					geometryDesc.triangles.indexType = nri::IndexType::UINT32;

					nri::AccelerationStructureDesc blasDesc = {};
					blasDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
					blasDesc.flags = GetStaticMapChunkBlasBuildBits();
					blasDesc.geometryOrInstanceNum = 1;
					blasDesc.geometries = &geometryDesc;
					if (mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, blasDesc, chunk.accelerationStructure.accelerationStructure) != nri::Result::SUCCESS)
					{
						return false;
					}

					nri::MemoryDesc memoryDesc = {};
					mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(*chunk.accelerationStructure.accelerationStructure, nri::MemoryLocation::DEVICE, memoryDesc);
					chunk.accelerationStructure.memorySize = memoryDesc.size;
					chunk.accelerationStructure.memoryLocation = nri::MemoryLocation::DEVICE;
				}

				{
					ScopedPtPerfTimer scratchPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasScratchMs);
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchCacheMissCount++;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchQueryCount++;
					const uint64_t scratchSize =
						mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*chunk.accelerationStructure.accelerationStructure);
					chunk.residentBlasScratchSizeCacheKey = chunk.accelerationStructure.accelerationStructure;
					chunk.residentBlasBuildScratchSize = scratchSize;
					chunk.residentBlasUpdateScratchSize = 0;
					maxScratchSize = std::max(maxScratchSize, scratchSize);
				}
			}
		}

		{
			ScopedPtPerfTimer scratchPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasScratchMs);
			if (mResidentStaticBlasScratchBuffer.buffer == nullptr || mResidentStaticBlasScratchBuffer.size < maxScratchSize)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchGrowCount++;
				if (mResidentStaticBlasScratchBuffer.buffer != nullptr)
				{
					WaitForCommandsTracked();
				}
				DestroyBufferResource(mResidentStaticBlasScratchBuffer);
				if (!CreateBufferWithoutView(mResidentStaticBlasScratchBuffer, maxScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
				{
					return false;
				}
			}
		}
	}

	auto& blasBarriers = mResidentStaticBlasBarriers;
	blasBarriers.clear();
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasBuildMs);
		blasBarriers.reserve(activeChunkListIndices.size());
		for (size_t i = 0; i < activeChunkListIndices.size(); ++i)
		{
			const uint32_t chunkListIndex = activeChunkListIndices[i];
			auto& chunk = mStaticMapScene.chunks[chunkListIndex];
			const auto& atlasChunk = mStaticMapChunkAtlas.chunks[chunkListIndex];

			nri::BottomLevelGeometryDesc geometryDesc = {};
			geometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
			geometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
			geometryDesc.triangles.vertexBuffer = mStaticVertexBuffer.buffer;
			geometryDesc.triangles.vertexOffset = 0;
			geometryDesc.triangles.vertexNum = mStaticMapChunkAtlas.vertexCount;
			geometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
			geometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
			geometryDesc.triangles.indexBuffer = mStaticIndexBuffer.buffer;
			geometryDesc.triangles.indexOffset = (uint64_t)atlasChunk.indexOffset * sizeof(uint32_t);
			geometryDesc.triangles.indexNum = atlasChunk.indexCount;
			geometryDesc.triangles.indexType = nri::IndexType::UINT32;

			nri::BuildBottomLevelAccelerationStructureDesc build = {};
			build.dst = chunk.accelerationStructure.accelerationStructure;
			build.src = chunk.blasUpdateEligible ? chunk.accelerationStructure.accelerationStructure : nullptr;
			build.geometries = &geometryDesc;
			build.geometryNum = 1;
			build.scratchBuffer = mResidentStaticBlasScratchBuffer.buffer;
			build.scratchOffset = 0;
			mFrameBuffer->mRayTracing.CmdBuildBottomLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &build, 1);
			mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasBuildCommandCount++;

			if (i + 1 < activeChunkListIndices.size())
			{
				ScopedPtPerfTimer barrierPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasBarrierMs);
				nri::BufferBarrierDesc scratchBarrier = {};
				scratchBarrier.buffer = mResidentStaticBlasScratchBuffer.buffer;
				scratchBarrier.before = NRIAccelerationStructureScratchAccess();
				scratchBarrier.after = NRIAccelerationStructureScratchAccess();

				nri::BarrierDesc scratchBarrierDesc = {};
				scratchBarrierDesc.buffers = &scratchBarrier;
				scratchBarrierDesc.bufferNum = 1;
				mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, scratchBarrierDesc);
				mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchBarrierCount++;
			}

			{
				ScopedPtPerfTimer barrierPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasBarrierMs);
				nri::BufferBarrierDesc barrier = {};
				barrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*chunk.accelerationStructure.accelerationStructure);
				barrier.before = NRIAccelerationStructureWriteAccess();
				barrier.after = NRIAccelerationStructureReadAccess();
				blasBarriers.push_back(barrier);
			}
		}

		if (!blasBarriers.empty())
		{
			ScopedPtPerfTimer barrierPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasBarrierMs);
			nri::BarrierDesc barrierDesc = {};
			barrierDesc.buffers = blasBarriers.data();
			barrierDesc.bufferNum = (uint32_t)blasBarriers.size();
			mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrierDesc);
		}
	}
	return true;
}

NRIRenderer::RuntimeMutationResidentApplyMode NRIRenderer::ClassifyRuntimeMutationResidentApplyMode(
	const RuntimeMapMutationCache::ChunkReplacement& replacement,
	bool hasResidentChunk,
	uint32_t resolvedChunkListIndex) const
{
	RuntimeMutationResidentApplyMode mode = {};
	mode.materialOnlyReplacement = IsMaterialOnlyChunkReplacement(replacement.reasonMask);
	mode.exclusiveMaterialOnlyReplacement =
		mode.materialOnlyReplacement &&
		RequiresExclusiveMaterialOnlyChunkReplacement(replacement.reasonMask);
	mode.fastResidentMaterialOnlyUpdate =
		mode.materialOnlyReplacement &&
		!mode.exclusiveMaterialOnlyReplacement &&
		hasResidentChunk &&
		replacement.valid &&
		resolvedChunkListIndex < mStaticMapChunkAtlas.chunks.size() &&
		(uint32_t)replacement.materialBridge.materials.size() ==
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].materialCount;
	return mode;
}

void NRIRenderer::RecordRuntimeMutationResidentApplyMode(
	const RuntimeMutationResidentApplyMode& mode,
	const RuntimeMapMutationCache::ChunkReplacement& replacement,
	bool hasResidentChunk,
	uint32_t resolvedChunkListIndex)
{
	if (mode.materialOnlyReplacement)
	{
		mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyCount++;
		if (mode.fastResidentMaterialOnlyUpdate)
		{
			mLastPerfShellTraceStats.runtimeMutationResidentApplyFastMaterialOnlyCount++;
		}
		else
		{
			mLastPerfShellTraceStats.runtimeMutationResidentApplySlowMaterialOnlyCount++;
			if (mode.exclusiveMaterialOnlyReplacement)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyExclusiveCount++;
			}
			if (!hasResidentChunk)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyNoResidentChunkCount++;
			}
			if (!replacement.valid)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyInvalidReplacementCount++;
			}
			if (resolvedChunkListIndex < mStaticMapChunkAtlas.chunks.size() &&
				(uint32_t)replacement.materialBridge.materials.size() !=
					mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].materialCount)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyMaterialCountMismatchCount++;
			}
		}
	}
	else
	{
		mLastPerfShellTraceStats.runtimeMutationResidentApplyStructuralCount++;
	}
}

bool NRIRenderer::TryApplyRuntimeMutationChunkToResidentScene(
	const nri_scene::PTMapChunk& mapChunk,
	RuntimeMapMutationCache::ChunkReplacement& replacement,
	uint32_t& outStaticSceneChunkListIndex,
	bool& outMaterialDirty,
	bool& outGeometryDirty,
	uint32_t& outSurfaceCount,
	uint32_t& outTriangleCount,
	uint32_t& outMaterialCount,
	bool& outRecoveredEmpty)
{
	outStaticSceneChunkListIndex = UINT32_MAX;
	outMaterialDirty = false;
	outGeometryDirty = false;
	outSurfaceCount = 0;
	outTriangleCount = 0;
	outMaterialCount = 0;
	outRecoveredEmpty = false;
	ScopedPtPerfTimer residentApplyPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyMs);
	mLastPerfShellTraceStats.runtimeMutationResidentApplyCount++;
	if (!mStaticMapScene.valid ||
		!mStaticMapScene.buffersResident ||
		!mStaticMapScene.accelerationResident ||
		!mStaticMapChunkAtlas.valid ||
		mapChunk.chunkIndex >= mResidentMapChunkRegistry.entries.size())
	{
		return false;
	}

	auto& entry = mResidentMapChunkRegistry.entries[mapChunk.chunkIndex];
	if (!entry.valid)
	{
		return false;
	}
	const uint32_t resolvedChunkListIndex =
		entry.staticSceneChunkListIndex < mStaticMapScene.chunks.size() &&
		mStaticMapScene.chunks[entry.staticSceneChunkListIndex].chunkIndex == mapChunk.chunkIndex ?
		entry.staticSceneChunkListIndex :
		FindPreferredStaticSceneChunkListIndex(mapChunk.chunkIndex);
	const bool hasResidentChunk =
		entry.active &&
		entry.mappedInStaticScene &&
		resolvedChunkListIndex < mStaticMapScene.chunks.size() &&
		resolvedChunkListIndex < mStaticMapChunkAtlas.chunks.size() &&
		mStaticMapScene.chunks[resolvedChunkListIndex].active;
	const bool hasChunkSlot =
		resolvedChunkListIndex < mStaticMapScene.chunks.size() &&
		resolvedChunkListIndex < mStaticMapChunkAtlas.chunks.size();
	if (hasChunkSlot)
	{
		entry.staticSceneChunkListIndex = resolvedChunkListIndex;
	}

	const RuntimeMutationResidentApplyMode applyMode =
		ClassifyRuntimeMutationResidentApplyMode(replacement, hasResidentChunk, resolvedChunkListIndex);
	RecordRuntimeMutationResidentApplyMode(applyMode, replacement, hasResidentChunk, resolvedChunkListIndex);
	const bool materialOnlyReplacement = applyMode.materialOnlyReplacement;
	const bool exclusiveMaterialOnlyReplacement = applyMode.exclusiveMaterialOnlyReplacement;
	const bool fastResidentMaterialOnlyUpdate = applyMode.fastResidentMaterialOnlyUpdate;

	nri_scene::SceneView residentSceneView;
	nri_scene::GeometryData residentGeometry;
	nri_scene::MaterialBridgeData residentMaterials;
	nri_scene::PTMapChunkMutationBaseline appliedBaseline;

	if (fastResidentMaterialOnlyUpdate)
	{
		const bool mergedSectorMaterialOnlyFastPath =
			IsPureSectorMaterialOnlyChunkReplacement(replacement.reasonMask) &&
			hasResidentChunk &&
			resolvedChunkListIndex < mStaticMapScene.lightChunkViews.size() &&
			TryBuildMergedSectorMaterialOnlySceneView(
				mStaticMapScene.lightChunkViews[resolvedChunkListIndex],
				replacement.sceneView,
				residentSceneView);
		if (!mergedSectorMaterialOnlyFastPath)
		{
			residentSceneView = replacement.sceneView;
		}
		residentMaterials = replacement.materialBridge;
		appliedBaseline = replacement.replacementBaseline;
	}
	else if (materialOnlyReplacement && !exclusiveMaterialOnlyReplacement)
	{
		nri_scene::PTMapWorld liveWorld = {};
		nri_scene::PTMapWorldStats ignoredStats = {};
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyLiveBuildMs);
			if (!nri_scene::BuildLiveMapChunkWorld(mapChunk, liveWorld, &ignoredStats))
			{
				return false;
			}

			nri_scene::BuildMapChunkSceneView(liveWorld, liveWorld.chunks[0], residentSceneView);
			const bool blindSpotReplacementNudged = replacement.blindSpot && replacement.dragged;
			if (blindSpotReplacementNudged)
			{
				NudgeBlindSpotReplacementFlats(residentSceneView);
			}
			else if (nri_ptceilingnudge)
			{
				NudgeMapCeilingSections(residentSceneView, (float)nri_ptceilingnudgedistance);
			}
		}
		{
			Clocker clock(NriPTGeometryBuild);
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryBuildMs);
			ScopedPtPerfTimer siteTimer(mLastPerfShellTraceStats.geometryBuildResidentApplyMs);
			nri_scene::BuildGeometry(residentSceneView, residentGeometry);
			AssignGeometryPortalIndices(mMapWorld, residentGeometry);
		}
		mLastPerfShellTraceStats.geometryBuildResidentApplyCalls++;
		mLastPerfShellTraceStats.geometryBuildResidentPrimitives += (uint32_t)residentGeometry.primitives.size();
		{
			Clocker clock(NriPTMaterialBuild);
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialBuildMs);
			BuildMaterialsWithActorOverrides(residentSceneView, residentMaterials, "resident_runtime_mutation_chunk");
		}
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyBaselineCaptureMs);
			if (!nri_scene::CaptureMapChunkMutationBaseline(mapChunk, appliedBaseline))
			{
				return false;
			}
		}
	}
	else
	{
		residentSceneView = replacement.sceneView;
		residentGeometry = replacement.geometry;
		residentMaterials = replacement.materialBridge;
		appliedBaseline = replacement.replacementBaseline;
	}
	outSurfaceCount = CountSceneViewSurfaces(residentSceneView);
	outTriangleCount = (uint32_t)residentGeometry.primitives.size();
	outMaterialCount = (uint32_t)residentMaterials.materials.size();

	const uint32_t appliedReasonMask = replacement.reasonMask;
	const bool appliedStaticAnimatedReplacement = replacement.staticAnimatedReplacement;
	const bool appliedAnimationOnlyRefreshed = replacement.animationOnlyRefreshed;
	uint64_t residentGeometryTopologySignature = ComputeGeometryTopologySignature(residentGeometry);
	uint64_t residentPrimitiveLayoutSignature = ComputePrimitiveLayoutSignature(residentGeometry);
	const uint32_t materialReasonMask =
		nri_scene::PTMapChunkMutationReason_SectorMaterial |
		nri_scene::PTMapChunkMutationReason_WallMaterial;
	const bool appliedForceTopology =
		(appliedReasonMask & (
			nri_scene::PTMapChunkMutationReason_SectorGeometry |
			nri_scene::PTMapChunkMutationReason_WallGeometry |
			nri_scene::PTMapChunkMutationReason_SectorDirty |
			nri_scene::PTMapChunkMutationReason_SectionDirty |
			nri_scene::PTMapChunkMutationReason_Dragged)) != 0;
	const bool hasResidentGeometry =
		hasResidentChunk &&
		mStaticMapScene.chunks[resolvedChunkListIndex].active &&
		mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].valid;
	const uint32_t chunkListIndex = hasChunkSlot ?
		resolvedChunkListIndex :
		(uint32_t)mStaticMapScene.chunks.size();
	uint32_t residentVertexCount = 0;
	uint32_t residentIndexCount = 0;
	uint32_t residentPrimitiveCount = 0;
	uint32_t residentMaterialCount = 0;
	bool preserveResidentGeometryForMaterialOnlyUpdate = false;
	uint32_t effectiveResidentVertexCount = 0;
	uint32_t effectiveResidentIndexCount = 0;
	uint32_t effectiveResidentPrimitiveCount = 0;
	bool chunkBecomesEmpty = false;
	bool recoveredFullLiveResidentChunk = false;
	const auto recomputeResidentCounts = [&]()
	{
		residentVertexCount =
			fastResidentMaterialOnlyUpdate && !recoveredFullLiveResidentChunk && hasResidentChunk ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].vertexCount :
			(uint32_t)residentGeometry.vertices.size();
		residentIndexCount =
			fastResidentMaterialOnlyUpdate && !recoveredFullLiveResidentChunk && hasResidentChunk ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].indexCount :
			(uint32_t)residentGeometry.indices.size();
		residentPrimitiveCount =
			fastResidentMaterialOnlyUpdate && !recoveredFullLiveResidentChunk && hasResidentChunk ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].primitiveCount :
			(uint32_t)residentGeometry.primitives.size();
		residentMaterialCount = (uint32_t)residentMaterials.materials.size();
		preserveResidentGeometryForMaterialOnlyUpdate =
			materialOnlyReplacement &&
			!exclusiveMaterialOnlyReplacement &&
			hasResidentChunk &&
			residentPrimitiveCount == 0 &&
			residentMaterialCount != 0 &&
			resolvedChunkListIndex < mStaticMapChunkAtlas.chunks.size() &&
			residentMaterialCount == mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].materialCount;
		effectiveResidentVertexCount =
			preserveResidentGeometryForMaterialOnlyUpdate ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].vertexCount :
			residentVertexCount;
		effectiveResidentIndexCount =
			preserveResidentGeometryForMaterialOnlyUpdate ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].indexCount :
			residentIndexCount;
		effectiveResidentPrimitiveCount =
			preserveResidentGeometryForMaterialOnlyUpdate ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].primitiveCount :
			residentPrimitiveCount;
		chunkBecomesEmpty = effectiveResidentPrimitiveCount == 0 || residentMaterialCount == 0;
	};
	recomputeResidentCounts();
	if (preserveResidentGeometryForMaterialOnlyUpdate)
	{
		mLastPerfShellTraceStats.runtimeMutationResidentApplyPreserveGeometryCount++;
	}
	const uint32_t residentChunkRemovalReasonMask =
		nri_scene::PTMapChunkMutationReason_SectorGeometry |
		nri_scene::PTMapChunkMutationReason_WallGeometry |
		nri_scene::PTMapChunkMutationReason_SectorDirty |
		nri_scene::PTMapChunkMutationReason_SectionDirty |
		nri_scene::PTMapChunkMutationReason_Dragged;
	const auto rebuildFullLiveResidentChunk = [&]() -> bool
	{
		nri_scene::PTMapWorld liveWorld = {};
		nri_scene::PTMapWorldStats ignoredStats = {};
		nri_scene::SceneView fullLiveSceneView;
		nri_scene::GeometryData fullLiveGeometry;
		nri_scene::MaterialBridgeData fullLiveMaterials;
		nri_scene::PTMapChunkMutationBaseline fullLiveBaseline;
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyLiveBuildMs);
			if (!nri_scene::BuildLiveMapChunkWorld(mapChunk, liveWorld, &ignoredStats))
			{
				return false;
			}

			nri_scene::BuildMapChunkSceneView(liveWorld, liveWorld.chunks[0], fullLiveSceneView);
			if (nri_ptceilingnudge)
			{
				NudgeMapCeilingSections(fullLiveSceneView, (float)nri_ptceilingnudgedistance);
			}
		}
		{
			Clocker clock(NriPTGeometryBuild);
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryBuildMs);
			ScopedPtPerfTimer siteTimer(mLastPerfShellTraceStats.geometryBuildResidentRecoverMs);
			nri_scene::BuildGeometry(fullLiveSceneView, fullLiveGeometry);
			AssignGeometryPortalIndices(mMapWorld, fullLiveGeometry);
		}
		mLastPerfShellTraceStats.geometryBuildResidentRecoverCalls++;
		mLastPerfShellTraceStats.geometryBuildResidentPrimitives += (uint32_t)fullLiveGeometry.primitives.size();
		{
			Clocker clock(NriPTMaterialBuild);
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialBuildMs);
			BuildMaterialsWithActorOverrides(fullLiveSceneView, fullLiveMaterials, "resident_runtime_mutation_chunk_recover");
		}
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyBaselineCaptureMs);
			if (!nri_scene::CaptureMapChunkMutationBaseline(mapChunk, fullLiveBaseline))
			{
				return false;
			}
		}

		if (fullLiveGeometry.primitives.empty() || fullLiveMaterials.materials.empty())
		{
			return false;
		}

		if (ShouldTracePtPerf())
		{
			Printf("NRI PT resident chunk trace: event=recover-empty chunk=%u reason_mask=0x%x recovered_prims=%u recovered_mats=%u\n",
				mapChunk.chunkIndex,
				appliedReasonMask,
				(uint32_t)fullLiveGeometry.primitives.size(),
				(uint32_t)fullLiveMaterials.materials.size());
		}

		residentSceneView = std::move(fullLiveSceneView);
		residentGeometry = std::move(fullLiveGeometry);
		residentMaterials = std::move(fullLiveMaterials);
		appliedBaseline = std::move(fullLiveBaseline);
		recoveredFullLiveResidentChunk = true;
		outRecoveredEmpty = true;
		mLastPerfShellTraceStats.runtimeMutationResidentApplyRecoverSuccessCount++;
		return true;
	};
	if (chunkBecomesEmpty)
	{
		mLastPerfShellTraceStats.runtimeMutationResidentApplyRecoverAttemptCount++;
		if (rebuildFullLiveResidentChunk())
		{
			residentGeometryTopologySignature = ComputeGeometryTopologySignature(residentGeometry);
			residentPrimitiveLayoutSignature = ComputePrimitiveLayoutSignature(residentGeometry);
			recomputeResidentCounts();
			if (preserveResidentGeometryForMaterialOnlyUpdate)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyPreserveGeometryCount++;
			}
		}
	}
	const bool suspiciousNonStructuralChunkRemoval =
		chunkBecomesEmpty &&
		hasResidentChunk &&
		(appliedReasonMask & residentChunkRemovalReasonMask) == 0;
	if (suspiciousNonStructuralChunkRemoval)
	{
		if (ShouldTracePtPerf())
		{
			Printf("NRI PT resident chunk trace: event=reject-empty-nonstructural chunk=%u reason_mask=0x%x resident_prims=%u resident_mats=%u\n",
				mapChunk.chunkIndex,
				appliedReasonMask,
				residentPrimitiveCount,
				residentMaterialCount);
		}
		return false;
	}
	const uint64_t residentMaterialPayloadHash =
		!chunkBecomesEmpty ? HashResidentMaterialPayload(residentMaterials) : 0;
	uint64_t residentGeometryPayloadHash = 0;
	bool residentMaterialPayloadHashSkip = false;
	bool residentGeometryPayloadHashSkip = false;
	bool appliedPreservedResidentMaterialSlice = false;
	bool appliedPreservedResidentGeometryPayload = false;

	if (chunkBecomesEmpty)
	{
		mLastPerfShellTraceStats.runtimeMutationResidentApplyEmptyRemovalCount++;
		if (hasResidentChunk)
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyAtlasMs);
			auto& mutableChunk = mStaticMapScene.chunks[chunkListIndex];
			const auto& oldAtlasChunk = mStaticMapChunkAtlas.chunks[chunkListIndex];
			const uint32_t oldMaterialOffset = oldAtlasChunk.materialOffset;
			const uint32_t oldMaterialCount = oldAtlasChunk.materialCount;
			RetireResidentAccelerationStructure(mutableChunk.accelerationStructure);
			ReleaseChunkAtlasRange(mStaticMapChunkAtlas.freeVertexRanges, oldAtlasChunk.vertexOffset, oldAtlasChunk.vertexCount);
			ReleaseChunkAtlasRange(mStaticMapChunkAtlas.freeIndexRanges, oldAtlasChunk.indexOffset, oldAtlasChunk.indexCount);
			ReleaseChunkAtlasRange(mStaticMapChunkAtlas.freePrimitiveRanges, oldAtlasChunk.primitiveOffset, oldAtlasChunk.primitiveCount);
			ReleaseChunkAtlasRange(mStaticMapChunkAtlas.freeMaterialRanges, oldAtlasChunk.materialOffset, oldAtlasChunk.materialCount);
			mStaticMapChunkAtlas.chunks[chunkListIndex] = {};
			mStaticMapChunkAtlas.chunks[chunkListIndex].chunkIndex = mapChunk.chunkIndex;
			mStaticMapChunkAtlas.chunks[chunkListIndex].staticSceneChunkListIndex = chunkListIndex;
			mutableChunk.active = false;
			mutableChunk.blasUpdateEligible = false;
			mutableChunk.lastResidentBlasReasonMask = 0;
			mutableChunk.lastResidentBlasSurfaceCount = 0;
			mutableChunk.lastResidentBlasTriangleCount = 0;
			mutableChunk.lastResidentBlasMaterialCount = 0;
			mutableChunk.lastResidentBlasForceTopology = false;
			mutableChunk.lastResidentBlasRecoveredEmpty = false;
			mutableChunk.lastResidentBlasKeptGeometrySlice = false;
			mutableChunk.lastResidentBlasTopologyChanged = false;
			mutableChunk.residentBlasScratchSizeCacheKey = nullptr;
			mutableChunk.residentBlasBuildScratchSize = 0;
			mutableChunk.residentBlasUpdateScratchSize = 0;
			mutableChunk.vertexCount = 0;
			mutableChunk.indexCount = 0;
			mutableChunk.primitiveCount = 0;
			mutableChunk.materialCount = 0;
			mutableChunk.materialBridge = {};
			mutableChunk.geometryTopologySignature = 0;
			mutableChunk.primitiveLayoutSignature = 0;
			mutableChunk.exactGeometrySignature = 0;
			mutableChunk.geometryPayloadHash = 0;
			mutableChunk.animatedMaterialSignature = 0;
			mutableChunk.animatedGeometrySignature = 0;
			mutableChunk.hasAnimatedTextureCandidates = false;
			mutableChunk.animatedRefreshSuppressed = false;
			if (chunkListIndex < mStaticMapScene.lightChunkViews.size())
			{
				mStaticMapScene.lightChunkViews[chunkListIndex] = {};
			}
			if (oldMaterialOffset + oldMaterialCount <= mStaticMapScene.materialBridge.materials.size())
			{
				std::fill_n(
					mStaticMapScene.materialBridge.materials.begin() + oldMaterialOffset,
					oldMaterialCount,
					nri_scene::MaterialData{});
			}
			if (oldMaterialOffset + oldMaterialCount <= mStaticMapScene.materialBridge.lightMetadata.size())
			{
				std::fill_n(
					mStaticMapScene.materialBridge.lightMetadata.begin() + oldMaterialOffset,
					oldMaterialCount,
					nri_scene::MaterialLightingMetadata{});
			}
			if (oldMaterialOffset + oldMaterialCount <= mStaticMapScene.gpuMaterials.size())
			{
				std::fill_n(
					mStaticMapScene.gpuMaterials.begin() + oldMaterialOffset,
					oldMaterialCount,
					nri_scene::MaterialData{});
			}
			if (oldMaterialCount > 0)
			{
				std::vector<nri_scene::MaterialData> clearedMaterials(oldMaterialCount);
				if (!StageResidentBufferCopyRange(
						mStaticMaterialBuffer,
						(uint64_t)oldMaterialOffset * sizeof(nri_scene::MaterialData),
						clearedMaterials.data(),
						(uint64_t)oldMaterialCount * sizeof(nri_scene::MaterialData),
						NRIComputeShaderResourceAccess(),
						ResidentUploadKind_Material))
				{
					return false;
				}
				mLastPerfResourceTraceStats.residentChunkBatchMaterialBytes +=
					(uint64_t)oldMaterialCount * sizeof(nri_scene::MaterialData);
			}
			outGeometryDirty = true;
			outMaterialDirty = true;
			outStaticSceneChunkListIndex = chunkListIndex;
		}
	}
	else
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyAtlasMs);
		StaticMapSceneCache::ChunkCache sourceChunk = {};
		StaticMapChunkAtlas nextAtlasState = {};
		StaticMapChunkAtlas::ChunkEntry nextAtlasChunk = {};
		bool keptGeometrySlices = false;
		bool keptMaterialSlice = false;
		const uint64_t previousAnimatedMaterialSignature =
			hasResidentChunk ?
			mStaticMapScene.chunks[chunkListIndex].animatedMaterialSignature :
			0;
		const bool previousAnimatedRefreshSuppressed =
			hasResidentChunk &&
			mStaticMapScene.chunks[chunkListIndex].animatedRefreshSuppressed;
		const uint64_t previousAnimatedGeometrySignature =
			hasResidentChunk ?
				mStaticMapScene.chunks[chunkListIndex].animatedGeometrySignature :
				0;
		const uint64_t previousGeometryTopologySignature =
			hasResidentChunk ?
				mStaticMapScene.chunks[chunkListIndex].geometryTopologySignature :
				0;
		const uint64_t previousPrimitiveLayoutSignature =
			hasResidentChunk ?
				mStaticMapScene.chunks[chunkListIndex].primitiveLayoutSignature :
				0;
		bool preserveResidentMaterialSlice = false;
		bool preserveResidentIndexSlice = false;
		bool preserveResidentPrimitiveSlice = false;
		bool preserveResidentBlasRefitOnly = false;
		{
			ScopedPtPerfTimer detailPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyAtlasBookkeepingMs);
			sourceChunk.vertexOffset = 0;
			sourceChunk.vertexCount = effectiveResidentVertexCount;
			sourceChunk.indexOffset = 0;
			sourceChunk.indexCount = effectiveResidentIndexCount;
			sourceChunk.primitiveOffset = 0;
			sourceChunk.primitiveCount = effectiveResidentPrimitiveCount;
			sourceChunk.materialOffset = 0;
			sourceChunk.materialCount = (uint32_t)residentMaterials.materials.size();

			for (;;)
			{
				nextAtlasState = mStaticMapChunkAtlas;
				if (chunkListIndex >= nextAtlasState.chunks.size())
				{
					nextAtlasState.chunks.resize(chunkListIndex + 1);
				}
				nextAtlasState.chunkCount = (uint32_t)nextAtlasState.chunks.size();
				const auto oldAtlasChunk = hasResidentChunk ? nextAtlasState.chunks[chunkListIndex] : StaticMapChunkAtlas::ChunkEntry{};
				const bool keepGeometrySlices =
					hasResidentGeometry &&
					oldAtlasChunk.vertexCount == effectiveResidentVertexCount &&
					oldAtlasChunk.indexCount == effectiveResidentIndexCount &&
					oldAtlasChunk.primitiveCount == effectiveResidentPrimitiveCount;
				const bool keepMaterialSlice =
					hasResidentGeometry &&
					oldAtlasChunk.materialCount == residentMaterialCount;

				if (hasResidentGeometry && !keepGeometrySlices)
				{
					ReleaseChunkAtlasRange(nextAtlasState.freeVertexRanges, oldAtlasChunk.vertexOffset, oldAtlasChunk.vertexCount);
					ReleaseChunkAtlasRange(nextAtlasState.freeIndexRanges, oldAtlasChunk.indexOffset, oldAtlasChunk.indexCount);
					ReleaseChunkAtlasRange(nextAtlasState.freePrimitiveRanges, oldAtlasChunk.primitiveOffset, oldAtlasChunk.primitiveCount);
				}
				if (hasResidentGeometry && !keepMaterialSlice)
				{
					ReleaseChunkAtlasRange(nextAtlasState.freeMaterialRanges, oldAtlasChunk.materialOffset, oldAtlasChunk.materialCount);
				}

				nextAtlasChunk = {};
				nextAtlasChunk.valid = true;
				nextAtlasChunk.chunkIndex = mapChunk.chunkIndex;
				nextAtlasChunk.staticSceneChunkListIndex = chunkListIndex;
				nextAtlasChunk.vertexCount = effectiveResidentVertexCount;
				nextAtlasChunk.indexCount = effectiveResidentIndexCount;
				nextAtlasChunk.primitiveCount = effectiveResidentPrimitiveCount;
				nextAtlasChunk.materialCount = residentMaterialCount;
				nextAtlasChunk.vertexOffset = keepGeometrySlices ?
					oldAtlasChunk.vertexOffset :
					AllocateChunkAtlasRange(effectiveResidentVertexCount, nextAtlasState.vertexCapacity, nextAtlasState.freeVertexRanges, nextAtlasState.vertexCount);
				nextAtlasChunk.indexOffset = keepGeometrySlices ?
					oldAtlasChunk.indexOffset :
					AllocateChunkAtlasRange(effectiveResidentIndexCount, nextAtlasState.indexCapacity, nextAtlasState.freeIndexRanges, nextAtlasState.indexCount);
				nextAtlasChunk.primitiveOffset = keepGeometrySlices ?
					oldAtlasChunk.primitiveOffset :
					AllocateChunkAtlasRange(effectiveResidentPrimitiveCount, nextAtlasState.primitiveCapacity, nextAtlasState.freePrimitiveRanges, nextAtlasState.primitiveCount);
				nextAtlasChunk.materialOffset = keepMaterialSlice ?
					oldAtlasChunk.materialOffset :
					AllocateChunkAtlasRange(residentMaterialCount, nextAtlasState.materialCapacity, nextAtlasState.freeMaterialRanges, nextAtlasState.materialCount);
				if (nextAtlasChunk.vertexOffset != UINT32_MAX &&
					nextAtlasChunk.indexOffset != UINT32_MAX &&
					nextAtlasChunk.primitiveOffset != UINT32_MAX &&
					nextAtlasChunk.materialOffset != UINT32_MAX)
				{
					keptGeometrySlices = keepGeometrySlices;
					keptMaterialSlice = keepMaterialSlice;
					break;
				}

				if (nextAtlasChunk.vertexOffset == UINT32_MAX)
				{
					nextAtlasState.vertexCapacity = GetChunkAtlasCapacity(nextAtlasState.vertexCount + effectiveResidentVertexCount);
				}
				if (nextAtlasChunk.indexOffset == UINT32_MAX)
				{
					nextAtlasState.indexCapacity = GetChunkAtlasCapacity(nextAtlasState.indexCount + effectiveResidentIndexCount);
				}
				if (nextAtlasChunk.primitiveOffset == UINT32_MAX)
				{
					nextAtlasState.primitiveCapacity = GetChunkAtlasCapacity(nextAtlasState.primitiveCount + effectiveResidentPrimitiveCount);
				}
				if (nextAtlasChunk.materialOffset == UINT32_MAX)
				{
					nextAtlasState.materialCapacity = GetChunkAtlasCapacity(nextAtlasState.materialCount + residentMaterialCount);
				}
				mLastPerfShellTraceStats.runtimeMutationResidentApplyAtlasGrowCount++;
				if (!EnsureResidentStaticMapChunkAtlasBufferCapacity(nextAtlasState))
				{
					return false;
				}
			}
			if (keptGeometrySlices)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyKeepGeometrySliceCount++;
			}
			if (keptMaterialSlice)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyKeepMaterialSliceCount++;
			}
			if (hasResidentChunk && residentMaterialCount != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashCheckCount++;
				if (keptMaterialSlice && entry.materialPayloadHash != 0)
				{
					if (entry.materialPayloadHash == residentMaterialPayloadHash)
					{
						residentMaterialPayloadHashSkip = true;
						mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashSkipCount++;
					}
					else
					{
						mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashMissCount++;
					}
				}
				else
				{
					mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashRejectCount++;
				}
			}
			if (!fastResidentMaterialOnlyUpdate &&
				!preserveResidentGeometryForMaterialOnlyUpdate &&
				hasResidentChunk &&
				effectiveResidentVertexCount != 0 &&
				effectiveResidentIndexCount != 0 &&
				effectiveResidentPrimitiveCount != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashCheckCount++;
				if (keptGeometrySlices && keptMaterialSlice && entry.geometryPayloadHash != 0)
				{
					residentGeometryPayloadHash = HashResidentGeometryPayload(
						mMapWorld,
						residentGeometry,
						sourceChunk.vertexOffset,
						sourceChunk.vertexCount,
						sourceChunk.indexOffset,
						sourceChunk.indexCount,
						sourceChunk.primitiveOffset,
						sourceChunk.primitiveCount,
						sourceChunk.materialOffset,
						sourceChunk.materialCount);
					if (residentGeometryPayloadHash != 0 &&
						entry.geometryPayloadHash == residentGeometryPayloadHash)
					{
						residentGeometryPayloadHashSkip = true;
						mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashSkipCount++;
						mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashBlasSkipCount++;
					}
					else
					{
						mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashMissCount++;
						if (ShouldTraceResidentGeometryOrderHash())
						{
							ScopedPtPerfTimer orderHashTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryOrderHashMs);
							mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderCheckCount++;
							const uint64_t residentGeometryOrderHash = HashResidentGeometryPayloadOrderIndependent(
								mMapWorld,
								residentGeometry,
								sourceChunk.vertexOffset,
								sourceChunk.vertexCount,
								sourceChunk.primitiveOffset,
								sourceChunk.primitiveCount,
								sourceChunk.materialOffset,
								sourceChunk.materialCount);
							const uint64_t currentGeometryOrderHash = HashResidentGeometryPayloadOrderIndependent(
								mMapWorld,
								mStaticMapScene.geometry,
								nextAtlasChunk.vertexOffset,
								nextAtlasChunk.vertexCount,
								nextAtlasChunk.primitiveOffset,
								nextAtlasChunk.primitiveCount,
								nextAtlasChunk.materialOffset,
								nextAtlasChunk.materialCount);
							if (residentGeometryOrderHash != 0 && currentGeometryOrderHash != 0)
							{
								if (residentGeometryOrderHash == currentGeometryOrderHash)
								{
									mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderEquivalentCount++;
								}
								else
								{
									mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderMissCount++;
								}
							}
							else
							{
								mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderRejectCount++;
							}
						}
					}
				}
				else
				{
					mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashRejectCount++;
				}
			}

			if (chunkListIndex >= mStaticMapScene.chunks.size())
			{
				mStaticMapScene.chunks.resize(chunkListIndex + 1);
			}
			if (chunkListIndex >= mStaticMapScene.lightChunkViews.size())
			{
				mStaticMapScene.lightChunkViews.resize(chunkListIndex + 1);
			}
			preserveResidentMaterialSlice =
				!materialOnlyReplacement &&
				(appliedReasonMask & materialReasonMask) == 0 &&
				!appliedStaticAnimatedReplacement &&
				!appliedAnimationOnlyRefreshed &&
				hasResidentChunk &&
				keptMaterialSlice &&
				previousAnimatedMaterialSignature == ComputeAnimatedMaterialSignature(residentSceneView);
			preserveResidentMaterialSlice = preserveResidentMaterialSlice || residentMaterialPayloadHashSkip;
			preserveResidentIndexSlice =
				!materialOnlyReplacement &&
				hasResidentChunk &&
				keptGeometrySlices &&
				previousGeometryTopologySignature == residentGeometryTopologySignature;
			preserveResidentPrimitiveSlice =
				!materialOnlyReplacement &&
				hasResidentChunk &&
				preserveResidentIndexSlice &&
				keptMaterialSlice &&
				previousPrimitiveLayoutSignature == residentPrimitiveLayoutSignature;
			const bool probeResidentBlasRefitOnly =
				!preserveResidentIndexSlice &&
				!materialOnlyReplacement &&
				hasResidentChunk;
			if (probeResidentBlasRefitOnly)
			{
				uint32_t rejectMask = 0;
				const bool hadAccelerationStructure =
					mStaticMapScene.chunks[chunkListIndex].accelerationStructure.accelerationStructure != nullptr;
				mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitProbeCount++;
				if (!hadAccelerationStructure)
				{
					rejectMask |= RuntimeResidentBlasRefitReject_NoPreviousAs;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectNoPreviousAsCount++;
				}
				if (entry.indexCount != effectiveResidentIndexCount)
				{
					rejectMask |= RuntimeResidentBlasRefitReject_IndexCountMismatch;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectIndexCountMismatchCount++;
				}
				if (entry.primitiveCount != effectiveResidentPrimitiveCount)
				{
					rejectMask |= RuntimeResidentBlasRefitReject_PrimitiveCountMismatch;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectPrimitiveCountMismatchCount++;
				}
				if (entry.indexCount == 0 || effectiveResidentIndexCount == 0)
				{
					rejectMask |= RuntimeResidentBlasRefitReject_ZeroIndexCount;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectZeroIndexCount++;
				}
				if (entry.primitiveCount == 0 || effectiveResidentPrimitiveCount == 0)
				{
					rejectMask |= RuntimeResidentBlasRefitReject_ZeroPrimitiveCount;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectZeroPrimitiveCount++;
				}
				preserveResidentBlasRefitOnly = rejectMask == 0;
				if (rejectMask != 0 && ShouldTracePtPerf())
				{
					RuntimeResidentBlasRefitRejectTraceEntry traceEntry = {};
					traceEntry.valid = true;
					traceEntry.chunkIndex = mapChunk.chunkIndex;
					traceEntry.sectorIndex = mapChunk.sectorIndex;
					traceEntry.reasonMask = appliedReasonMask;
					traceEntry.rejectMask = rejectMask;
					traceEntry.previousIndexCount = entry.indexCount;
					traceEntry.liveIndexCount = effectiveResidentIndexCount;
					traceEntry.previousPrimitiveCount = entry.primitiveCount;
					traceEntry.livePrimitiveCount = effectiveResidentPrimitiveCount;
					traceEntry.hadAccelerationStructure = hadAccelerationStructure;
					InsertRankedTraceEntry(
						mLastPerfShellTraceStats.runtimeResidentBlasRefitRejectEntries,
						traceEntry,
						ScoreRuntimeResidentBlasRefitRejectTraceEntry);
				}
			}
			if (preserveResidentIndexSlice)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyPreserveIndexCount++;
			}
			if (preserveResidentPrimitiveSlice)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyPreservePrimitiveCount++;
			}
			if (preserveResidentBlasRefitOnly)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitOnlyCount++;
			}

			mStaticMapChunkAtlas = std::move(nextAtlasState);
			mStaticMapChunkAtlas.chunks[chunkListIndex] = nextAtlasChunk;
			mStaticMapScene.geometry.vertices.resize(std::max<size_t>(mStaticMapScene.geometry.vertices.size(), mStaticMapChunkAtlas.vertexCount));
			mStaticMapScene.geometry.indices.resize(std::max<size_t>(mStaticMapScene.geometry.indices.size(), mStaticMapChunkAtlas.indexCount));
			mStaticMapScene.geometry.primitives.resize(std::max<size_t>(mStaticMapScene.geometry.primitives.size(), mStaticMapChunkAtlas.primitiveCount));
			mStaticMapScene.geometry.primitiveProvenance.resize(std::max<size_t>(mStaticMapScene.geometry.primitiveProvenance.size(), mStaticMapChunkAtlas.primitiveCount));
			mStaticMapScene.materialBridge.materials.resize(std::max<size_t>(mStaticMapScene.materialBridge.materials.size(), mStaticMapChunkAtlas.materialCount));
			mStaticMapScene.materialBridge.lightMetadata.resize(std::max<size_t>(mStaticMapScene.materialBridge.lightMetadata.size(), mStaticMapChunkAtlas.materialCount));
			mStaticMapScene.gpuMaterials.resize(std::max<size_t>(mStaticMapScene.gpuMaterials.size(), mStaticMapChunkAtlas.materialCount));
		}

		if (!fastResidentMaterialOnlyUpdate &&
			!preserveResidentGeometryForMaterialOnlyUpdate &&
			!residentGeometryPayloadHashSkip)
		{
			{
				ScopedPtPerfTimer detailPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexIndexCopyMs);
				if (preserveResidentIndexSlice)
				{
					{
						ScopedPtPerfTimer vertexCpuTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexCpuCopyMs);
						UploadChunkVertexDataToAtlas(
							residentGeometry,
							sourceChunk,
							nextAtlasChunk,
							mStaticMapScene.geometry.vertices);
					}
					const uint64_t vertexBytes = (uint64_t)nextAtlasChunk.vertexCount * sizeof(nri_scene::SceneVertex);
					if (!mRuntimeMutation.QueueResidentGeometryUploadRange(
							ResidentUploadKind_Vertex,
							(uint64_t)nextAtlasChunk.vertexOffset * sizeof(nri_scene::SceneVertex),
							vertexBytes,
							BuildRuntimeMutationResidentUploadServices()))
					{
						return false;
					}
				}
				else
				{
					{
						ScopedPtPerfTimer vertexCpuTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexCpuCopyMs);
						UploadChunkVertexDataToAtlas(
							residentGeometry,
							sourceChunk,
							nextAtlasChunk,
							mStaticMapScene.geometry.vertices);
					}
					{
						ScopedPtPerfTimer indexCpuTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyIndexCpuCopyMs);
						UploadChunkIndexDataToAtlas(
							residentGeometry,
							sourceChunk,
							nextAtlasChunk,
							mStaticMapScene.geometry.indices);
					}

					const uint64_t vertexBytes = (uint64_t)nextAtlasChunk.vertexCount * sizeof(nri_scene::SceneVertex);
					const uint64_t indexBytes = (uint64_t)nextAtlasChunk.indexCount * sizeof(uint32_t);
					if (!mRuntimeMutation.QueueResidentGeometryUploadRange(
							ResidentUploadKind_Vertex,
							(uint64_t)nextAtlasChunk.vertexOffset * sizeof(nri_scene::SceneVertex),
							vertexBytes,
							BuildRuntimeMutationResidentUploadServices()))
					{
						return false;
					}

					if (!mRuntimeMutation.QueueResidentGeometryUploadRange(
							ResidentUploadKind_Index,
							(uint64_t)nextAtlasChunk.indexOffset * sizeof(uint32_t),
							indexBytes,
							BuildRuntimeMutationResidentUploadServices()))
					{
						return false;
					}
				}
			}
			{
				ScopedPtPerfTimer detailPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveRewriteMs);
				if (!preserveResidentPrimitiveSlice)
				{
					{
						ScopedPtPerfTimer primitiveCpuTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveCpuRewriteMs);
						UploadChunkPrimitiveDataToAtlas(
							residentGeometry,
							sourceChunk,
							nextAtlasChunk,
							mStaticMapScene.geometry.primitives);
						if (residentGeometry.primitiveProvenance.size() >= nextAtlasChunk.primitiveCount &&
							nextAtlasChunk.primitiveOffset + nextAtlasChunk.primitiveCount <= mStaticMapScene.geometry.primitiveProvenance.size())
						{
							std::copy_n(
								residentGeometry.primitiveProvenance.data(),
								nextAtlasChunk.primitiveCount,
								mStaticMapScene.geometry.primitiveProvenance.data() + nextAtlasChunk.primitiveOffset);
						}
					}
					const uint64_t primitiveBytes = (uint64_t)nextAtlasChunk.primitiveCount * sizeof(nri_scene::PrimitiveData);
					if (!mRuntimeMutation.QueueResidentGeometryUploadRange(
							ResidentUploadKind_Primitive,
							(uint64_t)nextAtlasChunk.primitiveOffset * sizeof(nri_scene::PrimitiveData),
							primitiveBytes,
							BuildRuntimeMutationResidentUploadServices()))
					{
						return false;
					}
				}
			}
			mLastPerfResourceTraceStats.residentChunkBatchVertexBytes +=
				(uint64_t)nextAtlasChunk.vertexCount * sizeof(nri_scene::SceneVertex);
			if (!preserveResidentIndexSlice)
			{
				mLastPerfResourceTraceStats.residentChunkBatchIndexBytes +=
					(uint64_t)nextAtlasChunk.indexCount * sizeof(uint32_t);
			}
			if (!preserveResidentPrimitiveSlice)
			{
				mLastPerfResourceTraceStats.residentChunkBatchPrimitiveBytes +=
					(uint64_t)nextAtlasChunk.primitiveCount * sizeof(nri_scene::PrimitiveData);
			}
		}

		auto& mutableChunk = mStaticMapScene.chunks[chunkListIndex];
		mutableChunk.chunkIndex = mapChunk.chunkIndex;
		mutableChunk.vertexOffset = nextAtlasChunk.vertexOffset;
		mutableChunk.vertexCount = nextAtlasChunk.vertexCount;
		mutableChunk.indexOffset = nextAtlasChunk.indexOffset;
		mutableChunk.indexCount = nextAtlasChunk.indexCount;
		mutableChunk.primitiveOffset = nextAtlasChunk.primitiveOffset;
		mutableChunk.primitiveCount = nextAtlasChunk.primitiveCount;
		mutableChunk.materialOffset = nextAtlasChunk.materialOffset;
		mutableChunk.materialCount = nextAtlasChunk.materialCount;
		mutableChunk.active = true;
		mutableChunk.blasUpdateEligible = preserveResidentIndexSlice || preserveResidentBlasRefitOnly;
		mutableChunk.lastResidentBlasReasonMask = appliedReasonMask;
		mutableChunk.lastResidentBlasSurfaceCount = outSurfaceCount;
		mutableChunk.lastResidentBlasTriangleCount = outTriangleCount;
		mutableChunk.lastResidentBlasMaterialCount = outMaterialCount;
		mutableChunk.lastResidentBlasForceTopology = appliedForceTopology;
		mutableChunk.lastResidentBlasRecoveredEmpty = outRecoveredEmpty;
		mutableChunk.lastResidentBlasKeptGeometrySlice = keptGeometrySlices;
		mutableChunk.lastResidentBlasTopologyChanged =
			hasResidentChunk &&
			previousGeometryTopologySignature != residentGeometryTopologySignature;
		if (!preserveResidentMaterialSlice)
		{
			mutableChunk.materialBridge = residentMaterials;
		}
		mutableChunk.geometryTopologySignature = residentGeometryTopologySignature;
		mutableChunk.primitiveLayoutSignature = residentPrimitiveLayoutSignature;
		mutableChunk.exactGeometrySignature = ComputeExactGeometrySignature(residentSceneView);
		if (!residentGeometryPayloadHashSkip &&
			!fastResidentMaterialOnlyUpdate &&
			!preserveResidentGeometryForMaterialOnlyUpdate)
		{
			if (residentGeometryPayloadHash == 0)
			{
				residentGeometryPayloadHash = HashResidentGeometryPayload(
					mMapWorld,
					residentGeometry,
					sourceChunk.vertexOffset,
					sourceChunk.vertexCount,
					sourceChunk.indexOffset,
					sourceChunk.indexCount,
					sourceChunk.primitiveOffset,
					sourceChunk.primitiveCount,
					sourceChunk.materialOffset,
					sourceChunk.materialCount);
			}
			mutableChunk.geometryPayloadHash = residentGeometryPayloadHash;
		}
		mutableChunk.animatedMaterialSignature = ComputeAnimatedMaterialSignature(residentSceneView);
		mutableChunk.animatedGeometrySignature = ComputeAnimatedGeometrySignature(residentSceneView);
		mutableChunk.hasAnimatedTextureCandidates = ChunkHasAnimatedStaticMapSurfaceCandidates(mMapWorld, mapChunk);
		mutableChunk.animatedRefreshSuppressed =
			previousAnimatedRefreshSuppressed &&
			(appliedStaticAnimatedReplacement ||
			 appliedAnimationOnlyRefreshed ||
			 mutableChunk.animatedGeometrySignature == previousAnimatedGeometrySignature);
		mStaticMapScene.lightChunkViews[chunkListIndex] = residentSceneView;
		outStaticSceneChunkListIndex = chunkListIndex;
		outMaterialDirty = !preserveResidentMaterialSlice;
		appliedPreservedResidentMaterialSlice = preserveResidentMaterialSlice;
		appliedPreservedResidentGeometryPayload =
			residentGeometryPayloadHashSkip ||
			fastResidentMaterialOnlyUpdate ||
			preserveResidentGeometryForMaterialOnlyUpdate;
		outGeometryDirty =
			!residentGeometryPayloadHashSkip &&
			((!preserveResidentGeometryForMaterialOnlyUpdate && !keptGeometrySlices) ||
			(appliedReasonMask & (
				nri_scene::PTMapChunkMutationReason_SectorGeometry |
				nri_scene::PTMapChunkMutationReason_WallGeometry |
				nri_scene::PTMapChunkMutationReason_SectorDirty |
				nri_scene::PTMapChunkMutationReason_SectionDirty)) != 0);
	}

	replacement.baseline = appliedBaseline;
	replacement.replacementBaseline = appliedBaseline;
	replacement.baselineSignature = appliedBaseline.signature;
	replacement.liveSignature = appliedBaseline.signature;
	replacement.reasonMask = nri_scene::PTMapChunkMutationReason_None;
	replacement.sectionDirtyCount = 0;
	replacement.stableMutationFrameCount = 0;
	replacement.sectorDirty = false;
	replacement.dragged = false;
	replacement.blindSpot = false;
	replacement.excludeStaticChunk = false;
	replacement.staticAnimatedReplacement = false;
	replacement.active = false;
	replacement.valid = false;
	replacement.residentAuthoritative = true;
	replacement.animationOnlyRefreshed = false;
	replacement.animatedMaterialSignature = ComputeAnimatedMaterialSignature(residentSceneView);
	replacement.surfaceCount = 0;
	replacement.triangleCount = 0;
	mRuntimeMutation.ClearReplacementPayload(replacement, true);

	mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].appliedBaseline = appliedBaseline;
	mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].baselineSignature = appliedBaseline.signature;
	mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].liveSignature = appliedBaseline.signature;
	mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].staticSceneChunkListIndex = chunkListIndex;
	mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].active = !chunkBecomesEmpty;
	mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].mappedInStaticScene = !chunkBecomesEmpty;
	mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].exactGeometrySignature =
		!chunkBecomesEmpty ? mStaticMapScene.chunks[chunkListIndex].exactGeometrySignature : 0;
	mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].geometryTopologySignature =
		!chunkBecomesEmpty ? mStaticMapScene.chunks[chunkListIndex].geometryTopologySignature : 0;
	if (!chunkBecomesEmpty)
	{
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].vertexOffset = mStaticMapChunkAtlas.chunks[chunkListIndex].vertexOffset;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].vertexCount = mStaticMapChunkAtlas.chunks[chunkListIndex].vertexCount;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].indexOffset = mStaticMapChunkAtlas.chunks[chunkListIndex].indexOffset;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].indexCount = mStaticMapChunkAtlas.chunks[chunkListIndex].indexCount;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].primitiveOffset = mStaticMapChunkAtlas.chunks[chunkListIndex].primitiveOffset;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].primitiveCount = mStaticMapChunkAtlas.chunks[chunkListIndex].primitiveCount;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].materialOffset = mStaticMapChunkAtlas.chunks[chunkListIndex].materialOffset;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].materialCount = mStaticMapChunkAtlas.chunks[chunkListIndex].materialCount;
		if (!appliedPreservedResidentMaterialSlice)
		{
			mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].materialPayloadHash = residentMaterialPayloadHash;
		}
		if (!appliedPreservedResidentGeometryPayload)
		{
			mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].geometryPayloadHash = residentGeometryPayloadHash;
		}
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].accelerationResident =
			mStaticMapScene.chunks[chunkListIndex].accelerationStructure.accelerationStructure != nullptr;
	}
	else
	{
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].vertexOffset = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].vertexCount = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].indexOffset = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].indexCount = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].primitiveOffset = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].primitiveCount = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].materialOffset = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].materialCount = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].materialPayloadHash = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].geometryPayloadHash = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].accelerationResident = false;
	}
	if (!outMaterialDirty && !residentMaterialPayloadHashSkip)
	{
		outMaterialDirty =
			(appliedReasonMask & (nri_scene::PTMapChunkMutationReason_SectorMaterial | nri_scene::PTMapChunkMutationReason_WallMaterial)) != 0 ||
			appliedStaticAnimatedReplacement ||
			appliedAnimationOnlyRefreshed ||
			materialOnlyReplacement;
	}
	if (!outGeometryDirty && !residentGeometryPayloadHashSkip)
	{
		outGeometryDirty =
			(appliedReasonMask & (
				nri_scene::PTMapChunkMutationReason_SectorGeometry |
				nri_scene::PTMapChunkMutationReason_WallGeometry |
				nri_scene::PTMapChunkMutationReason_SectorDirty |
				nri_scene::PTMapChunkMutationReason_SectionDirty)) != 0;
	}
	return true;
}
