#include "nri_indirect_radiance_cache.h"

#include "nri_shader_contracts.h"
#include "nri_renderer.h"
#include "../system/nri_renderdevice.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace
{
	uint64_t HashCombine(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
	}

	uint32_t NormalizeEntryCount(uint32_t requested)
	{
		requested = std::clamp(
			requested,
			NRIIndirectRadianceCache::MinimumEntryCount,
			NRIIndirectRadianceCache::MaximumEntryCount);
		uint32_t result = NRIIndirectRadianceCache::MinimumEntryCount;
		while (result < requested && result < NRIIndirectRadianceCache::MaximumEntryCount)
		{
			result <<= 1u;
		}
		return result;
	}

	bool CreateBuffer(
		const NRIResourceServices& services,
		NRIBufferResource& resource,
		uint64_t size,
		uint32_t stride,
		nri::BufferUsageBits usage,
		nri::MemoryLocation memoryLocation,
		bool storageView)
	{
		const NRIResourceContext& context = services.context;
		if (context.device == nullptr || context.core == nullptr || size == 0 || stride == 0)
		{
			return false;
		}

		services.DestroyBufferResource(resource);
		nri::BufferDesc desc = {};
		desc.size = size;
		desc.structureStride = stride;
		desc.usage = usage;
		if (context.core->CreateCommittedBuffer(*context.device, memoryLocation, 0.0f, desc, resource.buffer) != nri::Result::SUCCESS)
		{
			return false;
		}

		nri::MemoryDesc memoryDesc = {};
		context.core->GetBufferMemoryDesc(*resource.buffer, memoryLocation, memoryDesc);
		resource.size = size;
		resource.memorySize = memoryDesc.size;
		resource.usedSize = size;
		resource.stride = stride;
		resource.usage = usage;
		resource.memoryLocation = memoryLocation;

		if (!storageView)
		{
			return true;
		}

		nri::BufferViewDesc view = {};
		view.buffer = resource.buffer;
		view.type = nri::BufferView::STORAGE_STRUCTURED_BUFFER;
		view.offset = 0;
		view.size = nri::WHOLE_SIZE;
		view.structureStride = stride;
		if (context.core->CreateBufferView(view, resource.storageView) != nri::Result::SUCCESS)
		{
			services.DestroyBufferResource(resource);
			return false;
		}
		return true;
	}

	template<typename Slots>
	uint32_t CountPendingReadbacks(const Slots& slots)
	{
		uint32_t count = 0;
		for (const auto& slot : slots)
		{
			count += slot.pending ? 1u : 0u;
		}
		return count;
	}
}

NRIIndirectRadianceCacheCompatibilitySnapshot BuildNRIIndirectRadianceCacheCompatibilitySnapshot(
	const NRIIndirectRadianceCacheCompatibilityInput& input)
{
	NRIIndirectRadianceCacheCompatibilitySnapshot snapshot = {};
	snapshot.valid = input.valid;
	snapshot.input = input;
	uint64_t hash = 1469598103934665603ull;
	hash = HashCombine(hash, input.mapIdentity);
	hash = HashCombine(hash, input.staticSceneIdentity);
	hash = HashCombine(hash, input.portalRouteIdentity);
	hash = HashCombine(hash, input.materialIdentity);
	hash = HashCombine(hash, input.mutationIdentity);
	hash = HashCombine(hash, input.voxelOccurrenceIdentity);
	hash = HashCombine(hash, input.lightingIdentity);
	snapshot.hash = hash;
	return snapshot;
}

uint32_t CompareNRIIndirectRadianceCacheCompatibility(
	const NRIIndirectRadianceCacheCompatibilitySnapshot& previous,
	const NRIIndirectRadianceCacheCompatibilitySnapshot& current)
{
	if (!current.valid)
	{
		return NRI_INDIRECT_RADIANCE_CACHE_INVALID_INPUT;
	}
	if (!previous.valid)
	{
		return NRI_INDIRECT_RADIANCE_CACHE_INVALID_FIRST_USE;
	}

	uint32_t result = NRI_INDIRECT_RADIANCE_CACHE_INVALID_NONE;
	if (previous.input.mapIdentity != current.input.mapIdentity)
		result |= NRI_INDIRECT_RADIANCE_CACHE_INVALID_MAP;
	if (previous.input.staticSceneIdentity != current.input.staticSceneIdentity)
		result |= NRI_INDIRECT_RADIANCE_CACHE_INVALID_STATIC_SCENE;
	if (previous.input.portalRouteIdentity != current.input.portalRouteIdentity)
		result |= NRI_INDIRECT_RADIANCE_CACHE_INVALID_PORTAL_ROUTE;
	if (previous.input.materialIdentity != current.input.materialIdentity)
		result |= NRI_INDIRECT_RADIANCE_CACHE_INVALID_MATERIAL;
	if (previous.input.mutationIdentity != current.input.mutationIdentity)
		result |= NRI_INDIRECT_RADIANCE_CACHE_INVALID_MUTATION;
	if (previous.input.voxelOccurrenceIdentity != current.input.voxelOccurrenceIdentity)
		result |= NRI_INDIRECT_RADIANCE_CACHE_INVALID_VOXEL_OCCURRENCE;
	if (previous.input.lightingIdentity != current.input.lightingIdentity)
		result |= NRI_INDIRECT_RADIANCE_CACHE_INVALID_LIGHTING;
	return result;
}

uint64_t NRIIndirectRadianceCacheFenceServices::GetRecordingCommandFenceValue() const
{
	return getRecordingCommandFenceValue != nullptr ? getRecordingCommandFenceValue(user) : 0;
}

bool NRIIndirectRadianceCacheFenceServices::IsCommandFenceValueComplete(uint64_t fenceValue) const
{
	return fenceValue != 0 && isCommandFenceValueComplete != nullptr &&
		isCommandFenceValueComplete(user, fenceValue);
}

bool NRIIndirectRadianceCacheFenceServices::IsCommandFenceValueAbandoned(uint64_t fenceValue) const
{
	return fenceValue != 0 && isCommandFenceValueAbandoned != nullptr &&
		isCommandFenceValueAbandoned(user, fenceValue);
}

NRIIndirectRadianceCacheServices BuildNRIIndirectRadianceCacheServices(NRIRenderer& renderer)
{
	NRIIndirectRadianceCacheServices services = {};
	services.resources = renderer.BuildResourceServices();
	services.descriptorPool = renderer.mFrameBuffer != nullptr ? renderer.mFrameBuffer->GetDescriptorPool() : nullptr;
	services.pipelineLayout = renderer.mIndirectRadianceCachePipelineLayout;
	services.fallbackStorageDescriptor = renderer.mTraceShaderStats.Descriptor();
	services.fences.user = renderer.mFrameBuffer;
	services.fences.getRecordingCommandFenceValue = [](void* user) -> uint64_t
	{
		return user != nullptr ? static_cast<NRIRenderDevice*>(user)->GetRecordingCommandFenceValue() : 0;
	};
	services.fences.isCommandFenceValueComplete = [](void* user, uint64_t fenceValue) -> bool
	{
		return user != nullptr && static_cast<NRIRenderDevice*>(user)->IsCommandFenceValueComplete(fenceValue);
	};
	services.fences.isCommandFenceValueAbandoned = [](void* user, uint64_t fenceValue) -> bool
	{
		return user != nullptr && static_cast<NRIRenderDevice*>(user)->IsCommandFenceValueAbandoned(fenceValue);
	};
	return services;
}

bool NRIIndirectRadianceCache::EnsureResources(
	const NRIIndirectRadianceCacheServices& services,
	uint32_t requestedEntryCount)
{
	const NRIResourceContext& context = services.resources.context;
	if (context.device == nullptr || context.core == nullptr || services.descriptorPool == nullptr ||
		services.pipelineLayout == nullptr || services.fallbackStorageDescriptor == nullptr)
	{
		return false;
	}

	const uint32_t entryCount = NormalizeEntryCount(requestedEntryCount);
	const uint64_t tableBytes = (uint64_t)entryCount * NRI_INDIRECT_RADIANCE_CACHE_RECORD_STRIDE;
	const uint64_t telemetryBytes = (uint64_t)NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_COUNT * sizeof(uint32_t);
	bool descriptorsDirty = false;
	const bool resourcesReady =
		mEntryCount == entryCount &&
		mTables[0].buffer != nullptr && mTables[0].storageView != nullptr &&
		mTables[1].buffer != nullptr && mTables[1].storageView != nullptr &&
		mTelemetryBuffer.buffer != nullptr && mTelemetryBuffer.storageView != nullptr &&
		mClearUploadBuffer.buffer != nullptr;
	if (!resourcesReady)
	{
		const std::array<nri::DescriptorSet*, 2> retainedDescriptorSets = mDescriptorSets;
		if (IsAllocated())
		{
			services.resources.WaitForCommands("indirect_radiance_cache_resize");
		}
		Destroy(services.resources);
		mDescriptorSets = retainedDescriptorSets;
		const nri::BufferUsageBits storageUsage = nri::BufferUsageBits::SHADER_RESOURCE_STORAGE;
		if (!CreateBuffer(services.resources, mTables[0], tableBytes,
				NRI_INDIRECT_RADIANCE_CACHE_RECORD_STRIDE, storageUsage, nri::MemoryLocation::DEVICE, true) ||
			!CreateBuffer(services.resources, mTables[1], tableBytes,
				NRI_INDIRECT_RADIANCE_CACHE_RECORD_STRIDE, storageUsage, nri::MemoryLocation::DEVICE, true) ||
			!CreateBuffer(services.resources, mTelemetryBuffer, telemetryBytes,
				sizeof(uint32_t), storageUsage, nri::MemoryLocation::DEVICE, true) ||
			!CreateBuffer(services.resources, mClearUploadBuffer, tableBytes,
				sizeof(uint32_t), nri::BufferUsageBits::NONE, nri::MemoryLocation::DEVICE_UPLOAD, false))
		{
			const std::array<nri::DescriptorSet*, 2> retainedDescriptorSets = mDescriptorSets;
			Destroy(services.resources);
			mDescriptorSets = retainedDescriptorSets;
			return false;
		}

		void* clearData = context.core->MapBuffer(*mClearUploadBuffer.buffer, 0, tableBytes);
		if (clearData == nullptr)
		{
			const std::array<nri::DescriptorSet*, 2> retainedDescriptorSets = mDescriptorSets;
			Destroy(services.resources);
			mDescriptorSets = retainedDescriptorSets;
			return false;
		}
		std::memset(clearData, 0, (size_t)tableBytes);
		context.core->UnmapBuffer(*mClearUploadBuffer.buffer);

		for (ReadbackSlot& slot : mReadbackSlots)
		{
			if (!CreateBuffer(services.resources, slot.buffer, telemetryBytes,
					sizeof(uint32_t), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, false))
			{
				const std::array<nri::DescriptorSet*, 2> retainedDescriptorSets = mDescriptorSets;
				Destroy(services.resources);
				mDescriptorSets = retainedDescriptorSets;
				return false;
			}
		}
		mEntryCount = entryCount;
		mClearPending = true;
		descriptorsDirty = true;
	}

	if (mDescriptorSets[0] == nullptr || mDescriptorSets[1] == nullptr)
	{
		if (context.core->AllocateDescriptorSets(
			*services.descriptorPool,
			*services.pipelineLayout,
			NRI_INDIRECT_RADIANCE_CACHE_SET_INDEX,
			mDescriptorSets.data(),
			(uint32_t)mDescriptorSets.size(),
			0) != nri::Result::SUCCESS)
		{
			Destroy(services.resources);
			return false;
		}
		descriptorsDirty = true;
	}

	return !descriptorsDirty || UpdateDescriptorSets(services, true);
}

bool NRIIndirectRadianceCache::UpdateDescriptorSets(
	const NRIIndirectRadianceCacheServices& services,
	bool useCacheTables)
{
	const NRIResourceContext& context = services.resources.context;
	if (context.core == nullptr || mDescriptorSets[0] == nullptr || mDescriptorSets[1] == nullptr ||
		services.fallbackStorageDescriptor == nullptr)
	{
		return false;
	}

	const nri::Descriptor* fallback = services.fallbackStorageDescriptor;
	std::array<std::array<const nri::Descriptor*, NRI_INDIRECT_RADIANCE_CACHE_DESCRIPTOR_NUM>, 2> descriptors = {};
	std::array<nri::UpdateDescriptorRangeDesc, 2> updates = {};
	for (uint32_t readIndex = 0; readIndex < 2u; ++readIndex)
	{
		descriptors[readIndex] = {
			useCacheTables && mTables[readIndex].storageView != nullptr ? mTables[readIndex].storageView : fallback,
			useCacheTables && mTables[1u - readIndex].storageView != nullptr ? mTables[1u - readIndex].storageView : fallback,
			useCacheTables && mTelemetryBuffer.storageView != nullptr ? mTelemetryBuffer.storageView : fallback,
		};
		updates[readIndex].descriptorSet = mDescriptorSets[readIndex];
		updates[readIndex].rangeIndex = 0;
		updates[readIndex].descriptors = descriptors[readIndex].data();
		updates[readIndex].descriptorNum = NRI_INDIRECT_RADIANCE_CACHE_DESCRIPTOR_NUM;
	}
	context.core->UpdateDescriptorRanges(updates.data(), (uint32_t)updates.size());
	return true;
}

NRIIndirectRadianceCachePrepareResult NRIIndirectRadianceCache::Prepare(
	const NRIIndirectRadianceCacheServices& services,
	bool enabled,
	const NRIIndirectRadianceCacheCompatibilityInput& compatibility,
	uint32_t entryCount)
{
	NRIIndirectRadianceCachePrepareResult result = {};
	ReconcileFrameCommits(services);
	if (!enabled)
	{
		mActive = false;
		return result;
	}
	if (!compatibility.valid)
	{
		mActive = false;
		mLastInvalidationMask = NRI_INDIRECT_RADIANCE_CACHE_INVALID_INPUT;
		return result;
	}

	if (!EnsureResources(services, entryCount))
	{
		mActive = false;
		return result;
	}

	const NRIIndirectRadianceCacheCompatibilitySnapshot current =
		BuildNRIIndirectRadianceCacheCompatibilitySnapshot(compatibility);
	const uint32_t invalidationMask = CompareNRIIndirectRadianceCacheCompatibility(mCompatibility, current);
	if (invalidationMask != NRI_INDIRECT_RADIANCE_CACHE_INVALID_NONE)
	{
		// Frame-tagged ping-pong entries form a logical reset after one forced-exact
		// frame: the next read table can only accept records written under the new
		// compatibility snapshot. Physically clear uninitialized storage and the
		// domains that can restart renderer frame numbering; avoid copying both
		// 12-MiB tables for ordinary light, mutation, or occurrence changes.
		const uint32_t physicalClearMask =
			NRI_INDIRECT_RADIANCE_CACHE_INVALID_FIRST_USE |
			NRI_INDIRECT_RADIANCE_CACHE_INVALID_INPUT |
			NRI_INDIRECT_RADIANCE_CACHE_INVALID_MAP |
			NRI_INDIRECT_RADIANCE_CACHE_INVALID_STATIC_SCENE;
		if (!mGpuBuffersInitialized || (invalidationMask & physicalClearMask) != 0u)
		{
			mClearPending = true;
			mReadTableIndex = 0;
		}
		mLastInvalidationMask = invalidationMask;
	}
	mCompatibility = current;
	mActive = true;

	result.active = true;
	result.clearRequired = mClearPending;
	result.invalidationMask = invalidationMask;
	result.readTableIndex = mReadTableIndex;
	result.writeTableIndex = 1u - mReadTableIndex;
	result.descriptorSet = mDescriptorSets[mReadTableIndex];
	return result;
}

bool NRIIndirectRadianceCache::RecordPendingClear(const NRIIndirectRadianceCacheServices& services)
{
	const NRIResourceContext& context = services.resources.context;
	mCurrentFrameRecordedClear = false;
	mCurrentFrameInitializedBeforeClear = false;
	if (!mActive || context.core == nullptr || context.commandBuffer == nullptr)
	{
		return false;
	}
	if (!mClearPending)
	{
		if (!mGpuBuffersInitialized || mTables[0].buffer == nullptr || mTables[1].buffer == nullptr)
		{
			return false;
		}

		// The write table from the preceding dispatch becomes this frame's read
		// table. An explicit storage-to-storage dependency is required even though
		// the resource state is unchanged (a UAV barrier on D3D12).
		std::array<nri::BufferBarrierDesc, 2> tableBarriers = {};
		for (uint32_t i = 0; i < tableBarriers.size(); ++i)
		{
			tableBarriers[i].buffer = mTables[i].buffer;
			tableBarriers[i].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
			tableBarriers[i].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		}
		nri::BarrierDesc tableBarrierDesc = {};
		tableBarrierDesc.buffers = tableBarriers.data();
		tableBarrierDesc.bufferNum = (uint32_t)tableBarriers.size();
		context.core->CmdBarrier(*context.commandBuffer, tableBarrierDesc);
		return true;
	}
	if (mClearUploadBuffer.buffer == nullptr || mTables[0].buffer == nullptr ||
		mTables[1].buffer == nullptr || mTelemetryBuffer.buffer == nullptr)
	{
		return false;
	}

	std::array<nri::BufferBarrierDesc, 4> before = {};
	before[0].buffer = mClearUploadBuffer.buffer;
	before[0].before = mGpuBuffersInitialized ? NRIResourceCopySourceAccess() : nri::AccessStage{};
	before[0].after = NRIResourceCopySourceAccess();
	for (uint32_t i = 0; i < 2; ++i)
	{
		before[1 + i].buffer = mTables[i].buffer;
		before[1 + i].before = mGpuBuffersInitialized ?
			nri::AccessStage{ nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER } :
			nri::AccessStage{};
		before[1 + i].after = NRIResourceCopyDestinationAccess();
	}
	before[3].buffer = mTelemetryBuffer.buffer;
	before[3].before = mGpuBuffersInitialized ?
		nri::AccessStage{ nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER } :
		nri::AccessStage{};
	before[3].after = NRIResourceCopyDestinationAccess();
	nri::BarrierDesc beforeDesc = {};
	beforeDesc.buffers = before.data();
	beforeDesc.bufferNum = (uint32_t)before.size();
	context.core->CmdBarrier(*context.commandBuffer, beforeDesc);

	const uint64_t tableBytes = (uint64_t)mEntryCount * NRI_INDIRECT_RADIANCE_CACHE_RECORD_STRIDE;
	const uint64_t telemetryBytes = (uint64_t)NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_COUNT * sizeof(uint32_t);
	for (NRIBufferResource& table : mTables)
	{
		context.core->CmdCopyBuffer(*context.commandBuffer, *table.buffer, 0, *mClearUploadBuffer.buffer, 0, tableBytes);
	}
	context.core->CmdCopyBuffer(
		*context.commandBuffer,
		*mTelemetryBuffer.buffer,
		0,
		*mClearUploadBuffer.buffer,
		0,
		telemetryBytes);

	std::array<nri::BufferBarrierDesc, 3> after = {};
	for (uint32_t i = 0; i < 2; ++i)
	{
		after[i].buffer = mTables[i].buffer;
		after[i].before = NRIResourceCopyDestinationAccess();
		after[i].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	}
	after[2].buffer = mTelemetryBuffer.buffer;
	after[2].before = NRIResourceCopyDestinationAccess();
	after[2].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	nri::BarrierDesc afterDesc = {};
	afterDesc.buffers = after.data();
	afterDesc.bufferNum = (uint32_t)after.size();
	context.core->CmdBarrier(*context.commandBuffer, afterDesc);

	mCurrentFrameRecordedClear = true;
	mCurrentFrameInitializedBeforeClear = mGpuBuffersInitialized;
	mGpuBuffersInitialized = true;
	mClearPending = false;
	return true;
}

void NRIIndirectRadianceCache::AdvanceFrame(const NRIIndirectRadianceCacheServices& services)
{
	if (!mActive || mClearPending)
	{
		return;
	}
	const uint64_t fenceValue = services.fences.GetRecordingCommandFenceValue();
	auto slotIt = std::find_if(mFrameCommitSlots.begin(), mFrameCommitSlots.end(),
		[](const FrameCommitSlot& slot) { return !slot.pending; });
	if (fenceValue == 0 || slotIt == mFrameCommitSlots.end())
	{
		if (mCurrentFrameRecordedClear && !mCurrentFrameInitializedBeforeClear)
		{
			mGpuBuffersInitialized = false;
		}
		mClearPending = true;
		mReadTableIndex = 0;
		mCurrentFrameRecordedClear = false;
		mCurrentFrameInitializedBeforeClear = false;
		return;
	}
	slotIt->fenceValue = fenceValue;
	slotIt->pending = true;
	slotIt->recordedClear = mCurrentFrameRecordedClear;
	slotIt->initializedBeforeClear = mCurrentFrameInitializedBeforeClear;
	mCurrentFrameRecordedClear = false;
	mCurrentFrameInitializedBeforeClear = false;
	mReadTableIndex = 1u - mReadTableIndex;
}

void NRIIndirectRadianceCache::ReconcileFrameCommits(const NRIIndirectRadianceCacheServices& services)
{
	bool abandoned = false;
	bool abandonedInitialClear = false;
	for (FrameCommitSlot& slot : mFrameCommitSlots)
	{
		if (!slot.pending)
		{
			continue;
		}
		if (services.fences.IsCommandFenceValueAbandoned(slot.fenceValue))
		{
			abandoned = true;
			abandonedInitialClear |= slot.recordedClear && !slot.initializedBeforeClear;
			slot = {};
		}
		else if (services.fences.IsCommandFenceValueComplete(slot.fenceValue))
		{
			if (slot.recordedClear)
			{
				++mClearCount;
			}
			slot = {};
		}
	}
	if (!abandoned)
	{
		return;
	}

	// Any lost dispatch breaks the ping-pong age chain. Later submitted work is
	// harmlessly superseded by a full clear recorded after it in queue order.
	if (abandonedInitialClear)
	{
		mGpuBuffersInitialized = false;
	}
	for (FrameCommitSlot& slot : mFrameCommitSlots)
	{
		slot = {};
	}
	mClearPending = true;
	mReadTableIndex = 0;
	mLastInvalidationMask |= NRI_INDIRECT_RADIANCE_CACHE_INVALID_INPUT;
}

void NRIIndirectRadianceCache::ClearReadbackSlot(uint32_t slotIndex)
{
	if (slotIndex >= mReadbackSlots.size())
	{
		return;
	}
	ReadbackSlot& slot = mReadbackSlots[slotIndex];
	slot.frameNumber = 0;
	slot.fenceValue = 0;
	slot.copySerial = 0;
	slot.pending = false;
}

void NRIIndirectRadianceCache::CopyTelemetryForReadback(
	const NRIIndirectRadianceCacheServices& services,
	uint64_t frameNumber)
{
	const NRIResourceContext& context = services.resources.context;
	if (!mActive || !mGpuBuffersInitialized || context.core == nullptr || context.commandBuffer == nullptr ||
		mTelemetryBuffer.buffer == nullptr)
	{
		return;
	}

	const uint64_t fenceValue = services.fences.GetRecordingCommandFenceValue();
	if (fenceValue == 0)
	{
		return;
	}
	for (uint32_t i = 0; i < mReadbackSlots.size(); ++i)
	{
		if (mReadbackSlots[i].pending && services.fences.IsCommandFenceValueAbandoned(mReadbackSlots[i].fenceValue))
		{
			ClearReadbackSlot(i);
		}
	}

	uint32_t selected = (uint32_t)mReadbackSlots.size();
	for (uint32_t offset = 0; offset < mReadbackSlots.size(); ++offset)
	{
		const uint32_t index = (mNextReadbackSlot + offset) % (uint32_t)mReadbackSlots.size();
		if (!mReadbackSlots[index].pending)
		{
			selected = index;
			break;
		}
	}
	if (selected == mReadbackSlots.size())
	{
		return;
	}

	ReadbackSlot& slot = mReadbackSlots[selected];
	nri::BufferBarrierDesc before[2] = {};
	before[0].buffer = mTelemetryBuffer.buffer;
	before[0].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	before[0].after = NRIResourceCopySourceAccess();
	before[1].buffer = slot.buffer.buffer;
	before[1].before = slot.initialized ? NRIResourceCopyDestinationAccess() : nri::AccessStage{};
	before[1].after = NRIResourceCopyDestinationAccess();
	nri::BarrierDesc beforeDesc = {};
	beforeDesc.buffers = before;
	beforeDesc.bufferNum = 2;
	context.core->CmdBarrier(*context.commandBuffer, beforeDesc);

	const uint64_t byteSize = (uint64_t)NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_COUNT * sizeof(uint32_t);
	context.core->CmdCopyBuffer(*context.commandBuffer, *slot.buffer.buffer, 0, *mTelemetryBuffer.buffer, 0, byteSize);

	nri::BufferBarrierDesc after = {};
	after.buffer = mTelemetryBuffer.buffer;
	after.before = NRIResourceCopySourceAccess();
	after.after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	nri::BarrierDesc afterDesc = {};
	afterDesc.buffers = &after;
	afterDesc.bufferNum = 1;
	context.core->CmdBarrier(*context.commandBuffer, afterDesc);

	slot.frameNumber = frameNumber;
	slot.fenceValue = fenceValue;
	slot.copySerial = mNextCopySerial++;
	slot.pending = true;
	slot.initialized = true;
	mNextReadbackSlot = (selected + 1u) % (uint32_t)mReadbackSlots.size();
}

void NRIIndirectRadianceCache::ReadbackTelemetry(
	const NRIIndirectRadianceCacheServices& services,
	bool enabled,
	NRIIndirectRadianceCacheTelemetrySnapshot& outSnapshot)
{
	const NRIResourceContext& context = services.resources.context;
	if (context.core == nullptr)
	{
		return;
	}

	uint32_t newestReady = (uint32_t)mReadbackSlots.size();
	uint64_t newestSerial = 0;
	for (uint32_t i = 0; i < mReadbackSlots.size(); ++i)
	{
		ReadbackSlot& slot = mReadbackSlots[i];
		if (!slot.pending)
		{
			continue;
		}
		if (services.fences.IsCommandFenceValueAbandoned(slot.fenceValue))
		{
			ClearReadbackSlot(i);
			continue;
		}
		if (services.fences.IsCommandFenceValueComplete(slot.fenceValue) && slot.copySerial > newestSerial)
		{
			newestReady = i;
			newestSerial = slot.copySerial;
		}
	}

	if (newestReady == mReadbackSlots.size())
	{
		outSnapshot.pendingReadbacks = CountPendingReadbacks(mReadbackSlots);
		return;
	}
	for (uint32_t i = 0; i < mReadbackSlots.size(); ++i)
	{
		if (i != newestReady && mReadbackSlots[i].pending &&
			services.fences.IsCommandFenceValueComplete(mReadbackSlots[i].fenceValue))
		{
			ClearReadbackSlot(i);
		}
	}

	ReadbackSlot& slot = mReadbackSlots[newestReady];
	if (!enabled)
	{
		ClearReadbackSlot(newestReady);
		outSnapshot.valid = false;
		outSnapshot.pendingReadbacks = CountPendingReadbacks(mReadbackSlots);
		return;
	}

	const uint64_t byteSize = (uint64_t)NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_COUNT * sizeof(uint32_t);
	const uint32_t* counters = static_cast<const uint32_t*>(context.core->MapBuffer(*slot.buffer.buffer, 0, byteSize));
	if (counters == nullptr)
	{
		ClearReadbackSlot(newestReady);
		return;
	}
	outSnapshot.valid = true;
	outSnapshot.frameNumber = slot.frameNumber;
	outSnapshot.lookupCount = counters[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_LOOKUPS];
	outSnapshot.acceptedHitCount = counters[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_ACCEPTED_HITS];
	outSnapshot.forcedMissCount = counters[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_FORCED_MISSES];
	outSnapshot.collisionCount = counters[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_COLLISIONS];
	outSnapshot.staleGenerationCount = counters[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_STALE_GENERATION];
	outSnapshot.unsupportedRouteCount = counters[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_UNSUPPORTED_ROUTE];
	outSnapshot.exactFallbackCount = counters[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_EXACT_FALLBACK];
	outSnapshot.occupancy = counters[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_OCCUPANCY];
	outSnapshot.updateCount = counters[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_UPDATES];
	outSnapshot.clearCount = std::max<uint64_t>(mClearCount, counters[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_CLEAR_COUNT]);
	outSnapshot.lookupTimeTicks = counters[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_LOOKUP_TIME];
	outSnapshot.updateTimeTicks = counters[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_UPDATE_TIME];
	outSnapshot.resolveTimeTicks = counters[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_RESOLVE_TIME];
	outSnapshot.clearTimeTicks = counters[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_CLEAR_TIME];
	outSnapshot.tableMemoryBytes = GetTableMemoryBytes();
	outSnapshot.totalMemoryBytes = GetTotalMemoryBytes();
	outSnapshot.invalidationMask = mLastInvalidationMask;
	context.core->UnmapBuffer(*slot.buffer.buffer);
	ClearReadbackSlot(newestReady);
	outSnapshot.pendingReadbacks = CountPendingReadbacks(mReadbackSlots);
}

uint64_t NRIIndirectRadianceCache::GetTableMemoryBytes() const
{
	return mTables[0].memorySize + mTables[1].memorySize;
}

uint64_t NRIIndirectRadianceCache::GetTotalMemoryBytes() const
{
	uint64_t result = GetTableMemoryBytes() + mTelemetryBuffer.memorySize + mClearUploadBuffer.memorySize;
	for (const ReadbackSlot& slot : mReadbackSlots)
	{
		result += slot.buffer.memorySize;
	}
	return result;
}

void NRIIndirectRadianceCache::Destroy(const NRIResourceServices& services)
{
	for (NRIBufferResource& table : mTables)
	{
		services.DestroyBufferResource(table);
	}
	services.DestroyBufferResource(mTelemetryBuffer);
	services.DestroyBufferResource(mClearUploadBuffer);
	for (ReadbackSlot& slot : mReadbackSlots)
	{
		services.DestroyBufferResource(slot.buffer);
		slot = {};
	}
	mCompatibility = {};
	mFrameCommitSlots = {};
	mNextCopySerial = 1;
	mClearCount = 0;
	mEntryCount = 0;
	mReadTableIndex = 0;
	mNextReadbackSlot = 0;
	mLastInvalidationMask = NRI_INDIRECT_RADIANCE_CACHE_INVALID_NONE;
	mActive = false;
	mClearPending = false;
	mGpuBuffersInitialized = false;
	mCurrentFrameRecordedClear = false;
	mCurrentFrameInitializedBeforeClear = false;
	mDescriptorSets = {};
}
