#include "nri_gpu_timing.h"

#include "nri_renderdevice.h"
#include "c_cvars.h"
#include "printf.h"

#include <cstring>

CVAR(Bool, nri_ptgputiming, false, 0)

namespace
{
	bool IsVoxelTimingScope(NRIGpuTimingScope scope)
	{
		return scope >= NRIGpuTimingScope::VoxelAdmission && scope <= NRIGpuTimingScope::WorldTlas;
	}

	double TimestampDeltaMs(uint64_t begin, uint64_t end, uint64_t frequency)
	{
		if (frequency == 0 || end < begin) return -1.0;
		return (double)((long double)(end - begin) * 1000.0L / (long double)frequency);
	}
}

void NRIGpuTiming::Prepare(nri::CoreInterface& core, nri::Device& device)
{
	if (mPrepared || mUnsupported || !nri_ptgputiming) return;
	mFrequencyHz = core.GetDeviceDesc(device).other.timestampFrequencyHz;
	if (mFrequencyHz == 0)
	{
		mUnsupported = true;
		Printf(TEXTCOLOR_YELLOW "NRI GPU timing disabled: timestamp frequency is zero.\n");
		return;
	}

	for (Slot& slot : mSlots)
	{
		nri::QueryPoolDesc queryDesc = {};
		queryDesc.queryType = nri::QueryType::TIMESTAMP;
		queryDesc.capacity = QueryCapacity;
		if (core.CreateQueryPool(device, queryDesc, slot.queryPool) != nri::Result::SUCCESS)
		{
			mUnsupported = true;
			break;
		}
		slot.querySize = core.GetQuerySize(*slot.queryPool);
		if (slot.querySize < sizeof(uint64_t))
		{
			mUnsupported = true;
			break;
		}
		nri::BufferDesc bufferDesc = {};
		bufferDesc.size = (uint64_t)slot.querySize * QueryCapacity;
		bufferDesc.structureStride = slot.querySize;
		bufferDesc.usage = nri::BufferUsageBits::NONE;
		if (core.CreateCommittedBuffer(device, nri::MemoryLocation::HOST_READBACK, 0.0f, bufferDesc, slot.readback) != nri::Result::SUCCESS)
		{
			mUnsupported = true;
			break;
		}
	}
	if (mUnsupported)
	{
		Destroy(core);
		mUnsupported = true;
		Printf(TEXTCOLOR_YELLOW "NRI GPU timing disabled: timestamp query resources could not be created.\n");
		return;
	}
	mPrepared = true;
}

void NRIGpuTiming::Destroy(nri::CoreInterface& core)
{
	mActiveSlot = SlotCount;
	for (Slot& slot : mSlots)
	{
		if (slot.readback != nullptr) core.DestroyBuffer(slot.readback);
		if (slot.queryPool != nullptr) core.DestroyQueryPool(slot.queryPool);
		slot = {};
	}
	mPrepared = false;
}

void NRIGpuTiming::RetireSlot(nri::CoreInterface& core, uint32_t slotIndex)
{
	if (!mPrepared || slotIndex >= SlotCount) return;
	Slot& slot = mSlots[slotIndex];
	if (!slot.pending) return;

	PerfCompactGpuTiming timing = {};
	timing.segmentCount = 1;
	timing.droppedScopes = slot.droppedScopes;
	double voxelAdmissionMs = 0.0;
	double voxelUploadMs = 0.0;
	double voxelArenaCopyMs = 0.0;
	double voxelClassifyMs = 0.0;
	double voxelScanMs = 0.0;
	double voxelEmitMs = 0.0;
	double voxelFinalizeMs = 0.0;
	double voxelBlasMs = 0.0;
	double worldTlasMs = 0.0;
	uint32_t voxelScopeCount = 0;
	uint32_t validVoxelScopes = 0;
	uint32_t invalidVoxelScopes = 0;
	bool segmentValid = false;
	const uint64_t readbackSize = (uint64_t)slot.querySize * slot.queryCount;
	const uint8_t* mapped = static_cast<const uint8_t*>(core.MapBuffer(*slot.readback, 0, readbackSize));
	if (mapped == nullptr)
	{
		timing.invalidPairs++;
		for (uint32_t i = 0; i < slot.markerCount; ++i)
		{
			if (IsVoxelTimingScope(slot.markers[i].scope))
			{
				voxelScopeCount++;
				invalidVoxelScopes++;
			}
		}
	}
	else
	{
		auto readTimestamp = [&](uint32_t query)
		{
			uint64_t value = 0;
			std::memcpy(&value, mapped + (uint64_t)query * slot.querySize, sizeof(value));
			return value;
		};
		const double segmentMs = TimestampDeltaMs(
			readTimestamp(slot.segmentBeginQuery), readTimestamp(slot.segmentEndQuery), mFrequencyHz);
		if (segmentMs < 0.0) timing.invalidPairs++;
		else
		{
			timing.segmentMs = segmentMs;
			segmentValid = true;
		}

		for (uint32_t i = 0; i < slot.markerCount; ++i)
		{
			const Marker& marker = slot.markers[i];
			const bool voxelScope = IsVoxelTimingScope(marker.scope);
			if (voxelScope) voxelScopeCount++;
			const double value = TimestampDeltaMs(readTimestamp(marker.beginQuery), readTimestamp(marker.endQuery), mFrequencyHz);
			if (value < 0.0)
			{
				timing.invalidPairs++;
				if (voxelScope) invalidVoxelScopes++;
				continue;
			}
			if (voxelScope) validVoxelScopes++;
			switch (marker.scope)
			{
			case NRIGpuTimingScope::Scene: timing.sceneMs += value; break;
			case NRIGpuTimingScope::Trace: timing.traceMs += value; break;
			case NRIGpuTimingScope::TraceDispatch: timing.traceDispatchMs += value; break;
			case NRIGpuTimingScope::Denoise: timing.denoiseMs += value; break;
			case NRIGpuTimingScope::Composition: timing.compositionMs += value; break;
			case NRIGpuTimingScope::Upscale: timing.upscaleMs += value; break;
			case NRIGpuTimingScope::Final: timing.finalMs += value; break;
			case NRIGpuTimingScope::VoxelAdmission: voxelAdmissionMs += value; break;
			case NRIGpuTimingScope::VoxelUpload: voxelUploadMs += value; break;
			case NRIGpuTimingScope::VoxelArenaCopy: voxelArenaCopyMs += value; break;
			case NRIGpuTimingScope::VoxelClassify: voxelClassifyMs += value; break;
			case NRIGpuTimingScope::VoxelScan: voxelScanMs += value; break;
			case NRIGpuTimingScope::VoxelEmit: voxelEmitMs += value; break;
			case NRIGpuTimingScope::VoxelFinalize: voxelFinalizeMs += value; break;
			case NRIGpuTimingScope::VoxelBlas: voxelBlasMs += value; break;
			case NRIGpuTimingScope::WorldTlas: worldTlasMs += value; break;
			default: break;
			}
		}
		core.UnmapBuffer(*slot.readback);
	}
	Printf("PERF pt voxel gpu timing NRI: renderer_frame=%llu presentation_gen=%llu queued_slot=%u segment=%.6f segment_valid=%u admission=%.6f upload=%.6f arena_copy=%.6f classify=%.6f scan=%.6f emit=%.6f finalize=%.6f voxel_blas=%.6f world_tlas=%.6f scopes=%u valid=%u invalid=%u dropped=%u compact=1 epoch=%llu record=%u\n",
		(unsigned long long)slot.rendererFrame,
		(unsigned long long)slot.token.presentationGeneration,
		slotIndex,
		timing.segmentMs,
		segmentValid ? 1u : 0u,
		voxelAdmissionMs,
		voxelUploadMs,
		voxelArenaCopyMs,
		voxelClassifyMs,
		voxelScanMs,
		voxelEmitMs,
		voxelFinalizeMs,
		voxelBlasMs,
		worldTlasMs,
		voxelScopeCount,
		validVoxelScopes,
		invalidVoxelScopes,
		slot.droppedVoxelScopes,
		(unsigned long long)slot.token.epoch,
		slot.token.recordIndex);
	PerfCompactCaptureResolveGpuSegment(slot.token, timing);
	slot.pending = false;
	slot.token = {};
	slot.queryCount = 0;
	slot.markerCount = 0;
	slot.droppedScopes = 0;
	slot.droppedVoxelScopes = 0;
	slot.rendererFrame = 0;
}

void NRIGpuTiming::BeginSegment(nri::CoreInterface& core, nri::CommandBuffer& commandBuffer, uint32_t slotIndex, uint64_t rendererFrame)
{
	if (!mPrepared || !nri_ptgputiming || !PerfCompactCaptureTimingActive() || slotIndex >= SlotCount) return;
	Slot& slot = mSlots[slotIndex];
	if (slot.pending) return;
	core.ResetQueries(*slot.queryPool, 0, QueryCapacity);
	slot.token = PerfCompactCaptureGetCurrentToken();
	if (!slot.token) return;
	slot.queryCount = 2;
	slot.markerCount = 0;
	slot.droppedScopes = 0;
	slot.droppedVoxelScopes = 0;
	slot.rendererFrame = rendererFrame;
	slot.segmentBeginQuery = 0;
	slot.segmentEndQuery = 1;
	core.CmdEndQuery(commandBuffer, *slot.queryPool, slot.segmentBeginQuery);
	mActiveSlot = slotIndex;
	PerfCompactCaptureExpectGpuSegment(slot.token);
}

uint32_t NRIGpuTiming::BeginScope(nri::CoreInterface& core, nri::CommandBuffer& commandBuffer, NRIGpuTimingScope scope)
{
	if (!IsRecording()) return UINT32_MAX;
	Slot& slot = mSlots[mActiveSlot];
	if (slot.markerCount >= ScopeCapacity || slot.queryCount + 2 > QueryCapacity)
	{
		slot.droppedScopes++;
		if (IsVoxelTimingScope(scope)) slot.droppedVoxelScopes++;
		return UINT32_MAX;
	}
	const uint32_t markerIndex = slot.markerCount++;
	Marker& marker = slot.markers[markerIndex];
	marker.scope = scope;
	marker.beginQuery = slot.queryCount++;
	marker.endQuery = slot.queryCount++;
	marker.open = true;
	core.CmdEndQuery(commandBuffer, *slot.queryPool, marker.beginQuery);
	return markerIndex;
}

void NRIGpuTiming::EndScope(nri::CoreInterface& core, nri::CommandBuffer& commandBuffer, uint32_t markerIndex)
{
	if (!IsRecording() || markerIndex == UINT32_MAX) return;
	Slot& slot = mSlots[mActiveSlot];
	if (markerIndex >= slot.markerCount || !slot.markers[markerIndex].open) return;
	Marker& marker = slot.markers[markerIndex];
	core.CmdEndQuery(commandBuffer, *slot.queryPool, marker.endQuery);
	marker.open = false;
}

void NRIGpuTiming::FinalizeSegment(nri::CoreInterface& core, nri::CommandBuffer& commandBuffer)
{
	if (!IsRecording()) return;
	Slot& slot = mSlots[mActiveSlot];
	for (uint32_t i = 0; i < slot.markerCount; ++i)
	{
		if (slot.markers[i].open)
		{
			core.CmdEndQuery(commandBuffer, *slot.queryPool, slot.markers[i].endQuery);
			slot.markers[i].open = false;
			slot.droppedScopes++;
			if (IsVoxelTimingScope(slot.markers[i].scope)) slot.droppedVoxelScopes++;
		}
	}
	core.CmdEndQuery(commandBuffer, *slot.queryPool, slot.segmentEndQuery);
	core.CmdCopyQueries(commandBuffer, *slot.queryPool, 0, slot.queryCount, *slot.readback, 0);
	slot.pending = true;
	mActiveSlot = SlotCount;
}

void NRIGpuTiming::AbandonSlot(uint32_t slotIndex)
{
	if (slotIndex >= SlotCount) return;
	Slot& slot = mSlots[slotIndex];
	if (!slot.pending && mActiveSlot != slotIndex) return;
	PerfCompactGpuTiming timing = {};
	timing.invalidPairs = 1;
	timing.droppedScopes = slot.droppedScopes;
	PerfCompactCaptureResolveGpuSegment(slot.token, timing);
	nri::QueryPool* queryPool = slot.queryPool;
	nri::Buffer* readback = slot.readback;
	const uint32_t querySize = slot.querySize;
	slot = {};
	slot.queryPool = queryPool;
	slot.readback = readback;
	slot.querySize = querySize;
	mActiveSlot = SlotCount;
}

NRIScopedGpuTiming::NRIScopedGpuTiming(NRIRenderDevice* device, NRIGpuTimingScope scope)
	: mDevice(device)
{
	if (mDevice != nullptr) mMarker = mDevice->BeginGpuTimingScope(scope);
}

NRIScopedGpuTiming::~NRIScopedGpuTiming()
{
	if (mDevice != nullptr) mDevice->EndGpuTimingScope(mMarker);
}

uint32_t NRIRenderDevice::BeginGpuTimingScope(NRIGpuTimingScope scope)
{
	if (mGpuTiming == nullptr || mCommandBuffer == nullptr || !mCommandBufferOpen) return UINT32_MAX;
	return mGpuTiming->BeginScope(mCore, *mCommandBuffer, scope);
}

void NRIRenderDevice::EndGpuTimingScope(uint32_t markerIndex)
{
	if (mGpuTiming == nullptr || mCommandBuffer == nullptr || !mCommandBufferOpen) return;
	mGpuTiming->EndScope(mCore, *mCommandBuffer, markerIndex);
}
