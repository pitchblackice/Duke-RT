#include "nri_smoke_grid_lighting.h"

#include <algorithm>
#include <string>
#include <vector>

namespace
{
	constexpr uint32_t kThreads = 64u;
	const char* const kPipelineNames[] = {
		"SmokeGridLightPrepare", "SmokeGridLightBuildActive", "SmokeGridLightBuildProposals", "SmokeGridLightSeed",
		"SmokeGridLightTemporal", "SmokeGridLightBuildLinks", "SmokeGridLightFilter"
	};
	static_assert(std::size(kPipelineNames) == (size_t)NRISmokeGridLightingPass::Count);

	uint32_t Groups(uint64_t count)
	{
		return (uint32_t)std::max<uint64_t>(1u, (count + kThreads - 1u) / kThreads);
	}

	nri::AccessStage StorageAccess()
	{
		return { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	}
}

bool NRISmokeGridLighting::Initialize(const NRISmokeGridServices& services, nri::PipelineLayout* sharedLayout)
{
	if (mSharedLayout != nullptr)
		return true;
	if (!services.IsDeviceValid() || sharedLayout == nullptr)
		return false;
	mSharedLayout = sharedLayout;
	const bool d3d12 = services.graphicsAPI == nri::GraphicsAPI::D3D12;
	for (uint32_t i = 0u; i < (uint32_t)mPipelines.size(); ++i)
	{
		std::vector<uint8_t> blob;
		const std::string file = std::string(kPipelineNames[i]) + ".cs." + (d3d12 ? "dxil" : "spirv");
		if (!services.LoadShaderBlob(file.c_str(), blob))
		{
			for (nri::Pipeline*& pipeline : mPipelines) { if (pipeline != nullptr) services.core->DestroyPipeline(pipeline); pipeline = nullptr; }
			mSharedLayout = nullptr;
			mStatus.failureReason = "shader-load-failed";
			return false;
		}
		nri::ShaderDesc shader = {};
		shader.stage = nri::StageBits::COMPUTE_SHADER;
		shader.bytecode = blob.data();
		shader.size = blob.size();
		shader.entryPointName = "main";
		nri::ComputePipelineDesc desc = {};
		desc.pipelineLayout = sharedLayout;
		desc.shader = shader;
		if (services.core->CreateComputePipeline(*services.device, desc, mPipelines[i]) != nri::Result::SUCCESS)
		{
			for (nri::Pipeline*& pipeline : mPipelines) { if (pipeline != nullptr) services.core->DestroyPipeline(pipeline); pipeline = nullptr; }
			mSharedLayout = nullptr;
			mStatus.failureReason = "pipeline-create-failed";
			return false;
		}
	}
	mStatus.initialized = true;
	mStatus.failureReason = "none";
	return true;
}

bool NRISmokeGridLighting::CreateBuffer(const NRISmokeGridServices& services, NRIBufferResource& out,
	uint64_t size, uint32_t stride, nri::BufferUsageBits usage)
{
	DestroyBuffer(services, out);
	nri::BufferDesc desc = {};
	desc.size = std::max<uint64_t>(size, stride);
	desc.structureStride = stride;
	desc.usage = usage;
	if (services.core->CreateCommittedBuffer(*services.device, nri::MemoryLocation::DEVICE, 0.0f, desc, out.buffer) != nri::Result::SUCCESS)
		return false;
	nri::MemoryDesc memory = {};
	services.core->GetBufferMemoryDesc(*out.buffer, nri::MemoryLocation::DEVICE, memory);
	out.size = out.usedSize = desc.size;
	out.memorySize = memory.size;
	out.stride = stride;
	out.usage = usage;
	out.memoryLocation = nri::MemoryLocation::DEVICE;
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
	return true;
}

void NRISmokeGridLighting::DestroyBuffer(const NRISmokeGridServices& services, NRIBufferResource& resource)
{
	if (resource.storageView != nullptr) services.core->DestroyDescriptor(resource.storageView);
	if (resource.shaderView != nullptr) services.core->DestroyDescriptor(resource.shaderView);
	if (resource.buffer != nullptr) services.core->DestroyBuffer(resource.buffer);
	resource = {};
}

void NRISmokeGridLighting::DestroyResources(const NRISmokeGridServices& services)
{
	DestroyBuffer(services, mCurrent);
	DestroyBuffer(services, mHistory);
	DestroyBuffer(services, mActive);
	DestroyBuffer(services, mControl);
	DestroyBuffer(services, mLinks);
	DestroyBuffer(services, mFiltered);
	DestroyBuffer(services, mProposals);
	mResourceCellCapacity = 0u;
	mResourceBrickCapacity = 0u;
	mStatus.resourcesReady = false;
	mResourcesInitialized = false;
	mStatus.fieldBytes = mStatus.workBytes = mStatus.linkBytes = mStatus.proposalBytes = mStatus.filterBytes = mStatus.totalBytes = 0u;
}

bool NRISmokeGridLighting::EnsureResources(const NRISmokeGridServices& services, uint32_t cellCapacity, bool filterRequested)
{
	const bool ready = mCurrent.buffer != nullptr && mHistory.buffer != nullptr && mActive.buffer != nullptr &&
		mControl.buffer != nullptr && mLinks.buffer != nullptr && mProposals.buffer != nullptr && mResourceCellCapacity == cellCapacity &&
		((mFiltered.buffer != nullptr) == filterRequested);
	if (ready)
		return true;
	services.WaitForCommands("smoke-grid-lighting-resize");
	DestroyResources(services);
	const nri::BufferUsageBits storage = nri::BufferUsageBits::SHADER_RESOURCE_STORAGE;
	const uint64_t cells = std::max(cellCapacity, 1u);
	const uint64_t bricks = std::max<uint64_t>((cells + NRI_SMOKE_GRID_CELLS_PER_BRICK - 1u) / NRI_SMOKE_GRID_CELLS_PER_BRICK, 1u);
	if (!CreateBuffer(services, mCurrent, cells * sizeof(NRISmokeGridLightRecordGpu), sizeof(NRISmokeGridLightRecordGpu), storage) ||
		!CreateBuffer(services, mHistory, cells * sizeof(NRISmokeGridLightRecordGpu), sizeof(NRISmokeGridLightRecordGpu), storage) ||
		!CreateBuffer(services, mActive, cells * sizeof(uint32_t), sizeof(uint32_t), storage) ||
		!CreateBuffer(services, mControl, sizeof(NRISmokeGridLightControlGpu), sizeof(NRISmokeGridLightControlGpu), storage) ||
		!CreateBuffer(services, mLinks, cells * sizeof(uint32_t) * 4u, sizeof(uint32_t) * 4u, storage) ||
		!CreateBuffer(services, mProposals, bricks * sizeof(NRISmokeGridLightProposalGpu), sizeof(NRISmokeGridLightProposalGpu), storage) ||
		(filterRequested && !CreateBuffer(services, mFiltered, cells * sizeof(NRISmokeGridLightRecordGpu), sizeof(NRISmokeGridLightRecordGpu), storage)))
	{
		DestroyResources(services);
		mStatus.failureReason = "allocation-failed";
		return false;
	}
	// The optional descriptor remains valid while filtering is disabled without
	// allocating a third field: the accepted current field is a safe alias.
	mResourceCellCapacity = cellCapacity;
	mResourceBrickCapacity = (uint32_t)bricks;
	mStatus.cellCapacity = cellCapacity;
	mStatus.filterAllocated = mFiltered.buffer != nullptr;
	mStatus.fieldBytes = mCurrent.memorySize + mHistory.memorySize;
	mStatus.workBytes = mActive.memorySize + mControl.memorySize;
	mStatus.linkBytes = mLinks.memorySize;
	mStatus.proposalBytes = mProposals.memorySize;
	mStatus.filterBytes = mFiltered.memorySize;
	mStatus.totalBytes = mStatus.fieldBytes + mStatus.workBytes + mStatus.linkBytes + mStatus.proposalBytes + mStatus.filterBytes;
	mStatus.resourcesReady = true;
	mStatus.failureReason = "none";
	mNeedsClear = true;
	return true;
}

bool NRISmokeGridLighting::PrepareFrame(const NRISmokeGridServices& services, const NRISmokeSettings& settings,
	uint32_t cellCapacity, uint32_t frameIndex, uint32_t simulationEpoch)
{
	mStatus.requested = settings.emissiveBackend != (uint32_t)NRISmokeEmissiveBackend::Legacy;
	mStatus.requestedBackend = settings.emissiveBackend;
	mStatus.filterRequested = settings.emissiveWorldFilter;
	mStatus.filterDecision = settings.emissiveWorldFilter ? "requested" : "disabled/variance-gate-not-accepted";
	mStatus.proposalDecision = settings.emissiveLocalProposals ? "brick-top16/uniform75+global25" : "global-cdf/manual-disable";
	if (!mStatus.initialized)
	{
		mStatus.failureReason = "pipelines-unavailable";
		return false;
	}
	if (!mStatus.requested)
	{
		mStatus.effectiveBackend = (uint32_t)NRISmokeEmissiveBackend::Legacy;
		mStatus.authority = "legacy";
		return true;
	}
	if (simulationEpoch != mSimulationEpoch)
		Reset(simulationEpoch, "simulation-epoch");
	if (!EnsureResources(services, cellCapacity, settings.emissiveWorldFilter))
		return false;
	mStatus.effectiveBackend = settings.emissiveBackend == (uint32_t)NRISmokeEmissiveBackend::Compare ?
		(uint32_t)NRISmokeEmissiveBackend::Compare : (uint32_t)NRISmokeEmissiveBackend::World;
	mStatus.authority = mStatus.effectiveBackend == (uint32_t)NRISmokeEmissiveBackend::Compare ? "compare" : "world";
	mStatus.simulationEpoch = simulationEpoch;
	mStatus.lastUpdatedFrame = frameIndex;
	return true;
}

void NRISmokeGridLighting::Barrier(const NRISmokeGridServices& services)
{
	NRIBufferResource* resources[] = { &mCurrent, &mHistory, &mActive, &mControl, &mLinks, &mProposals, &mFiltered };
	const uint32_t resourceCount = mFiltered.buffer != nullptr ? 7u : 6u;
	nri::BufferBarrierDesc barriers[7] = {};
	for (uint32_t i = 0u; i < resourceCount; ++i)
	{
		barriers[i].buffer = resources[i]->buffer;
		barriers[i].before = StorageAccess();
		barriers[i].after = StorageAccess();
	}
	nri::BarrierDesc desc = {};
	desc.buffers = barriers;
	desc.bufferNum = resourceCount;
	services.core->CmdBarrier(*services.commandBuffer, desc);
}

void NRISmokeGridLighting::Dispatch(const NRISmokeGridServices& services, NRISmokeGridLightingPass pass,
	NRISmokeConstants& constants, uint32_t groups)
{
	const uint32_t index = (uint32_t)pass;
	services.core->CmdBeginAnnotation(*services.commandBuffer, kPipelineNames[index], nri::BGRA_UNUSED);
	constants.pass = index;
	services.core->CmdSetRootConstants(*services.commandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	services.core->CmdSetPipeline(*services.commandBuffer, *mPipelines[index]);
	services.core->CmdDispatch(*services.commandBuffer, { groups, 1u, 1u });
	services.core->CmdEndAnnotation(*services.commandBuffer);
}

bool NRISmokeGridLighting::Record(const NRISmokeGridServices& services, const NRISmokeSettings& settings,
	NRISmokeConstants constants, bool emissiveResourcesReady)
{
	if (!mStatus.initialized || !mStatus.resourcesReady || !emissiveResourcesReady || !services.IsRecordingValid())
		return false;
	if (mLastRecordedFrame == constants.frameIndex)
		return true;
	if (!mResourcesInitialized)
	{
		NRIBufferResource* resources[] = { &mCurrent, &mHistory, &mActive, &mControl, &mLinks, &mProposals, &mFiltered };
		const uint32_t resourceCount = mFiltered.buffer != nullptr ? 7u : 6u;
		nri::BufferBarrierDesc barriers[7] = {};
		for (uint32_t i = 0u; i < resourceCount; ++i)
		{
			barriers[i].buffer = resources[i]->buffer;
			barriers[i].after = StorageAccess();
		}
		nri::BarrierDesc desc = {};
		desc.buffers = barriers;
		desc.bufferNum = resourceCount;
		services.core->CmdBarrier(*services.commandBuffer, desc);
		mResourcesInitialized = true;
	}
	if (mNeedsClear)
		constants.flags |= 1u;
	if (mFieldPing != 0u)
		constants.flags |= 0x4000000u;
	Dispatch(services, NRISmokeGridLightingPass::Prepare, constants, 1u);
	Barrier(services);
	Dispatch(services, NRISmokeGridLightingPass::BuildActive, constants, Groups(mResourceCellCapacity));
	Barrier(services);
	if (settings.emissiveLocalProposals)
	{
		Dispatch(services, NRISmokeGridLightingPass::BuildProposals, constants, Groups(mResourceBrickCapacity));
		Barrier(services);
	}
	Dispatch(services, NRISmokeGridLightingPass::Seed, constants, Groups(mResourceCellCapacity));
	Barrier(services);
	if (settings.emissiveReuseMode >= 1u)
	{
		Dispatch(services, NRISmokeGridLightingPass::Temporal, constants, Groups(mResourceCellCapacity));
		Barrier(services);
	}
	Dispatch(services, NRISmokeGridLightingPass::BuildLinks, constants, Groups(mResourceCellCapacity));
	Barrier(services);
	if (settings.emissiveWorldFilter && mFiltered.buffer != nullptr)
	{
		Dispatch(services, NRISmokeGridLightingPass::Filter, constants, Groups(mResourceCellCapacity));
		Barrier(services);
	}
	mNeedsClear = false;
	mLastRecordedFrame = constants.frameIndex;
	mStatus.lastUpdatedFrame = constants.frameIndex;
	mStatus.fieldPing = mFieldPing;
	mFieldPing = 1u - mFieldPing;
	return true;
}

bool NRISmokeGridLighting::GetStorageDescriptors(std::array<const nri::Descriptor*, StorageDescriptorCount>& descriptors) const
{
	if (!mStatus.resourcesReady)
		return false;
	descriptors = { mCurrent.storageView, mHistory.storageView, mActive.storageView, mControl.storageView,
		mLinks.storageView, mFiltered.storageView != nullptr ? mFiltered.storageView : mCurrent.storageView,
		mProposals.storageView };
	return true;
}

void NRISmokeGridLighting::PublishGridSnapshot(
	const std::array<const nri::Descriptor*, NRISmokeGrid::EvaluationDescriptorCount>& descriptors,
	uint32_t fieldPing, float cellSize)
{
	mGridDescriptors = descriptors;
	mGridFieldPing = fieldPing;
	mGridCellSize = cellSize;
}

NRISmokeGridLightingDirectSeedSnapshot NRISmokeGridLighting::GetDirectSeedSnapshot() const
{
	NRISmokeGridLightingDirectSeedSnapshot snapshot = {};
	if (!mStatus.resourcesReady)
		return snapshot;
	snapshot.current = mCurrent.storageView;
	snapshot.history = mHistory.storageView;
	snapshot.activeCells = mActive.storageView;
	snapshot.links = mLinks.storageView;
	snapshot.control = mControl.storageView;
	snapshot.filtered = mFiltered.storageView;
	snapshot.gridControl = mGridDescriptors[0];
	snapshot.gridHash = mGridDescriptors[1];
	snapshot.gridBricks = mGridDescriptors[2];
	snapshot.scalarA = mGridDescriptors[3];
	snapshot.scalarB = mGridDescriptors[4];
	snapshot.opticalA = mGridDescriptors[7];
	snapshot.opticalB = mGridDescriptors[8];
	snapshot.cellCapacity = mResourceCellCapacity;
	snapshot.fieldPing = mGridFieldPing;
	snapshot.simulationEpoch = mSimulationEpoch;
	snapshot.cellSize = mGridCellSize;
	return snapshot;
}

void NRISmokeGridLighting::Reset(uint32_t simulationEpoch, const char* reason)
{
	(void)reason;
	mSimulationEpoch = simulationEpoch;
	mFieldPing = 0u;
	mLastRecordedFrame = UINT32_MAX;
	mNeedsClear = true;
	mStatus.simulationEpoch = simulationEpoch;
	mGridDescriptors.fill(nullptr);
	mGridFieldPing = 0u;
	mGridCellSize = 0.0f;
}

void NRISmokeGridLighting::Shutdown(const NRISmokeGridServices& services)
{
	services.WaitForCommands("smoke-grid-lighting-shutdown");
	DestroyResources(services);
	for (nri::Pipeline*& pipeline : mPipelines)
	{
		if (pipeline != nullptr) services.core->DestroyPipeline(pipeline);
		pipeline = nullptr;
	}
	mSharedLayout = nullptr;
	mStatus = {};
}
