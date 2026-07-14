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
	constexpr uint32_t kWideCellCount = 16u * 9u;
	constexpr uint32_t kMaximumReferencesPerParticle = 512u;
	constexpr uint32_t kDirectionalProbesPerParticle = 8000u;
	constexpr uint32_t kDirectionalProbeThreadGroupWidth = 64u;
	constexpr uint32_t kDirectionalParticleThreadGroupHeight = 2u;
	constexpr uint32_t kSmokeCoreStorageBufferCount = 17u;
	constexpr uint32_t kSmokeDirectStorageBufferCount = 2u;
	constexpr uint32_t kSmokeGridLightingStorageBufferCount = NRISmokeGridLighting::StorageDescriptorCount;
	constexpr uint32_t kSmokeBarrierBufferCount = kSmokeCoreStorageBufferCount + kSmokeDirectStorageBufferCount;
	constexpr uint32_t kSmokeStorageDescriptorCount = kSmokeCoreStorageBufferCount + NRISmokeGrid::EvaluationDescriptorCount +
		kSmokeDirectStorageBufferCount + kSmokeGridLightingStorageBufferCount;
	constexpr uint32_t kSmokeFilteredSceneBufferCount = 8u;
	constexpr uint32_t kSmokeEmissiveSceneBufferCount = 7u;
	constexpr uint32_t kSmokeExtendedSceneBufferCount = 10u;
	constexpr uint32_t kSmokeFlagDirectReuseShift = 14u;
	constexpr uint32_t kSmokeFlagCompareRepresentation = 0x10000u;
	constexpr uint32_t kSmokeFlagGridRepresentation = 0x20000u;
	constexpr uint32_t kSmokeFlagDirectHistoryValid = 0x40000u;
	constexpr uint32_t kSmokeFlagDirectReferenceShift = 19u;
	constexpr uint32_t kSmokeFlagGridLightingWorld = 0x200000u;
	constexpr uint32_t kSmokeFlagGridLightingCompare = 0x400000u;
	constexpr uint32_t kSmokeFlagGridLightingFilter = 0x800000u;
	constexpr uint32_t kSmokeFlagEmissiveLegacyGatherDisabled = 0x1000000u;
	constexpr uint32_t kSmokeFlagEmissiveQuarterKey = 0x2000000u;
	constexpr uint32_t kSmokeFlagGridLightingFieldPing = 0x4000000u;
	constexpr uint32_t kSmokeFlagGridLightingDebugShift = 27u;
	constexpr uint32_t kSmokeFlagGridLightingLocalProposals = 0x80000000u;
	const char* const kSmokePipelineNames[] = { "SmokeClear", "SmokeSimulate", "SmokeSpawn", "SmokeBin", "SmokeLightDirectionalCarriers", "SmokeEvaluateMedium", "SmokeEvaluateGrid", "SmokeLightPoint", "SmokeLightDirectional", "SmokeLightDirectTemporal", "SmokeLightDirectSpatial", "SmokeLightEmissive", "SmokeLightEmissiveTemporal", "SmokeLightEmissiveSpatial", "SmokeLightIndirectReference", "SmokeLightIndirectTemporal", "SmokeLightIndirectSpatial", "SmokeIntegrate", "SmokeResolveVolume", "SmokeTemporalVolume", "SmokeComposite" };
	static_assert(std::size(kSmokePipelineNames) == 21u);

	uint32_t PackDirectionalLightColor24(const float color[3])
	{
		auto packChannel = [](float value) -> uint32_t
		{
			return (uint32_t)std::clamp((int)std::lround((double)(std::clamp(value, 0.0f, 8.0f) * (255.0f / 8.0f))), 0, 255);
		};
		return packChannel(color[0]) | (packChannel(color[1]) << 8u) | (packChannel(color[2]) << 16u);
	}

	uint32_t Groups(uint64_t count)
	{
		return (uint32_t)std::max<uint64_t>(1, (count + kThreads - 1) / kThreads);
	}

	uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	uint64_t HashCombine64(uint64_t hash, uint64_t value)
	{
		hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
		return hash;
	}

}

bool NRISmokeSystem::LoadGridShaderBlob(void* user, const char* fileName, std::vector<uint8_t>& outBlob)
{
	NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
	return renderer != nullptr && renderer->mFrameBuffer->LoadShaderBlob(fileName, outBlob);
}

void NRISmokeSystem::WaitForGridCommands(void* user, const char* reason)
{
	(void)reason;
	NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
	if (renderer != nullptr)
		renderer->WaitForCommandsTracked();
}

NRISmokeGridServices NRISmokeSystem::BuildGridServices(NRIRenderer& renderer) const
{
	NRISmokeGridServices services = {};
	services.core = &renderer.mFrameBuffer->mCore;
	services.device = renderer.mFrameBuffer->mDevice;
	services.commandBuffer = renderer.mFrameBuffer->mCommandBuffer;
	services.descriptorPool = renderer.mFrameBuffer->mDescriptorPool;
	services.graphicsAPI = renderer.mFrameBuffer->GetSelectedAPI();
	services.queuedFrameCount = std::max(1u, (uint32_t)renderer.mFrameBuffer->mQueuedFrames.size());
	services.queuedFrameIndex = renderer.mFrameBuffer->mCurrentQueuedFrameIndex;
	services.user = &renderer;
	services.loadShaderBlob = &NRISmokeSystem::LoadGridShaderBlob;
	services.waitForCommands = &NRISmokeSystem::WaitForGridCommands;
	return services;
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
	buffers.descriptorNum = kSmokeStorageDescriptorCount;
	buffers.descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
	buffers.shaderStages = nri::StageBits::COMPUTE_SHADER;
	buffers.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	nri::DescriptorRangeDesc textures = {};
	textures.baseRegisterIndex = 0;
	textures.descriptorNum = 8;
	textures.descriptorType = nri::DescriptorType::TEXTURE;
	textures.shaderStages = nri::StageBits::COMPUTE_SHADER;
	textures.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	nri::DescriptorRangeDesc output = {};
	output.baseRegisterIndex = 0;
	output.descriptorNum = 5;
	output.descriptorType = nri::DescriptorType::STORAGE_TEXTURE;
	output.shaderStages = nri::StageBits::COMPUTE_SHADER;
	output.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	nri::DescriptorRangeDesc lights = {};
	lights.baseRegisterIndex = 0;
	lights.descriptorNum = 3;
	lights.descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	lights.shaderStages = nri::StageBits::COMPUTE_SHADER;
	lights.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	nri::DescriptorRangeDesc filteredSceneRanges[5] = {};
	filteredSceneRanges[0].baseRegisterIndex = 0;
	filteredSceneRanges[0].descriptorNum = kSmokeFilteredSceneBufferCount;
	filteredSceneRanges[0].descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	filteredSceneRanges[0].shaderStages = nri::StageBits::COMPUTE_SHADER;
	filteredSceneRanges[0].flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	filteredSceneRanges[1].baseRegisterIndex = 8;
	filteredSceneRanges[1].descriptorNum = kSmokeExtendedSceneBufferCount;
	filteredSceneRanges[1].descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	filteredSceneRanges[1].shaderStages = nri::StageBits::COMPUTE_SHADER;
	filteredSceneRanges[1].flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	filteredSceneRanges[2].baseRegisterIndex = 18;
	filteredSceneRanges[2].descriptorNum = 514;
	filteredSceneRanges[2].descriptorType = nri::DescriptorType::TEXTURE;
	filteredSceneRanges[2].shaderStages = nri::StageBits::COMPUTE_SHADER;
	filteredSceneRanges[2].flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	filteredSceneRanges[3].baseRegisterIndex = 0;
	filteredSceneRanges[3].descriptorNum = 3;
	filteredSceneRanges[3].descriptorType = nri::DescriptorType::SAMPLER;
	filteredSceneRanges[3].shaderStages = nri::StageBits::COMPUTE_SHADER;
	filteredSceneRanges[3].flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	filteredSceneRanges[4].baseRegisterIndex = 532;
	filteredSceneRanges[4].descriptorNum = 1;
	filteredSceneRanges[4].descriptorType = nri::DescriptorType::ACCELERATION_STRUCTURE;
	filteredSceneRanges[4].shaderStages = nri::StageBits::COMPUTE_SHADER;
	filteredSceneRanges[4].flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	nri::DescriptorSetDesc sets[6] = {};
	nri::DescriptorRangeDesc* ranges[] = { &input, &buffers, &textures, &output, &lights };
	for (uint32_t i = 0; i < 5; ++i)
	{
		sets[i].registerSpace = i;
		sets[i].ranges = ranges[i];
		sets[i].rangeNum = 1;
		sets[i].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	}
	sets[5].registerSpace = 6;
	sets[5].ranges = filteredSceneRanges;
	sets[5].rangeNum = 5;
	sets[5].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	nri::RootConstantDesc root = {};
	root.registerIndex = 0;
	root.size = sizeof(NRISmokeConstants);
	root.shaderStages = nri::StageBits::COMPUTE_SHADER;
	nri::PipelineLayoutDesc layout = {};
	layout.rootRegisterSpace = 5;
	layout.rootConstants = &root;
	layout.rootConstantNum = 1;
	layout.descriptorSets = sets;
	layout.descriptorSetNum = 6;
	layout.shaderStages = nri::StageBits::COMPUTE_SHADER;
	if (renderer.mFrameBuffer->mCore.CreatePipelineLayout(*renderer.mFrameBuffer->mDevice, layout, mPipelineLayout) != nri::Result::SUCCESS)
		return false;

	const bool d3d12 = renderer.mFrameBuffer->GetSelectedAPI() == nri::GraphicsAPI::D3D12;
	for (uint32_t i = 0; i < (uint32_t)mPipelines.size(); ++i)
	{
		std::vector<uint8_t> blob;
		const std::string file = std::string(kSmokePipelineNames[i]) + ".cs." + (d3d12 ? "dxil" : "spirv");
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
	// The grid-lighting owner shares this layout so it can consume the already
	// resident smoke grid and scene tables without becoming another scene owner.
	// Failure remains local: particle and legacy-grid emissive lighting stay usable.
	mGridLighting.Initialize(BuildGridServices(renderer), mPipelineLayout);

	const uint32_t queued = std::max(1u, (uint32_t)renderer.mFrameBuffer->mQueuedFrames.size());
	mCommandSlots.resize(queued);
	for (CommandSlot& slot : mCommandSlots)
	{
		if (renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *mPipelineLayout, 0, &slot.inputSet, 1, 0) != nri::Result::SUCCESS ||
			renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *mPipelineLayout, 1, &slot.bufferSet, 1, 0) != nri::Result::SUCCESS ||
			renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *mPipelineLayout, 2, &slot.textureSet, 1, 0) != nri::Result::SUCCESS ||
			renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *mPipelineLayout, 3, &slot.outputSet, 1, 0) != nri::Result::SUCCESS ||
			renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *mPipelineLayout, 4, &slot.lightSet, 1, 0) != nri::Result::SUCCESS)
			return false;
		if (renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *mPipelineLayout, 5, &slot.filteredSceneSet, 1, 0) != nri::Result::SUCCESS)
			return false;
	}
	return true;
}

bool NRISmokeSystem::EnsureResources(NRIRenderer& renderer)
{
	if (mStyles.empty())
		mStyles.emplace_back();
	const uint32_t fw = (renderer.mRenderWidth + mSettings.froxelPixelSize - 1) / mSettings.froxelPixelSize;
	const uint32_t fh = (renderer.mRenderHeight + mSettings.froxelPixelSize - 1) / mSettings.froxelPixelSize;
	const bool pureGridWorld = mStatus.representationEffective == 1u && mGridLighting.IsWorldReady() &&
		mSettings.emissiveBackend != (uint32_t)NRISmokeEmissiveBackend::Legacy &&
		mSettings.emissiveBackend != (uint32_t)NRISmokeEmissiveBackend::Compare;
	const bool legacyEmissiveFull = !pureGridWorld;
	const bool persistentReady = mParticles.buffer != nullptr && mReferenceNext.buffer != nullptr && mParticleDirectionalVisibility.buffer != nullptr && mResourceParticleCapacity == mSettings.particleCapacity &&
		mResourceStyleCapacity == (uint32_t)mStyles.size();
	const bool viewReady = mFineCells.buffer != nullptr && mDirectCurrent.buffer != nullptr && mDirectHistory.buffer != nullptr && mResourceFroxelWidth == fw &&
		mResourceFroxelHeight == fh && mResourceFroxelDepth == mSettings.froxelDepth &&
		mResourceLegacyEmissiveFull == legacyEmissiveFull;
	if (persistentReady && viewReady)
		return true;

	renderer.WaitForCommandsTracked();
	if (!persistentReady)
		DestroyResources(renderer);
	else
		DestroyViewResources(renderer);
	const nri::BufferUsageBits storage = NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::BufferUsageBits::SHADER_RESOURCE);
	const nri::BufferUsageBits copyDevice = nri::BufferUsageBits::SHADER_RESOURCE;
	const uint64_t columns = (uint64_t)fw * fh;
	const uint64_t froxels = columns * mSettings.froxelDepth;
	const uint64_t legacyEmissiveRecords = legacyEmissiveFull ? froxels : 1u;
	const uint64_t wideCells = (uint64_t)kWideCellCount * mSettings.froxelDepth;
	if (!persistentReady)
	{
		if (!CreateBuffer(renderer, mParticles, (uint64_t)mSettings.particleCapacity * sizeof(NRISmokeParticleGpu), sizeof(NRISmokeParticleGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
			!CreateBuffer(renderer, mControl, sizeof(NRISmokeControlGpu), sizeof(NRISmokeControlGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
			!CreateBuffer(renderer, mReferenceNext, (uint64_t)mSettings.particleCapacity * kMaximumReferencesPerParticle * sizeof(uint32_t), sizeof(uint32_t), storage, nri::MemoryLocation::DEVICE, false, true) ||
			!CreateBuffer(renderer, mParticleDirectionalVisibility, (uint64_t)mSettings.particleCapacity * kDirectionalProbesPerParticle * sizeof(float), sizeof(float), storage, nri::MemoryLocation::DEVICE, false, true) ||
			!CreateBuffer(renderer, mStyleBuffer, std::max<size_t>(1, mStyles.size()) * sizeof(NRISmokeStyleGpu), sizeof(NRISmokeStyleGpu), copyDevice, nri::MemoryLocation::DEVICE, true, false))
		{
			DestroyResources(renderer);
			return false;
		}
		for (CommandSlot& slot : mCommandSlots)
		{
			if (!CreateBuffer(renderer, slot.upload, kMaxCommands * sizeof(NRISmokeInjectionCommandGpu), sizeof(NRISmokeInjectionCommandGpu), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_UPLOAD, false, false) ||
				!CreateBuffer(renderer, slot.device, kMaxCommands * sizeof(NRISmokeInjectionCommandGpu), sizeof(NRISmokeInjectionCommandGpu), copyDevice, nri::MemoryLocation::DEVICE, true, false) ||
				!CreateBuffer(renderer, slot.styleUpload, std::max<size_t>(1, mStyles.size()) * sizeof(NRISmokeStyleGpu), sizeof(NRISmokeStyleGpu), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_UPLOAD, false, false) ||
				!CreateBuffer(renderer, slot.controlReadback, sizeof(NRISmokeControlGpu), sizeof(NRISmokeControlGpu), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, false, false))
			{
				DestroyResources(renderer);
				return false;
			}
		}
		mResourceParticleCapacity = mSettings.particleCapacity;
		mResourceStyleCapacity = (uint32_t)mStyles.size();
		mNeedsClear = true;
	}
	if (!CreateBuffer(renderer, mFineCells, froxels * sizeof(uint32_t) * 2u, sizeof(uint32_t) * 2u, storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mWideCells, wideCells * sizeof(uint32_t) * 2u, sizeof(uint32_t) * 2u, storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mGlobalDepthCells, (uint64_t)mSettings.froxelDepth * sizeof(uint32_t) * 2u, sizeof(uint32_t) * 2u, storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mFroxelMedium, froxels * 16, 16, storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mFroxelIntegrated, froxels * 16, 16, storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mFroxelPhase, froxels * 16, 16, storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mFroxelSource, froxels * 16, 16, storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mOccupiedFroxelIndices, froxels * sizeof(uint32_t), sizeof(uint32_t), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mIndirectHistory, froxels * sizeof(NRISmokeIndirectCacheGpu), sizeof(NRISmokeIndirectCacheGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mIndirectScratch, froxels * sizeof(NRISmokeIndirectCacheGpu), sizeof(NRISmokeIndirectCacheGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mEmissiveCurrent, legacyEmissiveRecords * sizeof(NRISmokeEmissiveStorageGpu), sizeof(NRISmokeEmissiveStorageGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mEmissiveTemporal, legacyEmissiveRecords * sizeof(NRISmokeEmissiveStorageGpu), sizeof(NRISmokeEmissiveStorageGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mEmissiveHistory, legacyEmissiveRecords * sizeof(NRISmokeEmissiveStorageGpu), sizeof(NRISmokeEmissiveStorageGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mDirectCurrent, froxels * sizeof(NRISmokeDirectCacheGpu), sizeof(NRISmokeDirectCacheGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mDirectHistory, froxels * sizeof(NRISmokeDirectCacheGpu), sizeof(NRISmokeDirectCacheGpu), storage, nri::MemoryLocation::DEVICE, false, true))
	{
		DestroyViewResources(renderer);
		return false;
	}
	mResourceFroxelWidth = fw;
	mResourceFroxelHeight = fh;
	mResourceFroxelDepth = mSettings.froxelDepth;
	mResourceLegacyEmissiveFull = legacyEmissiveFull;
	mViewResourcesInitialized = false;
	mIndirectHistoryValid = false;
	mEmissiveHistoryValid = false;
	mStatus.emissiveHistoryValid = false;
	mLastEmissiveFrame = UINT32_MAX;
	mLastEmissiveRepresentation = UINT32_MAX;
	mLastEmissiveLaneCount = 0;
	mLastEmissiveLightMode = 0;
	mLastEmissiveVisibilityBackend = 0;
	mDirectHistoryValid = false;
	mLastDirectFrame = UINT32_MAX;
	mStatus.directHistoryValid = false;
	mStatus.directHistoryResetReason = "view-resources";
	mStatus.froxelWidth = fw;
	mStatus.froxelHeight = fh;
	mStatus.froxelDepth = mSettings.froxelDepth;
	mStatus.particleCapacity = mSettings.particleCapacity;
	mStatus.residentBytes = mParticles.memorySize + mControl.memorySize + mReferenceNext.memorySize + mParticleDirectionalVisibility.memorySize + mFineCells.memorySize +
		mWideCells.memorySize + mGlobalDepthCells.memorySize +
		mFroxelMedium.memorySize + mFroxelIntegrated.memorySize + mFroxelPhase.memorySize + mFroxelSource.memorySize +
		mOccupiedFroxelIndices.memorySize + mIndirectHistory.memorySize + mIndirectScratch.memorySize +
		mEmissiveCurrent.memorySize + mEmissiveTemporal.memorySize + mEmissiveHistory.memorySize +
		mDirectCurrent.memorySize + mDirectHistory.memorySize + mStyleBuffer.memorySize;
	mStatus.indirectCacheBytes = mIndirectHistory.memorySize + mIndirectScratch.memorySize;
	mStatus.emissiveReservoirBytes = mEmissiveCurrent.memorySize + mEmissiveTemporal.memorySize + mEmissiveHistory.memorySize;
	mStatus.directHistoryBytes = mDirectCurrent.memorySize + mDirectHistory.memorySize;
	for (const CommandSlot& slot : mCommandSlots)
		mStatus.residentBytes += slot.upload.memorySize + slot.device.memorySize + slot.styleUpload.memorySize + slot.controlReadback.memorySize;
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
	// Keep the test conspicuous without manufacturing a large overlapping
	// particle workload when the command is invoked repeatedly.
	command.count = 64;
	command.spawnRadius = 10.0f;
	command.densityScale = 4.0f;
	command.radiusScale = 1.5f;
	command.serial = mNextCommandSerial++;
	command.epoch = mStatus.simulationEpoch;
	mPendingCommands.push_back(command);
}

bool NRISmokeSystem::PrepareFrame(NRIRenderer& renderer, bool mainViewEligible, const TArray<PathTracingWeaponLightEvent>& weaponEvents)
{
	mSettings = BuildNRISmokeSettingsFromCVars();
	mStatus.enabled = mSettings.enabled;
	mStatus.dlrrModeRequested = mSettings.dlrrMode;
	mStatus.mainViewEligible = mainViewEligible;
	mStatus.preparedFrame = renderer.mFrameIndex;
	// Volume layers are frame-local products.  Do not let a route that was
	// skipped this frame expose last frame's reactive mask to TAA or an
	// upscaler.
	mStatus.volumeResolvedSlot = UINT32_MAX;
	mStatus.volumeMetaSlot = UINT32_MAX;
	mStatus.dlrrModeEffective = 0u;
	if (!mSettings.enabled || !mainViewEligible || mLastPreparedFrame == renderer.mFrameIndex)
		return true;
	mLastPreparedFrame = renderer.mFrameIndex;
	if (!mCommandSlots.empty())
	{
		CommandSlot& completedSlot = mCommandSlots[std::min(renderer.mFrameBuffer->mCurrentQueuedFrameIndex, (uint32_t)mCommandSlots.size() - 1)];
		if (completedSlot.readbackPending && completedSlot.controlReadback.buffer != nullptr)
		{
			const void* mapped = renderer.mFrameBuffer->mCore.MapBuffer(*completedSlot.controlReadback.buffer, 0, sizeof(NRISmokeControlGpu));
			if (mapped != nullptr)
			{
				const NRISmokeControlGpu control = *static_cast<const NRISmokeControlGpu*>(mapped);
				renderer.mFrameBuffer->mCore.UnmapBuffer(*completedSlot.controlReadback.buffer);
				mStatus.gpuStatsValid = true;
				mStatus.activeParticles = control.activeApprox;
				mStatus.spawnedParticles = control.spawned;
				mStatus.expiredParticles = control.expired;
				mStatus.liveEvictions = control.liveEvictions;
				mStatus.columnOverflow = control.columnOverflow;
				mStatus.wideParticlesProjected = control.wideParticlesProjected;
				mStatus.wideGlobalDrops = control.wideGlobalDrops;
				mStatus.fineColumnReferences = control.fineColumnReferences;
				mStatus.wideCellReferences = control.wideCellReferences;
				mStatus.globalDepthReferences = control.globalDepthReferences;
				mStatus.referenceInvalidLinks = control.referenceInvalidLinks;
				mStatus.referenceTraversalLimitExits = control.referenceTraversalLimitExits;
				mStatus.fineTierParticles = control.fineTierParticles;
				mStatus.wideTierParticles = control.wideTierParticles;
				mStatus.globalTierParticles = control.globalTierParticles;
				mStatus.fineOccupiedCells = control.fineOccupiedCells;
				mStatus.wideOccupiedCells = control.wideOccupiedCells;
				mStatus.globalOccupiedSlices = control.globalOccupiedSlices;
				mStatus.fineMaximumCellReferences = control.fineMaximumCellReferences;
				mStatus.wideMaximumCellReferences = control.wideMaximumCellReferences;
				mStatus.globalMaximumCellReferences = control.globalMaximumCellReferences;
				mStatus.maximumDepthSpan = control.maximumDepthSpan;
				mStatus.maximumCandidatesPerFroxel = control.maximumCandidatesPerFroxel;
				mStatus.occupiedCount = control.occupiedCount;
				mStatus.occupiedOverflow = control.occupiedOverflow;
				mStatus.mediumCandidateTests = control.mediumCandidateTests;
				mStatus.pointFroxelsProcessed = control.pointFroxelsProcessed;
				mStatus.directionalFroxelsProcessed = control.directionalFroxelsProcessed;
				mStatus.directionalSamples = control.directionalSamples;
				mStatus.directionalShadowRays = control.directionalShadowRays;
				mStatus.directionalShadowVisible = control.directionalShadowVisible;
				mStatus.directionalShadowOccluded = control.directionalShadowOccluded;
				mStatus.directionalRadianceClamps = control.directionalRadianceClamps;
				mStatus.emissiveFroxelsProcessed = control.emissiveFroxelsProcessed;
				mStatus.emissiveSamples = control.emissiveSamples;
				mStatus.emissiveCandidateMisses = control.emissiveCandidateMisses;
				mStatus.emissiveDistanceRejected = control.emissiveDistanceRejected;
				mStatus.emissiveFacingRejected = control.emissiveFacingRejected;
				mStatus.emissiveShadowRays = control.emissiveShadowRays;
				mStatus.emissiveShadowVisible = control.emissiveShadowVisible;
				mStatus.emissiveShadowOccluded = control.emissiveShadowOccluded;
				mStatus.emissiveContributed = control.emissiveContributed;
				mStatus.emissiveRadianceClamps = control.emissiveRadianceClamps;
				mStatus.emissiveReservoirInitial = control.emissiveReservoirInitial;
				mStatus.emissiveReservoirInvalid = control.emissiveReservoirInvalid;
				mStatus.emissiveTemporalAccepted = control.emissiveTemporalAccepted;
				mStatus.emissiveTemporalRejected = control.emissiveTemporalRejected;
				mStatus.emissiveSpatialAccepted = control.emissiveSpatialAccepted;
				mStatus.emissiveSpatialRejected = control.emissiveSpatialRejected;
				mStatus.emissiveFinalEvaluations = control.emissiveFinalEvaluations;
				mStatus.emissiveSourceClamps = control.emissiveSourceClamps;
				mStatus.emissiveRemovedEnergy = control.emissiveRemovedEnergy;
				mStatus.emissiveMaximumAge = control.emissiveMaximumAge;
				mStatus.emissiveReferenceSamples = control.emissiveReferenceSamples;
				mStatus.emissiveReferenceRays = control.emissiveReferenceRays;
				mStatus.emissiveIdentityRejects = control.emissiveIdentityRejects;
				mStatus.indirectFroxelsProcessed = control.indirectFroxelsProcessed;
				mStatus.indirectLocalityRays = control.indirectLocalityRays;
				mStatus.indirectLocalityAgreement = control.indirectLocalityAgreement;
				mStatus.indirectLocalityOneSided = control.indirectLocalityOneSided;
				mStatus.indirectLocalityMismatch = control.indirectLocalityMismatch;
				mStatus.indirectLocalityInvalid = control.indirectLocalityInvalid;
				mStatus.indirectReferenceRays = control.indirectReferenceRays;
				mStatus.indirectReferenceHits = control.indirectReferenceHits;
				mStatus.indirectReferenceMisses = control.indirectReferenceMisses;
				mStatus.indirectSectorContributions = control.indirectSectorContributions;
				mStatus.indirectSkyContributions = control.indirectSkyContributions;
				mStatus.indirectEmissionContributions = control.indirectEmissionContributions;
				mStatus.indirectRadianceClamps = control.indirectRadianceClamps;
				mStatus.indirectNanRejects = control.indirectNanRejects;
				mStatus.indirectTemporalAccepted = control.indirectTemporalAccepted;
				mStatus.indirectTemporalRejected = control.indirectTemporalRejected;
				mStatus.indirectSpatialAccepted = control.indirectSpatialAccepted;
				mStatus.indirectSpatialRejected = control.indirectSpatialRejected;
				mStatus.indirectCacheMaximumAge = control.indirectCacheMaximumAge;
				mStatus.indirectCacheClamps = control.indirectCacheClamps;
				mStatus.indirectCacheResolved = control.indirectCacheResolved;
				mStatus.directReceiverSamples = control.directReceiverSamples;
				mStatus.directFractionalVisibility = control.directFractionalVisibility;
				mStatus.directVisibilityZero = control.directVisibilityZero;
				mStatus.directVisibilityOne = control.directVisibilityOne;
				mStatus.directTemporalAccepted = control.directTemporalAccepted;
				mStatus.directTemporalRejected = control.directTemporalRejected;
				mStatus.directSpatialAccepted = control.directSpatialAccepted;
				mStatus.directSpatialRejected = control.directSpatialRejected;
				mStatus.directHistoryMaximumAge = control.directHistoryMaximumAge;
				mStatus.directHistoryResolved = control.directHistoryResolved;
				mStatus.directHistoryClamps = control.directHistoryClamps;
				mStatus.directNanRejects = control.directNanRejects;
				mStatus.lightCandidatesTested = control.lightCandidatesTested;
				mStatus.lightDistanceRejected = control.lightDistanceRejected;
				mStatus.lightShadowRays = control.lightShadowRays;
				mStatus.lightShadowVisible = control.lightShadowVisible;
				mStatus.lightShadowOccluded = control.lightShadowOccluded;
				mStatus.lightSoftSamples = control.lightSoftSamples;
				mStatus.lightRadianceClamps = control.lightRadianceClamps;
				mStatus.filterCandidateHits = control.filterCandidateHits;
				mStatus.filterAlphaRejects = control.filterAlphaRejects;
				mStatus.filterNoShadowRejects = control.filterNoShadowRejects;
				mStatus.filterOneWayRejects = control.filterOneWayRejects;
				mStatus.filterReflectionRejects = control.filterReflectionRejects;
				mStatus.filterPortalContinuations = control.filterPortalContinuations;
				mStatus.filterAcceptedBlockers = control.filterAcceptedBlockers;
				mStatus.filterMisses = control.filterMisses;
				mStatus.filterSkipLimitExits = control.filterSkipLimitExits;
				mStatus.filterContinuationLimitExits = control.filterContinuationLimitExits;
				mStatus.filterResourceDowngrades = control.filterResourceDowngrades;
				mStatus.controlReadbackBytes += sizeof(NRISmokeControlGpu);
			}
			completedSlot.readbackPending = false;
		}
	}
	const uint32_t previousGeneration = mEmitters.GetGeneration();
	mEmitters.Gather(mStatus.simulationEpoch, weaponEvents, mStyles, mPendingCommands, mNextCommandSerial, mSettings.traceMode);
	if (previousGeneration != 0 && previousGeneration != mEmitters.GetGeneration())
	{
		mStatus.simulationEpoch = std::max(1u, mStatus.simulationEpoch + 1u);
		mNeedsClear = true;
		for (NRISmokeInjectionCommandGpu& command : mPendingCommands) command.epoch = mStatus.simulationEpoch;
	}
	mStatus.representationRequested = mSettings.representation;
	const bool gridReady = mGrid.PrepareFrame(BuildGridServices(renderer), mSettings,
		renderer.mFrameIndex, mStatus.simulationEpoch);
	mStatus.gridReady = mSettings.representation != 0u && gridReady && mGrid.GetStatusSnapshot().resourcesReady;
	mStatus.representationEffective = mStatus.gridReady ? mSettings.representation : 0u;
	mStatus.representationFallback = mSettings.representation != 0u && !mStatus.gridReady ? "grid-unavailable" : "none";
	if (mStatus.gridReady && mSettings.emissiveBackend != (uint32_t)NRISmokeEmissiveBackend::Legacy)
	{
		const NRISmokeGridStatusSnapshot& gridStatus = mGrid.GetStatusSnapshot();
		if (!mGridLighting.PrepareFrame(BuildGridServices(renderer), mSettings, gridStatus.cellCapacity,
			renderer.mFrameIndex, mStatus.simulationEpoch))
			mStatus.representationFallback = "world-lighting-unavailable/legacy-emissive";
		else
		{
			std::array<const nri::Descriptor*, NRISmokeGrid::EvaluationDescriptorCount> gridLightingSnapshot = {};
			if (mGrid.GetEvaluationStorageDescriptors(gridLightingSnapshot))
				mGridLighting.PublishGridSnapshot(gridLightingSnapshot, mGrid.GetFieldPing(), mSettings.gridCellSize);
		}
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
	mParticleSimulationSeconds += (double)substeps * (double)step * (double)mSettings.timeScale;

	CommandSlot& slot = mCommandSlots[std::min(renderer.mFrameBuffer->mCurrentQueuedFrameIndex, (uint32_t)mCommandSlots.size() - 1)];
	const uint32_t commandCount = std::min((uint32_t)mPendingCommands.size(), kMaxCommands);
	mStatus.commandsUploaded = commandCount;
	mStatus.commandsUploadedTotal += commandCount;
	mStatus.styleCount = (uint32_t)mStyles.size();
	mStatus.commandsDropped += (uint32_t)mPendingCommands.size() - commandCount;
	if (commandCount > 0u)
	{
		mMayHaveParticleSmoke = true;
		for (uint32_t i = 0; i < commandCount; ++i)
		{
			const uint32_t styleIndex = mPendingCommands[i].styleIndex;
			if (styleIndex < mStyles.size())
				mLatestParticleDeathSeconds = std::max(mLatestParticleDeathSeconds,
					mParticleSimulationSeconds + (double)std::max(mStyles[styleIndex].lifetime, 0.0f));
		}
	}
	else if (mMayHaveParticleSmoke && mParticleSimulationSeconds >= mLatestParticleDeathSeconds)
	{
		mMayHaveParticleSmoke = false;
	}
	if (commandCount > 0 && !UploadBytes(renderer, slot.upload, mPendingCommands.data(), (uint64_t)commandCount * sizeof(NRISmokeInjectionCommandGpu)))
		return false;
	if (!UploadBytes(renderer, slot.styleUpload, mStyles.data(), mStyles.size() * sizeof(NRISmokeStyleGpu)))
		return false;
	mPendingCommands.clear();

	const bool firstWorldUse = !mResourcesInitialized;
	const bool firstSlotUse = !slot.initialized;
	nri::BufferBarrierDesc uploadBarriers[4] = {};
	uploadBarriers[0].buffer = slot.styleUpload.buffer; uploadBarriers[0].after = NRIResourceCopySourceAccess();
	uploadBarriers[1].buffer = mStyleBuffer.buffer; uploadBarriers[1].before = firstWorldUse ? nri::AccessStage{} : NRIResourceComputeShaderResourceAccess(); uploadBarriers[1].after = NRIResourceCopyDestinationAccess();
	uploadBarriers[2].buffer = slot.upload.buffer; uploadBarriers[2].after = NRIResourceCopySourceAccess();
	uploadBarriers[3].buffer = slot.device.buffer; uploadBarriers[3].before = firstSlotUse ? nri::AccessStage{} : NRIResourceComputeShaderResourceAccess(); uploadBarriers[3].after = NRIResourceCopyDestinationAccess();
	nri::BarrierDesc uploadBarrier = {}; uploadBarrier.buffers = uploadBarriers; uploadBarrier.bufferNum = 4;
	renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, uploadBarrier);
	renderer.mFrameBuffer->mCore.CmdCopyBuffer(*renderer.mFrameBuffer->mCommandBuffer, *mStyleBuffer.buffer, 0, *slot.styleUpload.buffer, 0, mStyles.size() * sizeof(NRISmokeStyleGpu));
	if (commandCount > 0)
		renderer.mFrameBuffer->mCore.CmdCopyBuffer(*renderer.mFrameBuffer->mCommandBuffer, *slot.device.buffer, 0, *slot.upload.buffer, 0, (uint64_t)commandCount * sizeof(NRISmokeInjectionCommandGpu));

	nri::BufferBarrierDesc compute[2 + kSmokeBarrierBufferCount] = {};
	nri::Buffer* computeBuffers[] = { mStyleBuffer.buffer, slot.device.buffer, mParticles.buffer, mControl.buffer,
		mFineCells.buffer, mReferenceNext.buffer, mFroxelMedium.buffer, mFroxelIntegrated.buffer,
		mWideCells.buffer, mGlobalDepthCells.buffer, mFroxelPhase.buffer, mFroxelSource.buffer, mOccupiedFroxelIndices.buffer,
		mIndirectHistory.buffer, mIndirectScratch.buffer, mParticleDirectionalVisibility.buffer,
		mEmissiveCurrent.buffer, mEmissiveTemporal.buffer, mEmissiveHistory.buffer,
		mDirectCurrent.buffer, mDirectHistory.buffer };
	for (uint32_t i = 0; i < 2 + kSmokeBarrierBufferCount; ++i)
	{
		compute[i].buffer = computeBuffers[i];
		compute[i].after = i < 2 ? NRIResourceComputeShaderResourceAccess() : StorageAccess();
	}
	compute[0].before = NRIResourceCopyDestinationAccess();
	compute[1].before = NRIResourceCopyDestinationAccess();
	if (!firstWorldUse)
	{
		compute[2].before = StorageAccess();
		compute[3].before = StorageAccess();
		if (mControlCopyPending) compute[3].before = NRIResourceCopySourceAccess();
	}
	if (mViewResourcesInitialized)
		for (uint32_t i = 4; i < 2 + kSmokeBarrierBufferCount; ++i) compute[i].before = StorageAccess();
	mControlCopyPending = false;
	slot.initialized = true;
	mResourcesInitialized = true;
	mViewResourcesInitialized = true;
	nri::BarrierDesc computeBarrier = {}; computeBarrier.buffers = compute; computeBarrier.bufferNum = 2 + kSmokeBarrierBufferCount;
	renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, computeBarrier);

	const nri::Descriptor* inputs[] = { mStyleBuffer.shaderView, slot.device.shaderView };
	std::array<const nri::Descriptor*, kSmokeStorageDescriptorCount> outputs = { mParticles.storageView, mControl.storageView, mFineCells.storageView, mReferenceNext.storageView,
		mFroxelMedium.storageView, mFroxelIntegrated.storageView, mWideCells.storageView, mGlobalDepthCells.storageView, mFroxelPhase.storageView,
		mFroxelSource.storageView, mOccupiedFroxelIndices.storageView, mIndirectHistory.storageView, mIndirectScratch.storageView,
		mParticleDirectionalVisibility.storageView, mEmissiveCurrent.storageView, mEmissiveTemporal.storageView, mEmissiveHistory.storageView };
	std::array<const nri::Descriptor*, NRISmokeGrid::EvaluationDescriptorCount> gridDescriptors = {};
	if (!mGrid.GetEvaluationStorageDescriptors(gridDescriptors))
		gridDescriptors.fill(mControl.storageView);
	std::copy(gridDescriptors.begin(), gridDescriptors.end(), outputs.begin() + kSmokeCoreStorageBufferCount);
	outputs[kSmokeCoreStorageBufferCount + NRISmokeGrid::EvaluationDescriptorCount] = mDirectCurrent.storageView;
	outputs[kSmokeCoreStorageBufferCount + NRISmokeGrid::EvaluationDescriptorCount + 1u] = mDirectHistory.storageView;
	std::array<const nri::Descriptor*, NRISmokeGridLighting::StorageDescriptorCount> gridLightingDescriptors = {};
	if (!mGridLighting.GetStorageDescriptors(gridLightingDescriptors))
		gridLightingDescriptors.fill(mControl.storageView);
	std::copy(gridLightingDescriptors.begin(), gridLightingDescriptors.end(),
		outputs.begin() + kSmokeCoreStorageBufferCount + NRISmokeGrid::EvaluationDescriptorCount + kSmokeDirectStorageBufferCount);
	nri::UpdateDescriptorRangeDesc updates[2] = {};
	updates[0].descriptorSet = slot.inputSet; updates[0].rangeIndex = 0; updates[0].descriptors = inputs; updates[0].descriptorNum = 2;
	updates[1].descriptorSet = slot.bufferSet; updates[1].rangeIndex = 0; updates[1].descriptors = outputs.data(); updates[1].descriptorNum = kSmokeStorageDescriptorCount;
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
	constants.directionalColorPacked = 0u;
	constants.deltaTime = step;
	constants.timeScale = mSettings.timeScale;
	std::copy(mSettings.wind, mSettings.wind + 3, constants.wind);
	auto dispatch = [&](NRISmokePass pass, uint32_t groups)
	{
		renderer.mFrameBuffer->mCore.CmdBeginAnnotation(*renderer.mFrameBuffer->mCommandBuffer,
			kSmokePipelineNames[(uint32_t)pass], nri::BGRA_UNUSED);
		constants.pass = (uint32_t)pass;
		renderer.mFrameBuffer->mCore.CmdSetRootConstants(*renderer.mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
		renderer.mFrameBuffer->mCore.CmdSetPipeline(*renderer.mFrameBuffer->mCommandBuffer, *mPipelines[(uint32_t)pass]);
		renderer.mFrameBuffer->mCore.CmdDispatch(*renderer.mFrameBuffer->mCommandBuffer, { groups, 1, 1 });
		renderer.mFrameBuffer->mCore.CmdEndAnnotation(*renderer.mFrameBuffer->mCommandBuffer);
	};
	auto storageBarrier = [&]()
	{
		nri::BufferBarrierDesc barriers[kSmokeBarrierBufferCount] = {};
		for (uint32_t i = 0; i < kSmokeBarrierBufferCount; ++i) { barriers[i].buffer = computeBuffers[i + 2]; barriers[i].before = StorageAccess(); barriers[i].after = StorageAccess(); }
		nri::BarrierDesc barrier = {}; barrier.buffers = barriers; barrier.bufferNum = kSmokeBarrierBufferCount;
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, barrier);
	};
	renderer.mFrameBuffer->mCore.CmdSetPipelineLayout(*renderer.mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 0, slot.inputSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 1, slot.bufferSet, nri::BindPoint::COMPUTE });
	if (mNeedsClear)
	{
		constants.flags = 1;
		const uint64_t froxelCount = (uint64_t)mResourceFroxelWidth * mResourceFroxelHeight * mResourceFroxelDepth;
		const uint64_t wideCellCount = (uint64_t)kWideCellCount * mResourceFroxelDepth;
		dispatch(NRISmokePass::Clear, Groups(std::max({ (uint64_t)mSettings.particleCapacity, froxelCount, wideCellCount, (uint64_t)mResourceFroxelDepth })));
		storageBarrier();
		mNeedsClear = false;
	}
	constants.flags = 0;
	for (uint32_t i = 0; i < substeps; ++i)
	{
		dispatch(NRISmokePass::Simulate, Groups(mSettings.particleCapacity));
		storageBarrier();
	}
	if (commandCount > 0)
	{
		dispatch(NRISmokePass::Spawn, Groups(commandCount));
		storageBarrier();
	}
	if (mStatus.representationEffective != 0u)
	{
		NRISmokeGridFrameDesc gridFrame = {};
		gridFrame.frameIndex = renderer.mFrameIndex;
		gridFrame.simulationEpoch = mStatus.simulationEpoch;
		gridFrame.commandCount = commandCount;
		gridFrame.styleCount = (uint32_t)mStyles.size();
		gridFrame.simulationSubsteps = substeps;
		gridFrame.simulationStep = step;
		gridFrame.styleView = mStyleBuffer.shaderView;
		gridFrame.commandView = slot.device.shaderView;
		if (!mGrid.RecordFrame(BuildGridServices(renderer), mSettings, gridFrame))
		{
			mStatus.gridReady = false;
			mStatus.representationEffective = 0u;
			mStatus.representationFallback = "grid-record-failed";
		}
	}
	return true;
}

bool NRISmokeSystem::RecordVolume(NRIRenderer& renderer, const NRISmokeRouteDesc& route)
{
	CommandSlot& slot = mCommandSlots[std::min(renderer.mFrameBuffer->mCurrentQueuedFrameIndex, (uint32_t)mCommandSlots.size() - 1)];
	NRITextureResource& input = renderer.GetFrameTexture(route.inputSlot);
	NRITextureResource& depth = renderer.GetFrameTexture(route.depthSlot);
	NRITextureResource& output = renderer.GetFrameTexture(route.outputSlot);
	NRITextureResource& volumeCurrent = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::SmokeVolumeCurrent);
	NRITextureResource& volumeCurrentMeta = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::SmokeVolumeCurrentMeta);
	const bool writePing = (renderer.mFrameIndex & 1u) == 0u;
	NRITextureResource& volumeHistoryRead = renderer.GetFrameTexture(writePing ? NRIRenderer::FrameTextureSlot::SmokeVolumeHistoryPong : NRIRenderer::FrameTextureSlot::SmokeVolumeHistoryPing);
	NRITextureResource& volumeHistoryWrite = renderer.GetFrameTexture(writePing ? NRIRenderer::FrameTextureSlot::SmokeVolumeHistoryPing : NRIRenderer::FrameTextureSlot::SmokeVolumeHistoryPong);
	NRITextureResource& volumeMetaRead = renderer.GetFrameTexture(writePing ? NRIRenderer::FrameTextureSlot::SmokeVolumeMetaPong : NRIRenderer::FrameTextureSlot::SmokeVolumeMetaPing);
	NRITextureResource& volumeMetaWrite = renderer.GetFrameTexture(writePing ? NRIRenderer::FrameTextureSlot::SmokeVolumeMetaPing : NRIRenderer::FrameTextureSlot::SmokeVolumeMetaPong);
	if (input.shaderView == nullptr || depth.shaderView == nullptr || output.storageView == nullptr ||
		volumeCurrent.shaderView == nullptr || volumeCurrent.storageView == nullptr ||
		volumeCurrentMeta.shaderView == nullptr || volumeCurrentMeta.storageView == nullptr ||
		volumeHistoryRead.shaderView == nullptr || volumeHistoryWrite.shaderView == nullptr || volumeHistoryWrite.storageView == nullptr ||
		volumeMetaRead.shaderView == nullptr || volumeMetaWrite.shaderView == nullptr || volumeMetaWrite.storageView == nullptr)
		return false;
	const bool standardExtent = (route.placement == NRISmokeRoutePlacement::StandardPreUpscale ||
		route.placement == NRISmokeRoutePlacement::DlrrPreUpscaleMainInput) &&
		route.width == renderer.mRenderWidth && route.height == renderer.mRenderHeight &&
		input.width == route.width && input.height == route.height &&
		depth.width == route.width && depth.height == route.height &&
		output.width == route.width && output.height == route.height;
	if (!standardExtent)
	{
		mStatus.routeSupported = false;
		if (mSettings.traceMode > 0u)
		{
			Printf("NRI PT smoke route rejected: placement=%u route=%ux%u render=%ux%u input=%ux%u depth=%ux%u output=%ux%u\n",
				(uint32_t)route.placement, route.width, route.height, renderer.mRenderWidth, renderer.mRenderHeight,
				input.width, input.height, depth.width, depth.height, output.width, output.height);
		}
		if (input.width == output.width && input.height == output.height)
		{
			renderer.CopyTexture(input, output);
			return true;
		}
		return false;
	}
	renderer.mFrameBuffer->TransitionTexture(input, { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(depth, { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(output, { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(volumeHistoryRead, { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(volumeMetaRead, { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(volumeCurrent, { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(volumeCurrentMeta, { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(volumeHistoryWrite, { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(volumeMetaWrite, { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER });
	const nri::Descriptor* textures[] = { input.shaderView, depth.shaderView, volumeHistoryRead.shaderView, volumeMetaRead.shaderView,
		volumeHistoryWrite.shaderView, volumeMetaWrite.shaderView, volumeCurrent.shaderView, volumeCurrentMeta.shaderView };
	const nri::Descriptor* outputTextures[] = { output.storageView, volumeCurrent.storageView, volumeCurrentMeta.storageView,
		volumeHistoryWrite.storageView, volumeMetaWrite.storageView };
	const nri::Descriptor* lightBuffers[] = {
		renderer.mSceneDataDescriptors[10],
		renderer.mSceneDataDescriptors[11],
		renderer.mSceneDataDescriptors[12],
	};
	const bool lightBuffersReady = lightBuffers[0] != nullptr && lightBuffers[1] != nullptr && lightBuffers[2] != nullptr;
	const nri::Descriptor* filteredSceneBuffers[] = {
		renderer.mSceneDataDescriptors[2], renderer.mSceneDataDescriptors[3],
		renderer.mSceneDataDescriptors[6], renderer.mSceneDataDescriptors[7],
		renderer.mSceneDataDescriptors[8], renderer.mSceneDataDescriptors[9],
		renderer.mSceneDataDescriptors[23], renderer.mSceneDataDescriptors[24],
	};
	const bool filteredBuffersReady = std::all_of(std::begin(filteredSceneBuffers), std::end(filteredSceneBuffers), [](const nri::Descriptor* descriptor) { return descriptor != nullptr; });
	const nri::Descriptor* emissiveSceneBuffers[] = {
		renderer.mSceneDataDescriptors[0], renderer.mSceneDataDescriptors[4], renderer.mSceneDataDescriptors[21],
		renderer.mSceneDataDescriptors[13], renderer.mSceneDataDescriptors[14], renderer.mSceneDataDescriptors[15],
		renderer.mSceneDataDescriptors[25],
	};
	const bool emissiveBuffersReady = std::all_of(std::begin(emissiveSceneBuffers), std::end(emissiveSceneBuffers), [](const nri::Descriptor* descriptor) { return descriptor != nullptr; });
	const bool filteredTexturesReady = renderer.mCurrentSceneTextureDescriptors.size() >= 514u && renderer.mCurrentSceneTextureDescriptors[0] != nullptr;
	const bool shadowReady = renderer.mTopLevelAS.descriptor != nullptr;
	const nri::Descriptor* indirectSceneBuffers[] = {
		renderer.mSceneDataDescriptors[16], renderer.mSceneDataDescriptors[17], renderer.mSceneDataDescriptors[18]
	};
	const bool sectorLightResourcesReady = indirectSceneBuffers[0] != nullptr && indirectSceneBuffers[1] != nullptr && renderer.mBoundSectorLightSectorCount > 0u;
	const bool reprojectionResourcesReady = indirectSceneBuffers[2] != nullptr;
	const nri::Descriptor* smokeSky[] = {
		renderer.mCurrentSceneTextureDescriptors.size() > 1u ? renderer.mCurrentSceneTextureDescriptors[1] : nullptr
	};
	const bool skyResourceReady = smokeSky[0] != nullptr;
	std::array<const nri::Descriptor*, kSmokeExtendedSceneBufferCount> extendedSceneBuffers = {};
	std::copy(std::begin(emissiveSceneBuffers), std::end(emissiveSceneBuffers), extendedSceneBuffers.begin());
	for (uint32_t i = 0; i < 3u; ++i)
		extendedSceneBuffers[kSmokeEmissiveSceneBufferCount + i] = indirectSceneBuffers[i] != nullptr ? indirectSceneBuffers[i] : emissiveSceneBuffers[0];
	const bool extendedSceneBuffersReady = emissiveBuffersReady;
	const bool filteredResourcesReady = filteredBuffersReady && filteredTexturesReady && skyResourceReady && extendedSceneBuffersReady;
	const bool emissiveResourcesReady = filteredResourcesReady && emissiveBuffersReady && renderer.mBoundEmissivePrimitiveCount > 0u;
	const bool indirectResourcesReady = filteredResourcesReady && shadowReady && sectorLightResourcesReady && skyResourceReady;
	std::array<const nri::Descriptor*, 514> smokeSceneTextures = {};
	if (filteredTexturesReady && skyResourceReady)
	{
		smokeSceneTextures[0] = renderer.mCurrentSceneTextureDescriptors[0];
		smokeSceneTextures[1] = smokeSky[0];
		std::copy(renderer.mCurrentSceneTextureDescriptors.begin() + 2, renderer.mCurrentSceneTextureDescriptors.begin() + 514, smokeSceneTextures.begin() + 2);
	}
	const nri::Descriptor* filteredSamplers[] = {
		renderer.mFrameBuffer->mSamplers[(size_t)NRISamplerMode::WrapPoint],
		renderer.mFrameBuffer->mSamplers[(size_t)NRISamplerMode::WrapLinear],
		renderer.mFrameBuffer->mSamplers[(size_t)NRISamplerMode::ClampPoint],
	};
	nri::UpdateDescriptorRangeDesc updates[11] = {};
	updates[0].descriptorSet = slot.textureSet; updates[0].rangeIndex = 0; updates[0].descriptors = textures; updates[0].descriptorNum = 8;
	updates[1].descriptorSet = slot.outputSet; updates[1].rangeIndex = 0; updates[1].descriptors = outputTextures; updates[1].descriptorNum = 5;
	uint32_t updateCount = 2;
	if (lightBuffersReady)
	{
		updates[2].descriptorSet = slot.lightSet; updates[2].rangeIndex = 0; updates[2].descriptors = lightBuffers; updates[2].descriptorNum = 3;
		updateCount++;
	}
	if (filteredResourcesReady)
	{
		updates[updateCount].descriptorSet = slot.filteredSceneSet; updates[updateCount].rangeIndex = 0; updates[updateCount].descriptors = filteredSceneBuffers; updates[updateCount].descriptorNum = kSmokeFilteredSceneBufferCount; updateCount++;
		if (extendedSceneBuffersReady)
		{
			updates[updateCount].descriptorSet = slot.filteredSceneSet; updates[updateCount].rangeIndex = 1; updates[updateCount].descriptors = extendedSceneBuffers.data(); updates[updateCount].descriptorNum = kSmokeExtendedSceneBufferCount; updateCount++;
		}
		updates[updateCount].descriptorSet = slot.filteredSceneSet; updates[updateCount].rangeIndex = 2; updates[updateCount].descriptors = smokeSceneTextures.data(); updates[updateCount].descriptorNum = (uint32_t)smokeSceneTextures.size(); updateCount++;
		updates[updateCount].descriptorSet = slot.filteredSceneSet; updates[updateCount].rangeIndex = 3; updates[updateCount].descriptors = filteredSamplers; updates[updateCount].descriptorNum = 3; updateCount++;
	}
	const nri::Descriptor* worldTlas[] = { renderer.mTopLevelAS.descriptor };
	if (shadowReady)
	{
		updates[updateCount].descriptorSet = slot.filteredSceneSet; updates[updateCount].rangeIndex = 4; updates[updateCount].descriptors = worldTlas; updates[updateCount].descriptorNum = 1; updateCount++;
	}
	renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(updates, updateCount);

	NRISmokeConstants constants = {};
	constants.frameIndex = renderer.mFrameIndex;
	constants.simulationEpoch = mStatus.simulationEpoch;
	constants.particleCapacity = mSettings.particleCapacity;
	constants.styleCount = (uint32_t)mStyles.size();
	constants.froxelWidth = mResourceFroxelWidth;
	constants.froxelHeight = mResourceFroxelHeight;
	constants.froxelDepth = mResourceFroxelDepth;
	uint64_t emissivePayloadHash = renderer.mEmissiveSamplingPayloadHash;
	if (renderer.ShouldUseSceneDataFrameRing())
	{
		const NRIRenderer::SceneDataFrameSlot& frameSlot = renderer.GetCurrentSceneDataFrameSlot();
		if (frameSlot.emissiveSamplingPayloadValid)
			emissivePayloadHash = frameSlot.emissiveSamplingPayloadHash;
	}
	// Candidate payload values and dynamic geometry legitimately change every
	// frame. Stable distribution keys guard candidate identity while this
	// epoch rejects records across a smoke/world reset; using the full payload
	// hash here would disable temporal reuse during ordinary gameplay.
	const uint32_t emissiveGeneration = std::max(1u, mStatus.simulationEpoch);
	constants.commandCount = emissiveGeneration;
	constants.directionalColorPacked = PackDirectionalLightColor24(renderer.mDirectionalLightState.color);
	constants.renderWidth = renderer.mRenderWidth;
	constants.renderHeight = renderer.mRenderHeight;
	constants.outputWidth = route.width;
	constants.outputHeight = route.height;
	constants.froxelMaxDistance = mSettings.froxelMaxDistance;
	constants.depthExponent = 2.0f;
	constants.densityScale = mSettings.densityScale;
	constants.radianceScale = mSettings.radianceScale;
	constants.deltaTime = mSettings.emissiveSourceClamp;
	constants.indirectScale = mSettings.indirectScale;
	constants.tanHalfFovX = renderer.mCurrentTanHalfFovX;
	constants.tanHalfFovY = renderer.mCurrentTanHalfFovY;
	std::copy(renderer.mCurrentCameraPos, renderer.mCurrentCameraPos + 3, constants.cameraPosition);
	std::copy(renderer.mCurrentCameraForward, renderer.mCurrentCameraForward + 3, constants.cameraForward);
	std::copy(renderer.mCurrentCameraRight, renderer.mCurrentCameraRight + 3, constants.cameraRight);
	std::copy(renderer.mCurrentCameraUp, renderer.mCurrentCameraUp + 3, constants.cameraUp);
	constants.directionalDirectionX = renderer.mDirectionalLightState.direction[0];
	constants.directionalDirectionY = renderer.mDirectionalLightState.direction[1];
	constants.directionalDirectionZ = renderer.mDirectionalLightState.direction[2];
	constants.directionalAngularSize = std::clamp(renderer.mDirectionalLightState.angularSize, 0.001f, 1.2f);
	std::copy(renderer.mCurrentJitter, renderer.mCurrentJitter + 2, constants.currentJitter);
	constants.debugMode = mSettings.debugMode;
	const bool pointLightsReady = mSettings.pointLights && lightBuffersReady && renderer.mBoundRuntimeLightCount > 0;
	const bool directionalLightReady = mSettings.directionalLight && renderer.mDirectionalLightState.enabled;
	const bool emissiveLightsReady = mSettings.emissiveLights && emissiveResourcesReady;
	constants.lightMode = (pointLightsReady || directionalLightReady || emissiveLightsReady) ? mSettings.lightMode : 0u;
	constants.lightSamples = mSettings.lightSamples;
	constants.maxLightCandidates = mSettings.maxLightCandidates;
	constants.runtimeLightCount = pointLightsReady ? renderer.mBoundRuntimeLightCount : 0u;
	constants.runtimeLightTileCountX = pointLightsReady ? renderer.mBoundRuntimeLightTileCountX : 0u;
	constants.runtimeLightTileCountY = pointLightsReady ? renderer.mBoundRuntimeLightTileCountY : 0u;
	constants.lightSourceFlags =
		(pointLightsReady ? 0x1u : 0u) |
		(directionalLightReady ? 0x2u : 0u) |
		(directionalLightReady && renderer.mDirectionalLightState.shadow ? 0x4u : 0u) |
		(emissiveLightsReady ? 0x8u : 0u);
	if (mSettings.indirect && indirectResourcesReady)
		constants.lightSourceFlags |= 0x10u;
	const bool filteredVisibilityEffective = constants.lightMode >= 2u && mSettings.filteredVisibility && filteredResourcesReady && shadowReady;
	constants.filteredVisibilityEnabled =
		(filteredVisibilityEffective ? 1u : 0u) |
		(filteredResourcesReady ? 2u : 0u) |
		(shadowReady ? 4u : 0u) |
		(mSettings.filteredVisibility ? 8u : 0u) |
		(std::min(BuildNRITraceSettingsFromCVars().portalDepth, 8u) << 8u);
	mStatus.requestedLightMode = mSettings.lightMode;
	mStatus.effectiveLightMode = constants.lightMode;
	mStatus.filteredVisibilityRequested = mSettings.filteredVisibility;
	mStatus.filteredVisibilityEffective = filteredVisibilityEffective;
	mStatus.forceOpaqueVisibility = constants.lightMode >= 2u && shadowReady && !filteredVisibilityEffective;
	mStatus.shadowTlasReady = shadowReady;
	uint32_t effectiveIndirectCacheMode = mSettings.indirectCacheMode;
	if (effectiveIndirectCacheMode >= 2u && !reprojectionResourcesReady)
		effectiveIndirectCacheMode = 1u;
	mStatus.indirectCacheModeRequested = mSettings.indirectCacheMode;
	mStatus.indirectCacheModeEffective = effectiveIndirectCacheMode;
	const uint64_t indirectSectorHash = renderer.mSectorLightingPayloadHash;
	const uint64_t indirectSkyKey = renderer.mSkyEnvironment.ActiveKey();
	const uint64_t indirectEmissiveHash = renderer.mEmissiveSamplingPayloadHash;
	const bool indirectHistoryCompatible = mIndirectHistoryValid && !renderer.mResetHistory &&
		mLastIndirectCacheMode == effectiveIndirectCacheMode &&
		mLastIndirectSectorHash == indirectSectorHash && mLastIndirectSkyKey == indirectSkyKey &&
		mLastIndirectEmissiveHash == indirectEmissiveHash;
	uint64_t directDirectionalHash = 1469598103934665603ull;
	directDirectionalHash = HashCombine64(directDirectionalHash, constants.directionalColorPacked);
	directDirectionalHash = HashCombine64(directDirectionalHash, FloatBits(constants.directionalDirectionX));
	directDirectionalHash = HashCombine64(directDirectionalHash, FloatBits(constants.directionalDirectionY));
	directDirectionalHash = HashCombine64(directDirectionalHash, FloatBits(constants.directionalDirectionZ));
	directDirectionalHash = HashCombine64(directDirectionalHash, FloatBits(constants.directionalAngularSize));
	directDirectionalHash = HashCombine64(directDirectionalHash, constants.lightSourceFlags & 0x7u);
	const bool gridRepresentationActive = mStatus.representationEffective != 0u;
	const bool worldEmissiveRequested = gridRepresentationActive &&
		mSettings.emissiveBackend != (uint32_t)NRISmokeEmissiveBackend::Legacy;
	const bool worldEmissiveReady = worldEmissiveRequested && mGridLighting.IsWorldReady() && emissiveLightsReady;
	const bool worldEmissiveCompare = worldEmissiveReady &&
		mSettings.emissiveBackend == (uint32_t)NRISmokeEmissiveBackend::Compare;
	const uint32_t effectiveDirectReuseMode = gridRepresentationActive ? mSettings.directReuseMode : 0u;
	const uint32_t directVisibilityBackend = constants.filteredVisibilityEnabled & 0xfu;
	const uint32_t emissiveLaneCount = gridRepresentationActive ? (1u << std::min(mSettings.quality, 2u)) : 1u;
	const uint32_t emissiveVisibilityBackend = constants.filteredVisibilityEnabled & 0xfu;
	const bool directHistoryCompatible = gridRepresentationActive && mDirectHistoryValid &&
		!renderer.mResetHistory && mLastDirectFrame + 1u == renderer.mFrameIndex &&
		mLastDirectReuseMode == effectiveDirectReuseMode &&
		mLastDirectReferenceMode == mSettings.directReferenceMode &&
		mLastDirectQuality == std::min(mSettings.quality, 2u) &&
		mLastDirectLightMode == constants.lightMode &&
		mLastDirectLightSamples == constants.lightSamples && mLastDirectSimulationEpoch == mStatus.simulationEpoch &&
		mLastDirectVisibilityBackend == directVisibilityBackend && mLastDirectDirectionalHash == directDirectionalHash;
	constants.flags = (mSettings.readback || mSettings.traceMode > 0u) ? 2u : 0u;
	if (mStatus.representationEffective == 2u)
		constants.flags |= kSmokeFlagCompareRepresentation;
	if (gridRepresentationActive)
		constants.flags |= kSmokeFlagGridRepresentation;
	constants.flags |= (effectiveDirectReuseMode & 3u) << kSmokeFlagDirectReuseShift;
	constants.flags |= (mSettings.directReferenceMode & 3u) << kSmokeFlagDirectReferenceShift;
	if (directHistoryCompatible)
		constants.flags |= kSmokeFlagDirectHistoryValid;
	if (worldEmissiveReady)
		constants.flags |= kSmokeFlagGridLightingWorld;
	if (worldEmissiveCompare)
		constants.flags |= kSmokeFlagGridLightingCompare;
	if (worldEmissiveReady && mSettings.emissiveWorldFilter && mGridLighting.GetStatusSnapshot().filterAllocated)
		constants.flags |= kSmokeFlagGridLightingFilter;
	if (mSettings.emissiveLegacyGatherDisabled)
		constants.flags |= kSmokeFlagEmissiveLegacyGatherDisabled;
	if (mSettings.emissiveQuarterKey)
		constants.flags |= kSmokeFlagEmissiveQuarterKey;
	if (worldEmissiveReady && mGridLighting.GetFieldPing() != 0u)
		constants.flags |= kSmokeFlagGridLightingFieldPing;
	if (worldEmissiveReady)
		constants.flags |= (mSettings.emissiveWorldDebug & 7u) << kSmokeFlagGridLightingDebugShift;
	if (worldEmissiveReady && mSettings.emissiveLocalProposals)
		constants.flags |= kSmokeFlagGridLightingLocalProposals;
	constants.flags |= (effectiveIndirectCacheMode & 3u) << 2u;
	constants.flags |= (std::min(mSettings.quality, 2u) & 3u) << 5u;
	if (indirectHistoryCompatible)
		constants.flags |= 0x10u;
	else if (effectiveIndirectCacheMode > 0u)
		constants.flags |= 0x80u;
	const bool emissiveHistoryCompatible = mEmissiveHistoryValid && !renderer.mResetHistory &&
		mLastEmissiveFrame + 1u == renderer.mFrameIndex &&
		mLastEmissiveReuseMode == mSettings.emissiveReuseMode &&
		mLastEmissiveGeneration == emissiveGeneration &&
		mLastEmissiveRepresentation == mStatus.representationEffective &&
		mLastEmissiveLaneCount == emissiveLaneCount &&
		mLastEmissiveLightMode == constants.lightMode &&
		mLastEmissiveVisibilityBackend == emissiveVisibilityBackend;
	if (emissiveHistoryCompatible)
		constants.flags |= 0x100u;
	constants.flags |= (mSettings.emissiveReuseMode & 3u) << 9u;
	if (mSettings.emissiveReference)
		constants.flags |= 0x800u;
	mStatus.emissiveReuseModeRequested = mSettings.emissiveReuseMode;
	mStatus.emissiveReuseModeEffective = emissiveLightsReady ? mSettings.emissiveReuseMode : 0u;
	mStatus.emissiveLaneCount = emissiveLaneCount;
	mStatus.emissiveReference = emissiveLightsReady && mSettings.emissiveReference;
	mStatus.emissiveHistoryValid = emissiveHistoryCompatible;
	mStatus.directReuseModeRequested = mSettings.directReuseMode;
	mStatus.directReuseModeEffective = effectiveDirectReuseMode;
	mStatus.directReferenceMode = gridRepresentationActive ? mSettings.directReferenceMode : 0u;
	mStatus.directHistoryValid = directHistoryCompatible;
	if (directHistoryCompatible)
		mStatus.directHistoryResetReason = "none";
	else if (!gridRepresentationActive)
		mStatus.directHistoryResetReason = "particle-backend";
	else if (renderer.mResetHistory)
		mStatus.directHistoryResetReason = "renderer-reset";
	else if (!mDirectHistoryValid)
		mStatus.directHistoryResetReason = "uninitialized";
	else if (mLastDirectFrame + 1u != renderer.mFrameIndex)
		mStatus.directHistoryResetReason = "frame-gap";
	else if (mLastDirectSimulationEpoch != mStatus.simulationEpoch)
		mStatus.directHistoryResetReason = "smoke-reset";
	else if (mLastDirectDirectionalHash != directDirectionalHash)
		mStatus.directHistoryResetReason = "directional-change";
	else
		mStatus.directHistoryResetReason = "mode-change";
	// Ordinary emissive payload churn is handled by current-frame reservoir
	// evaluation and the volume neighborhood clamp. Resetting the final
	// volume layer for every animated/dynamic emissive would prevent history
	// from ever accumulating in a live scene.
	const uint64_t volumeLightingHash = renderer.mSectorLightingPayloadHash ^
		(renderer.mSkyEnvironment.ActiveKey() * 0x9e3779b97f4a7c15ull);
	const bool volumeHistoryCompatible = mSettings.volumeHistory && mVolumeHistoryValid && mLastVolumeHistoryEnabled &&
		!renderer.mResetHistory && mLastVolumeFrame + 1u == renderer.mFrameIndex &&
		mLastVolumeWidth == route.width && mLastVolumeHeight == route.height &&
		mLastVolumePlacement == (uint32_t)route.placement && mLastVolumeSimulationEpoch == mStatus.simulationEpoch &&
		mLastVolumeLightingHash == volumeLightingHash;
	if (mSettings.volumeHistory)
		constants.flags |= 0x2000u;
	if (volumeHistoryCompatible)
		constants.flags |= 0x1000u;
	mStatus.volumeHistoryRequested = mSettings.volumeHistory;
	mStatus.volumeHistoryEffective = mSettings.volumeHistory && reprojectionResourcesReady;
	mStatus.volumeHistoryValid = volumeHistoryCompatible;
	mStatus.volumeHistoryAge = volumeHistoryCompatible ? std::min(mStatus.volumeHistoryAge + 1u, 255u) : 0u;
	mStatus.volumeResolvedSlot = (uint32_t)(writePing ? NRIRenderer::FrameTextureSlot::SmokeVolumeHistoryPing : NRIRenderer::FrameTextureSlot::SmokeVolumeHistoryPong);
	mStatus.volumeMetaSlot = (uint32_t)(writePing ? NRIRenderer::FrameTextureSlot::SmokeVolumeMetaPing : NRIRenderer::FrameTextureSlot::SmokeVolumeMetaPong);
	mStatus.volumeHistoryBytes = volumeCurrent.memorySize + volumeCurrentMeta.memorySize + volumeHistoryRead.memorySize +
		volumeHistoryWrite.memorySize + volumeMetaRead.memorySize + volumeMetaWrite.memorySize;
	if (volumeHistoryCompatible)
		mStatus.volumeHistoryResetReason = "none";
	else if (!mSettings.volumeHistory)
		mStatus.volumeHistoryResetReason = "disabled";
	else if (!reprojectionResourcesReady)
		mStatus.volumeHistoryResetReason = "missing-reprojection";
	else if (renderer.mResetHistory)
		mStatus.volumeHistoryResetReason = "renderer-reset";
	else if (!mVolumeHistoryValid)
		mStatus.volumeHistoryResetReason = "uninitialized";
	else if (!mLastVolumeHistoryEnabled)
		mStatus.volumeHistoryResetReason = "mode-change";
	else if (mLastVolumeFrame + 1u != renderer.mFrameIndex)
		mStatus.volumeHistoryResetReason = "frame-gap";
	else if (mLastVolumeWidth != route.width || mLastVolumeHeight != route.height)
		mStatus.volumeHistoryResetReason = "extent-change";
	else if (mLastVolumePlacement != (uint32_t)route.placement)
		mStatus.volumeHistoryResetReason = "route-change";
	else if (mLastVolumeSimulationEpoch != mStatus.simulationEpoch)
		mStatus.volumeHistoryResetReason = "smoke-reset";
	else if (mLastVolumeLightingHash != volumeLightingHash)
		mStatus.volumeHistoryResetReason = "lighting-change";
	else
		mStatus.volumeHistoryResetReason = "incompatible";
	auto dispatch = [&](NRISmokePass pass, uint32_t x, uint32_t y, uint32_t z)
	{
		renderer.mFrameBuffer->mCore.CmdBeginAnnotation(*renderer.mFrameBuffer->mCommandBuffer,
			kSmokePipelineNames[(uint32_t)pass], nri::BGRA_UNUSED);
		constants.pass = (uint32_t)pass;
		renderer.mFrameBuffer->mCore.CmdSetRootConstants(*renderer.mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
		renderer.mFrameBuffer->mCore.CmdSetPipeline(*renderer.mFrameBuffer->mCommandBuffer, *mPipelines[(uint32_t)pass]);
		renderer.mFrameBuffer->mCore.CmdDispatch(*renderer.mFrameBuffer->mCommandBuffer, { x, y, z });
		renderer.mFrameBuffer->mCore.CmdEndAnnotation(*renderer.mFrameBuffer->mCommandBuffer);
	};
	auto storageBarrier = [&]()
	{
		nri::BufferBarrierDesc barriers[kSmokeBarrierBufferCount] = {};
		nri::Buffer* buffers[] = { mParticles.buffer, mControl.buffer, mFineCells.buffer, mReferenceNext.buffer,
			mFroxelMedium.buffer, mFroxelIntegrated.buffer, mWideCells.buffer, mGlobalDepthCells.buffer, mFroxelPhase.buffer,
			mFroxelSource.buffer, mOccupiedFroxelIndices.buffer, mIndirectHistory.buffer, mIndirectScratch.buffer,
			mParticleDirectionalVisibility.buffer, mEmissiveCurrent.buffer, mEmissiveTemporal.buffer, mEmissiveHistory.buffer,
			mDirectCurrent.buffer, mDirectHistory.buffer };
		for (uint32_t i = 0; i < kSmokeBarrierBufferCount; ++i) { barriers[i].buffer = buffers[i]; barriers[i].before = StorageAccess(); barriers[i].after = StorageAccess(); }
		nri::BarrierDesc barrier = {}; barrier.buffers = barriers; barrier.bufferNum = kSmokeBarrierBufferCount;
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, barrier);
	};
	renderer.mFrameBuffer->mCore.CmdSetPipelineLayout(*renderer.mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 0, slot.inputSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 1, slot.bufferSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 2, slot.textureSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 3, slot.outputSet, nri::BindPoint::COMPUTE });
	if (lightBuffersReady)
		renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 4, slot.lightSet, nri::BindPoint::COMPUTE });
	if (filteredResourcesReady || shadowReady || sectorLightResourcesReady || skyResourceReady || reprojectionResourcesReady)
		renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 5, slot.filteredSceneSet, nri::BindPoint::COMPUTE });
	if (worldEmissiveReady)
		mGridLighting.Record(BuildGridServices(renderer), mSettings, constants, emissiveResourcesReady);
	const uint64_t froxelCount = (uint64_t)mResourceFroxelWidth * mResourceFroxelHeight * mResourceFroxelDepth;
	const uint64_t wideCellCount = (uint64_t)kWideCellCount * mResourceFroxelDepth;
	dispatch(NRISmokePass::Clear, Groups(std::max({ froxelCount, wideCellCount, (uint64_t)mResourceFroxelDepth })), 1, 1);
	storageBarrier();
	const bool renderParticles = mStatus.representationEffective != 1u;
	const bool renderGrid = mStatus.representationEffective != 0u;
	if (renderParticles)
	{
		dispatch(NRISmokePass::Bin, Groups(mSettings.particleCapacity), 1, 1);
		storageBarrier();
		if (directionalLightReady && constants.lightMode > 0u)
		{
			dispatch(NRISmokePass::LightDirectionalCarriers,
				(kDirectionalProbesPerParticle + kDirectionalProbeThreadGroupWidth - 1u) / kDirectionalProbeThreadGroupWidth,
				(mSettings.particleCapacity + kDirectionalParticleThreadGroupHeight - 1u) / kDirectionalParticleThreadGroupHeight, 1);
			storageBarrier();
		}
		dispatch(NRISmokePass::EvaluateMedium, (mResourceFroxelWidth + 3) / 4, (mResourceFroxelHeight + 3) / 4, (mResourceFroxelDepth + 3) / 4);
		storageBarrier();
	}
	if (renderGrid)
	{
		dispatch(NRISmokePass::EvaluateGrid, (mResourceFroxelWidth + 3) / 4,
			(mResourceFroxelHeight + 3) / 4, (mResourceFroxelDepth + 3) / 4);
		storageBarrier();
	}
	dispatch(NRISmokePass::LightPoint, Groups(froxelCount), 1, 1);
	storageBarrier();
	dispatch(NRISmokePass::LightDirectional, Groups(froxelCount), 1, 1);
	storageBarrier();
	if (renderGrid)
	{
		if (effectiveDirectReuseMode >= 1u)
		{
			dispatch(NRISmokePass::LightDirectTemporal, Groups(froxelCount), 1, 1);
			storageBarrier();
		}
		dispatch(NRISmokePass::LightDirectSpatial, Groups(froxelCount), 1, 1);
		storageBarrier();
		mDirectHistoryValid = true;
		mLastDirectFrame = renderer.mFrameIndex;
		mLastDirectReuseMode = effectiveDirectReuseMode;
		mLastDirectReferenceMode = mSettings.directReferenceMode;
		mLastDirectQuality = std::min(mSettings.quality, 2u);
		mLastDirectLightMode = constants.lightMode;
		mLastDirectLightSamples = constants.lightSamples;
		mLastDirectSimulationEpoch = mStatus.simulationEpoch;
		mLastDirectVisibilityBackend = directVisibilityBackend;
		mLastDirectDirectionalHash = directDirectionalHash;
	}
	else
	{
		mDirectHistoryValid = false;
		mLastDirectFrame = UINT32_MAX;
	}
	const bool runLegacyEmissive = renderParticles || !worldEmissiveReady;
	if (runLegacyEmissive)
	{
		dispatch(NRISmokePass::LightEmissiveInitial, Groups(froxelCount), 1, 1);
		storageBarrier();
		dispatch(NRISmokePass::LightEmissiveTemporal, Groups(froxelCount), 1, 1);
		storageBarrier();
		dispatch(NRISmokePass::LightEmissiveSpatial, Groups(froxelCount), 1, 1);
		storageBarrier();
	}
	if (emissiveLightsReady && runLegacyEmissive)
	{
		mEmissiveHistoryValid = true;
		mLastEmissiveReuseMode = mSettings.emissiveReuseMode;
		mLastEmissiveGeneration = emissiveGeneration;
		mLastEmissiveFrame = renderer.mFrameIndex;
		mLastEmissiveRepresentation = mStatus.representationEffective;
		mLastEmissiveLaneCount = emissiveLaneCount;
		mLastEmissiveLightMode = constants.lightMode;
		mLastEmissiveVisibilityBackend = emissiveVisibilityBackend;
	}
	else
	{
		mEmissiveHistoryValid = false;
		mStatus.emissiveHistoryValid = false;
		mLastEmissiveFrame = UINT32_MAX;
		mLastEmissiveRepresentation = UINT32_MAX;
		mLastEmissiveLaneCount = 0;
		mLastEmissiveLightMode = 0;
		mLastEmissiveVisibilityBackend = 0;
	}
	if (mSettings.indirect && indirectResourcesReady && mSettings.indirectScale > 0.0f)
	{
		dispatch(NRISmokePass::LightIndirectReference, Groups(froxelCount), 1, 1);
		storageBarrier();
		if (effectiveIndirectCacheMode > 0u)
		{
			dispatch(NRISmokePass::LightIndirectTemporal, Groups(froxelCount), 1, 1);
			storageBarrier();
			dispatch(NRISmokePass::LightIndirectSpatial, Groups(froxelCount), 1, 1);
			storageBarrier();
			mIndirectHistoryValid = true;
			mLastIndirectCacheMode = effectiveIndirectCacheMode;
			mLastIndirectSectorHash = indirectSectorHash;
			mLastIndirectSkyKey = indirectSkyKey;
			mLastIndirectEmissiveHash = indirectEmissiveHash;
		}
		else
		{
			mIndirectHistoryValid = false;
		}
	}
	else
	{
		mIndirectHistoryValid = false;
	}
	dispatch(NRISmokePass::Integrate, (mResourceFroxelWidth + 7) / 8, (mResourceFroxelHeight + 7) / 8, 1);
	storageBarrier();
	dispatch(NRISmokePass::ResolveVolume, (route.width + 7) / 8, (route.height + 7) / 8, 1);
	renderer.mFrameBuffer->TransitionTexture(volumeCurrent, { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(volumeCurrentMeta, { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER });
	dispatch(NRISmokePass::TemporalVolume, (route.width + 7) / 8, (route.height + 7) / 8, 1);
	renderer.mFrameBuffer->TransitionTexture(volumeHistoryWrite, { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(volumeMetaWrite, { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER });
	dispatch(NRISmokePass::Composite, (route.width + 7) / 8, (route.height + 7) / 8, 1);
	mVolumeHistoryValid = true;
	mLastVolumeFrame = renderer.mFrameIndex;
	mLastVolumeWidth = route.width;
	mLastVolumeHeight = route.height;
	mLastVolumePlacement = (uint32_t)route.placement;
	mLastVolumeSimulationEpoch = mStatus.simulationEpoch;
	mLastVolumeHistoryEnabled = mSettings.volumeHistory;
	mLastVolumeLightingHash = volumeLightingHash;
	if (mSettings.readback)
	{
		nri::BufferBarrierDesc copyBarriers[2] = {};
		copyBarriers[0].buffer = mControl.buffer;
		copyBarriers[0].before = StorageAccess();
		copyBarriers[0].after = NRIResourceCopySourceAccess();
		copyBarriers[1].buffer = slot.controlReadback.buffer;
		copyBarriers[1].before = slot.readbackInitialized ? NRIResourceCopyDestinationAccess() : nri::AccessStage{};
		copyBarriers[1].after = NRIResourceCopyDestinationAccess();
		nri::BarrierDesc barrier = {};
		barrier.buffers = copyBarriers;
		barrier.bufferNum = 2;
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, barrier);
		renderer.mFrameBuffer->mCore.CmdCopyBuffer(*renderer.mFrameBuffer->mCommandBuffer, *slot.controlReadback.buffer, 0, *mControl.buffer, 0, sizeof(NRISmokeControlGpu));
		slot.readbackPending = true;
		slot.readbackInitialized = true;
		mControlCopyPending = true;
	}
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
	mStatus.dlrrModeEffective = route.placement == NRISmokeRoutePlacement::DlrrPreUpscaleMainInput && route.supported ? 1u : 0u;
	mStatus.exposureDomain = (uint32_t)route.exposureDomain;
	if (!mSettings.enabled || !mStatus.mainViewEligible || !route.supported)
	{
		mStatus.volumeResolvedSlot = UINT32_MAX;
		mStatus.volumeMetaSlot = UINT32_MAX;
		mStatus.volumeHistoryValid = false;
		if (route.supported)
			renderer.CopyTexture(renderer.GetFrameTexture(route.inputSlot), renderer.GetFrameTexture(route.outputSlot));
		return true;
	}
	if (mStatus.representationEffective == 0u && !mMayHaveParticleSmoke)
	{
		mStatus.volumeResolvedSlot = UINT32_MAX;
		mStatus.volumeMetaSlot = UINT32_MAX;
		mStatus.volumeHistoryValid = false;
		mVolumeHistoryValid = false;
		mIndirectHistoryValid = false;
		mEmissiveHistoryValid = false;
		mStatus.emissiveHistoryValid = false;
		mLastEmissiveFrame = UINT32_MAX;
		mLastEmissiveRepresentation = UINT32_MAX;
		mLastEmissiveLaneCount = 0;
		mLastEmissiveLightMode = 0;
		mLastEmissiveVisibilityBackend = 0;
		mDirectHistoryValid = false;
		mLastDirectFrame = UINT32_MAX;
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
	mStatus.gpuStatsValid = false;
	mStatus.activeParticles = 0;
	mStatus.spawnedParticles = 0;
	mStatus.expiredParticles = 0;
	mStatus.liveEvictions = 0;
	mStatus.columnOverflow = 0;
	mStatus.wideParticlesProjected = 0;
	mStatus.wideGlobalDrops = 0;
	mStatus.fineColumnReferences = 0;
	mStatus.wideCellReferences = 0;
	mStatus.globalDepthReferences = 0;
	mStatus.referenceInvalidLinks = 0;
	mStatus.referenceTraversalLimitExits = 0;
	mStatus.fineTierParticles = 0;
	mStatus.wideTierParticles = 0;
	mStatus.globalTierParticles = 0;
	mStatus.fineOccupiedCells = 0;
	mStatus.wideOccupiedCells = 0;
	mStatus.globalOccupiedSlices = 0;
	mStatus.fineMaximumCellReferences = 0;
	mStatus.wideMaximumCellReferences = 0;
	mStatus.globalMaximumCellReferences = 0;
	mStatus.maximumDepthSpan = 0;
	mStatus.maximumCandidatesPerFroxel = 0;
	mStatus.occupiedCount = 0;
	mStatus.occupiedOverflow = 0;
	mStatus.mediumCandidateTests = 0;
	mStatus.pointFroxelsProcessed = 0;
	mStatus.directionalFroxelsProcessed = 0;
	mStatus.directionalSamples = 0;
	mStatus.directionalShadowRays = 0;
	mStatus.directionalShadowVisible = 0;
	mStatus.directionalShadowOccluded = 0;
	mStatus.directionalRadianceClamps = 0;
	mStatus.emissiveFroxelsProcessed = 0;
	mStatus.emissiveSamples = 0;
	mStatus.emissiveCandidateMisses = 0;
	mStatus.emissiveDistanceRejected = 0;
	mStatus.emissiveFacingRejected = 0;
	mStatus.emissiveShadowRays = 0;
	mStatus.emissiveShadowVisible = 0;
	mStatus.emissiveShadowOccluded = 0;
	mStatus.emissiveContributed = 0;
	mStatus.emissiveRadianceClamps = 0;
	mStatus.emissiveReservoirInitial = 0;
	mStatus.emissiveReservoirInvalid = 0;
	mStatus.emissiveTemporalAccepted = 0;
	mStatus.emissiveTemporalRejected = 0;
	mStatus.emissiveSpatialAccepted = 0;
	mStatus.emissiveSpatialRejected = 0;
	mStatus.emissiveFinalEvaluations = 0;
	mStatus.emissiveSourceClamps = 0;
	mStatus.emissiveRemovedEnergy = 0;
	mStatus.emissiveMaximumAge = 0;
	mStatus.emissiveReferenceSamples = 0;
	mStatus.emissiveReferenceRays = 0;
	mStatus.emissiveIdentityRejects = 0;
	mStatus.indirectFroxelsProcessed = 0;
	mStatus.indirectLocalityRays = 0;
	mStatus.indirectLocalityAgreement = 0;
	mStatus.indirectLocalityOneSided = 0;
	mStatus.indirectLocalityMismatch = 0;
	mStatus.indirectLocalityInvalid = 0;
	mStatus.indirectReferenceRays = 0;
	mStatus.indirectReferenceHits = 0;
	mStatus.indirectReferenceMisses = 0;
	mStatus.indirectSectorContributions = 0;
	mStatus.indirectSkyContributions = 0;
	mStatus.indirectEmissionContributions = 0;
	mStatus.indirectRadianceClamps = 0;
	mStatus.indirectNanRejects = 0;
	mStatus.indirectTemporalAccepted = 0;
	mStatus.indirectTemporalRejected = 0;
	mStatus.indirectSpatialAccepted = 0;
	mStatus.indirectSpatialRejected = 0;
	mStatus.indirectCacheMaximumAge = 0;
	mStatus.indirectCacheClamps = 0;
	mStatus.indirectCacheResolved = 0;
	mStatus.directReceiverSamples = 0;
	mStatus.directFractionalVisibility = 0;
	mStatus.directVisibilityZero = 0;
	mStatus.directVisibilityOne = 0;
	mStatus.directTemporalAccepted = 0;
	mStatus.directTemporalRejected = 0;
	mStatus.directSpatialAccepted = 0;
	mStatus.directSpatialRejected = 0;
	mStatus.directHistoryMaximumAge = 0;
	mStatus.directHistoryResolved = 0;
	mStatus.directHistoryClamps = 0;
	mStatus.directNanRejects = 0;
	mStatus.lightCandidatesTested = 0;
	mStatus.lightDistanceRejected = 0;
	mStatus.lightShadowRays = 0;
	mStatus.lightShadowVisible = 0;
	mStatus.lightShadowOccluded = 0;
	mStatus.lightSoftSamples = 0;
	mStatus.lightRadianceClamps = 0;
	mStatus.filterCandidateHits = 0;
	mStatus.filterAlphaRejects = 0;
	mStatus.filterNoShadowRejects = 0;
	mStatus.filterOneWayRejects = 0;
	mStatus.filterReflectionRejects = 0;
	mStatus.filterPortalContinuations = 0;
	mStatus.filterAcceptedBlockers = 0;
	mStatus.filterMisses = 0;
	mStatus.filterSkipLimitExits = 0;
	mStatus.filterContinuationLimitExits = 0;
	mStatus.filterResourceDowngrades = 0;
	mPendingCommands.clear();
	mAccumulator = 0.0f;
	mLastGameplaySeconds = -1.0;
	mParticleSimulationSeconds = 0.0;
	mLatestParticleDeathSeconds = 0.0;
	mMayHaveParticleSmoke = false;
	mLastPreparedFrame = UINT32_MAX;
	mLastSimulatedFrame = UINT32_MAX;
	mNeedsClear = true;
	mIndirectHistoryValid = false;
	mEmissiveHistoryValid = false;
	mStatus.emissiveHistoryValid = false;
	mLastEmissiveFrame = UINT32_MAX;
	mLastEmissiveRepresentation = UINT32_MAX;
	mLastEmissiveLaneCount = 0;
	mLastEmissiveLightMode = 0;
	mLastEmissiveVisibilityBackend = 0;
	mDirectHistoryValid = false;
	mLastDirectFrame = UINT32_MAX;
	mStatus.directHistoryValid = false;
	mStatus.directHistoryResetReason = mStatus.resetReason;
	mVolumeHistoryValid = false;
	mLastVolumeFrame = UINT32_MAX;
	mStatus.volumeHistoryValid = false;
	mStatus.volumeResolvedSlot = UINT32_MAX;
	mStatus.volumeMetaSlot = UINT32_MAX;
	mStatus.volumeHistoryAge = 0;
	mStatus.volumeHistoryResetReason = mStatus.resetReason;
	mEmitters.Reset();
	mGrid.Reset(mStatus.simulationEpoch, mStatus.resetReason);
	mGridLighting.Reset(mStatus.simulationEpoch, mStatus.resetReason);
}

void NRISmokeSystem::DestroyViewResources(NRIRenderer& renderer)
{
	auto destroy = [&](NRIBufferResource& resource) { renderer.DestroyBufferResource(resource); };
	destroy(mFineCells); destroy(mWideCells); destroy(mGlobalDepthCells); destroy(mFroxelMedium); destroy(mFroxelIntegrated);
	destroy(mFroxelPhase); destroy(mFroxelSource); destroy(mOccupiedFroxelIndices);
	destroy(mIndirectHistory); destroy(mIndirectScratch);
	destroy(mEmissiveCurrent); destroy(mEmissiveTemporal); destroy(mEmissiveHistory);
	destroy(mDirectCurrent); destroy(mDirectHistory);
	mViewResourcesInitialized = false;
	mIndirectHistoryValid = false;
	mEmissiveHistoryValid = false;
	mStatus.emissiveHistoryValid = false;
	mLastEmissiveFrame = UINT32_MAX;
	mLastEmissiveRepresentation = UINT32_MAX;
	mLastEmissiveLaneCount = 0;
	mLastEmissiveLightMode = 0;
	mLastEmissiveVisibilityBackend = 0;
	mDirectHistoryValid = false;
	mLastDirectFrame = UINT32_MAX;
	mStatus.indirectCacheBytes = 0;
	mStatus.emissiveReservoirBytes = 0;
	mStatus.directHistoryBytes = 0;
	mStatus.directHistoryValid = false;
	mVolumeHistoryValid = false;
	mLastVolumeFrame = UINT32_MAX;
	mStatus.volumeHistoryValid = false;
	mResourceFroxelWidth = mResourceFroxelHeight = mResourceFroxelDepth = 0;
	mResourceLegacyEmissiveFull = true;
}

void NRISmokeSystem::DestroyResources(NRIRenderer& renderer)
{
	auto destroy = [&](NRIBufferResource& resource) { renderer.DestroyBufferResource(resource); };
	DestroyViewResources(renderer);
	destroy(mStyleBuffer); destroy(mParticles); destroy(mControl); destroy(mReferenceNext); destroy(mParticleDirectionalVisibility);
	for (CommandSlot& slot : mCommandSlots)
	{
		destroy(slot.upload); destroy(slot.device); destroy(slot.styleUpload); destroy(slot.controlReadback);
		slot.readbackPending = false;
		slot.initialized = false;
		slot.readbackInitialized = false;
	}
	mControlCopyPending = false;
	mResourcesInitialized = false;
	mResourceParticleCapacity = mResourceStyleCapacity = 0;
}

void NRISmokeSystem::Shutdown(NRIRenderer& renderer)
{
	mGridLighting.Shutdown(BuildGridServices(renderer));
	mGrid.Shutdown(BuildGridServices(renderer));
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
	Printf("NRI PT smoke representation: requested=%u effective=%u grid=%s fallback=%s particle_lifetime_active=%s\n",
		mStatus.representationRequested, mStatus.representationEffective, mStatus.gridReady ? "ready" : "unavailable",
		mStatus.representationFallback, mMayHaveParticleSmoke ? "yes" : "no");
	mGrid.PrintStatus();
	const NRISmokeGridLightingStatusSnapshot& world = mGridLighting.GetStatusSnapshot();
	Printf("NRI PT smoke grid emissive: requested_backend=%u effective_backend=%u authority=%s ready=%s cells=%u ping=%u field_mib=%.2f work_mib=%.2f links_mib=%.2f proposal_mib=%.3f filter=%s filter_mib=%.2f total_mib=%.2f proposal=%s field_readback=0\n",
		world.requestedBackend, world.effectiveBackend, world.authority, world.resourcesReady ? "yes" : "no",
		world.cellCapacity, world.fieldPing, (double)world.fieldBytes / (1024.0 * 1024.0),
		(double)world.workBytes / (1024.0 * 1024.0), (double)world.linkBytes / (1024.0 * 1024.0),
		(double)world.proposalBytes / (1024.0 * 1024.0),
		world.filterDecision, (double)world.filterBytes / (1024.0 * 1024.0),
		(double)world.totalBytes / (1024.0 * 1024.0), world.proposalDecision);
	const char* placement = mStatus.routePlacement == (uint32_t)NRISmokeRoutePlacement::DlrrPostUpscale ? "dlrr_post_upscale" :
		(mStatus.routePlacement == (uint32_t)NRISmokeRoutePlacement::DlrrPreUpscaleMainInput ? "dlrr_pre_upscale_main" : "standard_pre_upscale");
	const char* inputName = mStatus.inputSlot < (uint32_t)NRIRenderer::FrameTextureSlot::Count ? renderer.GetFrameTextureSlotName((NRIRenderer::FrameTextureSlot)mStatus.inputSlot) : "none";
	const char* outputName = mStatus.outputSlot < (uint32_t)NRIRenderer::FrameTextureSlot::Count ? renderer.GetFrameTextureSlotName((NRIRenderer::FrameTextureSlot)mStatus.outputSlot) : "none";
	Printf("NRI PT smoke status: enabled=%s epoch=%u main_view=%s route_supported=%s placement=%s input=%s output=%s extent=%ux%u froxels=%ux%ux%u particles=%u styles=%u commands=%u commands_total=%llu dropped=%u substeps=%u light_mode_requested=%u light_mode_effective=%u light_samples=%u light_candidates_max=%u point_lights=%s filtered_visibility_requested=%s filtered_visibility_effective=%s visibility_fallback=%s shadow_tlas=%s runtime_lights=%u gpu_stats=%s active=%u spawned=%u expired=%u evictions=%u column_overflow=%u reference_mode=complete reference_stride=512 invalid_links=%u traversal_limit=%u wide_projected=%u wide_global_drops=%u fine_refs=%u wide_refs=%u global_refs=%u tier_particles=%u/%u/%u occupied_cells=%u/%u/%u max_cell_refs=%u/%u/%u depth_span_max=%u carrier_candidates_max=%u medium_occupied=%u occupied_overflow=%u medium_candidate_tests=%u point_froxels=%u light_candidates=%u light_distance_rejected=%u light_shadow_rays=%u light_visible=%u light_occluded=%u light_soft_samples=%u light_clamps=%u filter_hits=%u filter_alpha=%u filter_no_shadow=%u filter_one_way=%u filter_reflection=%u filter_portals=%u filter_blockers=%u filter_misses=%u filter_skip_limit=%u filter_continuation_limit=%u filter_downgrades=%u resident_mib=%.2f particle_readback=0 control_readback=%llu reset=%s\n",
		mStatus.enabled ? "yes" : "no", mStatus.simulationEpoch, mStatus.mainViewEligible ? "yes" : "no", mStatus.routeSupported ? "yes" : "no", placement,
		inputName, outputName, mStatus.routeWidth, mStatus.routeHeight, mStatus.froxelWidth, mStatus.froxelHeight, mStatus.froxelDepth,
		mStatus.particleCapacity, mStatus.styleCount, mStatus.commandsUploaded, (unsigned long long)mStatus.commandsUploadedTotal, mStatus.commandsDropped, mStatus.simulationSubsteps,
		mStatus.requestedLightMode, mStatus.effectiveLightMode, mSettings.lightSamples, mSettings.maxLightCandidates, mSettings.pointLights ? "yes" : "no",
		mStatus.filteredVisibilityRequested ? "yes" : "no", mStatus.filteredVisibilityEffective ? "yes" : "no",
		mStatus.forceOpaqueVisibility ? "force_opaque" : "none", mStatus.shadowTlasReady ? "ready" : "missing", renderer.mBoundRuntimeLightCount,
		mStatus.gpuStatsValid ? "valid" : "disabled", mStatus.activeParticles, mStatus.spawnedParticles, mStatus.expiredParticles, mStatus.liveEvictions, mStatus.columnOverflow,
		mStatus.referenceInvalidLinks, mStatus.referenceTraversalLimitExits,
		mStatus.wideParticlesProjected, mStatus.wideGlobalDrops, mStatus.fineColumnReferences, mStatus.wideCellReferences, mStatus.globalDepthReferences,
		mStatus.fineTierParticles, mStatus.wideTierParticles, mStatus.globalTierParticles, mStatus.fineOccupiedCells, mStatus.wideOccupiedCells, mStatus.globalOccupiedSlices,
		mStatus.fineMaximumCellReferences, mStatus.wideMaximumCellReferences, mStatus.globalMaximumCellReferences, mStatus.maximumDepthSpan, mStatus.maximumCandidatesPerFroxel,
		mStatus.occupiedCount, mStatus.occupiedOverflow, mStatus.mediumCandidateTests, mStatus.pointFroxelsProcessed,
		mStatus.lightCandidatesTested, mStatus.lightDistanceRejected, mStatus.lightShadowRays, mStatus.lightShadowVisible, mStatus.lightShadowOccluded, mStatus.lightSoftSamples, mStatus.lightRadianceClamps,
		mStatus.filterCandidateHits, mStatus.filterAlphaRejects, mStatus.filterNoShadowRejects, mStatus.filterOneWayRejects, mStatus.filterReflectionRejects,
		mStatus.filterPortalContinuations, mStatus.filterAcceptedBlockers, mStatus.filterMisses, mStatus.filterSkipLimitExits, mStatus.filterContinuationLimitExits, mStatus.filterResourceDowngrades,
		(double)mStatus.residentBytes / (1024.0 * 1024.0), (unsigned long long)mStatus.controlReadbackBytes, mStatus.resetReason);
	Printf("NRI PT smoke lighting status: directional=%s resolved=%s shadow=%s directional_probe_grid=20x20x20 directional_probe_cell=2.0 directional_probe_mib=%.2f directional_froxels=%u directional_samples=%u directional_shadow_rays=%u directional_visible=%u directional_occluded=%u directional_clamps=%u emissive=%s emissive_primitives=%u emissive_froxels=%u emissive_samples=%u emissive_no_candidate=%u emissive_distance_rejected=%u emissive_facing_rejected=%u emissive_shadow_rays=%u emissive_visible=%u emissive_occluded=%u emissive_contributed=%u emissive_clamps=%u\n",
		mSettings.directionalLight ? "yes" : "no", renderer.mDirectionalLightState.enabled ? "yes" : "no", renderer.mDirectionalLightState.shadow ? "yes" : "no",
		(double)mParticleDirectionalVisibility.memorySize / (1024.0 * 1024.0),
		mStatus.directionalFroxelsProcessed, mStatus.directionalSamples, mStatus.directionalShadowRays, mStatus.directionalShadowVisible, mStatus.directionalShadowOccluded, mStatus.directionalRadianceClamps,
		mSettings.emissiveLights ? "yes" : "no", renderer.mBoundEmissivePrimitiveCount, mStatus.emissiveFroxelsProcessed, mStatus.emissiveSamples,
		mStatus.emissiveCandidateMisses, mStatus.emissiveDistanceRejected, mStatus.emissiveFacingRejected, mStatus.emissiveShadowRays,
		mStatus.emissiveShadowVisible, mStatus.emissiveShadowOccluded, mStatus.emissiveContributed, mStatus.emissiveRadianceClamps);
	Printf("NRI PT smoke direct reconstruction: grid_only=yes reuse_requested=%u reuse_effective=%u reference=%u history=%s reset=%s cache_mib=%.2f receiver_samples=%u visibility_fractional=%u visibility_zero=%u visibility_one=%u temporal=%u/%u spatial=%u/%u maximum_age=%u resolved=%u clamps=%u nan=%u field_readback=0\n",
		mStatus.directReuseModeRequested, mStatus.directReuseModeEffective, mStatus.directReferenceMode,
		mStatus.directHistoryValid ? "valid" : "invalid", mStatus.directHistoryResetReason,
		(double)mStatus.directHistoryBytes / (1024.0 * 1024.0), mStatus.directReceiverSamples,
		mStatus.directFractionalVisibility, mStatus.directVisibilityZero, mStatus.directVisibilityOne,
		mStatus.directTemporalAccepted, mStatus.directTemporalRejected,
		mStatus.directSpatialAccepted, mStatus.directSpatialRejected,
		mStatus.directHistoryMaximumAge, mStatus.directHistoryResolved,
		mStatus.directHistoryClamps, mStatus.directNanRejects);
	Printf("NRI PT smoke emissive reservoir: reuse_requested=%u reuse_effective=%u lanes=%u reference=%s history=%s reservoir_mib=%.2f initialized=%u invalid=%u temporal=%u/%u spatial=%u/%u final=%u source_clamps=%u removed_energy=%u maximum_age=%u identity_rejects=%u reference_samples=%u reference_rays=%u field_readback=0\n",
		mStatus.emissiveReuseModeRequested, mStatus.emissiveReuseModeEffective, mStatus.emissiveLaneCount, mStatus.emissiveReference ? "yes" : "no",
		mStatus.emissiveHistoryValid ? "valid" : "invalid", (double)mStatus.emissiveReservoirBytes / (1024.0 * 1024.0),
		mStatus.emissiveReservoirInitial, mStatus.emissiveReservoirInvalid,
		mStatus.emissiveTemporalAccepted, mStatus.emissiveTemporalRejected,
		mStatus.emissiveSpatialAccepted, mStatus.emissiveSpatialRejected,
		mStatus.emissiveFinalEvaluations, mStatus.emissiveSourceClamps, mStatus.emissiveRemovedEnergy, mStatus.emissiveMaximumAge,
		mStatus.emissiveIdentityRejects, mStatus.emissiveReferenceSamples, mStatus.emissiveReferenceRays);
	Printf("NRI PT smoke indirect status: enabled=%s scale=%.3f cache_mode_requested=%u cache_mode_effective=%u samples=%u history=%s cache_mib=%.2f froxels=%u locality_rays=%u agreement=%u one_sided=%u mismatch=%u invalid=%u reference_rays=%u hits=%u misses=%u sector=%u sky=%u emission=%u clamps=%u nan=%u temporal=%u/%u spatial=%u/%u cache_age=%u cache_clamps=%u resolved=%u field_readback=0\n",
		mSettings.indirect ? "yes" : "no", mSettings.indirectScale, mStatus.indirectCacheModeRequested, mStatus.indirectCacheModeEffective, 1u << std::min(mSettings.quality, 2u),
		mIndirectHistoryValid ? "valid" : "invalid", (double)mStatus.indirectCacheBytes / (1024.0 * 1024.0), mStatus.indirectFroxelsProcessed,
		mStatus.indirectLocalityRays, mStatus.indirectLocalityAgreement, mStatus.indirectLocalityOneSided,
		mStatus.indirectLocalityMismatch, mStatus.indirectLocalityInvalid, mStatus.indirectReferenceRays,
		mStatus.indirectReferenceHits, mStatus.indirectReferenceMisses, mStatus.indirectSectorContributions,
		mStatus.indirectSkyContributions, mStatus.indirectEmissionContributions, mStatus.indirectRadianceClamps,
		mStatus.indirectNanRejects, mStatus.indirectTemporalAccepted, mStatus.indirectTemporalRejected,
		mStatus.indirectSpatialAccepted, mStatus.indirectSpatialRejected, mStatus.indirectCacheMaximumAge,
		mStatus.indirectCacheClamps, mStatus.indirectCacheResolved);
	const char* volumeName = mStatus.volumeResolvedSlot < (uint32_t)NRIRenderer::FrameTextureSlot::Count ?
		renderer.GetFrameTextureSlotName((NRIRenderer::FrameTextureSlot)mStatus.volumeResolvedSlot) : "none";
	const char* volumeMetaName = mStatus.volumeMetaSlot < (uint32_t)NRIRenderer::FrameTextureSlot::Count ?
		renderer.GetFrameTextureSlotName((NRIRenderer::FrameTextureSlot)mStatus.volumeMetaSlot) : "none";
	Printf("NRI PT smoke volume status: history_requested=%s history_effective=%s history_valid=%s history_age=%u reset=%s layer_mib=%.2f resolved=%s metadata=%s dlrr_mode_requested=%u dlrr_mode_effective=%u field_readback=0\n",
		mStatus.volumeHistoryRequested ? "yes" : "no", mStatus.volumeHistoryEffective ? "yes" : "no",
		mStatus.volumeHistoryValid ? "yes" : "no", mStatus.volumeHistoryAge, mStatus.volumeHistoryResetReason,
		(double)mStatus.volumeHistoryBytes / (1024.0 * 1024.0), volumeName, volumeMetaName,
		mStatus.dlrrModeRequested, mStatus.dlrrModeEffective);
}
