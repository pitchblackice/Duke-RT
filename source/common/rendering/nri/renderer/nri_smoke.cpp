#include "nri_smoke.h"

#include "nri_pass_dispatch.h"
#include "nri_renderer.h"
#include "../system/nri_renderdevice.h"
#include "gamecontrol.h"
#include "printf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace
{
	constexpr uint32_t kThreads = 64;
	constexpr uint32_t kMaxCommands = 256;

	uint32_t Groups(uint64_t count)
	{
		return (uint32_t)std::max<uint64_t>(1, (count + kThreads - 1) / kThreads);
	}

}

bool NRISmokeSystem::CreateBuffer(NRIRenderer& renderer, NRIBufferResource& out, uint64_t size, uint32_t stride,
		nri::BufferUsageBits usage, nri::MemoryLocation location, bool srv, bool uav)
	{
		renderer.DestroyBufferResource(out);
		nri::BufferDesc desc = {};
		desc.size = std::max<uint64_t>(size, stride);
		desc.structureStride = stride;
		desc.usage = usage;
		if (renderer.mFrameBuffer->mCore.CreateCommittedBuffer(*renderer.mFrameBuffer->mDevice, location, 0.0f, desc, out.buffer) != nri::Result::SUCCESS)
			return false;
		nri::MemoryDesc memory = {};
		renderer.mFrameBuffer->mCore.GetBufferMemoryDesc(*out.buffer, location, memory);
		out.size = desc.size;
		out.usedSize = desc.size;
		out.memorySize = memory.size;
		out.stride = stride;
		out.usage = usage;
		out.memoryLocation = location;

		auto createView = [&](nri::BufferView type, nri::Descriptor*& descriptor)
		{
			nri::BufferViewDesc view = {};
			view.buffer = out.buffer;
			view.type = type;
			view.offset = 0;
			view.size = nri::WHOLE_SIZE;
			view.structureStride = stride;
			return renderer.mFrameBuffer->mCore.CreateBufferView(view, descriptor) == nri::Result::SUCCESS;
		};
		if ((srv && !createView(nri::BufferView::STRUCTURED_BUFFER, out.shaderView)) ||
			(uav && !createView(nri::BufferView::STORAGE_STRUCTURED_BUFFER, out.storageView)))
		{
			renderer.DestroyBufferResource(out);
			return false;
		}
		return true;
	}

bool NRISmokeSystem::UploadBytes(NRIRenderer& renderer, NRIBufferResource& upload, const void* data, uint64_t size)
	{
		void* mapped = renderer.mFrameBuffer->mCore.MapBuffer(*upload.buffer, 0, size);
		if (mapped == nullptr)
			return false;
		std::memcpy(mapped, data, (size_t)size);
		renderer.mFrameBuffer->mCore.UnmapBuffer(*upload.buffer);
		return true;
	}

namespace
{
	nri::AccessStage StorageAccess()
	{
		return { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	}
}

bool NRISmokeSystem::Initialize(NRIRenderer& renderer)
{
	if (mPipelineLayout != nullptr)
		return true;

	nri::DescriptorRangeDesc input = {};
	input.baseRegisterIndex = 0;
	input.descriptorNum = 2;
	input.descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	input.shaderStages = nri::StageBits::COMPUTE_SHADER;
	input.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	nri::DescriptorRangeDesc buffers = {};
	buffers.baseRegisterIndex = 0;
	buffers.descriptorNum = 6;
	buffers.descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
	buffers.shaderStages = nri::StageBits::COMPUTE_SHADER;
	buffers.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	nri::DescriptorRangeDesc textures = {};
	textures.baseRegisterIndex = 0;
	textures.descriptorNum = 2;
	textures.descriptorType = nri::DescriptorType::TEXTURE;
	textures.shaderStages = nri::StageBits::COMPUTE_SHADER;
	textures.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	nri::DescriptorRangeDesc output = {};
	output.baseRegisterIndex = 0;
	output.descriptorNum = 1;
	output.descriptorType = nri::DescriptorType::STORAGE_TEXTURE;
	output.shaderStages = nri::StageBits::COMPUTE_SHADER;
	output.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	nri::DescriptorSetDesc sets[4] = {};
	nri::DescriptorRangeDesc* ranges[] = { &input, &buffers, &textures, &output };
	for (uint32_t i = 0; i < 4; ++i)
	{
		sets[i].registerSpace = i;
		sets[i].ranges = ranges[i];
		sets[i].rangeNum = 1;
		sets[i].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	}
	nri::RootConstantDesc root = {};
	root.registerIndex = 0;
	root.size = sizeof(NRISmokeConstants);
	root.shaderStages = nri::StageBits::COMPUTE_SHADER;
	nri::PipelineLayoutDesc layout = {};
	layout.rootRegisterSpace = 4;
	layout.rootConstants = &root;
	layout.rootConstantNum = 1;
	layout.descriptorSets = sets;
	layout.descriptorSetNum = 4;
	layout.shaderStages = nri::StageBits::COMPUTE_SHADER;
	if (renderer.mFrameBuffer->mCore.CreatePipelineLayout(*renderer.mFrameBuffer->mDevice, layout, mPipelineLayout) != nri::Result::SUCCESS)
		return false;

	const bool d3d12 = renderer.mFrameBuffer->GetSelectedAPI() == nri::GraphicsAPI::D3D12;
	const char* names[] = { "SmokeClear", "SmokeSimulate", "SmokeSpawn", "SmokeBin", "SmokeEvaluate", "SmokeIntegrate", "SmokeComposite" };
	for (uint32_t i = 0; i < (uint32_t)mPipelines.size(); ++i)
	{
		std::vector<uint8_t> blob;
		const std::string file = std::string(names[i]) + ".cs." + (d3d12 ? "dxil" : "spirv");
		if (!renderer.mFrameBuffer->LoadShaderBlob(file.c_str(), blob))
			return false;
		nri::ShaderDesc shader = {};
		shader.stage = nri::StageBits::COMPUTE_SHADER;
		shader.bytecode = blob.data();
		shader.size = blob.size();
		shader.entryPointName = "main";
		nri::ComputePipelineDesc pipeline = {};
		pipeline.pipelineLayout = mPipelineLayout;
		pipeline.shader = shader;
		if (renderer.mFrameBuffer->mCore.CreateComputePipeline(*renderer.mFrameBuffer->mDevice, pipeline, mPipelines[i]) != nri::Result::SUCCESS)
			return false;
	}

	const uint32_t queued = std::max(1u, (uint32_t)renderer.mFrameBuffer->mQueuedFrames.size());
	mCommandSlots.resize(queued);
	for (CommandSlot& slot : mCommandSlots)
	{
		if (renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *mPipelineLayout, 0, &slot.inputSet, 1, 0) != nri::Result::SUCCESS ||
			renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *mPipelineLayout, 1, &slot.bufferSet, 1, 0) != nri::Result::SUCCESS ||
			renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *mPipelineLayout, 2, &slot.textureSet, 1, 0) != nri::Result::SUCCESS ||
			renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *mPipelineLayout, 3, &slot.outputSet, 1, 0) != nri::Result::SUCCESS)
			return false;
	}
	return true;
}

bool NRISmokeSystem::EnsureResources(NRIRenderer& renderer)
{
	const uint32_t fw = (renderer.mRenderWidth + mSettings.froxelPixelSize - 1) / mSettings.froxelPixelSize;
	const uint32_t fh = (renderer.mRenderHeight + mSettings.froxelPixelSize - 1) / mSettings.froxelPixelSize;
	if (mParticles.buffer != nullptr && mResourceParticleCapacity == mSettings.particleCapacity &&
		mResourceFroxelWidth == fw && mResourceFroxelHeight == fh && mResourceFroxelDepth == mSettings.froxelDepth &&
		mResourceColumnCapacity == mSettings.columnCapacity && mResourceStyleCapacity == (uint32_t)mStyles.size())
		return true;

	renderer.WaitForCommandsTracked();
	DestroyResources(renderer);
	const nri::BufferUsageBits storage = NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::BufferUsageBits::SHADER_RESOURCE);
	const nri::BufferUsageBits copyDevice = nri::BufferUsageBits::SHADER_RESOURCE;
	const uint64_t columns = (uint64_t)fw * fh;
	const uint64_t froxels = columns * mSettings.froxelDepth;
	if (!CreateBuffer(renderer, mParticles, (uint64_t)mSettings.particleCapacity * sizeof(NRISmokeParticleGpu), sizeof(NRISmokeParticleGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mControl, sizeof(NRISmokeControlGpu), sizeof(NRISmokeControlGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mColumnCounts, columns * sizeof(uint32_t), sizeof(uint32_t), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mColumnIndices, columns * mSettings.columnCapacity * sizeof(uint32_t), sizeof(uint32_t), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mFroxelLocal, froxels * 16, 16, storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mFroxelIntegrated, froxels * 16, 16, storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mStyleBuffer, std::max<size_t>(1, mStyles.size()) * sizeof(NRISmokeStyleGpu), sizeof(NRISmokeStyleGpu), copyDevice, nri::MemoryLocation::DEVICE, true, false) ||
		!CreateBuffer(renderer, mStyleUpload, std::max<size_t>(1, mStyles.size()) * sizeof(NRISmokeStyleGpu), sizeof(NRISmokeStyleGpu), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_UPLOAD, false, false))
		return false;
	for (CommandSlot& slot : mCommandSlots)
	{
		if (!CreateBuffer(renderer, slot.upload, kMaxCommands * sizeof(NRISmokeInjectionCommandGpu), sizeof(NRISmokeInjectionCommandGpu), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_UPLOAD, false, false) ||
			!CreateBuffer(renderer, slot.device, kMaxCommands * sizeof(NRISmokeInjectionCommandGpu), sizeof(NRISmokeInjectionCommandGpu), copyDevice, nri::MemoryLocation::DEVICE, true, false))
			return false;
	}
	if (mStyles.empty()) mStyles.emplace_back();
	if (!UploadBytes(renderer, mStyleUpload, mStyles.data(), mStyles.size() * sizeof(NRISmokeStyleGpu)))
		return false;
	mResourceParticleCapacity = mSettings.particleCapacity;
	mResourceFroxelWidth = fw;
	mResourceFroxelHeight = fh;
	mResourceFroxelDepth = mSettings.froxelDepth;
	mResourceColumnCapacity = mSettings.columnCapacity;
	mResourceStyleCapacity = (uint32_t)mStyles.size();
	mNeedsClear = true;
	mStatus.froxelWidth = fw;
	mStatus.froxelHeight = fh;
	mStatus.froxelDepth = mSettings.froxelDepth;
	mStatus.particleCapacity = mSettings.particleCapacity;
	mStatus.residentBytes = mParticles.memorySize + mControl.memorySize + mColumnCounts.memorySize + mColumnIndices.memorySize +
		mFroxelLocal.memorySize + mFroxelIntegrated.memorySize + mStyleBuffer.memorySize + mStyleUpload.memorySize;
	for (const CommandSlot& slot : mCommandSlots)
		mStatus.residentBytes += slot.upload.memorySize + slot.device.memorySize;
	return true;
}

void NRISmokeSystem::AppendSyntheticCommand(NRIRenderer& renderer)
{
	if (!mSyntheticRequested)
		return;
	mSyntheticRequested = false;
	NRISmokeInjectionCommandGpu command = {};
	for (uint32_t i = 0; i < 3; ++i)
		command.position[i] = renderer.mCurrentCameraPos[i] + renderer.mCurrentCameraForward[i] * 96.0f;
	command.count = 48;
	command.spawnRadius = 12.0f;
	command.serial = mNextCommandSerial++;
	command.epoch = mStatus.simulationEpoch;
	mPendingCommands.push_back(command);
}

bool NRISmokeSystem::PrepareFrame(NRIRenderer& renderer, bool mainViewEligible)
{
	mSettings = BuildNRISmokeSettingsFromCVars();
	mStatus.enabled = mSettings.enabled;
	mStatus.mainViewEligible = mainViewEligible;
	mStatus.preparedFrame = renderer.mFrameIndex;
	if (!mSettings.enabled || !mainViewEligible || mLastPreparedFrame == renderer.mFrameIndex)
		return true;
	mLastPreparedFrame = renderer.mFrameIndex;
	const uint32_t previousGeneration = mEmitters.GetGeneration();
	mEmitters.Gather(mStatus.simulationEpoch, mStyles, mPendingCommands, mNextCommandSerial);
	if (previousGeneration != 0 && previousGeneration != mEmitters.GetGeneration())
	{
		mStatus.simulationEpoch = std::max(1u, mStatus.simulationEpoch + 1u);
		mNeedsClear = true;
		for (NRISmokeInjectionCommandGpu& command : mPendingCommands) command.epoch = mStatus.simulationEpoch;
	}
	if (!EnsureResources(renderer))
		return false;
	AppendSyntheticCommand(renderer);
	return RecordSimulation(renderer);
}

bool NRISmokeSystem::RecordSimulation(NRIRenderer& renderer)
{
	if (mLastSimulatedFrame == renderer.mFrameIndex)
		return true;
	mLastSimulatedFrame = renderer.mFrameIndex;
	const double now = PlayClock > 0 ? (double)PlayClock / 120.0 : 0.0;
	const float elapsed = mLastGameplaySeconds < 0.0 ? 0.0f : (float)std::max(0.0, std::min(0.25, now - mLastGameplaySeconds));
	mLastGameplaySeconds = now;
	mAccumulator += elapsed * mSettings.timeScale;
	const float step = 1.0f / (float)mSettings.simulationRate;
	uint32_t substeps = std::min((uint32_t)std::floor(mAccumulator / step), mSettings.maxSubsteps);
	mAccumulator -= (float)substeps * step;
	mStatus.simulationSubsteps = substeps;

	CommandSlot& slot = mCommandSlots[std::min(renderer.mFrameBuffer->mCurrentQueuedFrameIndex, (uint32_t)mCommandSlots.size() - 1)];
	const uint32_t commandCount = std::min((uint32_t)mPendingCommands.size(), kMaxCommands);
	mStatus.commandsUploaded = commandCount;
	mStatus.commandsUploadedTotal += commandCount;
	mStatus.styleCount = (uint32_t)mStyles.size();
	mStatus.commandsDropped += (uint32_t)mPendingCommands.size() - commandCount;
	if (commandCount > 0 && !UploadBytes(renderer, slot.upload, mPendingCommands.data(), (uint64_t)commandCount * sizeof(NRISmokeInjectionCommandGpu)))
		return false;
	if (!UploadBytes(renderer, mStyleUpload, mStyles.data(), mStyles.size() * sizeof(NRISmokeStyleGpu)))
		return false;
	mPendingCommands.clear();

	nri::BufferBarrierDesc uploadBarriers[4] = {};
	uploadBarriers[0].buffer = mStyleUpload.buffer; uploadBarriers[0].after = NRIResourceCopySourceAccess();
	uploadBarriers[1].buffer = mStyleBuffer.buffer; uploadBarriers[1].after = NRIResourceCopyDestinationAccess();
	uploadBarriers[2].buffer = slot.upload.buffer; uploadBarriers[2].after = NRIResourceCopySourceAccess();
	uploadBarriers[3].buffer = slot.device.buffer; uploadBarriers[3].after = NRIResourceCopyDestinationAccess();
	nri::BarrierDesc uploadBarrier = {}; uploadBarrier.buffers = uploadBarriers; uploadBarrier.bufferNum = 4;
	renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, uploadBarrier);
	renderer.mFrameBuffer->mCore.CmdCopyBuffer(*renderer.mFrameBuffer->mCommandBuffer, *mStyleBuffer.buffer, 0, *mStyleUpload.buffer, 0, mStyles.size() * sizeof(NRISmokeStyleGpu));
	if (commandCount > 0)
		renderer.mFrameBuffer->mCore.CmdCopyBuffer(*renderer.mFrameBuffer->mCommandBuffer, *slot.device.buffer, 0, *slot.upload.buffer, 0, (uint64_t)commandCount * sizeof(NRISmokeInjectionCommandGpu));

	nri::BufferBarrierDesc compute[8] = {};
	nri::Buffer* computeBuffers[] = { mStyleBuffer.buffer, slot.device.buffer, mParticles.buffer, mControl.buffer, mColumnCounts.buffer, mColumnIndices.buffer, mFroxelLocal.buffer, mFroxelIntegrated.buffer };
	for (uint32_t i = 0; i < 8; ++i)
	{
		compute[i].buffer = computeBuffers[i];
		compute[i].after = i < 2 ? NRIResourceComputeShaderResourceAccess() : StorageAccess();
	}
	nri::BarrierDesc computeBarrier = {}; computeBarrier.buffers = compute; computeBarrier.bufferNum = 8;
	renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, computeBarrier);

	const nri::Descriptor* inputs[] = { mStyleBuffer.shaderView, slot.device.shaderView };
	const nri::Descriptor* outputs[] = { mParticles.storageView, mControl.storageView, mColumnCounts.storageView, mColumnIndices.storageView, mFroxelLocal.storageView, mFroxelIntegrated.storageView };
	nri::UpdateDescriptorRangeDesc updates[2] = {};
	updates[0].descriptorSet = slot.inputSet; updates[0].rangeIndex = 0; updates[0].descriptors = inputs; updates[0].descriptorNum = 2;
	updates[1].descriptorSet = slot.bufferSet; updates[1].rangeIndex = 0; updates[1].descriptors = outputs; updates[1].descriptorNum = 6;
	renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(updates, 2);

	NRISmokeConstants constants = {};
	constants.frameIndex = renderer.mFrameIndex;
	constants.simulationEpoch = mStatus.simulationEpoch;
	constants.particleCapacity = mSettings.particleCapacity;
	constants.commandCount = commandCount;
	constants.styleCount = (uint32_t)mStyles.size();
	constants.froxelWidth = mResourceFroxelWidth;
	constants.froxelHeight = mResourceFroxelHeight;
	constants.froxelDepth = mResourceFroxelDepth;
	constants.columnCapacity = mResourceColumnCapacity;
	constants.deltaTime = step;
	constants.timeScale = mSettings.timeScale;
	std::copy(mSettings.wind, mSettings.wind + 3, constants.wind);
	auto dispatch = [&](NRISmokePass pass, uint32_t groups)
	{
		constants.pass = (uint32_t)pass;
		renderer.mFrameBuffer->mCore.CmdSetRootConstants(*renderer.mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
		renderer.mFrameBuffer->mCore.CmdSetPipeline(*renderer.mFrameBuffer->mCommandBuffer, *mPipelines[(uint32_t)pass]);
		renderer.mFrameBuffer->mCore.CmdDispatch(*renderer.mFrameBuffer->mCommandBuffer, { groups, 1, 1 });
	};
	renderer.mFrameBuffer->mCore.CmdSetPipelineLayout(*renderer.mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 0, slot.inputSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 1, slot.bufferSet, nri::BindPoint::COMPUTE });
	if (mNeedsClear)
	{
		constants.flags = 1;
		dispatch(NRISmokePass::Clear, Groups(std::max<uint64_t>(mSettings.particleCapacity, (uint64_t)mResourceFroxelWidth * mResourceFroxelHeight * mResourceFroxelDepth)));
		mNeedsClear = false;
	}
	constants.flags = 0;
	for (uint32_t i = 0; i < substeps; ++i)
	{
		dispatch(NRISmokePass::Simulate, Groups(mSettings.particleCapacity));
		nri::BarrierDesc barrier = {}; barrier.buffers = compute + 2; barrier.bufferNum = 2;
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, barrier);
	}
	if (commandCount > 0)
		dispatch(NRISmokePass::Spawn, Groups(commandCount));
	return true;
}

bool NRISmokeSystem::RecordVolume(NRIRenderer& renderer, const NRISmokeRouteDesc& route)
{
	CommandSlot& slot = mCommandSlots[std::min(renderer.mFrameBuffer->mCurrentQueuedFrameIndex, (uint32_t)mCommandSlots.size() - 1)];
	NRITextureResource& input = renderer.GetFrameTexture(route.inputSlot);
	NRITextureResource& depth = renderer.GetFrameTexture(route.depthSlot);
	NRITextureResource& output = renderer.GetFrameTexture(route.outputSlot);
	if (input.shaderView == nullptr || depth.shaderView == nullptr || output.storageView == nullptr)
		return false;
	renderer.mFrameBuffer->TransitionTexture(input, { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(depth, { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(output, { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER });
	const nri::Descriptor* textures[] = { input.shaderView, depth.shaderView };
	const nri::Descriptor* outputTexture[] = { output.storageView };
	nri::UpdateDescriptorRangeDesc updates[2] = {};
	updates[0].descriptorSet = slot.textureSet; updates[0].rangeIndex = 0; updates[0].descriptors = textures; updates[0].descriptorNum = 2;
	updates[1].descriptorSet = slot.outputSet; updates[1].rangeIndex = 0; updates[1].descriptors = outputTexture; updates[1].descriptorNum = 1;
	renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(updates, 2);

	NRISmokeConstants constants = {};
	constants.frameIndex = renderer.mFrameIndex;
	constants.simulationEpoch = mStatus.simulationEpoch;
	constants.particleCapacity = mSettings.particleCapacity;
	constants.froxelWidth = mResourceFroxelWidth;
	constants.froxelHeight = mResourceFroxelHeight;
	constants.froxelDepth = mResourceFroxelDepth;
	constants.columnCapacity = mResourceColumnCapacity;
	constants.renderWidth = renderer.mRenderWidth;
	constants.renderHeight = renderer.mRenderHeight;
	constants.outputWidth = route.width;
	constants.outputHeight = route.height;
	constants.froxelMaxDistance = mSettings.froxelMaxDistance;
	constants.depthExponent = 2.0f;
	constants.densityScale = mSettings.densityScale;
	constants.radianceScale = mSettings.radianceScale;
	constants.tanHalfFovX = renderer.mCurrentTanHalfFovX;
	constants.tanHalfFovY = renderer.mCurrentTanHalfFovY;
	std::copy(renderer.mCurrentCameraPos, renderer.mCurrentCameraPos + 3, constants.cameraPosition);
	std::copy(renderer.mCurrentCameraForward, renderer.mCurrentCameraForward + 3, constants.cameraForward);
	std::copy(renderer.mCurrentCameraRight, renderer.mCurrentCameraRight + 3, constants.cameraRight);
	std::copy(renderer.mCurrentCameraUp, renderer.mCurrentCameraUp + 3, constants.cameraUp);
	constants.debugMode = mSettings.debugMode;
	auto dispatch = [&](NRISmokePass pass, uint32_t x, uint32_t y, uint32_t z)
	{
		constants.pass = (uint32_t)pass;
		renderer.mFrameBuffer->mCore.CmdSetRootConstants(*renderer.mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
		renderer.mFrameBuffer->mCore.CmdSetPipeline(*renderer.mFrameBuffer->mCommandBuffer, *mPipelines[(uint32_t)pass]);
		renderer.mFrameBuffer->mCore.CmdDispatch(*renderer.mFrameBuffer->mCommandBuffer, { x, y, z });
	};
	auto storageBarrier = [&]()
	{
		nri::BufferBarrierDesc barriers[6] = {};
		nri::Buffer* buffers[] = { mParticles.buffer, mControl.buffer, mColumnCounts.buffer, mColumnIndices.buffer, mFroxelLocal.buffer, mFroxelIntegrated.buffer };
		for (uint32_t i = 0; i < 6; ++i) { barriers[i].buffer = buffers[i]; barriers[i].before = StorageAccess(); barriers[i].after = StorageAccess(); }
		nri::BarrierDesc barrier = {}; barrier.buffers = barriers; barrier.bufferNum = 6;
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, barrier);
	};
	renderer.mFrameBuffer->mCore.CmdSetPipelineLayout(*renderer.mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 0, slot.inputSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 1, slot.bufferSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 2, slot.textureSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 3, slot.outputSet, nri::BindPoint::COMPUTE });
	dispatch(NRISmokePass::Clear, Groups((uint64_t)mResourceFroxelWidth * mResourceFroxelHeight * mResourceFroxelDepth), 1, 1);
	storageBarrier();
	dispatch(NRISmokePass::Bin, Groups(mSettings.particleCapacity), 1, 1);
	storageBarrier();
	dispatch(NRISmokePass::Evaluate, (mResourceFroxelWidth + 3) / 4, (mResourceFroxelHeight + 3) / 4, (mResourceFroxelDepth + 3) / 4);
	storageBarrier();
	dispatch(NRISmokePass::Integrate, (mResourceFroxelWidth + 7) / 8, (mResourceFroxelHeight + 7) / 8, 1);
	storageBarrier();
	dispatch(NRISmokePass::Composite, (route.width + 7) / 8, (route.height + 7) / 8, 1);
	return true;
}

bool NRISmokeSystem::DispatchRoute(NRIRenderer& renderer, const NRISmokeRouteDesc& route)
{
	mStatus.routeSupported = route.supported;
	mStatus.dispatchedFrame = renderer.mFrameIndex;
	mStatus.inputSlot = (uint32_t)route.inputSlot;
	mStatus.outputSlot = (uint32_t)route.outputSlot;
	mStatus.depthSlot = (uint32_t)route.depthSlot;
	mStatus.routeWidth = route.width;
	mStatus.routeHeight = route.height;
	mStatus.routePlacement = (uint32_t)route.placement;
	mStatus.exposureDomain = (uint32_t)route.exposureDomain;
	if (!mSettings.enabled || !mStatus.mainViewEligible || !route.supported)
	{
		if (route.supported)
			renderer.CopyTexture(renderer.GetFrameTexture(route.inputSlot), renderer.GetFrameTexture(route.outputSlot));
		return true;
	}
	return RecordVolume(renderer, route);
}

void NRISmokeSystem::QueueSyntheticInjection()
{
	mSyntheticRequested = true;
}

void NRISmokeSystem::Reset(const char* reason)
{
	mStatus.simulationEpoch = std::max(1u, mStatus.simulationEpoch + 1u);
	mStatus.preparedFrame = UINT32_MAX;
	mStatus.dispatchedFrame = UINT32_MAX;
	mStatus.resetReason = reason != nullptr ? reason : "unspecified";
	mPendingCommands.clear();
	mAccumulator = 0.0f;
	mLastGameplaySeconds = -1.0;
	mLastPreparedFrame = UINT32_MAX;
	mLastSimulatedFrame = UINT32_MAX;
	mNeedsClear = true;
	mEmitters.Reset();
}

void NRISmokeSystem::DestroyResources(NRIRenderer& renderer)
{
	auto destroy = [&](NRIBufferResource& resource) { renderer.DestroyBufferResource(resource); };
	destroy(mStyleBuffer); destroy(mStyleUpload); destroy(mParticles); destroy(mControl); destroy(mColumnCounts);
	destroy(mColumnIndices); destroy(mFroxelLocal); destroy(mFroxelIntegrated);
	for (CommandSlot& slot : mCommandSlots) { destroy(slot.upload); destroy(slot.device); }
	mResourceParticleCapacity = mResourceFroxelWidth = mResourceFroxelHeight = mResourceFroxelDepth = mResourceColumnCapacity = mResourceStyleCapacity = 0;
}

void NRISmokeSystem::Shutdown(NRIRenderer& renderer)
{
	DestroyResources(renderer);
	for (nri::Pipeline*& pipeline : mPipelines)
	{
		if (pipeline != nullptr) renderer.mFrameBuffer->mCore.DestroyPipeline(pipeline);
		pipeline = nullptr;
	}
	if (mPipelineLayout != nullptr) renderer.mFrameBuffer->mCore.DestroyPipelineLayout(mPipelineLayout);
	mPipelineLayout = nullptr;
	Reset("renderer-shutdown");
}

void NRISmokeSystem::PrintStatus(const NRIRenderer& renderer) const
{
	const char* placement = mStatus.routePlacement == (uint32_t)NRISmokeRoutePlacement::DlrrPostUpscale ? "dlrr_post_upscale" : "standard_pre_upscale";
	const char* inputName = mStatus.inputSlot < (uint32_t)NRIRenderer::FrameTextureSlot::Count ? renderer.GetFrameTextureSlotName((NRIRenderer::FrameTextureSlot)mStatus.inputSlot) : "none";
	const char* outputName = mStatus.outputSlot < (uint32_t)NRIRenderer::FrameTextureSlot::Count ? renderer.GetFrameTextureSlotName((NRIRenderer::FrameTextureSlot)mStatus.outputSlot) : "none";
	Printf("NRI PT smoke status: enabled=%s epoch=%u main_view=%s route_supported=%s placement=%s input=%s output=%s extent=%ux%u froxels=%ux%ux%u particles=%u styles=%u commands=%u commands_total=%llu dropped=%u substeps=%u resident_mib=%.2f particle_readback=%llu reset=%s\n",
		mStatus.enabled ? "yes" : "no", mStatus.simulationEpoch, mStatus.mainViewEligible ? "yes" : "no", mStatus.routeSupported ? "yes" : "no", placement,
		inputName, outputName, mStatus.routeWidth, mStatus.routeHeight, mStatus.froxelWidth, mStatus.froxelHeight, mStatus.froxelDepth,
		mStatus.particleCapacity, mStatus.styleCount, mStatus.commandsUploaded, (unsigned long long)mStatus.commandsUploadedTotal, mStatus.commandsDropped, mStatus.simulationSubsteps,
		(double)mStatus.residentBytes / (1024.0 * 1024.0), (unsigned long long)mStatus.routineParticleReadbackBytes, mStatus.resetReason);
}
