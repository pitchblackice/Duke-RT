#include "nri_acceleration.h"
#include "nri_cvars.h"

#include "nri_renderer.h"
#include "nri_diagnostic_names.h"
#include "nri_persistent_voxel_services.h"
#include "nri_scene_upload.h"
#include "nri_shader_contracts.h"
#include "../scene/nri_hash.h"
#include "../system/nri_renderdevice.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_map>


namespace
{
	double DurationMs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	bool ShouldCollectAccelerationPerfTiming()
	{
		return ((int)perf_looptraceframes > 0 || (!!nri_pttemporaltrace && (int)nri_pttraceframes > 0)) || (bool)nri_ptslowdowntrace;
	}

	class ScopedPtPerfTimer
	{
	public:
		explicit ScopedPtPerfTimer(double& targetMs)
			: mTarget(ShouldCollectAccelerationPerfTiming() ? &targetMs : nullptr)
		{
			if (mTarget != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedPtPerfTimer()
		{
			if (mTarget != nullptr)
			{
				*mTarget += DurationMs(mStart, std::chrono::steady_clock::now());
			}
		}

	private:
		double* mTarget = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};

	uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	uint64_t BuildEmissiveTlasInstancePayloadHash(const std::vector<nri::TopLevelInstance>& instances)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, (uint64_t)instances.size());
		for (const nri::TopLevelInstance& instance : instances)
		{
			hash = nri_scene::HashCombine64(hash, (uint64_t)instance.instanceId);
			hash = nri_scene::HashCombine64(hash, (uint64_t)instance.mask);
			hash = nri_scene::HashCombine64(hash, (uint64_t)instance.shaderBindingTableLocalOffset);
			hash = nri_scene::HashCombine64(hash, (uint64_t)instance.flags);
			hash = nri_scene::HashCombine64(hash, instance.accelerationStructureHandle);
			for (uint32_t row = 0; row < 3; ++row)
			{
				hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(instance.transform[row][0]));
				hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(instance.transform[row][1]));
				hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(instance.transform[row][2]));
				hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(instance.transform[row][3]));
			}
		}

		return hash;
	}
}

bool NRIAccelerationStructureManager::BuildDynamic(NRIRenderer& renderer, const nri_scene::GeometryData& geometry)
{
	return BuildDynamic(
		renderer,
		geometry,
		0u,
		(uint32_t)geometry.indices.size(),
		(uint32_t)geometry.primitives.size(),
		renderer.GetCurrentDynamicBottomLevelAS(),
		true);
}

bool NRIAccelerationStructureManager::BuildDynamic(
	NRIRenderer& renderer,
	const nri_scene::GeometryData& geometry,
	uint32_t indexOffset,
	uint32_t indexCount,
	uint32_t primitiveCount,
	NRIAccelerationStructureResource& outAccelerationStructure,
	bool updateDynamicPerfStats)
{
	if (indexOffset > geometry.indices.size() || indexOffset + indexCount > geometry.indices.size())
	{
		return false;
	}
	return BuildBottomLevel(
		renderer,
		renderer.GetCurrentDynamicVertexBuffer(),
		renderer.GetCurrentDynamicIndexBuffer(),
		(uint32_t)geometry.vertices.size(),
		indexOffset,
		indexCount,
		primitiveCount,
		outAccelerationStructure,
		updateDynamicPerfStats);
}

bool NRIAccelerationStructureManager::BuildBottomLevel(
	NRIRenderer& renderer,
	const NRIBufferResource& vertexBuffer,
	const NRIBufferResource& indexBuffer,
	uint32_t vertexCount,
	uint32_t indexOffset,
	uint32_t indexCount,
	uint32_t primitiveCount,
	NRIAccelerationStructureResource& outAccelerationStructure,
	bool updateDynamicPerfStats)
{
	Clocker clock(NriPTAcceleration);
	ScopedPtPerfTimer perfTimer(renderer.mLastPerfShellTraceStats.dynamicAsMs);
	if (updateDynamicPerfStats)
	{
		renderer.mLastPerfShellTraceStats.dynamicAsPrimitiveCount = primitiveCount;
		renderer.mLastPerfShellTraceStats.dynamicAsVertexCount = vertexCount;
		renderer.mLastPerfShellTraceStats.dynamicAsIndexCount = indexCount;
	}
	if (primitiveCount == 0 || vertexCount == 0 || indexCount == 0 || vertexBuffer.buffer == nullptr || indexBuffer.buffer == nullptr)
	{
		return false;
	}

	nri::BottomLevelGeometryDesc dynamicGeometryDesc = {};
	bool reuseAccelerationStructure = false;
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.dynamicAsSetupMs);
		dynamicGeometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
		dynamicGeometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
		dynamicGeometryDesc.triangles.vertexBuffer = vertexBuffer.buffer;
		dynamicGeometryDesc.triangles.vertexOffset = 0;
		dynamicGeometryDesc.triangles.vertexNum = vertexCount;
		dynamicGeometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
		dynamicGeometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
		dynamicGeometryDesc.triangles.indexBuffer = indexBuffer.buffer;
		dynamicGeometryDesc.triangles.indexOffset = (uint64_t)indexOffset * sizeof(uint32_t);
		dynamicGeometryDesc.triangles.indexNum = indexCount;
		dynamicGeometryDesc.triangles.indexType = nri::IndexType::UINT32;

		reuseAccelerationStructure =
			updateDynamicPerfStats &&
			outAccelerationStructure.accelerationStructure != nullptr &&
			outAccelerationStructure.buildVertexBuffer == vertexBuffer.buffer &&
			outAccelerationStructure.buildIndexBuffer == indexBuffer.buffer &&
			outAccelerationStructure.buildVertexCount == vertexCount &&
			outAccelerationStructure.buildIndexOffset == indexOffset &&
			outAccelerationStructure.buildIndexCount == indexCount &&
			outAccelerationStructure.buildPrimitiveCount == primitiveCount;
	}

	if (reuseAccelerationStructure)
	{
		if (updateDynamicPerfStats)
		{
			renderer.mLastPerfShellTraceStats.dynamicAsReuseCount++;
		}
	}
	else
	{
		renderer.RetireResidentAccelerationStructure(outAccelerationStructure);
	}

	nri::AccelerationStructureDesc blasDesc = {};
	blasDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
	blasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_BUILD;
	blasDesc.geometryOrInstanceNum = 1;
	blasDesc.geometries = &dynamicGeometryDesc;
	const bool createdAs = reuseAccelerationStructure || [&]()
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.dynamicAsCreateMs);
		if (updateDynamicPerfStats)
		{
			renderer.mLastPerfShellTraceStats.dynamicAsCreateCalls++;
		}
		return renderer.mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*renderer.mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, blasDesc, outAccelerationStructure.accelerationStructure) == nri::Result::SUCCESS;
	}();
	if (!createdAs)
	{
		return false;
	}

	if (!reuseAccelerationStructure || outAccelerationStructure.memorySize == 0)
	{
		nri::MemoryDesc memoryDesc = {};
		renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(*outAccelerationStructure.accelerationStructure, nri::MemoryLocation::DEVICE, memoryDesc);
		outAccelerationStructure.memorySize = memoryDesc.size;
		outAccelerationStructure.memoryLocation = nri::MemoryLocation::DEVICE;
	}
	if (updateDynamicPerfStats)
	{
		renderer.mLastPerfShellTraceStats.dynamicAsMemoryBytes = outAccelerationStructure.memorySize;
	}

	uint64_t requiredScratchSize = updateDynamicPerfStats ? outAccelerationStructure.buildScratchSize : 0;
	if (requiredScratchSize == 0)
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.dynamicAsScratchMs);
		if (updateDynamicPerfStats)
		{
			renderer.mLastPerfShellTraceStats.dynamicAsScratchQueries++;
		}
		requiredScratchSize = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*outAccelerationStructure.accelerationStructure);
		if (updateDynamicPerfStats)
		{
			outAccelerationStructure.buildScratchSize = requiredScratchSize;
		}
	}
	if (updateDynamicPerfStats)
	{
		renderer.mLastPerfShellTraceStats.dynamicAsScratchRequestedBytes = requiredScratchSize;
	}
	if (renderer.mScratchBuffer.buffer == nullptr || renderer.mScratchBuffer.size < requiredScratchSize)
	{
		renderer.DestroyBufferResource(renderer.mScratchBuffer);
		{
			ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.dynamicAsScratchMs);
			if (updateDynamicPerfStats)
			{
				renderer.mLastPerfShellTraceStats.dynamicAsScratchGrowCount++;
			}
			if (!renderer.CreateBufferWithoutView(renderer.mScratchBuffer, requiredScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
			{
				return false;
			}
		}
	}

	nri::BuildBottomLevelAccelerationStructureDesc dynamicBuild = {};
	dynamicBuild.dst = outAccelerationStructure.accelerationStructure;
	dynamicBuild.geometries = &dynamicGeometryDesc;
	dynamicBuild.geometryNum = 1;
	dynamicBuild.scratchBuffer = renderer.mScratchBuffer.buffer;
	dynamicBuild.scratchOffset = 0;
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.dynamicAsBuildMs);
		renderer.mFrameBuffer->mRayTracing.CmdBuildBottomLevelAccelerationStructures(*renderer.mFrameBuffer->mCommandBuffer, &dynamicBuild, 1);
	}

	nri::BufferBarrierDesc barriers[2] = {};
	barriers[0].buffer = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*outAccelerationStructure.accelerationStructure);
	barriers[0].before = NRIResourceAccelerationStructureWriteAccess();
	barriers[0].after = NRIResourceAccelerationStructureReadAccess();
	barriers[1].buffer = renderer.mScratchBuffer.buffer;
	barriers[1].before = NRIResourceAccelerationStructureScratchAccess();
	barriers[1].after = NRIResourceAccelerationStructureScratchAccess();

	nri::BarrierDesc barrierDesc = {};
	barrierDesc.buffers = barriers;
	barrierDesc.bufferNum = 2;
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.dynamicAsBarrierMs);
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, barrierDesc);
	}
	renderer.mBuiltDynamicSceneASLastFrame = true;
	if (updateDynamicPerfStats)
	{
		outAccelerationStructure.buildVertexBuffer = vertexBuffer.buffer;
		outAccelerationStructure.buildIndexBuffer = indexBuffer.buffer;
		outAccelerationStructure.buildVertexCount = vertexCount;
		outAccelerationStructure.buildIndexOffset = indexOffset;
		outAccelerationStructure.buildIndexCount = indexCount;
		outAccelerationStructure.buildPrimitiveCount = primitiveCount;
		outAccelerationStructure.buildScratchSize = requiredScratchSize;
		renderer.mDynamicSceneLastFrame.asBuildCount++;
	}
	return true;
}

bool NRIAccelerationStructureManager::BuildEmissiveTopLevel(NRIRenderer& renderer)
{
	ScopedPtPerfTimer perfTimer(renderer.mLastPerfShellTraceStats.emissiveTlasMs);
	renderer.mEmissiveTlasInstanceCount = 0;
	renderer.mEmissiveTlasStaticInstanceCount = 0;
	renderer.mEmissiveTlasDynamicInstanceCount = 0;
	renderer.mLastPerfShellTraceStats.emissiveAsEnabled = (bool)nri_ptemissivetlas;
	renderer.mLastPerfShellTraceStats.emissiveAsRecords = (uint32_t)renderer.mBoundEmissivePrimitiveRecords.size();
	for (const NRIRenderer::EmissivePrimitiveDebugRecord& record : renderer.mBoundEmissivePrimitiveRecords)
	{
		if (record.dataSource == nri_diag::SceneDataSourceStatic)
		{
			renderer.mLastPerfShellTraceStats.emissiveAsRecordsStatic++;
		}
		else if (record.dataSource == nri_diag::SceneDataSourceDynamic)
		{
			renderer.mLastPerfShellTraceStats.emissiveAsRecordsDynamic++;
		}
		else if (record.dataSource == nri_diag::SceneDataSourcePersistentVoxel)
		{
			renderer.mLastPerfShellTraceStats.emissiveAsRecordsPersistentVoxel++;
		}
	}

	if (!nri_ptemissivetlas ||
		renderer.mBoundEmissivePrimitiveRecords.empty() ||
		renderer.mBoundSceneInstances.empty())
	{
		renderer.DestroyBufferResource(renderer.mEmissiveTlasInstanceBuffer);
		renderer.RetireTopLevelAccelerationStructure(renderer.mEmissiveTopLevelAS);
		renderer.mEmissiveTlasInstancePayloadCacheValid = false;
		renderer.mEmissiveTlasInstancePayloadHash = 0;
		return true;
	}

	std::unordered_map<uint32_t, uint32_t> staticSceneInstanceByPrimitiveOffset;
	staticSceneInstanceByPrimitiveOffset.reserve(renderer.mBoundSceneInstances.size());
	uint32_t dynamicSceneInstanceIndex = UINT32_MAX;
	for (uint32_t sceneInstanceIndex = 0; sceneInstanceIndex < (uint32_t)renderer.mBoundSceneInstances.size(); ++sceneInstanceIndex)
	{
		const SceneInstanceData& sceneInstance = renderer.mBoundSceneInstances[sceneInstanceIndex];
		if (sceneInstance.dataSource == nri_diag::SceneDataSourceStatic)
		{
			staticSceneInstanceByPrimitiveOffset.emplace(sceneInstance.primitiveBase, sceneInstanceIndex);
		}
		else if (sceneInstance.dataSource == nri_diag::SceneDataSourceDynamic && dynamicSceneInstanceIndex == UINT32_MAX)
		{
			dynamicSceneInstanceIndex = sceneInstanceIndex;
		}
	}

	std::vector<uint8_t> emissiveStaticChunks(renderer.mStaticMapScene.chunks.size(), 0u);
	bool includeDynamicInstance = false;
	const NRIAccelerationStructureResource* dynamicBottomLevelAS = &renderer.GetCurrentDynamicBottomLevelAS();
	const auto findStaticChunkIndexForPrimitive = [&](uint32_t primitiveIndex) -> int32_t
	{
		for (uint32_t chunkIndex = 0; chunkIndex < (uint32_t)renderer.mStaticMapScene.chunks.size(); ++chunkIndex)
		{
			const auto& chunk = renderer.mStaticMapScene.chunks[chunkIndex];
			if (!chunk.active)
			{
				continue;
			}
			const uint32_t chunkBegin = chunk.primitiveOffset;
			const uint32_t chunkEnd = chunkBegin + chunk.primitiveCount;
			if (primitiveIndex >= chunkBegin && primitiveIndex < chunkEnd)
			{
				return (int32_t)chunkIndex;
			}
		}

		return -1;
	};

	for (const NRIRenderer::EmissivePrimitiveDebugRecord& record : renderer.mBoundEmissivePrimitiveRecords)
	{
		if (record.dataSource == nri_diag::SceneDataSourceStatic)
		{
			const int32_t chunkIndex = findStaticChunkIndexForPrimitive(record.primitiveIndex);
			if (chunkIndex >= 0)
			{
				emissiveStaticChunks[(size_t)chunkIndex] = 1u;
				renderer.mLastPerfShellTraceStats.emissiveAsStaticRecordMatchedChunks++;
			}
			else
			{
				renderer.mLastPerfShellTraceStats.emissiveAsStaticRecordUnmatchedChunks++;
			}
		}
		else if (record.dataSource == nri_diag::SceneDataSourceDynamic &&
			dynamicSceneInstanceIndex != UINT32_MAX &&
			dynamicBottomLevelAS != nullptr &&
			dynamicBottomLevelAS->accelerationStructure != nullptr)
		{
			renderer.mLastPerfShellTraceStats.emissiveAsDynamicRecordCount++;
			includeDynamicInstance = true;
		}
		else if (record.dataSource == nri_diag::SceneDataSourcePersistentVoxel)
		{
			renderer.mLastPerfShellTraceStats.emissiveAsPersistentVoxelIgnoredRecords++;
		}
	}

	std::vector<nri::TopLevelInstance> instances;
	instances.reserve(renderer.mStaticMapScene.chunks.size() + (includeDynamicInstance ? 1u : 0u));
	for (size_t chunkIndex = 0; chunkIndex < renderer.mStaticMapScene.chunks.size(); ++chunkIndex)
	{
		if (emissiveStaticChunks[chunkIndex] == 0u)
		{
			continue;
		}

		const auto& chunk = renderer.mStaticMapScene.chunks[chunkIndex];
		if (!chunk.active || chunk.accelerationStructure.accelerationStructure == nullptr)
		{
			continue;
		}

		const auto sceneInstanceIt = staticSceneInstanceByPrimitiveOffset.find(chunk.primitiveOffset);
		if (sceneInstanceIt == staticSceneInstanceByPrimitiveOffset.end())
		{
			continue;
		}

		nri::TopLevelInstance instance = {};
		instance.transform[0][0] = 1.0f;
		instance.transform[1][1] = 1.0f;
		instance.transform[2][2] = 1.0f;
		instance.instanceId = sceneInstanceIt->second;
		instance.mask = NRI_TLAS_MASK_ALL_WORKLOADS;
		instance.shaderBindingTableLocalOffset = 0;
		instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
		instance.accelerationStructureHandle = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*chunk.accelerationStructure.accelerationStructure);
		instances.push_back(instance);
		renderer.mEmissiveTlasStaticInstanceCount++;
		renderer.mLastPerfShellTraceStats.emissiveAsStaticChunkRefs++;
		if (instance.mask == NRI_TLAS_MASK_ALL_WORKLOADS)
		{
			renderer.mLastPerfShellTraceStats.emissiveAsMaskAllWorkloadsRefs++;
		}
		else
		{
			renderer.mLastPerfShellTraceStats.emissiveAsMaskOtherRefs++;
		}
	}

	if (includeDynamicInstance)
	{
		nri::TopLevelInstance instance = {};
		instance.transform[0][0] = 1.0f;
		instance.transform[1][1] = 1.0f;
		instance.transform[2][2] = 1.0f;
		instance.instanceId = dynamicSceneInstanceIndex;
		instance.mask = NRI_TLAS_MASK_ALL_WORKLOADS;
		instance.shaderBindingTableLocalOffset = 0;
		instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
		instance.accelerationStructureHandle = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*dynamicBottomLevelAS->accelerationStructure);
		instances.push_back(instance);
		renderer.mEmissiveTlasDynamicInstanceCount = 1;
		renderer.mLastPerfShellTraceStats.emissiveAsDynamicAggregateRefs = 1;
		if (instance.mask == NRI_TLAS_MASK_ALL_WORKLOADS)
		{
			renderer.mLastPerfShellTraceStats.emissiveAsMaskAllWorkloadsRefs++;
		}
		else
		{
			renderer.mLastPerfShellTraceStats.emissiveAsMaskOtherRefs++;
		}
	}

	if (instances.empty())
	{
		renderer.DestroyBufferResource(renderer.mEmissiveTlasInstanceBuffer);
		renderer.RetireTopLevelAccelerationStructure(renderer.mEmissiveTopLevelAS);
		renderer.mEmissiveTlasInstancePayloadCacheValid = false;
		renderer.mEmissiveTlasInstancePayloadHash = 0;
		return true;
	}

	const uint64_t payloadHash = BuildEmissiveTlasInstancePayloadHash(instances);
	if (renderer.mEmissiveTlasInstancePayloadCacheValid &&
		renderer.mEmissiveTlasInstancePayloadHash == payloadHash &&
		renderer.mEmissiveTlasInstanceBuffer.buffer != nullptr &&
		renderer.mEmissiveTopLevelAS.accelerationStructure != nullptr)
	{
		renderer.mEmissiveTlasInstanceCount = (uint32_t)instances.size();
		renderer.mLastPerfShellTraceStats.emissiveAsPayloadCacheHits++;
		return true;
	}
	renderer.mLastPerfShellTraceStats.emissiveAsPayloadCacheMisses++;

	renderer.RetireTopLevelAccelerationStructure(renderer.mEmissiveTopLevelAS);
	if (!renderer.EnsureStructuredBuffer(
		renderer.mEmissiveTlasInstanceBuffer,
		renderer.mEmissiveTlasInstanceBufferStats,
		instances.data(),
		instances.size() * sizeof(nri::TopLevelInstance),
		sizeof(nri::TopLevelInstance),
		nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
		NRIResourceAccelerationStructureBuildInputAccess(),
		false,
		"emissive_tlas_instance_upload"))
	{
		return false;
	}

	nri::AccelerationStructureDesc tlasDesc = {};
	tlasDesc.type = nri::AccelerationStructureType::TOP_LEVEL;
	tlasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
	tlasDesc.geometryOrInstanceNum = (uint32_t)instances.size();
	if (renderer.mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*renderer.mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, tlasDesc, renderer.mEmissiveTopLevelAS.accelerationStructure) != nri::Result::SUCCESS)
	{
		return false;
	}

	{
		nri::MemoryDesc memoryDesc = {};
		renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(*renderer.mEmissiveTopLevelAS.accelerationStructure, nri::MemoryLocation::DEVICE, memoryDesc);
		renderer.mEmissiveTopLevelAS.memorySize = memoryDesc.size;
		renderer.mEmissiveTopLevelAS.memoryLocation = nri::MemoryLocation::DEVICE;
	}

	const uint64_t requiredScratchSize = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*renderer.mEmissiveTopLevelAS.accelerationStructure);
	if (renderer.mEmissiveTopLevelScratchBuffer.buffer == nullptr || renderer.mEmissiveTopLevelScratchBuffer.size < requiredScratchSize)
	{
		if (renderer.mEmissiveTopLevelScratchBuffer.buffer != nullptr)
		{
			renderer.WaitForCommandsTracked("emissive_tlas_scratch_resize");
		}
		renderer.DestroyBufferResource(renderer.mEmissiveTopLevelScratchBuffer);
		if (!renderer.CreateBufferWithoutView(renderer.mEmissiveTopLevelScratchBuffer, requiredScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
		{
			return false;
		}
	}

	if (renderer.mFrameBuffer->mRayTracing.CreateAccelerationStructureDescriptor(*renderer.mEmissiveTopLevelAS.accelerationStructure, renderer.mEmissiveTopLevelAS.descriptor) != nri::Result::SUCCESS)
	{
		return false;
	}

	nri::BuildTopLevelAccelerationStructureDesc tlasBuild = {};
	tlasBuild.dst = renderer.mEmissiveTopLevelAS.accelerationStructure;
	tlasBuild.instanceNum = (uint32_t)instances.size();
	tlasBuild.instanceBuffer = renderer.mEmissiveTlasInstanceBuffer.buffer;
	tlasBuild.instanceOffset = 0;
	tlasBuild.scratchBuffer = renderer.mEmissiveTopLevelScratchBuffer.buffer;
	tlasBuild.scratchOffset = 0;
	renderer.mFrameBuffer->mRayTracing.CmdBuildTopLevelAccelerationStructures(*renderer.mFrameBuffer->mCommandBuffer, &tlasBuild, 1);

	nri::BufferBarrierDesc tlasBarrier = {};
	tlasBarrier.buffer = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*renderer.mEmissiveTopLevelAS.accelerationStructure);
	tlasBarrier.before = NRIResourceAccelerationStructureWriteAccess();
	tlasBarrier.after = NRIResourceComputeAccelerationStructureReadAccess();

	nri::BarrierDesc barrierDesc = {};
	barrierDesc.buffers = &tlasBarrier;
	barrierDesc.bufferNum = 1;
	renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, barrierDesc);

	renderer.mEmissiveTlasInstanceCount = (uint32_t)instances.size();
	renderer.mEmissiveTlasBuildCount++;
	renderer.mEmissiveTlasInstancePayloadCacheValid = true;
	renderer.mEmissiveTlasInstancePayloadHash = payloadHash;
	return true;
}

bool NRIAccelerationStructureManager::BuildTopLevel(NRIRenderer& renderer, const std::vector<nri::TopLevelInstance>& instances, uint32_t sceneBufferMask)
{
	return BuildTopLevel(
		renderer,
		instances,
		sceneBufferMask,
		renderer.mTopLevelAS,
		renderer.GetCurrentTlasInstanceBuffer(),
		renderer.mTopLevelScratchBuffer,
		&renderer.mStaticVertexBuffer,
		&renderer.mStaticIndexBuffer,
		&renderer.mActiveTlasInstanceCount,
		true,
		true);
}

bool NRIAccelerationStructureManager::BuildTopLevel(
	NRIRenderer& renderer,
	const std::vector<nri::TopLevelInstance>& instances,
	uint32_t sceneBufferMask,
	NRIAccelerationStructureResource& topLevelAS,
	NRIBufferResource& tlasInstanceBuffer,
	NRIBufferResource& topLevelScratchBuffer,
	const NRIBufferResource* staticVertexBuffer,
	const NRIBufferResource* staticIndexBuffer,
	uint32_t* outTlasInstanceCount,
	bool updateLiveState,
	bool tlasInstanceWritesQuiesced)
{
	Clocker clock(NriPTAcceleration);
	ScopedPtPerfTimer perfTimer(renderer.mLastPerfShellTraceStats.worldTlasMs);
	renderer.mLastPerfShellTraceStats.worldTlasBuildCalls++;
	renderer.mLastPerfShellTraceStats.worldTlasInstanceCount = (uint32_t)instances.size();
	if (instances.empty())
	{
		return false;
	}
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.worldTlasRetireMs);
		renderer.RetireTopLevelAccelerationStructure(topLevelAS);
	}

	static NRIRenderer::SceneBufferDebugStats sTlasInstanceStats = { "TLASInstance" };
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.worldTlasInstanceUploadMs);
		if (!renderer.EnsureStructuredBuffer(
			tlasInstanceBuffer,
			sTlasInstanceStats,
			instances.data(),
			instances.size() * sizeof(nri::TopLevelInstance),
			sizeof(nri::TopLevelInstance),
			nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
			NRIResourceAccelerationStructureBuildInputAccess(),
			tlasInstanceWritesQuiesced,
			"world_tlas_instance_upload"))
		{
			return false;
		}
	}

	nri::AccelerationStructureDesc tlasDesc = {};
	tlasDesc.type = nri::AccelerationStructureType::TOP_LEVEL;
	tlasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
	tlasDesc.geometryOrInstanceNum = (uint32_t)instances.size();
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.worldTlasCreateMs);
		renderer.mLastPerfShellTraceStats.worldTlasCreateCalls++;
		if (renderer.mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*renderer.mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, tlasDesc, topLevelAS.accelerationStructure) != nri::Result::SUCCESS)
		{
			return false;
		}
	}

	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.worldTlasMemoryMs);
		nri::MemoryDesc memoryDesc = {};
		renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(*topLevelAS.accelerationStructure, nri::MemoryLocation::DEVICE, memoryDesc);
		topLevelAS.memorySize = memoryDesc.size;
		topLevelAS.memoryLocation = nri::MemoryLocation::DEVICE;
		renderer.mLastPerfShellTraceStats.worldTlasMemoryBytes = memoryDesc.size;
	}

	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.worldTlasScratchMs);
		renderer.mLastPerfShellTraceStats.worldTlasScratchQueries++;
		const uint64_t requiredScratchSize = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*topLevelAS.accelerationStructure);
		renderer.mLastPerfShellTraceStats.worldTlasScratchRequestedBytes = requiredScratchSize;
		if (topLevelScratchBuffer.buffer == nullptr || topLevelScratchBuffer.size < requiredScratchSize)
		{
			renderer.mLastPerfShellTraceStats.worldTlasScratchGrowCount++;
			if (topLevelScratchBuffer.buffer != nullptr)
			{
				renderer.WaitForCommandsTracked("world_tlas_scratch_resize");
			}
			renderer.DestroyBufferResource(topLevelScratchBuffer);
			if (!renderer.CreateBufferWithoutView(topLevelScratchBuffer, requiredScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
			{
				return false;
			}
		}
	}

	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.worldTlasDescriptorMs);
		renderer.mLastPerfShellTraceStats.worldTlasDescriptorCreateCalls++;
		if (renderer.mFrameBuffer->mRayTracing.CreateAccelerationStructureDescriptor(*topLevelAS.accelerationStructure, topLevelAS.descriptor) != nri::Result::SUCCESS)
		{
			return false;
		}
	}

	nri::BuildTopLevelAccelerationStructureDesc tlasBuild = {};
	tlasBuild.dst = topLevelAS.accelerationStructure;
	tlasBuild.instanceNum = (uint32_t)instances.size();
	tlasBuild.instanceBuffer = tlasInstanceBuffer.buffer;
	tlasBuild.instanceOffset = 0;
	tlasBuild.scratchBuffer = topLevelScratchBuffer.buffer;
	tlasBuild.scratchOffset = 0;
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.worldTlasBuildMs);
		renderer.mFrameBuffer->mRayTracing.CmdBuildTopLevelAccelerationStructures(*renderer.mFrameBuffer->mCommandBuffer, &tlasBuild, 1);
	}

	nri::BufferBarrierDesc tlasBarrier = {};
	tlasBarrier.buffer = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*topLevelAS.accelerationStructure);
	tlasBarrier.before = NRIResourceAccelerationStructureWriteAccess();
	tlasBarrier.after = NRIResourceComputeAccelerationStructureReadAccess();

	std::vector<nri::BufferBarrierDesc> barriers;
	barriers.reserve(5);
	barriers.push_back(tlasBarrier);
	if ((sceneBufferMask & NRIRenderer::SceneDataBufferMask_Static) != 0 && staticVertexBuffer != nullptr && staticIndexBuffer != nullptr)
	{
		nri::BufferBarrierDesc vertexBarrier = {};
		vertexBarrier.buffer = staticVertexBuffer->buffer;
		vertexBarrier.before = NRIResourceAccelerationStructureBuildInputAccess();
		vertexBarrier.after = NRIResourceComputeShaderResourceAccess();
		barriers.push_back(vertexBarrier);

		nri::BufferBarrierDesc indexBarrier = {};
		indexBarrier.buffer = staticIndexBuffer->buffer;
		indexBarrier.before = NRIResourceAccelerationStructureBuildInputAccess();
		indexBarrier.after = NRIResourceComputeShaderResourceAccess();
		barriers.push_back(indexBarrier);
	}
	if ((sceneBufferMask & NRIRenderer::SceneDataBufferMask_Dynamic) != 0)
	{
		const NRIBufferResource& dynamicVertexBuffer = renderer.GetCurrentDynamicVertexBuffer();
		const NRIBufferResource& dynamicIndexBuffer = renderer.GetCurrentDynamicIndexBuffer();
		nri::BufferBarrierDesc vertexBarrier = {};
		vertexBarrier.buffer = dynamicVertexBuffer.buffer;
		vertexBarrier.before = NRIResourceAccelerationStructureBuildInputAccess();
		vertexBarrier.after = NRIResourceComputeShaderResourceAccess();
		barriers.push_back(vertexBarrier);

		nri::BufferBarrierDesc indexBarrier = {};
		indexBarrier.buffer = dynamicIndexBuffer.buffer;
		indexBarrier.before = NRIResourceAccelerationStructureBuildInputAccess();
		indexBarrier.after = NRIResourceComputeShaderResourceAccess();
		barriers.push_back(indexBarrier);
	}

	nri::BarrierDesc barrierDesc = {};
	barrierDesc.buffers = barriers.data();
	barrierDesc.bufferNum = (uint32_t)barriers.size();
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.worldTlasBarrierMs);
		renderer.mLastPerfShellTraceStats.worldTlasBarrierCount = barrierDesc.bufferNum;
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, barrierDesc);
	}

	if (outTlasInstanceCount != nullptr)
	{
		*outTlasInstanceCount = (uint32_t)instances.size();
	}

	if (updateLiveState)
	{
		renderer.mActiveTlasInstanceCount = (uint32_t)instances.size();
		if ((sceneBufferMask & NRIRenderer::SceneDataBufferMask_Static) != 0 &&
			(sceneBufferMask & NRIRenderer::SceneDataBufferMask_Dynamic) == 0)
		{
			renderer.mStaticMapScene.tlasInstanceCount = (uint32_t)instances.size();
			renderer.mStaticMapScene.accelerationResident = true;
			renderer.mBuiltStaticMapSceneASLastFrame = true;
		}
	}
	return true;
}

bool NRIAccelerationStructureManager::EnsureTopLevelCapacity(NRIRenderer& renderer, uint32_t instanceCount)
{
	if (instanceCount == 0 || renderer.mFrameBuffer == nullptr)
	{
		return true;
	}

	ScopedPtPerfTimer perfTimer(renderer.mLastPerfShellTraceStats.worldTlasPreGrowMs);
	renderer.mLastPerfShellTraceStats.worldTlasPreGrowCalls++;
	renderer.mLastPerfShellTraceStats.worldTlasPreGrowInstanceCount = std::max(
		renderer.mLastPerfShellTraceStats.worldTlasPreGrowInstanceCount,
		instanceCount);

	const uint64_t instanceBytes = (uint64_t)instanceCount * sizeof(nri::TopLevelInstance);
	static NRIRenderer::SceneBufferDebugStats sTlasInstancePreGrowStats = { "TLASInstancePreGrow" };
	(void)renderer.GetCurrentTlasInstanceBuffer();
	for (NRIBufferResource& tlasInstanceBuffer : renderer.mTlasInstanceBufferRing)
	{
		if (!NRISceneUploadManager::EnsureStructuredBufferCapacity(
			renderer,
			tlasInstanceBuffer,
			sTlasInstancePreGrowStats,
			instanceBytes,
			sizeof(nri::TopLevelInstance),
			nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
			"world_tlas_instance_upload"))
		{
			return false;
		}
		if (sTlasInstancePreGrowStats.growEventsLastFrame != 0)
		{
			renderer.mLastPerfShellTraceStats.worldTlasPreGrowInstanceGrowCount++;
			renderer.mLastPerfShellTraceStats.worldTlasPreGrowInstanceRequestedBytes +=
				sTlasInstancePreGrowStats.growthRequestedBytesLastFrame;
			renderer.mLastPerfShellTraceStats.worldTlasPreGrowInstanceAllocatedBytes +=
				sTlasInstancePreGrowStats.growthAllocatedBytesLastFrame;
		}
	}

	nri::AccelerationStructureDesc tlasDesc = {};
	tlasDesc.type = nri::AccelerationStructureType::TOP_LEVEL;
	tlasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
	tlasDesc.geometryOrInstanceNum = instanceCount;

	NRIAccelerationStructureResource probe = {};
	if (renderer.mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(
		*renderer.mFrameBuffer->mDevice,
		nri::MemoryLocation::DEVICE,
		0.0f,
		tlasDesc,
		probe.accelerationStructure) != nri::Result::SUCCESS)
	{
		return false;
	}
	const uint64_t requiredScratchSize =
		renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*probe.accelerationStructure);
	renderer.DestroyAccelerationStructureResource(probe);
	renderer.mLastPerfShellTraceStats.worldTlasPreGrowScratchRequestedBytes =
		std::max(renderer.mLastPerfShellTraceStats.worldTlasPreGrowScratchRequestedBytes, requiredScratchSize);

	if (renderer.mTopLevelScratchBuffer.buffer == nullptr || renderer.mTopLevelScratchBuffer.size < requiredScratchSize)
	{
		renderer.mLastPerfShellTraceStats.worldTlasPreGrowScratchGrowCount++;
		if (renderer.mTopLevelScratchBuffer.buffer != nullptr)
		{
			const auto waitStart = std::chrono::steady_clock::now();
			renderer.WaitForCommandsTracked("world_tlas_scratch_resize");
			renderer.mLastPerfShellTraceStats.worldTlasPreGrowWaitMs += DurationMs(waitStart, std::chrono::steady_clock::now());
		}
		renderer.DestroyBufferResource(renderer.mTopLevelScratchBuffer);
		if (!renderer.CreateBufferWithoutView(renderer.mTopLevelScratchBuffer, requiredScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
		{
			return false;
		}
		renderer.mLastPerfShellTraceStats.worldTlasPreGrowScratchAllocatedBytes =
			std::max(renderer.mLastPerfShellTraceStats.worldTlasPreGrowScratchAllocatedBytes, renderer.mTopLevelScratchBuffer.size);
	}

	return true;
}

void NRIRenderer::ReleaseWorldAccelerationBuildScratch(const char* reason)
{
	const uint64_t scratchBytes = mScratchBuffer.memorySize + mTopLevelScratchBuffer.memorySize;
	const uint32_t scratchBuffers =
		(mScratchBuffer.buffer != nullptr ? 1u : 0u) +
		(mTopLevelScratchBuffer.buffer != nullptr ? 1u : 0u);
	if (scratchBuffers == 0)
	{
		return;
	}

	DestroyBufferResource(mScratchBuffer);
	DestroyBufferResource(mTopLevelScratchBuffer);
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT transient scratch: event=release reason=%s buffers=%u bytes=%llu\n",
			reason != nullptr ? reason : "unspecified",
			scratchBuffers,
			(unsigned long long)scratchBytes);
	}
}

bool NRIRenderer::BuildDynamicAccelerationStructure(const nri_scene::GeometryData& geometry)
{
	return NRIAccelerationStructureManager::BuildDynamic(*this, geometry);
}

bool NRIRenderer::BuildDynamicAccelerationStructure(
	const nri_scene::GeometryData& geometry,
	uint32_t indexOffset,
	uint32_t indexCount,
	uint32_t primitiveCount,
	NRIAccelerationStructureResource& outAccelerationStructure,
	bool updateDynamicPerfStats)
{
	return NRIAccelerationStructureManager::BuildDynamic(*this, geometry, indexOffset, indexCount, primitiveCount, outAccelerationStructure, updateDynamicPerfStats);
}

bool NRIRenderer::BuildBottomLevelAccelerationStructure(
	const NRIBufferResource& vertexBuffer,
	const NRIBufferResource& indexBuffer,
	uint32_t vertexCount,
	uint32_t indexOffset,
	uint32_t indexCount,
	uint32_t primitiveCount,
	NRIAccelerationStructureResource& outAccelerationStructure,
	bool updateDynamicPerfStats)
{
	return NRIAccelerationStructureManager::BuildBottomLevel(
		*this,
		vertexBuffer,
		indexBuffer,
		vertexCount,
		indexOffset,
		indexCount,
		primitiveCount,
		outAccelerationStructure,
		updateDynamicPerfStats);
}

bool NRIRenderer::BuildEmissiveTopLevelAccelerationStructure()
{
	return NRIAccelerationStructureManager::BuildEmissiveTopLevel(*this);
}

bool NRIRenderer::BuildTopLevelAccelerationStructure(const std::vector<nri::TopLevelInstance>& instances, uint32_t sceneBufferMask)
{
	return NRIAccelerationStructureManager::BuildTopLevel(*this, instances, sceneBufferMask);
}

bool NRIRenderer::BuildTopLevelAccelerationStructure(
	const std::vector<nri::TopLevelInstance>& instances,
	uint32_t sceneBufferMask,
	NRIAccelerationStructureResource& topLevelAS,
	NRIBufferResource& tlasInstanceBuffer,
	NRIBufferResource& topLevelScratchBuffer,
	const NRIBufferResource* staticVertexBuffer,
	const NRIBufferResource* staticIndexBuffer,
	uint32_t* outTlasInstanceCount,
	bool updateLiveState,
	bool tlasInstanceWritesQuiesced)
{
	return NRIAccelerationStructureManager::BuildTopLevel(
		*this,
		instances,
		sceneBufferMask,
		topLevelAS,
		tlasInstanceBuffer,
		topLevelScratchBuffer,
		staticVertexBuffer,
		staticIndexBuffer,
		outTlasInstanceCount,
		updateLiveState,
		tlasInstanceWritesQuiesced);
}

bool NRIRenderer::EnsureTopLevelAccelerationStructureCapacity(uint32_t instanceCount)
{
	return NRIAccelerationStructureManager::EnsureTopLevelCapacity(*this, instanceCount);
}

void NRIRenderer::DestroyDynamicBottomLevelAccelerationStructures()
{
	for (SceneUploadBufferRingSlot& slot : mSceneUploadBufferRing)
	{
		DestroyAccelerationStructureResource(slot.dynamicBottomLevelAS);
	}
}

void NRIRenderer::DestroyAccelerationStructures()
{
	mStaticMapScene.accelerationResident = false;
	for (auto& chunk : mStaticMapScene.chunks)
	{
		DestroyAccelerationStructureResource(chunk.accelerationStructure);
		chunk.residentBlasScratchSizeCacheKey = nullptr;
		chunk.residentBlasBuildScratchSize = 0;
		chunk.residentBlasUpdateScratchSize = 0;
	}
	DestroyDynamicBottomLevelAccelerationStructures();
	mPersistentVoxels.Reset("destroy-acceleration-structures", true, (int)nri_ptloadingtrace >= 1 || (bool)nri_voxelstats, BuildNRIPersistentVoxelResetServices(*this));
	DestroyAccelerationStructureResource(mTopLevelAS);
	DestroyAccelerationStructureResource(mEmissiveTopLevelAS);
	mStaticAccelerationBuildSerial = 0;
	mActiveTlasInstanceCount = 0;
	mEmissiveTlasInstanceCount = 0;
	mEmissiveTlasStaticInstanceCount = 0;
	mEmissiveTlasDynamicInstanceCount = 0;
	mEmissiveTlasBuildCount = 0;
	mEmissiveTlasInstancePayloadCacheValid = false;
	mEmissiveTlasInstancePayloadHash = 0;
	SyncResidentMapChunkRegistryFromStaticScene();
}
