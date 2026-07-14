#include "nri_renderer.h"
#include "nri_cvars.h"
#include "../system/nri_renderdevice.h"
#include "printf.h"

#include <algorithm>
#include <cstring>

void NRIRenderer::ResetPersistentVoxelBlasCompaction()
{
	for (NRIAccelerationStructureResource& destination : mPersistentVoxelBlasCompaction.destinations)
	{
		DestroyAccelerationStructureResource(destination);
	}
	mPersistentVoxelBlasCompaction.destinations.clear();
	mPersistentVoxelBlasCompaction.sources.clear();
	DestroyBufferResource(mPersistentVoxelBlasCompaction.readbackBuffer);
	if (mPersistentVoxelBlasCompaction.queryPool != nullptr)
	{
		mFrameBuffer->mCore.DestroyQueryPool(mPersistentVoxelBlasCompaction.queryPool);
		mPersistentVoxelBlasCompaction.queryPool = nullptr;
	}
	mPersistentVoxelBlasCompaction = {};
}

bool NRIRenderer::PumpPersistentVoxelBlasCompaction(uint64_t buildSerial)
{
	PersistentVoxelBlasCompactionState& state = mPersistentVoxelBlasCompaction;
	if (!(bool)nri_ptvoxelblascompact &&
		state.stage != PersistentVoxelBlasCompactionState::Stage::QueryPending &&
		state.stage != PersistentVoxelBlasCompactionState::Stage::CopyPending)
	{
		return true;
	}
	if (state.buildSerial != 0 && state.buildSerial != buildSerial)
	{
		ResetPersistentVoxelBlasCompaction();
	}
	if (state.stage == PersistentVoxelBlasCompactionState::Stage::Complete ||
		state.stage == PersistentVoxelBlasCompactionState::Stage::Failed)
	{
		return true;
	}

	auto fail = [&](const char* reason) -> bool
	{
		Printf(TEXTCOLOR_RED "PERF pt voxel blas compaction NRI: stage=failed build_serial=%llu reason=%s original_bytes=%llu compacted_bytes=%llu\n",
			(unsigned long long)buildSerial,
			reason,
			(unsigned long long)state.originalBytes,
			(unsigned long long)state.compactedBytes);
		for (NRIAccelerationStructureResource& destination : state.destinations)
		{
			DestroyAccelerationStructureResource(destination);
		}
		state.destinations.clear();
		DestroyBufferResource(state.readbackBuffer);
		if (state.queryPool != nullptr)
		{
			mFrameBuffer->mCore.DestroyQueryPool(state.queryPool);
			state.queryPool = nullptr;
		}
		state.stage = PersistentVoxelBlasCompactionState::Stage::Failed;
		return true;
	};

	if (state.stage == PersistentVoxelBlasCompactionState::Stage::Idle)
	{
		state.buildSerial = buildSerial;
		mPersistentVoxels.CollectResidentAccelerationStructures(state.sources);
		if (state.sources.empty())
		{
			state.stage = PersistentVoxelBlasCompactionState::Stage::Complete;
			return true;
		}
		for (const NRIAccelerationStructureResource* source : state.sources)
		{
			if (source == nullptr || source->accelerationStructure == nullptr ||
				!(((uint32_t)source->buildFlags & (uint32_t)nri::AccelerationStructureBits::ALLOW_COMPACTION) != 0))
			{
				return fail("source-not-compaction-capable");
			}
			state.originalBytes += source->memorySize;
		}

		nri::QueryPoolDesc queryDesc = {};
		queryDesc.queryType = nri::QueryType::ACCELERATION_STRUCTURE_COMPACTED_SIZE;
		queryDesc.capacity = (uint32_t)state.sources.size();
		if (mFrameBuffer->mCore.CreateQueryPool(*mFrameBuffer->mDevice, queryDesc, state.queryPool) != nri::Result::SUCCESS)
		{
			return fail("query-pool-create");
		}
		const uint32_t querySize = mFrameBuffer->mCore.GetQuerySize(*state.queryPool);
		if (querySize < sizeof(uint64_t) ||
			!CreateBufferWithoutViewAtLocation(
				state.readbackBuffer,
				(uint64_t)querySize * state.sources.size(),
				querySize,
				nri::BufferUsageBits::NONE,
				nri::MemoryLocation::HOST_READBACK))
		{
			return fail("query-readback-create");
		}

		std::vector<const nri::AccelerationStructure*> sourceHandles;
		sourceHandles.reserve(state.sources.size());
		for (const NRIAccelerationStructureResource* source : state.sources)
		{
			sourceHandles.push_back(source->accelerationStructure);
		}
		mFrameBuffer->mCore.ResetQueries(*state.queryPool, 0, (uint32_t)state.sources.size());
		mFrameBuffer->mRayTracing.CmdWriteAccelerationStructuresSizes(
			*mFrameBuffer->mCommandBuffer,
			sourceHandles.data(),
			(uint32_t)sourceHandles.size(),
			*state.queryPool,
			0);
		mFrameBuffer->mCore.CmdCopyQueries(
			*mFrameBuffer->mCommandBuffer,
			*state.queryPool,
			0,
			(uint32_t)state.sources.size(),
			*state.readbackBuffer.buffer,
			0);
		state.queryFence = GetRecordingCommandFenceValue();
		if (state.queryFence == 0)
		{
			return fail("query-fence");
		}
		state.stage = PersistentVoxelBlasCompactionState::Stage::QueryPending;
		Printf("PERF pt voxel blas compaction NRI: stage=query-submit build_serial=%llu resources=%u original_bytes=%llu query_bytes=%llu fence=%llu\n",
			(unsigned long long)buildSerial,
			(uint32_t)state.sources.size(),
			(unsigned long long)state.originalBytes,
			(unsigned long long)state.readbackBuffer.memorySize,
			(unsigned long long)state.queryFence);
		return false;
	}

	if (state.stage == PersistentVoxelBlasCompactionState::Stage::QueryPending)
	{
		if (!IsCommandFenceValueComplete(state.queryFence))
		{
			return false;
		}
		const uint32_t querySize = mFrameBuffer->mCore.GetQuerySize(*state.queryPool);
		const uint64_t readbackBytes = (uint64_t)querySize * state.sources.size();
		const uint8_t* mapped = static_cast<const uint8_t*>(
			mFrameBuffer->mCore.MapBuffer(*state.readbackBuffer.buffer, 0, readbackBytes));
		if (mapped == nullptr)
		{
			return fail("query-map");
		}
		std::vector<uint64_t> compactedSizes(state.sources.size());
		for (size_t index = 0; index < state.sources.size(); ++index)
		{
			std::memcpy(&compactedSizes[index], mapped + index * querySize, sizeof(uint64_t));
		}
		mFrameBuffer->mCore.UnmapBuffer(*state.readbackBuffer.buffer);

		state.destinations.resize(state.sources.size());
		std::vector<nri::BufferBarrierDesc> barriers;
		barriers.reserve(state.sources.size());
		for (size_t index = 0; index < state.sources.size(); ++index)
		{
			NRIAccelerationStructureResource& source = *state.sources[index];
			const uint64_t compactedSize = compactedSizes[index];
			if (compactedSize == 0 || compactedSize >= source.memorySize)
			{
				state.compactedBytes += source.memorySize;
				continue;
			}

			nri::BottomLevelGeometryDesc geometry = {};
			geometry.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
			geometry.type = nri::BottomLevelGeometryType::TRIANGLES;
			geometry.triangles.vertexBuffer = source.buildVertexBuffer;
			geometry.triangles.vertexOffset = (uint64_t)source.buildVertexOffset * sizeof(nri_scene::SceneVertex);
			geometry.triangles.vertexNum = source.buildVertexCount;
			geometry.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
			geometry.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
			geometry.triangles.indexBuffer = source.buildIndexBuffer;
			geometry.triangles.indexOffset = (uint64_t)source.buildIndexOffset * sizeof(uint32_t);
			geometry.triangles.indexNum = source.buildIndexCount;
			geometry.triangles.indexType = nri::IndexType::UINT32;

			nri::AccelerationStructureDesc desc = {};
			desc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
			desc.flags = source.buildFlags;
			desc.geometryOrInstanceNum = 1;
			desc.geometries = &geometry;
			desc.optimizedSize = compactedSize;
			NRIAccelerationStructureResource& destination = state.destinations[index];
			if (mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(
					*mFrameBuffer->mDevice,
					nri::MemoryLocation::DEVICE,
					0.0f,
					desc,
					destination.accelerationStructure) != nri::Result::SUCCESS)
			{
				return fail("compact-destination-create");
			}
			nri::MemoryDesc memoryDesc = {};
			mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(
				*destination.accelerationStructure,
				nri::MemoryLocation::DEVICE,
				memoryDesc);
			destination.memorySize = memoryDesc.size;
			destination.uncompactedMemorySize = source.memorySize;
			destination.compacted = true;
			destination.buildFlags = source.buildFlags;
			destination.buildType = source.buildType;
			destination.buildTypeValid = source.buildTypeValid;
			destination.buildVertexBuffer = source.buildVertexBuffer;
			destination.buildIndexBuffer = source.buildIndexBuffer;
			destination.buildVertexOffset = source.buildVertexOffset;
			destination.buildVertexCount = source.buildVertexCount;
			destination.buildIndexOffset = source.buildIndexOffset;
			destination.buildIndexCount = source.buildIndexCount;
			destination.buildPrimitiveCount = source.buildPrimitiveCount;
			state.compactedBytes += destination.memorySize;
		}

		uint32_t copiedResources = 0;
		for (size_t index = 0; index < state.sources.size(); ++index)
		{
			NRIAccelerationStructureResource& destination = state.destinations[index];
			if (destination.accelerationStructure == nullptr)
			{
				continue;
			}
			NRIAccelerationStructureResource& source = *state.sources[index];
			mFrameBuffer->mRayTracing.CmdCopyAccelerationStructure(
				*mFrameBuffer->mCommandBuffer,
				*destination.accelerationStructure,
				*source.accelerationStructure,
				nri::CopyMode::COMPACT);

			nri::BufferBarrierDesc barrier = {};
			barrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*destination.accelerationStructure);
			barrier.before = NRIResourceAccelerationStructureWriteAccess();
			barrier.after = NRIResourceAccelerationStructureReadAccess();
			barriers.push_back(barrier);
			copiedResources++;
		}
		if (barriers.empty())
		{
			DestroyBufferResource(state.readbackBuffer);
			mFrameBuffer->mCore.DestroyQueryPool(state.queryPool);
			state.queryPool = nullptr;
			state.stage = PersistentVoxelBlasCompactionState::Stage::Complete;
			Printf("PERF pt voxel blas compaction NRI: stage=complete build_serial=%llu resources=%u copied=0 original_bytes=%llu compacted_bytes=%llu saved_bytes=0\n",
				(unsigned long long)buildSerial,
				(uint32_t)state.sources.size(),
				(unsigned long long)state.originalBytes,
				(unsigned long long)state.compactedBytes);
			return true;
		}
		nri::BarrierDesc barrierDesc = {};
		barrierDesc.buffers = barriers.data();
		barrierDesc.bufferNum = (uint32_t)barriers.size();
		mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrierDesc);
		state.copyFence = GetRecordingCommandFenceValue();
		if (state.copyFence == 0)
		{
			return fail("copy-fence");
		}
		state.stage = PersistentVoxelBlasCompactionState::Stage::CopyPending;
		Printf("PERF pt voxel blas compaction NRI: stage=copy-submit build_serial=%llu resources=%u copied=%u original_bytes=%llu compacted_bytes=%llu fence=%llu\n",
			(unsigned long long)buildSerial,
			(uint32_t)state.sources.size(),
			copiedResources,
			(unsigned long long)state.originalBytes,
			(unsigned long long)state.compactedBytes,
			(unsigned long long)state.copyFence);
		return false;
	}

	if (state.stage == PersistentVoxelBlasCompactionState::Stage::CopyPending)
	{
		if (!IsCommandFenceValueComplete(state.copyFence))
		{
			return false;
		}
		uint32_t copiedResources = 0;
		for (size_t index = 0; index < state.sources.size(); ++index)
		{
			NRIAccelerationStructureResource& destination = state.destinations[index];
			if (destination.accelerationStructure == nullptr)
			{
				continue;
			}
			NRIAccelerationStructureResource& source = *state.sources[index];
			NRIAccelerationStructureResource retired = source;
			source = destination;
			destination = {};
			RetireResidentAccelerationStructure(retired);
			copiedResources++;
		}
		state.destinations.clear();
		DestroyBufferResource(state.readbackBuffer);
		mFrameBuffer->mCore.DestroyQueryPool(state.queryPool);
		state.queryPool = nullptr;
		state.stage = PersistentVoxelBlasCompactionState::Stage::Complete;
		Printf("PERF pt voxel blas compaction NRI: stage=complete build_serial=%llu resources=%u copied=%u original_bytes=%llu compacted_bytes=%llu saved_bytes=%llu\n",
			(unsigned long long)buildSerial,
			(uint32_t)state.sources.size(),
			copiedResources,
			(unsigned long long)state.originalBytes,
			(unsigned long long)state.compactedBytes,
			(unsigned long long)(state.originalBytes - std::min(state.originalBytes, state.compactedBytes)));
		return true;
	}
	return true;
}
