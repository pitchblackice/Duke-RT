#include "nri_voxel_compute_meshing.h"

#include "nri_cvars.h"
#include "nri_renderer.h"
#include "nri_shader_contracts.h"
#include "nri_resources.h"
#include "../scene/nri_geometry_bridge.h"
#include "../system/nri_renderdevice.h"
#include "common/models/model_kvx.h"
#include "printf.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
	enum class VoxelComputeAdmissionState : uint32_t
	{
		Queued = 0,
		Counting,
		CountReady,
		Emitting,
		ReadyForBlas,
		BlasBuilding,
		BlasReady,
		Failed
	};

	struct PendingVoxelComputeJob
	{
		FVoxelModel* model = nullptr;
		FVoxelRawMeshStats stats = {};
		uint32_t cpuVertexCount = 0;
		uint32_t cpuIndexCount = 0;
		uint32_t jobId = 0;
		VoxelComputeAdmissionState admissionState = VoxelComputeAdmissionState::Queued;
		std::vector<NRIVoxelComputeSlabRecord> slabs;
		std::vector<NRIVoxelComputeFaceRecord> faces;
	};

	struct PendingReadbackJob
	{
		uint32_t jobId = 0;
		uint32_t expectedFaces = 0;
		uint32_t expectedIndices = 0;
		uint32_t expectedVerticesNoDedupe = 0;
		uint32_t expectedVoxels = 0;
		uint32_t expectedPrimitives = 0;
		uint32_t cpuVertices = 0;
		uint32_t cpuIndices = 0;
		uint32_t vertexOffset = 0;
		uint32_t indexOffset = 0;
		uint32_t primitiveOffset = 0;
		VoxelComputeAdmissionState admissionState = VoxelComputeAdmissionState::Counting;
	};

	struct VoxelComputeState
	{
		std::vector<PendingVoxelComputeJob> queuedJobs;
		std::vector<PendingReadbackJob> pendingReadbackJobs;
		uint64_t pendingFrame = 0;
		uint32_t nextJobId = 1;
		bool pendingReadbackValid = false;
		bool pendingEmit = false;
		bool pendingBlas = false;
		uint32_t pendingVertexCount = 0;
		uint32_t pendingIndexCount = 0;
		uint32_t pendingPrimitiveCount = 0;
		uint32_t diagnosticBlasBuildsSubmitted = 0;

		NRIBufferResource jobUploadBuffer = {};
		NRIBufferResource slabUploadBuffer = {};
		NRIBufferResource faceUploadBuffer = {};
		NRIBufferResource jobBuffer = {};
		NRIBufferResource slabBuffer = {};
		NRIBufferResource faceBuffer = {};
		NRIBufferResource resultBuffer = {};
		NRIBufferResource vertexBuffer = {};
		NRIBufferResource indexBuffer = {};
		NRIBufferResource primitiveBuffer = {};
		NRIBufferResource readbackBuffer = {};
		NRIBufferResource vertexReadbackBuffer = {};
		NRIBufferResource indexReadbackBuffer = {};
		NRIBufferResource primitiveReadbackBuffer = {};
		NRIAccelerationStructureResource diagnosticBlas = {};
	};

	VoxelComputeState gVoxelComputeState;

	bool IsTraceEnabled()
	{
		return (int)nri_ptvoxelcomputetrace > 0;
	}

	bool IsEmitEnabled()
	{
		return (bool)nri_ptvoxelcompute && (int)nri_ptvoxelcomputemode >= 3;
	}

	bool IsAdmissionTraceEnabled()
	{
		return (bool)nri_ptvoxelcompute && (int)nri_ptvoxelcomputemode >= 4;
	}

	bool IsBlasEnabled()
	{
		return (bool)nri_ptvoxelcompute && (int)nri_ptvoxelcomputemode >= 5;
	}

	const char* AdmissionStateName(VoxelComputeAdmissionState state)
	{
		switch (state)
		{
		case VoxelComputeAdmissionState::Queued: return "queued";
		case VoxelComputeAdmissionState::Counting: return "counting";
		case VoxelComputeAdmissionState::CountReady: return "count_ready";
		case VoxelComputeAdmissionState::Emitting: return "emitting";
		case VoxelComputeAdmissionState::ReadyForBlas: return "ready_for_blas";
		case VoxelComputeAdmissionState::BlasBuilding: return "blas_building";
		case VoxelComputeAdmissionState::BlasReady: return "blas_ready";
		case VoxelComputeAdmissionState::Failed: return "failed";
		default: return "unknown";
		}
	}

	uint32_t HashBytes(const void* data, uint64_t size)
	{
		const uint8_t* bytes = static_cast<const uint8_t*>(data);
		uint32_t hash = 2166136261u;
		for (uint64_t i = 0; i < size; ++i)
		{
			hash ^= bytes[i];
			hash *= 16777619u;
		}
		return hash;
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

	uint32_t HashReadbackBuffer(const NRIResourceContext& context, NRIBufferResource& buffer, uint64_t byteSize)
	{
		if (buffer.buffer == nullptr || byteSize == 0)
		{
			return 0;
		}

		const void* mapped = context.core->MapBuffer(*buffer.buffer, 0, byteSize);
		if (mapped == nullptr)
		{
			return 0;
		}
		const uint32_t hash = HashBytes(mapped, byteSize);
		context.core->UnmapBuffer(*buffer.buffer);
		return hash;
	}

	void TraceAdmission(uint64_t frameNumber, const PendingReadbackJob& job, VoxelComputeAdmissionState state, const char* reason)
	{
		if (!IsAdmissionTraceEnabled() && !IsTraceEnabled())
		{
			return;
		}

		Printf(
			"PERF pt voxel compute admission NRI: frame=%llu job=%u state=%s reason=%s faces=%u vertices=%u indices=%u primitives=%u\n",
			(unsigned long long)frameNumber,
			job.jobId,
			AdmissionStateName(state),
			reason != nullptr ? reason : "unknown",
			job.expectedFaces,
			job.expectedVerticesNoDedupe,
			job.expectedIndices,
			job.expectedPrimitives);
	}

	void ReadbackPreviousResults(NRIRenderer& renderer, const NRIResourceServices& services)
	{
		VoxelComputeState& state = gVoxelComputeState;
		if (!state.pendingReadbackValid || state.readbackBuffer.buffer == nullptr || state.pendingReadbackJobs.empty() || services.context.core == nullptr)
		{
			return;
		}

		services.WaitForCommands(state.pendingEmit ? "voxel_compute_emit_readback" : "voxel_compute_count_readback");
		const uint64_t resultByteSize = (uint64_t)state.pendingReadbackJobs.size() * sizeof(NRIVoxelComputeResult);
		const void* mapped = services.context.core->MapBuffer(*state.readbackBuffer.buffer, 0, resultByteSize);
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
			PendingReadbackJob& job = state.pendingReadbackJobs[i];
			const NRIVoxelComputeResult& result = results[i];
			const bool ok = result.MismatchMask == 0u &&
				((state.pendingEmit && result.Status == NRI_VOXEL_COMPUTE_STATUS_EMIT_OK) ||
				(!state.pendingEmit && result.Status == NRI_VOXEL_COMPUTE_STATUS_COUNT_OK));
			okCount += ok ? 1u : 0u;
			mismatchCount += ok ? 0u : 1u;
			job.admissionState = ok ? (state.pendingEmit ? VoxelComputeAdmissionState::ReadyForBlas : VoxelComputeAdmissionState::CountReady) : VoxelComputeAdmissionState::Failed;
			TraceAdmission(state.pendingFrame, job, job.admissionState, state.pendingEmit ? "readback_emit" : "readback_count");
			if (IsTraceEnabled())
			{
				Printf(
					"PERF pt voxel compute %s NRI: frame=%llu job=%u status=%u mismatch=%u faces=%u expected_faces=%u indices=%u expected_indices=%u vertices_nodedupe=%u expected_vertices_nodedupe=%u primitives=%u expected_primitives=%u voxels=%u expected_voxels=%u cpu_vertices=%u cpu_indices=%u vertex_hash=%u index_hash=%u primitive_hash=%u\n",
					state.pendingEmit ? "emit" : "count",
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
					result.PrimitiveCount,
					job.expectedPrimitives,
					result.VoxelCount,
					job.expectedVoxels,
					job.cpuVertices,
					job.cpuIndices,
					result.VertexHash,
					result.IndexHash,
					result.PrimitiveHash);
			}
		}
		services.context.core->UnmapBuffer(*state.readbackBuffer.buffer);

		if (state.pendingEmit && IsTraceEnabled())
		{
			const uint64_t vertexBytes = (uint64_t)state.pendingVertexCount * sizeof(NRIVoxelComputeSceneVertex);
			const uint64_t indexBytes = (uint64_t)state.pendingIndexCount * sizeof(uint32_t);
			const uint64_t primitiveBytes = (uint64_t)state.pendingPrimitiveCount * sizeof(NRIVoxelComputePrimitiveData);
			Printf(
				"PERF pt voxel compute emit readback NRI: frame=%llu vertices=%u indices=%u primitives=%u vertex_bytes=%llu index_bytes=%llu primitive_bytes=%llu vertex_readback_hash=%u index_readback_hash=%u primitive_readback_hash=%u\n",
				(unsigned long long)state.pendingFrame,
				state.pendingVertexCount,
				state.pendingIndexCount,
				state.pendingPrimitiveCount,
				(unsigned long long)vertexBytes,
				(unsigned long long)indexBytes,
				(unsigned long long)primitiveBytes,
				HashReadbackBuffer(services.context, state.vertexReadbackBuffer, vertexBytes),
				HashReadbackBuffer(services.context, state.indexReadbackBuffer, indexBytes),
				HashReadbackBuffer(services.context, state.primitiveReadbackBuffer, primitiveBytes));
		}

		if (IsTraceEnabled())
		{
			Printf(
				"PERF pt voxel compute summary NRI: frame=%llu mode=%s jobs=%u ok=%u mismatch=%u\n",
				(unsigned long long)state.pendingFrame,
				state.pendingEmit ? "emit" : "count",
				(unsigned)state.pendingReadbackJobs.size(),
				okCount,
				mismatchCount);
		}

		state.pendingReadbackValid = false;
		state.pendingEmit = false;
		state.pendingBlas = false;
		state.pendingReadbackJobs.clear();
		state.pendingVertexCount = 0;
		state.pendingIndexCount = 0;
		state.pendingPrimitiveCount = 0;
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

bool ShouldEmitNRIVoxelComputeMeshing()
{
	return IsEmitEnabled();
}

void QueueNRIVoxelComputeCountJob(
	FVoxelModel* model,
	const FVoxelRawMeshStats& stats,
	const TArray<FVoxelRawSlabRecord>* slabs,
	const TArray<FVoxelRawFaceRecord>* faces,
	const FVoxelMeshData& cpuMesh)
{
	if (!ShouldRunNRIVoxelComputeMeshing() || model == nullptr || slabs == nullptr || stats.slabCount == 0)
	{
		return;
	}
	if (IsEmitEnabled() && (faces == nullptr || faces->Size() != stats.coalescedFaceCount))
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
	job.jobId = gVoxelComputeState.nextJobId++;
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
	if (faces != nullptr)
	{
		job.faces.reserve((size_t)faces->Size());
		for (unsigned int i = 0; i < faces->Size(); ++i)
		{
			const FVoxelRawFaceRecord& face = (*faces)[i];
			NRIVoxelComputeFaceRecord record = {};
			for (uint32_t v = 0; v < 4; ++v)
			{
				record.X[v] = face.x[v];
				record.Y[v] = face.y[v];
				record.Z[v] = face.z[v];
			}
			record.Color = face.color;
			record.MaterialIndex = face.materialIndex;
			job.faces.push_back(record);
		}
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

	const bool emit = IsEmitEnabled();
	const bool buildBlas = emit && IsBlasEnabled();
	std::vector<NRIVoxelComputeJob> gpuJobs;
	std::vector<NRIVoxelComputeSlabRecord> gpuSlabs;
	std::vector<NRIVoxelComputeFaceRecord> gpuFaces;
	std::vector<PendingReadbackJob> pendingJobs;
	gpuJobs.reserve(state.queuedJobs.size());
	uint32_t slabOffset = 0;
	uint32_t faceOffset = 0;
	uint32_t vertexOffset = 0;
	uint32_t indexOffset = 0;
	uint32_t primitiveOffset = 0;
	for (PendingVoxelComputeJob& queued : state.queuedJobs)
	{
		if (emit && queued.faces.size() != queued.stats.coalescedFaceCount)
		{
			continue;
		}

		NRIVoxelComputeJob gpuJob = {};
		gpuJob.SlabOffset = slabOffset;
		gpuJob.SlabCount = (uint32_t)queued.slabs.size();
		gpuJob.FaceOffset = faceOffset;
		gpuJob.ExpectedFaces = queued.stats.coalescedFaceCount;
		gpuJob.ExpectedIndices = queued.stats.indexCount;
		gpuJob.ExpectedVerticesNoDedupe = queued.stats.noDedupeVertexCount;
		gpuJob.ExpectedVoxels = queued.stats.voxelCount;
		gpuJob.JobId = queued.jobId;
		gpuJob.VertexOffset = vertexOffset;
		gpuJob.IndexOffset = indexOffset;
		gpuJob.PrimitiveOffset = primitiveOffset;
		gpuJob.PivotX = queued.stats.pivotX;
		gpuJob.PivotY = queued.stats.pivotY;
		gpuJob.PivotZ = queued.stats.pivotZ;
		gpuJobs.push_back(gpuJob);

		PendingReadbackJob pending = {};
		pending.jobId = gpuJob.JobId;
		pending.expectedFaces = gpuJob.ExpectedFaces;
		pending.expectedIndices = gpuJob.ExpectedIndices;
		pending.expectedVerticesNoDedupe = gpuJob.ExpectedVerticesNoDedupe;
		pending.expectedVoxels = gpuJob.ExpectedVoxels;
		pending.expectedPrimitives = gpuJob.ExpectedFaces * 2u;
		pending.cpuVertices = queued.cpuVertexCount;
		pending.cpuIndices = queued.cpuIndexCount;
		pending.vertexOffset = gpuJob.VertexOffset;
		pending.indexOffset = gpuJob.IndexOffset;
		pending.primitiveOffset = gpuJob.PrimitiveOffset;
		pending.admissionState = emit ? VoxelComputeAdmissionState::Emitting : VoxelComputeAdmissionState::Counting;
		pendingJobs.push_back(pending);
		TraceAdmission(frameNumber, pending, pending.admissionState, "dispatch");

		gpuSlabs.insert(gpuSlabs.end(), queued.slabs.begin(), queued.slabs.end());
		gpuFaces.insert(gpuFaces.end(), queued.faces.begin(), queued.faces.end());
		slabOffset += gpuJob.SlabCount;
		faceOffset += gpuJob.ExpectedFaces;
		vertexOffset += gpuJob.ExpectedVerticesNoDedupe;
		indexOffset += gpuJob.ExpectedIndices;
		primitiveOffset += gpuJob.ExpectedFaces * 2u;
	}
	state.queuedJobs.clear();

	if (gpuJobs.empty() || gpuSlabs.empty())
	{
		return;
	}

	const uint64_t jobBytes = (uint64_t)gpuJobs.size() * sizeof(NRIVoxelComputeJob);
	const uint64_t slabBytes = (uint64_t)gpuSlabs.size() * sizeof(NRIVoxelComputeSlabRecord);
	const uint64_t faceBytes = (uint64_t)gpuFaces.size() * sizeof(NRIVoxelComputeFaceRecord);
	const uint64_t resultBytes = (uint64_t)gpuJobs.size() * sizeof(NRIVoxelComputeResult);
	const uint64_t vertexBytes = (uint64_t)vertexOffset * sizeof(NRIVoxelComputeSceneVertex);
	const uint64_t indexBytes = (uint64_t)indexOffset * sizeof(uint32_t);
	const uint64_t primitiveBytes = (uint64_t)primitiveOffset * sizeof(NRIVoxelComputePrimitiveData);
	if (!EnsureBuffer(services, state.jobUploadBuffer, jobBytes, sizeof(NRIVoxelComputeJob), nri::BufferUsageBits::NONE, nri::MemoryLocation::DEVICE_UPLOAD, nri::BufferView::STRUCTURED_BUFFER, false) ||
		!EnsureBuffer(services, state.slabUploadBuffer, slabBytes, sizeof(NRIVoxelComputeSlabRecord), nri::BufferUsageBits::NONE, nri::MemoryLocation::DEVICE_UPLOAD, nri::BufferView::STRUCTURED_BUFFER, false) ||
		!EnsureBuffer(services, state.jobBuffer, jobBytes, sizeof(NRIVoxelComputeJob), nri::BufferUsageBits::SHADER_RESOURCE, nri::MemoryLocation::DEVICE, nri::BufferView::STRUCTURED_BUFFER, true) ||
		!EnsureBuffer(services, state.slabBuffer, slabBytes, sizeof(NRIVoxelComputeSlabRecord), nri::BufferUsageBits::SHADER_RESOURCE, nri::MemoryLocation::DEVICE, nri::BufferView::STRUCTURED_BUFFER, true) ||
		!EnsureBuffer(services, state.resultBuffer, resultBytes, sizeof(NRIVoxelComputeResult), nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::MemoryLocation::DEVICE, nri::BufferView::STORAGE_STRUCTURED_BUFFER, true) ||
		!EnsureBuffer(services, state.readbackBuffer, resultBytes, sizeof(NRIVoxelComputeResult), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, nri::BufferView::STRUCTURED_BUFFER, false))
	{
		return;
	}
	if (emit)
	{
		if (gpuFaces.empty() ||
			!EnsureBuffer(services, state.faceUploadBuffer, faceBytes, sizeof(NRIVoxelComputeFaceRecord), nri::BufferUsageBits::NONE, nri::MemoryLocation::DEVICE_UPLOAD, nri::BufferView::STRUCTURED_BUFFER, false) ||
			!EnsureBuffer(services, state.faceBuffer, faceBytes, sizeof(NRIVoxelComputeFaceRecord), nri::BufferUsageBits::SHADER_RESOURCE, nri::MemoryLocation::DEVICE, nri::BufferView::STRUCTURED_BUFFER, true) ||
			!EnsureBuffer(services, state.vertexBuffer, vertexBytes, sizeof(NRIVoxelComputeSceneVertex), NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), nri::MemoryLocation::DEVICE, nri::BufferView::STORAGE_STRUCTURED_BUFFER, true) ||
			!EnsureBuffer(services, state.indexBuffer, indexBytes, sizeof(uint32_t), NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), nri::MemoryLocation::DEVICE, nri::BufferView::STORAGE_STRUCTURED_BUFFER, true) ||
			!EnsureBuffer(services, state.primitiveBuffer, primitiveBytes, sizeof(NRIVoxelComputePrimitiveData), nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::MemoryLocation::DEVICE, nri::BufferView::STORAGE_STRUCTURED_BUFFER, true) ||
			!EnsureBuffer(services, state.vertexReadbackBuffer, vertexBytes, sizeof(NRIVoxelComputeSceneVertex), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, nri::BufferView::STRUCTURED_BUFFER, false) ||
			!EnsureBuffer(services, state.indexReadbackBuffer, indexBytes, sizeof(uint32_t), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, nri::BufferView::STRUCTURED_BUFFER, false) ||
			!EnsureBuffer(services, state.primitiveReadbackBuffer, primitiveBytes, sizeof(NRIVoxelComputePrimitiveData), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, nri::BufferView::STRUCTURED_BUFFER, false))
		{
			return;
		}
	}

	if (!CopyToUploadBuffer(context, state.jobUploadBuffer, gpuJobs.data(), jobBytes) ||
		!CopyToUploadBuffer(context, state.slabUploadBuffer, gpuSlabs.data(), slabBytes) ||
		(emit && !CopyToUploadBuffer(context, state.faceUploadBuffer, gpuFaces.data(), faceBytes)))
	{
		return;
	}

	std::vector<nri::BufferBarrierDesc> uploadBarriers;
	uploadBarriers.resize(emit ? 6 : 4);
	uploadBarriers[0].buffer = state.jobUploadBuffer.buffer;
	uploadBarriers[0].after = NRIResourceCopySourceAccess();
	uploadBarriers[1].buffer = state.slabUploadBuffer.buffer;
	uploadBarriers[1].after = NRIResourceCopySourceAccess();
	uploadBarriers[2].buffer = state.jobBuffer.buffer;
	uploadBarriers[2].after = NRIResourceCopyDestinationAccess();
	uploadBarriers[3].buffer = state.slabBuffer.buffer;
	uploadBarriers[3].after = NRIResourceCopyDestinationAccess();
	if (emit)
	{
		uploadBarriers[4].buffer = state.faceUploadBuffer.buffer;
		uploadBarriers[4].after = NRIResourceCopySourceAccess();
		uploadBarriers[5].buffer = state.faceBuffer.buffer;
		uploadBarriers[5].after = NRIResourceCopyDestinationAccess();
	}
	nri::BarrierDesc uploadBarrier = {};
	uploadBarrier.buffers = uploadBarriers.data();
	uploadBarrier.bufferNum = (uint32_t)uploadBarriers.size();
	context.core->CmdBarrier(*context.commandBuffer, uploadBarrier);
	context.core->CmdCopyBuffer(*context.commandBuffer, *state.jobBuffer.buffer, 0, *state.jobUploadBuffer.buffer, 0, jobBytes);
	context.core->CmdCopyBuffer(*context.commandBuffer, *state.slabBuffer.buffer, 0, *state.slabUploadBuffer.buffer, 0, slabBytes);
	if (emit)
	{
		context.core->CmdCopyBuffer(*context.commandBuffer, *state.faceBuffer.buffer, 0, *state.faceUploadBuffer.buffer, 0, faceBytes);
	}

	std::vector<nri::BufferBarrierDesc> computeBarriers;
	computeBarriers.resize(emit ? 7 : 3);
	computeBarriers[0].buffer = state.jobBuffer.buffer;
	computeBarriers[0].before = NRIResourceCopyDestinationAccess();
	computeBarriers[0].after = NRIResourceComputeShaderResourceAccess();
	computeBarriers[1].buffer = state.slabBuffer.buffer;
	computeBarriers[1].before = NRIResourceCopyDestinationAccess();
	computeBarriers[1].after = NRIResourceComputeShaderResourceAccess();
	computeBarriers[2].buffer = state.resultBuffer.buffer;
	computeBarriers[2].before = NRIResourceCopySourceAccess();
	computeBarriers[2].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	if (emit)
	{
		computeBarriers[3].buffer = state.faceBuffer.buffer;
		computeBarriers[3].before = NRIResourceCopyDestinationAccess();
		computeBarriers[3].after = NRIResourceComputeShaderResourceAccess();
		computeBarriers[4].buffer = state.vertexBuffer.buffer;
		computeBarriers[4].before = buildBlas ? NRIResourceAccelerationStructureBuildInputAccess() : NRIResourceCopySourceAccess();
		computeBarriers[4].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		computeBarriers[5].buffer = state.indexBuffer.buffer;
		computeBarriers[5].before = buildBlas ? NRIResourceAccelerationStructureBuildInputAccess() : NRIResourceCopySourceAccess();
		computeBarriers[5].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		computeBarriers[6].buffer = state.primitiveBuffer.buffer;
		computeBarriers[6].before = NRIResourceCopySourceAccess();
		computeBarriers[6].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	}
	nri::BarrierDesc computeBarrier = {};
	computeBarrier.buffers = computeBarriers.data();
	computeBarrier.bufferNum = (uint32_t)computeBarriers.size();
	context.core->CmdBarrier(*context.commandBuffer, computeBarrier);

	const nri::Descriptor* inputDescriptors[2] = { state.jobBuffer.shaderView, state.slabBuffer.shaderView };
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = renderer.mVoxelComputeInputSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputDescriptors;
	inputUpdate.descriptorNum = 2;
	context.core->UpdateDescriptorRanges(&inputUpdate, 1);
	if (emit)
	{
		const nri::Descriptor* faceDescriptor[1] = { state.faceBuffer.shaderView };
		nri::UpdateDescriptorRangeDesc faceUpdate = {};
		faceUpdate.descriptorSet = renderer.mVoxelComputeInputSet;
		faceUpdate.rangeIndex = 1;
		faceUpdate.descriptors = faceDescriptor;
		faceUpdate.descriptorNum = 1;
		context.core->UpdateDescriptorRanges(&faceUpdate, 1);
	}

	const nri::Descriptor* resultDescriptor[1] = { state.resultBuffer.shaderView };
	nri::UpdateDescriptorRangeDesc resultUpdate = {};
	resultUpdate.descriptorSet = renderer.mVoxelComputeOutputSet;
	resultUpdate.rangeIndex = 0;
	resultUpdate.descriptors = resultDescriptor;
	resultUpdate.descriptorNum = 1;
	context.core->UpdateDescriptorRanges(&resultUpdate, 1);
	if (emit)
	{
		const nri::Descriptor* emitDescriptors[3] = { state.vertexBuffer.shaderView, state.indexBuffer.shaderView, state.primitiveBuffer.shaderView };
		nri::UpdateDescriptorRangeDesc emitUpdate = {};
		emitUpdate.descriptorSet = renderer.mVoxelComputeOutputSet;
		emitUpdate.rangeIndex = 1;
		emitUpdate.descriptors = emitDescriptors;
		emitUpdate.descriptorNum = 3;
		context.core->UpdateDescriptorRanges(&emitUpdate, 1);
	}

	NRIVoxelComputeConstants constants = {};
	constants.JobCount = (uint32_t)gpuJobs.size();
	constants.SlabRecordCount = (uint32_t)gpuSlabs.size();
	constants.FaceRecordCount = (uint32_t)gpuFaces.size();
	context.core->CmdSetPipelineLayout(*context.commandBuffer, nri::BindPoint::COMPUTE, *renderer.mVoxelComputePipelineLayout);
	context.core->CmdSetRootConstants(*context.commandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	context.core->CmdSetDescriptorSet(*context.commandBuffer, { 0, renderer.mVoxelComputeInputSet, nri::BindPoint::COMPUTE });
	context.core->CmdSetDescriptorSet(*context.commandBuffer, { 1, renderer.mVoxelComputeOutputSet, nri::BindPoint::COMPUTE });
	context.core->CmdSetPipeline(*context.commandBuffer, *renderer.GetPipeline(emit ? NRIRenderer::PipelineSlot::VoxelComputeEmit : NRIRenderer::PipelineSlot::VoxelComputeCount));
	context.core->CmdDispatch(*context.commandBuffer, { (uint32_t)gpuJobs.size(), 1, 1 });

	std::vector<nri::BufferBarrierDesc> readbackBarriers;
	readbackBarriers.resize(emit ? 4 : 1);
	readbackBarriers[0].buffer = state.resultBuffer.buffer;
	readbackBarriers[0].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	readbackBarriers[0].after = NRIResourceCopySourceAccess();
	if (emit)
	{
		readbackBarriers[1].buffer = state.vertexBuffer.buffer;
		readbackBarriers[1].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		readbackBarriers[1].after = NRIResourceCopySourceAccess();
		readbackBarriers[2].buffer = state.indexBuffer.buffer;
		readbackBarriers[2].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		readbackBarriers[2].after = NRIResourceCopySourceAccess();
		readbackBarriers[3].buffer = state.primitiveBuffer.buffer;
		readbackBarriers[3].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		readbackBarriers[3].after = NRIResourceCopySourceAccess();
	}
	nri::BarrierDesc readbackBarrierDesc = {};
	readbackBarrierDesc.buffers = readbackBarriers.data();
	readbackBarrierDesc.bufferNum = (uint32_t)readbackBarriers.size();
	context.core->CmdBarrier(*context.commandBuffer, readbackBarrierDesc);
	context.core->CmdCopyBuffer(*context.commandBuffer, *state.readbackBuffer.buffer, 0, *state.resultBuffer.buffer, 0, resultBytes);
	if (emit)
	{
		context.core->CmdCopyBuffer(*context.commandBuffer, *state.vertexReadbackBuffer.buffer, 0, *state.vertexBuffer.buffer, 0, vertexBytes);
		context.core->CmdCopyBuffer(*context.commandBuffer, *state.indexReadbackBuffer.buffer, 0, *state.indexBuffer.buffer, 0, indexBytes);
		context.core->CmdCopyBuffer(*context.commandBuffer, *state.primitiveReadbackBuffer.buffer, 0, *state.primitiveBuffer.buffer, 0, primitiveBytes);
	}

	constexpr uint32_t MaxDiagnosticBlasPrimitives = 8192;
	const bool allowDiagnosticBlas =
		buildBlas &&
		state.diagnosticBlasBuildsSubmitted == 0 &&
		vertexOffset > 0 &&
		indexOffset > 0 &&
		primitiveOffset > 0 &&
		primitiveOffset <= MaxDiagnosticBlasPrimitives;
	if (allowDiagnosticBlas)
	{
		nri::BufferBarrierDesc blasBarriers[2] = {};
		blasBarriers[0].buffer = state.vertexBuffer.buffer;
		blasBarriers[0].before = NRIResourceCopySourceAccess();
		blasBarriers[0].after = NRIResourceAccelerationStructureBuildInputAccess();
		blasBarriers[1].buffer = state.indexBuffer.buffer;
		blasBarriers[1].before = NRIResourceCopySourceAccess();
		blasBarriers[1].after = NRIResourceAccelerationStructureBuildInputAccess();
		nri::BarrierDesc blasBarrierDesc = {};
		blasBarrierDesc.buffers = blasBarriers;
		blasBarrierDesc.bufferNum = 2;
		context.core->CmdBarrier(*context.commandBuffer, blasBarrierDesc);

		for (PendingReadbackJob& job : pendingJobs)
		{
			job.admissionState = VoxelComputeAdmissionState::BlasBuilding;
			TraceAdmission(frameNumber, job, job.admissionState, "blas_build");
		}
		const bool blasOk = renderer.BuildBottomLevelAccelerationStructure(
			state.vertexBuffer,
			state.indexBuffer,
			vertexOffset,
			0,
			indexOffset,
			primitiveOffset,
			state.diagnosticBlas,
			false);
		for (PendingReadbackJob& job : pendingJobs)
		{
			job.admissionState = blasOk ? VoxelComputeAdmissionState::BlasReady : VoxelComputeAdmissionState::Failed;
			TraceAdmission(frameNumber, job, job.admissionState, blasOk ? "blas_ready" : "blas_failed");
		}
		if (IsTraceEnabled())
		{
			Printf(
				"PERF pt voxel compute blas NRI: frame=%llu jobs=%u success=%u vertices=%u indices=%u primitives=%u blas_bytes=%llu scratch_bytes=%llu\n",
				(unsigned long long)frameNumber,
				(unsigned)gpuJobs.size(),
				blasOk ? 1u : 0u,
				vertexOffset,
				indexOffset,
				primitiveOffset,
				(unsigned long long)state.diagnosticBlas.memorySize,
				(unsigned long long)state.diagnosticBlas.buildScratchSize);
		}
		if (blasOk)
		{
			state.diagnosticBlasBuildsSubmitted++;
		}
	}
	else if (buildBlas && IsTraceEnabled())
	{
		Printf(
			"PERF pt voxel compute blas NRI: frame=%llu jobs=%u success=0 skipped=1 reason=%s vertices=%u indices=%u primitives=%u max_primitives=%u submitted=%u\n",
			(unsigned long long)frameNumber,
			(unsigned)gpuJobs.size(),
			state.diagnosticBlasBuildsSubmitted != 0 ? "single_build_budget" : "primitive_budget",
			vertexOffset,
			indexOffset,
			primitiveOffset,
			MaxDiagnosticBlasPrimitives,
			state.diagnosticBlasBuildsSubmitted);
	}

	state.pendingFrame = frameNumber;
	state.pendingReadbackValid = true;
	state.pendingEmit = emit;
	state.pendingBlas = buildBlas;
	state.pendingReadbackJobs = std::move(pendingJobs);
	state.pendingVertexCount = vertexOffset;
	state.pendingIndexCount = indexOffset;
	state.pendingPrimitiveCount = primitiveOffset;
	if (IsTraceEnabled())
	{
		Printf(
			"PERF pt voxel compute dispatch NRI: frame=%llu mode=%s jobs=%u slab_records=%u face_records=%u job_bytes=%llu slab_bytes=%llu face_bytes=%llu result_bytes=%llu vertex_bytes=%llu index_bytes=%llu primitive_bytes=%llu\n",
			(unsigned long long)frameNumber,
			emit ? (buildBlas ? "emit_blas" : "emit") : "count",
			(unsigned)gpuJobs.size(),
			(unsigned)gpuSlabs.size(),
			(unsigned)gpuFaces.size(),
			(unsigned long long)jobBytes,
			(unsigned long long)slabBytes,
			(unsigned long long)faceBytes,
			(unsigned long long)resultBytes,
			(unsigned long long)vertexBytes,
			(unsigned long long)indexBytes,
			(unsigned long long)primitiveBytes);
	}
}

void DestroyNRIVoxelComputeMeshingDiagnostics(NRIRenderer& renderer)
{
	NRIResourceServices services = renderer.BuildResourceServices();
	VoxelComputeState& state = gVoxelComputeState;
	services.DestroyBufferResource(state.jobUploadBuffer);
	services.DestroyBufferResource(state.slabUploadBuffer);
	services.DestroyBufferResource(state.faceUploadBuffer);
	services.DestroyBufferResource(state.jobBuffer);
	services.DestroyBufferResource(state.slabBuffer);
	services.DestroyBufferResource(state.faceBuffer);
	services.DestroyBufferResource(state.resultBuffer);
	services.DestroyBufferResource(state.vertexBuffer);
	services.DestroyBufferResource(state.indexBuffer);
	services.DestroyBufferResource(state.primitiveBuffer);
	services.DestroyBufferResource(state.readbackBuffer);
	services.DestroyBufferResource(state.vertexReadbackBuffer);
	services.DestroyBufferResource(state.indexReadbackBuffer);
	services.DestroyBufferResource(state.primitiveReadbackBuffer);
	renderer.DestroyAccelerationStructureResource(state.diagnosticBlas);
	state.queuedJobs.clear();
	state.pendingReadbackJobs.clear();
	state.pendingFrame = 0;
	state.pendingReadbackValid = false;
	state.pendingEmit = false;
	state.pendingBlas = false;
	state.pendingVertexCount = 0;
	state.pendingIndexCount = 0;
	state.pendingPrimitiveCount = 0;
	state.diagnosticBlasBuildsSubmitted = 0;
}
