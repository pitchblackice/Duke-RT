#include "nri_scene_upload.h"

#include "nri_renderer.h"
#include "../system/nri_renderdevice.h"

#include <algorithm>
#include <cstring>
#include <vector>
bool NRIRenderer::StageResidentMaterialUploadRanges(
	const NRIBufferResource& targetBuffer,
	const std::vector<RuntimeMutationResidentUploadRange>& ranges,
	const uint8_t* data,
	uint64_t availableBytes,
	uint32_t& batchCount,
	uint32_t& batchRangeCount,
	uint32_t& barrierCommandCount,
	uint32_t& copyCommandCount)
{
	if (ranges.empty())
	{
		return true;
	}

	if (targetBuffer.buffer == nullptr ||
		data == nullptr ||
		mFrameBuffer == nullptr ||
		mFrameBuffer->mCommandBuffer == nullptr)
	{
		return false;
	}

	constexpr uint64_t kResidentUploadScratchAlignment = 16u;
	auto& frameScratch = GetResidentUploadScratchFrame();
	ResidentBufferUploadScratch& scratch = frameScratch.material;
	uint64_t requiredSize = scratch.cursor;
	for (const RuntimeMutationResidentUploadRange& range : ranges)
	{
		if (range.uploadKind != ResidentUploadKind_Material ||
			range.size == 0 ||
			range.byteOffset > availableBytes ||
			range.size > availableBytes - range.byteOffset ||
			range.byteOffset > targetBuffer.size ||
			range.size > targetBuffer.size - range.byteOffset)
		{
			return false;
		}

		requiredSize =
			(requiredSize + kResidentUploadScratchAlignment - 1u) &
			~(kResidentUploadScratchAlignment - 1u);
		requiredSize += range.size;
	}

	if (!EnsureResidentUploadScratchBuffer(scratch, frameScratch, requiredSize))
	{
		return false;
	}

	struct StagedCopy
	{
		uint64_t targetOffset = 0;
		uint64_t scratchOffset = 0;
		uint64_t size = 0;
		const uint8_t* data = nullptr;
	};

	std::vector<StagedCopy> stagedCopies;
	stagedCopies.reserve(ranges.size());
	uint64_t mapStart = UINT64_MAX;
	uint64_t mapEnd = 0;
	for (const RuntimeMutationResidentUploadRange& range : ranges)
	{
		const uint64_t scratchOffset =
			(scratch.cursor + kResidentUploadScratchAlignment - 1u) &
			~(kResidentUploadScratchAlignment - 1u);
		const uint64_t rangeEnd = scratchOffset + range.size;
		if (rangeEnd > scratch.buffer.size)
		{
			return false;
		}

		scratch.cursor = rangeEnd;
		mapStart = std::min(mapStart, scratchOffset);
		mapEnd = std::max(mapEnd, rangeEnd);
		stagedCopies.push_back({ range.byteOffset, scratchOffset, range.size, data + range.byteOffset });
	}

	const uint64_t mapSize = mapEnd - mapStart;
	void* mapped = mFrameBuffer->mCore.MapBuffer(*scratch.buffer.buffer, mapStart, mapSize);
	if (mapped == nullptr)
	{
		return false;
	}

	for (const StagedCopy& copy : stagedCopies)
	{
		std::memcpy(static_cast<uint8_t*>(mapped) + (copy.scratchOffset - mapStart), copy.data, (size_t)copy.size);
	}
	mFrameBuffer->mCore.UnmapBuffer(*scratch.buffer.buffer);

	batchCount++;
	batchRangeCount += (uint32_t)stagedCopies.size();

	if (!scratch.copySourceActive)
	{
		nri::BufferBarrierDesc sourceBarrier = {};
		sourceBarrier.buffer = scratch.buffer.buffer;
		sourceBarrier.before = {};
		sourceBarrier.after = NRIResourceCopySourceAccess();

		nri::BarrierDesc sourceBarrierDesc = {};
		sourceBarrierDesc.buffers = &sourceBarrier;
		sourceBarrierDesc.bufferNum = 1;
		mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, sourceBarrierDesc);
		scratch.copySourceActive = true;
		barrierCommandCount++;
	}

	nri::BufferBarrierDesc beforeCopyBarrier = {};
	beforeCopyBarrier.buffer = targetBuffer.buffer;
	beforeCopyBarrier.before = NRIResourceComputeShaderResourceAccess();
	beforeCopyBarrier.after = NRIResourceCopyDestinationAccess();

	nri::BarrierDesc beforeCopyBarrierDesc = {};
	beforeCopyBarrierDesc.buffers = &beforeCopyBarrier;
	beforeCopyBarrierDesc.bufferNum = 1;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, beforeCopyBarrierDesc);
	barrierCommandCount++;

	for (const StagedCopy& copy : stagedCopies)
	{
		mFrameBuffer->mCore.CmdCopyBuffer(
			*mFrameBuffer->mCommandBuffer,
			*targetBuffer.buffer,
			copy.targetOffset,
			*scratch.buffer.buffer,
			copy.scratchOffset,
			copy.size);
		copyCommandCount++;
	}

	nri::BufferBarrierDesc afterCopyBarrier = {};
	afterCopyBarrier.buffer = targetBuffer.buffer;
	afterCopyBarrier.before = NRIResourceCopyDestinationAccess();
	afterCopyBarrier.after = NRIResourceComputeShaderResourceAccess();

	nri::BarrierDesc afterCopyBarrierDesc = {};
	afterCopyBarrierDesc.buffers = &afterCopyBarrier;
	afterCopyBarrierDesc.bufferNum = 1;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, afterCopyBarrierDesc);
	barrierCommandCount++;

	return true;
}
