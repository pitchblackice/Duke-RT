#include "nri_smoke_grid.h"

#include "printf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
	constexpr uint32_t kThreadGroupWidth = 64u;
	constexpr uint32_t kDispatchWordCount = 6u;

	nri::AccessStage StorageAccess()
	{
		return { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	}

	nri::AccessStage ArgumentAccess()
	{
		return { nri::AccessBits::ARGUMENT_BUFFER, nri::StageBits::INDIRECT };
	}

	uint32_t Groups(uint64_t count)
	{
		return (uint32_t)std::max<uint64_t>(1u, (count + kThreadGroupWidth - 1u) / kThreadGroupWidth);
	}

	uint32_t NextPowerOfTwo(uint32_t value)
	{
		value = std::max(value, 2u);
		value--;
		value |= value >> 1u;
		value |= value >> 2u;
		value |= value >> 4u;
		value |= value >> 8u;
		value |= value >> 16u;
		return value + 1u;
	}

	const char* const kPipelineNames[] = {
		"SmokeGridClear",
		"SmokeGridAllocateCommands",
		"SmokeGridBuildDispatch",
		"SmokeGridPrepareBricks",
		"SmokeGridDeposit",
		"SmokeGridResolveDeposit",
		"SmokeGridAllocateHalo",
		"SmokeGridBeginRebuild",
		"SmokeGridAdvectVelocity",
		"SmokeGridAdvectFields",
		"SmokeGridRebuild",
	};

	static_assert(std::size(kPipelineNames) == 11u);
}

std::array<NRIBufferResource*, NRISmokeGrid::StorageDescriptorCount> NRISmokeGrid::StorageResources()
{
	return { &mControl, &mHash, &mBricks, &mFreeList, &mActiveA, &mActiveB, &mDispatchArgs,
		&mScalarA, &mScalarB, &mVelocityA, &mVelocityB, &mOpticalA, &mOpticalB,
		&mDynamicsA, &mDynamicsB, &mDeposit0, &mDeposit1, &mDeposit2, &mDeposit3 };
}

std::array<const NRIBufferResource*, NRISmokeGrid::StorageDescriptorCount> NRISmokeGrid::StorageResources() const
{
	return { &mControl, &mHash, &mBricks, &mFreeList, &mActiveA, &mActiveB, &mDispatchArgs,
		&mScalarA, &mScalarB, &mVelocityA, &mVelocityB, &mOpticalA, &mOpticalB,
		&mDynamicsA, &mDynamicsB, &mDeposit0, &mDeposit1, &mDeposit2, &mDeposit3 };
}

void NRISmokeGrid::SetFailure(const char* reason)
{
	mStatus.resourcesReady = false;
	mStatus.failureReason = reason != nullptr ? reason : "unspecified";
}

bool NRISmokeGrid::CreateBuffer(const NRISmokeGridServices& services, NRIBufferResource& out,
	uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::MemoryLocation location, bool storageView)
{
	DestroyBuffer(services, out);
	if (!services.IsDeviceValid() || stride == 0u)
		return false;

	nri::BufferDesc desc = {};
	desc.size = std::max<uint64_t>(size, stride);
	desc.structureStride = stride;
	desc.usage = usage;
	if (services.core->CreateCommittedBuffer(*services.device, location, 0.0f, desc, out.buffer) != nri::Result::SUCCESS)
		return false;

	nri::MemoryDesc memory = {};
	services.core->GetBufferMemoryDesc(*out.buffer, location, memory);
	out.size = desc.size;
	out.usedSize = desc.size;
	out.memorySize = memory.size;
	out.stride = stride;
	out.usage = usage;
	out.memoryLocation = location;

	if (storageView)
	{
		nri::BufferViewDesc view = {};
		view.buffer = out.buffer;
		view.type = nri::BufferView::STORAGE_STRUCTURED_BUFFER;
		view.offset = 0;
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

void NRISmokeGrid::DestroyBuffer(const NRISmokeGridServices& services, NRIBufferResource& resource)
{
	if (services.core != nullptr)
	{
		if (resource.shaderView != nullptr)
			services.core->DestroyDescriptor(resource.shaderView);
		if (resource.storageView != nullptr)
			services.core->DestroyDescriptor(resource.storageView);
		if (resource.buffer != nullptr)
			services.core->DestroyBuffer(resource.buffer);
	}
	resource = {};
}

bool NRISmokeGrid::Initialize(const NRISmokeGridServices& services)
{
	if (mInitialized)
		return true;
	if (!services.IsDeviceValid() || services.loadShaderBlob == nullptr || services.queuedFrameCount == 0u)
	{
		SetFailure("invalid-services");
		return false;
	}

	nri::DescriptorRangeDesc inputRange = {};
	inputRange.baseRegisterIndex = 0;
	inputRange.descriptorNum = 2;
	inputRange.descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	inputRange.shaderStages = nri::StageBits::COMPUTE_SHADER;
	inputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc storageRange = {};
	storageRange.baseRegisterIndex = 0;
	storageRange.descriptorNum = StorageDescriptorCount;
	storageRange.descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
	storageRange.shaderStages = nri::StageBits::COMPUTE_SHADER;
	storageRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorSetDesc sets[2] = {};
	sets[0].registerSpace = 0;
	sets[0].ranges = &inputRange;
	sets[0].rangeNum = 1;
	sets[0].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	sets[1].registerSpace = 1;
	sets[1].ranges = &storageRange;
	sets[1].rangeNum = 1;
	sets[1].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;

	nri::RootConstantDesc root = {};
	root.registerIndex = 0;
	root.size = sizeof(NRISmokeGridConstants);
	root.shaderStages = nri::StageBits::COMPUTE_SHADER;
	nri::PipelineLayoutDesc layout = {};
	layout.rootRegisterSpace = 2;
	layout.rootConstants = &root;
	layout.rootConstantNum = 1;
	layout.descriptorSets = sets;
	layout.descriptorSetNum = 2;
	layout.shaderStages = nri::StageBits::COMPUTE_SHADER;
	if (services.core->CreatePipelineLayout(*services.device, layout, mPipelineLayout) != nri::Result::SUCCESS)
	{
		SetFailure("pipeline-layout");
		return false;
	}

	const bool d3d12 = services.graphicsAPI == nri::GraphicsAPI::D3D12;
	for (uint32_t i = 0; i < mPipelines.size(); ++i)
	{
		std::vector<uint8_t> blob;
		const std::string fileName = std::string(kPipelineNames[i]) + ".cs." + (d3d12 ? "dxil" : "spirv");
		if (!services.LoadShaderBlob(fileName.c_str(), blob))
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
		if (services.core->CreateComputePipeline(*services.device, pipeline, mPipelines[i]) != nri::Result::SUCCESS)
		{
			SetFailure("compute-pipeline");
			Shutdown(services);
			return false;
		}
	}

	if (services.core->AllocateDescriptorSets(*services.descriptorPool, *mPipelineLayout, 1,
		&mStorageSet, 1, 0) != nri::Result::SUCCESS)
	{
		SetFailure("storage-descriptor-set");
		Shutdown(services);
		return false;
	}
	mFrameSlots.resize(services.queuedFrameCount);
	for (FrameSlot& slot : mFrameSlots)
	{
		if (services.core->AllocateDescriptorSets(*services.descriptorPool, *mPipelineLayout, 0,
			&slot.inputSet, 1, 0) != nri::Result::SUCCESS)
		{
			SetFailure("input-descriptor-set");
			Shutdown(services);
			return false;
		}
	}

	mInitialized = true;
	mStatus.initialized = true;
	mStatus.failureReason = "none";
	return true;
}

bool NRISmokeGrid::EnsureResources(const NRISmokeGridServices& services, const NRISmokeSettings& settings)
{
	const uint32_t brickCapacity = std::clamp(settings.gridBrickCapacity, 64u, 4096u);
	const uint32_t hashCapacity = NextPowerOfTwo(brickCapacity * 2u);
	const uint32_t cellCapacity = brickCapacity * NRI_SMOKE_GRID_CELLS_PER_BRICK;
	if (mControl.buffer != nullptr && mResourceBrickCapacity == brickCapacity &&
		mResourceHashCapacity == hashCapacity && mResourceCellCapacity == cellCapacity)
	{
		if (mResourceCellSize != settings.gridCellSize)
		{
			mResourceCellSize = settings.gridCellSize;
			mNeedsClear = true;
			mActivePing = 0;
			mFieldPing = 0;
			mStatus.resetReason = "grid-cell-size";
		}
		return true;
	}

	services.WaitForCommands("smoke-grid-resource-layout");
	DestroyResources(services);
	const nri::BufferUsageBits storage = nri::BufferUsageBits::SHADER_RESOURCE_STORAGE;
	const nri::BufferUsageBits indirectStorage = NRIResourceFlags(storage, nri::BufferUsageBits::ARGUMENT_BUFFER);
	const uint64_t cells = cellCapacity;
	bool created =
		CreateBuffer(services, mControl, sizeof(NRISmokeGridControlGpu), sizeof(NRISmokeGridControlGpu), storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mHash, (uint64_t)hashCapacity * sizeof(NRISmokeGridHashEntryGpu), sizeof(NRISmokeGridHashEntryGpu), storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mBricks, (uint64_t)brickCapacity * sizeof(NRISmokeGridBrickGpu), sizeof(NRISmokeGridBrickGpu), storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mFreeList, (uint64_t)brickCapacity * sizeof(uint32_t), sizeof(uint32_t), storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mActiveA, (uint64_t)brickCapacity * sizeof(uint32_t), sizeof(uint32_t), storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mActiveB, (uint64_t)brickCapacity * sizeof(uint32_t), sizeof(uint32_t), storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mDispatchArgs, kDispatchWordCount * sizeof(uint32_t), sizeof(uint32_t), indirectStorage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mScalarA, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mScalarB, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mVelocityA, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mVelocityB, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mOpticalA, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mOpticalB, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mDynamicsA, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mDynamicsB, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mDeposit0, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mDeposit1, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mDeposit2, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mDeposit3, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true);

	for (FrameSlot& slot : mFrameSlots)
	{
		created = created && CreateBuffer(services, slot.controlReadback, sizeof(NRISmokeGridControlGpu),
			sizeof(NRISmokeGridControlGpu), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, false);
		slot.readbackPending = false;
		slot.readbackInitialized = false;
	}
	if (!created)
	{
		DestroyResources(services);
		SetFailure("resource-allocation");
		return false;
	}

	const auto resources = StorageResources();
	std::array<const nri::Descriptor*, StorageDescriptorCount> descriptors = {};
	for (uint32_t i = 0; i < descriptors.size(); ++i)
		descriptors[i] = resources[i]->storageView;
	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = mStorageSet;
	update.rangeIndex = 0;
	update.descriptors = descriptors.data();
	update.descriptorNum = (uint32_t)descriptors.size();
	services.core->UpdateDescriptorRanges(&update, 1);

	mResourceBrickCapacity = brickCapacity;
	mResourceHashCapacity = hashCapacity;
	mResourceCellCapacity = cellCapacity;
	mResourceCellSize = settings.gridCellSize;
	mResourceEpoch = 0;
	mActivePing = 0;
	mFieldPing = 0;
	mNeedsClear = true;
	mResourcesInitialized = false;
	mDispatchIsArgument = false;
	mStatus.brickCapacity = brickCapacity;
	mStatus.hashCapacity = hashCapacity;
	mStatus.cellCapacity = cellCapacity;
	mStatus.residentBytes = 0;
	for (const NRIBufferResource* resource : StorageResources())
		mStatus.residentBytes += resource->memorySize;
	for (const FrameSlot& slot : mFrameSlots)
		mStatus.residentBytes += slot.controlReadback.memorySize;
	mStatus.resourcesReady = true;
	mStatus.failureReason = "none";
	return true;
}

void NRISmokeGrid::ConsumeReadback(const NRISmokeGridServices& services)
{
	if (mFrameSlots.empty() || services.core == nullptr)
		return;
	FrameSlot& slot = mFrameSlots[std::min(services.queuedFrameIndex, (uint32_t)mFrameSlots.size() - 1u)];
	if (!slot.readbackPending || slot.controlReadback.buffer == nullptr)
		return;
	const void* mapped = services.core->MapBuffer(*slot.controlReadback.buffer, 0, sizeof(NRISmokeGridControlGpu));
	if (mapped != nullptr)
	{
		std::memcpy(&mStatus.gpu, mapped, sizeof(mStatus.gpu));
		services.core->UnmapBuffer(*slot.controlReadback.buffer);
		mStatus.gpuStatsValid = true;
		mStatus.controlReadbackBytes += sizeof(NRISmokeGridControlGpu);
	}
	slot.readbackPending = false;
}

bool NRISmokeGrid::PrepareFrame(const NRISmokeGridServices& services, const NRISmokeSettings& settings,
	uint32_t frameIndex, uint32_t simulationEpoch)
{
	(void)frameIndex;
	ConsumeReadback(services);
	mStatus.requested = settings.enabled && settings.representation != 0u;
	mStatus.representation = settings.representation;
	if (!mStatus.requested)
	{
		mStatus.failureReason = "not-requested";
		return true;
	}
	if (!Initialize(services) || !EnsureResources(services, settings))
		return false;
	if (mResourceEpoch != 0u && mResourceEpoch != simulationEpoch)
		Reset(simulationEpoch, "simulation-epoch");
	mStatus.activePing = mActivePing;
	mStatus.fieldPing = mFieldPing;
	return true;
}

void NRISmokeGrid::TransitionResourcesToStorage(const NRISmokeGridServices& services)
{
	const auto resources = StorageResources();
	std::array<nri::BufferBarrierDesc, StorageDescriptorCount> barriers = {};
	for (uint32_t i = 0; i < barriers.size(); ++i)
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
	mDispatchIsArgument = false;
}

void NRISmokeGrid::StorageBarrier(const NRISmokeGridServices& services)
{
	const auto resources = StorageResources();
	std::array<nri::BufferBarrierDesc, StorageDescriptorCount - 1u> barriers = {};
	uint32_t count = 0;
	for (const NRIBufferResource* resource : resources)
	{
		if (resource == &mDispatchArgs)
			continue;
		barriers[count].buffer = resource->buffer;
		barriers[count].before = StorageAccess();
		barriers[count].after = StorageAccess();
		count++;
	}
	nri::BarrierDesc barrier = {};
	barrier.buffers = barriers.data();
	barrier.bufferNum = count;
	services.core->CmdBarrier(*services.commandBuffer, barrier);
}

void NRISmokeGrid::TransitionDispatchToArgument(const NRISmokeGridServices& services)
{
	if (mDispatchIsArgument)
		return;
	nri::BufferBarrierDesc buffer = {};
	buffer.buffer = mDispatchArgs.buffer;
	buffer.before = StorageAccess();
	buffer.after = ArgumentAccess();
	nri::BarrierDesc barrier = {};
	barrier.buffers = &buffer;
	barrier.bufferNum = 1;
	services.core->CmdBarrier(*services.commandBuffer, barrier);
	mDispatchIsArgument = true;
}

void NRISmokeGrid::TransitionDispatchToStorage(const NRISmokeGridServices& services)
{
	if (!mDispatchIsArgument)
		return;
	nri::BufferBarrierDesc buffer = {};
	buffer.buffer = mDispatchArgs.buffer;
	buffer.before = ArgumentAccess();
	buffer.after = StorageAccess();
	nri::BarrierDesc barrier = {};
	barrier.buffers = &buffer;
	barrier.bufferNum = 1;
	services.core->CmdBarrier(*services.commandBuffer, barrier);
	mDispatchIsArgument = false;
}

void NRISmokeGrid::Dispatch(const NRISmokeGridServices& services, NRISmokeGridConstants& constants,
	NRISmokeGridPass pass, uint32_t x, uint32_t y, uint32_t z)
{
	services.core->CmdBeginAnnotation(*services.commandBuffer, kPipelineNames[(uint32_t)pass], nri::BGRA_UNUSED);
	constants.pass = (uint32_t)pass;
	services.core->CmdSetRootConstants(*services.commandBuffer,
		{ 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	services.core->CmdSetPipeline(*services.commandBuffer, *mPipelines[(uint32_t)pass]);
	services.core->CmdDispatch(*services.commandBuffer, { x, y, z });
	services.core->CmdEndAnnotation(*services.commandBuffer);
}

void NRISmokeGrid::DispatchIndirect(const NRISmokeGridServices& services, NRISmokeGridConstants& constants,
	NRISmokeGridPass pass, uint64_t byteOffset)
{
	TransitionDispatchToArgument(services);
	services.core->CmdBeginAnnotation(*services.commandBuffer, kPipelineNames[(uint32_t)pass], nri::BGRA_UNUSED);
	constants.pass = (uint32_t)pass;
	services.core->CmdSetRootConstants(*services.commandBuffer,
		{ 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	services.core->CmdSetPipeline(*services.commandBuffer, *mPipelines[(uint32_t)pass]);
	services.core->CmdDispatchIndirect(*services.commandBuffer, *mDispatchArgs.buffer, byteOffset);
	services.core->CmdEndAnnotation(*services.commandBuffer);
}

bool NRISmokeGrid::RecordControlReadback(const NRISmokeGridServices& services, const NRISmokeSettings& settings)
{
	if (!settings.readback || mFrameSlots.empty())
		return true;
	FrameSlot& slot = mFrameSlots[std::min(services.queuedFrameIndex, (uint32_t)mFrameSlots.size() - 1u)];
	if (slot.controlReadback.buffer == nullptr)
		return false;

	nri::BufferBarrierDesc before[2] = {};
	before[0].buffer = mControl.buffer;
	before[0].before = StorageAccess();
	before[0].after = NRIResourceCopySourceAccess();
	before[1].buffer = slot.controlReadback.buffer;
	before[1].before = slot.readbackInitialized ? NRIResourceCopyDestinationAccess() : nri::AccessStage{};
	before[1].after = NRIResourceCopyDestinationAccess();
	nri::BarrierDesc beforeCopy = {};
	beforeCopy.buffers = before;
	beforeCopy.bufferNum = 2;
	services.core->CmdBarrier(*services.commandBuffer, beforeCopy);
	services.core->CmdCopyBuffer(*services.commandBuffer, *slot.controlReadback.buffer, 0,
		*mControl.buffer, 0, sizeof(NRISmokeGridControlGpu));

	nri::BufferBarrierDesc restore = {};
	restore.buffer = mControl.buffer;
	restore.before = NRIResourceCopySourceAccess();
	restore.after = StorageAccess();
	nri::BarrierDesc afterCopy = {};
	afterCopy.buffers = &restore;
	afterCopy.bufferNum = 1;
	services.core->CmdBarrier(*services.commandBuffer, afterCopy);
	slot.readbackPending = true;
	slot.readbackInitialized = true;
	return true;
}

bool NRISmokeGrid::RecordFrame(const NRISmokeGridServices& services, const NRISmokeSettings& settings,
	const NRISmokeGridFrameDesc& frame)
{
	if (!mStatus.requested)
		return true;
	if (!services.IsRecordingValid() || !mInitialized || !mStatus.resourcesReady ||
		mFrameSlots.empty() || frame.styleView == nullptr || frame.commandView == nullptr)
	{
		SetFailure("record-prerequisite");
		return false;
	}

	FrameSlot& slot = mFrameSlots[std::min(services.queuedFrameIndex, (uint32_t)mFrameSlots.size() - 1u)];
	const nri::Descriptor* inputs[] = { frame.styleView, frame.commandView };
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = slot.inputSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputs;
	inputUpdate.descriptorNum = 2;
	services.core->UpdateDescriptorRanges(&inputUpdate, 1);

	TransitionResourcesToStorage(services);
	services.core->CmdSetPipelineLayout(*services.commandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	services.core->CmdSetDescriptorSet(*services.commandBuffer, { 0, slot.inputSet, nri::BindPoint::COMPUTE });
	services.core->CmdSetDescriptorSet(*services.commandBuffer, { 1, mStorageSet, nri::BindPoint::COMPUTE });

	NRISmokeGridConstants constants = {};
	constants.frameIndex = frame.frameIndex;
	constants.simulationEpoch = frame.simulationEpoch;
	constants.commandCount = frame.commandCount;
	constants.styleCount = frame.styleCount;
	constants.brickCapacity = mResourceBrickCapacity;
	constants.hashCapacity = mResourceHashCapacity;
	constants.cellCapacity = mResourceCellCapacity;
	constants.activePing = mActivePing;
	constants.fieldPing = mFieldPing;
	constants.representation = settings.representation;
	constants.cellSize = settings.gridCellSize;
	constants.deltaTime = std::max(frame.simulationStep, 0.0f);
	constants.timeScale = settings.timeScale;
	// A backtrace outside the source brick cannot be sampled coherently until
	// a wider halo contract exists. Keep the initial solver inside one brick.
	constants.maxBacktrace = std::min(settings.gridMaxBacktrace,
		settings.gridCellSize * (float)NRI_SMOKE_GRID_BRICK_AXIS);
	std::copy(settings.wind, settings.wind + 3, constants.wind);
	constants.buoyancy = settings.gridBuoyancy;
	constants.velocityDamping = settings.gridVelocityDamping;
	constants.windCoupling = settings.gridWindCoupling;
	constants.densityHalfLifeScale = settings.gridDensityHalfLifeScale;
	constants.coolingScale = settings.gridCoolingScale;
	constants.maxVelocity = settings.gridMaxVelocity;
	constants.activeThreshold = settings.gridActiveThreshold;
	constants.reclaimGrace = settings.gridReclaimGrace;
	constants.massQuantization = 4096.0f;
	constants.momentumQuantization = 256.0f;

	if (mNeedsClear || mResourceEpoch != frame.simulationEpoch)
	{
		mActivePing = 0;
		mFieldPing = 0;
		constants.activePing = 0;
		constants.fieldPing = 0;
		Dispatch(services, constants, NRISmokeGridPass::Clear,
			Groups(std::max(mResourceHashCapacity, mResourceBrickCapacity)));
		StorageBarrier(services);
		mNeedsClear = false;
		mResourceEpoch = frame.simulationEpoch;
	}

	if (frame.commandCount > 0u)
	{
		// Allocation is a serial GPU control-plane pass. This avoids duplicate
		// open-addressed claims while field population remains fully parallel.
		Dispatch(services, constants, NRISmokeGridPass::AllocateCommands, 1u);
		StorageBarrier(services);
	}
	Dispatch(services, constants, NRISmokeGridPass::BuildDispatch, 1u);
	StorageBarrier(services);
	DispatchIndirect(services, constants, NRISmokeGridPass::PrepareBricks);
	StorageBarrier(services);
	if (frame.commandCount > 0u)
	{
		Dispatch(services, constants, NRISmokeGridPass::Deposit, frame.commandCount);
		StorageBarrier(services);
		DispatchIndirect(services, constants, NRISmokeGridPass::ResolveDeposit);
		StorageBarrier(services);
	}

	for (uint32_t step = 0; step < frame.simulationSubsteps; ++step)
	{
		// Allocation is deliberately one serial GPU control-plane dispatch. It
		// snapshots and walks the current active list without CPU involvement.
		Dispatch(services, constants, NRISmokeGridPass::AllocateHalo, 1u);
		StorageBarrier(services);
		TransitionDispatchToStorage(services);
		Dispatch(services, constants, NRISmokeGridPass::BuildDispatch, 1u);
		StorageBarrier(services);
		DispatchIndirect(services, constants, NRISmokeGridPass::PrepareBricks);
		StorageBarrier(services);
		Dispatch(services, constants, NRISmokeGridPass::BeginRebuild, 1u);
		StorageBarrier(services);
		DispatchIndirect(services, constants, NRISmokeGridPass::AdvectVelocity);
		StorageBarrier(services);
		DispatchIndirect(services, constants, NRISmokeGridPass::AdvectFields);
		StorageBarrier(services);
		DispatchIndirect(services, constants, NRISmokeGridPass::Rebuild);
		StorageBarrier(services);

		mActivePing ^= 1u;
		mFieldPing ^= 1u;
		constants.activePing = mActivePing;
		constants.fieldPing = mFieldPing;
	}

	TransitionDispatchToStorage(services);
	if (frame.simulationSubsteps > 0u)
	{
		// Publish the final ping/count for render evaluation. Without this last
		// control update, the camera pass would sample the previous field.
		Dispatch(services, constants, NRISmokeGridPass::BuildDispatch, 1u);
		StorageBarrier(services);
	}
	if (!RecordControlReadback(services, settings))
	{
		SetFailure("control-readback");
		return false;
	}
	mStatus.activePing = mActivePing;
	mStatus.fieldPing = mFieldPing;
	mStatus.resourcesReady = true;
	mStatus.failureReason = "none";
	return true;
}

bool NRISmokeGrid::GetEvaluationStorageDescriptors(
	std::array<const nri::Descriptor*, EvaluationDescriptorCount>& descriptors) const
{
	descriptors = { mControl.storageView, mHash.storageView, mBricks.storageView,
		mScalarA.storageView, mScalarB.storageView, mVelocityA.storageView, mVelocityB.storageView,
		mOpticalA.storageView, mOpticalB.storageView, mDynamicsA.storageView, mDynamicsB.storageView };
	return mStatus.resourcesReady && std::all_of(descriptors.begin(), descriptors.end(),
		[](const nri::Descriptor* descriptor) { return descriptor != nullptr; });
}

void NRISmokeGrid::Reset(uint32_t simulationEpoch, const char* reason)
{
	mResourceEpoch = simulationEpoch;
	mActivePing = 0;
	mFieldPing = 0;
	mNeedsClear = true;
	mStatus.activePing = 0;
	mStatus.fieldPing = 0;
	mStatus.gpuStatsValid = false;
	mStatus.gpu = {};
	mStatus.resetReason = reason != nullptr ? reason : "unspecified";
}

void NRISmokeGrid::DestroyResources(const NRISmokeGridServices& services)
{
	for (NRIBufferResource* resource : StorageResources())
		DestroyBuffer(services, *resource);
	for (FrameSlot& slot : mFrameSlots)
	{
		DestroyBuffer(services, slot.controlReadback);
		slot.readbackPending = false;
		slot.readbackInitialized = false;
	}
	mResourceBrickCapacity = 0;
	mResourceHashCapacity = 0;
	mResourceCellCapacity = 0;
	mResourceCellSize = 0.0f;
	mResourceEpoch = 0;
	mResourcesInitialized = false;
	mDispatchIsArgument = false;
	mStatus.resourcesReady = false;
	mStatus.residentBytes = 0;
}

void NRISmokeGrid::Shutdown(const NRISmokeGridServices& services)
{
	if (mInitialized || mControl.buffer != nullptr)
		services.WaitForCommands("smoke-grid-shutdown");
	DestroyResources(services);
	if (services.core != nullptr)
	{
		for (nri::Pipeline*& pipeline : mPipelines)
		{
			if (pipeline != nullptr)
				services.core->DestroyPipeline(pipeline);
			pipeline = nullptr;
		}
		if (mPipelineLayout != nullptr)
			services.core->DestroyPipelineLayout(mPipelineLayout);
	}
	mPipelineLayout = nullptr;
	mStorageSet = nullptr;
	mFrameSlots.clear();
	mInitialized = false;
	mNeedsClear = true;
	mStatus.initialized = false;
}

void NRISmokeGrid::PrintStatus() const
{
	Printf("NRI PT smoke grid status: requested=%s representation=%u initialized=%s resources=%s "
		"bricks=%u hash=%u cells=%u active_ping=%u field_ping=%u gpu_stats=%s "
		"resident=%u active=%u/%u free=%u allocated=%u reclaimed=%u allocation_failures=%u "
		"probe_failures=%u max_probe=%u commands=%u requested_mass_q=%u deposited_mass_q=%u "
		"rejected_mass_q=%u saturated=%u halo=%u occupied=%u empty=%u cfl_clamps=%u "
		"backtrace_clamps=%u nan=%u field_hash=%08x%08x resident_mib=%.2f "
		"field_readback=0 control_readback=%llu fallback=%s reset=%s\n",
		mStatus.requested ? "yes" : "no", mStatus.representation,
		mStatus.initialized ? "yes" : "no", mStatus.resourcesReady ? "ready" : "unavailable",
		mStatus.brickCapacity, mStatus.hashCapacity, mStatus.cellCapacity,
		mStatus.activePing, mStatus.fieldPing, mStatus.gpuStatsValid ? "valid" : "disabled",
		mStatus.gpu.residentCount, mStatus.gpu.activeCountA, mStatus.gpu.activeCountB,
		mStatus.gpu.freeCount, mStatus.gpu.allocated, mStatus.gpu.reclaimed,
		mStatus.gpu.allocationFailures, mStatus.gpu.probeFailures, mStatus.gpu.maximumProbe,
		mStatus.gpu.commandsProcessed, mStatus.gpu.requestedMassQ, mStatus.gpu.depositedMassQ,
		mStatus.gpu.rejectedMassQ, mStatus.gpu.saturatedDeposits, mStatus.gpu.haloAllocations,
		mStatus.gpu.occupiedBricks, mStatus.gpu.emptyBricks, mStatus.gpu.cflClamps,
		mStatus.gpu.backtraceClamps, mStatus.gpu.nanRejects,
		mStatus.gpu.fieldHashHi, mStatus.gpu.fieldHashLo,
		(double)mStatus.residentBytes / (1024.0 * 1024.0),
		(unsigned long long)mStatus.controlReadbackBytes,
		mStatus.failureReason.c_str(), mStatus.resetReason.c_str());
}
