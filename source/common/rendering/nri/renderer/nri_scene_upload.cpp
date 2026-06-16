#include "nri_scene_upload.h"
#include "nri_cvars.h"

#include "nri_renderer.h"

#include "nri_descriptor_sets.h"
#include "../scene/nri_hash.h"
#include "nri_runtime_mutation_trace.h"
#include "nri_scene_lights.h"
#include "nri_shader_contracts.h"
#include "nri_static_scene_geometry.h"
#include "nri_upload_hash.h"
#include "c_cvars.h"
#include "../../hwrenderer/data/hw_clock.h"

#include <algorithm>
#include <chrono>
#include <vector>


namespace
{
	static double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	}

	static bool ShouldCollectSceneDataTiming()
	{
		return (int)nri_pttraceframes > 0 || (int)perf_looptraceframes > 0 || (bool)nri_ptslowdowntrace;
	}

	class ScopedPtPerfTimer
	{
	public:
		explicit ScopedPtPerfTimer(double& targetMs)
			: mTarget(ShouldCollectSceneDataTiming() ? &targetMs : nullptr)
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

	struct ScenePortalData
	{
		uint32_t traversalClass = 0;
		uint32_t kind = 0;
		uint32_t targetLocalSpaceIndex = UINT32_MAX;
		uint32_t flags = 0;
		float delta[3] = {};
		uint32_t reserved0 = 0;
	};

	static uint32_t GetPortalTraversalClass(nri_scene::PTPortalKind kind)
	{
		switch (kind)
		{
		case nri_scene::PTPortalKind::WallMirror:
		case nri_scene::PTPortalKind::SectorFloorMirror:
		case nri_scene::PTPortalKind::SectorCeilingMirror:
			return NRI_PORTAL_TRAVERSAL_CLASS_REFLECTIVE;

		case nri_scene::PTPortalKind::WallView:
		case nri_scene::PTPortalKind::SectorFloorStack:
		case nri_scene::PTPortalKind::SectorCeilingStack:
			return NRI_PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER;

		case nri_scene::PTPortalKind::WallToSprite:
			return NRI_PORTAL_TRAVERSAL_CLASS_RUNTIME_BOUND;

		default:
			return NRI_PORTAL_TRAVERSAL_CLASS_NONE;
		}
	}

	static std::vector<ScenePortalData> BuildScenePortalData(const nri_scene::PTMapWorld& mapWorld)
	{
		std::vector<ScenePortalData> portals;
		portals.reserve(std::max<size_t>(mapWorld.portals.size(), 1u));

		for (const auto& portal : mapWorld.portals)
		{
			ScenePortalData data = {};
			data.traversalClass = GetPortalTraversalClass(portal.kind);
			data.kind = (uint32_t)portal.kind;
			data.flags = portal.runtimeBoundTarget ? NRI_PORTAL_FLAG_RUNTIME_BOUND : 0u;
			if (portal.targetCount > 0 && portal.firstTarget < mapWorld.portalTargets.size())
			{
				data.targetLocalSpaceIndex = mapWorld.portalTargets[portal.firstTarget].localSpaceIndex;
			}
			data.delta[0] = (float)portal.delta[0];
			data.delta[1] = (float)portal.delta[1];
			data.delta[2] = (float)portal.delta[2];
			portals.push_back(data);
		}

		if (portals.empty())
		{
			portals.push_back({});
		}

		return portals;
	}
}

#include "../system/nri_renderdevice.h"

#include <cstring>

namespace
{
	struct NRIReprojectionData
	{
		float currentViewToClip[16] = {};
		float previousViewToClip[16] = {};
		float currentWorldToView[16] = {};
		float previousWorldToView[16] = {};
	};

	static bool ShouldTraceSceneBufferDirtyRanges()
	{
		return (int)perf_looptraceframes > 0;
	}

	static uint64_t HashUploadPayloadBytes(const void* data, uint64_t size)
	{
		return NRIHashUploadPayloadBytes(data, size);
	}

	static uint64_t HashPrimitiveRewriteProvenancePayload(const std::vector<nri_scene::SurfaceProvenance>& provenanceList)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, (uint64_t)provenanceList.size());
		for (const nri_scene::SurfaceProvenance& provenance : provenanceList)
		{
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)provenance.sourceType);
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.sectorIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.wallIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.sectionIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.mapChunkIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.nextSectorIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.actorIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)provenance.drawListType);
			hash = nri_scene::HashCombine64(hash, (uint64_t)provenance.cstat);
			hash = nri_scene::HashCombine64(hash, (uint64_t)provenance.materialFlags);
		}
		return hash != 0 ? hash : 1;
	}

	static uint64_t HashPrimitiveRewriteVisibilityIdentity(const nri_scene::PTMapWorld& mapWorld)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, mapWorld.valid ? 1ull : 0ull);
		hash = nri_scene::HashCombine64(hash, mapWorld.buildSerial);
		hash = nri_scene::HashCombine64(hash, (uint64_t)mapWorld.chunks.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)mapWorld.stats.chunkCount);
		for (const nri_scene::PTMapChunk& chunk : mapWorld.chunks)
		{
			hash = nri_scene::HashCombine64(hash, (uint64_t)chunk.chunkIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(chunk.sectorIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)chunk.firstSurface);
			hash = nri_scene::HashCombine64(hash, (uint64_t)chunk.surfaceCount);
		}
		return hash != 0 ? hash : 1;
	}

	static bool StructuredBufferUpdateNeedsWait(
		const NRIBufferResource& resource,
		const void* data,
		uint64_t size,
		uint32_t stride)
	{
		const uint64_t requiredSize = std::max<uint64_t>(size, stride);
		const bool needsGrowth =
			resource.buffer == nullptr ||
			resource.shaderView == nullptr ||
			resource.stride != stride ||
			resource.size < requiredSize;
		if (needsGrowth)
		{
			return resource.buffer != nullptr || resource.shaderView != nullptr;
		}

		return data != nullptr && size != 0;
	}
}

void NRIRenderer::ResetSceneBufferFrameStats()
{
	mVertexBufferStats.bytesUploadedLastFrame = 0;
	mVertexBufferStats.growEventsLastFrame = 0;
	mVertexBufferStats.overwriteEventsLastFrame = 0;
	mVertexBufferStats.growthOldBytesLastFrame = 0;
	mVertexBufferStats.growthRequestedBytesLastFrame = 0;
	mVertexBufferStats.growthAllocatedBytesLastFrame = 0;
	mIndexBufferStats.bytesUploadedLastFrame = 0;
	mIndexBufferStats.growEventsLastFrame = 0;
	mIndexBufferStats.overwriteEventsLastFrame = 0;
	mIndexBufferStats.growthOldBytesLastFrame = 0;
	mIndexBufferStats.growthRequestedBytesLastFrame = 0;
	mIndexBufferStats.growthAllocatedBytesLastFrame = 0;
	mPrimitiveBufferStats.bytesUploadedLastFrame = 0;
	mPrimitiveBufferStats.growEventsLastFrame = 0;
	mPrimitiveBufferStats.overwriteEventsLastFrame = 0;
	mPrimitiveBufferStats.growthOldBytesLastFrame = 0;
	mPrimitiveBufferStats.growthRequestedBytesLastFrame = 0;
	mPrimitiveBufferStats.growthAllocatedBytesLastFrame = 0;
	mMaterialBufferStats.bytesUploadedLastFrame = 0;
	mMaterialBufferStats.growEventsLastFrame = 0;
	mMaterialBufferStats.overwriteEventsLastFrame = 0;
	mMaterialBufferStats.growthOldBytesLastFrame = 0;
	mMaterialBufferStats.growthRequestedBytesLastFrame = 0;
	mMaterialBufferStats.growthAllocatedBytesLastFrame = 0;
	mPortalBufferStats.bytesUploadedLastFrame = 0;
	mPortalBufferStats.growEventsLastFrame = 0;
	mPortalBufferStats.overwriteEventsLastFrame = 0;
}

const NRIBufferResource& NRIRenderer::GetActiveVertexBuffer() const
{
	return mBoundDynamicPrimitiveCount > 0 ? GetCurrentDynamicVertexBuffer() : mStaticVertexBuffer;
}

const NRIBufferResource& NRIRenderer::GetActiveIndexBuffer() const
{
	return mBoundDynamicPrimitiveCount > 0 ? GetCurrentDynamicIndexBuffer() : mStaticIndexBuffer;
}

const NRIBufferResource& NRIRenderer::GetActivePrimitiveBuffer() const
{
	return mBoundDynamicPrimitiveCount > 0 ? GetCurrentDynamicPrimitiveBuffer() : mStaticPrimitiveBuffer;
}

const NRIBufferResource& NRIRenderer::GetActiveMaterialBuffer() const
{
	return mBoundDynamicMaterialCount > 0 ? GetCurrentDynamicMaterialBuffer() : mStaticMaterialBuffer;
}

NRIRenderer::SceneUploadBufferRingSlot& NRIRenderer::GetCurrentSceneUploadBufferRingSlot()
{
	const uint32_t queuedFrameCount =
		mFrameBuffer != nullptr && !mFrameBuffer->mQueuedFrames.empty() ?
		(uint32_t)mFrameBuffer->mQueuedFrames.size() :
		1u;
	if (mSceneUploadBufferRing.size() < queuedFrameCount)
	{
		mSceneUploadBufferRing.resize(queuedFrameCount);
	}

	return mSceneUploadBufferRing[GetCurrentQueuedFrameIndex() % (uint32_t)mSceneUploadBufferRing.size()];
}

const NRIRenderer::SceneUploadBufferRingSlot* NRIRenderer::GetCurrentSceneUploadBufferRingSlot() const
{
	if (mSceneUploadBufferRing.empty())
	{
		return nullptr;
	}

	return &mSceneUploadBufferRing[GetCurrentQueuedFrameIndex() % (uint32_t)mSceneUploadBufferRing.size()];
}

NRIBufferResource& NRIRenderer::GetCurrentDynamicVertexBuffer()
{
	return GetCurrentSceneUploadBufferRingSlot().vertexBuffer;
}

NRIBufferResource& NRIRenderer::GetCurrentDynamicIndexBuffer()
{
	return GetCurrentSceneUploadBufferRingSlot().indexBuffer;
}

NRIBufferResource& NRIRenderer::GetCurrentDynamicPrimitiveBuffer()
{
	return GetCurrentSceneUploadBufferRingSlot().primitiveBuffer;
}

NRIBufferResource& NRIRenderer::GetCurrentDynamicMaterialBuffer()
{
	return GetCurrentSceneUploadBufferRingSlot().materialBuffer;
}

NRIAccelerationStructureResource& NRIRenderer::GetCurrentDynamicBottomLevelAS()
{
	return GetCurrentSceneUploadBufferRingSlot().dynamicBottomLevelAS;
}

NRIBufferResource& NRIRenderer::GetCurrentTlasInstanceBuffer()
{
	const uint32_t queuedFrameCount =
		mFrameBuffer != nullptr && !mFrameBuffer->mQueuedFrames.empty() ?
		(uint32_t)mFrameBuffer->mQueuedFrames.size() :
		1u;
	if (mTlasInstanceBufferRing.size() < queuedFrameCount)
	{
		mTlasInstanceBufferRing.resize(queuedFrameCount);
	}

	return mTlasInstanceBufferRing[GetCurrentQueuedFrameIndex() % (uint32_t)mTlasInstanceBufferRing.size()];
}

const NRIBufferResource& NRIRenderer::GetCurrentDynamicVertexBuffer() const
{
	const SceneUploadBufferRingSlot* slot = GetCurrentSceneUploadBufferRingSlot();
	return slot != nullptr ? slot->vertexBuffer : mVertexBuffer;
}

const NRIBufferResource& NRIRenderer::GetCurrentDynamicIndexBuffer() const
{
	const SceneUploadBufferRingSlot* slot = GetCurrentSceneUploadBufferRingSlot();
	return slot != nullptr ? slot->indexBuffer : mIndexBuffer;
}

const NRIBufferResource& NRIRenderer::GetCurrentDynamicPrimitiveBuffer() const
{
	const SceneUploadBufferRingSlot* slot = GetCurrentSceneUploadBufferRingSlot();
	return slot != nullptr ? slot->primitiveBuffer : mPrimitiveBuffer;
}

const NRIBufferResource& NRIRenderer::GetCurrentDynamicMaterialBuffer() const
{
	const SceneUploadBufferRingSlot* slot = GetCurrentSceneUploadBufferRingSlot();
	return slot != nullptr ? slot->materialBuffer : mMaterialBuffer;
}

const NRIAccelerationStructureResource* NRIRenderer::GetCurrentDynamicBottomLevelAS() const
{
	const SceneUploadBufferRingSlot* slot = GetCurrentSceneUploadBufferRingSlot();
	return slot != nullptr ? &slot->dynamicBottomLevelAS : nullptr;
}

const NRIBufferResource& NRIRenderer::GetCurrentTlasInstanceBuffer() const
{
	if (mTlasInstanceBufferRing.empty())
	{
		return mTlasInstanceBuffer;
	}

	return mTlasInstanceBufferRing[GetCurrentQueuedFrameIndex() % (uint32_t)mTlasInstanceBufferRing.size()];
}

bool NRIRenderer::HasAnyDynamicBottomLevelAS() const
{
	for (const SceneUploadBufferRingSlot& slot : mSceneUploadBufferRing)
	{
		if (slot.dynamicBottomLevelAS.accelerationStructure != nullptr ||
			slot.dynamicBottomLevelAS.descriptor != nullptr)
		{
			return true;
		}
	}

	return false;
}

NRIRenderer::ResidentUploadScratchFrame& NRIRenderer::GetResidentUploadScratchFrame()
{
	const uint32_t frameSlot = GetCurrentQueuedFrameIndex() % (uint32_t)mResidentUploadScratchFrames.size();
	auto& frameScratch = mResidentUploadScratchFrames[frameSlot];
	if (frameScratch.frameIndex != mFrameIndex)
	{
		for (NRIBufferResource& retired : frameScratch.retiredBuffers)
		{
			DestroyBufferResource(retired);
		}
		frameScratch.retiredBuffers.clear();
		for (NRIAccelerationStructureResource& retired : frameScratch.retiredAccelerationStructures)
		{
			DestroyAccelerationStructureResource(retired);
		}
		frameScratch.retiredAccelerationStructures.clear();
		frameScratch.frameIndex = mFrameIndex;
		frameScratch.vertex.cursor = 0;
		frameScratch.vertex.copySourceActive = false;
		frameScratch.index.cursor = 0;
		frameScratch.index.copySourceActive = false;
		frameScratch.primitive.cursor = 0;
		frameScratch.primitive.copySourceActive = false;
		frameScratch.material.cursor = 0;
		frameScratch.material.copySourceActive = false;
	}

	return frameScratch;
}

nri::DescriptorSet* NRIRenderer::GetCurrentSceneTextureSet() const
{
	return NRIDescriptorSetManager::GetCurrentSceneTextureSet(*this);
}

nri::DescriptorSet* NRIRenderer::GetCurrentSceneDataSet() const
{
	return NRIDescriptorSetManager::GetCurrentSceneDataSet(*this);
}

bool NRIRenderer::IsCurrentSceneDataDescriptorsInitialized() const
{
	return NRIDescriptorSetManager::IsCurrentSceneDataDescriptorsInitialized(*this);
}

void NRIRenderer::SetCurrentSceneDataDescriptorsInitialized(bool value)
{
	NRIDescriptorSetManager::SetCurrentSceneDataDescriptorsInitialized(*this, value);
}

void NRIRenderer::TraceSharedDescriptorRewrite(const char* setName, const char* reason, uint64_t descriptorHash, uint32_t descriptorCount, bool sceneTextureSet)
{
	NRIDescriptorSetManager::TraceSharedDescriptorRewrite(*this, setName, reason, descriptorHash, descriptorCount, sceneTextureSet);
}

bool NRISceneUploadManager::SceneDataDescriptorsReady(NRIRenderer& renderer)
{
	if (!renderer.IsCurrentSceneDataDescriptorsInitialized() || renderer.GetCurrentSceneDataSet() == nullptr)
	{
		return false;
	}

	for (const nri::Descriptor* descriptor : renderer.mSceneDataDescriptors)
	{
		if (descriptor == nullptr)
		{
			return false;
		}
	}

	return true;
}

bool NRISceneUploadManager::UpdateSceneDataDescriptorSlot(
	NRIRenderer& renderer,
	uint32_t slot,
	nri::Descriptor* descriptor,
	const char* reason)
{
	if (slot >= renderer.mSceneDataDescriptors.size() ||
		renderer.mSceneDataDescriptors[slot] == descriptor)
	{
		return true;
	}

	renderer.mSceneDataDescriptors[slot] = descriptor;
	if (SceneDataDescriptorsReady(renderer))
	{
		return NRIDescriptorSetManager::CommitSceneDataDescriptors(renderer, reason);
	}

	return true;
}

bool NRISceneUploadManager::WaitIfStructuredUpdateNeedsIt(
	NRIRenderer& renderer,
	NRIBufferResource& resource,
	const void* data,
	uint64_t size,
	uint32_t stride,
	bool* ioWaitedForWrites)
{
	if (ioWaitedForWrites != nullptr &&
		!*ioWaitedForWrites &&
		StructuredBufferUpdateNeedsWait(resource, data, size, stride))
	{
		renderer.WaitForCommandsTracked("scene_data_upload");
		*ioWaitedForWrites = true;
	}

	return true;
}

bool NRISceneUploadManager::UpdateReprojectionBuffer(NRIRenderer& renderer, bool* ioWaitedForWrites)
{
	NRIReprojectionData data = {};
	std::memcpy(data.currentViewToClip, renderer.mCurrentViewToClip, sizeof(data.currentViewToClip));
	std::memcpy(data.previousViewToClip, renderer.mPreviousViewToClip, sizeof(data.previousViewToClip));
	std::memcpy(data.currentWorldToView, renderer.mCurrentWorldToView, sizeof(data.currentWorldToView));
	std::memcpy(data.previousWorldToView, renderer.mPreviousWorldToView, sizeof(data.previousWorldToView));

	WaitIfStructuredUpdateNeedsIt(renderer, renderer.mReprojectionBuffer, &data, sizeof(data), sizeof(data), ioWaitedForWrites);
	if (!renderer.EnsureStructuredBuffer(
		renderer.mReprojectionBuffer,
		renderer.mReprojectionBufferStats,
		&data,
		sizeof(data),
		sizeof(data),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess(),
		ioWaitedForWrites != nullptr && *ioWaitedForWrites,
		"scene_data_upload"))
	{
		return false;
	}

	return UpdateSceneDataDescriptorSlot(renderer, 18, renderer.mReprojectionBuffer.shaderView, "reprojection_refresh");
}

bool NRISceneUploadManager::UpdateVisibleChunkBuffer(NRIRenderer& renderer, bool* ioWaitedForWrites)
{
	const uint32_t defaultVisibleChunkWord = 0u;
	const void* visibleChunkData = renderer.mCurrentVisibleChunkWords.empty() ? (const void*)&defaultVisibleChunkWord : renderer.mCurrentVisibleChunkWords.data();
	const size_t visibleChunkSize = renderer.mCurrentVisibleChunkWords.empty() ? sizeof(uint32_t) : renderer.mCurrentVisibleChunkWords.size() * sizeof(uint32_t);

	WaitIfStructuredUpdateNeedsIt(renderer, renderer.mVisibleChunkBuffer, visibleChunkData, visibleChunkSize, sizeof(uint32_t), ioWaitedForWrites);
	if (!renderer.EnsureStructuredBuffer(
		renderer.mVisibleChunkBuffer,
		renderer.mVisibleChunkBufferStats,
		visibleChunkData,
		visibleChunkSize,
		sizeof(uint32_t),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess(),
		ioWaitedForWrites != nullptr && *ioWaitedForWrites,
		"scene_data_upload"))
	{
		return false;
	}

	return UpdateSceneDataDescriptorSlot(renderer, 19, renderer.mVisibleChunkBuffer.shaderView, "visible_chunk_refresh");
}

bool NRISceneUploadManager::UpdateVisibleFlatPlaneBuffer(NRIRenderer& renderer, bool* ioWaitedForWrites)
{
	const uint32_t defaultVisibleFlatPlaneWord = 0u;
	const void* visibleFlatPlaneData = renderer.mCurrentVisibleFlatPlaneWords.empty() ? (const void*)&defaultVisibleFlatPlaneWord : renderer.mCurrentVisibleFlatPlaneWords.data();
	const size_t visibleFlatPlaneSize = renderer.mCurrentVisibleFlatPlaneWords.empty() ? sizeof(uint32_t) : renderer.mCurrentVisibleFlatPlaneWords.size() * sizeof(uint32_t);

	WaitIfStructuredUpdateNeedsIt(renderer, renderer.mVisibleFlatPlaneBuffer, visibleFlatPlaneData, visibleFlatPlaneSize, sizeof(uint32_t), ioWaitedForWrites);
	if (!renderer.EnsureStructuredBuffer(
		renderer.mVisibleFlatPlaneBuffer,
		renderer.mVisibleFlatPlaneBufferStats,
		visibleFlatPlaneData,
		visibleFlatPlaneSize,
		sizeof(uint32_t),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess(),
		ioWaitedForWrites != nullptr && *ioWaitedForWrites,
		"scene_data_upload"))
	{
		return false;
	}

	return UpdateSceneDataDescriptorSlot(renderer, 20, renderer.mVisibleFlatPlaneBuffer.shaderView, "visible_flat_refresh");
}

bool NRIRenderer::UploadSceneBuffers(
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& materials,
	const std::vector<SceneBufferUploadDomainSpan>* domainSpans)
{
	return UploadSceneBuffers(GetCurrentSceneUploadBufferRingSlot(), geometry, materials, domainSpans);
}

bool NRIRenderer::UploadSceneBuffers(
	SceneUploadBufferRingSlot& uploadSlot,
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& materials,
	const std::vector<SceneBufferUploadDomainSpan>* domainSpans)
{
	Clocker clock(NriPTSceneBuffers);
	NRIBufferResource& vertexBuffer = uploadSlot.vertexBuffer;
	NRIBufferResource& indexBuffer = uploadSlot.indexBuffer;
	NRIBufferResource& primitiveBuffer = uploadSlot.primitiveBuffer;
	NRIBufferResource& materialBuffer = uploadSlot.materialBuffer;
	std::vector<uint8_t>& vertexMirror = uploadSlot.vertexMirror;
	std::vector<uint8_t>& indexMirror = uploadSlot.indexMirror;
	std::vector<uint8_t>& primitiveMirror = uploadSlot.primitiveMirror;
	std::vector<uint8_t>& materialMirror = uploadSlot.materialMirror;
	mVertexBufferStats.bytesUploadedLastFrame = 0;
	mVertexBufferStats.growEventsLastFrame = 0;
	mVertexBufferStats.overwriteEventsLastFrame = 0;
	mVertexBufferStats.growthOldBytesLastFrame = 0;
	mVertexBufferStats.growthRequestedBytesLastFrame = 0;
	mVertexBufferStats.growthAllocatedBytesLastFrame = 0;
	mIndexBufferStats.bytesUploadedLastFrame = 0;
	mIndexBufferStats.growEventsLastFrame = 0;
	mIndexBufferStats.overwriteEventsLastFrame = 0;
	mIndexBufferStats.growthOldBytesLastFrame = 0;
	mIndexBufferStats.growthRequestedBytesLastFrame = 0;
	mIndexBufferStats.growthAllocatedBytesLastFrame = 0;
	mPrimitiveBufferStats.bytesUploadedLastFrame = 0;
	mPrimitiveBufferStats.growEventsLastFrame = 0;
	mPrimitiveBufferStats.overwriteEventsLastFrame = 0;
	mPrimitiveBufferStats.growthOldBytesLastFrame = 0;
	mPrimitiveBufferStats.growthRequestedBytesLastFrame = 0;
	mPrimitiveBufferStats.growthAllocatedBytesLastFrame = 0;
	mMaterialBufferStats.bytesUploadedLastFrame = 0;
	mMaterialBufferStats.growEventsLastFrame = 0;
	mMaterialBufferStats.overwriteEventsLastFrame = 0;
	mMaterialBufferStats.growthOldBytesLastFrame = 0;
	mMaterialBufferStats.growthRequestedBytesLastFrame = 0;
	mMaterialBufferStats.growthAllocatedBytesLastFrame = 0;
	{
		mLastPerfShellTraceStats.sceneSelectBufferUploadVertexRequestedBytes = geometry.vertices.size() * sizeof(nri_scene::SceneVertex);
		mLastPerfShellTraceStats.sceneSelectBufferUploadIndexRequestedBytes = geometry.indices.size() * sizeof(uint32_t);
		mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRequestedBytes = geometry.primitives.size() * sizeof(nri_scene::PrimitiveData);
		mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialRequestedBytes = materials.size() * sizeof(nri_scene::MaterialData);
	}
	enum class SceneUploadBufferKind
	{
		Vertex,
		Index,
		Primitive,
		Material
	};
	const auto getDomainEntry = [&](SceneBufferUploadDomain domain) -> PerfShellTraceStats::SceneBufferUploadDomainTraceEntry*
	{
		const size_t domainIndex = (size_t)domain;
		if (domainIndex >= SceneBufferUploadDomainCount)
		{
			return nullptr;
		}
		return &mLastPerfShellTraceStats.sceneSelectBufferUploadDomains[domainIndex];
	};
	const auto getSpanByteRange =
		[](const SceneBufferUploadDomainSpan& span, SceneUploadBufferKind kind, uint64_t& outOffset, uint64_t& outSize)
	{
		switch (kind)
		{
		case SceneUploadBufferKind::Vertex:
			outOffset = (uint64_t)span.vertexOffset * sizeof(nri_scene::SceneVertex);
			outSize = (uint64_t)span.vertexCount * sizeof(nri_scene::SceneVertex);
			break;
		case SceneUploadBufferKind::Index:
			outOffset = (uint64_t)span.indexOffset * sizeof(uint32_t);
			outSize = (uint64_t)span.indexCount * sizeof(uint32_t);
			break;
		case SceneUploadBufferKind::Primitive:
			outOffset = (uint64_t)span.primitiveOffset * sizeof(nri_scene::PrimitiveData);
			outSize = (uint64_t)span.primitiveCount * sizeof(nri_scene::PrimitiveData);
			break;
		case SceneUploadBufferKind::Material:
			outOffset = (uint64_t)span.materialOffset * sizeof(nri_scene::MaterialData);
			outSize = (uint64_t)span.materialCount * sizeof(nri_scene::MaterialData);
			break;
		}
	};
	const auto addDomainPayload =
		[&](SceneUploadBufferKind kind, bool skipped)
	{
		if (domainSpans == nullptr)
		{
			return;
		}
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			if (size == 0)
			{
				continue;
			}
			auto* domain = getDomainEntry(span.domain);
			if (domain == nullptr)
			{
				continue;
			}
			domain->payloadBytes += size;
			domain->hashChecks++;
			if (!skipped)
			{
				domain->hashMisses++;
			}
			switch (kind)
			{
			case SceneUploadBufferKind::Vertex: domain->vertexPayloadBytes += size; break;
			case SceneUploadBufferKind::Index: domain->indexPayloadBytes += size; break;
			case SceneUploadBufferKind::Primitive: domain->primitivePayloadBytes += size; break;
			case SceneUploadBufferKind::Material: domain->materialPayloadBytes += size; break;
			}
		}
	};
	const auto addDomainFullUpload =
		[&](SceneUploadBufferKind kind)
	{
		if (domainSpans == nullptr)
		{
			return;
		}
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			if (size == 0)
			{
				continue;
			}
			auto* domain = getDomainEntry(span.domain);
			if (domain == nullptr)
			{
				continue;
			}
			domain->uploadedBytes += size;
			if (kind == SceneUploadBufferKind::Primitive)
			{
				domain->primitiveUploadedBytes += size;
			}
			else if (kind == SceneUploadBufferKind::Material)
			{
				domain->materialUploadedBytes += size;
			}
		}
	};
	const auto addDomainRangeBytes =
		[&](SceneUploadBufferKind kind, const std::vector<SceneUploadDirtyRange>& ranges, bool countDirty)
	{
		if (domainSpans == nullptr)
		{
			return;
		}
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t spanOffset = 0;
			uint64_t spanSize = 0;
			getSpanByteRange(span, kind, spanOffset, spanSize);
			if (spanSize == 0)
			{
				continue;
			}
			const uint64_t spanEnd = spanOffset + spanSize;
			uint64_t domainBytes = 0;
			uint32_t domainRanges = 0;
			for (const SceneUploadDirtyRange& range : ranges)
			{
				const uint64_t rangeEnd = range.byteOffset + range.size;
				const uint64_t overlapStart = std::max(spanOffset, range.byteOffset);
				const uint64_t overlapEnd = std::min(spanEnd, rangeEnd);
				if (overlapEnd > overlapStart)
				{
					domainBytes += overlapEnd - overlapStart;
					domainRanges++;
				}
			}
			if (domainBytes == 0)
			{
				continue;
			}
			auto* domain = getDomainEntry(span.domain);
			if (domain == nullptr)
			{
				continue;
			}
			if (countDirty)
			{
				domain->dirtyRanges += domainRanges;
				domain->dirtyChangedBytes += domainBytes;
				domain->dirtyUploadedBytes += domainBytes;
			}
			else
			{
				domain->uploadedBytes += domainBytes;
				if (kind == SceneUploadBufferKind::Primitive)
				{
					domain->primitiveUploadedBytes += domainBytes;
				}
				else if (kind == SceneUploadBufferKind::Material)
				{
					domain->materialUploadedBytes += domainBytes;
				}
			}
		}
	};
	const auto addDomainWait =
		[&](SceneUploadBufferKind kind, double waitMs)
	{
		if (domainSpans == nullptr || waitMs <= 0.0)
		{
			return;
		}
		uint64_t totalBytes = 0;
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			totalBytes += size;
		}
		if (totalBytes == 0)
		{
			return;
		}
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			if (size == 0)
			{
				continue;
			}
			auto* domain = getDomainEntry(span.domain);
			if (domain != nullptr)
			{
				domain->waitMs += waitMs * ((double)size / (double)totalBytes);
			}
		}
	};
	const auto addDomainGrowth =
		[&](SceneUploadBufferKind kind, uint64_t requestedBytes, uint64_t allocatedBytes)
	{
		if (domainSpans == nullptr || requestedBytes == 0 || allocatedBytes == 0)
		{
			return;
		}
		uint64_t totalBytes = 0;
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			totalBytes += size;
		}
		if (totalBytes == 0)
		{
			return;
		}
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			if (size == 0)
			{
				continue;
			}
			auto* domain = getDomainEntry(span.domain);
			if (domain != nullptr)
			{
				domain->growthEvents++;
				domain->growthRequestedBytes += (uint64_t)((double)requestedBytes * ((double)size / (double)totalBytes));
				domain->growthAllocatedBytes += (uint64_t)((double)allocatedBytes * ((double)size / (double)totalBytes));
			}
		}
	};
	const auto buildProducerPayloadHash =
		[&](SceneUploadBufferKind kind, uint64_t payloadSize, uint32_t payloadStride, uint64_t extraIdentity, uint64_t& outHash) -> bool
	{
		mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampChecks++;
		if (domainSpans == nullptr)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampFallbacks++;
			return false;
		}
		uint64_t coveredBytes = 0;
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, (uint64_t)kind);
		hash = nri_scene::HashCombine64(hash, payloadSize);
		hash = nri_scene::HashCombine64(hash, payloadStride);
		hash = nri_scene::HashCombine64(hash, extraIdentity);
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			if (size == 0)
			{
				continue;
			}
			uint64_t stamp = 0;
			switch (kind)
			{
			case SceneUploadBufferKind::Vertex:
				stamp = span.stamp.vertexPayloadStamp;
				break;
			case SceneUploadBufferKind::Index:
				stamp = span.stamp.indexPayloadStamp;
				break;
			case SceneUploadBufferKind::Primitive:
				stamp = span.stamp.primitivePayloadStamp;
				break;
			case SceneUploadBufferKind::Material:
				stamp = span.stamp.materialPayloadStamp;
				break;
			}
			if (stamp == 0)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampFallbacks++;
				return false;
			}
			coveredBytes += size;
			hash = nri_scene::HashCombine64(hash, (uint64_t)span.domain);
			hash = nri_scene::HashCombine64(hash, offset);
			hash = nri_scene::HashCombine64(hash, size);
			hash = nri_scene::HashCombine64(hash, stamp);
		}
		if (coveredBytes != payloadSize)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampFallbacks++;
			return false;
		}
		outHash = hash != 0 ? hash : 1;
		mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampUses++;
		switch (kind)
		{
		case SceneUploadBufferKind::Vertex:
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampVertexUses++;
			break;
		case SceneUploadBufferKind::Index:
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampIndexUses++;
			break;
		case SceneUploadBufferKind::Primitive:
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampPrimitiveUses++;
			break;
		case SceneUploadBufferKind::Material:
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampMaterialUses++;
			break;
		}
		return true;
	};
	const auto buildProducerProvenanceHash =
		[&](uint64_t primitiveCount, uint64_t& outHash) -> bool
	{
		mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampChecks++;
		if (domainSpans == nullptr)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampFallbacks++;
			return false;
		}
		uint64_t coveredPrimitives = 0;
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, primitiveCount);
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			if (span.primitiveCount == 0)
			{
				continue;
			}
			if (span.stamp.primitiveProvenanceStamp == 0)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampFallbacks++;
				return false;
			}
			coveredPrimitives += span.primitiveCount;
			hash = nri_scene::HashCombine64(hash, (uint64_t)span.domain);
			hash = nri_scene::HashCombine64(hash, (uint64_t)span.primitiveOffset);
			hash = nri_scene::HashCombine64(hash, (uint64_t)span.primitiveCount);
			hash = nri_scene::HashCombine64(hash, span.stamp.primitiveProvenanceStamp);
		}
		if (coveredPrimitives != primitiveCount)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampFallbacks++;
			return false;
		}
		outHash = hash != 0 ? hash : 1;
		mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampUses++;
		return true;
	};
	const uint64_t primitiveInputSize = geometry.primitives.size() * sizeof(nri_scene::PrimitiveData);
	uint64_t primitiveInputPayloadHash = 0;
	uint64_t primitiveProvenanceHash = 0;
	uint64_t primitiveVisibilityIdentityHash = 0;
	const std::vector<nri_scene::PrimitiveData>* gpuPrimitives = nullptr;
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteMs);
		if (buildProducerPayloadHash(SceneUploadBufferKind::Primitive, primitiveInputSize, sizeof(nri_scene::PrimitiveData), 0, primitiveInputPayloadHash))
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampRewritePrimitiveUses++;
		}
		else
		{
			ScopedPtPerfTimer hashTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewritePrimitiveHashMs);
			primitiveInputPayloadHash = HashUploadPayloadBytes(geometry.primitives.data(), primitiveInputSize);
		}
		if (buildProducerProvenanceHash((uint64_t)geometry.primitiveProvenance.size(), primitiveProvenanceHash))
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampRewriteProvenanceUses++;
		}
		else
		{
			ScopedPtPerfTimer hashTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteProvenanceHashMs);
			primitiveProvenanceHash = HashPrimitiveRewriteProvenancePayload(geometry.primitiveProvenance);
		}
		{
			ScopedPtPerfTimer hashTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteVisibilityHashMs);
			primitiveVisibilityIdentityHash = HashPrimitiveRewriteVisibilityIdentity(mMapWorld);
		}
		mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheChecks++;
		if (mSelectPrimitiveRewriteCache.valid &&
			mSelectPrimitiveRewriteCache.primitivePayloadHash == primitiveInputPayloadHash &&
			mSelectPrimitiveRewriteCache.primitiveProvenanceHash == primitiveProvenanceHash &&
			mSelectPrimitiveRewriteCache.visibilityIdentityHash == primitiveVisibilityIdentityHash &&
			mSelectPrimitiveRewriteCache.primitiveCount == geometry.primitives.size() &&
			mSelectPrimitiveRewriteCache.primitives.size() == geometry.primitives.size())
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheHits++;
			gpuPrimitives = &mSelectPrimitiveRewriteCache.primitives;
		}
		else
		{
			if (!mSelectPrimitiveRewriteCache.valid)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheRejectInvalid++;
			}
			else
			{
				if (mSelectPrimitiveRewriteCache.primitivePayloadHash != primitiveInputPayloadHash)
				{
					mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheRejectPrimitive++;
				}
				if (mSelectPrimitiveRewriteCache.primitiveProvenanceHash != primitiveProvenanceHash)
				{
					mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheRejectProvenance++;
				}
				if (mSelectPrimitiveRewriteCache.visibilityIdentityHash != primitiveVisibilityIdentityHash)
				{
					mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheRejectVisibility++;
				}
				if (mSelectPrimitiveRewriteCache.primitiveCount != geometry.primitives.size() ||
					mSelectPrimitiveRewriteCache.primitives.size() != geometry.primitives.size())
				{
					mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheRejectCount++;
				}
			}
			mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheMisses++;
			{
				ScopedPtPerfTimer copyTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCopyMs);
				mSelectPrimitiveRewriteCache.primitives.assign(geometry.primitives.begin(), geometry.primitives.end());
			}
			{
				ScopedPtPerfTimer resolveTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteResolveMs);
				std::vector<nri_scene::PrimitiveData>& rewrittenPrimitives = mSelectPrimitiveRewriteCache.primitives;
				const size_t primitiveCount = std::min(rewrittenPrimitives.size(), geometry.primitiveProvenance.size());
				for (size_t primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
				{
					const nri_scene::SurfaceProvenance& provenance = geometry.primitiveProvenance[primitiveIndex];
					mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteResolvePrimitives++;
					int32_t chunkIndex = provenance.mapChunkIndex;
					if (chunkIndex >= 0)
					{
						mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteResolveMapChunk++;
					}
					else
					{
						mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteResolveSectorFallback++;
						chunkIndex = nri_static_scene_geometry::FindMapChunkIndexForSector(mMapWorld, provenance.sectorIndex);
						if (chunkIndex < 0)
						{
							mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteResolveSectorMiss++;
						}
					}
					rewrittenPrimitives[primitiveIndex].reserved0 = chunkIndex >= 0 ? (uint32_t)chunkIndex : UINT32_MAX;
				}
				for (size_t primitiveIndex = primitiveCount; primitiveIndex < rewrittenPrimitives.size(); ++primitiveIndex)
				{
					rewrittenPrimitives[primitiveIndex].reserved0 = UINT32_MAX;
				}
			}

			{
				ScopedPtPerfTimer storeTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteStoreMs);
				mSelectPrimitiveRewriteCache.valid = true;
				mSelectPrimitiveRewriteCache.primitivePayloadHash = primitiveInputPayloadHash;
				mSelectPrimitiveRewriteCache.primitiveProvenanceHash = primitiveProvenanceHash;
				mSelectPrimitiveRewriteCache.visibilityIdentityHash = primitiveVisibilityIdentityHash;
				mSelectPrimitiveRewriteCache.primitiveCount = geometry.primitives.size();
			}
			gpuPrimitives = &mSelectPrimitiveRewriteCache.primitives;
		}
	}

	const uint64_t vertexSize = geometry.vertices.size() * sizeof(nri_scene::SceneVertex);
	const uint64_t indexSize = geometry.indices.size() * sizeof(uint32_t);
	const uint64_t primitiveSize = gpuPrimitives != nullptr ? gpuPrimitives->size() * sizeof(nri_scene::PrimitiveData) : 0;
	const uint64_t materialSize = materials.size() * sizeof(nri_scene::MaterialData);
	uint64_t vertexPayloadHash = 0;
	uint64_t indexPayloadHash = 0;
	uint64_t primitivePayloadHash = 0;
	uint64_t materialPayloadHash = 0;
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMs);
		if (!buildProducerPayloadHash(SceneUploadBufferKind::Vertex, vertexSize, sizeof(nri_scene::SceneVertex), 0, vertexPayloadHash))
		{
			vertexPayloadHash = HashUploadPayloadBytes(geometry.vertices.data(), vertexSize);
		}
		if (!buildProducerPayloadHash(SceneUploadBufferKind::Index, indexSize, sizeof(uint32_t), 0, indexPayloadHash))
		{
			indexPayloadHash = HashUploadPayloadBytes(geometry.indices.data(), indexSize);
		}
		if (!buildProducerPayloadHash(SceneUploadBufferKind::Primitive, primitiveSize, sizeof(nri_scene::PrimitiveData), primitiveVisibilityIdentityHash, primitivePayloadHash))
		{
			primitivePayloadHash = HashUploadPayloadBytes(gpuPrimitives != nullptr && !gpuPrimitives->empty() ? gpuPrimitives->data() : nullptr, primitiveSize);
		}
		if (!buildProducerPayloadHash(SceneUploadBufferKind::Material, materialSize, sizeof(nri_scene::MaterialData), 0, materialPayloadHash))
		{
			materialPayloadHash = HashUploadPayloadBytes(materials.data(), materialSize);
		}
	}

	// Scene upload buffers are ringed by queued frame. The frame shell waits before
	// reusing a queued-frame slot, so the selected slot is safe to overwrite here.
	bool waitedForWrites = true;
	const auto notePayloadHashState =
		[&](const NRIBufferResource& resource,
			uint64_t payloadHash,
			uint64_t payloadSize,
			uint32_t payloadStride,
			uint32_t& bufferHitCount,
			uint32_t& bufferSkipCount,
			uint32_t& bufferMissCount) -> bool
	{
		mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashChecks++;
		if (resource.buffer == nullptr || resource.shaderView == nullptr || resource.payloadHash == 0)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMisses++;
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashRejectMissing++;
			bufferMissCount++;
			return false;
		}
		const uint64_t requiredSize = std::max<uint64_t>(payloadSize, payloadStride);
		if (resource.size < requiredSize ||
			resource.usedSize != payloadSize ||
			resource.payloadSize != payloadSize)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMisses++;
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashRejectSize++;
			bufferMissCount++;
			return false;
		}
		if (resource.stride != payloadStride ||
			resource.payloadStride != payloadStride)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMisses++;
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashRejectStride++;
			bufferMissCount++;
			return false;
		}
		if (resource.payloadHash == payloadHash)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashHits++;
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashSkips++;
			bufferHitCount++;
			bufferSkipCount++;
			return true;
		}

		mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMisses++;
		bufferMissCount++;
		return false;
	};
	struct SceneUploadDirtyRangeStats
	{
		uint32_t rawRanges = 0;
		uint32_t coalescedRanges = 0;
		uint32_t rejectedCoalesces = 0;
		uint64_t changedBytes = 0;
		uint64_t uploadedBytes = 0;
		uint64_t gapBytes = 0;
	};
	const auto noteDirtyRanges =
		[&](const std::vector<uint8_t>& mirror,
			const void* bufferData,
			uint64_t bufferSize,
			bool skipUpload,
			bool forceFullDirty,
			std::vector<SceneUploadDirtyRange>* outRanges) -> SceneUploadDirtyRangeStats
	{
		if (outRanges != nullptr)
		{
			outRanges->clear();
		}
		SceneUploadDirtyRangeStats result = {};
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeChecks++;
		if (skipUpload)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeSkips++;
			return result;
		}
		if (bufferSize == 0)
		{
			return result;
		}
		if (forceFullDirty || bufferData == nullptr || mirror.empty() || mirror.size() != bufferSize)
		{
			if (forceFullDirty || bufferData == nullptr)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeForcedFull++;
			}
			if (mirror.empty())
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeMissingMirror++;
			}
			else if (mirror.size() != bufferSize)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeSizeMismatch++;
			}
			result.rawRanges = 1;
			result.coalescedRanges = 1;
			result.changedBytes = bufferSize;
			result.uploadedBytes = bufferSize;
			if (outRanges != nullptr)
			{
				outRanges->push_back({ 0, bufferSize });
			}
			return result;
		}

		const uint64_t maxGapBytes = (uint64_t)(int)nri_ptscenebufferdirtyrangegap;
		const uint8_t* current = static_cast<const uint8_t*>(bufferData);
		const uint8_t* previous = mirror.data();
		const size_t byteCount = (size_t)bufferSize;
		bool hasCoalescedRange = false;
		size_t coalescedStart = 0;
		size_t coalescedEnd = 0;
		size_t cursor = 0;
		while (cursor < byteCount)
		{
			while (cursor < byteCount && current[cursor] == previous[cursor])
			{
				cursor++;
			}
			if (cursor >= byteCount)
			{
				break;
			}
			const size_t rangeStart = cursor;
			while (cursor < byteCount && current[cursor] != previous[cursor])
			{
				cursor++;
			}
			const size_t rangeEnd = cursor;
			result.rawRanges++;
			result.changedBytes += (uint64_t)(rangeEnd - rangeStart);
			if (!hasCoalescedRange)
			{
				hasCoalescedRange = true;
				coalescedStart = rangeStart;
				coalescedEnd = rangeEnd;
				result.coalescedRanges = 1;
				continue;
			}

			const uint64_t gapBytes = (uint64_t)(rangeStart - coalescedEnd);
			if (gapBytes <= maxGapBytes)
			{
				result.gapBytes += gapBytes;
				coalescedEnd = rangeEnd;
			}
			else
			{
				result.uploadedBytes += (uint64_t)(coalescedEnd - coalescedStart);
				if (outRanges != nullptr)
				{
					outRanges->push_back({ (uint64_t)coalescedStart, (uint64_t)(coalescedEnd - coalescedStart) });
				}
				result.rejectedCoalesces++;
				result.coalescedRanges++;
				coalescedStart = rangeStart;
				coalescedEnd = rangeEnd;
			}
		}
		if (hasCoalescedRange)
		{
			result.uploadedBytes += (uint64_t)(coalescedEnd - coalescedStart);
			if (outRanges != nullptr)
			{
				outRanges->push_back({ (uint64_t)coalescedStart, (uint64_t)(coalescedEnd - coalescedStart) });
			}
		}
		return result;
	};
	const auto addDirtyRangeStats =
		[&](const SceneUploadDirtyRangeStats& dirtyStats,
			uint32_t& bufferRangeCount,
			uint64_t& bufferChangedBytes,
			uint64_t& bufferUploadedBytes)
	{
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeRawRanges += dirtyStats.rawRanges;
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeCoalescedRanges += dirtyStats.coalescedRanges;
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeRejectedCoalesces += dirtyStats.rejectedCoalesces;
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeChangedBytes += dirtyStats.changedBytes;
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeUploadedBytes += dirtyStats.uploadedBytes;
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeGapBytes += dirtyStats.gapBytes;
		bufferRangeCount = dirtyStats.coalescedRanges;
		bufferChangedBytes = dirtyStats.changedBytes;
		bufferUploadedBytes = dirtyStats.uploadedBytes;
	};
	const auto updatePayloadMirror =
		[](std::vector<uint8_t>& mirror, const void* bufferData, uint64_t bufferSize)
	{
		if (bufferData == nullptr || bufferSize == 0)
		{
			mirror.clear();
			return;
		}
		const uint8_t* bytes = static_cast<const uint8_t*>(bufferData);
		mirror.assign(bytes, bytes + (size_t)bufferSize);
	};
	const auto updatePayloadMirrorRanges =
		[](std::vector<uint8_t>& mirror, const void* bufferData, const std::vector<SceneUploadDirtyRange>& ranges)
	{
		if (bufferData == nullptr || mirror.empty())
		{
			return;
		}
		const uint8_t* bytes = static_cast<const uint8_t*>(bufferData);
		for (const SceneUploadDirtyRange& range : ranges)
		{
			if (range.size != 0 && range.byteOffset <= mirror.size() && range.size <= mirror.size() - range.byteOffset)
			{
				std::memcpy(mirror.data() + range.byteOffset, bytes + range.byteOffset, (size_t)range.size);
			}
		}
	};
	const auto updateStructuredBufferRanges =
		[&](NRIBufferResource& resource,
			SceneBufferDebugStats& stats,
			std::vector<uint8_t>& payloadMirror,
			const void* bufferData,
			uint64_t bufferSize,
			uint32_t bufferStride,
			uint64_t payloadHash,
			const std::vector<SceneUploadDirtyRange>& ranges,
			nri::AccessStage afterAccess,
			double& uploadMs,
			uint64_t& uploadedBytes,
			uint32_t& growEvents,
			uint32_t& overwriteEvents,
			uint32_t& bufferRangeUploadCount) -> bool
	{
		const uint8_t* bytes = static_cast<const uint8_t*>(bufferData);
		uint64_t rangeBytes = 0;
		for (const SceneUploadDirtyRange& range : ranges)
		{
			if (bytes == nullptr ||
				range.size == 0 ||
				range.byteOffset > bufferSize ||
				range.size > bufferSize - range.byteOffset ||
				range.byteOffset > resource.size ||
				range.size > resource.size - range.byteOffset)
			{
				return false;
			}
			rangeBytes += range.size;
		}

		bool result = true;
		{
			ScopedPtPerfTimer perfTimer(uploadMs);
			for (const SceneUploadDirtyRange& range : ranges)
			{
				void* mapped = mFrameBuffer->mCore.MapBuffer(*resource.buffer, range.byteOffset, range.size);
				if (mapped == nullptr)
				{
					result = false;
					break;
				}
				std::memcpy(mapped, bytes + range.byteOffset, (size_t)range.size);
				mFrameBuffer->mCore.UnmapBuffer(*resource.buffer);
			}

			if (result && mFrameBuffer->mCommandBuffer != nullptr && afterAccess.access != nri::AccessBits::NONE)
			{
				nri::BufferBarrierDesc barrier = {};
				barrier.buffer = resource.buffer;
				barrier.before = {};
				barrier.after = afterAccess;

				nri::BarrierDesc barrierDesc = {};
				barrierDesc.buffers = &barrier;
				barrierDesc.bufferNum = 1;
				mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrierDesc);
			}
		}
		if (!result)
		{
			return false;
		}

		stats.bytesUploadedLastFrame = rangeBytes;
		stats.growEventsLastFrame = 0;
		stats.overwriteEventsLastFrame = 1;
		stats.growthOldBytesLastFrame = 0;
		stats.growthRequestedBytesLastFrame = 0;
		stats.growthAllocatedBytesLastFrame = 0;
		stats.uploadCount++;
		stats.overwriteCount++;
		stats.peakUsedBytes = std::max(stats.peakUsedBytes, bufferSize);
		NotePerfBufferUpload(&stats, rangeBytes, false, "scene_buffer_upload_range", -1);

		resource.usedSize = bufferSize;
		resource.payloadHash = payloadHash;
		resource.payloadSize = bufferSize;
		resource.payloadStride = bufferStride;
		updatePayloadMirrorRanges(payloadMirror, bufferData, ranges);

		uploadedBytes = rangeBytes;
		growEvents = 0;
		overwriteEvents = 1;
		mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashUploads++;
		mLastPerfShellTraceStats.sceneSelectBufferUploadRangeUploads++;
		mLastPerfShellTraceStats.sceneSelectBufferUploadRangeUploadedBytes += rangeBytes;
		bufferRangeUploadCount++;
		return true;
	};
	const auto ensureStructuredBufferBatched =
		[&](NRIBufferResource& resource,
			SceneBufferDebugStats& stats,
			std::vector<uint8_t>& payloadMirror,
			std::vector<SceneUploadDirtyRange>* dirtyRangeScratch,
			const void* bufferData,
			uint64_t bufferSize,
			uint32_t bufferStride,
			uint64_t payloadHash,
			bool skipUpload,
			bool allowRangeUpload,
			nri::BufferUsageBits usageBits,
			nri::AccessStage afterAccess,
			double& uploadMs,
			uint64_t& uploadedBytes,
			uint32_t& growEvents,
			uint32_t& overwriteEvents,
			uint32_t& dirtyRanges,
			uint64_t& dirtyChangedBytes,
			uint64_t& dirtyUploadedBytes,
			uint32_t& bufferRangeUploadCount,
			SceneUploadBufferKind bufferKind) -> bool
	{
		const uint64_t requiredSize = std::max<uint64_t>(bufferSize, bufferStride);
		const bool forceFullDirty =
			resource.buffer == nullptr ||
			resource.shaderView == nullptr ||
			resource.memoryLocation != nri::MemoryLocation::DEVICE_UPLOAD ||
			resource.payloadHash == 0 ||
			resource.size < requiredSize ||
			resource.usedSize != bufferSize ||
			resource.payloadSize != bufferSize ||
			resource.stride != bufferStride ||
			resource.payloadStride != bufferStride;
		const bool traceDirtyRanges = ShouldTraceSceneBufferDirtyRanges();
		SceneUploadDirtyRangeStats dirtyStats = {};
		const bool collectDirtyRanges = allowRangeUpload && (traceDirtyRanges || (!skipUpload && !forceFullDirty));
		if (!collectDirtyRanges)
		{
			if (dirtyRangeScratch != nullptr)
			{
				dirtyRangeScratch->clear();
			}
			dirtyRanges = 0;
			dirtyChangedBytes = 0;
			dirtyUploadedBytes = 0;
		}
		else
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeMs);
			dirtyStats = noteDirtyRanges(payloadMirror, bufferData, bufferSize, skipUpload, forceFullDirty, dirtyRangeScratch);
			addDirtyRangeStats(dirtyStats, dirtyRanges, dirtyChangedBytes, dirtyUploadedBytes);
			if (dirtyRangeScratch != nullptr)
			{
				addDomainRangeBytes(bufferKind, *dirtyRangeScratch, true);
			}
		}

		if (skipUpload)
		{
			resource.usedSize = bufferSize;
			uploadedBytes = 0;
			growEvents = 0;
			overwriteEvents = 0;
			return true;
		}

		const bool canRangeUpload =
			allowRangeUpload &&
			!forceFullDirty &&
			dirtyRangeScratch != nullptr &&
			!dirtyRangeScratch->empty() &&
			dirtyStats.uploadedBytes != 0 &&
			dirtyStats.uploadedBytes < bufferSize;
		bool useRangeUpload = false;
		if (canRangeUpload)
		{
			const uint32_t maxRangeCount = (uint32_t)(int)nri_ptscenebufferrangeuploadmaxranges;
			const uint32_t maxUploadPercent = (uint32_t)(int)nri_ptscenebufferrangeuploadmaxpercent;
			if (dirtyStats.coalescedRanges > maxRangeCount)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadRangeFallbacks++;
				mLastPerfShellTraceStats.sceneSelectBufferUploadRangeFallbackFragmented++;
			}
			else if (dirtyStats.uploadedBytes * 100u >= bufferSize * maxUploadPercent)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadRangeFallbacks++;
				mLastPerfShellTraceStats.sceneSelectBufferUploadRangeFallbackLarge++;
			}
			else
			{
				useRangeUpload = true;
			}
		}
		else if (allowRangeUpload && !forceFullDirty && dirtyRangeScratch != nullptr && dirtyRangeScratch->empty() && dirtyStats.changedBytes == 0)
		{
			resource.usedSize = bufferSize;
			resource.payloadHash = payloadHash;
			resource.payloadSize = bufferSize;
			resource.payloadStride = bufferStride;
			uploadedBytes = 0;
			growEvents = 0;
			overwriteEvents = 0;
			return true;
		}

		bool needsWait = false;
		if (!waitedForWrites)
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadWaitCheckMs);
			needsWait = StructuredBufferUpdateNeedsWait(resource, bufferData, bufferSize, bufferStride);
		}
		if (!waitedForWrites && needsWait)
		{
			const auto waitStart = std::chrono::steady_clock::now();
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadWaitMs);
			mLastPerfShellTraceStats.sceneSelectBufferUploadWaitCount++;
			WaitForCommandsTracked("scene_buffer_upload");
			addDomainWait(bufferKind, DurationMs(waitStart, std::chrono::steady_clock::now()));
			waitedForWrites = true;
		}

		if (useRangeUpload)
		{
			if (updateStructuredBufferRanges(resource, stats, payloadMirror, bufferData, bufferSize, bufferStride, payloadHash, *dirtyRangeScratch, afterAccess, uploadMs, uploadedBytes, growEvents, overwriteEvents, bufferRangeUploadCount))
			{
				addDomainRangeBytes(bufferKind, *dirtyRangeScratch, false);
				return true;
			}
			mLastPerfShellTraceStats.sceneSelectBufferUploadRangeFallbacks++;
		}

		bool result = false;
		{
			ScopedPtPerfTimer perfTimer(uploadMs);
			result = EnsureStructuredBuffer(resource, stats, bufferData, bufferSize, bufferStride, usageBits, afterAccess, waitedForWrites, "scene_buffer_upload");
		}
		uploadedBytes = stats.bytesUploadedLastFrame;
		growEvents = stats.growEventsLastFrame;
		overwriteEvents = stats.overwriteEventsLastFrame;
		if (result)
		{
			if (stats.growEventsLastFrame != 0)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadGrowthEvents += stats.growEventsLastFrame;
				mLastPerfShellTraceStats.sceneSelectBufferUploadGrowthOldBytes += stats.growthOldBytesLastFrame;
				mLastPerfShellTraceStats.sceneSelectBufferUploadGrowthRequestedBytes += stats.growthRequestedBytesLastFrame;
				mLastPerfShellTraceStats.sceneSelectBufferUploadGrowthAllocatedBytes += stats.growthAllocatedBytesLastFrame;
				if (stats.growthAllocatedBytesLastFrame > stats.growthRequestedBytesLastFrame)
				{
					mLastPerfShellTraceStats.sceneSelectBufferUploadGrowthHeadroomBytes +=
						stats.growthAllocatedBytesLastFrame - stats.growthRequestedBytesLastFrame;
				}
				addDomainGrowth(bufferKind, stats.growthRequestedBytesLastFrame, stats.growthAllocatedBytesLastFrame);
			}
			resource.payloadHash = payloadHash;
			resource.payloadSize = bufferSize;
			resource.payloadStride = bufferStride;
			updatePayloadMirror(payloadMirror, bufferData, bufferSize);
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashUploads++;
			addDomainFullUpload(bufferKind);
		}
		return result;
	};

	const bool skipVertexUpload = notePayloadHashState(vertexBuffer, vertexPayloadHash, vertexSize, sizeof(nri_scene::SceneVertex), mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashVertexHits, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashVertexSkips, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashVertexMisses);
	const bool skipIndexUpload = notePayloadHashState(indexBuffer, indexPayloadHash, indexSize, sizeof(uint32_t), mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashIndexHits, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashIndexSkips, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashIndexMisses);
	const bool skipPrimitiveUpload = notePayloadHashState(primitiveBuffer, primitivePayloadHash, primitiveSize, sizeof(nri_scene::PrimitiveData), mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashPrimitiveHits, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashPrimitiveSkips, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashPrimitiveMisses);
	const bool skipMaterialUpload = notePayloadHashState(materialBuffer, materialPayloadHash, materialSize, sizeof(nri_scene::MaterialData), mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMaterialHits, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMaterialSkips, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMaterialMisses);
	addDomainPayload(SceneUploadBufferKind::Vertex, skipVertexUpload);
	addDomainPayload(SceneUploadBufferKind::Index, skipIndexUpload);
	addDomainPayload(SceneUploadBufferKind::Primitive, skipPrimitiveUpload);
	addDomainPayload(SceneUploadBufferKind::Material, skipMaterialUpload);
	uint32_t ignoredRangeUploadCount = 0;

	return
		ensureStructuredBufferBatched(vertexBuffer, mVertexBufferStats, vertexMirror, nullptr, geometry.vertices.data(), vertexSize, sizeof(nri_scene::SceneVertex), vertexPayloadHash, skipVertexUpload, false, NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), NRIResourceAccelerationStructureBuildInputAccess(), mLastPerfShellTraceStats.sceneSelectBufferUploadVertexMs, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexGrowEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexOverwriteEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexDirtyRanges, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexDirtyChangedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexDirtyUploadedBytes, ignoredRangeUploadCount, SceneUploadBufferKind::Vertex) &&
		ensureStructuredBufferBatched(indexBuffer, mIndexBufferStats, indexMirror, nullptr, geometry.indices.data(), indexSize, sizeof(uint32_t), indexPayloadHash, skipIndexUpload, false, NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), NRIResourceAccelerationStructureBuildInputAccess(), mLastPerfShellTraceStats.sceneSelectBufferUploadIndexMs, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexGrowEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexOverwriteEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexDirtyRanges, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexDirtyChangedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexDirtyUploadedBytes, ignoredRangeUploadCount, SceneUploadBufferKind::Index) &&
		ensureStructuredBufferBatched(primitiveBuffer, mPrimitiveBufferStats, primitiveMirror, &mSceneUploadPrimitiveDirtyRangeScratch, gpuPrimitives != nullptr && !gpuPrimitives->empty() ? gpuPrimitives->data() : nullptr, primitiveSize, sizeof(nri_scene::PrimitiveData), primitivePayloadHash, skipPrimitiveUpload, true, nri::BufferUsageBits::SHADER_RESOURCE, NRIResourceComputeShaderResourceAccess(), mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveMs, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveGrowEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveOverwriteEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveDirtyRanges, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveDirtyChangedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveDirtyUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRangeUploads, SceneUploadBufferKind::Primitive) &&
		ensureStructuredBufferBatched(materialBuffer, mMaterialBufferStats, materialMirror, &mSceneUploadMaterialDirtyRangeScratch, materials.data(), materialSize, sizeof(nri_scene::MaterialData), materialPayloadHash, skipMaterialUpload, true, nri::BufferUsageBits::SHADER_RESOURCE, NRIResourceComputeShaderResourceAccess(), mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialMs, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialGrowEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialOverwriteEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialDirtyRanges, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialDirtyChangedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialDirtyUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialRangeUploads, SceneUploadBufferKind::Material);
}


bool NRIRenderer::UpdateEmissiveSamplingBuffers(const EmissiveSamplingBuildContext& context, bool* ioWaitedForWrites)
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.emissiveUpdateMs);
	const uint64_t payloadHash = mSceneLights.BuildEmissiveSamplingPayloadHash(context);
	const uint64_t sectorResponsePayloadHash = mSceneLights.BuildEmissiveSectorResponsePayloadHash();
	const bool sectorResponseChanged =
		mEmissiveSectorResponsePayloadCacheValid &&
		mEmissiveSectorResponsePayloadHash != sectorResponsePayloadHash;
	if (mEmissiveSamplingPayloadCacheValid &&
		mEmissiveSamplingPayloadHash == payloadHash &&
		mEmissivePrimitiveHeaderBuffer.shaderView != nullptr &&
		mEmissivePrimitiveBuffer.shaderView != nullptr &&
		mEmissivePrimitiveCdfBuffer.shaderView != nullptr &&
		mEmissiveMaterialResponseBuffer.shaderView != nullptr)
	{
		if (!mEmissiveSectorResponsePayloadCacheValid)
		{
			mEmissiveSectorResponsePayloadCacheValid = true;
			mEmissiveSectorResponsePayloadHash = sectorResponsePayloadHash;
		}
		return true;
	}

	NRIEmissivePrimitiveHeaderGpuData emissiveHeader = {};
	std::vector<NRIEmissivePrimitiveGpuData> emissivePrimitives;
	std::vector<float> emissiveCdf;
	std::vector<NRIEmissiveMaterialResponseGpuData> emissiveMaterialResponses;
	std::vector<NRIEmissivePrimitiveDebugRecord> emissiveDebugRecords;
	mSceneLights.BuildEmissiveSamplingUpload(context, emissiveHeader, emissivePrimitives, emissiveCdf, emissiveMaterialResponses, emissiveDebugRecords);

	const auto ensureStructuredBufferBatched = [this, ioWaitedForWrites](NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after) -> bool
	{
		if (ioWaitedForWrites != nullptr &&
			!*ioWaitedForWrites &&
			StructuredBufferUpdateNeedsWait(resource, data, size, stride))
		{
			WaitForCommandsTracked("emissive_sampling_upload");
			*ioWaitedForWrites = true;
		}

		return EnsureStructuredBuffer(resource, stats, data, size, stride, usage, after, ioWaitedForWrites != nullptr && *ioWaitedForWrites, "emissive_sampling_upload");
	};

	if (!ensureStructuredBufferBatched(
		mEmissivePrimitiveHeaderBuffer,
		mEmissivePrimitiveHeaderBufferStats,
		&emissiveHeader,
		sizeof(emissiveHeader),
		sizeof(NRIEmissivePrimitiveHeaderGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!ensureStructuredBufferBatched(
		mEmissivePrimitiveBuffer,
		mEmissivePrimitiveBufferStats,
		emissivePrimitives.empty() ? nullptr : emissivePrimitives.data(),
		emissivePrimitives.empty() ? 0u : emissivePrimitives.size() * sizeof(NRIEmissivePrimitiveGpuData),
		sizeof(NRIEmissivePrimitiveGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!ensureStructuredBufferBatched(
		mEmissivePrimitiveCdfBuffer,
		mEmissivePrimitiveCdfBufferStats,
		emissiveCdf.data(),
		emissiveCdf.size() * sizeof(float),
		sizeof(float),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!ensureStructuredBufferBatched(
		mEmissiveMaterialResponseBuffer,
		mEmissiveMaterialResponseBufferStats,
		emissiveMaterialResponses.empty() ? nullptr : emissiveMaterialResponses.data(),
		emissiveMaterialResponses.empty() ? 0u : emissiveMaterialResponses.size() * sizeof(NRIEmissiveMaterialResponseGpuData),
		sizeof(NRIEmissiveMaterialResponseGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess()))
	{
		return false;
	}

	mBoundEmissivePrimitiveCount = emissiveHeader.activeCount;
	mBoundEmissiveTotalPower = emissiveHeader.totalPower;
	mBoundEmissiveDominantPrimitive = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissiveDebugRecords.size() ? emissiveDebugRecords[emissiveHeader.dominantIndex].primitiveIndex : UINT32_MAX;
	mBoundEmissiveDominantTile = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].textureId : 0u;
	mBoundEmissiveDominantFlags = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].sourceFlags : 0u;
	mBoundEmissiveDominantDataSource = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].dataSource : 0u;
	mBoundEmissiveDominantPower = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].powerEstimate : 0.0f;
	mBoundEmissivePrimitiveRecords = std::move(emissiveDebugRecords);

	mSceneDataDescriptors[13] = mEmissivePrimitiveHeaderBuffer.shaderView;
	mSceneDataDescriptors[14] = mEmissivePrimitiveBuffer.shaderView;
	mSceneDataDescriptors[15] = mEmissivePrimitiveCdfBuffer.shaderView;
	mSceneDataDescriptors[25] = mEmissiveMaterialResponseBuffer.shaderView;

	bool descriptorsReady = IsCurrentSceneDataDescriptorsInitialized() && GetCurrentSceneDataSet() != nullptr;
	if (descriptorsReady)
	{
		for (const nri::Descriptor* descriptor : mSceneDataDescriptors)
		{
			if (descriptor == nullptr)
			{
				descriptorsReady = false;
				break;
			}
		}
	}

	if (descriptorsReady)
	{
		NRIDescriptorSetManager::CommitSceneDataDescriptors(*this, "emissive_sampling_refresh");
	}
	if (sectorResponseChanged && nri_runtime_mutation::ShouldTracePtPerf())
	{
		const auto& sectorRegistry = mSceneLights.GetSectorLighting();
		Printf("NRI PT emissive sampling refresh: frame=%u reason=sector-response-change primitives=%u total_power=%.3f dominant_primitive=%u dominant_tile=%u sector_response_hash=0x%016llx->0x%016llx response=boost:%u dim:%u neutral:%u\n",
			mFrameIndex,
			mBoundEmissivePrimitiveCount,
			mBoundEmissiveTotalPower,
			mBoundEmissiveDominantPrimitive,
			mBoundEmissiveDominantTile,
			(unsigned long long)mEmissiveSectorResponsePayloadHash,
			(unsigned long long)sectorResponsePayloadHash,
			sectorRegistry.responseBoostSectorCount,
			sectorRegistry.responseDimSectorCount,
			sectorRegistry.responseNeutralSectorCount);
	}
	mEmissiveSamplingPayloadCacheValid = true;
	mEmissiveSamplingPayloadHash = payloadHash;
	mEmissiveSectorResponsePayloadCacheValid = true;
	mEmissiveSectorResponsePayloadHash = sectorResponsePayloadHash;
	return true;
}

NRIResourceContext NRIRenderer::BuildResourceContext() const
{
	NRIResourceContext context = {};
	context.device = mFrameBuffer != nullptr ? mFrameBuffer->mDevice : nullptr;
	context.core = mFrameBuffer != nullptr ? &mFrameBuffer->mCore : nullptr;
	context.commandBuffer = mFrameBuffer != nullptr ? mFrameBuffer->mCommandBuffer : nullptr;
	return context;
}

NRIResourceServices NRIRenderer::BuildResourceServices()
{
	NRIResourceServices services = {};
	services.context = BuildResourceContext();
	services.user = this;
	services.waitForCommands = [](void* user, const char* reason)
	{
		static_cast<NRIRenderer*>(user)->WaitForCommandsTracked(reason);
	};
	services.destroyBufferResource = [](void* user, NRIBufferResource& resource)
	{
		static_cast<NRIRenderer*>(user)->DestroyBufferResource(resource);
	};
	return services;
}

bool NRIRenderer::CreateStructuredBuffer(NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after)
{
	return NRISceneUploadManager::CreateStructuredBuffer(*this, resource, data, size, stride, usage, after);
}

bool NRIRenderer::EnsureStructuredBuffer(NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, bool writesQuiesced, const char* waitReason)
{
	return NRISceneUploadManager::EnsureStructuredBuffer(*this, resource, stats, data, size, stride, usage, after, writesQuiesced, waitReason);
}

bool NRIRenderer::UpdateStructuredBufferRange(NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after)
{
	return NRISceneUploadManager::UpdateStructuredBufferRange(*this, resource, byteOffset, data, size, after);
}

bool NRIRenderer::CreateBufferWithoutView(NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage)
{
	if (resource.buffer != nullptr)
	{
		WaitForCommandsTracked();
	}

	return CreateBufferWithoutViewAtLocation(resource, size, stride, usage, nri::MemoryLocation::DEVICE);
}

bool NRIRenderer::CreateBufferWithoutViewAtLocation(NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::MemoryLocation memoryLocation)
{
	const NRIResourceContext resourceContext = BuildResourceContext();
	DestroyBufferResource(resource);

	nri::BufferDesc desc = {};
	desc.size = std::max<uint64_t>(size, stride);
	desc.structureStride = stride;
	desc.usage = usage;
	if (resourceContext.core->CreateCommittedBuffer(*resourceContext.device, memoryLocation, 0.0f, desc, resource.buffer) != nri::Result::SUCCESS)
	{
		return false;
	}

	nri::MemoryDesc memoryDesc = {};
	resourceContext.core->GetBufferMemoryDesc(*resource.buffer, memoryLocation, memoryDesc);
	resource.size = desc.size;
	resource.memorySize = memoryDesc.size;
	resource.usedSize = size;
	resource.stride = stride;
	resource.memoryLocation = memoryLocation;
	return true;
}

bool NRIRenderer::EnsureResidentArenaBuffer(NRIBufferResource& resource, uint64_t requiredSize, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after)
{
	const uint64_t alignedRequiredSize = std::max<uint64_t>(requiredSize, stride);
	if (resource.buffer != nullptr &&
		resource.shaderView != nullptr &&
		resource.memoryLocation == nri::MemoryLocation::DEVICE &&
		resource.stride == stride &&
		resource.size >= alignedRequiredSize)
	{
		resource.usedSize = std::max(resource.usedSize, requiredSize);
		return true;
	}

	NRIBufferResource oldResource = resource;
	resource = {};

	const uint64_t grownSize = GetNRIGrownBufferSize(oldResource.size, alignedRequiredSize, stride);
	if (!CreateBufferWithoutViewAtLocation(resource, grownSize, stride, usage, nri::MemoryLocation::DEVICE))
	{
		resource = oldResource;
		return false;
	}

	nri::BufferViewDesc viewDesc = {};
	viewDesc.buffer = resource.buffer;
	viewDesc.type = nri::BufferView::STRUCTURED_BUFFER;
	viewDesc.offset = 0;
	viewDesc.size = nri::WHOLE_SIZE;
	viewDesc.structureStride = stride;
	if (mFrameBuffer->mCore.CreateBufferView(viewDesc, resource.shaderView) != nri::Result::SUCCESS)
	{
		DestroyBufferResource(resource);
		resource = oldResource;
		return false;
	}
	resource.usedSize = requiredSize;

	if (oldResource.buffer != nullptr && mFrameBuffer->mCommandBuffer != nullptr)
	{
		const uint64_t copySize = std::min(oldResource.usedSize, resource.size);
		if (copySize > 0)
		{
			nri::BufferBarrierDesc beforeBarriers[2] = {};
			beforeBarriers[0].buffer = oldResource.buffer;
			beforeBarriers[0].before = after;
			beforeBarriers[0].after = NRIResourceCopySourceAccess();
			beforeBarriers[1].buffer = resource.buffer;
			beforeBarriers[1].before = {};
			beforeBarriers[1].after = NRIResourceCopyDestinationAccess();
			nri::BarrierDesc beforeBarrierDesc = {};
			beforeBarrierDesc.buffers = beforeBarriers;
			beforeBarrierDesc.bufferNum = 2;
			mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, beforeBarrierDesc);

			mFrameBuffer->mCore.CmdCopyBuffer(
				*mFrameBuffer->mCommandBuffer,
				*resource.buffer,
				0,
				*oldResource.buffer,
				0,
				copySize);

			nri::BufferBarrierDesc afterBarrier = {};
			afterBarrier.buffer = resource.buffer;
			afterBarrier.before = NRIResourceCopyDestinationAccess();
			afterBarrier.after = after;
			nri::BarrierDesc afterBarrierDesc = {};
			afterBarrierDesc.buffers = &afterBarrier;
			afterBarrierDesc.bufferNum = 1;
			mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, afterBarrierDesc);
		}

		auto& frameScratch = GetResidentUploadScratchFrame();
		frameScratch.retiredBuffers.push_back(oldResource);
	}
	else if (oldResource.buffer != nullptr || oldResource.shaderView != nullptr)
	{
		DestroyBufferResource(oldResource);
	}

	return true;
}

bool NRIRenderer::EnsureResidentUploadScratchBuffer(ResidentBufferUploadScratch& scratch, ResidentUploadScratchFrame& frameScratch, uint64_t requiredSize)
{
	constexpr uint32_t kResidentUploadScratchStride = 16u;
	const uint64_t alignedRequiredSize = std::max<uint64_t>(requiredSize, kResidentUploadScratchStride);
	if (scratch.buffer.buffer != nullptr &&
		scratch.buffer.memoryLocation == nri::MemoryLocation::DEVICE_UPLOAD &&
		scratch.buffer.size >= alignedRequiredSize)
	{
		return true;
	}

	const uint64_t grownSize = GetNRIGrownBufferSize(scratch.buffer.size, alignedRequiredSize, kResidentUploadScratchStride);
	if (scratch.buffer.buffer != nullptr || scratch.buffer.shaderView != nullptr)
	{
		frameScratch.retiredBuffers.push_back(scratch.buffer);
		scratch.buffer = {};
		scratch.cursor = 0;
		scratch.copySourceActive = false;
	}
	const bool created = CreateBufferWithoutViewAtLocation(
		scratch.buffer,
		grownSize,
		kResidentUploadScratchStride,
		nri::BufferUsageBits::NONE,
		nri::MemoryLocation::DEVICE_UPLOAD);
	if (created)
	{
		mLastPerfShellTraceStats.runtimeMutationResidentApplyStageScratchGrowCount++;
		mLastPerfShellTraceStats.runtimeMutationResidentApplyStageScratchGrowBytes += grownSize;
	}
	return created;
}

bool NRIRenderer::StageResidentBufferCopyRange(NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after, int uploadKind)
{
	if (resource.buffer == nullptr ||
		data == nullptr ||
		size == 0 ||
		byteOffset > resource.size ||
		size > resource.size - byteOffset)
	{
		return false;
	}

	if (mFrameBuffer == nullptr || mFrameBuffer->mCommandBuffer == nullptr)
	{
		return false;
	}

	constexpr uint64_t kResidentUploadScratchAlignment = 16u;
	auto& frameScratch = GetResidentUploadScratchFrame();

	ResidentBufferUploadScratch* scratch = nullptr;
	switch (uploadKind)
	{
	case ResidentUploadKind_Vertex: scratch = &frameScratch.vertex; break;
	case ResidentUploadKind_Index: scratch = &frameScratch.index; break;
	case ResidentUploadKind_Primitive: scratch = &frameScratch.primitive; break;
	case ResidentUploadKind_Material: scratch = &frameScratch.material; break;
	default: return false;
	}

	const uint64_t scratchOffset =
		(scratch->cursor + kResidentUploadScratchAlignment - 1u) &
		~(kResidentUploadScratchAlignment - 1u);
	const uint64_t requiredSize = scratchOffset + size;
	if (!EnsureResidentUploadScratchBuffer(*scratch, frameScratch, requiredSize))
	{
		return false;
	}

	void* mapped = mFrameBuffer->mCore.MapBuffer(*scratch->buffer.buffer, scratchOffset, size);
	if (mapped == nullptr)
	{
		return false;
	}

	std::memcpy(mapped, data, (size_t)size);
	mFrameBuffer->mCore.UnmapBuffer(*scratch->buffer.buffer);

	if (!scratch->copySourceActive)
	{
		nri::BufferBarrierDesc sourceBarrier = {};
		sourceBarrier.buffer = scratch->buffer.buffer;
		sourceBarrier.before = {};
		sourceBarrier.after = NRIResourceCopySourceAccess();

		nri::BarrierDesc sourceBarrierDesc = {};
		sourceBarrierDesc.buffers = &sourceBarrier;
		sourceBarrierDesc.bufferNum = 1;
		mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, sourceBarrierDesc);
		scratch->copySourceActive = true;
	}

	nri::BufferBarrierDesc beforeCopyBarrier = {};
	beforeCopyBarrier.buffer = resource.buffer;
	beforeCopyBarrier.before = after;
	beforeCopyBarrier.after = NRIResourceCopyDestinationAccess();

	nri::BarrierDesc beforeCopyBarrierDesc = {};
	beforeCopyBarrierDesc.buffers = &beforeCopyBarrier;
	beforeCopyBarrierDesc.bufferNum = 1;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, beforeCopyBarrierDesc);

	mFrameBuffer->mCore.CmdCopyBuffer(
		*mFrameBuffer->mCommandBuffer,
		*resource.buffer,
		byteOffset,
		*scratch->buffer.buffer,
		scratchOffset,
		size);

	nri::BufferBarrierDesc afterCopyBarrier = {};
	afterCopyBarrier.buffer = resource.buffer;
	afterCopyBarrier.before = NRIResourceCopyDestinationAccess();
	afterCopyBarrier.after = after;

	nri::BarrierDesc afterCopyBarrierDesc = {};
	afterCopyBarrierDesc.buffers = &afterCopyBarrier;
	afterCopyBarrierDesc.bufferNum = 1;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, afterCopyBarrierDesc);

	scratch->cursor = scratchOffset + size;
	return true;
}

void NRIRenderer::RetireResidentBufferResource(NRIBufferResource& resource)
{
	if (resource.buffer == nullptr && resource.shaderView == nullptr)
	{
		return;
	}

	if (mFrameBuffer != nullptr &&
		mFrameBuffer->mCommandBuffer != nullptr &&
		!mResidentUploadScratchFrames.empty())
	{
		auto& frameScratch = GetResidentUploadScratchFrame();
		frameScratch.retiredBuffers.push_back(resource);
		resource = {};
		return;
	}

	DestroyBufferResource(resource);
}

void NRIRenderer::RetireResidentAccelerationStructure(NRIAccelerationStructureResource& resource)
{
	if (resource.accelerationStructure == nullptr && resource.descriptor == nullptr)
	{
		return;
	}

	if (mFrameBuffer == nullptr ||
		mFrameBuffer->mCommandBuffer == nullptr ||
		mResidentUploadScratchFrames.empty())
	{
		DestroyAccelerationStructureResource(resource);
		return;
	}

	auto& frameScratch = GetResidentUploadScratchFrame();
	frameScratch.retiredAccelerationStructures.push_back(resource);
	resource = {};
}

void NRIRenderer::RetireTopLevelAccelerationStructure(NRIAccelerationStructureResource& resource)
{
	if (resource.accelerationStructure == nullptr && resource.descriptor == nullptr)
	{
		return;
	}

	RetireResidentAccelerationStructure(resource);
}

bool NRIRenderer::EnsureResidentStructuredBuffer(NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, const char* waitReason, int uploadKind)
{
	const uint64_t requiredSize = std::max<uint64_t>(size, stride);
	const bool needsGrowth =
		resource.buffer == nullptr ||
		resource.shaderView == nullptr ||
		resource.memoryLocation != nri::MemoryLocation::DEVICE ||
		resource.stride != stride ||
		resource.size < requiredSize;

	stats.bytesUploadedLastFrame = size;
	stats.growEventsLastFrame = 0;
	stats.overwriteEventsLastFrame = 0;
	stats.growthOldBytesLastFrame = 0;
	stats.growthRequestedBytesLastFrame = 0;
	stats.growthAllocatedBytesLastFrame = 0;
	stats.uploadCount++;
	stats.peakUsedBytes = std::max(stats.peakUsedBytes, size);
	NotePerfBufferUpload(&stats, size, needsGrowth, waitReason, uploadKind);

	if (needsGrowth)
	{
		const uint64_t oldSize = resource.size;
		const uint64_t grownSize = GetNRIGrownBufferSize(resource.size, requiredSize, stride);
		NRIBufferResource oldResource = resource;
		NRIBufferResource newResource = {};
		if (!CreateBufferWithoutViewAtLocation(newResource, grownSize, stride, usage, nri::MemoryLocation::DEVICE))
		{
			return false;
		}

		nri::BufferViewDesc viewDesc = {};
		viewDesc.buffer = newResource.buffer;
		viewDesc.type = nri::BufferView::STRUCTURED_BUFFER;
		viewDesc.offset = 0;
		viewDesc.size = nri::WHOLE_SIZE;
		viewDesc.structureStride = stride;
		if (mFrameBuffer->mCore.CreateBufferView(viewDesc, newResource.shaderView) != nri::Result::SUCCESS)
		{
			DestroyBufferResource(newResource);
			return false;
		}

		newResource.usedSize = size;
		if (data != nullptr && size != 0)
		{
			if (!StageResidentBufferCopyRange(newResource, 0, data, size, after, uploadKind))
			{
				DestroyBufferResource(newResource);
				return false;
			}
		}

		resource = newResource;
		RetireResidentBufferResource(oldResource);
		stats.growthCount++;
		stats.growEventsLastFrame = 1;
		stats.growthOldBytesLastFrame = oldSize;
		stats.growthRequestedBytesLastFrame = requiredSize;
		stats.growthAllocatedBytesLastFrame = grownSize;
		return true;
	}
	else
	{
		stats.overwriteCount++;
		stats.overwriteEventsLastFrame = 1;
	}

	resource.usedSize = size;
	if (data != nullptr && size != 0)
	{
		if (!StageResidentBufferCopyRange(resource, 0, data, size, after, uploadKind))
		{
			return false;
		}
	}

	return true;
}

bool NRISceneUploadManager::UpdateSceneDataSet(
	NRIRenderer& renderer,
	const NRIBufferResource& staticVertexBuffer,
	const NRIBufferResource& staticIndexBuffer,
	const NRIBufferResource& staticPrimitiveBuffer,
	const NRIBufferResource& staticMaterialBuffer,
	const NRIBufferResource& dynamicVertexBuffer,
	const NRIBufferResource& dynamicIndexBuffer,
	const NRIBufferResource& dynamicPrimitiveBuffer,
	const NRIBufferResource& dynamicMaterialBuffer,
	const std::vector<SceneInstanceData>& sceneInstances,
	uint32_t staticPrimitiveCount,
	uint32_t dynamicPrimitiveCount,
	uint32_t staticMaterialCount,
	uint32_t dynamicMaterialCount,
	const char* reason)
{
	ScopedPtPerfTimer perfTimer(renderer.mLastPerfShellTraceStats.sceneDataSetMs);
	renderer.mLastPerfShellTraceStats.sceneDataSetCalls++;
	renderer.SetCurrentSceneDataDescriptorsInitialized(false);
	bool waitedForWrites = false;
	const auto noteDataSetUpload = [&](const SceneBufferDebugStats& stats, uint64_t size, uint64_t& requestedBytes, uint64_t& uploadedBytes)
	{
		requestedBytes += size;
		uploadedBytes += stats.bytesUploadedLastFrame;
		renderer.mLastPerfShellTraceStats.sceneDataSetResourceGrowEvents += stats.growEventsLastFrame;
		renderer.mLastPerfShellTraceStats.sceneDataSetResourceOverwriteEvents += stats.overwriteEventsLastFrame;
	};
	const auto ensureStructuredBufferBatched = [&](NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, double& uploadMs, uint64_t& requestedBytes, uint64_t& uploadedBytes) -> bool
	{
		bool needsWait = false;
		{
			ScopedPtPerfTimer waitCheckTimer(renderer.mLastPerfShellTraceStats.sceneDataSetWaitCheckMs);
			needsWait = !waitedForWrites && StructuredBufferUpdateNeedsWait(resource, data, size, stride);
		}
		if (needsWait)
		{
			{
				ScopedPtPerfTimer waitTimer(renderer.mLastPerfShellTraceStats.sceneDataSetWaitMs);
				renderer.WaitForCommandsTracked("scene_data_upload");
			}
			renderer.mLastPerfShellTraceStats.sceneDataSetWaitCount++;
			waitedForWrites = true;
		}

		bool updated = false;
		{
			ScopedPtPerfTimer uploadTimer(uploadMs);
			updated = EnsureStructuredBuffer(renderer, resource, stats, data, size, stride, usage, after, waitedForWrites, "scene_data_upload");
		}
		if (updated)
		{
			noteDataSetUpload(stats, size, requestedBytes, uploadedBytes);
		}
		return updated;
	};

	{
		ScopedPtPerfTimer reprojectionTimer(renderer.mLastPerfShellTraceStats.sceneDataSetReprojectionMs);
		if (!UpdateReprojectionBuffer(renderer, &waitedForWrites))
		{
			return false;
		}
	}

	{
		ScopedPtPerfTimer visibleFlatTimer(renderer.mLastPerfShellTraceStats.sceneDataSetVisibleFlatPlaneMs);
		if (!UpdateVisibleFlatPlaneBuffer(renderer, &waitedForWrites))
		{
			return false;
		}
	}

	{
		ScopedPtPerfTimer visibleChunkTimer(renderer.mLastPerfShellTraceStats.sceneDataSetVisibleChunkMs);
		if (!UpdateVisibleChunkBuffer(renderer, &waitedForWrites))
		{
			return false;
		}
	}

	if (sceneInstances.empty())
	{
		return false;
	}

	renderer.mBoundRuntimeLightCount = 0;

	if (!ensureStructuredBufferBatched(
		renderer.mSceneInstanceBuffer,
		renderer.mSceneInstanceBufferStats,
		sceneInstances.data(),
		sceneInstances.size() * sizeof(SceneInstanceData),
		sizeof(SceneInstanceData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess(),
		renderer.mLastPerfShellTraceStats.sceneDataSetSceneInstanceMs,
		renderer.mLastPerfShellTraceStats.sceneDataSetSceneInstanceRequestedBytes,
		renderer.mLastPerfShellTraceStats.sceneDataSetSceneInstanceUploadedBytes))
	{
		return false;
	}
	renderer.mBoundSceneInstances = sceneInstances;

	std::vector<ScenePortalData> scenePortals;
	{
		ScopedPtPerfTimer portalTimer(renderer.mLastPerfShellTraceStats.sceneDataSetPortalMs);
		scenePortals = BuildScenePortalData(renderer.mMapWorld);
	}
	if (!ensureStructuredBufferBatched(
		renderer.mPortalBuffer,
		renderer.mPortalBufferStats,
		scenePortals.data(),
		scenePortals.size() * sizeof(ScenePortalData),
		sizeof(ScenePortalData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess(),
		renderer.mLastPerfShellTraceStats.sceneDataSetPortalMs,
		renderer.mLastPerfShellTraceStats.sceneDataSetPortalRequestedBytes,
		renderer.mLastPerfShellTraceStats.sceneDataSetPortalUploadedBytes))
	{
		return false;
	}

	uint64_t runtimeLightPayloadHash = 0;
	{
		ScopedPtPerfTimer hashTimer(renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightHashMs);
		runtimeLightPayloadHash = renderer.mSceneLights.BuildRuntimeLightPayloadHash();
	}
	const uint32_t activeRuntimeLightCount = (uint32_t)renderer.mSceneLights.GetAnalyticLights().activeLights.size();
	if (!renderer.mRuntimeLightPayloadCacheValid ||
		renderer.mRuntimeLightPayloadHash != runtimeLightPayloadHash ||
		renderer.mRuntimeLightBuffer.shaderView == nullptr)
	{
		renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightUploads++;
		std::vector<NRIRuntimePointLightGpuData> runtimeLights;
		{
			ScopedPtPerfTimer runtimeLightTimer(renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightUploadMs);
			renderer.mSceneLights.BuildRuntimePointLightUpload(runtimeLights);
		}
		if (!ensureStructuredBufferBatched(
			renderer.mRuntimeLightBuffer,
			renderer.mRuntimeLightBufferStats,
			runtimeLights.empty() ? nullptr : runtimeLights.data(),
			runtimeLights.size() * sizeof(NRIRuntimePointLightGpuData),
			sizeof(NRIRuntimePointLightGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightUploadMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightUploadedBytes))
		{
			return false;
		}

		renderer.mRuntimeLightPayloadCacheValid = true;
		renderer.mRuntimeLightPayloadHash = runtimeLightPayloadHash;
	}
	else
	{
		renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightCacheHits++;
	}

	uint32_t runtimeLightTileCountX = 0;
	uint32_t runtimeLightTileCountY = 0;
	uint32_t runtimeLightTileIndexCount = 0;
	uint32_t runtimeLightMaxTileOccupancy = 0;
	uint64_t runtimeLightClusterCameraHash = 0;
	{
		ScopedPtPerfTimer runtimeLightClusterTimer(renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterMs);
		runtimeLightClusterCameraHash = renderer.mSceneLights.BuildRuntimeLightClusterCameraHash(renderer.BuildRuntimeLightClusterInput());
	}
	const uint64_t runtimeLightClusterPayloadHash =
		nri_scene::HashCombine64(runtimeLightPayloadHash, runtimeLightClusterCameraHash);
	if (!renderer.mRuntimeLightClusterCacheValid ||
		renderer.mRuntimeLightClusterPayloadHash != runtimeLightClusterPayloadHash ||
		renderer.mRuntimeLightTileHeaderBuffer.shaderView == nullptr ||
		renderer.mRuntimeLightTileIndexBuffer.shaderView == nullptr)
	{
		renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterUploads++;
		std::vector<NRIRuntimeLightTileHeaderGpuData> runtimeLightTileHeaders;
		std::vector<uint32_t> runtimeLightTileIndices;
		{
			ScopedPtPerfTimer runtimeLightClusterTimer(renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterMs);
			renderer.mSceneLights.BuildRuntimeLightClusterUpload(
				renderer.BuildRuntimeLightClusterInput(),
				runtimeLightTileHeaders,
				runtimeLightTileIndices,
				runtimeLightTileCountX,
				runtimeLightTileCountY,
				runtimeLightTileIndexCount,
				runtimeLightMaxTileOccupancy);
		}
		if (!ensureStructuredBufferBatched(
			renderer.mRuntimeLightTileHeaderBuffer,
			renderer.mRuntimeLightTileHeaderBufferStats,
			runtimeLightTileHeaders.data(),
			runtimeLightTileHeaders.size() * sizeof(NRIRuntimeLightTileHeaderGpuData),
			sizeof(NRIRuntimeLightTileHeaderGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterUploadedBytes))
		{
			return false;
		}

		if (!ensureStructuredBufferBatched(
			renderer.mRuntimeLightTileIndexBuffer,
			renderer.mRuntimeLightTileIndexBufferStats,
			runtimeLightTileIndices.data(),
			runtimeLightTileIndices.size() * sizeof(uint32_t),
			sizeof(uint32_t),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterUploadedBytes))
		{
			return false;
		}

		renderer.mRuntimeLightClusterCacheValid = true;
		renderer.mRuntimeLightClusterPayloadHash = runtimeLightClusterPayloadHash;
		renderer.mRuntimeLightClusterCameraHash = runtimeLightClusterCameraHash;
	}
	else
	{
		runtimeLightTileCountX = renderer.mBoundRuntimeLightTileCountX;
		runtimeLightTileCountY = renderer.mBoundRuntimeLightTileCountY;
		runtimeLightTileIndexCount = renderer.mBoundRuntimeLightTileIndexCount;
		runtimeLightMaxTileOccupancy = renderer.mBoundRuntimeLightMaxTileOccupancy;
		renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterCacheHits++;
	}

	if (!renderer.mEmissiveSamplingPayloadCacheValid ||
		renderer.mEmissivePrimitiveHeaderBuffer.shaderView == nullptr ||
		renderer.mEmissivePrimitiveBuffer.shaderView == nullptr ||
		renderer.mEmissivePrimitiveCdfBuffer.shaderView == nullptr ||
		renderer.mEmissiveMaterialResponseBuffer.shaderView == nullptr)
	{
		renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveUploads++;
		NRIEmissivePrimitiveHeaderGpuData emissiveHeader = {};
		std::vector<NRIEmissivePrimitiveGpuData> emissivePrimitives;
		std::vector<float> emissiveCdf;
		std::vector<NRIEmissiveMaterialResponseGpuData> emissiveMaterialResponses;
		std::vector<NRIEmissivePrimitiveDebugRecord> ignoredEmissiveDebugRecords;
		{
			ScopedPtPerfTimer emissiveTimer(renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveMs);
			renderer.mSceneLights.BuildEmissiveSamplingUpload({}, emissiveHeader, emissivePrimitives, emissiveCdf, emissiveMaterialResponses, ignoredEmissiveDebugRecords);
		}
		if (!ensureStructuredBufferBatched(
			renderer.mEmissivePrimitiveHeaderBuffer,
			renderer.mEmissivePrimitiveHeaderBufferStats,
			&emissiveHeader,
			sizeof(emissiveHeader),
			sizeof(NRIEmissivePrimitiveHeaderGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveUploadedBytes))
		{
			return false;
		}

		if (!ensureStructuredBufferBatched(
			renderer.mEmissivePrimitiveBuffer,
			renderer.mEmissivePrimitiveBufferStats,
			emissivePrimitives.empty() ? nullptr : emissivePrimitives.data(),
			emissivePrimitives.empty() ? 0u : emissivePrimitives.size() * sizeof(NRIEmissivePrimitiveGpuData),
			sizeof(NRIEmissivePrimitiveGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveUploadedBytes))
		{
			return false;
		}

		if (!ensureStructuredBufferBatched(
			renderer.mEmissivePrimitiveCdfBuffer,
			renderer.mEmissivePrimitiveCdfBufferStats,
			emissiveCdf.data(),
			emissiveCdf.size() * sizeof(float),
			sizeof(float),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveUploadedBytes))
		{
			return false;
		}

		if (!ensureStructuredBufferBatched(
			renderer.mEmissiveMaterialResponseBuffer,
			renderer.mEmissiveMaterialResponseBufferStats,
			emissiveMaterialResponses.empty() ? nullptr : emissiveMaterialResponses.data(),
			emissiveMaterialResponses.empty() ? 0u : emissiveMaterialResponses.size() * sizeof(NRIEmissiveMaterialResponseGpuData),
			sizeof(NRIEmissiveMaterialResponseGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveUploadedBytes))
		{
			return false;
		}
	}
	else
	{
		renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveCacheHits++;
	}

	uint64_t sectorLightingPayloadHash = 0;
	{
		ScopedPtPerfTimer sectorLightTimer(renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightMs);
		renderer.UpdateBoundSectorLightingState();
		sectorLightingPayloadHash = renderer.mSceneLights.BuildSectorLightingPayloadHash(NRIGetSectorLightMultiplier(), nri_ptsectorlighting);
	}
	if (!renderer.mSectorLightingPayloadCacheValid ||
		renderer.mSectorLightingPayloadHash != sectorLightingPayloadHash ||
		renderer.mSectorLightHeaderBuffer.shaderView == nullptr ||
		renderer.mSectorLightBuffer.shaderView == nullptr)
	{
		renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightUploads++;
		NRISectorLightHeaderGpuData sectorLightHeader = {};
		std::vector<NRISectorLightGpuData> sectorLights;
		{
			ScopedPtPerfTimer sectorLightTimer(renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightMs);
			renderer.UpdateBoundSectorLightingState();
			renderer.mSceneLights.BuildSectorLightingUpload(NRIGetSectorLightMultiplier(), nri_ptsectorlighting, sectorLightHeader, sectorLights);
		}
		if (!ensureStructuredBufferBatched(
			renderer.mSectorLightHeaderBuffer,
			renderer.mSectorLightHeaderBufferStats,
			&sectorLightHeader,
			sizeof(sectorLightHeader),
			sizeof(NRISectorLightHeaderGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightUploadedBytes))
		{
			return false;
		}

		if (!ensureStructuredBufferBatched(
			renderer.mSectorLightBuffer,
			renderer.mSectorLightBufferStats,
			sectorLights.empty() ? nullptr : sectorLights.data(),
			sectorLights.empty() ? 0u : sectorLights.size() * sizeof(NRISectorLightGpuData),
			sizeof(NRISectorLightGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightUploadedBytes))
		{
			return false;
		}

		renderer.mSectorLightingPayloadCacheValid = true;
		renderer.mSectorLightingPayloadHash = sectorLightingPayloadHash;
	}
	else
	{
		renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightCacheHits++;
	}

	auto selectView = [](const NRIBufferResource& primary, const NRIBufferResource& fallback) -> nri::Descriptor*
	{
		return primary.shaderView != nullptr ? primary.shaderView : fallback.shaderView;
	};

	{
		ScopedPtPerfTimer descriptorBuildTimer(renderer.mLastPerfShellTraceStats.sceneDataSetDescriptorBuildMs);
		renderer.mSceneDataDescriptors.fill(nullptr);
		renderer.mSceneDataDescriptors[0] = selectView(staticVertexBuffer, dynamicVertexBuffer);
		renderer.mSceneDataDescriptors[1] = selectView(staticIndexBuffer, dynamicIndexBuffer);
		renderer.mSceneDataDescriptors[2] = selectView(staticPrimitiveBuffer, dynamicPrimitiveBuffer);
		renderer.mSceneDataDescriptors[3] = selectView(staticMaterialBuffer, dynamicMaterialBuffer);
		renderer.mSceneDataDescriptors[4] = selectView(dynamicVertexBuffer, staticVertexBuffer);
		renderer.mSceneDataDescriptors[5] = selectView(dynamicIndexBuffer, staticIndexBuffer);
		renderer.mSceneDataDescriptors[6] = selectView(dynamicPrimitiveBuffer, staticPrimitiveBuffer);
		renderer.mSceneDataDescriptors[7] = selectView(dynamicMaterialBuffer, staticMaterialBuffer);
		renderer.mSceneDataDescriptors[8] = renderer.mSceneInstanceBuffer.shaderView;
		renderer.mSceneDataDescriptors[9] = renderer.mPortalBuffer.shaderView;
		renderer.mSceneDataDescriptors[10] = renderer.mRuntimeLightBuffer.shaderView;
		renderer.mSceneDataDescriptors[11] = renderer.mRuntimeLightTileHeaderBuffer.shaderView;
		renderer.mSceneDataDescriptors[12] = renderer.mRuntimeLightTileIndexBuffer.shaderView;
		renderer.mSceneDataDescriptors[13] = renderer.mEmissivePrimitiveHeaderBuffer.shaderView;
		renderer.mSceneDataDescriptors[14] = renderer.mEmissivePrimitiveBuffer.shaderView;
		renderer.mSceneDataDescriptors[15] = renderer.mEmissivePrimitiveCdfBuffer.shaderView;
		renderer.mSceneDataDescriptors[16] = renderer.mSectorLightHeaderBuffer.shaderView;
		renderer.mSceneDataDescriptors[17] = renderer.mSectorLightBuffer.shaderView;
		renderer.mSceneDataDescriptors[18] = renderer.mReprojectionBuffer.shaderView;
		renderer.mSceneDataDescriptors[19] = renderer.mVisibleChunkBuffer.shaderView;
		renderer.mSceneDataDescriptors[20] = renderer.mVisibleFlatPlaneBuffer.shaderView;
		const NRIPersistentVoxelDescriptorSnapshot persistentVoxelDescriptors =
			renderer.mPersistentVoxels.BuildDescriptorSnapshot(dynamicVertexBuffer, dynamicIndexBuffer, dynamicPrimitiveBuffer, dynamicMaterialBuffer);
		renderer.mSceneDataDescriptors[21] = persistentVoxelDescriptors.vertex;
		renderer.mSceneDataDescriptors[22] = persistentVoxelDescriptors.index;
		renderer.mSceneDataDescriptors[23] = persistentVoxelDescriptors.primitive;
		renderer.mSceneDataDescriptors[24] = persistentVoxelDescriptors.material;
		renderer.mSceneDataDescriptors[25] = renderer.mEmissiveMaterialResponseBuffer.shaderView;
	}

	{
		ScopedPtPerfTimer descriptorValidateTimer(renderer.mLastPerfShellTraceStats.sceneDataSetDescriptorValidateMs);
		for (const nri::Descriptor* descriptor : renderer.mSceneDataDescriptors)
		{
			if (descriptor == nullptr)
			{
				renderer.mLastPerfShellTraceStats.sceneDataSetDescriptorNullCount++;
				return false;
			}
		}
	}

	if (!NRIDescriptorSetManager::CommitSceneDataDescriptors(renderer, reason != nullptr ? reason : "scene_data_full_rebuild"))
	{
		return false;
	}

	renderer.mBoundStaticPrimitiveCount = staticPrimitiveCount;
	renderer.mBoundDynamicPrimitiveCount = dynamicPrimitiveCount;
	renderer.mBoundStaticMaterialCount = staticMaterialCount;
	renderer.mBoundDynamicMaterialCount = dynamicMaterialCount;
	renderer.mBoundPortalCount = renderer.mMapWorld.valid ? (uint32_t)renderer.mMapWorld.portals.size() : 0u;
	renderer.mBoundRuntimeLightCount = activeRuntimeLightCount;
	renderer.mBoundRuntimeLightTileCountX = runtimeLightTileCountX;
	renderer.mBoundRuntimeLightTileCountY = runtimeLightTileCountY;
	renderer.mBoundRuntimeLightTileSize = NRI_RUNTIME_LIGHT_TILE_SIZE;
	renderer.mBoundRuntimeLightTileIndexCount = runtimeLightTileIndexCount;
	renderer.mBoundRuntimeLightMaxTileOccupancy = runtimeLightMaxTileOccupancy;
	renderer.mRuntimeLightSceneDataDirty = false;
	return true;
}