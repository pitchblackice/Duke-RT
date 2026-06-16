#include "nri_static_scene.h"

#include "nri_runtime_mutation.h"

#include <vector>
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
			for (const RuntimeMutationResidentReplacementInfo& replacement : *input.replacements)
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

uint32_t NRIStaticSceneResidency::FindPreferredStaticSceneChunkListIndex(
	const StaticMapSceneCache& staticScene,
	const StaticMapChunkAtlas& atlas,
	uint32_t chunkIndex)
{
	uint32_t bestChunkListIndex = UINT32_MAX;
	uint32_t bestScore = 0;
	for (uint32_t chunkListIndex = 0; chunkListIndex < (uint32_t)staticScene.chunks.size(); ++chunkListIndex)
	{
		const auto& chunk = staticScene.chunks[chunkListIndex];
		if (chunk.chunkIndex != chunkIndex)
		{
			continue;
		}

		const uint32_t score = GetStaticSceneChunkSlotPreference(staticScene, atlas, chunkListIndex);
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

uint32_t NRIStaticSceneResidency::CountStaticSceneChunkSlots(
	const StaticMapSceneCache& staticScene,
	uint32_t chunkIndex)
{
	uint32_t count = 0;
	for (const auto& chunk : staticScene.chunks)
	{
		if (chunk.chunkIndex == chunkIndex)
		{
			count++;
		}
	}
	return count;
}

NRIStaticSceneResidency::ChunkDiagnosticFacts NRIStaticSceneResidency::BuildChunkDiagnosticFacts(
	const StaticMapSceneCache& staticScene,
	const StaticMapChunkAtlas& atlas,
	uint32_t chunkIndex)
{
	ChunkDiagnosticFacts facts = {};
	uint32_t bestScore = 0;
	for (uint32_t chunkListIndex = 0; chunkListIndex < (uint32_t)staticScene.chunks.size(); ++chunkListIndex)
	{
		const auto& chunk = staticScene.chunks[chunkListIndex];
		if (chunk.chunkIndex != chunkIndex)
		{
			continue;
		}

		facts.duplicateChunkSlotCount++;
		const uint32_t score = GetStaticSceneChunkSlotPreference(staticScene, atlas, chunkListIndex);
		if (facts.preferredChunkListIndex == UINT32_MAX ||
			score > bestScore ||
			(score == bestScore && chunkListIndex > facts.preferredChunkListIndex))
		{
			facts.preferredChunkListIndex = chunkListIndex;
			bestScore = score;
		}
	}

	if (facts.preferredChunkListIndex >= staticScene.chunks.size())
	{
		return facts;
	}

	const auto& staticChunk = staticScene.chunks[facts.preferredChunkListIndex];
	facts.hasStaticChunk = true;
	facts.residentStatic = true;
	facts.staticTlasInstanced = staticChunk.active && staticChunk.accelerationStructure.accelerationStructure != nullptr;
	facts.staticProbeIncluded = staticChunk.active;
	facts.staticPrimitiveOffset = staticChunk.primitiveOffset;
	facts.staticPrimitiveCount = staticChunk.primitiveCount;
	facts.staticMaterialOffset = staticChunk.materialOffset;
	facts.staticMaterialCount = staticChunk.materialCount;
	facts.staticAsReady = staticChunk.accelerationStructure.accelerationStructure != nullptr;
	return facts;
}
