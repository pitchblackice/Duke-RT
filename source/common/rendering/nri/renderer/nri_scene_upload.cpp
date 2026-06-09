#include "nri_scene_upload.h"

#include "nri_renderer.h"
#include "../system/nri_renderdevice.h"

#include <algorithm>
#include <cstring>

bool NRISceneUploadManager::CreateStructuredBuffer(
	NRIRenderer& renderer,
	NRIBufferResource& resource,
	const void* data,
	uint64_t size,
	uint32_t stride,
	nri::BufferUsageBits usage,
	nri::AccessStage after)
{
	const NRIResourceContext resourceContext = renderer.BuildResourceContext();
	if (resource.buffer != nullptr || resource.shaderView != nullptr)
	{
		renderer.WaitForCommandsTracked();
	}

	renderer.DestroyBufferResource(resource);

	nri::BufferDesc desc = {};
	desc.size = std::max<uint64_t>(size, stride);
	desc.structureStride = stride;
	desc.usage = usage;

	if (resourceContext.core->CreateCommittedBuffer(*resourceContext.device, nri::MemoryLocation::DEVICE_UPLOAD, 0.0f, desc, resource.buffer) != nri::Result::SUCCESS)
	{
		return false;
	}

	nri::MemoryDesc memoryDesc = {};
	resourceContext.core->GetBufferMemoryDesc(*resource.buffer, nri::MemoryLocation::DEVICE_UPLOAD, memoryDesc);
	resource.size = desc.size;
	resource.memorySize = memoryDesc.size;
	resource.memoryLocation = nri::MemoryLocation::DEVICE_UPLOAD;
	resource.usedSize = size;
	resource.stride = stride;

	nri::BufferViewDesc viewDesc = {};
	viewDesc.buffer = resource.buffer;
	viewDesc.type = nri::BufferView::STRUCTURED_BUFFER;
	viewDesc.offset = 0;
	viewDesc.size = nri::WHOLE_SIZE;
	viewDesc.structureStride = stride;
	if (resourceContext.core->CreateBufferView(viewDesc, resource.shaderView) != nri::Result::SUCCESS)
	{
		return false;
	}

	if (data != nullptr && size != 0)
	{
		void* mapped = resourceContext.core->MapBuffer(*resource.buffer, 0, desc.size);
		if (mapped == nullptr)
		{
			return false;
		}

		std::memcpy(mapped, data, (size_t)size);
		if (desc.size > size)
		{
			std::memset(static_cast<uint8_t*>(mapped) + size, 0, (size_t)(desc.size - size));
		}
		resourceContext.core->UnmapBuffer(*resource.buffer);
	}

	if (resourceContext.commandBuffer != nullptr && after.access != nri::AccessBits::NONE)
	{
		nri::BufferBarrierDesc barrier = {};
		barrier.buffer = resource.buffer;
		barrier.before = {};
		barrier.after = after;

		nri::BarrierDesc barrierDesc = {};
		barrierDesc.buffers = &barrier;
		barrierDesc.bufferNum = 1;
		resourceContext.core->CmdBarrier(*resourceContext.commandBuffer, barrierDesc);
	}

	return true;
}

bool NRISceneUploadManager::EnsureStructuredBuffer(
	NRIRenderer& renderer,
	NRIBufferResource& resource,
	SceneBufferDebugStats& stats,
	const void* data,
	uint64_t size,
	uint32_t stride,
	nri::BufferUsageBits usage,
	nri::AccessStage after,
	bool writesQuiesced,
	const char* waitReason)
{
	const uint64_t requiredSize = std::max<uint64_t>(size, stride);
	const bool needsGrowth =
		resource.buffer == nullptr ||
		resource.shaderView == nullptr ||
		resource.stride != stride ||
		resource.size < requiredSize;

	stats.bytesUploadedLastFrame = size;
	stats.growEventsLastFrame = 0;
	stats.overwriteEventsLastFrame = 0;
	stats.growthOldBytesLastFrame = 0;
	stats.growthRequestedBytesLastFrame = 0;
	stats.growthAllocatedBytesLastFrame = 0;
	stats.uploadCount++;
	stats.peakUsedBytes = std::max(stats.peakUsedBytes, size);
	renderer.NotePerfBufferUpload(&stats, size, needsGrowth, waitReason, -1);

	if (needsGrowth)
	{
		const bool isSceneBufferUpload =
			waitReason != nullptr &&
			std::strcmp(waitReason, "scene_buffer_upload") == 0;
		const uint64_t oldSize = resource.size;
		const uint64_t grownSize =
			isSceneBufferUpload ?
			GetNRISceneUploadGrownBufferSize(resource.size, requiredSize, stride) :
			GetNRIGrownBufferSize(resource.size, requiredSize, stride);
		if (!writesQuiesced && (resource.buffer != nullptr || resource.shaderView != nullptr))
		{
			renderer.WaitForCommandsTracked(waitReason);
		}
		renderer.DestroyBufferResource(resource);

		nri::BufferDesc desc = {};
		desc.size = std::max<uint64_t>(grownSize, stride);
		desc.structureStride = stride;
		desc.usage = usage;

		const NRIResourceContext resourceContext = renderer.BuildResourceContext();
		if (resourceContext.core->CreateCommittedBuffer(*resourceContext.device, nri::MemoryLocation::DEVICE_UPLOAD, 0.0f, desc, resource.buffer) != nri::Result::SUCCESS)
		{
			return false;
		}

		nri::MemoryDesc memoryDesc = {};
		resourceContext.core->GetBufferMemoryDesc(*resource.buffer, nri::MemoryLocation::DEVICE_UPLOAD, memoryDesc);
		resource.size = desc.size;
		resource.memorySize = memoryDesc.size;
		resource.memoryLocation = nri::MemoryLocation::DEVICE_UPLOAD;
		resource.usedSize = size;
		resource.stride = stride;

		nri::BufferViewDesc viewDesc = {};
		viewDesc.buffer = resource.buffer;
		viewDesc.type = nri::BufferView::STRUCTURED_BUFFER;
		viewDesc.offset = 0;
		viewDesc.size = nri::WHOLE_SIZE;
		viewDesc.structureStride = stride;
		if (resourceContext.core->CreateBufferView(viewDesc, resource.shaderView) != nri::Result::SUCCESS)
		{
			return false;
		}

		stats.growthCount++;
		stats.growEventsLastFrame = 1;
		stats.growthOldBytesLastFrame = oldSize;
		stats.growthRequestedBytesLastFrame = requiredSize;
		stats.growthAllocatedBytesLastFrame = desc.size;
	}
	else
	{
		resource.usedSize = size;
		stats.overwriteCount++;
		stats.overwriteEventsLastFrame = 1;
	}

	if (data != nullptr && size != 0)
	{
		if (!needsGrowth && !writesQuiesced)
		{
			// Scene buffers are reused persistent DEVICE_UPLOAD allocations. Fence before
			// overwriting them so prior queued frames cannot read partially updated data.
			renderer.WaitForCommandsTracked(waitReason);
		}

		const NRIResourceContext resourceContext = renderer.BuildResourceContext();
		void* mapped = resourceContext.core->MapBuffer(*resource.buffer, 0, resource.size);
		if (mapped == nullptr)
		{
			return false;
		}

		std::memcpy(mapped, data, (size_t)size);
		if (needsGrowth && resource.size > size)
		{
			std::memset(static_cast<uint8_t*>(mapped) + size, 0, (size_t)(resource.size - size));
		}
		resourceContext.core->UnmapBuffer(*resource.buffer);
	}

	const NRIResourceContext resourceContext = renderer.BuildResourceContext();
	if (resourceContext.commandBuffer != nullptr && after.access != nri::AccessBits::NONE)
	{
		nri::BufferBarrierDesc barrier = {};
		barrier.buffer = resource.buffer;
		barrier.before = {};
		barrier.after = after;

		nri::BarrierDesc barrierDesc = {};
		barrierDesc.buffers = &barrier;
		barrierDesc.bufferNum = 1;
		resourceContext.core->CmdBarrier(*resourceContext.commandBuffer, barrierDesc);
	}

	return true;
}

bool NRISceneUploadManager::UpdateStructuredBufferRange(
	NRIRenderer& renderer,
	NRIBufferResource& resource,
	uint64_t byteOffset,
	const void* data,
	uint64_t size,
	nri::AccessStage after)
{
	if (resource.buffer == nullptr ||
		data == nullptr ||
		size == 0 ||
		byteOffset > resource.size ||
		size > resource.size - byteOffset)
	{
		return false;
	}

	const NRIResourceContext resourceContext = renderer.BuildResourceContext();
	void* mapped = resourceContext.core->MapBuffer(*resource.buffer, byteOffset, size);
	if (mapped == nullptr)
	{
		return false;
	}

	std::memcpy(mapped, data, (size_t)size);
	resourceContext.core->UnmapBuffer(*resource.buffer);

	if (resourceContext.commandBuffer != nullptr && after.access != nri::AccessBits::NONE)
	{
		nri::BufferBarrierDesc barrier = {};
		barrier.buffer = resource.buffer;
		barrier.before = {};
		barrier.after = after;

		nri::BarrierDesc barrierDesc = {};
		barrierDesc.buffers = &barrier;
		barrierDesc.bufferNum = 1;
		resourceContext.core->CmdBarrier(*resourceContext.commandBuffer, barrierDesc);
	}

	return true;
}
