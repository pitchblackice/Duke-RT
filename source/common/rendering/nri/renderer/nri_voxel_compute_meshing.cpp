#include "nri_voxel_compute_meshing.h"

#include "nri_cvars.h"
#include "nri_renderer.h"
#include "nri_shader_contracts.h"
#include "nri_resources.h"
#include "../system/nri_renderdevice.h"
#include "common/models/model_kvx.h"
#include "printf.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
	struct PendingVoxelComputeJob
	{
		FVoxelModel* model = nullptr;
		FVoxelRawMeshStats stats = {};
		uint32_t cpuVertexCount = 0;
		uint32_t cpuIndexCount = 0;
		std::vector<NRIVoxelComputeSlabRecord> slabs;
	};

	struct PendingReadbackJob
	{
		uint32_t jobId = 0;
		uint32_t expectedFaces = 0;
		uint32_t expectedIndices = 0;
		uint32_t expectedVerticesNoDedupe = 0;
		uint32_t expectedVoxels = 0;
		uint32_t cpuVertices = 0;
		uint32_t cpuIndices = 0;
	};

	struct VoxelComputeState
	{
		std::vector<PendingVoxelComputeJob> queuedJobs;
		std::vector<PendingReadbackJob> pendingReadbackJobs;
		uint64_t pendingFrame = 0;
		uint32_t nextJobId = 1;
		NRIBufferResource jobUploadBuffer = {};
		NRIBufferResource slabUploadBuffer = {};
		NRIBufferResource jobBuffer = {};
		NRIBufferResource slabBuffer = {};
		NRIBufferResource resultBuffer = {};
		NRIBufferResource readbackBuffer = {};
		bool pendingReadbackValid = false;
	};

	VoxelComputeState gVoxelComputeState;

	uint32_t Popcount4(uint32_t value)
	{
		value &= 0xFu;
		uint32_t count = 0;
		for (uint32_t bit = 0; bit < 4; ++bit)
		{
			count += (value & (1u << bit)) != 0 ? 1u : 0u;
		}
		return count;
	}

	bool IsTraceEnabled()
	{
		return (int)nri_ptvoxelcomputetrace > 0;
	}

	bool CreateBuffer(
		const NRIResourceServices& services,
		NRIBufferResource& resource,
		uint64_t size,
		uint32_t stride,
		nri::BufferUsageBits usage,
		nri::MemoryLocation memoryLocation,
		nri::BufferView viewType,
		bool createView)
	{
		const NRIResourceContext& context = services.context;
		if (context.device == nullptr || context.core == nullptr || size == 0)
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

		if (createView)
		{
			nri::BufferViewDesc viewDesc = {};
			viewDesc.buffer = resource.buffer;
			viewDesc.type = viewType;
			viewDesc.offset = 0;
			viewDesc.size = nri::WHOLE_SIZE;
			viewDesc.structureStride = stride;
			if (context.core->CreateBufferView(viewDesc, resource.shaderView) != nri::Result::SUCCESS)
			{
				services.DestroyBufferResource(resource);
				return false;
			}
		}

		return true;
	}

	bool EnsureBuffer(
		const NRIResourceServices& services,
		NRIBufferResource& resource,
		uint64_t size,
		uint32_t stride,
		nri::BufferUsageBits usage,
		nri::MemoryLocation memoryLocation,
		nri::BufferView viewType,
		bool createView)
	{
		if (resource.buffer != nullptr && resource.size >= size && resource.stride == stride && resource.memoryLocation == memoryLocation)
		{
			return true;
		}
		return CreateBuffer(services, resource, size, stride, usage, memoryLocation, viewType, createView);
	}

	bool CopyToUploadBuffer(const NRIResourceContext& context, NRIBufferResource& upload, const void* data, uint64_t size)
	{
		void* mapped = context.core->MapBuffer(*upload.buffer, 0, size);
		if (mapped == nullptr)
		{
			return false;
		}
		std::memcpy(mapped, data, (size_t)size);
		context.core->UnmapBuffer(*upload.buffer);
		return true;
	}

	void ReadbackPreviousResults(NRIRenderer& renderer, const NRIResourceServices& services)
	{
		VoxelComputeState& state = gVoxelComputeState;
		if (!state.pendingReadbackValid || state.readbackBuffer.buffer == nullptr || state.pendingReadbackJobs.empty() || services.context.core == nullptr)
		{
			return;
		}

		services.WaitForCommands("voxel_compute_count_readback");
		const uint64_t byteSize = (uint64_t)state.pendingReadbackJobs.size() * sizeof(NRIVoxelComputeResult);
		const void* mapped = services.context.core->MapBuffer(*state.readbackBuffer.buffer, 0, byteSize);
		if (mapped == nullptr)
		{
			state.pendingReadbackValid = false;
			state.pendingReadbackJobs.clear();
			return;
		}

		const NRIVoxelComputeResult* results = static_cast<const NRIVoxelComputeResult*>(mapped);
		uint32_t okCount = 0;
		uint32_t mismatchCount = 0;
		for (size_t i = 0; i < state.pendingReadbackJobs.size(); ++i)
		{
			const PendingReadbackJob& job = state.pendingReadbackJobs[i];
			const NRIVoxelComputeResult& result = results[i];
			const bool ok = result.Status == 1u && result.MismatchMask == 0u;
			okCount += ok ? 1u : 0u;
			mismatchCount += ok ? 0u : 1u;
			if (IsTraceEnabled())
			{
				Printf(
					"PERF pt voxel compute count NRI: frame=%llu job=%u status=%u mismatch=%u faces=%u expected_faces=%u indices=%u expected_indices=%u vertices_nodedupe=%u expected_vertices_nodedupe=%u voxels=%u expected_voxels=%u cpu_vertices=%u cpu_indices=%u\n",
					(unsigned long long)state.pendingFrame,
					job.jobId,
					result.Status,
					result.MismatchMask,
					result.FaceCount,
					job.expectedFaces,
					result.IndexCount,
					job.expectedIndices,
					result.VertexCountNoDedupe,
					job.expectedVerticesNoDedupe,
					result.VoxelCount,
					job.expectedVoxels,
					job.cpuVertices,
					job.cpuIndices);
			}
		}
		services.context.core->UnmapBuffer(*state.readbackBuffer.buffer);

		if (IsTraceEnabled())
		{
			Printf(
				"PERF pt voxel compute summary NRI: frame=%llu jobs=%u ok=%u mismatch=%u\n",
				(unsigned long long)state.pendingFrame,
				(unsigned)state.pendingReadbackJobs.size(),
				okCount,
				mismatchCount);
		}

		state.pendingReadbackValid = false;
		state.pendingReadbackJobs.clear();
	}
}

bool ShouldTraceNRIVoxelComputeMeshing()
{
	return IsTraceEnabled() || (int)nri_ptvoxelcomputemode >= 1;
}

bool ShouldRunNRIVoxelComputeMeshing()
{
	return (bool)nri_ptvoxelcompute && (int)nri_ptvoxelcomputemode >= 2;
}

void QueueNRIVoxelComputeCountJob(
	FVoxelModel* model,
	const FVoxelRawMeshStats& stats,
	const TArray<FVoxelRawSlabRecord>* slabs,
	const FVoxelMeshData& cpuMesh)
{
	if (!ShouldRunNRIVoxelComputeMeshing() || model == nullptr || slabs == nullptr || stats.slabCount == 0)
	{
		return;
	}

	const uint32_t maxJobs = std::max(0, (int)nri_ptvoxelcomputemaxjobs);
	if (maxJobs == 0 || gVoxelComputeState.queuedJobs.size() >= maxJobs)
	{
		return;
	}

	PendingVoxelComputeJob job = {};
	job.model = model;
	job.stats = stats;
	job.cpuVertexCount = (uint32_t)cpuMesh.vertices.Size();
	job.cpuIndexCount = (uint32_t)cpuMesh.indices.Size();
	job.slabs.reserve((size_t)slabs->Size());
	for (unsigned int i = 0; i < slabs->Size(); ++i)
	{
		const FVoxelRawSlabRecord& slab = (*slabs)[i];
		NRIVoxelComputeSlabRecord record = {};
		record.CullMask = slab.cullMask;
		record.ZLength = slab.zLength;
		record.ColorRunCount = slab.colorRunCount;
		job.slabs.push_back(record);
	}
	gVoxelComputeState.queuedJobs.push_back(std::move(job));
}

void DispatchNRIVoxelComputeMeshingDiagnostics(NRIRenderer& renderer, uint64_t frameNumber)
{
	if (!ShouldRunNRIVoxelComputeMeshing())
	{
		return;
	}

	NRIResourceServices services = renderer.BuildResourceServices();
	const NRIResourceContext& context = services.context;
	if (context.device == nullptr || context.core == nullptr || context.commandBuffer == nullptr ||
		renderer.mVoxelComputePipelineLayout == nullptr || renderer.mVoxelComputeInputSet == nullptr || renderer.mVoxelComputeOutputSet == nullptr)
	{
		return;
	}

	ReadbackPreviousResults(renderer, services);

	VoxelComputeState& state = gVoxelComputeState;
	if (state.queuedJobs.empty())
	{
		return;
	}

	std::vector<NRIVoxelComputeJob> gpuJobs;
	std::vector<NRIVoxelComputeSlabRecord> gpuSlabs;
	std::vector<PendingReadbackJob> pendingJobs;
	gpuJobs.reserve(state.queuedJobs.size());
	uint32_t slabOffset = 0;
	for (const PendingVoxelComputeJob& queued : state.queuedJobs)
	{
		NRIVoxelComputeJob gpuJob = {};
		gpuJob.SlabOffset = slabOffset;
		gpuJob.SlabCount = (uint32_t)queued.slabs.size();
		gpuJob.ExpectedFaces = queued.stats.coalescedFaceCount;
		gpuJob.ExpectedIndices = queued.stats.indexCount;
		gpuJob.ExpectedVerticesNoDedupe = queued.stats.noDedupeVertexCount;
		gpuJob.ExpectedVoxels = queued.stats.voxelCount;
		gpuJob.JobId = state.nextJobId++;
		gpuJobs.push_back(gpuJob);

		PendingReadbackJob pending = {};
		pending.jobId = gpuJob.JobId;
		pending.expectedFaces = gpuJob.ExpectedFaces;
		pending.expectedIndices = gpuJob.ExpectedIndices;
		pending.expectedVerticesNoDedupe = gpuJob.ExpectedVerticesNoDedupe;
		pending.expectedVoxels = gpuJob.ExpectedVoxels;
		pending.cpuVertices = queued.cpuVertexCount;
		pending.cpuIndices = queued.cpuIndexCount;
		pendingJobs.push_back(pending);

		gpuSlabs.insert(gpuSlabs.end(), queued.slabs.begin(), queued.slabs.end());
		slabOffset += gpuJob.SlabCount;
	}
	state.queuedJobs.clear();

	if (gpuJobs.empty() || gpuSlabs.empty())
	{
		return;
	}

	const uint64_t jobBytes = (uint64_t)gpuJobs.size() * sizeof(NRIVoxelComputeJob);
	const uint64_t slabBytes = (uint64_t)gpuSlabs.size() * sizeof(NRIVoxelComputeSlabRecord);
	const uint64_t resultBytes = (uint64_t)gpuJobs.size() * sizeof(NRIVoxelComputeResult);
	if (!EnsureBuffer(services, state.jobUploadBuffer, jobBytes, sizeof(NRIVoxelComputeJob), nri::BufferUsageBits::NONE, nri::MemoryLocation::DEVICE_UPLOAD, nri::BufferView::STRUCTURED_BUFFER, false) ||
		!EnsureBuffer(services, state.slabUploadBuffer, slabBytes, sizeof(NRIVoxelComputeSlabRecord), nri::BufferUsageBits::NONE, nri::MemoryLocation::DEVICE_UPLOAD, nri::BufferView::STRUCTURED_BUFFER, false) ||
		!EnsureBuffer(services, state.jobBuffer, jobBytes, sizeof(NRIVoxelComputeJob), nri::BufferUsageBits::SHADER_RESOURCE, nri::MemoryLocation::DEVICE, nri::BufferView::STRUCTURED_BUFFER, true) ||
		!EnsureBuffer(services, state.slabBuffer, slabBytes, sizeof(NRIVoxelComputeSlabRecord), nri::BufferUsageBits::SHADER_RESOURCE, nri::MemoryLocation::DEVICE, nri::BufferView::STRUCTURED_BUFFER, true) ||
		!EnsureBuffer(services, state.resultBuffer, resultBytes, sizeof(NRIVoxelComputeResult), nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::MemoryLocation::DEVICE, nri::BufferView::STORAGE_STRUCTURED_BUFFER, true) ||
		!EnsureBuffer(services, state.readbackBuffer, resultBytes, sizeof(NRIVoxelComputeResult), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, nri::BufferView::STRUCTURED_BUFFER, false))
	{
		return;
	}

	if (!CopyToUploadBuffer(context, state.jobUploadBuffer, gpuJobs.data(), jobBytes) ||
		!CopyToUploadBuffer(context, state.slabUploadBuffer, gpuSlabs.data(), slabBytes))
	{
		return;
	}

	nri::BufferBarrierDesc uploadBarriers[4] = {};
	uploadBarriers[0].buffer = state.jobUploadBuffer.buffer;
	uploadBarriers[0].after = NRIResourceCopySourceAccess();
	uploadBarriers[1].buffer = state.slabUploadBuffer.buffer;
	uploadBarriers[1].after = NRIResourceCopySourceAccess();
	uploadBarriers[2].buffer = state.jobBuffer.buffer;
	uploadBarriers[2].after = NRIResourceCopyDestinationAccess();
	uploadBarriers[3].buffer = state.slabBuffer.buffer;
	uploadBarriers[3].after = NRIResourceCopyDestinationAccess();
	nri::BarrierDesc uploadBarrier = {};
	uploadBarrier.buffers = uploadBarriers;
	uploadBarrier.bufferNum = 4;
	context.core->CmdBarrier(*context.commandBuffer, uploadBarrier);
	context.core->CmdCopyBuffer(*context.commandBuffer, *state.jobBuffer.buffer, 0, *state.jobUploadBuffer.buffer, 0, jobBytes);
	context.core->CmdCopyBuffer(*context.commandBuffer, *state.slabBuffer.buffer, 0, *state.slabUploadBuffer.buffer, 0, slabBytes);

	nri::BufferBarrierDesc computeBarriers[3] = {};
	computeBarriers[0].buffer = state.jobBuffer.buffer;
	computeBarriers[0].before = NRIResourceCopyDestinationAccess();
	computeBarriers[0].after = NRIResourceComputeShaderResourceAccess();
	computeBarriers[1].buffer = state.slabBuffer.buffer;
	computeBarriers[1].before = NRIResourceCopyDestinationAccess();
	computeBarriers[1].after = NRIResourceComputeShaderResourceAccess();
	computeBarriers[2].buffer = state.resultBuffer.buffer;
	computeBarriers[2].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	nri::BarrierDesc computeBarrier = {};
	computeBarrier.buffers = computeBarriers;
	computeBarrier.bufferNum = 3;
	context.core->CmdBarrier(*context.commandBuffer, computeBarrier);

	const nri::Descriptor* inputDescriptors[2] = { state.jobBuffer.shaderView, state.slabBuffer.shaderView };
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = renderer.mVoxelComputeInputSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputDescriptors;
	inputUpdate.descriptorNum = 2;
	context.core->UpdateDescriptorRanges(&inputUpdate, 1);

	const nri::Descriptor* outputDescriptors[1] = { state.resultBuffer.shaderView };
	nri::UpdateDescriptorRangeDesc outputUpdate = {};
	outputUpdate.descriptorSet = renderer.mVoxelComputeOutputSet;
	outputUpdate.rangeIndex = 0;
	outputUpdate.descriptors = outputDescriptors;
	outputUpdate.descriptorNum = 1;
	context.core->UpdateDescriptorRanges(&outputUpdate, 1);

	NRIVoxelComputeConstants constants = {};
	constants.JobCount = (uint32_t)gpuJobs.size();
	constants.SlabRecordCount = (uint32_t)gpuSlabs.size();
	context.core->CmdSetPipelineLayout(*context.commandBuffer, nri::BindPoint::COMPUTE, *renderer.mVoxelComputePipelineLayout);
	context.core->CmdSetRootConstants(*context.commandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	context.core->CmdSetDescriptorSet(*context.commandBuffer, { 0, renderer.mVoxelComputeInputSet, nri::BindPoint::COMPUTE });
	context.core->CmdSetDescriptorSet(*context.commandBuffer, { 1, renderer.mVoxelComputeOutputSet, nri::BindPoint::COMPUTE });
	context.core->CmdSetPipeline(*context.commandBuffer, *renderer.GetPipeline(NRIRenderer::PipelineSlot::VoxelComputeCount));
	context.core->CmdDispatch(*context.commandBuffer, { (uint32_t)gpuJobs.size(), 1, 1 });

	nri::BufferBarrierDesc readbackBarrier = {};
	readbackBarrier.buffer = state.resultBuffer.buffer;
	readbackBarrier.before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	readbackBarrier.after = NRIResourceCopySourceAccess();
	nri::BarrierDesc readbackBarrierDesc = {};
	readbackBarrierDesc.buffers = &readbackBarrier;
	readbackBarrierDesc.bufferNum = 1;
	context.core->CmdBarrier(*context.commandBuffer, readbackBarrierDesc);
	context.core->CmdCopyBuffer(*context.commandBuffer, *state.readbackBuffer.buffer, 0, *state.resultBuffer.buffer, 0, resultBytes);

	state.pendingFrame = frameNumber;
	state.pendingReadbackValid = true;
	state.pendingReadbackJobs = std::move(pendingJobs);
	if (IsTraceEnabled())
	{
		Printf(
			"PERF pt voxel compute dispatch NRI: frame=%llu jobs=%u slab_records=%u job_bytes=%llu slab_bytes=%llu result_bytes=%llu\n",
			(unsigned long long)frameNumber,
			(unsigned)gpuJobs.size(),
			(unsigned)gpuSlabs.size(),
			(unsigned long long)jobBytes,
			(unsigned long long)slabBytes,
			(unsigned long long)resultBytes);
	}
}

void DestroyNRIVoxelComputeMeshingDiagnostics(NRIRenderer& renderer)
{
	NRIResourceServices services = renderer.BuildResourceServices();
	VoxelComputeState& state = gVoxelComputeState;
	services.DestroyBufferResource(state.jobUploadBuffer);
	services.DestroyBufferResource(state.slabUploadBuffer);
	services.DestroyBufferResource(state.jobBuffer);
	services.DestroyBufferResource(state.slabBuffer);
	services.DestroyBufferResource(state.resultBuffer);
	services.DestroyBufferResource(state.readbackBuffer);
	state.queuedJobs.clear();
	state.pendingReadbackJobs.clear();
	state.pendingFrame = 0;
	state.pendingReadbackValid = false;
}
