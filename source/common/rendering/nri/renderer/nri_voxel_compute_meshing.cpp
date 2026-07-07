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
#include <unordered_map>
#include <unordered_set>
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
		uint64_t consumeKey = 0;
		uint64_t directMeshResourceKey = 0;
		uint64_t directKey = 0;
		uint64_t directGeneration = 0;
		uint64_t sourceArchiveSerial = 0;
		uint32_t cpuVertexCount = 0;
		uint32_t cpuIndexCount = 0;
		uint32_t jobId = 0;
		uint32_t materialBase = 0;
		uint32_t materialCount = 0;
		bool directPublication = false;
		NRIVoxelComputeDirectPublishOutputKind outputKind = NRIVoxelComputeDirectPublishOutputKind::None;
		NRIVoxelComputeDirectPublishOutputBuffers outputBuffers;
		NRIVoxelComputeDirectPublishRange vertices;
		NRIVoxelComputeDirectPublishRange indices;
		NRIVoxelComputeDirectPublishRange primitives;
		VoxelComputeAdmissionState admissionState = VoxelComputeAdmissionState::Queued;
		std::vector<NRIVoxelComputeSlabRecord> slabs;
		std::vector<NRIVoxelComputeFaceRecord> faces;
		std::vector<NRIVoxelComputeColorRunRecord> colorRuns;
	};

	struct RawVoxelSourceArchiveEntry
	{
		FVoxelRawMeshStats stats = {};
		std::vector<NRIVoxelComputeSlabRecord> slabs;
		std::vector<NRIVoxelComputeFaceRecord> faces;
		std::vector<NRIVoxelComputeColorRunRecord> colorRuns;
		NRIBufferResource slabUploadBuffer = {};
		NRIBufferResource colorRunUploadBuffer = {};
		NRIBufferResource slabBuffer = {};
		NRIBufferResource colorRunBuffer = {};
		uint64_t recordSerial = 0;
		uint64_t slabBytes = 0;
		uint64_t faceBytes = 0;
		uint64_t colorRunBytes = 0;
		bool uploadQueued = false;
		bool uploaded = false;
		bool failed = false;
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
		uint64_t consumeKey = 0;
		uint64_t directMeshResourceKey = 0;
		uint64_t directKey = 0;
		uint64_t directGeneration = 0;
		uint64_t sourceArchiveSerial = 0;
		uint32_t materialBase = 0;
		uint32_t materialCount = 0;
		bool directPublication = false;
		NRIVoxelComputeDirectPublishOutputKind outputKind = NRIVoxelComputeDirectPublishOutputKind::None;
		NRIVoxelComputeDirectPublishRange vertices;
		NRIVoxelComputeDirectPublishRange indices;
		NRIVoxelComputeDirectPublishRange primitives;
		VoxelComputeAdmissionState admissionState = VoxelComputeAdmissionState::Counting;
	};

	struct GeneratedVoxelGeometry
	{
		nri_scene::GeometryData geometry;
		uint32_t jobId = 0;
		uint32_t vertexHash = 0;
		uint32_t indexHash = 0;
		uint32_t primitiveHash = 0;
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
		bool pendingFullGeneratedReadback = false;
		uint32_t pendingVertexCount = 0;
		uint32_t pendingIndexCount = 0;
		uint32_t pendingPrimitiveCount = 0;
		uint32_t diagnosticBlasBuildsSubmitted = 0;
		uint64_t nextRawSourceSerial = 1;
		uint64_t rawSourceArchiveHits = 0;
		uint64_t rawSourceArchiveMisses = 0;
		uint64_t rawSourceArchiveRecords = 0;
		uint64_t rawSourceArchiveUploadBytes = 0;
		uint64_t rawSourceArchiveUploadFailures = 0;
		std::unordered_set<uint64_t> queuedConsumeKeys;
		std::unordered_set<uint64_t> pendingConsumeKeys;
		std::unordered_set<uint64_t> failedConsumeKeys;
		std::unordered_set<uint64_t> queuedDirectKeys;
		std::unordered_set<uint64_t> pendingDirectKeys;
		std::unordered_set<uint64_t> failedDirectKeys;
		std::unordered_map<uint64_t, GeneratedVoxelGeometry> readyGeneratedGeometry;
		std::unordered_map<uint64_t, NRIVoxelComputeDirectPublishedMesh> readyDirectPublishedMeshes;
		std::unordered_map<FVoxelModel*, RawVoxelSourceArchiveEntry> rawSourceArchive;

		NRIBufferResource jobUploadBuffer = {};
		NRIBufferResource slabUploadBuffer = {};
		NRIBufferResource faceUploadBuffer = {};
		NRIBufferResource colorRunUploadBuffer = {};
		NRIBufferResource jobBuffer = {};
		NRIBufferResource slabBuffer = {};
		NRIBufferResource faceBuffer = {};
		NRIBufferResource colorRunBuffer = {};
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

	bool IsConsumptionEnabled()
	{
		return (bool)nri_ptvoxelcompute && (int)nri_ptvoxelcomputemode >= 6;
	}

	bool IsDirectGpuPublicationEnabled()
	{
		return (bool)nri_ptvoxelcompute && (bool)nri_ptvoxelcomputedirectgpu;
	}

	bool IsDirectPublicationEnabled()
	{
		return (bool)nri_ptvoxelcompute && (bool)nri_ptvoxelcomputedirectpublish;
	}

	bool IsRawSourceArchiveEnabled()
	{
		return (bool)nri_ptvoxelcompute && (bool)nri_ptvoxelcomputerawarchive;
	}

	bool IsFullGeneratedReadbackEnabled()
	{
		return (bool)nri_ptvoxelcomputevalidatefullreadback;
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

	const char* DirectPublishOutputKindName(NRIVoxelComputeDirectPublishOutputKind kind)
	{
		switch (kind)
		{
		case NRIVoxelComputeDirectPublishOutputKind::None: return "none";
		case NRIVoxelComputeDirectPublishOutputKind::SharedPersistentArena: return "shared_persistent_arena";
		case NRIVoxelComputeDirectPublishOutputKind::PrivateBlasInputsAndSharedArena: return "private_blas_inputs_and_shared_arena";
		case NRIVoxelComputeDirectPublishOutputKind::PrivateBuffers: return "private_buffers";
		default: return "unknown";
		}
	}

	uint64_t BuildDirectPublishKey(uint64_t meshResourceKey, uint64_t generation)
	{
		uint64_t hash = meshResourceKey;
		hash ^= generation + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		return hash;
	}

	bool DirectPublishBufferReady(const NRIBufferResource* resource, nri::BufferUsageBits usage)
	{
		return
			resource != nullptr &&
			resource->buffer != nullptr &&
			resource->shaderView != nullptr &&
			resource->storageView != nullptr &&
			NRIResourceUsageIncludes(resource->usage, usage);
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

	void CopySlabRecords(const TArray<FVoxelRawSlabRecord>& source, std::vector<NRIVoxelComputeSlabRecord>& target)
	{
		target.clear();
		target.reserve((size_t)source.Size());
		for (unsigned int i = 0; i < source.Size(); ++i)
		{
			const FVoxelRawSlabRecord& slab = source[i];
			NRIVoxelComputeSlabRecord record = {};
			record.X = slab.x;
			record.Y = slab.y;
			record.ZTop = slab.zTop;
			record.CullMask = slab.cullMask;
			record.ZLength = slab.zLength;
			record.ColorRunCount = slab.colorRunCount;
			record.ColorRunOffset = slab.colorRunOffset;
			target.push_back(record);
		}
	}

	void CopyColorRunRecords(const TArray<FVoxelRawColorRunRecord>& source, std::vector<NRIVoxelComputeColorRunRecord>& target)
	{
		target.clear();
		target.reserve((size_t)source.Size());
		for (unsigned int i = 0; i < source.Size(); ++i)
		{
			const FVoxelRawColorRunRecord& run = source[i];
			NRIVoxelComputeColorRunRecord record = {};
			record.ZOffset = run.zOffset;
			record.ZLength = run.zLength;
			record.Color = run.color;
			target.push_back(record);
		}
	}

	void CopyFaceRecords(const TArray<FVoxelRawFaceRecord>& source, std::vector<NRIVoxelComputeFaceRecord>& target)
	{
		target.clear();
		target.reserve((size_t)source.Size());
		for (unsigned int i = 0; i < source.Size(); ++i)
		{
			const FVoxelRawFaceRecord& face = source[i];
			NRIVoxelComputeFaceRecord record = {};
			for (uint32_t v = 0; v < 4; ++v)
			{
				record.X[v] = face.x[v];
				record.Y[v] = face.y[v];
				record.Z[v] = face.z[v];
			}
			record.Color = face.color;
			record.MaterialIndex = face.materialIndex;
			target.push_back(record);
		}
	}

	RawVoxelSourceArchiveEntry* RecordRawSourceArchive(
		FVoxelModel* model,
		const FVoxelRawMeshStats& stats,
		const TArray<FVoxelRawSlabRecord>& slabs,
		const TArray<FVoxelRawFaceRecord>* faces,
		const TArray<FVoxelRawColorRunRecord>* colorRuns)
	{
		if (!IsRawSourceArchiveEnabled() || model == nullptr || stats.slabCount == 0 || slabs.Size() != stats.slabCount ||
			colorRuns == nullptr || colorRuns->Size() == 0)
		{
			return nullptr;
		}

		VoxelComputeState& state = gVoxelComputeState;
		auto found = state.rawSourceArchive.find(model);
		if (found != state.rawSourceArchive.end())
		{
			state.rawSourceArchiveHits++;
			return &found->second;
		}

		RawVoxelSourceArchiveEntry entry = {};
		entry.stats = stats;
		entry.recordSerial = state.nextRawSourceSerial++;
		CopySlabRecords(slabs, entry.slabs);
		CopyColorRunRecords(*colorRuns, entry.colorRuns);
		if (faces != nullptr)
		{
			CopyFaceRecords(*faces, entry.faces);
		}
		entry.slabBytes = (uint64_t)entry.slabs.size() * sizeof(NRIVoxelComputeSlabRecord);
		entry.faceBytes = (uint64_t)entry.faces.size() * sizeof(NRIVoxelComputeFaceRecord);
		entry.colorRunBytes = (uint64_t)entry.colorRuns.size() * sizeof(NRIVoxelComputeColorRunRecord);
		entry.uploadQueued = true;
		state.rawSourceArchiveMisses++;
		state.rawSourceArchiveRecords++;
		auto inserted = state.rawSourceArchive.emplace(model, std::move(entry));
		RawVoxelSourceArchiveEntry& archived = inserted.first->second;
		if (IsTraceEnabled())
		{
			Printf(
				"PERF pt voxel raw source archive NRI: event=record model=%p serial=%llu raw_bytes=%llu slab_upload_bytes=%llu color_run_bytes=%llu transient_face_bytes=%llu slabs=%u color_runs=%u faces=%u voxels=%u\n",
				(void*)model,
				(unsigned long long)archived.recordSerial,
				(unsigned long long)archived.stats.rawByteCount,
				(unsigned long long)archived.slabBytes,
				(unsigned long long)archived.colorRunBytes,
				(unsigned long long)archived.faceBytes,
				archived.stats.slabCount,
				(uint32_t)archived.colorRuns.size(),
				archived.stats.coalescedFaceCount,
				archived.stats.voxelCount);
		}
		return &archived;
	}

	RawVoxelSourceArchiveEntry* FindUploadedRawSourceArchive(FVoxelModel* model)
	{
		if (model == nullptr)
		{
			return nullptr;
		}
		VoxelComputeState& state = gVoxelComputeState;
		auto found = state.rawSourceArchive.find(model);
		if (found == state.rawSourceArchive.end() || !found->second.uploaded || found->second.failed ||
			found->second.slabBuffer.shaderView == nullptr || found->second.colorRunBuffer.shaderView == nullptr)
		{
			return nullptr;
		}
		state.rawSourceArchiveHits++;
		return &found->second;
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
		resource.usage = usage;

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
		if (resource.buffer != nullptr &&
			resource.size >= size &&
			resource.stride == stride &&
			resource.memoryLocation == memoryLocation &&
			NRIResourceUsageIncludes(resource.usage, usage))
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

	bool ImportGeneratedGeometry(
		const PendingReadbackJob& job,
		const NRIVoxelComputeResult& result,
		const NRIVoxelComputeSceneVertex* vertices,
		const uint32_t* indices,
		const NRIVoxelComputePrimitiveData* primitives,
		GeneratedVoxelGeometry& outGenerated)
	{
		if (vertices == nullptr || indices == nullptr || primitives == nullptr ||
			result.VertexCountNoDedupe != job.expectedVerticesNoDedupe ||
			result.IndexCount != job.expectedIndices ||
			result.PrimitiveCount != job.expectedPrimitives)
		{
			return false;
		}

		nri_scene::GeometryData geometry = {};
		geometry.vertices.resize(result.VertexCountNoDedupe);
		geometry.indices.resize(result.IndexCount);
		geometry.primitives.resize(result.PrimitiveCount);

		for (uint32_t i = 0; i < result.VertexCountNoDedupe; ++i)
		{
			const NRIVoxelComputeSceneVertex& source = vertices[(uint64_t)job.vertexOffset + i];
			nri_scene::SceneVertex& target = geometry.vertices[i];
			target.position[0] = source.Position[0];
			target.position[1] = source.Position[1];
			target.position[2] = source.Position[2];
			target.prevPosition[0] = source.PrevPosition[0];
			target.prevPosition[1] = source.PrevPosition[1];
			target.prevPosition[2] = source.PrevPosition[2];
			target.uv[0] = source.Uv[0];
			target.uv[1] = source.Uv[1];
		}

		for (uint32_t i = 0; i < result.IndexCount; ++i)
		{
			const uint32_t globalIndex = indices[(uint64_t)job.indexOffset + i];
			if (globalIndex < job.vertexOffset || globalIndex >= job.vertexOffset + result.VertexCountNoDedupe)
			{
				return false;
			}
			geometry.indices[i] = globalIndex - job.vertexOffset;
		}

		for (uint32_t i = 0; i < result.PrimitiveCount; ++i)
		{
			const NRIVoxelComputePrimitiveData& source = primitives[(uint64_t)job.primitiveOffset + i];
			nri_scene::PrimitiveData& target = geometry.primitives[i];
			for (uint32_t vertex = 0; vertex < 3; ++vertex)
			{
				if (source.Indices[vertex] < job.vertexOffset || source.Indices[vertex] >= job.vertexOffset + result.VertexCountNoDedupe)
				{
					return false;
				}
				target.indices[vertex] = source.Indices[vertex] - job.vertexOffset;
			}
			target.materialIndex = source.MaterialIndex;
			target.uv0[0] = source.Uv0[0];
			target.uv0[1] = source.Uv0[1];
			target.uv1[0] = source.Uv1[0];
			target.uv1[1] = source.Uv1[1];
			target.uv2[0] = source.Uv2[0];
			target.uv2[1] = source.Uv2[1];
			target.normal[0] = source.Normal[0];
			target.normal[1] = source.Normal[1];
			target.normal[2] = source.Normal[2];
			target.flags = source.Flags;
			target.portalIndex = source.PortalIndex;
			target.reserved0 = source.Reserved0;
		}

		outGenerated.geometry = std::move(geometry);
		outGenerated.jobId = job.jobId;
		outGenerated.vertexHash = result.VertexHash;
		outGenerated.indexHash = result.IndexHash;
		outGenerated.primitiveHash = result.PrimitiveHash;
		return true;
	}

	void TraceAdmission(uint64_t frameNumber, const PendingReadbackJob& job, VoxelComputeAdmissionState state, const char* reason)
	{
		if (!IsAdmissionTraceEnabled() && !IsTraceEnabled())
		{
			return;
		}

		Printf(
			"PERF pt voxel compute admission NRI: frame=%llu job=%u consume_key=0x%llx state=%s reason=%s faces=%u vertices=%u indices=%u primitives=%u\n",
			(unsigned long long)frameNumber,
			job.jobId,
			(unsigned long long)job.consumeKey,
			AdmissionStateName(state),
			reason != nullptr ? reason : "unknown",
			job.expectedFaces,
			job.expectedVerticesNoDedupe,
			job.expectedIndices,
			job.expectedPrimitives);
	}

	void UploadPendingRawSources(const NRIResourceServices& services, uint64_t frameNumber)
	{
		VoxelComputeState& state = gVoxelComputeState;
		if (!IsRawSourceArchiveEnabled() || state.rawSourceArchive.empty() || services.context.core == nullptr || services.context.commandBuffer == nullptr)
		{
			return;
		}

		uint32_t uploads = 0;
		uint32_t failures = 0;
		uint64_t uploadedBytes = 0;
		for (auto& archivePair : state.rawSourceArchive)
		{
			RawVoxelSourceArchiveEntry& entry = archivePair.second;
			if (!entry.uploadQueued || entry.uploaded || entry.failed ||
				entry.slabBytes == 0 || entry.colorRunBytes == 0 ||
				entry.slabs.empty() || entry.colorRuns.empty())
			{
				continue;
			}

			const bool ok =
				EnsureBuffer(services, entry.slabUploadBuffer, entry.slabBytes, sizeof(NRIVoxelComputeSlabRecord), nri::BufferUsageBits::NONE, nri::MemoryLocation::DEVICE_UPLOAD, nri::BufferView::STRUCTURED_BUFFER, false) &&
				EnsureBuffer(services, entry.slabBuffer, entry.slabBytes, sizeof(NRIVoxelComputeSlabRecord), nri::BufferUsageBits::SHADER_RESOURCE, nri::MemoryLocation::DEVICE, nri::BufferView::STRUCTURED_BUFFER, true) &&
				EnsureBuffer(services, entry.colorRunUploadBuffer, entry.colorRunBytes, sizeof(NRIVoxelComputeColorRunRecord), nri::BufferUsageBits::NONE, nri::MemoryLocation::DEVICE_UPLOAD, nri::BufferView::STRUCTURED_BUFFER, false) &&
				EnsureBuffer(services, entry.colorRunBuffer, entry.colorRunBytes, sizeof(NRIVoxelComputeColorRunRecord), nri::BufferUsageBits::SHADER_RESOURCE, nri::MemoryLocation::DEVICE, nri::BufferView::STRUCTURED_BUFFER, true) &&
				CopyToUploadBuffer(services.context, entry.slabUploadBuffer, entry.slabs.data(), entry.slabBytes) &&
				CopyToUploadBuffer(services.context, entry.colorRunUploadBuffer, entry.colorRuns.data(), entry.colorRunBytes);

			if (!ok)
			{
				entry.failed = true;
				entry.uploadQueued = false;
				state.rawSourceArchiveUploadFailures++;
				failures++;
				continue;
			}

			nri::BufferBarrierDesc uploadBarriers[4] = {};
			uploadBarriers[0].buffer = entry.slabUploadBuffer.buffer;
			uploadBarriers[0].after = NRIResourceCopySourceAccess();
			uploadBarriers[1].buffer = entry.slabBuffer.buffer;
			uploadBarriers[1].after = NRIResourceCopyDestinationAccess();
			uploadBarriers[2].buffer = entry.colorRunUploadBuffer.buffer;
			uploadBarriers[2].after = NRIResourceCopySourceAccess();
			uploadBarriers[3].buffer = entry.colorRunBuffer.buffer;
			uploadBarriers[3].after = NRIResourceCopyDestinationAccess();
			nri::BarrierDesc uploadBarrier = {};
			uploadBarrier.buffers = uploadBarriers;
			uploadBarrier.bufferNum = 4;
			services.context.core->CmdBarrier(*services.context.commandBuffer, uploadBarrier);
			services.context.core->CmdCopyBuffer(*services.context.commandBuffer, *entry.slabBuffer.buffer, 0, *entry.slabUploadBuffer.buffer, 0, entry.slabBytes);
			services.context.core->CmdCopyBuffer(*services.context.commandBuffer, *entry.colorRunBuffer.buffer, 0, *entry.colorRunUploadBuffer.buffer, 0, entry.colorRunBytes);

			nri::BufferBarrierDesc shaderBarriers[2] = {};
			shaderBarriers[0].buffer = entry.slabBuffer.buffer;
			shaderBarriers[0].before = NRIResourceCopyDestinationAccess();
			shaderBarriers[0].after = NRIResourceComputeShaderResourceAccess();
			shaderBarriers[1].buffer = entry.colorRunBuffer.buffer;
			shaderBarriers[1].before = NRIResourceCopyDestinationAccess();
			shaderBarriers[1].after = NRIResourceComputeShaderResourceAccess();
			nri::BarrierDesc shaderBarrier = {};
			shaderBarrier.buffers = shaderBarriers;
			shaderBarrier.bufferNum = 2;
			services.context.core->CmdBarrier(*services.context.commandBuffer, shaderBarrier);

			entry.uploaded = true;
			entry.uploadQueued = false;
			uploads++;
			uploadedBytes += entry.slabBytes + entry.colorRunBytes;
		}

		if (uploads != 0 || failures != 0)
		{
			state.rawSourceArchiveUploadBytes += uploadedBytes;
			if (IsTraceEnabled())
			{
				Printf(
					"PERF pt voxel raw source upload NRI: frame=%llu uploads=%u failures=%u bytes=%llu records=%llu hits=%llu misses=%llu total_upload_bytes=%llu total_failures=%llu\n",
					(unsigned long long)frameNumber,
					uploads,
					failures,
					(unsigned long long)uploadedBytes,
					(unsigned long long)state.rawSourceArchiveRecords,
					(unsigned long long)state.rawSourceArchiveHits,
					(unsigned long long)state.rawSourceArchiveMisses,
					(unsigned long long)state.rawSourceArchiveUploadBytes,
					(unsigned long long)state.rawSourceArchiveUploadFailures);
			}
		}
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
		const NRIVoxelComputeSceneVertex* generatedVertices = nullptr;
		const uint32_t* generatedIndices = nullptr;
		const NRIVoxelComputePrimitiveData* generatedPrimitives = nullptr;
		if (state.pendingEmit && state.pendingFullGeneratedReadback)
		{
			const uint64_t vertexBytes = (uint64_t)state.pendingVertexCount * sizeof(NRIVoxelComputeSceneVertex);
			const uint64_t indexBytes = (uint64_t)state.pendingIndexCount * sizeof(uint32_t);
			const uint64_t primitiveBytes = (uint64_t)state.pendingPrimitiveCount * sizeof(NRIVoxelComputePrimitiveData);
			generatedVertices = state.vertexReadbackBuffer.buffer != nullptr && vertexBytes != 0 ?
				static_cast<const NRIVoxelComputeSceneVertex*>(services.context.core->MapBuffer(*state.vertexReadbackBuffer.buffer, 0, vertexBytes)) :
				nullptr;
			generatedIndices = state.indexReadbackBuffer.buffer != nullptr && indexBytes != 0 ?
				static_cast<const uint32_t*>(services.context.core->MapBuffer(*state.indexReadbackBuffer.buffer, 0, indexBytes)) :
				nullptr;
			generatedPrimitives = state.primitiveReadbackBuffer.buffer != nullptr && primitiveBytes != 0 ?
				static_cast<const NRIVoxelComputePrimitiveData*>(services.context.core->MapBuffer(*state.primitiveReadbackBuffer.buffer, 0, primitiveBytes)) :
				nullptr;
		}
		uint32_t okCount = 0;
		uint32_t mismatchCount = 0;
		uint32_t directStatusJobs = 0;
		uint32_t directStatusReady = 0;
		uint32_t directStatusFailed = 0;
		const uint64_t fullGeometryReadbackBytes =
			state.pendingEmit && state.pendingFullGeneratedReadback ?
			(uint64_t)state.pendingVertexCount * sizeof(NRIVoxelComputeSceneVertex) +
				(uint64_t)state.pendingIndexCount * sizeof(uint32_t) +
				(uint64_t)state.pendingPrimitiveCount * sizeof(NRIVoxelComputePrimitiveData) :
			0ull;
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
			if (job.consumeKey != 0)
			{
				state.pendingConsumeKeys.erase(job.consumeKey);
				if (ok && state.pendingEmit && state.pendingFullGeneratedReadback)
				{
					GeneratedVoxelGeometry generated = {};
					if (ImportGeneratedGeometry(job, result, generatedVertices, generatedIndices, generatedPrimitives, generated))
					{
						state.readyGeneratedGeometry[job.consumeKey] = std::move(generated);
						state.failedConsumeKeys.erase(job.consumeKey);
					}
					else
					{
						state.failedConsumeKeys.insert(job.consumeKey);
						job.admissionState = VoxelComputeAdmissionState::Failed;
					}
				}
				else
				{
					state.failedConsumeKeys.insert(job.consumeKey);
				}
			}
			if (job.directPublication)
			{
				directStatusJobs++;
				directStatusReady += ok ? 1u : 0u;
				directStatusFailed += ok ? 0u : 1u;
				const bool stillPending = state.pendingDirectKeys.erase(job.directKey) != 0;
				if (ok && state.pendingEmit && stillPending)
				{
					NRIVoxelComputeDirectPublishedMesh published = {};
					published.meshResourceKey = job.directMeshResourceKey;
					published.generation = job.directGeneration;
					published.sourceArchiveSerial = job.sourceArchiveSerial;
					published.jobId = job.jobId;
					published.readyFrame = (uint32_t)std::min<uint64_t>(state.pendingFrame, UINT32_MAX);
					published.status = NRIVoxelComputeGeneratedGeometryStatus::Ready;
					published.failure = NRIVoxelComputeDirectPublishFailure::None;
					published.outputKind = job.outputKind;
					published.vertices = job.vertices;
					published.vertices.count = result.VertexCountNoDedupe;
					published.indices = job.indices;
					published.indices.count = result.IndexCount;
					published.primitives = job.primitives;
					published.primitives.count = result.PrimitiveCount;
					published.materialBase = job.materialBase;
					published.materialCount = job.materialCount;
					state.readyDirectPublishedMeshes[job.directKey] = published;
					state.failedDirectKeys.erase(job.directKey);
				}
				else if (stillPending)
				{
					state.failedDirectKeys.insert(job.directKey);
				}
				if (IsTraceEnabled())
				{
					Printf(
						"PERF pt voxel compute direct publish NRI: action=status frame=%llu job=%u generation=%llu status=%s mismatch=%u vertices=%u indices=%u primitives=%u source_serial=%llu status_readback_bytes=%u full_geometry_readback_bytes=%llu\n",
						(unsigned long long)state.pendingFrame,
						job.jobId,
						(unsigned long long)job.directGeneration,
						ok ? "ready" : "failed",
						result.MismatchMask,
						result.VertexCountNoDedupe,
						result.IndexCount,
						result.PrimitiveCount,
						(unsigned long long)job.sourceArchiveSerial,
						(uint32_t)sizeof(NRIVoxelComputeResult),
						(unsigned long long)fullGeometryReadbackBytes);
				}
			}
			TraceAdmission(state.pendingFrame, job, job.admissionState, state.pendingEmit ? "readback_emit" : "readback_count");
			if (IsTraceEnabled())
			{
				Printf(
					"PERF pt voxel compute %s NRI: frame=%llu job=%u consume_key=0x%llx status=%u mismatch=%u faces=%u expected_faces=%u indices=%u expected_indices=%u vertices_nodedupe=%u expected_vertices_nodedupe=%u primitives=%u expected_primitives=%u voxels=%u expected_voxels=%u cpu_vertices=%u cpu_indices=%u vertex_hash=%u index_hash=%u primitive_hash=%u\n",
					state.pendingEmit ? "emit" : "count",
					(unsigned long long)state.pendingFrame,
					job.jobId,
					(unsigned long long)job.consumeKey,
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
		if (generatedVertices != nullptr)
		{
			services.context.core->UnmapBuffer(*state.vertexReadbackBuffer.buffer);
		}
		if (generatedIndices != nullptr)
		{
			services.context.core->UnmapBuffer(*state.indexReadbackBuffer.buffer);
		}
		if (generatedPrimitives != nullptr)
		{
			services.context.core->UnmapBuffer(*state.primitiveReadbackBuffer.buffer);
		}
		services.context.core->UnmapBuffer(*state.readbackBuffer.buffer);

		if (state.pendingEmit && state.pendingFullGeneratedReadback && IsTraceEnabled())
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
		if (directStatusJobs != 0 && IsTraceEnabled())
		{
			Printf(
				"PERF pt voxel compute direct readback NRI: frame=%llu jobs=%u ready=%u failed=%u status_readback_bytes=%llu full_geometry_readback_bytes=%llu production_guard_ok=%u\n",
				(unsigned long long)state.pendingFrame,
				directStatusJobs,
				directStatusReady,
				directStatusFailed,
				(unsigned long long)((uint64_t)directStatusJobs * sizeof(NRIVoxelComputeResult)),
				(unsigned long long)fullGeometryReadbackBytes,
				fullGeometryReadbackBytes == 0 ? 1u : 0u);
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
		state.pendingFullGeneratedReadback = false;
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

bool ShouldConsumeNRIVoxelComputeMeshing()
{
	return IsConsumptionEnabled() && !(bool)nri_ptvoxelcomputeforcecpu;
}

bool ShouldDirectPublishNRIVoxelComputeMeshing()
{
	return
		ShouldConsumeNRIVoxelComputeMeshing() &&
		IsDirectGpuPublicationEnabled() &&
		IsDirectPublicationEnabled() &&
		IsRawSourceArchiveEnabled() &&
		!IsFullGeneratedReadbackEnabled();
}

NRIVoxelComputeGeneratedGeometryStatus RequestNRIVoxelComputeGeneratedGeometry(uint64_t requestKey, FVoxelModel* model)
{
	constexpr uint32_t MaxConsumptionPrimitives = 8192;
	VoxelComputeState& state = gVoxelComputeState;
	if (!ShouldConsumeNRIVoxelComputeMeshing() || requestKey == 0 || model == nullptr)
	{
		return NRIVoxelComputeGeneratedGeometryStatus::Unavailable;
	}
	if (state.readyGeneratedGeometry.find(requestKey) != state.readyGeneratedGeometry.end())
	{
		return NRIVoxelComputeGeneratedGeometryStatus::Ready;
	}
	if (state.failedConsumeKeys.find(requestKey) != state.failedConsumeKeys.end())
	{
		return NRIVoxelComputeGeneratedGeometryStatus::Failed;
	}
	if (state.queuedConsumeKeys.find(requestKey) != state.queuedConsumeKeys.end() ||
		state.pendingConsumeKeys.find(requestKey) != state.pendingConsumeKeys.end())
	{
		return NRIVoxelComputeGeneratedGeometryStatus::Queued;
	}

	const uint32_t maxJobs = std::max(0, (int)nri_ptvoxelcomputemaxjobs);
	if (maxJobs == 0)
	{
		return NRIVoxelComputeGeneratedGeometryStatus::Unavailable;
	}
	if (state.queuedJobs.size() >= maxJobs)
	{
		auto diagnosticJob = std::find_if(state.queuedJobs.rbegin(), state.queuedJobs.rend(), [](const PendingVoxelComputeJob& job)
		{
			return job.consumeKey == 0;
		});
		if (diagnosticJob == state.queuedJobs.rend())
		{
			return NRIVoxelComputeGeneratedGeometryStatus::Unavailable;
		}
		state.queuedJobs.erase(std::next(diagnosticJob).base());
	}

	FVoxelRawMeshStats rawStats = {};
	TArray<FVoxelRawSlabRecord> rawSlabs;
	TArray<FVoxelRawFaceRecord> rawFaces;
	TArray<FVoxelRawColorRunRecord> rawColorRuns;
	model->BuildRawMeshStats(rawStats, &rawSlabs, &rawFaces, &rawColorRuns);
	RawVoxelSourceArchiveEntry* archivedSource = RecordRawSourceArchive(model, rawStats, rawSlabs, &rawFaces, &rawColorRuns);
	if (rawStats.slabCount == 0 || rawStats.coalescedFaceCount == 0 ||
		rawFaces.Size() != rawStats.coalescedFaceCount ||
		rawStats.coalescedFaceCount > MaxConsumptionPrimitives / 2u)
	{
		if (IsTraceEnabled())
		{
			Printf(
				"PERF pt voxel compute consume NRI: action=fallback reason=%s consume_key=0x%llx faces=%u primitives=%u max_primitives=%u\n",
				rawStats.coalescedFaceCount > MaxConsumptionPrimitives / 2u ? "primitive_budget" : "invalid_raw",
				(unsigned long long)requestKey,
				rawStats.coalescedFaceCount,
				rawStats.coalescedFaceCount * 2u,
				MaxConsumptionPrimitives);
		}
		return NRIVoxelComputeGeneratedGeometryStatus::Unavailable;
	}

	PendingVoxelComputeJob job = {};
	job.model = model;
	job.stats = rawStats;
	job.consumeKey = requestKey;
	job.jobId = state.nextJobId++;
	if (archivedSource != nullptr)
	{
		job.slabs = archivedSource->slabs;
		job.faces = archivedSource->faces;
		job.colorRuns = archivedSource->colorRuns;
	}
	else
	{
		CopySlabRecords(rawSlabs, job.slabs);
		CopyFaceRecords(rawFaces, job.faces);
		CopyColorRunRecords(rawColorRuns, job.colorRuns);
	}
	state.queuedConsumeKeys.insert(requestKey);
	state.queuedJobs.push_back(std::move(job));
	if (IsTraceEnabled())
	{
		Printf(
			"PERF pt voxel compute consume NRI: action=queue consume_key=0x%llx job=%u faces=%u vertices=%u indices=%u primitives=%u\n",
			(unsigned long long)requestKey,
			state.queuedJobs.back().jobId,
			rawStats.coalescedFaceCount,
			rawStats.noDedupeVertexCount,
			rawStats.indexCount,
			rawStats.coalescedFaceCount * 2u);
	}
	return NRIVoxelComputeGeneratedGeometryStatus::Queued;
}

bool TakeNRIVoxelComputeGeneratedGeometry(uint64_t requestKey, nri_scene::GeometryData& outGeometry, uint32_t* outJobId)
{
	VoxelComputeState& state = gVoxelComputeState;
	auto found = state.readyGeneratedGeometry.find(requestKey);
	if (found == state.readyGeneratedGeometry.end())
	{
		return false;
	}
	if (outJobId != nullptr)
	{
		*outJobId = found->second.jobId;
	}
	outGeometry = std::move(found->second.geometry);
	state.readyGeneratedGeometry.erase(found);
	return true;
}

NRIVoxelComputeGeneratedGeometryStatus RequestNRIVoxelComputeDirectPublication(const NRIVoxelComputeDirectPublishRequest& request)
{
	constexpr uint32_t MaxDirectPublishPrimitives = 8192;
	VoxelComputeState& state = gVoxelComputeState;
	const uint64_t directKey = BuildDirectPublishKey(request.meshResourceKey, request.generation);
	if (!ShouldDirectPublishNRIVoxelComputeMeshing())
	{
		return NRIVoxelComputeGeneratedGeometryStatus::Unavailable;
	}
	if (request.meshResourceKey == 0 || request.generation == 0 || request.model == nullptr ||
		request.outputKind == NRIVoxelComputeDirectPublishOutputKind::None ||
		!DirectPublishBufferReady(request.outputBuffers.vertices, NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT)) ||
		!DirectPublishBufferReady(request.outputBuffers.indices, NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT)) ||
		!DirectPublishBufferReady(request.outputBuffers.primitives, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE))
	{
		if (IsTraceEnabled())
		{
			Printf(
				"PERF pt voxel compute direct publish NRI: action=reject reason=invalid_request mesh_resource=0x%llx generation=%llu output=%s\n",
				(unsigned long long)request.meshResourceKey,
				(unsigned long long)request.generation,
				DirectPublishOutputKindName(request.outputKind));
		}
		return NRIVoxelComputeGeneratedGeometryStatus::Failed;
	}
	if (state.readyDirectPublishedMeshes.find(directKey) != state.readyDirectPublishedMeshes.end())
	{
		return NRIVoxelComputeGeneratedGeometryStatus::Ready;
	}
	if (state.failedDirectKeys.find(directKey) != state.failedDirectKeys.end())
	{
		return NRIVoxelComputeGeneratedGeometryStatus::Failed;
	}
	if (state.queuedDirectKeys.find(directKey) != state.queuedDirectKeys.end() ||
		state.pendingDirectKeys.find(directKey) != state.pendingDirectKeys.end())
	{
		return NRIVoxelComputeGeneratedGeometryStatus::Queued;
	}
	if (request.outputKind != NRIVoxelComputeDirectPublishOutputKind::PrivateBlasInputsAndSharedArena)
	{
		if (IsTraceEnabled())
		{
			Printf(
				"PERF pt voxel compute direct publish NRI: action=reject reason=unsupported_output mesh_resource=0x%llx generation=%llu output=%s\n",
				(unsigned long long)request.meshResourceKey,
				(unsigned long long)request.generation,
				DirectPublishOutputKindName(request.outputKind));
		}
		return NRIVoxelComputeGeneratedGeometryStatus::Unavailable;
	}

	const uint32_t maxJobs = std::max(0, (int)nri_ptvoxelcomputemaxjobs);
	if (maxJobs == 0 || state.queuedJobs.size() >= maxJobs)
	{
		return NRIVoxelComputeGeneratedGeometryStatus::Unavailable;
	}

	FVoxelRawMeshStats rawStats = {};
	TArray<FVoxelRawSlabRecord> rawSlabs;
	TArray<FVoxelRawColorRunRecord> rawColorRuns;
	request.model->BuildRawMeshStats(rawStats, &rawSlabs, nullptr, &rawColorRuns);
	RawVoxelSourceArchiveEntry* archivedSource = RecordRawSourceArchive(request.model, rawStats, rawSlabs, nullptr, &rawColorRuns);
	const uint32_t expectedPrimitives = rawStats.coalescedFaceCount * 2u;
	if (rawStats.slabCount == 0 ||
		rawStats.coalescedFaceCount == 0 ||
		rawStats.coalescedFaceCount > MaxDirectPublishPrimitives / 2u ||
		rawStats.noDedupeVertexCount > request.vertices.capacity ||
		rawStats.indexCount > request.indices.capacity ||
		expectedPrimitives > request.primitives.capacity)
	{
		state.failedDirectKeys.insert(directKey);
		if (IsTraceEnabled())
		{
			Printf(
				"PERF pt voxel compute direct publish NRI: action=reject reason=%s mesh_resource=0x%llx generation=%llu faces=%u vertices=%u/%u indices=%u/%u primitives=%u/%u max_primitives=%u\n",
				rawStats.coalescedFaceCount > MaxDirectPublishPrimitives / 2u ? "primitive_budget" : "capacity_or_raw",
				(unsigned long long)request.meshResourceKey,
				(unsigned long long)request.generation,
				rawStats.coalescedFaceCount,
				rawStats.noDedupeVertexCount,
				request.vertices.capacity,
				rawStats.indexCount,
				request.indices.capacity,
				expectedPrimitives,
				request.primitives.capacity,
				MaxDirectPublishPrimitives);
		}
		return NRIVoxelComputeGeneratedGeometryStatus::Failed;
	}

	PendingVoxelComputeJob job = {};
	job.model = request.model;
	job.stats = rawStats;
	job.directPublication = true;
	job.directMeshResourceKey = request.meshResourceKey;
	job.directKey = directKey;
	job.directGeneration = request.generation;
	job.sourceArchiveSerial = archivedSource != nullptr ? archivedSource->recordSerial : 0;
	job.jobId = state.nextJobId++;
	job.materialBase = request.materialBase;
	job.materialCount = request.materialCount;
	job.outputKind = request.outputKind;
	job.outputBuffers = request.outputBuffers;
	job.vertices = request.vertices;
	job.indices = request.indices;
	job.primitives = request.primitives;
	state.queuedDirectKeys.insert(directKey);
	state.queuedJobs.push_back(std::move(job));
	if (IsTraceEnabled())
	{
		Printf(
			"PERF pt voxel compute direct publish NRI: action=queue mesh_resource=0x%llx generation=%llu output=%s job=%u source_serial=%llu vertex_offset=%u vertex_capacity=%u index_offset=%u index_capacity=%u primitive_offset=%u primitive_capacity=%u material_base=%u material_count=%u\n",
			(unsigned long long)request.meshResourceKey,
			(unsigned long long)request.generation,
			DirectPublishOutputKindName(request.outputKind),
			state.queuedJobs.back().jobId,
			(unsigned long long)state.queuedJobs.back().sourceArchiveSerial,
			request.vertices.offset,
			request.vertices.capacity,
			request.indices.offset,
			request.indices.capacity,
			request.primitives.offset,
			request.primitives.capacity,
			request.materialBase,
			request.materialCount);
	}
	return NRIVoxelComputeGeneratedGeometryStatus::Queued;
}

bool TakeNRIVoxelComputeDirectPublication(uint64_t meshResourceKey, uint64_t generation, NRIVoxelComputeDirectPublishedMesh& outMesh)
{
	VoxelComputeState& state = gVoxelComputeState;
	const uint64_t directKey = BuildDirectPublishKey(meshResourceKey, generation);
	auto found = state.readyDirectPublishedMeshes.find(directKey);
	if (found == state.readyDirectPublishedMeshes.end())
	{
		outMesh = {};
		outMesh.meshResourceKey = meshResourceKey;
		outMesh.generation = generation;
		outMesh.status = state.failedDirectKeys.find(directKey) != state.failedDirectKeys.end() ?
			NRIVoxelComputeGeneratedGeometryStatus::Failed :
			NRIVoxelComputeGeneratedGeometryStatus::Unavailable;
		return false;
	}
	outMesh = found->second;
	state.readyDirectPublishedMeshes.erase(found);
	return true;
}

void CancelNRIVoxelComputeDirectPublication(uint64_t meshResourceKey, uint64_t generation)
{
	VoxelComputeState& state = gVoxelComputeState;
	const uint64_t directKey = BuildDirectPublishKey(meshResourceKey, generation);
	state.queuedDirectKeys.erase(directKey);
	state.pendingDirectKeys.erase(directKey);
	state.readyDirectPublishedMeshes.erase(directKey);
	state.queuedJobs.erase(
		std::remove_if(
			state.queuedJobs.begin(),
			state.queuedJobs.end(),
			[directKey](const PendingVoxelComputeJob& job)
			{
				return job.directPublication && job.directKey == directKey;
			}),
		state.queuedJobs.end());
	if (IsTraceEnabled() && meshResourceKey != 0)
	{
		Printf(
			"PERF pt voxel compute direct publish NRI: action=cancel mesh_resource=0x%llx generation=%llu\n",
			(unsigned long long)meshResourceKey,
			(unsigned long long)generation);
	}
}

void QueueNRIVoxelComputeCountJob(
	FVoxelModel* model,
	const FVoxelRawMeshStats& stats,
	const TArray<FVoxelRawSlabRecord>* slabs,
	const TArray<FVoxelRawFaceRecord>* faces,
	const TArray<FVoxelRawColorRunRecord>* colorRuns,
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
	RawVoxelSourceArchiveEntry* archivedSource = colorRuns != nullptr ? RecordRawSourceArchive(model, stats, *slabs, faces, colorRuns) : nullptr;
	if (archivedSource != nullptr)
	{
		job.slabs = archivedSource->slabs;
		job.faces = archivedSource->faces;
		job.colorRuns = archivedSource->colorRuns;
	}
	else
	{
		CopySlabRecords(*slabs, job.slabs);
		if (faces != nullptr)
		{
			CopyFaceRecords(*faces, job.faces);
		}
		if (colorRuns != nullptr)
		{
			CopyColorRunRecords(*colorRuns, job.colorRuns);
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
	UploadPendingRawSources(services, frameNumber);

	VoxelComputeState& state = gVoxelComputeState;
	if (state.queuedJobs.empty())
	{
		return;
	}

	const bool emit = IsEmitEnabled();
	const bool buildBlas = emit && IsBlasEnabled();
	const bool fullGeneratedReadback = emit && IsFullGeneratedReadbackEnabled();
	const bool directSource = emit && IsDirectGpuPublicationEnabled() && IsRawSourceArchiveEnabled();
	RawVoxelSourceArchiveEntry* directArchive = nullptr;
	std::vector<NRIVoxelComputeJob> gpuJobs;
	std::vector<NRIVoxelComputeSlabRecord> gpuSlabs;
	std::vector<NRIVoxelComputeFaceRecord> gpuFaces;
	std::vector<NRIVoxelComputeColorRunRecord> gpuColorRuns;
	std::vector<PendingReadbackJob> pendingJobs;
	gpuJobs.reserve(state.queuedJobs.size());
	uint32_t slabOffset = 0;
	uint32_t faceOffset = 0;
	uint32_t colorRunOffset = 0;
	uint32_t vertexOffset = 0;
	uint32_t indexOffset = 0;
	uint32_t primitiveOffset = 0;
	uint32_t emittedVertexCount = 0;
	uint32_t emittedIndexCount = 0;
	uint32_t emittedPrimitiveCount = 0;
	bool directOutput = false;
	const NRIBufferResource* outputVertexBuffer = nullptr;
	const NRIBufferResource* outputIndexBuffer = nullptr;
	const NRIBufferResource* outputPrimitiveBuffer = nullptr;
	const size_t jobsToProcess = directSource ? 1u : state.queuedJobs.size();
	for (size_t queuedIndex = 0; queuedIndex < jobsToProcess; ++queuedIndex)
	{
		PendingVoxelComputeJob& queued = state.queuedJobs[queuedIndex];
		if (emit && !queued.directPublication && queued.faces.size() != queued.stats.coalescedFaceCount)
		{
			if (queued.consumeKey != 0)
			{
				state.queuedConsumeKeys.erase(queued.consumeKey);
				state.failedConsumeKeys.insert(queued.consumeKey);
			}
			continue;
		}
		if (directSource)
		{
			directArchive = FindUploadedRawSourceArchive(queued.model);
			if (directArchive == nullptr)
			{
				return;
			}
		}

		NRIVoxelComputeJob gpuJob = {};
		gpuJob.SlabOffset = directSource ? 0u : slabOffset;
		gpuJob.SlabCount = directSource ? (uint32_t)directArchive->slabs.size() : (uint32_t)queued.slabs.size();
		gpuJob.FaceOffset = directSource ? 0u : faceOffset;
		gpuJob.ExpectedFaces = queued.stats.coalescedFaceCount;
		gpuJob.ExpectedIndices = queued.stats.indexCount;
		gpuJob.ExpectedVerticesNoDedupe = queued.stats.noDedupeVertexCount;
		gpuJob.ExpectedVoxels = queued.stats.voxelCount;
		gpuJob.JobId = queued.jobId;
		gpuJob.VertexOffset = queued.directPublication ? queued.vertices.offset : vertexOffset;
		gpuJob.IndexOffset = queued.directPublication ? queued.indices.offset : indexOffset;
		gpuJob.PrimitiveOffset = queued.directPublication ? queued.primitives.offset : primitiveOffset;
		gpuJob.PivotX = queued.stats.pivotX;
		gpuJob.PivotY = queued.stats.pivotY;
		gpuJob.PivotZ = queued.stats.pivotZ;
		gpuJob.MaterialBase = queued.directPublication ? queued.materialBase : 0u;
		gpuJob.VertexCapacity = queued.directPublication ? queued.vertices.capacity : queued.stats.noDedupeVertexCount;
		gpuJob.IndexCapacity = queued.directPublication ? queued.indices.capacity : queued.stats.indexCount;
		gpuJob.PrimitiveCapacity = queued.directPublication ? queued.primitives.capacity : queued.stats.coalescedFaceCount * 2u;
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
		pending.consumeKey = queued.consumeKey;
		pending.directPublication = queued.directPublication;
		pending.directMeshResourceKey = queued.directMeshResourceKey;
		pending.directKey = queued.directKey;
		pending.directGeneration = queued.directGeneration;
		pending.sourceArchiveSerial = directArchive != nullptr ? directArchive->recordSerial : queued.sourceArchiveSerial;
		pending.materialBase = queued.materialBase;
		pending.materialCount = queued.materialCount;
		pending.outputKind = queued.outputKind;
		pending.vertices = queued.vertices;
		pending.indices = queued.indices;
		pending.primitives = queued.primitives;
		pending.admissionState = emit ? VoxelComputeAdmissionState::Emitting : VoxelComputeAdmissionState::Counting;
		pendingJobs.push_back(pending);
		if (queued.consumeKey != 0)
		{
			state.queuedConsumeKeys.erase(queued.consumeKey);
		}
		if (queued.directPublication)
		{
			directOutput = true;
			outputVertexBuffer = queued.outputBuffers.vertices;
			outputIndexBuffer = queued.outputBuffers.indices;
			outputPrimitiveBuffer = queued.outputBuffers.primitives;
			state.queuedDirectKeys.erase(queued.directKey);
			emittedVertexCount += queued.stats.noDedupeVertexCount;
			emittedIndexCount += queued.stats.indexCount;
			emittedPrimitiveCount += queued.stats.coalescedFaceCount * 2u;
		}
		TraceAdmission(frameNumber, pending, pending.admissionState, "dispatch");

		if (!directSource)
		{
			for (NRIVoxelComputeSlabRecord slab : queued.slabs)
			{
				slab.ColorRunOffset += colorRunOffset;
				gpuSlabs.push_back(slab);
			}
			gpuFaces.insert(gpuFaces.end(), queued.faces.begin(), queued.faces.end());
			gpuColorRuns.insert(gpuColorRuns.end(), queued.colorRuns.begin(), queued.colorRuns.end());
		}
		slabOffset += gpuJob.SlabCount;
		faceOffset += gpuJob.ExpectedFaces;
		colorRunOffset += (uint32_t)queued.colorRuns.size();
		if (!queued.directPublication)
		{
			vertexOffset += gpuJob.ExpectedVerticesNoDedupe;
			indexOffset += gpuJob.ExpectedIndices;
			primitiveOffset += gpuJob.ExpectedFaces * 2u;
			emittedVertexCount += gpuJob.ExpectedVerticesNoDedupe;
			emittedIndexCount += gpuJob.ExpectedIndices;
			emittedPrimitiveCount += gpuJob.ExpectedFaces * 2u;
		}
	}
	if (directSource)
	{
		state.queuedJobs.erase(state.queuedJobs.begin());
	}
	else
	{
		state.queuedJobs.clear();
	}

	if (gpuJobs.empty() || (!directSource && gpuSlabs.empty()))
	{
		return;
	}

	const uint64_t jobBytes = (uint64_t)gpuJobs.size() * sizeof(NRIVoxelComputeJob);
	const uint64_t slabBytes = directSource && directArchive != nullptr ? directArchive->slabBytes : (uint64_t)gpuSlabs.size() * sizeof(NRIVoxelComputeSlabRecord);
	const uint64_t faceBytes = directSource ? 0 : (uint64_t)gpuFaces.size() * sizeof(NRIVoxelComputeFaceRecord);
	const uint64_t colorRunBytes = directSource && directArchive != nullptr ? directArchive->colorRunBytes : (uint64_t)gpuColorRuns.size() * sizeof(NRIVoxelComputeColorRunRecord);
	const uint64_t resultBytes = (uint64_t)gpuJobs.size() * sizeof(NRIVoxelComputeResult);
	const uint64_t vertexBytes = (uint64_t)emittedVertexCount * sizeof(NRIVoxelComputeSceneVertex);
	const uint64_t indexBytes = (uint64_t)emittedIndexCount * sizeof(uint32_t);
	const uint64_t primitiveBytes = (uint64_t)emittedPrimitiveCount * sizeof(NRIVoxelComputePrimitiveData);
	if (!EnsureBuffer(services, state.jobUploadBuffer, jobBytes, sizeof(NRIVoxelComputeJob), nri::BufferUsageBits::NONE, nri::MemoryLocation::DEVICE_UPLOAD, nri::BufferView::STRUCTURED_BUFFER, false) ||
		!EnsureBuffer(services, state.jobBuffer, jobBytes, sizeof(NRIVoxelComputeJob), nri::BufferUsageBits::SHADER_RESOURCE, nri::MemoryLocation::DEVICE, nri::BufferView::STRUCTURED_BUFFER, true) ||
		(!directSource &&
			(!EnsureBuffer(services, state.slabUploadBuffer, slabBytes, sizeof(NRIVoxelComputeSlabRecord), nri::BufferUsageBits::NONE, nri::MemoryLocation::DEVICE_UPLOAD, nri::BufferView::STRUCTURED_BUFFER, false) ||
			!EnsureBuffer(services, state.slabBuffer, slabBytes, sizeof(NRIVoxelComputeSlabRecord), nri::BufferUsageBits::SHADER_RESOURCE, nri::MemoryLocation::DEVICE, nri::BufferView::STRUCTURED_BUFFER, true) ||
			!EnsureBuffer(services, state.colorRunUploadBuffer, colorRunBytes, sizeof(NRIVoxelComputeColorRunRecord), nri::BufferUsageBits::NONE, nri::MemoryLocation::DEVICE_UPLOAD, nri::BufferView::STRUCTURED_BUFFER, false) ||
			!EnsureBuffer(services, state.colorRunBuffer, colorRunBytes, sizeof(NRIVoxelComputeColorRunRecord), nri::BufferUsageBits::SHADER_RESOURCE, nri::MemoryLocation::DEVICE, nri::BufferView::STRUCTURED_BUFFER, true))) ||
		!EnsureBuffer(services, state.resultBuffer, resultBytes, sizeof(NRIVoxelComputeResult), nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::MemoryLocation::DEVICE, nri::BufferView::STORAGE_STRUCTURED_BUFFER, true) ||
		!EnsureBuffer(services, state.readbackBuffer, resultBytes, sizeof(NRIVoxelComputeResult), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, nri::BufferView::STRUCTURED_BUFFER, false))
	{
		return;
	}
	if (emit)
	{
		if ((!directSource && gpuFaces.empty()) ||
			(!directSource &&
				(!EnsureBuffer(services, state.faceUploadBuffer, faceBytes, sizeof(NRIVoxelComputeFaceRecord), nri::BufferUsageBits::NONE, nri::MemoryLocation::DEVICE_UPLOAD, nri::BufferView::STRUCTURED_BUFFER, false) ||
				!EnsureBuffer(services, state.faceBuffer, faceBytes, sizeof(NRIVoxelComputeFaceRecord), nri::BufferUsageBits::SHADER_RESOURCE, nri::MemoryLocation::DEVICE, nri::BufferView::STRUCTURED_BUFFER, true))) ||
			(directSource &&
				!EnsureBuffer(services, state.faceBuffer, sizeof(NRIVoxelComputeFaceRecord), sizeof(NRIVoxelComputeFaceRecord), nri::BufferUsageBits::SHADER_RESOURCE, nri::MemoryLocation::DEVICE, nri::BufferView::STRUCTURED_BUFFER, true)) ||
			(!directOutput &&
				(!EnsureBuffer(services, state.vertexBuffer, vertexBytes, sizeof(NRIVoxelComputeSceneVertex), NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), nri::MemoryLocation::DEVICE, nri::BufferView::STORAGE_STRUCTURED_BUFFER, true) ||
				!EnsureBuffer(services, state.indexBuffer, indexBytes, sizeof(uint32_t), NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), nri::MemoryLocation::DEVICE, nri::BufferView::STORAGE_STRUCTURED_BUFFER, true) ||
				!EnsureBuffer(services, state.primitiveBuffer, primitiveBytes, sizeof(NRIVoxelComputePrimitiveData), nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::MemoryLocation::DEVICE, nri::BufferView::STORAGE_STRUCTURED_BUFFER, true))) ||
			(fullGeneratedReadback &&
				(!EnsureBuffer(services, state.vertexReadbackBuffer, vertexBytes, sizeof(NRIVoxelComputeSceneVertex), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, nri::BufferView::STRUCTURED_BUFFER, false) ||
				!EnsureBuffer(services, state.indexReadbackBuffer, indexBytes, sizeof(uint32_t), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, nri::BufferView::STRUCTURED_BUFFER, false) ||
				!EnsureBuffer(services, state.primitiveReadbackBuffer, primitiveBytes, sizeof(NRIVoxelComputePrimitiveData), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, nri::BufferView::STRUCTURED_BUFFER, false))))
		{
			return;
		}
	}

	if (!CopyToUploadBuffer(context, state.jobUploadBuffer, gpuJobs.data(), jobBytes) ||
		(!directSource &&
			(!CopyToUploadBuffer(context, state.slabUploadBuffer, gpuSlabs.data(), slabBytes) ||
			!CopyToUploadBuffer(context, state.colorRunUploadBuffer, gpuColorRuns.data(), colorRunBytes) ||
			(emit && !CopyToUploadBuffer(context, state.faceUploadBuffer, gpuFaces.data(), faceBytes)))))
	{
		return;
	}

	std::vector<nri::BufferBarrierDesc> uploadBarriers;
	uploadBarriers.resize(directSource ? 2 : (emit ? 8 : 6));
	uploadBarriers[0].buffer = state.jobUploadBuffer.buffer;
	uploadBarriers[0].after = NRIResourceCopySourceAccess();
	uploadBarriers[1].buffer = state.jobBuffer.buffer;
	uploadBarriers[1].after = NRIResourceCopyDestinationAccess();
	if (!directSource)
	{
		uploadBarriers[2].buffer = state.slabUploadBuffer.buffer;
		uploadBarriers[2].after = NRIResourceCopySourceAccess();
		uploadBarriers[3].buffer = state.slabBuffer.buffer;
		uploadBarriers[3].after = NRIResourceCopyDestinationAccess();
		uploadBarriers[4].buffer = state.colorRunUploadBuffer.buffer;
		uploadBarriers[4].after = NRIResourceCopySourceAccess();
		uploadBarriers[5].buffer = state.colorRunBuffer.buffer;
		uploadBarriers[5].after = NRIResourceCopyDestinationAccess();
		if (emit)
		{
			uploadBarriers[6].buffer = state.faceUploadBuffer.buffer;
			uploadBarriers[6].after = NRIResourceCopySourceAccess();
			uploadBarriers[7].buffer = state.faceBuffer.buffer;
			uploadBarriers[7].after = NRIResourceCopyDestinationAccess();
		}
	}
	nri::BarrierDesc uploadBarrier = {};
	uploadBarrier.buffers = uploadBarriers.data();
	uploadBarrier.bufferNum = (uint32_t)uploadBarriers.size();
	context.core->CmdBarrier(*context.commandBuffer, uploadBarrier);
	context.core->CmdCopyBuffer(*context.commandBuffer, *state.jobBuffer.buffer, 0, *state.jobUploadBuffer.buffer, 0, jobBytes);
	if (!directSource)
	{
		context.core->CmdCopyBuffer(*context.commandBuffer, *state.slabBuffer.buffer, 0, *state.slabUploadBuffer.buffer, 0, slabBytes);
		context.core->CmdCopyBuffer(*context.commandBuffer, *state.colorRunBuffer.buffer, 0, *state.colorRunUploadBuffer.buffer, 0, colorRunBytes);
		if (emit)
		{
			context.core->CmdCopyBuffer(*context.commandBuffer, *state.faceBuffer.buffer, 0, *state.faceUploadBuffer.buffer, 0, faceBytes);
		}
	}

	const NRIBufferResource& emitVertexBuffer = directOutput ? *outputVertexBuffer : state.vertexBuffer;
	const NRIBufferResource& emitIndexBuffer = directOutput ? *outputIndexBuffer : state.indexBuffer;
	const NRIBufferResource& emitPrimitiveBuffer = directOutput ? *outputPrimitiveBuffer : state.primitiveBuffer;

	std::vector<nri::BufferBarrierDesc> computeBarriers;
	computeBarriers.resize(directSource ? (emit ? 7 : 4) : (emit ? 8 : 4));
	computeBarriers[0].buffer = state.jobBuffer.buffer;
	computeBarriers[0].before = NRIResourceCopyDestinationAccess();
	computeBarriers[0].after = NRIResourceComputeShaderResourceAccess();
	computeBarriers[1].buffer = directSource && directArchive != nullptr ? directArchive->slabBuffer.buffer : state.slabBuffer.buffer;
	computeBarriers[1].before = directSource ? NRIResourceComputeShaderResourceAccess() : NRIResourceCopyDestinationAccess();
	computeBarriers[1].after = NRIResourceComputeShaderResourceAccess();
	computeBarriers[2].buffer = directSource && directArchive != nullptr ? directArchive->colorRunBuffer.buffer : state.colorRunBuffer.buffer;
	computeBarriers[2].before = directSource ? NRIResourceComputeShaderResourceAccess() : NRIResourceCopyDestinationAccess();
	computeBarriers[2].after = NRIResourceComputeShaderResourceAccess();
	computeBarriers[3].buffer = state.resultBuffer.buffer;
	computeBarriers[3].before = NRIResourceCopySourceAccess();
	computeBarriers[3].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	if (emit)
	{
		computeBarriers[4].buffer = emitVertexBuffer.buffer;
		computeBarriers[4].before = directOutput ? NRIResourceComputeShaderResourceAccess() : (buildBlas ? NRIResourceAccelerationStructureBuildInputAccess() : NRIResourceCopySourceAccess());
		computeBarriers[4].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		computeBarriers[5].buffer = emitIndexBuffer.buffer;
		computeBarriers[5].before = directOutput ? NRIResourceComputeShaderResourceAccess() : (buildBlas ? NRIResourceAccelerationStructureBuildInputAccess() : NRIResourceCopySourceAccess());
		computeBarriers[5].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		computeBarriers[6].buffer = emitPrimitiveBuffer.buffer;
		computeBarriers[6].before = directOutput ? NRIResourceComputeShaderResourceAccess() : NRIResourceCopySourceAccess();
		computeBarriers[6].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		if (!directSource)
		{
			computeBarriers[7].buffer = state.faceBuffer.buffer;
			computeBarriers[7].before = NRIResourceCopyDestinationAccess();
			computeBarriers[7].after = NRIResourceComputeShaderResourceAccess();
		}
	}
	nri::BarrierDesc computeBarrier = {};
	computeBarrier.buffers = computeBarriers.data();
	computeBarrier.bufferNum = (uint32_t)computeBarriers.size();
	context.core->CmdBarrier(*context.commandBuffer, computeBarrier);

	const nri::Descriptor* inputDescriptors[2] = {
		state.jobBuffer.shaderView,
		directSource && directArchive != nullptr ? directArchive->slabBuffer.shaderView : state.slabBuffer.shaderView
	};
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = renderer.mVoxelComputeInputSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputDescriptors;
	inputUpdate.descriptorNum = 2;
	context.core->UpdateDescriptorRanges(&inputUpdate, 1);
	if (emit)
	{
		const nri::Descriptor* faceDescriptor[2] = {
			state.faceBuffer.shaderView,
			directSource && directArchive != nullptr ? directArchive->colorRunBuffer.shaderView : state.colorRunBuffer.shaderView
		};
		nri::UpdateDescriptorRangeDesc faceUpdate = {};
		faceUpdate.descriptorSet = renderer.mVoxelComputeInputSet;
		faceUpdate.rangeIndex = 1;
		faceUpdate.descriptors = faceDescriptor;
		faceUpdate.descriptorNum = 2;
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
		const nri::Descriptor* emitDescriptors[3] = {
			directOutput ? emitVertexBuffer.storageView : emitVertexBuffer.shaderView,
			directOutput ? emitIndexBuffer.storageView : emitIndexBuffer.shaderView,
			directOutput ? emitPrimitiveBuffer.storageView : emitPrimitiveBuffer.shaderView
		};
		nri::UpdateDescriptorRangeDesc emitUpdate = {};
		emitUpdate.descriptorSet = renderer.mVoxelComputeOutputSet;
		emitUpdate.rangeIndex = 1;
		emitUpdate.descriptors = emitDescriptors;
		emitUpdate.descriptorNum = 3;
		context.core->UpdateDescriptorRanges(&emitUpdate, 1);
	}

	NRIVoxelComputeConstants constants = {};
	constants.JobCount = (uint32_t)gpuJobs.size();
	constants.SlabRecordCount = directSource && directArchive != nullptr ? (uint32_t)directArchive->slabs.size() : (uint32_t)gpuSlabs.size();
	constants.FaceRecordCount = directSource ? 0u : (uint32_t)gpuFaces.size();
	constants.ColorRunRecordCount = directSource && directArchive != nullptr ? (uint32_t)directArchive->colorRuns.size() : 0u;
	context.core->CmdSetPipelineLayout(*context.commandBuffer, nri::BindPoint::COMPUTE, *renderer.mVoxelComputePipelineLayout);
	context.core->CmdSetRootConstants(*context.commandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	context.core->CmdSetDescriptorSet(*context.commandBuffer, { 0, renderer.mVoxelComputeInputSet, nri::BindPoint::COMPUTE });
	context.core->CmdSetDescriptorSet(*context.commandBuffer, { 1, renderer.mVoxelComputeOutputSet, nri::BindPoint::COMPUTE });
	context.core->CmdSetPipeline(*context.commandBuffer, *renderer.GetPipeline(emit ? NRIRenderer::PipelineSlot::VoxelComputeEmit : NRIRenderer::PipelineSlot::VoxelComputeCount));
	context.core->CmdDispatch(*context.commandBuffer, { (uint32_t)gpuJobs.size(), 1, 1 });

	std::vector<nri::BufferBarrierDesc> readbackBarriers;
	readbackBarriers.resize((fullGeneratedReadback ? 4 : 1) + (directOutput ? 3 : 0));
	readbackBarriers[0].buffer = state.resultBuffer.buffer;
	readbackBarriers[0].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	readbackBarriers[0].after = NRIResourceCopySourceAccess();
	if (fullGeneratedReadback)
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
	if (directOutput)
	{
		const uint32_t directBarrierOffset = fullGeneratedReadback ? 4u : 1u;
		readbackBarriers[directBarrierOffset + 0u].buffer = emitVertexBuffer.buffer;
		readbackBarriers[directBarrierOffset + 0u].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		readbackBarriers[directBarrierOffset + 0u].after = NRIResourceComputeShaderResourceAccess();
		readbackBarriers[directBarrierOffset + 1u].buffer = emitIndexBuffer.buffer;
		readbackBarriers[directBarrierOffset + 1u].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		readbackBarriers[directBarrierOffset + 1u].after = NRIResourceComputeShaderResourceAccess();
		readbackBarriers[directBarrierOffset + 2u].buffer = emitPrimitiveBuffer.buffer;
		readbackBarriers[directBarrierOffset + 2u].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		readbackBarriers[directBarrierOffset + 2u].after = NRIResourceComputeShaderResourceAccess();
	}
	nri::BarrierDesc readbackBarrierDesc = {};
	readbackBarrierDesc.buffers = readbackBarriers.data();
	readbackBarrierDesc.bufferNum = (uint32_t)readbackBarriers.size();
	context.core->CmdBarrier(*context.commandBuffer, readbackBarrierDesc);
	context.core->CmdCopyBuffer(*context.commandBuffer, *state.readbackBuffer.buffer, 0, *state.resultBuffer.buffer, 0, resultBytes);
	if (fullGeneratedReadback)
	{
		context.core->CmdCopyBuffer(*context.commandBuffer, *state.vertexReadbackBuffer.buffer, 0, *state.vertexBuffer.buffer, 0, vertexBytes);
		context.core->CmdCopyBuffer(*context.commandBuffer, *state.indexReadbackBuffer.buffer, 0, *state.indexBuffer.buffer, 0, indexBytes);
		context.core->CmdCopyBuffer(*context.commandBuffer, *state.primitiveReadbackBuffer.buffer, 0, *state.primitiveBuffer.buffer, 0, primitiveBytes);
	}

	constexpr uint32_t MaxDiagnosticBlasPrimitives = 8192;
	const bool allowDiagnosticBlas =
		buildBlas &&
		!directOutput &&
		state.diagnosticBlasBuildsSubmitted == 0 &&
		vertexOffset > 0 &&
		indexOffset > 0 &&
		primitiveOffset > 0 &&
		primitiveOffset <= MaxDiagnosticBlasPrimitives;
	if (allowDiagnosticBlas)
	{
		nri::BufferBarrierDesc blasBarriers[2] = {};
		blasBarriers[0].buffer = state.vertexBuffer.buffer;
		blasBarriers[0].before = fullGeneratedReadback ? NRIResourceCopySourceAccess() : nri::AccessStage{ nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		blasBarriers[0].after = NRIResourceAccelerationStructureBuildInputAccess();
		blasBarriers[1].buffer = state.indexBuffer.buffer;
		blasBarriers[1].before = fullGeneratedReadback ? NRIResourceCopySourceAccess() : nri::AccessStage{ nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
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
			0u,
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
				"PERF pt voxel compute blas NRI: frame=%llu jobs=%u success=%u vertices=%u indices=%u primitives=%u blas_bytes=%llu scratch_bytes=%llu direct_source=%u full_readback=%u\n",
				(unsigned long long)frameNumber,
				(unsigned)gpuJobs.size(),
				blasOk ? 1u : 0u,
				vertexOffset,
				indexOffset,
				primitiveOffset,
				(unsigned long long)state.diagnosticBlas.memorySize,
				(unsigned long long)state.diagnosticBlas.buildScratchSize,
				directSource ? 1u : 0u,
				fullGeneratedReadback ? 1u : 0u);
		}
		if (blasOk)
		{
			state.diagnosticBlasBuildsSubmitted++;
		}
	}
	else if (buildBlas && IsTraceEnabled())
	{
		Printf(
			"PERF pt voxel compute blas NRI: frame=%llu jobs=%u success=0 skipped=1 reason=%s vertices=%u indices=%u primitives=%u max_primitives=%u submitted=%u direct_source=%u full_readback=%u\n",
			(unsigned long long)frameNumber,
			(unsigned)gpuJobs.size(),
			state.diagnosticBlasBuildsSubmitted != 0 ? "single_build_budget" : "primitive_budget",
			vertexOffset,
			indexOffset,
			primitiveOffset,
			MaxDiagnosticBlasPrimitives,
			state.diagnosticBlasBuildsSubmitted,
			directSource ? 1u : 0u,
			fullGeneratedReadback ? 1u : 0u);
	}

	state.pendingFrame = frameNumber;
	state.pendingReadbackValid = true;
	state.pendingEmit = emit;
	state.pendingBlas = buildBlas;
	state.pendingFullGeneratedReadback = fullGeneratedReadback;
	for (const PendingReadbackJob& job : pendingJobs)
	{
		if (job.consumeKey != 0)
		{
			state.pendingConsumeKeys.insert(job.consumeKey);
		}
		if (job.directPublication)
		{
			state.pendingDirectKeys.insert(job.directKey);
		}
	}
	state.pendingReadbackJobs = std::move(pendingJobs);
	state.pendingVertexCount = emittedVertexCount;
	state.pendingIndexCount = emittedIndexCount;
	state.pendingPrimitiveCount = emittedPrimitiveCount;
	if (IsTraceEnabled())
	{
		Printf(
			"PERF pt voxel compute dispatch NRI: frame=%llu mode=%s source=%s jobs=%u slab_records=%u color_run_records=%u face_records=%u job_bytes=%llu slab_bytes=%llu color_run_bytes=%llu face_bytes=%llu result_bytes=%llu vertex_bytes=%llu index_bytes=%llu primitive_bytes=%llu production_readback_bytes=%llu validation_readback_bytes=%llu direct_gpu=%u raw_archive=%u direct_publish=%u\n",
			(unsigned long long)frameNumber,
			emit ? (buildBlas ? "emit_blas" : "emit") : "count",
			directSource ? "archive_decode" : (emit ? "face_records" : "slab_count"),
			(unsigned)gpuJobs.size(),
			directSource && directArchive != nullptr ? (uint32_t)directArchive->slabs.size() : (uint32_t)gpuSlabs.size(),
			directSource && directArchive != nullptr ? (uint32_t)directArchive->colorRuns.size() : 0u,
			directSource ? 0u : (uint32_t)gpuFaces.size(),
			(unsigned long long)jobBytes,
			(unsigned long long)slabBytes,
			(unsigned long long)colorRunBytes,
			(unsigned long long)faceBytes,
			(unsigned long long)resultBytes,
			(unsigned long long)vertexBytes,
			(unsigned long long)indexBytes,
			(unsigned long long)primitiveBytes,
			(unsigned long long)resultBytes,
			(unsigned long long)(fullGeneratedReadback ? vertexBytes + indexBytes + primitiveBytes : 0),
			IsDirectGpuPublicationEnabled() ? 1u : 0u,
			IsRawSourceArchiveEnabled() ? 1u : 0u,
			directOutput ? 1u : 0u);
	}
}

void DestroyNRIVoxelComputeMeshingDiagnostics(NRIRenderer& renderer)
{
	NRIResourceServices services = renderer.BuildResourceServices();
	VoxelComputeState& state = gVoxelComputeState;
	services.DestroyBufferResource(state.jobUploadBuffer);
	services.DestroyBufferResource(state.slabUploadBuffer);
	services.DestroyBufferResource(state.faceUploadBuffer);
	services.DestroyBufferResource(state.colorRunUploadBuffer);
	services.DestroyBufferResource(state.jobBuffer);
	services.DestroyBufferResource(state.slabBuffer);
	services.DestroyBufferResource(state.faceBuffer);
	services.DestroyBufferResource(state.colorRunBuffer);
	services.DestroyBufferResource(state.resultBuffer);
	services.DestroyBufferResource(state.vertexBuffer);
	services.DestroyBufferResource(state.indexBuffer);
	services.DestroyBufferResource(state.primitiveBuffer);
	services.DestroyBufferResource(state.readbackBuffer);
	services.DestroyBufferResource(state.vertexReadbackBuffer);
	services.DestroyBufferResource(state.indexReadbackBuffer);
	services.DestroyBufferResource(state.primitiveReadbackBuffer);
	for (auto& archivePair : state.rawSourceArchive)
	{
		RawVoxelSourceArchiveEntry& entry = archivePair.second;
		services.DestroyBufferResource(entry.slabUploadBuffer);
		services.DestroyBufferResource(entry.colorRunUploadBuffer);
		services.DestroyBufferResource(entry.slabBuffer);
		services.DestroyBufferResource(entry.colorRunBuffer);
	}
	renderer.DestroyAccelerationStructureResource(state.diagnosticBlas);
	state.queuedJobs.clear();
	state.pendingReadbackJobs.clear();
	state.pendingFrame = 0;
	state.pendingReadbackValid = false;
	state.pendingEmit = false;
	state.pendingBlas = false;
	state.pendingFullGeneratedReadback = false;
	state.pendingVertexCount = 0;
	state.pendingIndexCount = 0;
	state.pendingPrimitiveCount = 0;
	state.diagnosticBlasBuildsSubmitted = 0;
	state.nextRawSourceSerial = 1;
	state.rawSourceArchiveHits = 0;
	state.rawSourceArchiveMisses = 0;
	state.rawSourceArchiveRecords = 0;
	state.rawSourceArchiveUploadBytes = 0;
	state.rawSourceArchiveUploadFailures = 0;
	state.queuedConsumeKeys.clear();
	state.pendingConsumeKeys.clear();
	state.failedConsumeKeys.clear();
	state.queuedDirectKeys.clear();
	state.pendingDirectKeys.clear();
	state.failedDirectKeys.clear();
	state.readyGeneratedGeometry.clear();
	state.readyDirectPublishedMeshes.clear();
	state.rawSourceArchive.clear();
}
