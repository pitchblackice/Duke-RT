#include "nri_scene_upload.h"

#include "nri_renderer.h"
#include "nri_resources.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace
{
	double DurationMs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}
}

bool NRISceneUploadManager::CreateStructuredBuffer(
	NRIRenderer& renderer,
	NRIBufferResource& resource,
	const void* data,
	uint64_t size,
	uint32_t stride,
	nri::BufferUsageBits usage,
	nri::AccessStage after)
{
	const NRIResourceServices resourceServices = renderer.BuildResourceServices();
	const NRIResourceContext& resourceContext = resourceServices.context;
	if (resource.buffer != nullptr || resource.shaderView != nullptr)
	{
		resourceServices.WaitForCommands();
	}

	resourceServices.DestroyBufferResource(resource);

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
	resource.usage = desc.usage;

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

	// DEVICE_UPLOAD buffers remain in their backend-defined, GPU-readable state.
	// Queue submission makes completed host writes available to GPU reads; issuing
	// a transition from an invented COMMON state is invalid on D3D12 upload heaps.
	(void)after;

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
		!NRIResourceUsageIncludes(resource.usage, usage) ||
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
	const NRIResourceServices resourceServices = renderer.BuildResourceServices();
	const NRIResourceContext& resourceContext = resourceServices.context;

	if (needsGrowth)
	{
		const bool isSceneBufferUpload =
			waitReason != nullptr &&
			std::strcmp(waitReason, "scene_buffer_upload") == 0;
		NRIBufferResource oldResource = resource;
		const uint64_t oldSize = oldResource.size;
		const uint64_t grownSize =
			isSceneBufferUpload ?
			GetNRISceneUploadGrownBufferSize(oldResource.size, requiredSize, stride) :
			GetNRIGrownBufferSize(oldResource.size, requiredSize, stride);
		if (!writesQuiesced && (oldResource.buffer != nullptr || oldResource.shaderView != nullptr))
		{
			resourceServices.WaitForCommands(waitReason);
		}
		resource = {};

		nri::BufferDesc desc = {};
		desc.size = std::max<uint64_t>(grownSize, stride);
		desc.structureStride = stride;
		desc.usage = usage;

		if (resourceContext.core->CreateCommittedBuffer(*resourceContext.device, nri::MemoryLocation::DEVICE_UPLOAD, 0.0f, desc, resource.buffer) != nri::Result::SUCCESS)
		{
			resource = oldResource;
			return false;
		}

		nri::MemoryDesc memoryDesc = {};
		resourceContext.core->GetBufferMemoryDesc(*resource.buffer, nri::MemoryLocation::DEVICE_UPLOAD, memoryDesc);
		resource.size = desc.size;
		resource.memorySize = memoryDesc.size;
		resource.memoryLocation = nri::MemoryLocation::DEVICE_UPLOAD;
		resource.usedSize = size;
		resource.stride = stride;
		resource.usage = desc.usage;

		nri::BufferViewDesc viewDesc = {};
		viewDesc.buffer = resource.buffer;
		viewDesc.type = nri::BufferView::STRUCTURED_BUFFER;
		viewDesc.offset = 0;
		viewDesc.size = nri::WHOLE_SIZE;
		viewDesc.structureStride = stride;
		if (resourceContext.core->CreateBufferView(viewDesc, resource.shaderView) != nri::Result::SUCCESS)
		{
			resourceServices.DestroyBufferResource(resource);
			resource = oldResource;
			return false;
		}

		stats.growthCount++;
		stats.growEventsLastFrame = 1;
		stats.growthOldBytesLastFrame = oldSize;
		stats.growthRequestedBytesLastFrame = requiredSize;
		stats.growthAllocatedBytesLastFrame = desc.size;
		if (oldResource.buffer != nullptr || oldResource.shaderView != nullptr)
		{
			if (writesQuiesced)
			{
				renderer.RetireResidentBufferResource(oldResource);
			}
			else
			{
				resourceServices.DestroyBufferResource(oldResource);
			}
		}
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
			resourceServices.WaitForCommands(waitReason);
		}

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

	// See CreateStructuredBuffer: mapped upload buffers are not transitioned.
	(void)after;

	return true;
}

bool NRISceneUploadManager::EnsureStructuredBufferCapacity(
	NRIRenderer& renderer,
	NRIBufferResource& resource,
	SceneBufferDebugStats& stats,
	uint64_t size,
	uint32_t stride,
	nri::BufferUsageBits usage,
	const char* waitReason)
{
	const uint64_t requiredSize = std::max<uint64_t>(size, stride);
	stats.bytesUploadedLastFrame = 0;
	stats.growEventsLastFrame = 0;
	stats.overwriteEventsLastFrame = 0;
	stats.growthOldBytesLastFrame = 0;
	stats.growthRequestedBytesLastFrame = 0;
	stats.growthAllocatedBytesLastFrame = 0;
	stats.peakUsedBytes = std::max(stats.peakUsedBytes, size);

	if (resource.buffer != nullptr &&
		resource.shaderView != nullptr &&
		resource.stride == stride &&
		NRIResourceUsageIncludes(resource.usage, usage) &&
		resource.size >= requiredSize)
	{
		return true;
	}

	const NRIResourceServices resourceServices = renderer.BuildResourceServices();
	const NRIResourceContext& resourceContext = resourceServices.context;
	if (resourceContext.device == nullptr || resourceContext.core == nullptr)
	{
		return false;
	}

	const uint64_t oldSize = resource.size;
	const uint64_t grownSize = GetNRIGrownBufferSize(resource.size, requiredSize, stride);
	if (resource.buffer != nullptr || resource.shaderView != nullptr)
	{
		const auto waitStart = std::chrono::steady_clock::now();
		resourceServices.WaitForCommands(waitReason);
		renderer.mLastPerfShellTraceStats.sceneDataPreGrowWaitMs += DurationMs(waitStart, std::chrono::steady_clock::now());
	}
	resourceServices.DestroyBufferResource(resource);

	nri::BufferDesc desc = {};
	desc.size = std::max<uint64_t>(grownSize, stride);
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
	resource.usedSize = 0;
	resource.stride = stride;
	resource.usage = desc.usage;

	nri::BufferViewDesc viewDesc = {};
	viewDesc.buffer = resource.buffer;
	viewDesc.type = nri::BufferView::STRUCTURED_BUFFER;
	viewDesc.offset = 0;
	viewDesc.size = nri::WHOLE_SIZE;
	viewDesc.structureStride = stride;
	if (resourceContext.core->CreateBufferView(viewDesc, resource.shaderView) != nri::Result::SUCCESS)
	{
		resourceServices.DestroyBufferResource(resource);
		return false;
	}

	stats.growthCount++;
	stats.growEventsLastFrame = 1;
	stats.growthOldBytesLastFrame = oldSize;
	stats.growthRequestedBytesLastFrame = requiredSize;
	stats.growthAllocatedBytesLastFrame = desc.size;
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
		(resource.memoryLocation != nri::MemoryLocation::DEVICE_UPLOAD &&
		 resource.memoryLocation != nri::MemoryLocation::HOST_UPLOAD) ||
		data == nullptr ||
		size == 0 ||
		byteOffset > resource.size ||
		size > resource.size - byteOffset)
	{
		return false;
	}

	const NRIResourceServices resourceServices = renderer.BuildResourceServices();
	const NRIResourceContext& resourceContext = resourceServices.context;
	void* mapped = resourceContext.core->MapBuffer(*resource.buffer, byteOffset, size);
	if (mapped == nullptr)
	{
		return false;
	}

	std::memcpy(mapped, data, (size_t)size);
	resourceContext.core->UnmapBuffer(*resource.buffer);

	// See CreateStructuredBuffer: mapped upload buffers are not transitioned.
	(void)after;

	return true;
}
