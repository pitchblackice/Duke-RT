#include "nri_static_scene.h"

#include "nri_renderer.h"
#include "nri_sky_environment.h"
#include "../scene/nri_scene_math.h"
#include "../system/nri_renderdevice.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "mapinfo.h"

#include <algorithm>
#include <chrono>

EXTERN_CVAR(Bool, nri_ptscenestats)
EXTERN_CVAR(Int, nri_ptloadingtrace)
EXTERN_CVAR(Bool, nri_voxelstats)

namespace
{
	constexpr uint32_t NRI_SCENE_DATA_SOURCE_STATIC = 0;

	double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	static const char* YesNo(bool value)
	{
		return value ? "yes" : "no";
	}

	template<typename T>
	static T NRIFlags(T a, T b)
	{
		return (T)((uint32_t)a | (uint32_t)b);
	}

	static nri::AccessStage NRIComputeShaderResourceAccess()
	{
		return { nri::AccessBits::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER };
	}

	static nri::AccessStage NRIAccelerationStructureBuildInputAccess()
	{
		return { nri::AccessBits::ACCELERATION_STRUCTURE_READ, nri::StageBits::ACCELERATION_STRUCTURE };
	}

	static nri::AccessStage NRIAccelerationStructureWriteAccess()
	{
		return { nri::AccessBits::ACCELERATION_STRUCTURE_WRITE, nri::StageBits::ACCELERATION_STRUCTURE };
	}

	static nri::AccessStage NRIAccelerationStructureScratchAccess()
	{
		return { nri::AccessBits::SCRATCH_BUFFER, nri::StageBits::ACCELERATION_STRUCTURE };
	}

	static nri::AccessStage NRIAccelerationStructureReadAccess()
	{
		return { nri::AccessBits::ACCELERATION_STRUCTURE_READ, nri::StageBits::ACCELERATION_STRUCTURE };
	}

	static nri::AccelerationStructureBits GetStaticMapChunkBlasBuildBits()
	{
		return
			nri::AccelerationStructureBits::PREFER_FAST_TRACE |
			nri::AccelerationStructureBits::ALLOW_UPDATE;
	}

	static bool ShouldTracePtPerf()
	{
		return PerfLoopTraceActive() || ShouldEmitRendererTemporalTraceLogs();
	}
}

void NRIRenderer::ResetResidentMapChunkRegistry()
{
	mStaticSceneResidency.Registry() = {};
}

uint32_t NRIStaticSceneResidency::GetStaticSceneChunkSlotPreference(
	const StaticMapSceneCache& staticScene,
	const StaticMapChunkAtlas& atlas,
	uint32_t chunkListIndex)
{
	if (chunkListIndex >= staticScene.chunks.size())
	{
		return 0;
	}

	const auto& chunk = staticScene.chunks[chunkListIndex];
	uint32_t score = 0;
	if (chunk.active)
	{
		score += 8u;
	}
	if (chunk.accelerationStructure.accelerationStructure != nullptr)
	{
		score += 4u;
	}
	if (chunk.primitiveCount > 0 && chunk.materialCount > 0)
	{
		score += 2u;
	}
	if (atlas.valid &&
		chunkListIndex < atlas.chunks.size() &&
		atlas.chunks[chunkListIndex].valid)
	{
		score += 1u;
	}
	return score;
}

void NRIStaticSceneResidency::SyncResidentMapChunkRegistryFromStaticScene(const NRIStaticSceneRegistrySyncInput& input)
{
	ResetResidentMapChunkRegistry();
	if (input.mapWorld == nullptr || !input.mapWorld->valid)
	{
		return;
	}
	if (input.staticScene == nullptr || input.atlas == nullptr)
	{
		return;
	}

	const nri_scene::PTMapWorld& mapWorld = *input.mapWorld;
	const StaticMapSceneCache& staticScene = *input.staticScene;
	const StaticMapChunkAtlas& atlas = *input.atlas;
	auto& registry = mResidentMapChunkRegistry;
	registry.valid = true;
	registry.buildSerial = mapWorld.buildSerial;
	registry.chunkCount = (uint32_t)mapWorld.chunks.size();
	registry.entries.resize(mapWorld.chunks.size());

	for (size_t chunkListIndex = 0; chunkListIndex < mapWorld.chunks.size(); ++chunkListIndex)
	{
		const auto& mapChunk = mapWorld.chunks[chunkListIndex];
		auto& entry = registry.entries[chunkListIndex];
		entry.valid = true;
		entry.chunkIndex = mapChunk.chunkIndex;
		if (input.replacements != nullptr)
		{
			for (const NRIResidentChunkReplacementInfo& replacement : *input.replacements)
			{
				if (replacement.chunkListIndex != chunkListIndex)
				{
					continue;
				}

				entry.appliedBaseline = replacement.baseline;
				entry.baselineSignature = replacement.baselineSignature;
				entry.liveSignature = replacement.liveSignature != 0 ? replacement.liveSignature : replacement.baselineSignature;
				break;
			}
		}
	}

	const bool atlasMatchesStaticScene =
		atlas.valid &&
		atlas.buildSerial == staticScene.buildSerial &&
		atlas.chunks.size() == staticScene.chunks.size();
	std::vector<int32_t> bestSlotScores(registry.entries.size(), -1);
	for (size_t chunkListIndex = 0; chunkListIndex < staticScene.chunks.size(); ++chunkListIndex)
	{
		const auto& staticChunk = staticScene.chunks[chunkListIndex];
		if (staticChunk.chunkIndex >= registry.entries.size())
		{
			continue;
		}

		const int32_t candidateScore = (int32_t)GetStaticSceneChunkSlotPreference(staticScene, atlas, (uint32_t)chunkListIndex);
		auto& entry = registry.entries[staticChunk.chunkIndex];
		if (candidateScore < bestSlotScores[staticChunk.chunkIndex])
		{
			continue;
		}
		if (candidateScore == bestSlotScores[staticChunk.chunkIndex] &&
			entry.staticSceneChunkListIndex != UINT32_MAX &&
			entry.staticSceneChunkListIndex > chunkListIndex)
		{
			continue;
		}

		bestSlotScores[staticChunk.chunkIndex] = candidateScore;
		entry.staticSceneChunkListIndex = (uint32_t)chunkListIndex;
		entry.active = staticChunk.active;
		entry.mappedInStaticScene = staticChunk.active;
		if (atlasMatchesStaticScene &&
			chunkListIndex < atlas.chunks.size() &&
			atlas.chunks[chunkListIndex].valid)
		{
			const auto& atlasChunk = atlas.chunks[chunkListIndex];
			entry.vertexOffset = atlasChunk.vertexOffset;
			entry.vertexCount = atlasChunk.vertexCount;
			entry.indexOffset = atlasChunk.indexOffset;
			entry.indexCount = atlasChunk.indexCount;
			entry.primitiveOffset = atlasChunk.primitiveOffset;
			entry.primitiveCount = atlasChunk.primitiveCount;
			entry.materialOffset = atlasChunk.materialOffset;
			entry.materialCount = atlasChunk.materialCount;
		}
		else
		{
			entry.vertexOffset = staticChunk.vertexOffset;
			entry.vertexCount = staticChunk.vertexCount;
			entry.indexOffset = staticChunk.indexOffset;
			entry.indexCount = staticChunk.indexCount;
			entry.primitiveOffset = staticChunk.primitiveOffset;
			entry.primitiveCount = staticChunk.primitiveCount;
			entry.materialOffset = staticChunk.materialOffset;
			entry.materialCount = staticChunk.materialCount;
		}
		entry.geometryTopologySignature = staticChunk.geometryTopologySignature;
		entry.animatedMaterialSignature = staticChunk.animatedMaterialSignature;
		entry.materialPayloadHash =
			staticChunk.active && input.hashResidentMaterialPayload != nullptr ?
			input.hashResidentMaterialPayload(staticChunk.materialBridge) :
			0;
		entry.geometryPayloadHash = staticChunk.active ? staticChunk.geometryPayloadHash : 0;
		entry.animatedGeometrySignature = staticChunk.animatedGeometrySignature;
		entry.exactGeometrySignature = staticChunk.exactGeometrySignature;
		entry.hasAnimatedTextureCandidates = staticChunk.hasAnimatedTextureCandidates;
		entry.animatedRefreshSuppressed = staticChunk.animatedRefreshSuppressed;
		entry.accelerationResident = staticChunk.active && staticChunk.accelerationStructure.accelerationStructure != nullptr;

		if (entry.active)
		{
			registry.activeChunkCount++;
			registry.mappedChunkCount++;
		}
		if (entry.accelerationResident)
		{
			registry.accelerationResidentChunkCount++;
		}
		if (entry.hasAnimatedTextureCandidates)
		{
			registry.animatedCandidateChunkCount++;
		}
		if (entry.animatedRefreshSuppressed)
		{
			registry.animatedRefreshSuppressedChunkCount++;
		}
	}
}

uint32_t NRIRenderer::GetStaticSceneChunkSlotPreference(uint32_t chunkListIndex) const
{
	return NRIStaticSceneResidency::GetStaticSceneChunkSlotPreference(
		mStaticMapScene,
		mStaticMapChunkAtlas,
		chunkListIndex);
}

uint32_t NRIRenderer::FindPreferredStaticSceneChunkListIndex(uint32_t chunkIndex) const
{
	uint32_t bestChunkListIndex = UINT32_MAX;
	uint32_t bestScore = 0;
	for (uint32_t chunkListIndex = 0; chunkListIndex < (uint32_t)mStaticMapScene.chunks.size(); ++chunkListIndex)
	{
		const auto& chunk = mStaticMapScene.chunks[chunkListIndex];
		if (chunk.chunkIndex != chunkIndex)
		{
			continue;
		}

		const uint32_t score = GetStaticSceneChunkSlotPreference(chunkListIndex);
		if (bestChunkListIndex == UINT32_MAX ||
			score > bestScore ||
			(score == bestScore && chunkListIndex > bestChunkListIndex))
		{
			bestChunkListIndex = chunkListIndex;
			bestScore = score;
		}
	}

	return bestChunkListIndex;
}

uint32_t NRIRenderer::CountStaticSceneChunkSlots(uint32_t chunkIndex) const
{
	uint32_t count = 0;
	for (const auto& chunk : mStaticMapScene.chunks)
	{
		if (chunk.chunkIndex == chunkIndex)
		{
			count++;
		}
	}
	return count;
}

bool NRIRenderer::RebuildResidentStaticMaterialBridgeFromChunks()
{
	if (!mStaticMapChunkAtlas.valid || mStaticMapChunkAtlas.chunks.size() != mStaticMapScene.chunks.size())
	{
		return false;
	}

	nri_scene::MaterialBridgeData bridge = {};
	std::vector<uint32_t> chunkListIndices;
	chunkListIndices.reserve(mStaticMapScene.chunks.size());
	for (uint32_t chunkListIndex = 0; chunkListIndex < mStaticMapScene.chunks.size(); ++chunkListIndex)
	{
		const auto& chunkCache = mStaticMapScene.chunks[chunkListIndex];
		const auto& atlasChunk = mStaticMapChunkAtlas.chunks[chunkListIndex];
		if (!chunkCache.active || !atlasChunk.valid || atlasChunk.materialCount == 0)
		{
			continue;
		}

		chunkListIndices.push_back(chunkListIndex);
	}

	std::sort(
		chunkListIndices.begin(),
		chunkListIndices.end(),
		[this](uint32_t lhs, uint32_t rhs)
		{
			const auto& lhsChunk = mStaticMapChunkAtlas.chunks[lhs];
			const auto& rhsChunk = mStaticMapChunkAtlas.chunks[rhs];
			if (lhsChunk.materialOffset != rhsChunk.materialOffset)
			{
				return lhsChunk.materialOffset < rhsChunk.materialOffset;
			}

			return lhs < rhs;
		});

	for (uint32_t chunkListIndex : chunkListIndices)
	{
		const auto& chunkCache = mStaticMapScene.chunks[chunkListIndex];
		const auto& atlasChunk = mStaticMapChunkAtlas.chunks[chunkListIndex];

		if (bridge.materials.size() < atlasChunk.materialOffset)
		{
			bridge.materials.resize(atlasChunk.materialOffset);
			bridge.lightMetadata.resize(atlasChunk.materialOffset);
		}

		const uint32_t nextMaterialOffset = (uint32_t)bridge.materials.size();
		if (nextMaterialOffset != atlasChunk.materialOffset ||
			(uint32_t)chunkCache.materialBridge.materials.size() != atlasChunk.materialCount)
		{
			if (nri_ptscenestats && ShouldTracePtPerf())
			{
				Printf("NRI PT static scene trace: event=resident_material_bridge_failed chunk=%u atlas_offset=%u next_offset=%u atlas_count=%u bridge_count=%u\n",
					chunkCache.chunkIndex,
					atlasChunk.materialOffset,
					nextMaterialOffset,
					atlasChunk.materialCount,
					(uint32_t)chunkCache.materialBridge.materials.size());
			}
			return false;
		}

		nri_scene::AppendMaterialBridge(chunkCache.materialBridge, bridge);
	}

	if (bridge.materials.size() < mStaticMapChunkAtlas.materialCount)
	{
		bridge.materials.resize(mStaticMapChunkAtlas.materialCount);
		bridge.lightMetadata.resize(mStaticMapChunkAtlas.materialCount);
	}

	mStaticMapScene.materialBridge = std::move(bridge);
	return true;
}

void NRIRenderer::UploadChunkMaterialsToAtlas(
	const std::vector<nri_scene::MaterialData>& sourceMaterials,
	const StaticMapSceneCache::ChunkCache& sourceChunk,
	const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
	std::vector<nri_scene::MaterialData>& outMaterials) const
{
	if (!atlasChunk.valid)
	{
		return;
	}

	if (sourceChunk.materialOffset + sourceChunk.materialCount <= sourceMaterials.size() &&
		atlasChunk.materialOffset + atlasChunk.materialCount <= outMaterials.size())
	{
		std::copy_n(
			sourceMaterials.data() + sourceChunk.materialOffset,
			sourceChunk.materialCount,
			outMaterials.data() + atlasChunk.materialOffset);
	}
}

bool NRIRenderer::UploadStaticMapChunkMaterialAtlas(
	NRIBufferResource& materialBuffer,
	const StaticMapChunkAtlas& atlas,
	const StaticMapSceneCache& staticScene,
	const std::vector<nri_scene::MaterialData>& gpuMaterials)
{
	if (!atlas.valid || atlas.chunks.size() != staticScene.chunks.size())
	{
		return false;
	}

	std::vector<nri_scene::MaterialData> atlasMaterials(atlas.materialCount);
	for (uint32_t chunkListIndex = 0; chunkListIndex < staticScene.chunks.size(); ++chunkListIndex)
	{
		UploadChunkMaterialsToAtlas(
			gpuMaterials,
			staticScene.chunks[chunkListIndex],
			atlas.chunks[chunkListIndex],
			atlasMaterials);
	}

	return EnsureResidentStructuredBuffer(
		materialBuffer,
		mMaterialBufferStats,
		atlasMaterials.data(),
		atlasMaterials.size() * sizeof(nri_scene::MaterialData),
		sizeof(nri_scene::MaterialData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess(),
		"resident_chunk_write",
		ResidentUploadKind_Material);
}

void NRIRenderer::ResetStaticMapChunkAtlas(StaticMapChunkAtlas& atlas) const
{
	atlas = {};
}

uint32_t NRIRenderer::GetChunkAtlasCapacity(uint32_t usedCount) const
{
	if (usedCount == 0)
	{
		return 0;
	}

	const uint64_t reserveFloor = std::max<uint64_t>(usedCount + 64u, usedCount * 2ull);
	const uint64_t grown = GetNRIGrownBufferSize((uint64_t)usedCount, reserveFloor, 1u);
	return (uint32_t)std::min<uint64_t>(grown, (uint64_t)UINT32_MAX);
}

uint32_t NRIRenderer::AllocateChunkAtlasSlice(uint32_t count, uint32_t alignment, uint32_t& cursor) const
{
	if (alignment == 0)
	{
		alignment = 1;
	}

	const uint32_t alignedCursor =
		cursor % alignment == 0 ?
		cursor :
		cursor + (alignment - cursor % alignment);
	cursor = alignedCursor + count;
	return alignedCursor;
}

uint32_t NRIRenderer::AllocateChunkAtlasRange(uint32_t count, uint32_t capacity, std::vector<StaticMapChunkAtlas::FreeRange>& freeRanges, uint32_t& cursor) const
{
	if (count == 0)
	{
		return 0;
	}

	for (size_t i = 0; i < freeRanges.size(); ++i)
	{
		auto& range = freeRanges[i];
		if (range.count < count)
		{
			continue;
		}

		const uint32_t offset = range.offset;
		range.offset += count;
		range.count -= count;
		if (range.count == 0)
		{
			freeRanges.erase(freeRanges.begin() + i);
		}
		return offset;
	}

	if (cursor > capacity || count > capacity - cursor)
	{
		return UINT32_MAX;
	}

	const uint32_t offset = cursor;
	cursor += count;
	return offset;
}

void NRIRenderer::ReleaseChunkAtlasRange(std::vector<StaticMapChunkAtlas::FreeRange>& freeRanges, uint32_t offset, uint32_t count) const
{
	if (count == 0)
	{
		return;
	}

	freeRanges.push_back({ offset, count });
	std::sort(
		freeRanges.begin(),
		freeRanges.end(),
		[](const StaticMapChunkAtlas::FreeRange& a, const StaticMapChunkAtlas::FreeRange& b)
		{
			return a.offset < b.offset;
		});

	size_t writeIndex = 0;
	for (size_t i = 0; i < freeRanges.size(); ++i)
	{
		if (writeIndex == 0)
		{
			freeRanges[writeIndex++] = freeRanges[i];
			continue;
		}

		auto& previous = freeRanges[writeIndex - 1];
		const auto& current = freeRanges[i];
		if (previous.offset + previous.count >= current.offset)
		{
			const uint32_t end = std::max(previous.offset + previous.count, current.offset + current.count);
			previous.count = end - previous.offset;
			continue;
		}

		freeRanges[writeIndex++] = current;
	}

	freeRanges.resize(writeIndex);
}

bool NRIRenderer::BuildStaticMapChunkAtlasLayout(const StaticMapSceneCache& staticScene, StaticMapChunkAtlas& outAtlas) const
{
	ResetStaticMapChunkAtlas(outAtlas);
	if (staticScene.chunks.empty())
	{
		return false;
	}

	outAtlas.valid = true;
	outAtlas.buildSerial = staticScene.buildSerial;
	outAtlas.chunkCount = (uint32_t)staticScene.chunks.size();
	outAtlas.chunks.resize(staticScene.chunks.size());

	uint32_t vertexCursor = 0;
	uint32_t indexCursor = 0;
	uint32_t primitiveCursor = 0;
	uint32_t materialCursor = 0;
	for (uint32_t chunkListIndex = 0; chunkListIndex < staticScene.chunks.size(); ++chunkListIndex)
	{
		const auto& sourceChunk = staticScene.chunks[chunkListIndex];
		auto& atlasChunk = outAtlas.chunks[chunkListIndex];
		atlasChunk.valid = true;
		atlasChunk.chunkIndex = sourceChunk.chunkIndex;
		atlasChunk.staticSceneChunkListIndex = chunkListIndex;
		atlasChunk.vertexOffset = AllocateChunkAtlasSlice(sourceChunk.vertexCount, 1u, vertexCursor);
		atlasChunk.vertexCount = sourceChunk.vertexCount;
		atlasChunk.indexOffset = AllocateChunkAtlasSlice(sourceChunk.indexCount, 1u, indexCursor);
		atlasChunk.indexCount = sourceChunk.indexCount;
		atlasChunk.primitiveOffset = AllocateChunkAtlasSlice(sourceChunk.primitiveCount, 1u, primitiveCursor);
		atlasChunk.primitiveCount = sourceChunk.primitiveCount;
		atlasChunk.materialOffset = AllocateChunkAtlasSlice(sourceChunk.materialCount, 1u, materialCursor);
		atlasChunk.materialCount = sourceChunk.materialCount;
	}

	outAtlas.vertexCount = vertexCursor;
	outAtlas.indexCount = indexCursor;
	outAtlas.primitiveCount = primitiveCursor;
	outAtlas.materialCount = materialCursor;
	outAtlas.vertexCapacity = GetChunkAtlasCapacity(vertexCursor);
	outAtlas.indexCapacity = GetChunkAtlasCapacity(indexCursor);
	outAtlas.primitiveCapacity = GetChunkAtlasCapacity(primitiveCursor);
	outAtlas.materialCapacity = GetChunkAtlasCapacity(materialCursor);
	return true;
}

bool NRIRenderer::EnsureResidentStaticMapChunkAtlasBufferCapacity(const StaticMapChunkAtlas& atlas)
{
	if (!atlas.valid || !mStaticMapScene.valid)
	{
		return false;
	}

	const uint32_t targetVertexCapacity = std::max(atlas.vertexCapacity, atlas.vertexCount);
	const uint32_t targetIndexCapacity = std::max(atlas.indexCapacity, atlas.indexCount);
	const uint32_t targetPrimitiveCapacity = std::max(atlas.primitiveCapacity, atlas.primitiveCount);
	const uint32_t targetMaterialCapacity = std::max(atlas.materialCapacity, atlas.materialCount);

	const uint32_t currentVertexCapacity =
		mStaticVertexBuffer.stride != 0 ?
		(uint32_t)(mStaticVertexBuffer.size / mStaticVertexBuffer.stride) :
		0u;
	const uint32_t currentIndexCapacity =
		mStaticIndexBuffer.stride != 0 ?
		(uint32_t)(mStaticIndexBuffer.size / mStaticIndexBuffer.stride) :
		0u;
	const uint32_t currentPrimitiveCapacity =
		mStaticPrimitiveBuffer.stride != 0 ?
		(uint32_t)(mStaticPrimitiveBuffer.size / mStaticPrimitiveBuffer.stride) :
		0u;
	const uint32_t currentMaterialCapacity =
		mStaticMaterialBuffer.stride != 0 ?
		(uint32_t)(mStaticMaterialBuffer.size / mStaticMaterialBuffer.stride) :
		0u;

	const bool growVertexBuffer = currentVertexCapacity < targetVertexCapacity;
	const bool growIndexBuffer = currentIndexCapacity < targetIndexCapacity;
	const bool growPrimitiveBuffer = currentPrimitiveCapacity < targetPrimitiveCapacity;
	const bool growMaterialBuffer = currentMaterialCapacity < targetMaterialCapacity;
	if (!growVertexBuffer && !growIndexBuffer && !growPrimitiveBuffer && !growMaterialBuffer)
	{
		mStaticMapChunkAtlas.vertexCapacity = currentVertexCapacity;
		mStaticMapChunkAtlas.indexCapacity = currentIndexCapacity;
		mStaticMapChunkAtlas.primitiveCapacity = currentPrimitiveCapacity;
		mStaticMapChunkAtlas.materialCapacity = currentMaterialCapacity;
		return true;
	}

	if (growVertexBuffer)
	{
		std::vector<nri_scene::SceneVertex> uploadVertices(targetVertexCapacity);
		const size_t copyCount = std::min<size_t>(mStaticMapScene.geometry.vertices.size(), atlas.vertexCount);
		if (copyCount != 0)
		{
			std::copy_n(mStaticMapScene.geometry.vertices.data(), copyCount, uploadVertices.data());
		}
		if (!EnsureResidentStructuredBuffer(
				mStaticVertexBuffer,
				mVertexBufferStats,
				uploadVertices.data(),
				(uint64_t)uploadVertices.size() * sizeof(nri_scene::SceneVertex),
				sizeof(nri_scene::SceneVertex),
				NRIFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT),
				NRIAccelerationStructureBuildInputAccess(),
				"resident_chunk_write",
				ResidentUploadKind_Vertex))
		{
			return false;
		}
		mStaticVertexBuffer.usedSize = (uint64_t)atlas.vertexCount * sizeof(nri_scene::SceneVertex);
	}

	if (growIndexBuffer)
	{
		std::vector<uint32_t> uploadIndices(targetIndexCapacity);
		const size_t copyCount = std::min<size_t>(mStaticMapScene.geometry.indices.size(), atlas.indexCount);
		if (copyCount != 0)
		{
			std::copy_n(mStaticMapScene.geometry.indices.data(), copyCount, uploadIndices.data());
		}
		if (!EnsureResidentStructuredBuffer(
				mStaticIndexBuffer,
				mIndexBufferStats,
				uploadIndices.data(),
				(uint64_t)uploadIndices.size() * sizeof(uint32_t),
				sizeof(uint32_t),
				NRIFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT),
				NRIAccelerationStructureBuildInputAccess(),
				"resident_chunk_write",
				ResidentUploadKind_Index))
		{
			return false;
		}
		mStaticIndexBuffer.usedSize = (uint64_t)atlas.indexCount * sizeof(uint32_t);
	}

	if (growPrimitiveBuffer)
	{
		std::vector<nri_scene::PrimitiveData> uploadPrimitives(targetPrimitiveCapacity);
		const size_t copyCount = std::min<size_t>(mStaticMapScene.geometry.primitives.size(), atlas.primitiveCount);
		if (copyCount != 0)
		{
			std::copy_n(mStaticMapScene.geometry.primitives.data(), copyCount, uploadPrimitives.data());
		}
		if (!EnsureResidentStructuredBuffer(
				mStaticPrimitiveBuffer,
				mPrimitiveBufferStats,
				uploadPrimitives.data(),
				(uint64_t)uploadPrimitives.size() * sizeof(nri_scene::PrimitiveData),
				sizeof(nri_scene::PrimitiveData),
				nri::BufferUsageBits::SHADER_RESOURCE,
				NRIComputeShaderResourceAccess(),
				"resident_chunk_write",
				ResidentUploadKind_Primitive))
		{
			return false;
		}
		mStaticPrimitiveBuffer.usedSize = (uint64_t)atlas.primitiveCount * sizeof(nri_scene::PrimitiveData);
	}

	if (growMaterialBuffer)
	{
		std::vector<nri_scene::MaterialData> uploadMaterials(targetMaterialCapacity);
		const size_t copyCount = std::min<size_t>(mStaticMapScene.gpuMaterials.size(), atlas.materialCount);
		if (copyCount != 0)
		{
			std::copy_n(mStaticMapScene.gpuMaterials.data(), copyCount, uploadMaterials.data());
		}
		if (!EnsureResidentStructuredBuffer(
				mStaticMaterialBuffer,
				mMaterialBufferStats,
				uploadMaterials.data(),
				(uint64_t)uploadMaterials.size() * sizeof(nri_scene::MaterialData),
				sizeof(nri_scene::MaterialData),
				nri::BufferUsageBits::SHADER_RESOURCE,
				NRIComputeShaderResourceAccess(),
				"resident_chunk_write",
				ResidentUploadKind_Material))
		{
			return false;
		}
		mStaticMaterialBuffer.usedSize = (uint64_t)atlas.materialCount * sizeof(nri_scene::MaterialData);
	}

	mStaticMapChunkAtlas.vertexCapacity =
		mStaticVertexBuffer.stride != 0 ?
		(uint32_t)(mStaticVertexBuffer.size / mStaticVertexBuffer.stride) :
		atlas.vertexCapacity;
	mStaticMapChunkAtlas.indexCapacity =
		mStaticIndexBuffer.stride != 0 ?
		(uint32_t)(mStaticIndexBuffer.size / mStaticIndexBuffer.stride) :
		atlas.indexCapacity;
	mStaticMapChunkAtlas.primitiveCapacity =
		mStaticPrimitiveBuffer.stride != 0 ?
		(uint32_t)(mStaticPrimitiveBuffer.size / mStaticPrimitiveBuffer.stride) :
		atlas.primitiveCapacity;
	mStaticMapChunkAtlas.materialCapacity =
		mStaticMaterialBuffer.stride != 0 ?
		(uint32_t)(mStaticMaterialBuffer.size / mStaticMaterialBuffer.stride) :
		atlas.materialCapacity;
	NoteResidentStaticAtlasGrow();

	return RefreshResidentStaticSceneDataSet();
}

void NRIRenderer::BuildStaticMapInstances(std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances) const
{
	if (!mStaticSceneResidency.Registry().valid ||
		mStaticSceneResidency.Registry().buildSerial != mStaticMapScene.buildSerial ||
		mStaticSceneResidency.Registry().entries.empty())
	{
		if (mStaticMapChunkAtlas.valid &&
			mStaticMapChunkAtlas.buildSerial == mStaticMapScene.buildSerial &&
			mStaticMapChunkAtlas.chunks.size() == mStaticMapScene.chunks.size())
		{
			BuildStaticMapInstances(mStaticMapScene, mStaticMapChunkAtlas, outTlasInstances, outSceneInstances);
			return;
		}

		BuildStaticMapInstances(mStaticMapScene, outTlasInstances, outSceneInstances);
		return;
	}

	outTlasInstances.clear();
	outSceneInstances.clear();
	outTlasInstances.reserve(mStaticSceneResidency.Registry().activeChunkCount);
	outSceneInstances.reserve(mStaticSceneResidency.Registry().activeChunkCount);

	for (const auto& entry : mStaticSceneResidency.Registry().entries)
	{
		if (!entry.valid ||
			!entry.active ||
			!entry.mappedInStaticScene ||
			entry.staticSceneChunkListIndex >= mStaticMapScene.chunks.size())
		{
			continue;
		}

		const auto& chunk = mStaticMapScene.chunks[entry.staticSceneChunkListIndex];
		if (chunk.accelerationStructure.accelerationStructure == nullptr)
		{
			continue;
		}

		nri::TopLevelInstance instance = {};
		nri_scene::SetTopLevelInstanceTransform(instance, nri_scene::MakeIdentityPTTransform3x4());
		instance.instanceId = (uint32_t)outSceneInstances.size();
		instance.mask = 0xFF;
		instance.shaderBindingTableLocalOffset = 0;
		instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
		instance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*chunk.accelerationStructure.accelerationStructure);
		const uint32_t sectorIndex =
			entry.chunkIndex < mMapWorld.chunks.size() && mMapWorld.chunks[entry.chunkIndex].sectorIndex >= 0 ?
			(uint32_t)mMapWorld.chunks[entry.chunkIndex].sectorIndex :
			UINT32_MAX;
		outTlasInstances.push_back(instance);
		outSceneInstances.push_back({ entry.primitiveOffset, NRI_SCENE_DATA_SOURCE_STATIC, entry.chunkIndex, sectorIndex });
	}
}

void NRIRenderer::BuildStaticMapInstances(const StaticMapSceneCache& staticScene, std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances) const
{
	outTlasInstances.clear();
	outSceneInstances.clear();
	outTlasInstances.reserve(staticScene.chunks.size());
	outSceneInstances.reserve(staticScene.chunks.size());

	for (uint32_t chunkIndex = 0; chunkIndex < (uint32_t)staticScene.chunks.size(); ++chunkIndex)
	{
		const auto& chunk = staticScene.chunks[chunkIndex];
		if (!chunk.active)
		{
			continue;
		}
		if (chunk.accelerationStructure.accelerationStructure == nullptr)
		{
			continue;
		}

		nri::TopLevelInstance instance = {};
		nri_scene::SetTopLevelInstanceTransform(instance, nri_scene::MakeIdentityPTTransform3x4());
		instance.instanceId = (uint32_t)outSceneInstances.size();
		instance.mask = 0xFF;
		instance.shaderBindingTableLocalOffset = 0;
		instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
		instance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*chunk.accelerationStructure.accelerationStructure);
		const uint32_t sectorIndex =
			chunk.chunkIndex < mMapWorld.chunks.size() && mMapWorld.chunks[chunk.chunkIndex].sectorIndex >= 0 ?
			(uint32_t)mMapWorld.chunks[chunk.chunkIndex].sectorIndex :
			UINT32_MAX;
		outTlasInstances.push_back(instance);
		outSceneInstances.push_back({ chunk.primitiveOffset, NRI_SCENE_DATA_SOURCE_STATIC, chunk.chunkIndex, sectorIndex });
	}
}

void NRIRenderer::BuildStaticMapInstances(const StaticMapSceneCache& staticScene, const StaticMapChunkAtlas& atlas, std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances) const
{
	if (!atlas.valid || atlas.chunks.size() != staticScene.chunks.size())
	{
		BuildStaticMapInstances(staticScene, outTlasInstances, outSceneInstances);
		return;
	}

	outTlasInstances.clear();
	outSceneInstances.clear();
	outTlasInstances.reserve(staticScene.chunks.size());
	outSceneInstances.reserve(staticScene.chunks.size());

	for (uint32_t chunkListIndex = 0; chunkListIndex < staticScene.chunks.size(); ++chunkListIndex)
	{
		const auto& chunk = staticScene.chunks[chunkListIndex];
		const auto& atlasChunk = atlas.chunks[chunkListIndex];
		if (!chunk.active || !atlasChunk.valid)
		{
			continue;
		}
		if (chunk.accelerationStructure.accelerationStructure == nullptr)
		{
			continue;
		}

		nri::TopLevelInstance instance = {};
		nri_scene::SetTopLevelInstanceTransform(instance, nri_scene::MakeIdentityPTTransform3x4());
		instance.instanceId = (uint32_t)outSceneInstances.size();
		instance.mask = 0xFF;
		instance.shaderBindingTableLocalOffset = 0;
		instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
		instance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*chunk.accelerationStructure.accelerationStructure);
		const uint32_t sectorIndex =
			atlasChunk.chunkIndex < mMapWorld.chunks.size() && mMapWorld.chunks[atlasChunk.chunkIndex].sectorIndex >= 0 ?
			(uint32_t)mMapWorld.chunks[atlasChunk.chunkIndex].sectorIndex :
			UINT32_MAX;
		outTlasInstances.push_back(instance);
		outSceneInstances.push_back({ atlasChunk.primitiveOffset, NRI_SCENE_DATA_SOURCE_STATIC, atlasChunk.chunkIndex, sectorIndex });
	}
}

bool NRIRenderer::BuildStaticMapAccelerationStructures()
{
	Clocker clock(NriPTAcceleration);

	if (mStaticMapScene.chunks.empty() ||
		!mStaticMapChunkAtlas.valid ||
		mStaticMapChunkAtlas.chunks.size() != mStaticMapScene.chunks.size())
	{
		return false;
	}

	const bool needsWait =
		mTopLevelAS.accelerationStructure != nullptr ||
		mEmissiveTopLevelAS.accelerationStructure != nullptr ||
		HasAnyDynamicBottomLevelAS() ||
		mTlasInstanceBuffer.buffer != nullptr ||
		mEmissiveTlasInstanceBuffer.buffer != nullptr ||
		mSceneInstanceBuffer.buffer != nullptr ||
		mScratchBuffer.buffer != nullptr ||
		mTopLevelScratchBuffer.buffer != nullptr ||
		mEmissiveTopLevelScratchBuffer.buffer != nullptr;
	if (needsWait)
	{
		WaitForCommandsTracked();
	}

	DestroyBufferResource(mTlasInstanceBuffer);
	DestroyBufferResource(mEmissiveTlasInstanceBuffer);
	DestroyBufferResource(mSceneInstanceBuffer);
	DestroyBufferResource(mScratchBuffer);
	DestroyBufferResource(mTopLevelScratchBuffer);
	DestroyBufferResource(mEmissiveTopLevelScratchBuffer);
	DestroyDynamicBottomLevelAccelerationStructures();
	mPersistentVoxels.Reset("static-acceleration-rebuild", false, (int)nri_ptloadingtrace >= 1 || (bool)nri_voxelstats, BuildPersistentVoxelResetServices());
	DestroyAccelerationStructureResource(mTopLevelAS);
	DestroyAccelerationStructureResource(mEmissiveTopLevelAS);

	for (auto& chunk : mStaticMapScene.chunks)
	{
		DestroyAccelerationStructureResource(chunk.accelerationStructure);
		chunk.residentBlasScratchSizeCacheKey = nullptr;
		chunk.residentBlasBuildScratchSize = 0;
		chunk.residentBlasUpdateScratchSize = 0;
	}

	uint64_t maxScratchSize = 0;
	for (size_t chunkIndex = 0; chunkIndex < mStaticMapScene.chunks.size(); ++chunkIndex)
	{
		auto& chunk = mStaticMapScene.chunks[chunkIndex];
		const auto& atlasChunk = mStaticMapChunkAtlas.chunks[chunkIndex];
		nri::BottomLevelGeometryDesc geometryDesc = {};
		geometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
		geometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
		geometryDesc.triangles.vertexBuffer = mStaticVertexBuffer.buffer;
		geometryDesc.triangles.vertexOffset = 0;
		geometryDesc.triangles.vertexNum = mStaticMapChunkAtlas.vertexCount;
		geometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
		geometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
		geometryDesc.triangles.indexBuffer = mStaticIndexBuffer.buffer;
		geometryDesc.triangles.indexOffset = (uint64_t)atlasChunk.indexOffset * sizeof(uint32_t);
		geometryDesc.triangles.indexNum = atlasChunk.indexCount;
		geometryDesc.triangles.indexType = nri::IndexType::UINT32;

		nri::AccelerationStructureDesc blasDesc = {};
		blasDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
		blasDesc.flags = GetStaticMapChunkBlasBuildBits();
		blasDesc.geometryOrInstanceNum = 1;
		blasDesc.geometries = &geometryDesc;
		if (mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, blasDesc, chunk.accelerationStructure.accelerationStructure) != nri::Result::SUCCESS)
		{
			return false;
		}

		nri::MemoryDesc memoryDesc = {};
		mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(*chunk.accelerationStructure.accelerationStructure, nri::MemoryLocation::DEVICE, memoryDesc);
		chunk.accelerationStructure.memorySize = memoryDesc.size;
		chunk.accelerationStructure.memoryLocation = nri::MemoryLocation::DEVICE;

		const uint64_t scratchSize =
			mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*chunk.accelerationStructure.accelerationStructure);
		chunk.residentBlasScratchSizeCacheKey = chunk.accelerationStructure.accelerationStructure;
		chunk.residentBlasBuildScratchSize = scratchSize;
		chunk.residentBlasUpdateScratchSize = 0;
		maxScratchSize = std::max(maxScratchSize, scratchSize);
	}

	if (!CreateBufferWithoutView(mScratchBuffer, maxScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
	{
		return false;
	}

	std::vector<nri::BufferBarrierDesc> blasBarriers;
	blasBarriers.reserve(mStaticMapScene.chunks.size());
	for (size_t chunkIndex = 0; chunkIndex < mStaticMapScene.chunks.size(); ++chunkIndex)
	{
		auto& chunk = mStaticMapScene.chunks[chunkIndex];
		const auto& atlasChunk = mStaticMapChunkAtlas.chunks[chunkIndex];
		nri::BottomLevelGeometryDesc geometryDesc = {};
		geometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
		geometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
		geometryDesc.triangles.vertexBuffer = mStaticVertexBuffer.buffer;
		geometryDesc.triangles.vertexOffset = 0;
		geometryDesc.triangles.vertexNum = mStaticMapChunkAtlas.vertexCount;
		geometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
		geometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
		geometryDesc.triangles.indexBuffer = mStaticIndexBuffer.buffer;
		geometryDesc.triangles.indexOffset = (uint64_t)atlasChunk.indexOffset * sizeof(uint32_t);
		geometryDesc.triangles.indexNum = atlasChunk.indexCount;
		geometryDesc.triangles.indexType = nri::IndexType::UINT32;

		nri::BuildBottomLevelAccelerationStructureDesc build = {};
		build.dst = chunk.accelerationStructure.accelerationStructure;
		build.geometries = &geometryDesc;
		build.geometryNum = 1;
		build.scratchBuffer = mScratchBuffer.buffer;
		build.scratchOffset = 0;
		mFrameBuffer->mRayTracing.CmdBuildBottomLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &build, 1);

		if (chunkIndex + 1 < mStaticMapScene.chunks.size())
		{
			nri::BufferBarrierDesc scratchBarrier = {};
			scratchBarrier.buffer = mScratchBuffer.buffer;
			scratchBarrier.before = NRIAccelerationStructureScratchAccess();
			scratchBarrier.after = NRIAccelerationStructureScratchAccess();

			nri::BarrierDesc scratchBarrierDesc = {};
			scratchBarrierDesc.buffers = &scratchBarrier;
			scratchBarrierDesc.bufferNum = 1;
			mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, scratchBarrierDesc);
		}

		nri::BufferBarrierDesc barrier = {};
		barrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*chunk.accelerationStructure.accelerationStructure);
		barrier.before = NRIAccelerationStructureWriteAccess();
		barrier.after = NRIAccelerationStructureReadAccess();
		blasBarriers.push_back(barrier);
	}

	if (!blasBarriers.empty())
	{
		nri::BarrierDesc blasBarrierDesc = {};
		blasBarrierDesc.buffers = blasBarriers.data();
		blasBarrierDesc.bufferNum = (uint32_t)blasBarriers.size();
		mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, blasBarrierDesc);
	}

	std::vector<nri::TopLevelInstance> instances;
	std::vector<SceneInstanceData> sceneInstances;
	BuildStaticMapInstances(instances, sceneInstances);
	mStaticAccelerationBuildSerial = mStaticMapScene.buildSerial;
	return
		BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static) &&
		UpdateSceneDataSet(
			mStaticVertexBuffer,
			mStaticIndexBuffer,
			mStaticPrimitiveBuffer,
			mStaticMaterialBuffer,
			mStaticVertexBuffer,
			mStaticIndexBuffer,
			mStaticPrimitiveBuffer,
			mStaticMaterialBuffer,
			sceneInstances,
			(uint32_t)mStaticMapScene.geometry.primitives.size(),
			0u,
			(uint32_t)mStaticMapScene.gpuMaterials.size(),
			0u,
			"build_static_map_scene");
}

bool NRIRenderer::BuildStaticMapAccelerationStructures(
	StaticMapSceneCache& staticScene,
	StaticMapSceneResources& staticResources,
	bool updateLiveState)
{
	Clocker clock(NriPTAcceleration);

	if (staticScene.chunks.empty() ||
		!staticResources.chunkAtlas.valid ||
		staticResources.chunkAtlas.chunks.size() != staticScene.chunks.size())
	{
		return false;
	}

	const bool needsWait =
		staticResources.topLevelAS.accelerationStructure != nullptr ||
		staticResources.tlasInstanceBuffer.buffer != nullptr ||
		staticResources.scratchBuffer.buffer != nullptr ||
		staticResources.topLevelScratchBuffer.buffer != nullptr;
	if (needsWait)
	{
		WaitForCommandsTracked();
	}

	DestroyBufferResource(staticResources.tlasInstanceBuffer);
	DestroyBufferResource(staticResources.scratchBuffer);
	DestroyBufferResource(staticResources.topLevelScratchBuffer);
	DestroyAccelerationStructureResource(staticResources.topLevelAS);

	for (auto& chunk : staticScene.chunks)
	{
		DestroyAccelerationStructureResource(chunk.accelerationStructure);
		chunk.residentBlasScratchSizeCacheKey = nullptr;
		chunk.residentBlasBuildScratchSize = 0;
		chunk.residentBlasUpdateScratchSize = 0;
	}

	uint64_t maxScratchSize = 0;
	for (size_t chunkIndex = 0; chunkIndex < staticScene.chunks.size(); ++chunkIndex)
	{
		auto& chunk = staticScene.chunks[chunkIndex];
		const auto& atlasChunk = staticResources.chunkAtlas.chunks[chunkIndex];
		nri::BottomLevelGeometryDesc geometryDesc = {};
		geometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
		geometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
		geometryDesc.triangles.vertexBuffer = staticResources.vertexBuffer.buffer;
		geometryDesc.triangles.vertexOffset = 0;
		geometryDesc.triangles.vertexNum = staticResources.chunkAtlas.vertexCount;
		geometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
		geometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
		geometryDesc.triangles.indexBuffer = staticResources.indexBuffer.buffer;
		geometryDesc.triangles.indexOffset = (uint64_t)atlasChunk.indexOffset * sizeof(uint32_t);
		geometryDesc.triangles.indexNum = atlasChunk.indexCount;
		geometryDesc.triangles.indexType = nri::IndexType::UINT32;

		nri::AccelerationStructureDesc blasDesc = {};
		blasDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
		blasDesc.flags = GetStaticMapChunkBlasBuildBits();
		blasDesc.geometryOrInstanceNum = 1;
		blasDesc.geometries = &geometryDesc;
		if (mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, blasDesc, chunk.accelerationStructure.accelerationStructure) != nri::Result::SUCCESS)
		{
			return false;
		}

		nri::MemoryDesc memoryDesc = {};
		mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(*chunk.accelerationStructure.accelerationStructure, nri::MemoryLocation::DEVICE, memoryDesc);
		chunk.accelerationStructure.memorySize = memoryDesc.size;
		chunk.accelerationStructure.memoryLocation = nri::MemoryLocation::DEVICE;

		const uint64_t scratchSize =
			mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*chunk.accelerationStructure.accelerationStructure);
		chunk.residentBlasScratchSizeCacheKey = chunk.accelerationStructure.accelerationStructure;
		chunk.residentBlasBuildScratchSize = scratchSize;
		chunk.residentBlasUpdateScratchSize = 0;
		maxScratchSize = std::max(maxScratchSize, scratchSize);
	}

	if (!CreateBufferWithoutView(staticResources.scratchBuffer, maxScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
	{
		return false;
	}

	std::vector<nri::BufferBarrierDesc> blasBarriers;
	blasBarriers.reserve(staticScene.chunks.size());
	for (size_t chunkIndex = 0; chunkIndex < staticScene.chunks.size(); ++chunkIndex)
	{
		auto& chunk = staticScene.chunks[chunkIndex];
		const auto& atlasChunk = staticResources.chunkAtlas.chunks[chunkIndex];
		nri::BottomLevelGeometryDesc geometryDesc = {};
		geometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
		geometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
		geometryDesc.triangles.vertexBuffer = staticResources.vertexBuffer.buffer;
		geometryDesc.triangles.vertexOffset = 0;
		geometryDesc.triangles.vertexNum = staticResources.chunkAtlas.vertexCount;
		geometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
		geometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
		geometryDesc.triangles.indexBuffer = staticResources.indexBuffer.buffer;
		geometryDesc.triangles.indexOffset = (uint64_t)atlasChunk.indexOffset * sizeof(uint32_t);
		geometryDesc.triangles.indexNum = atlasChunk.indexCount;
		geometryDesc.triangles.indexType = nri::IndexType::UINT32;

		nri::BuildBottomLevelAccelerationStructureDesc build = {};
		build.dst = chunk.accelerationStructure.accelerationStructure;
		build.geometries = &geometryDesc;
		build.geometryNum = 1;
		build.scratchBuffer = staticResources.scratchBuffer.buffer;
		build.scratchOffset = 0;
		mFrameBuffer->mRayTracing.CmdBuildBottomLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &build, 1);

		if (chunkIndex + 1 < staticScene.chunks.size())
		{
			// The static chunk path deliberately reuses one scratch buffer across many BLAS builds.
			// Serialize reuse explicitly so later builds do not stomp scratch data that the GPU is still consuming.
			nri::BufferBarrierDesc scratchBarrier = {};
			scratchBarrier.buffer = staticResources.scratchBuffer.buffer;
			scratchBarrier.before = NRIAccelerationStructureScratchAccess();
			scratchBarrier.after = NRIAccelerationStructureScratchAccess();

			nri::BarrierDesc scratchBarrierDesc = {};
			scratchBarrierDesc.buffers = &scratchBarrier;
			scratchBarrierDesc.bufferNum = 1;
			mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, scratchBarrierDesc);
		}

		nri::BufferBarrierDesc barrier = {};
		barrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*chunk.accelerationStructure.accelerationStructure);
		barrier.before = NRIAccelerationStructureWriteAccess();
		barrier.after = NRIAccelerationStructureReadAccess();
		blasBarriers.push_back(barrier);
	}

	if (!blasBarriers.empty())
	{
		nri::BarrierDesc blasBarrierDesc = {};
		blasBarrierDesc.buffers = blasBarriers.data();
		blasBarrierDesc.bufferNum = (uint32_t)blasBarriers.size();
		mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, blasBarrierDesc);
	}

	std::vector<nri::TopLevelInstance> instances;
	std::vector<SceneInstanceData> sceneInstances;
	BuildStaticMapInstances(staticScene, staticResources.chunkAtlas, instances, sceneInstances);
	staticResources.sceneInstances = sceneInstances;
	staticResources.accelerationBuildSerial = staticScene.buildSerial;
	return
		BuildTopLevelAccelerationStructure(
			instances,
			SceneDataBufferMask_Static,
			staticResources.topLevelAS,
			staticResources.tlasInstanceBuffer,
			staticResources.topLevelScratchBuffer,
			&staticResources.vertexBuffer,
			&staticResources.indexBuffer,
			&staticResources.tlasInstanceCount,
			updateLiveState,
			false);
}

void NRIRenderer::DestroyStaticMapSceneResources(StaticMapSceneCache& staticScene, StaticMapSceneResources& staticResources, bool waitForCommands)
{
	const bool hasResidentResources =
		!staticScene.chunks.empty() ||
		staticResources.vertexBuffer.buffer != nullptr ||
		staticResources.indexBuffer.buffer != nullptr ||
		staticResources.primitiveBuffer.buffer != nullptr ||
		staticResources.materialBuffer.buffer != nullptr ||
		staticResources.tlasInstanceBuffer.buffer != nullptr ||
		staticResources.scratchBuffer.buffer != nullptr ||
		staticResources.topLevelScratchBuffer.buffer != nullptr ||
		staticResources.topLevelAS.accelerationStructure != nullptr;
	if (waitForCommands && hasResidentResources && mFrameBuffer != nullptr)
	{
		WaitForCommandsTracked();
	}

	for (auto& chunk : staticScene.chunks)
	{
		DestroyAccelerationStructureResource(chunk.accelerationStructure);
		chunk.residentBlasScratchSizeCacheKey = nullptr;
		chunk.residentBlasBuildScratchSize = 0;
		chunk.residentBlasUpdateScratchSize = 0;
	}

	DestroyAccelerationStructureResource(staticResources.topLevelAS);
	DestroyBufferResource(staticResources.vertexBuffer);
	DestroyBufferResource(staticResources.indexBuffer);
	DestroyBufferResource(staticResources.primitiveBuffer);
	DestroyBufferResource(staticResources.materialBuffer);
	DestroyBufferResource(staticResources.tlasInstanceBuffer);
	DestroyBufferResource(staticResources.scratchBuffer);
	DestroyBufferResource(staticResources.topLevelScratchBuffer);

	if (&staticScene == &mStaticMapScene)
	{
		mSceneFrameGeometry.Reset();
	}
	staticScene = {};
	staticResources = {};
}

void NRIRenderer::DestroyStaticMapSceneCache(const char* reason)
{
	if (nri_ptscenestats)
	{
		Printf("NRI PT static scene trace: event=destroy reason=%s frame=%u scene_valid=%s textures=%s buffers=%s acceleration=%s scene_build_serial=%llu map_build_serial=%llu chunks=%u\n",
			reason != nullptr ? reason : "unspecified",
			mFrameIndex,
			YesNo(mStaticMapScene.valid),
			YesNo(mStaticMapScene.texturesResident),
			YesNo(mStaticMapScene.buffersResident),
			YesNo(mStaticMapScene.accelerationResident),
			(unsigned long long)mStaticMapScene.buildSerial,
			(unsigned long long)mMapWorld.buildSerial,
			(uint32_t)mStaticMapScene.chunks.size());
	}

	ResetPersistentDynamicEmissiveCache();
	const bool hasResidentStaticSceneResources =
		!mStaticMapScene.chunks.empty() ||
		mStaticVertexBuffer.buffer != nullptr ||
		mStaticIndexBuffer.buffer != nullptr ||
		mStaticPrimitiveBuffer.buffer != nullptr ||
		mStaticMaterialBuffer.buffer != nullptr;
	if (hasResidentStaticSceneResources && mFrameBuffer != nullptr)
	{
		// The resident PT static scene can still be referenced by the previous frame's
		// TLAS and descriptor bindings. Wait before tearing it down for live rebuilds.
		WaitForCommandsTracked();
	}

	for (auto& chunk : mStaticMapScene.chunks)
	{
		DestroyAccelerationStructureResource(chunk.accelerationStructure);
		chunk.residentBlasScratchSizeCacheKey = nullptr;
		chunk.residentBlasBuildScratchSize = 0;
		chunk.residentBlasUpdateScratchSize = 0;
	}

	DestroyBufferResource(mStaticVertexBuffer);
	DestroyBufferResource(mStaticIndexBuffer);
	DestroyBufferResource(mStaticPrimitiveBuffer);
	DestroyBufferResource(mStaticMaterialBuffer);
	mBoundStaticPrimitiveCount = 0;
	mBoundDynamicPrimitiveCount = 0;
	mBoundStaticMaterialCount = 0;
	mBoundDynamicMaterialCount = 0;
	mBoundPortalCount = 0;
	ResetStaticMapChunkAtlas(mStaticMapChunkAtlas);
	mSceneFrameGeometry.Reset();
	ResetRuntimeMutationCacheAndFrameForStaticScene();
	ResetResidentMapChunkRegistry();
}

void NRIRenderer::PrintStaticMapSceneStatus() const
{
	const char* source = mUsedStaticMapSceneLastFrame ? "authoritative-map-world" : "captured-scene";
	Printf("NRI PT static scene: source=%s resident=%s build_serial=%llu scene_builds=%u uploads=%u as_builds=%u animated_candidate_chunks=%u animated_refreshes=%u animated_refresh_uploads=%u animated_geometry_fallbacks=%u animated_refresh_suppressed=%u reuses=%u last_frame_upload=%s last_frame_as_build=%s chunks=%u tlas_instances=%u tris=%u materials=%u\n",
		source,
		(mStaticMapScene.valid && mStaticMapScene.texturesResident && mStaticMapScene.buffersResident && mStaticMapScene.accelerationResident) ? "yes" : "no",
		(unsigned long long)mStaticMapScene.buildSerial,
		mStaticMapScene.sceneBuildCount,
		mStaticMapScene.gpuUploadCount,
		mStaticMapScene.accelerationBuildCount,
		mStaticMapScene.animatedCandidateChunkCount,
		mStaticMapScene.animatedRefreshCount,
		mStaticMapScene.animatedRefreshUploadCount,
		mStaticMapScene.animatedGeometryFallbackCount,
		mStaticMapScene.animatedRefreshSuppressedChunkCount,
		mStaticMapScene.reuseCount,
		mUploadedStaticMapSceneLastFrame ? "yes" : "no",
		mBuiltStaticMapSceneASLastFrame ? "yes" : "no",
		(uint32_t)mStaticMapScene.chunks.size(),
		mStaticMapScene.tlasInstanceCount,
		(uint32_t)mStaticMapScene.geometry.primitives.size(),
		(uint32_t)mStaticMapScene.gpuMaterials.size());
}

bool NRIRenderer::PreloadStaticMapResources()
{
	const auto start = std::chrono::steady_clock::now();
	const bool wasResident =
		mStaticMapScene.valid &&
		mStaticMapScene.texturesResident &&
		mStaticMapScene.buffersResident &&
		mStaticMapScene.accelerationResident &&
		mStaticMapScene.buildSerial == mMapWorld.buildSerial;
	const uint32_t previousSceneBuildCount = mStaticMapScene.sceneBuildCount;
	const uint32_t previousUploadCount = mStaticMapScene.gpuUploadCount;
	const uint32_t previousAccelerationBuildCount = mStaticMapScene.accelerationBuildCount;
	const auto countDelta = [](uint32_t current, uint32_t previous) -> uint32_t
	{
		return current >= previous ? current - previous : current;
	};

	if (!EnsureStaticMapScene())
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading static chunk: event=failed level=%s build_serial=%llu scene_valid=%u textures=%u buffers=%u acceleration=%u map_valid=%u ms=%.3f\n",
				mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
				(unsigned long long)mMapWorld.buildSerial,
				mStaticMapScene.valid ? 1u : 0u,
				mStaticMapScene.texturesResident ? 1u : 0u,
				mStaticMapScene.buffersResident ? 1u : 0u,
				mStaticMapScene.accelerationResident ? 1u : 0u,
				mMapWorld.valid ? 1u : 0u,
				DurationMs(start, std::chrono::steady_clock::now()));
		}
		return false;
	}

	uint32_t activeChunks = 0;
	uint32_t blasResidentChunks = 0;
	for (const auto& chunk : mStaticMapScene.chunks)
	{
		if (chunk.active)
		{
			activeChunks++;
		}
		if (chunk.active && chunk.accelerationStructure.accelerationStructure != nullptr)
		{
			blasResidentChunks++;
		}
	}

	const bool atlasResident =
		mStaticMapChunkAtlas.valid &&
		mStaticMapChunkAtlas.buildSerial == mStaticMapScene.buildSerial &&
		mStaticMapChunkAtlas.chunks.size() == mStaticMapScene.chunks.size();
	const bool registryResident =
		mStaticSceneResidency.Registry().valid &&
		mStaticSceneResidency.Registry().buildSerial == mStaticMapScene.buildSerial;

	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading static chunk: event=%s level=%s build_serial=%llu scene_builds=%u uploads=%u as_builds=%u new_scene_builds=%u new_uploads=%u new_as_builds=%u chunks=%u active=%u blas=%u atlas=%u atlas_vertices=%u atlas_indices=%u atlas_primitives=%u atlas_materials=%u registry=%u registry_chunks=%u registry_mapped=%u registry_blas=%u light_chunks=%u textures=%u materials=%u tris=%u tlas_instances=%u ms=%.3f\n",
			wasResident ? "reuse" : "warm",
			mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
			(unsigned long long)mStaticMapScene.buildSerial,
			mStaticMapScene.sceneBuildCount,
			mStaticMapScene.gpuUploadCount,
			mStaticMapScene.accelerationBuildCount,
			countDelta(mStaticMapScene.sceneBuildCount, previousSceneBuildCount),
			countDelta(mStaticMapScene.gpuUploadCount, previousUploadCount),
			countDelta(mStaticMapScene.accelerationBuildCount, previousAccelerationBuildCount),
			(uint32_t)mStaticMapScene.chunks.size(),
			activeChunks,
			blasResidentChunks,
			atlasResident ? 1u : 0u,
			mStaticMapChunkAtlas.vertexCount,
			mStaticMapChunkAtlas.indexCount,
			mStaticMapChunkAtlas.primitiveCount,
			mStaticMapChunkAtlas.materialCount,
			registryResident ? 1u : 0u,
			mStaticSceneResidency.Registry().chunkCount,
			mStaticSceneResidency.Registry().mappedChunkCount,
			mStaticSceneResidency.Registry().accelerationResidentChunkCount,
			(uint32_t)mStaticMapScene.lightChunkViews.size(),
			(uint32_t)mStaticMapScene.materialBridge.textures.size(),
			(uint32_t)mStaticMapScene.gpuMaterials.size(),
			(uint32_t)mStaticMapScene.geometry.primitives.size(),
			mStaticMapScene.tlasInstanceCount,
			DurationMs(start, std::chrono::steady_clock::now()));
	}

	if ((int)nri_ptloadingtrace >= 2)
	{
		for (uint32_t chunkListIndex = 0; chunkListIndex < (uint32_t)mStaticMapScene.chunks.size(); ++chunkListIndex)
		{
			const auto& chunk = mStaticMapScene.chunks[chunkListIndex];
			const bool atlasChunkResident =
				atlasResident &&
				chunkListIndex < mStaticMapChunkAtlas.chunks.size() &&
				mStaticMapChunkAtlas.chunks[chunkListIndex].valid;
			const auto atlasChunk =
				atlasChunkResident ?
				mStaticMapChunkAtlas.chunks[chunkListIndex] :
				StaticMapChunkAtlas::ChunkEntry{};
			Printf("NRI PT loading static chunk: event=chunk chunk_list=%u chunk=%u active=%u blas=%u atlas=%u vertex_offset=%u vertex_count=%u index_offset=%u index_count=%u primitive_offset=%u primitive_count=%u material_offset=%u material_count=%u light_view=%u animated=%u suppressed=%u\n",
				chunkListIndex,
				chunk.chunkIndex,
				chunk.active ? 1u : 0u,
				chunk.accelerationStructure.accelerationStructure != nullptr ? 1u : 0u,
				atlasChunkResident ? 1u : 0u,
				atlasChunkResident ? atlasChunk.vertexOffset : chunk.vertexOffset,
				atlasChunkResident ? atlasChunk.vertexCount : chunk.vertexCount,
				atlasChunkResident ? atlasChunk.indexOffset : chunk.indexOffset,
				atlasChunkResident ? atlasChunk.indexCount : chunk.indexCount,
				atlasChunkResident ? atlasChunk.primitiveOffset : chunk.primitiveOffset,
				atlasChunkResident ? atlasChunk.primitiveCount : chunk.primitiveCount,
				atlasChunkResident ? atlasChunk.materialOffset : chunk.materialOffset,
				atlasChunkResident ? atlasChunk.materialCount : chunk.materialCount,
				chunkListIndex < mStaticMapScene.lightChunkViews.size() ? 1u : 0u,
				chunk.hasAnimatedTextureCandidates ? 1u : 0u,
				chunk.animatedRefreshSuppressed ? 1u : 0u);
		}
	}

	return true;
}
