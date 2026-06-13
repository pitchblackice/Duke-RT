#include "nri_scene_upload.h"

#include "nri_renderer.h"
#include "../system/nri_renderdevice.h"

#include <algorithm>
#include <cstring>

namespace
{
	struct NRIReprojectionData
	{
		float currentViewToClip[16] = {};
		float previousViewToClip[16] = {};
		float currentWorldToView[16] = {};
		float previousWorldToView[16] = {};
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

	static bool StructuredBufferUpdateNeedsWait(
		const NRIBufferResource& resource,
		const void* data,
		uint64_t size,
		uint32_t stride)
	{
		const uint64_t requiredSize = std::max<uint64_t>(size, stride);
		const bool needsGrowth =
			resource.buffer == nullptr ||
			resource.shaderView == nullptr ||
			resource.stride != stride ||
			resource.size < requiredSize;
		if (needsGrowth)
		{
			return resource.buffer != nullptr || resource.shaderView != nullptr;
		}

		return data != nullptr && size != 0;
	}
}

bool NRISceneUploadManager::SceneDataDescriptorsReady(NRIRenderer& renderer)
{
	if (!renderer.IsCurrentSceneDataDescriptorsInitialized() || renderer.GetCurrentSceneDataSet() == nullptr)
	{
		return false;
	}

	for (const nri::Descriptor* descriptor : renderer.mSceneDataDescriptors)
	{
		if (descriptor == nullptr)
		{
			return false;
		}
	}

	return true;
}

bool NRISceneUploadManager::UpdateSceneDataDescriptorSlot(
	NRIRenderer& renderer,
	uint32_t slot,
	nri::Descriptor* descriptor,
	const char* reason)
{
	if (slot >= renderer.mSceneDataDescriptors.size() ||
		renderer.mSceneDataDescriptors[slot] == descriptor)
	{
		return true;
	}

	renderer.mSceneDataDescriptors[slot] = descriptor;
	if (SceneDataDescriptorsReady(renderer))
	{
		return renderer.CommitSceneDataDescriptors(reason);
	}

	return true;
}

bool NRISceneUploadManager::WaitIfStructuredUpdateNeedsIt(
	NRIRenderer& renderer,
	NRIBufferResource& resource,
	const void* data,
	uint64_t size,
	uint32_t stride,
	bool* ioWaitedForWrites)
{
	if (ioWaitedForWrites != nullptr &&
		!*ioWaitedForWrites &&
		StructuredBufferUpdateNeedsWait(resource, data, size, stride))
	{
		renderer.WaitForCommandsTracked("scene_data_upload");
		*ioWaitedForWrites = true;
	}

	return true;
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
	const NRIResourceServices resourceServices = renderer.BuildResourceServices();
	const NRIResourceContext& resourceContext = resourceServices.context;

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
			resourceServices.WaitForCommands(waitReason);
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

	const NRIResourceServices resourceServices = renderer.BuildResourceServices();
	const NRIResourceContext& resourceContext = resourceServices.context;
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

bool NRISceneUploadManager::UpdateReprojectionBuffer(NRIRenderer& renderer, bool* ioWaitedForWrites)
{
	NRIReprojectionData data = {};
	std::memcpy(data.currentViewToClip, renderer.mCurrentViewToClip, sizeof(data.currentViewToClip));
	std::memcpy(data.previousViewToClip, renderer.mPreviousViewToClip, sizeof(data.previousViewToClip));
	std::memcpy(data.currentWorldToView, renderer.mCurrentWorldToView, sizeof(data.currentWorldToView));
	std::memcpy(data.previousWorldToView, renderer.mPreviousWorldToView, sizeof(data.previousWorldToView));

	WaitIfStructuredUpdateNeedsIt(renderer, renderer.mReprojectionBuffer, &data, sizeof(data), sizeof(data), ioWaitedForWrites);
	if (!renderer.EnsureStructuredBuffer(
		renderer.mReprojectionBuffer,
		renderer.mReprojectionBufferStats,
		&data,
		sizeof(data),
		sizeof(data),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess(),
		ioWaitedForWrites != nullptr && *ioWaitedForWrites,
		"scene_data_upload"))
	{
		return false;
	}

	return UpdateSceneDataDescriptorSlot(renderer, 18, renderer.mReprojectionBuffer.shaderView, "reprojection_refresh");
}

bool NRISceneUploadManager::UpdateVisibleChunkBuffer(NRIRenderer& renderer, bool* ioWaitedForWrites)
{
	const uint32_t defaultVisibleChunkWord = 0u;
	const void* visibleChunkData = renderer.mCurrentVisibleChunkWords.empty() ? (const void*)&defaultVisibleChunkWord : renderer.mCurrentVisibleChunkWords.data();
	const size_t visibleChunkSize = renderer.mCurrentVisibleChunkWords.empty() ? sizeof(uint32_t) : renderer.mCurrentVisibleChunkWords.size() * sizeof(uint32_t);

	WaitIfStructuredUpdateNeedsIt(renderer, renderer.mVisibleChunkBuffer, visibleChunkData, visibleChunkSize, sizeof(uint32_t), ioWaitedForWrites);
	if (!renderer.EnsureStructuredBuffer(
		renderer.mVisibleChunkBuffer,
		renderer.mVisibleChunkBufferStats,
		visibleChunkData,
		visibleChunkSize,
		sizeof(uint32_t),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess(),
		ioWaitedForWrites != nullptr && *ioWaitedForWrites,
		"scene_data_upload"))
	{
		return false;
	}

	return UpdateSceneDataDescriptorSlot(renderer, 19, renderer.mVisibleChunkBuffer.shaderView, "visible_chunk_refresh");
}

bool NRISceneUploadManager::UpdateVisibleFlatPlaneBuffer(NRIRenderer& renderer, bool* ioWaitedForWrites)
{
	const uint32_t defaultVisibleFlatPlaneWord = 0u;
	const void* visibleFlatPlaneData = renderer.mCurrentVisibleFlatPlaneWords.empty() ? (const void*)&defaultVisibleFlatPlaneWord : renderer.mCurrentVisibleFlatPlaneWords.data();
	const size_t visibleFlatPlaneSize = renderer.mCurrentVisibleFlatPlaneWords.empty() ? sizeof(uint32_t) : renderer.mCurrentVisibleFlatPlaneWords.size() * sizeof(uint32_t);

	WaitIfStructuredUpdateNeedsIt(renderer, renderer.mVisibleFlatPlaneBuffer, visibleFlatPlaneData, visibleFlatPlaneSize, sizeof(uint32_t), ioWaitedForWrites);
	if (!renderer.EnsureStructuredBuffer(
		renderer.mVisibleFlatPlaneBuffer,
		renderer.mVisibleFlatPlaneBufferStats,
		visibleFlatPlaneData,
		visibleFlatPlaneSize,
		sizeof(uint32_t),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess(),
		ioWaitedForWrites != nullptr && *ioWaitedForWrites,
		"scene_data_upload"))
	{
		return false;
	}

	return UpdateSceneDataDescriptorSlot(renderer, 20, renderer.mVisibleFlatPlaneBuffer.shaderView, "visible_flat_refresh");
}

NRIResourceContext NRIRenderer::BuildResourceContext() const
{
	NRIResourceContext context = {};
	context.device = mFrameBuffer != nullptr ? mFrameBuffer->mDevice : nullptr;
	context.core = mFrameBuffer != nullptr ? &mFrameBuffer->mCore : nullptr;
	context.commandBuffer = mFrameBuffer != nullptr ? mFrameBuffer->mCommandBuffer : nullptr;
	return context;
}

NRIResourceServices NRIRenderer::BuildResourceServices()
{
	NRIResourceServices services = {};
	services.context = BuildResourceContext();
	services.user = this;
	services.waitForCommands = [](void* user, const char* reason)
	{
		static_cast<NRIRenderer*>(user)->WaitForCommandsTracked(reason);
	};
	services.destroyBufferResource = [](void* user, NRIBufferResource& resource)
	{
		static_cast<NRIRenderer*>(user)->DestroyBufferResource(resource);
	};
	return services;
}

bool NRIRenderer::CreateStructuredBuffer(NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after)
{
	return NRISceneUploadManager::CreateStructuredBuffer(*this, resource, data, size, stride, usage, after);
}

bool NRIRenderer::EnsureStructuredBuffer(NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, bool writesQuiesced, const char* waitReason)
{
	return NRISceneUploadManager::EnsureStructuredBuffer(*this, resource, stats, data, size, stride, usage, after, writesQuiesced, waitReason);
}

bool NRIRenderer::UpdateStructuredBufferRange(NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after)
{
	return NRISceneUploadManager::UpdateStructuredBufferRange(*this, resource, byteOffset, data, size, after);
}

bool NRIRenderer::CreateBufferWithoutView(NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage)
{
	if (resource.buffer != nullptr)
	{
		WaitForCommandsTracked();
	}

	return CreateBufferWithoutViewAtLocation(resource, size, stride, usage, nri::MemoryLocation::DEVICE);
}

bool NRIRenderer::CreateBufferWithoutViewAtLocation(NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::MemoryLocation memoryLocation)
{
	const NRIResourceContext resourceContext = BuildResourceContext();
	DestroyBufferResource(resource);

	nri::BufferDesc desc = {};
	desc.size = std::max<uint64_t>(size, stride);
	desc.structureStride = stride;
	desc.usage = usage;
	if (resourceContext.core->CreateCommittedBuffer(*resourceContext.device, memoryLocation, 0.0f, desc, resource.buffer) != nri::Result::SUCCESS)
	{
		return false;
	}

	nri::MemoryDesc memoryDesc = {};
	resourceContext.core->GetBufferMemoryDesc(*resource.buffer, memoryLocation, memoryDesc);
	resource.size = desc.size;
	resource.memorySize = memoryDesc.size;
	resource.usedSize = size;
	resource.stride = stride;
	resource.memoryLocation = memoryLocation;
	return true;
}

bool NRIRenderer::EnsureResidentArenaBuffer(NRIBufferResource& resource, uint64_t requiredSize, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after)
{
	const uint64_t alignedRequiredSize = std::max<uint64_t>(requiredSize, stride);
	if (resource.buffer != nullptr &&
		resource.shaderView != nullptr &&
		resource.memoryLocation == nri::MemoryLocation::DEVICE &&
		resource.stride == stride &&
		resource.size >= alignedRequiredSize)
	{
		resource.usedSize = std::max(resource.usedSize, requiredSize);
		return true;
	}

	NRIBufferResource oldResource = resource;
	resource = {};

	const uint64_t grownSize = GetNRIGrownBufferSize(oldResource.size, alignedRequiredSize, stride);
	if (!CreateBufferWithoutViewAtLocation(resource, grownSize, stride, usage, nri::MemoryLocation::DEVICE))
	{
		resource = oldResource;
		return false;
	}

	nri::BufferViewDesc viewDesc = {};
	viewDesc.buffer = resource.buffer;
	viewDesc.type = nri::BufferView::STRUCTURED_BUFFER;
	viewDesc.offset = 0;
	viewDesc.size = nri::WHOLE_SIZE;
	viewDesc.structureStride = stride;
	if (mFrameBuffer->mCore.CreateBufferView(viewDesc, resource.shaderView) != nri::Result::SUCCESS)
	{
		DestroyBufferResource(resource);
		resource = oldResource;
		return false;
	}
	resource.usedSize = requiredSize;

	if (oldResource.buffer != nullptr && mFrameBuffer->mCommandBuffer != nullptr)
	{
		const uint64_t copySize = std::min(oldResource.usedSize, resource.size);
		if (copySize > 0)
		{
			nri::BufferBarrierDesc beforeBarriers[2] = {};
			beforeBarriers[0].buffer = oldResource.buffer;
			beforeBarriers[0].before = after;
			beforeBarriers[0].after = NRICopySourceAccess();
			beforeBarriers[1].buffer = resource.buffer;
			beforeBarriers[1].before = {};
			beforeBarriers[1].after = NRICopyDestinationAccess();
			nri::BarrierDesc beforeBarrierDesc = {};
			beforeBarrierDesc.buffers = beforeBarriers;
			beforeBarrierDesc.bufferNum = 2;
			mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, beforeBarrierDesc);

			mFrameBuffer->mCore.CmdCopyBuffer(
				*mFrameBuffer->mCommandBuffer,
				*resource.buffer,
				0,
				*oldResource.buffer,
				0,
				copySize);

			nri::BufferBarrierDesc afterBarrier = {};
			afterBarrier.buffer = resource.buffer;
			afterBarrier.before = NRICopyDestinationAccess();
			afterBarrier.after = after;
			nri::BarrierDesc afterBarrierDesc = {};
			afterBarrierDesc.buffers = &afterBarrier;
			afterBarrierDesc.bufferNum = 1;
			mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, afterBarrierDesc);
		}

		auto& frameScratch = GetResidentUploadScratchFrame();
		frameScratch.retiredBuffers.push_back(oldResource);
	}
	else if (oldResource.buffer != nullptr || oldResource.shaderView != nullptr)
	{
		DestroyBufferResource(oldResource);
	}

	return true;
}

bool NRIRenderer::EnsureResidentUploadScratchBuffer(ResidentBufferUploadScratch& scratch, ResidentUploadScratchFrame& frameScratch, uint64_t requiredSize)
{
	constexpr uint32_t kResidentUploadScratchStride = 16u;
	const uint64_t alignedRequiredSize = std::max<uint64_t>(requiredSize, kResidentUploadScratchStride);
	if (scratch.buffer.buffer != nullptr &&
		scratch.buffer.memoryLocation == nri::MemoryLocation::DEVICE_UPLOAD &&
		scratch.buffer.size >= alignedRequiredSize)
	{
		return true;
	}

	const uint64_t grownSize = GetNRIGrownBufferSize(scratch.buffer.size, alignedRequiredSize, kResidentUploadScratchStride);
	if (scratch.buffer.buffer != nullptr || scratch.buffer.shaderView != nullptr)
	{
		frameScratch.retiredBuffers.push_back(scratch.buffer);
		scratch.buffer = {};
		scratch.cursor = 0;
		scratch.copySourceActive = false;
	}
	const bool created = CreateBufferWithoutViewAtLocation(
		scratch.buffer,
		grownSize,
		kResidentUploadScratchStride,
		nri::BufferUsageBits::NONE,
		nri::MemoryLocation::DEVICE_UPLOAD);
	if (created)
	{
		mLastPerfShellTraceStats.runtimeMutationResidentApplyStageScratchGrowCount++;
		mLastPerfShellTraceStats.runtimeMutationResidentApplyStageScratchGrowBytes += grownSize;
	}
	return created;
}

bool NRIRenderer::StageResidentBufferCopyRange(NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after, int uploadKind)
{
	if (resource.buffer == nullptr ||
		data == nullptr ||
		size == 0 ||
		byteOffset > resource.size ||
		size > resource.size - byteOffset)
	{
		return false;
	}

	if (mFrameBuffer == nullptr || mFrameBuffer->mCommandBuffer == nullptr)
	{
		return false;
	}

	constexpr uint64_t kResidentUploadScratchAlignment = 16u;
	auto& frameScratch = GetResidentUploadScratchFrame();

	ResidentBufferUploadScratch* scratch = nullptr;
	switch (uploadKind)
	{
	case ResidentUploadKind_Vertex: scratch = &frameScratch.vertex; break;
	case ResidentUploadKind_Index: scratch = &frameScratch.index; break;
	case ResidentUploadKind_Primitive: scratch = &frameScratch.primitive; break;
	case ResidentUploadKind_Material: scratch = &frameScratch.material; break;
	default: return false;
	}

	const uint64_t scratchOffset =
		(scratch->cursor + kResidentUploadScratchAlignment - 1u) &
		~(kResidentUploadScratchAlignment - 1u);
	const uint64_t requiredSize = scratchOffset + size;
	if (!EnsureResidentUploadScratchBuffer(*scratch, frameScratch, requiredSize))
	{
		return false;
	}

	void* mapped = mFrameBuffer->mCore.MapBuffer(*scratch->buffer.buffer, scratchOffset, size);
	if (mapped == nullptr)
	{
		return false;
	}

	std::memcpy(mapped, data, (size_t)size);
	mFrameBuffer->mCore.UnmapBuffer(*scratch->buffer.buffer);

	if (!scratch->copySourceActive)
	{
		nri::BufferBarrierDesc sourceBarrier = {};
		sourceBarrier.buffer = scratch->buffer.buffer;
		sourceBarrier.before = {};
		sourceBarrier.after = NRICopySourceAccess();

		nri::BarrierDesc sourceBarrierDesc = {};
		sourceBarrierDesc.buffers = &sourceBarrier;
		sourceBarrierDesc.bufferNum = 1;
		mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, sourceBarrierDesc);
		scratch->copySourceActive = true;
	}

	nri::BufferBarrierDesc beforeCopyBarrier = {};
	beforeCopyBarrier.buffer = resource.buffer;
	beforeCopyBarrier.before = after;
	beforeCopyBarrier.after = NRICopyDestinationAccess();

	nri::BarrierDesc beforeCopyBarrierDesc = {};
	beforeCopyBarrierDesc.buffers = &beforeCopyBarrier;
	beforeCopyBarrierDesc.bufferNum = 1;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, beforeCopyBarrierDesc);

	mFrameBuffer->mCore.CmdCopyBuffer(
		*mFrameBuffer->mCommandBuffer,
		*resource.buffer,
		byteOffset,
		*scratch->buffer.buffer,
		scratchOffset,
		size);

	nri::BufferBarrierDesc afterCopyBarrier = {};
	afterCopyBarrier.buffer = resource.buffer;
	afterCopyBarrier.before = NRICopyDestinationAccess();
	afterCopyBarrier.after = after;

	nri::BarrierDesc afterCopyBarrierDesc = {};
	afterCopyBarrierDesc.buffers = &afterCopyBarrier;
	afterCopyBarrierDesc.bufferNum = 1;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, afterCopyBarrierDesc);

	scratch->cursor = scratchOffset + size;
	return true;
}

void NRIRenderer::RetireResidentBufferResource(NRIBufferResource& resource)
{
	if (resource.buffer == nullptr && resource.shaderView == nullptr)
	{
		return;
	}

	if (mFrameBuffer != nullptr &&
		mFrameBuffer->mCommandBuffer != nullptr &&
		!mResidentUploadScratchFrames.empty())
	{
		auto& frameScratch = GetResidentUploadScratchFrame();
		frameScratch.retiredBuffers.push_back(resource);
		resource = {};
		return;
	}

	DestroyBufferResource(resource);
}

void NRIRenderer::RetireResidentAccelerationStructure(NRIAccelerationStructureResource& resource)
{
	if (resource.accelerationStructure == nullptr && resource.descriptor == nullptr)
	{
		return;
	}

	if (mFrameBuffer == nullptr ||
		mFrameBuffer->mCommandBuffer == nullptr ||
		mResidentUploadScratchFrames.empty())
	{
		DestroyAccelerationStructureResource(resource);
		return;
	}

	auto& frameScratch = GetResidentUploadScratchFrame();
	frameScratch.retiredAccelerationStructures.push_back(resource);
	resource = {};
}

void NRIRenderer::RetireTopLevelAccelerationStructure(NRIAccelerationStructureResource& resource)
{
	if (resource.accelerationStructure == nullptr && resource.descriptor == nullptr)
	{
		return;
	}

	RetireResidentAccelerationStructure(resource);
}

bool NRIRenderer::EnsureResidentStructuredBuffer(NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, const char* waitReason, int uploadKind)
{
	const uint64_t requiredSize = std::max<uint64_t>(size, stride);
	const bool needsGrowth =
		resource.buffer == nullptr ||
		resource.shaderView == nullptr ||
		resource.memoryLocation != nri::MemoryLocation::DEVICE ||
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
	NotePerfBufferUpload(&stats, size, needsGrowth, waitReason, uploadKind);

	if (needsGrowth)
	{
		const uint64_t oldSize = resource.size;
		const uint64_t grownSize = GetNRIGrownBufferSize(resource.size, requiredSize, stride);
		NRIBufferResource oldResource = resource;
		NRIBufferResource newResource = {};
		if (!CreateBufferWithoutViewAtLocation(newResource, grownSize, stride, usage, nri::MemoryLocation::DEVICE))
		{
			return false;
		}

		nri::BufferViewDesc viewDesc = {};
		viewDesc.buffer = newResource.buffer;
		viewDesc.type = nri::BufferView::STRUCTURED_BUFFER;
		viewDesc.offset = 0;
		viewDesc.size = nri::WHOLE_SIZE;
		viewDesc.structureStride = stride;
		if (mFrameBuffer->mCore.CreateBufferView(viewDesc, newResource.shaderView) != nri::Result::SUCCESS)
		{
			DestroyBufferResource(newResource);
			return false;
		}

		newResource.usedSize = size;
		if (data != nullptr && size != 0)
		{
			if (!StageResidentBufferCopyRange(newResource, 0, data, size, after, uploadKind))
			{
				DestroyBufferResource(newResource);
				return false;
			}
		}

		resource = newResource;
		RetireResidentBufferResource(oldResource);
		stats.growthCount++;
		stats.growEventsLastFrame = 1;
		stats.growthOldBytesLastFrame = oldSize;
		stats.growthRequestedBytesLastFrame = requiredSize;
		stats.growthAllocatedBytesLastFrame = grownSize;
		return true;
	}
	else
	{
		stats.overwriteCount++;
		stats.overwriteEventsLastFrame = 1;
	}

	resource.usedSize = size;
	if (data != nullptr && size != 0)
	{
		if (!StageResidentBufferCopyRange(resource, 0, data, size, after, uploadKind))
		{
			return false;
		}
	}

	return true;
}
