#include "nri_trace_stats.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
	static constexpr uint32_t NRI_TRACE_SHADER_STATS_COUNTER_COUNT = NRI_TRACE_SHADER_STAT_COUNT;
	static constexpr uint32_t NRI_SCENE_DATA_SOURCE_STATIC = 0;
	static constexpr uint32_t NRI_SCENE_DATA_SOURCE_DYNAMIC = 1;
	static constexpr uint32_t NRI_SCENE_DATA_SOURCE_PERSISTENT_VOXEL = 2;

	template<typename T>
	static T NRIFlags(T a, T b)
	{
		return (T)((uint32_t)a | (uint32_t)b);
	}

	static nri::AccessStage NRICopySourceAccess()
	{
		return { nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY };
	}

	static nri::AccessStage NRICopyDestinationAccess()
	{
		return { nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY };
	}
}

bool NRITraceShaderStats::CreateBufferWithoutViewAtLocation(
	const NRIResourceServices& services,
	NRIBufferResource& resource,
	uint64_t size,
	uint32_t stride,
	nri::BufferUsageBits usage,
	nri::MemoryLocation memoryLocation)
{
	const NRIResourceContext& context = services.context;
	if (context.device == nullptr || context.core == nullptr)
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
	resource.size = desc.size;
	resource.memorySize = memoryDesc.size;
	resource.memoryLocation = memoryLocation;
	resource.usedSize = size;
	resource.stride = stride;
	return true;
}

bool NRITraceShaderStats::Ensure(const NRIResourceServices& services)
{
	const NRIResourceContext& context = services.context;
	if (context.device == nullptr || context.core == nullptr)
	{
		return false;
	}

	constexpr uint32_t kStride = sizeof(uint32_t);
	const uint64_t byteSize = (uint64_t)NRI_TRACE_SHADER_STATS_COUNTER_COUNT * kStride;
	if (mStatsBuffer.buffer == nullptr || mStatsBuffer.shaderView == nullptr)
	{
		services.DestroyBufferResource(mStatsBuffer);
		nri::BufferDesc desc = {};
		desc.size = byteSize;
		desc.structureStride = kStride;
		desc.usage = NRIFlags(
			nri::BufferUsageBits::SHADER_RESOURCE_STORAGE,
			nri::BufferUsageBits::SHADER_RESOURCE);
		if (context.core->CreateCommittedBuffer(*context.device, nri::MemoryLocation::DEVICE, 0.0f, desc, mStatsBuffer.buffer) != nri::Result::SUCCESS)
		{
			return false;
		}

		nri::MemoryDesc memoryDesc = {};
		context.core->GetBufferMemoryDesc(*mStatsBuffer.buffer, nri::MemoryLocation::DEVICE, memoryDesc);
		mStatsBuffer.size = desc.size;
		mStatsBuffer.memorySize = memoryDesc.size;
		mStatsBuffer.memoryLocation = nri::MemoryLocation::DEVICE;
		mStatsBuffer.usedSize = byteSize;
		mStatsBuffer.stride = kStride;

		nri::BufferViewDesc viewDesc = {};
		viewDesc.buffer = mStatsBuffer.buffer;
		viewDesc.type = nri::BufferView::STORAGE_STRUCTURED_BUFFER;
		viewDesc.offset = 0;
		viewDesc.size = nri::WHOLE_SIZE;
		viewDesc.structureStride = kStride;
		if (context.core->CreateBufferView(viewDesc, mStatsBuffer.shaderView) != nri::Result::SUCCESS)
		{
			return false;
		}
	}

	if (mReadbackBuffer.buffer == nullptr)
	{
		if (!CreateBufferWithoutViewAtLocation(
			services,
			mReadbackBuffer,
			byteSize,
			kStride,
			nri::BufferUsageBits::NONE,
			nri::MemoryLocation::HOST_READBACK))
		{
			return false;
		}
	}

	if (mZeroBuffer.buffer == nullptr)
	{
		if (!CreateBufferWithoutViewAtLocation(
			services,
			mZeroBuffer,
			byteSize,
			kStride,
			nri::BufferUsageBits::NONE,
			nri::MemoryLocation::DEVICE_UPLOAD))
		{
			return false;
		}

		void* mapped = context.core->MapBuffer(*mZeroBuffer.buffer, 0, byteSize);
		if (mapped == nullptr)
		{
			return false;
		}
		std::memset(mapped, 0, (size_t)byteSize);
		context.core->UnmapBuffer(*mZeroBuffer.buffer);
	}

	return true;
}

void NRITraceShaderStats::Destroy(const NRIResourceServices& services)
{
	services.DestroyBufferResource(mStatsBuffer);
	services.DestroyBufferResource(mReadbackBuffer);
	services.DestroyBufferResource(mZeroBuffer);
	mPendingFrame = 0;
}

void NRITraceShaderStats::ResetBuffer(const NRIResourceServices& services, bool enabled)
{
	const NRIResourceContext& context = services.context;
	if (!enabled || context.commandBuffer == nullptr || context.core == nullptr || !Ensure(services))
	{
		return;
	}

	const uint64_t byteSize = (uint64_t)NRI_TRACE_SHADER_STATS_COUNTER_COUNT * sizeof(uint32_t);
	nri::BufferBarrierDesc beforeBarriers[2] = {};
	beforeBarriers[0].buffer = mZeroBuffer.buffer;
	beforeBarriers[0].before = {};
	beforeBarriers[0].after = NRICopySourceAccess();
	beforeBarriers[1].buffer = mStatsBuffer.buffer;
	beforeBarriers[1].before = {};
	beforeBarriers[1].after = NRICopyDestinationAccess();
	nri::BarrierDesc beforeDesc = {};
	beforeDesc.buffers = beforeBarriers;
	beforeDesc.bufferNum = 2;
	context.core->CmdBarrier(*context.commandBuffer, beforeDesc);
	context.core->CmdCopyBuffer(
		*context.commandBuffer,
		*mStatsBuffer.buffer,
		0,
		*mZeroBuffer.buffer,
		0,
		byteSize);

	nri::BufferBarrierDesc afterBarrier = {};
	afterBarrier.buffer = mStatsBuffer.buffer;
	afterBarrier.before = NRICopyDestinationAccess();
	afterBarrier.after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	nri::BarrierDesc afterDesc = {};
	afterDesc.buffers = &afterBarrier;
	afterDesc.bufferNum = 1;
	context.core->CmdBarrier(*context.commandBuffer, afterDesc);
}

void NRITraceShaderStats::CopyForReadback(const NRIResourceServices& services, bool enabled, uint64_t frameNumber)
{
	const NRIResourceContext& context = services.context;
	if (!enabled || context.commandBuffer == nullptr || context.core == nullptr || !Ensure(services))
	{
		return;
	}

	const uint64_t byteSize = (uint64_t)NRI_TRACE_SHADER_STATS_COUNTER_COUNT * sizeof(uint32_t);
	nri::BufferBarrierDesc beforeBarrier = {};
	beforeBarrier.buffer = mStatsBuffer.buffer;
	beforeBarrier.before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	beforeBarrier.after = NRICopySourceAccess();
	nri::BarrierDesc beforeDesc = {};
	beforeDesc.buffers = &beforeBarrier;
	beforeDesc.bufferNum = 1;
	context.core->CmdBarrier(*context.commandBuffer, beforeDesc);
	context.core->CmdCopyBuffer(
		*context.commandBuffer,
		*mReadbackBuffer.buffer,
		0,
		*mStatsBuffer.buffer,
		0,
		byteSize);
	mPendingFrame = frameNumber;
}

void NRITraceShaderStats::Readback(
	const NRIResourceServices& services,
	const NRITraceShaderStatsReadbackInput& input,
	NRITraceShaderStatsSnapshot& outStats)
{
	const NRIResourceContext& context = services.context;
	if (!input.enabled || mPendingFrame == 0 || mReadbackBuffer.buffer == nullptr || context.core == nullptr)
	{
		return;
	}

	services.WaitForCommands("trace_shader_stats_readback");
	const uint64_t byteSize = (uint64_t)NRI_TRACE_SHADER_STATS_COUNTER_COUNT * sizeof(uint32_t);
	const void* mapped = context.core->MapBuffer(*mReadbackBuffer.buffer, 0, byteSize);
	if (mapped == nullptr)
	{
		mPendingFrame = 0;
		return;
	}

	outStats.valid = true;
	outStats.frameNumber = mPendingFrame;
	std::memcpy(outStats.counters.data(), mapped, (size_t)byteSize);
	outStats.hotInstanceCount = 0;
	outStats.hotInstances = {};

	struct TraceShaderHotCandidate
	{
		uint32_t instanceId = 0;
		uint32_t committed = 0;
		uint32_t accepted = 0;
	};

	const std::vector<SceneInstanceData> emptySceneInstances;
	const std::vector<SceneInstanceData>& sceneInstances =
		input.boundSceneInstances != nullptr ? *input.boundSceneInstances : emptySceneInstances;
	std::vector<TraceShaderHotCandidate> hotCandidates;
	const uint32_t instanceBucketCount = std::min<uint32_t>((uint32_t)sceneInstances.size(), NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT);
	hotCandidates.reserve(instanceBucketCount);
	for (uint32_t instanceId = 0; instanceId < instanceBucketCount; ++instanceId)
	{
		const uint32_t committed = outStats.counters[NRI_TRACE_SHADER_INSTANCE_COMMITTED_BASE + instanceId];
		const uint32_t accepted = outStats.counters[NRI_TRACE_SHADER_INSTANCE_ACCEPTED_BASE + instanceId];
		if (committed == 0 && accepted == 0)
		{
			continue;
		}
		hotCandidates.push_back({ instanceId, committed, accepted });
	}
	std::sort(
		hotCandidates.begin(),
		hotCandidates.end(),
		[](const TraceShaderHotCandidate& a, const TraceShaderHotCandidate& b)
		{
			if (a.committed != b.committed)
			{
				return a.committed > b.committed;
			}
			return a.accepted > b.accepted;
		});

	auto getDataSourcePrimitiveTotal = [&input](uint32_t dataSource) -> uint32_t
	{
		switch (dataSource)
		{
		case NRI_SCENE_DATA_SOURCE_STATIC: return input.staticPrimitiveCount;
		case NRI_SCENE_DATA_SOURCE_DYNAMIC: return input.dynamicPrimitiveCount;
		case NRI_SCENE_DATA_SOURCE_PERSISTENT_VOXEL: return input.persistentVoxelPrimitiveCount;
		default: return 0;
		}
	};
	auto estimateInstancePrimitiveCount = [&input, &getDataSourcePrimitiveTotal, &sceneInstances](const SceneInstanceData& instance) -> uint32_t
	{
		if (instance.dataSource == NRI_SCENE_DATA_SOURCE_PERSISTENT_VOXEL && input.estimatePersistentVoxelPrimitiveCount != nullptr)
		{
			const uint32_t persistentVoxelPrimitiveCount = input.estimatePersistentVoxelPrimitiveCount(input.user, instance.primitiveOffset);
			if (persistentVoxelPrimitiveCount > 0)
			{
				return persistentVoxelPrimitiveCount;
			}
		}

		const uint32_t total = getDataSourcePrimitiveTotal(instance.dataSource);
		if (instance.primitiveOffset >= total)
		{
			return 0;
		}

		uint32_t endOffset = total;
		for (const SceneInstanceData& other : sceneInstances)
		{
			if (other.dataSource == instance.dataSource &&
				other.primitiveOffset > instance.primitiveOffset &&
				other.primitiveOffset < endOffset)
			{
				endOffset = other.primitiveOffset;
			}
		}
		return endOffset - instance.primitiveOffset;
	};

	const uint32_t hotCount = std::min<uint32_t>((uint32_t)hotCandidates.size(), NRI_TRACE_SHADER_HOT_INSTANCE_COUNT);
	outStats.hotInstanceCount = hotCount;
	for (uint32_t hotIndex = 0; hotIndex < hotCount; ++hotIndex)
	{
		const TraceShaderHotCandidate& candidate = hotCandidates[hotIndex];
		const SceneInstanceData& instance = sceneInstances[candidate.instanceId];
		NRITraceShaderHotInstance& hot = outStats.hotInstances[hotIndex];
		hot.instanceId = candidate.instanceId;
		hot.dataSource = instance.dataSource;
		hot.primitiveOffset = instance.primitiveOffset;
		hot.primitiveCount = estimateInstancePrimitiveCount(instance);
		hot.metadata0 = instance.reserved0;
		hot.metadata1 = instance.reserved1;
		hot.committed = candidate.committed;
		hot.accepted = candidate.accepted;
		hot.primaryCommitted = outStats.counters[NRI_TRACE_SHADER_INSTANCE_KIND_COMMITTED_BASE + 0u * NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT + candidate.instanceId];
		hot.ungatedCommitted = outStats.counters[NRI_TRACE_SHADER_INSTANCE_KIND_COMMITTED_BASE + 1u * NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT + candidate.instanceId];
		hot.sunCommitted = outStats.counters[NRI_TRACE_SHADER_INSTANCE_KIND_COMMITTED_BASE + 2u * NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT + candidate.instanceId];
		hot.pointCommitted = outStats.counters[NRI_TRACE_SHADER_INSTANCE_KIND_COMMITTED_BASE + 3u * NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT + candidate.instanceId];
		hot.emissiveCommitted = outStats.counters[NRI_TRACE_SHADER_INSTANCE_KIND_COMMITTED_BASE + 4u * NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT + candidate.instanceId];
		hot.fastEmissiveCommitted = outStats.counters[NRI_TRACE_SHADER_INSTANCE_KIND_COMMITTED_BASE + 5u * NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT + candidate.instanceId];
	}
	context.core->UnmapBuffer(*mReadbackBuffer.buffer);
	mPendingFrame = 0;
}
