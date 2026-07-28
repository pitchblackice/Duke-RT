#include "nri_smoke_dormant_grid.h"

#include <algorithm>
#include <cstring>

namespace
{
	constexpr uint32_t kThreadGroupWidth = 64u;
	const char* const kPipelineNames[] = {
		"SmokeDormantGridClear",
		"SmokeDormantGridArchive",
		"SmokeDormantGridCompactFineActive",
		"SmokeDormantGridRehydrate",
		"SmokeDormantGridEvolve",
		"SmokeDormantGridInject",
	};

	nri::AccessStage StorageAccess()
	{
		return { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	}

	nri::AccessStage CopySourceAccess()
	{
		return { nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY };
	}

	nri::AccessStage CopyDestinationAccess()
	{
		return { nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY };
	}

	uint32_t Groups(uint32_t count)
	{
		return std::max(1u, (count + kThreadGroupWidth - 1u) / kThreadGroupWidth);
	}

	uint32_t NextPowerOfTwo(uint32_t value)
	{
		value = std::max(value, 2u) - 1u;
		value |= value >> 1u;
		value |= value >> 2u;
		value |= value >> 4u;
		value |= value >> 8u;
		value |= value >> 16u;
		return value + 1u;
	}
}

bool NRISmokeDormantGridFineDescriptors::IsValid() const
{
	if (brickCapacity == 0u || hashCapacity == 0u || cellCapacity <
		brickCapacity * NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK || fieldPing > 1u || activePing > 1u ||
		(hashCapacity & (hashCapacity - 1u)) != 0u || !(cellSize > 0.0f))
		return false;
	for (uint32_t i = 0u; i < storage.size(); ++i)
		if (storage[i] == nullptr || buffers[i] == nullptr) return false;
	return true;
}

std::array<NRIBufferResource*, NRISmokeDormantGrid::StorageDescriptorCount>
NRISmokeDormantGrid::StorageResources()
{
	return { &mControl, &mHash, &mRecords, &mFreeList, &mScalar, &mVelocity,
		&mOptical, &mDynamics, &mDemotions, &mPromotions, &mResults, &mInjections };
}

std::array<const NRIBufferResource*, NRISmokeDormantGrid::StorageDescriptorCount>
NRISmokeDormantGrid::StorageResources() const
{
	return { &mControl, &mHash, &mRecords, &mFreeList, &mScalar, &mVelocity,
		&mOptical, &mDynamics, &mDemotions, &mPromotions, &mResults, &mInjections };
}

void NRISmokeDormantGrid::SetFailure(const char* reason)
{
	mStatus.resourcesReady = false;
	mStatus.failureReason = reason != nullptr ? reason : "unspecified";
}

bool NRISmokeDormantGrid::CreateBuffer(const NRISmokeGridServices& services,
	NRIBufferResource& out, uint64_t size, uint32_t stride, nri::BufferUsageBits usage,
	nri::MemoryLocation location, bool storageView)
{
	DestroyBuffer(services, out);
	if (!services.IsDeviceValid() || stride == 0u) return false;
	nri::BufferDesc desc = {};
	desc.size = std::max<uint64_t>(size, stride);
	desc.structureStride = stride;
	desc.usage = usage;
	if (services.core->CreateCommittedBuffer(*services.device, location, 0.0f, desc,
		out.buffer) != nri::Result::SUCCESS)
		return false;
	nri::MemoryDesc memory = {};
	services.core->GetBufferMemoryDesc(*out.buffer, location, memory);
	out.size = out.usedSize = desc.size;
	out.memorySize = memory.size;
	out.stride = stride;
	out.usage = usage;
	out.memoryLocation = location;
	if (storageView)
	{
		nri::BufferViewDesc view = {};
		view.buffer = out.buffer;
		view.type = nri::BufferView::STORAGE_STRUCTURED_BUFFER;
		view.offset = 0u;
		view.size = nri::WHOLE_SIZE;
		view.structureStride = stride;
		if (services.core->CreateBufferView(view, out.storageView) != nri::Result::SUCCESS)
		{
			DestroyBuffer(services, out);
			return false;
		}
	}
	return true;
}

void NRISmokeDormantGrid::DestroyBuffer(const NRISmokeGridServices& services,
	NRIBufferResource& resource)
{
	if (services.core != nullptr)
	{
		if (resource.shaderView != nullptr) services.core->DestroyDescriptor(resource.shaderView);
		if (resource.storageView != nullptr) services.core->DestroyDescriptor(resource.storageView);
		if (resource.buffer != nullptr) services.core->DestroyBuffer(resource.buffer);
	}
	resource = {};
}

bool NRISmokeDormantGrid::Initialize(const NRISmokeGridServices& services)
{
	if (mInitialized) return true;
	if (!services.IsDeviceValid() || services.loadShaderBlob == nullptr ||
		services.queuedFrameCount == 0u)
	{
		SetFailure("invalid-services");
		return false;
	}

	nri::DescriptorRangeDesc ranges[2] = {};
	ranges[0].baseRegisterIndex = 0u;
	ranges[0].descriptorNum = FineDescriptorCount;
	ranges[0].descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
	ranges[0].shaderStages = nri::StageBits::COMPUTE_SHADER;
	ranges[0].flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	ranges[1].baseRegisterIndex = 0u;
	ranges[1].descriptorNum = StorageDescriptorCount;
	ranges[1].descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
	ranges[1].shaderStages = nri::StageBits::COMPUTE_SHADER;
	ranges[1].flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	nri::DescriptorSetDesc sets[2] = {};
	for (uint32_t i = 0u; i < 2u; ++i)
	{
		sets[i].registerSpace = i;
		sets[i].ranges = &ranges[i];
		sets[i].rangeNum = 1u;
		sets[i].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	}
	nri::RootConstantDesc root = {};
	root.registerIndex = 0u;
	root.size = sizeof(NRISmokeDormantGridConstants);
	root.shaderStages = nri::StageBits::COMPUTE_SHADER;
	nri::PipelineLayoutDesc layout = {};
	layout.rootRegisterSpace = 2u;
	layout.rootConstants = &root;
	layout.rootConstantNum = 1u;
	layout.descriptorSets = sets;
	layout.descriptorSetNum = 2u;
	layout.shaderStages = nri::StageBits::COMPUTE_SHADER;
	if (services.core->CreatePipelineLayout(*services.device, layout, mPipelineLayout) !=
		nri::Result::SUCCESS)
	{
		SetFailure("pipeline-layout");
		return false;
	}

	const bool d3d12 = services.graphicsAPI == nri::GraphicsAPI::D3D12;
	for (uint32_t i = 0u; i < mPipelines.size(); ++i)
	{
		std::vector<uint8_t> blob;
		const std::string name = std::string(kPipelineNames[i]) + ".cs." +
			(d3d12 ? "dxil" : "spirv");
		if (!services.LoadShaderBlob(name.c_str(), blob))
		{
			SetFailure("shader-blob");
			Shutdown(services);
			return false;
		}
		nri::ShaderDesc shader = {};
		shader.stage = nri::StageBits::COMPUTE_SHADER;
		shader.bytecode = blob.data();
		shader.size = blob.size();
		shader.entryPointName = "main";
		nri::ComputePipelineDesc pipeline = {};
		pipeline.pipelineLayout = mPipelineLayout;
		pipeline.shader = shader;
		if (services.core->CreateComputePipeline(*services.device, pipeline,
			mPipelines[i]) != nri::Result::SUCCESS)
		{
			SetFailure("compute-pipeline");
			Shutdown(services);
			return false;
		}
	}

	if (services.core->AllocateDescriptorSets(*services.descriptorPool, *mPipelineLayout,
		0u, &mFineSet, 1u, 0u) != nri::Result::SUCCESS ||
		services.core->AllocateDescriptorSets(*services.descriptorPool, *mPipelineLayout,
		1u, &mStorageSet, 1u, 0u) != nri::Result::SUCCESS)
	{
		SetFailure("descriptor-set");
		Shutdown(services);
		return false;
	}
	mFrameSlots.resize(services.queuedFrameCount);
	mInitialized = true;
	mStatus.initialized = true;
	mStatus.failureReason = "none";
	return true;
}

bool NRISmokeDormantGrid::EnsureResources(const NRISmokeGridServices& services,
	const NRISmokeDormantGridConfig& config)
{
	const uint32_t capacity = std::clamp(config.archiveCapacity, 16u, 4096u);
	const uint32_t hashCapacity = NextPowerOfTwo(capacity * 2u);
	const uint32_t maximumWork = std::clamp(std::max({ config.maximumDemotionsPerFrame,
		config.maximumPromotionsPerFrame, config.maximumContinuousInjectionsPerFrame }), 1u, 256u);
	if (mControl.buffer != nullptr && mResourceArchiveCapacity == capacity &&
		mResourceHashCapacity == hashCapacity && mResourceMaximumWork == maximumWork)
		return true;

	services.WaitForCommands("smoke-dormant-grid-resource-layout");
	DestroyResources(services);
	const nri::BufferUsageBits storage = nri::BufferUsageBits::SHADER_RESOURCE_STORAGE;
	const nri::BufferUsageBits storageCopyDestination = storage;
	const nri::BufferUsageBits storageCopySource = storage;
	const uint64_t cells = (uint64_t)capacity * NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK;
	bool created =
		CreateBuffer(services, mControl, sizeof(NRISmokeDormantGridControlGpu),
			sizeof(NRISmokeDormantGridControlGpu), storageCopySource,
			nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mHash, (uint64_t)hashCapacity * sizeof(NRISmokeDormantGridHashEntryGpu),
			sizeof(NRISmokeDormantGridHashEntryGpu), storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mRecords, (uint64_t)capacity * sizeof(NRISmokeDormantGridRecordGpu),
			sizeof(NRISmokeDormantGridRecordGpu), storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mFreeList, (uint64_t)capacity * sizeof(uint32_t), sizeof(uint32_t),
			storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mScalar, cells * sizeof(float) * 4u, sizeof(float) * 4u,
			storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mVelocity, cells * sizeof(float) * 4u, sizeof(float) * 4u,
			storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mOptical, cells * sizeof(float) * 4u, sizeof(float) * 4u,
			storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mDynamics, cells * sizeof(float) * 4u, sizeof(float) * 4u,
			storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mDemotions, (uint64_t)maximumWork * sizeof(NRISmokeDormantGridWorkGpu),
			sizeof(NRISmokeDormantGridWorkGpu), storageCopyDestination,
			nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mPromotions, (uint64_t)maximumWork * sizeof(NRISmokeDormantGridWorkGpu),
			sizeof(NRISmokeDormantGridWorkGpu), storageCopyDestination,
			nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mResults, (uint64_t)maximumWork * 2u * sizeof(NRISmokeDormantGridResultGpu),
			sizeof(NRISmokeDormantGridResultGpu), storageCopySource,
			nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mInjections,
			(uint64_t)maximumWork * sizeof(NRISmokeDormantGridInjectionGpu),
			sizeof(NRISmokeDormantGridInjectionGpu), storageCopyDestination,
			nri::MemoryLocation::DEVICE, true);
	for (FrameSlot& slot : mFrameSlots)
	{
		created = created && CreateBuffer(services, slot.demotionUpload,
			(uint64_t)maximumWork * sizeof(NRISmokeDormantGridWorkGpu),
			sizeof(NRISmokeDormantGridWorkGpu), nri::BufferUsageBits::NONE,
			nri::MemoryLocation::HOST_UPLOAD, false);
		created = created && CreateBuffer(services, slot.promotionUpload,
			(uint64_t)maximumWork * sizeof(NRISmokeDormantGridWorkGpu),
			sizeof(NRISmokeDormantGridWorkGpu), nri::BufferUsageBits::NONE,
			nri::MemoryLocation::HOST_UPLOAD, false);
		created = created && CreateBuffer(services, slot.injectionUpload,
			(uint64_t)maximumWork * sizeof(NRISmokeDormantGridInjectionGpu),
			sizeof(NRISmokeDormantGridInjectionGpu), nri::BufferUsageBits::NONE,
			nri::MemoryLocation::HOST_UPLOAD, false);
		created = created && CreateBuffer(services, slot.controlReadback,
			sizeof(NRISmokeDormantGridControlGpu), sizeof(NRISmokeDormantGridControlGpu),
			nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, false);
		created = created && CreateBuffer(services, slot.resultReadback,
			(uint64_t)maximumWork * 2u * sizeof(NRISmokeDormantGridResultGpu),
			sizeof(NRISmokeDormantGridResultGpu), nri::BufferUsageBits::NONE,
			nri::MemoryLocation::HOST_READBACK, false);
		slot.readbackPending = false;
	}
	if (!created)
	{
		DestroyResources(services);
		SetFailure("resource-allocation");
		return false;
	}

	const auto resources = StorageResources();
	std::array<const nri::Descriptor*, StorageDescriptorCount> descriptors = {};
	for (uint32_t i = 0u; i < descriptors.size(); ++i)
		descriptors[i] = resources[i]->storageView;
	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = mStorageSet;
	update.rangeIndex = 0u;
	update.descriptors = descriptors.data();
	update.descriptorNum = (uint32_t)descriptors.size();
	services.core->UpdateDescriptorRanges(&update, 1u);

	mResourceArchiveCapacity = capacity;
	mResourceHashCapacity = hashCapacity;
	mResourceMaximumWork = maximumWork;
	mResourceEpoch = 0u;
	mNeedsClear = true;
	mResourcesInitialized = false;
	mStatus.archiveCapacity = capacity;
	mStatus.hashCapacity = hashCapacity;
	mStatus.maximumWork = maximumWork;
	mStatus.residentBytes = 0u;
	for (const NRIBufferResource* resource : StorageResources())
		mStatus.residentBytes += resource->memorySize;
	for (const FrameSlot& slot : mFrameSlots)
		mStatus.residentBytes += slot.demotionUpload.memorySize + slot.promotionUpload.memorySize +
			slot.injectionUpload.memorySize + slot.controlReadback.memorySize +
			slot.resultReadback.memorySize;
	mStatus.payloadBytes = cells * sizeof(float) * 4u * 4u;
	mStatus.resourcesReady = true;
	mStatus.failureReason = "none";
	return true;
}

void NRISmokeDormantGrid::ConsumeReadback(const NRISmokeGridServices& services,
	uint32_t simulationEpoch)
{
	if (mFrameSlots.empty() || services.core == nullptr) return;
	FrameSlot& slot = mFrameSlots[std::min(services.queuedFrameIndex,
		(uint32_t)mFrameSlots.size() - 1u)];
	if (!slot.readbackPending) return;
	if (slot.readbackEpoch == simulationEpoch)
	{
		const void* control = services.core->MapBuffer(*slot.controlReadback.buffer, 0u,
			sizeof(NRISmokeDormantGridControlGpu));
		if (control != nullptr)
		{
			std::memcpy(&mStatus.gpu, control, sizeof(mStatus.gpu));
			services.core->UnmapBuffer(*slot.controlReadback.buffer);
			mStatus.gpuStatsValid = true;
			mStatus.gpuRendererFrame = slot.readbackRendererFrame;
			mStatus.readbackBytes += sizeof(NRISmokeDormantGridControlGpu);
		}
		mStatus.results.clear();
		if (slot.resultCount != 0u)
		{
			const uint64_t bytes = (uint64_t)slot.resultCount * sizeof(NRISmokeDormantGridResultGpu);
			const void* results = services.core->MapBuffer(*slot.resultReadback.buffer, 0u, bytes);
			if (results != nullptr)
			{
				const auto* begin = static_cast<const NRISmokeDormantGridResultGpu*>(results);
				mStatus.results.assign(begin, begin + slot.resultCount);
				services.core->UnmapBuffer(*slot.resultReadback.buffer);
				mStatus.readbackBytes += bytes;
			}
		}
	}
	slot.readbackPending = false;
}

bool NRISmokeDormantGrid::PrepareFrame(const NRISmokeGridServices& services,
	const NRISmokeDormantGridConfig& config, uint32_t simulationEpoch)
{
	ConsumeReadback(services, simulationEpoch);
	mStatus.requested = config.enabled;
	if (!config.enabled)
	{
		mStatus.failureReason = "not-requested";
		return true;
	}
	if (!Initialize(services) || !EnsureResources(services, config)) return false;
	if (mResourceEpoch != 0u && mResourceEpoch != simulationEpoch)
		Reset(simulationEpoch, "simulation-epoch");
	mStatus.epoch = simulationEpoch;
	return true;
}

void NRISmokeDormantGrid::TransitionArchiveToStorage(const NRISmokeGridServices& services)
{
	const auto resources = StorageResources();
	std::array<nri::BufferBarrierDesc, StorageDescriptorCount> barriers = {};
	for (uint32_t i = 0u; i < barriers.size(); ++i)
	{
		barriers[i].buffer = resources[i]->buffer;
		barriers[i].before = mResourcesInitialized ? StorageAccess() : nri::AccessStage{};
		barriers[i].after = StorageAccess();
	}
	nri::BarrierDesc barrier = {};
	barrier.buffers = barriers.data();
	barrier.bufferNum = (uint32_t)barriers.size();
	services.core->CmdBarrier(*services.commandBuffer, barrier);
	mResourcesInitialized = true;
}

void NRISmokeDormantGrid::StorageBarrier(const NRISmokeGridServices& services,
	const NRISmokeDormantGridFineDescriptors& fine)
{
	std::array<nri::BufferBarrierDesc, StorageDescriptorCount + FineDescriptorCount> barriers = {};
	uint32_t count = 0u;
	for (const NRIBufferResource* resource : StorageResources())
	{
		barriers[count].buffer = resource->buffer;
		barriers[count].before = StorageAccess();
		barriers[count].after = StorageAccess();
		count++;
	}
	for (nri::Buffer* buffer : fine.buffers)
	{
		barriers[count].buffer = buffer;
		barriers[count].before = StorageAccess();
		barriers[count].after = StorageAccess();
		count++;
	}
	nri::BarrierDesc barrier = {};
	barrier.buffers = barriers.data();
	barrier.bufferNum = count;
	services.core->CmdBarrier(*services.commandBuffer, barrier);
}

void NRISmokeDormantGrid::Dispatch(const NRISmokeGridServices& services,
	NRISmokeDormantGridConstants& constants, NRISmokeDormantGridPass pass, uint32_t x)
{
	services.core->CmdBeginAnnotation(*services.commandBuffer,
		kPipelineNames[(uint32_t)pass], nri::BGRA_UNUSED);
	constants.pass = (uint32_t)pass;
	services.core->CmdSetRootConstants(*services.commandBuffer,
		{ 0u, &constants, sizeof(constants), 0u, nri::BindPoint::COMPUTE });
	services.core->CmdSetPipeline(*services.commandBuffer, *mPipelines[(uint32_t)pass]);
	services.core->CmdDispatch(*services.commandBuffer, { std::max(x, 1u), 1u, 1u });
	services.core->CmdEndAnnotation(*services.commandBuffer);
}

bool NRISmokeDormantGrid::RecordStage(const NRISmokeGridServices& services,
	const NRISmokeDormantGridConfig& config, const NRISmokeDormantGridFrameDesc& frame,
	bool promotions)
{
	if (!config.enabled) return true;
	if (!services.IsRecordingValid() || !mInitialized || !mStatus.resourcesReady ||
		!frame.fine.IsValid() || frame.simulationEpoch == 0u || mFrameSlots.empty())
	{
		SetFailure("invalid-frame");
		return false;
	}
	const uint32_t demotionCount = promotions ? 0u : std::min({ frame.demotionCount,
		config.maximumDemotionsPerFrame, mResourceMaximumWork });
	const uint32_t promotionCount = promotions ? std::min({ frame.promotionCount,
		config.maximumPromotionsPerFrame, mResourceMaximumWork }) : 0u;
	if ((demotionCount != 0u && frame.demotions == nullptr) ||
		(promotionCount != 0u && frame.promotions == nullptr))
	{
		SetFailure("invalid-work");
		return false;
	}
	mStatus.submittedDemotions += demotionCount;
	mStatus.submittedPromotions += promotionCount;
	if (!promotions) mStatus.clippedDemotions += frame.demotionCount - demotionCount;
	if (promotions) mStatus.clippedPromotions += frame.promotionCount - promotionCount;

	FrameSlot& slot = mFrameSlots[std::min(services.queuedFrameIndex,
		(uint32_t)mFrameSlots.size() - 1u)];
	if (slot.workRendererFrame != services.rendererFrame)
	{
		slot.workRendererFrame = services.rendererFrame;
		slot.promotionResultCount = 0u;
		slot.demotionResultCount = 0u;
		slot.resultCount = 0u;
	}
	if (demotionCount != 0u)
	{
		const uint64_t bytes = (uint64_t)demotionCount * sizeof(NRISmokeDormantGridWorkGpu);
		void* mapped = services.core->MapBuffer(*slot.demotionUpload.buffer, 0u, bytes);
		if (mapped == nullptr) { SetFailure("demotion-upload-map"); return false; }
		std::memcpy(mapped, frame.demotions, bytes);
		services.core->UnmapBuffer(*slot.demotionUpload.buffer);
	}
	if (promotionCount != 0u)
	{
		const uint64_t bytes = (uint64_t)promotionCount * sizeof(NRISmokeDormantGridWorkGpu);
		void* mapped = services.core->MapBuffer(*slot.promotionUpload.buffer, 0u, bytes);
		if (mapped == nullptr) { SetFailure("promotion-upload-map"); return false; }
		std::memcpy(mapped, frame.promotions, bytes);
		services.core->UnmapBuffer(*slot.promotionUpload.buffer);
	}

	nri::UpdateDescriptorRangeDesc fineUpdate = {};
	fineUpdate.descriptorSet = mFineSet;
	fineUpdate.rangeIndex = 0u;
	fineUpdate.descriptors = frame.fine.storage.data();
	fineUpdate.descriptorNum = FineDescriptorCount;
	services.core->UpdateDescriptorRanges(&fineUpdate, 1u);
	TransitionArchiveToStorage(services);

	nri::BufferBarrierDesc copyBarriers[2] = {};
	uint32_t copyBarrierCount = 0u;
	if (demotionCount != 0u)
	{
		copyBarriers[copyBarrierCount].buffer = mDemotions.buffer;
		copyBarriers[copyBarrierCount].before = StorageAccess();
		copyBarriers[copyBarrierCount].after = CopyDestinationAccess();
		copyBarrierCount++;
	}
	if (promotionCount != 0u)
	{
		copyBarriers[copyBarrierCount].buffer = mPromotions.buffer;
		copyBarriers[copyBarrierCount].before = StorageAccess();
		copyBarriers[copyBarrierCount].after = CopyDestinationAccess();
		copyBarrierCount++;
	}
	if (copyBarrierCount != 0u)
	{
		nri::BarrierDesc barrier = {};
		barrier.buffers = copyBarriers;
		barrier.bufferNum = copyBarrierCount;
		services.core->CmdBarrier(*services.commandBuffer, barrier);
	}
	if (demotionCount != 0u)
		services.core->CmdCopyBuffer(*services.commandBuffer, *mDemotions.buffer, 0u,
			*slot.demotionUpload.buffer, 0u,
			(uint64_t)demotionCount * sizeof(NRISmokeDormantGridWorkGpu));
	if (promotionCount != 0u)
		services.core->CmdCopyBuffer(*services.commandBuffer, *mPromotions.buffer, 0u,
			*slot.promotionUpload.buffer, 0u,
			(uint64_t)promotionCount * sizeof(NRISmokeDormantGridWorkGpu));
	for (uint32_t i = 0u; i < copyBarrierCount; ++i)
	{
		copyBarriers[i].before = CopyDestinationAccess();
		copyBarriers[i].after = StorageAccess();
	}
	if (copyBarrierCount != 0u)
	{
		nri::BarrierDesc barrier = {};
		barrier.buffers = copyBarriers;
		barrier.bufferNum = copyBarrierCount;
		services.core->CmdBarrier(*services.commandBuffer, barrier);
	}

	services.core->CmdSetPipelineLayout(*services.commandBuffer,
		nri::BindPoint::COMPUTE, *mPipelineLayout);
	services.core->CmdSetDescriptorSet(*services.commandBuffer,
		{ 0u, mFineSet, nri::BindPoint::COMPUTE });
	services.core->CmdSetDescriptorSet(*services.commandBuffer,
		{ 1u, mStorageSet, nri::BindPoint::COMPUTE });
	NRISmokeDormantGridConstants constants = {};
	constants.frameIndex = frame.frameIndex;
	constants.simulationEpoch = frame.simulationEpoch;
	constants.fieldPing = frame.fine.fieldPing;
	constants.fineBrickCapacity = frame.fine.brickCapacity;
	constants.fineHashCapacity = frame.fine.hashCapacity;
	constants.fineCellCapacity = frame.fine.cellCapacity;
	constants.archiveCapacity = mResourceArchiveCapacity;
	constants.archiveHashCapacity = mResourceHashCapacity;
	constants.demotionCount = demotionCount;
	constants.promotionCount = promotionCount;
	constants.evolutionCount = std::min(config.maximumEvolutionPerFrame,
		mResourceMaximumWork);
	constants.cellSize = frame.fine.cellSize;
	constants.deltaTime = frame.deltaTime;
	constants.opticalMassRelativeTolerance = std::max(config.opticalMassRelativeTolerance, 0.0f);
	constants.activePing = frame.fine.activePing;
	std::copy(std::begin(frame.cameraPosition), std::end(frame.cameraPosition),
		constants.cameraPosition);
	StorageBarrier(services, frame.fine);

	if (mNeedsClear || mResourceEpoch != frame.simulationEpoch)
	{
		Dispatch(services, constants, NRISmokeDormantGridPass::Clear,
			Groups(std::max({ mResourceArchiveCapacity, mResourceHashCapacity,
				mResourceMaximumWork * 2u })));
		StorageBarrier(services, frame.fine);
		mNeedsClear = false;
		mResourceEpoch = frame.simulationEpoch;
	}
	if (demotionCount != 0u)
	{
		Dispatch(services, constants, NRISmokeDormantGridPass::Archive, demotionCount);
		StorageBarrier(services, frame.fine);
		Dispatch(services, constants, NRISmokeDormantGridPass::CompactFineActive, 1u);
		StorageBarrier(services, frame.fine);
	}
	if (promotionCount != 0u)
	{
		Dispatch(services, constants, NRISmokeDormantGridPass::Rehydrate, promotionCount);
		StorageBarrier(services, frame.fine);
	}

	nri::BufferBarrierDesc readbackBefore = {};
	readbackBefore.buffer = mResults.buffer;
	readbackBefore.before = StorageAccess();
	readbackBefore.after = CopySourceAccess();
	nri::BarrierDesc readbackBarrier = {};
	readbackBarrier.buffers = &readbackBefore;
	readbackBarrier.bufferNum = 1u;
	services.core->CmdBarrier(*services.commandBuffer, readbackBarrier);
	const uint32_t resultCount = demotionCount + promotionCount;
	if (resultCount != 0u)
	{
		const uint32_t resultOffset = promotions ? 0u : slot.promotionResultCount;
		services.core->CmdCopyBuffer(*services.commandBuffer, *slot.resultReadback.buffer,
			(uint64_t)resultOffset * sizeof(NRISmokeDormantGridResultGpu),
			*mResults.buffer, 0u,
			(uint64_t)resultCount * sizeof(NRISmokeDormantGridResultGpu));
		if (promotions) slot.promotionResultCount = resultCount;
		else slot.demotionResultCount = resultCount;
	}
	readbackBefore.before = CopySourceAccess();
	readbackBefore.after = StorageAccess();
	services.core->CmdBarrier(*services.commandBuffer, readbackBarrier);
	slot.resultCount = slot.promotionResultCount + slot.demotionResultCount;
	mStatus.failureReason = "none";
	return true;
}

bool NRISmokeDormantGrid::RecordPromotions(const NRISmokeGridServices& services,
	const NRISmokeDormantGridConfig& config, const NRISmokeDormantGridFrameDesc& frame)
{
	return RecordStage(services, config, frame, true);
}

bool NRISmokeDormantGrid::RecordDemotions(const NRISmokeGridServices& services,
	const NRISmokeDormantGridConfig& config, const NRISmokeDormantGridFrameDesc& frame)
{
	return RecordStage(services, config, frame, false);
}

bool NRISmokeDormantGrid::RecordEvolution(const NRISmokeGridServices& services,
	const NRISmokeDormantGridConfig& config, const NRISmokeDormantGridFrameDesc& frame)
{
	if (!config.enabled) return true;
	if (!services.IsRecordingValid() || !mInitialized || !mStatus.resourcesReady ||
		!frame.fine.IsValid() || frame.simulationEpoch == 0u || mFrameSlots.empty())
	{
		SetFailure("invalid-evolution-frame");
		return false;
	}
	const uint32_t injectionCount = std::min({ frame.injectionCount,
		config.maximumContinuousInjectionsPerFrame, mResourceMaximumWork });
	if (injectionCount != 0u && frame.injections == nullptr)
	{
		SetFailure("invalid-injection-work");
		return false;
	}
	mStatus.submittedInjections += injectionCount;
	mStatus.clippedInjections += frame.injectionCount - injectionCount;

	FrameSlot& slot = mFrameSlots[std::min(services.queuedFrameIndex,
		(uint32_t)mFrameSlots.size() - 1u)];
	if (injectionCount != 0u)
	{
		const uint64_t bytes = (uint64_t)injectionCount *
			sizeof(NRISmokeDormantGridInjectionGpu);
		void* mapped = services.core->MapBuffer(*slot.injectionUpload.buffer, 0u, bytes);
		if (mapped == nullptr) { SetFailure("injection-upload-map"); return false; }
		std::memcpy(mapped, frame.injections, bytes);
		services.core->UnmapBuffer(*slot.injectionUpload.buffer);
	}

	nri::UpdateDescriptorRangeDesc fineUpdate = {};
	fineUpdate.descriptorSet = mFineSet;
	fineUpdate.rangeIndex = 0u;
	fineUpdate.descriptors = frame.fine.storage.data();
	fineUpdate.descriptorNum = FineDescriptorCount;
	services.core->UpdateDescriptorRanges(&fineUpdate, 1u);
	TransitionArchiveToStorage(services);
	if (injectionCount != 0u)
	{
		nri::BufferBarrierDesc copyBarrier = {};
		copyBarrier.buffer = mInjections.buffer;
		copyBarrier.before = StorageAccess();
		copyBarrier.after = CopyDestinationAccess();
		nri::BarrierDesc barrier = {};
		barrier.buffers = &copyBarrier;
		barrier.bufferNum = 1u;
		services.core->CmdBarrier(*services.commandBuffer, barrier);
		services.core->CmdCopyBuffer(*services.commandBuffer, *mInjections.buffer, 0u,
			*slot.injectionUpload.buffer, 0u,
			(uint64_t)injectionCount * sizeof(NRISmokeDormantGridInjectionGpu));
		copyBarrier.before = CopyDestinationAccess();
		copyBarrier.after = StorageAccess();
		services.core->CmdBarrier(*services.commandBuffer, barrier);
	}

	services.core->CmdSetPipelineLayout(*services.commandBuffer,
		nri::BindPoint::COMPUTE, *mPipelineLayout);
	services.core->CmdSetDescriptorSet(*services.commandBuffer,
		{ 0u, mFineSet, nri::BindPoint::COMPUTE });
	services.core->CmdSetDescriptorSet(*services.commandBuffer,
		{ 1u, mStorageSet, nri::BindPoint::COMPUTE });
	NRISmokeDormantGridConstants constants = {};
	constants.frameIndex = frame.frameIndex;
	constants.simulationEpoch = frame.simulationEpoch;
	constants.fieldPing = frame.fine.fieldPing;
	constants.fineBrickCapacity = frame.fine.brickCapacity;
	constants.fineHashCapacity = frame.fine.hashCapacity;
	constants.fineCellCapacity = frame.fine.cellCapacity;
	constants.archiveCapacity = mResourceArchiveCapacity;
	constants.archiveHashCapacity = mResourceHashCapacity;
	constants.evolutionCount = std::min(config.maximumEvolutionPerFrame,
		mResourceArchiveCapacity);
	constants.injectionCount = injectionCount;
	constants.evolutionInjectionIndex = UINT32_MAX;
	constants.cellSize = frame.fine.cellSize;
	constants.deltaTime = std::max(frame.deltaTime, 0.0f);
	constants.maximumTransportCells = std::clamp(
		config.maximumEvolutionTransportCells, 0.0f, 0.95f);
	StorageBarrier(services, frame.fine);

	if (mNeedsClear || mResourceEpoch != frame.simulationEpoch)
	{
		Dispatch(services, constants, NRISmokeDormantGridPass::Clear,
			Groups(std::max({ mResourceArchiveCapacity, mResourceHashCapacity,
				mResourceMaximumWork * 2u })));
		StorageBarrier(services, frame.fine);
		mNeedsClear = false;
		mResourceEpoch = frame.simulationEpoch;
	}
	if (constants.evolutionCount != 0u)
	{
		Dispatch(services, constants, NRISmokeDormantGridPass::Evolve,
			constants.evolutionCount);
		StorageBarrier(services, frame.fine);
	}
	if (injectionCount != 0u)
	{
		// Bring every target to the current simulation time before adding a
		// current-time cadence aggregate. Duplicate targets are harmless: the
		// first pass advances the record and later passes observe the frame stamp.
		for (uint32_t i = 0u; i < injectionCount; ++i)
		{
			constants.evolutionInjectionIndex = i;
			Dispatch(services, constants, NRISmokeDormantGridPass::Evolve, 1u);
			StorageBarrier(services, frame.fine);
		}
		constants.evolutionInjectionIndex = UINT32_MAX;
		// One group processes the bounded list serially. This intentionally
		// permits several established sources to deposit into the same record
		// without unordered float read/modify/write races or replay queues.
		Dispatch(services, constants, NRISmokeDormantGridPass::Inject, 1u);
		StorageBarrier(services, frame.fine);
	}
	mStatus.failureReason = "none";
	return true;
}

bool NRISmokeDormantGrid::RecordReadback(const NRISmokeGridServices& services,
	const NRISmokeDormantGridFrameDesc& frame)
{
	if (!services.IsRecordingValid() || mFrameSlots.empty() || mControl.buffer == nullptr)
		return false;
	FrameSlot& slot = mFrameSlots[std::min(services.queuedFrameIndex,
		(uint32_t)mFrameSlots.size() - 1u)];
	nri::BufferBarrierDesc barrier = {};
	barrier.buffer = mControl.buffer;
	barrier.before = StorageAccess();
	barrier.after = CopySourceAccess();
	nri::BarrierDesc barriers = {};
	barriers.buffers = &barrier;
	barriers.bufferNum = 1u;
	services.core->CmdBarrier(*services.commandBuffer, barriers);
	services.core->CmdCopyBuffer(*services.commandBuffer, *slot.controlReadback.buffer, 0u,
		*mControl.buffer, 0u, sizeof(NRISmokeDormantGridControlGpu));
	barrier.before = CopySourceAccess();
	barrier.after = StorageAccess();
	services.core->CmdBarrier(*services.commandBuffer, barriers);
	slot.readbackPending = true;
	slot.readbackRendererFrame = services.rendererFrame;
	slot.readbackEpoch = frame.simulationEpoch;
	return true;
}

bool NRISmokeDormantGrid::RecordFrame(const NRISmokeGridServices& services,
	const NRISmokeDormantGridConfig& config, const NRISmokeDormantGridFrameDesc& frame)
{
	return RecordPromotions(services, config, frame) &&
		RecordDemotions(services, config, frame) && RecordEvolution(services, config, frame) &&
		RecordReadback(services, frame);
}

bool NRISmokeDormantGrid::GetEvaluationStorageDescriptors(
	std::array<const nri::Descriptor*, EvaluationDescriptorCount>& descriptors) const
{
	descriptors = {};
	if (!mStatus.resourcesReady) return false;
	descriptors = { mControl.storageView, mHash.storageView, mRecords.storageView,
		mScalar.storageView, mVelocity.storageView, mOptical.storageView,
		mDynamics.storageView };
	return std::all_of(descriptors.begin(), descriptors.end(),
		[](const nri::Descriptor* descriptor) { return descriptor != nullptr; });
}

void NRISmokeDormantGrid::Reset(uint32_t simulationEpoch, const char* reason)
{
	mResourceEpoch = simulationEpoch;
	mNeedsClear = true;
	mStatus.epoch = simulationEpoch;
	mStatus.gpuStatsValid = false;
	mStatus.results.clear();
	mStatus.resetReason = reason != nullptr ? reason : "unspecified";
	for (FrameSlot& slot : mFrameSlots) slot.readbackPending = false;
}

void NRISmokeDormantGrid::DestroyResources(const NRISmokeGridServices& services)
{
	for (NRIBufferResource* resource : StorageResources()) DestroyBuffer(services, *resource);
	for (FrameSlot& slot : mFrameSlots)
	{
		DestroyBuffer(services, slot.demotionUpload);
		DestroyBuffer(services, slot.promotionUpload);
		DestroyBuffer(services, slot.injectionUpload);
		DestroyBuffer(services, slot.controlReadback);
		DestroyBuffer(services, slot.resultReadback);
		slot = {};
	}
	mResourceArchiveCapacity = mResourceHashCapacity = mResourceMaximumWork = 0u;
	mResourceEpoch = 0u;
	mResourcesInitialized = false;
	mNeedsClear = true;
	mStatus.resourcesReady = false;
}

void NRISmokeDormantGrid::Shutdown(const NRISmokeGridServices& services)
{
	DestroyResources(services);
	if (services.core != nullptr)
	{
		for (nri::Pipeline*& pipeline : mPipelines)
		{
			if (pipeline != nullptr) services.core->DestroyPipeline(pipeline);
			pipeline = nullptr;
		}
		if (mPipelineLayout != nullptr) services.core->DestroyPipelineLayout(mPipelineLayout);
	}
	mPipelineLayout = nullptr;
	mFineSet = mStorageSet = nullptr;
	mFrameSlots.clear();
	mInitialized = false;
	mStatus = {};
}
