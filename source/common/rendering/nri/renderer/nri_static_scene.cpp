#include "nri_static_scene.h"

#include "nri_renderer.h"

#include <algorithm>

void NRIRenderer::ResetResidentMapChunkRegistry()
{
	mResidentMapChunkRegistry = {};
}

uint32_t NRIRenderer::GetStaticSceneChunkSlotPreference(uint32_t chunkListIndex) const
{
	if (chunkListIndex >= mStaticMapScene.chunks.size())
	{
		return 0;
	}

	const auto& chunk = mStaticMapScene.chunks[chunkListIndex];
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
	if (mStaticMapChunkAtlas.valid &&
		chunkListIndex < mStaticMapChunkAtlas.chunks.size() &&
		mStaticMapChunkAtlas.chunks[chunkListIndex].valid)
	{
		score += 1u;
	}
	return score;
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
