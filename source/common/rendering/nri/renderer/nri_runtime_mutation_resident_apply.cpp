#include "nri_renderer.h"

#include "../scene/nri_hash.h"
#include "../scene/nri_map_builder.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "nri_render_geometry_helpers.h"
#include "nri_runtime_mutation_shared.h"
#include "c_cvars.h"
#include "gamecontrol.h"
#include "gamestate.h"
#include "hw_sections.h"
#include "mapinfo.h"
#include "printf.h"
#include "texinfo.h"
#include "texturemanager.h"

#include <array>
#include <chrono>
#include <cstring>
#include <unordered_map>

EXTERN_CVAR(Bool, nri_ptceilingnudge)
EXTERN_CVAR(Float, nri_ptceilingnudgedistance)
EXTERN_CVAR(Int, nri_ptmutationtracechunk)
EXTERN_CVAR(Int, nri_ptmutationtracesector)
EXTERN_CVAR(Int, nri_ptmutationworklistvalidate)
EXTERN_CVAR(Bool, nri_ptslowdowntrace)
EXTERN_CVAR(Bool, nri_pttemporaltrace)
EXTERN_CVAR(Int, nri_pttraceframes)
EXTERN_CVAR(Int, perf_looptraceframes)

namespace
{
	static bool ShouldEmitTemporalTraceLogs()
	{
		return !!nri_pttemporaltrace && nri_pttraceframes > 0;
	}

	static bool ShouldTracePtPerf()
	{
		return (int)perf_looptraceframes > 0 || ShouldEmitTemporalTraceLogs();
	}

	static bool ShouldCollectOverlayPerfTiming()
	{
		return ShouldTracePtPerf() || (bool)nri_ptslowdowntrace;
	}

	static double RuntimeMutationOverlayDurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	class ScopedPtPerfTimer
	{
	public:
		explicit ScopedPtPerfTimer(double& targetMs)
			: mTarget(ShouldCollectOverlayPerfTiming() ? &targetMs : nullptr)
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
				*mTarget += RuntimeMutationOverlayDurationMs(mStart, std::chrono::steady_clock::now());
			}
		}

	private:
		double* mTarget = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};

	static const char* YesNo(bool value)
	{
		return value ? "yes" : "no";
	}

	using nri_runtime_mutation::ChunkHasUnresolvedAuthoredTextures;
	using nri_runtime_mutation::ComputeAnimatedGeometrySignature;
	using nri_runtime_mutation::ComputeAnimatedMaterialSignature;
	using nri_runtime_mutation::ComputeExactGeometrySignature;
	using nri_runtime_mutation::ComputeRecurringChunkStateSignature;
	using nri_runtime_mutation::CountSceneViewSurfaces;
	using nri_runtime_mutation::FilterMaterialOnlyReplacementSceneView;
	using nri_runtime_mutation::HashMaterialBridgeSummary;
	using nri_runtime_mutation::IsChunkMarkedVisible;
	using nri_runtime_mutation::NudgeBlindSpotReplacementFlats;
	using nri_runtime_mutation::RuntimeMutationFloatBits;
	using nri_runtime_mutation::RuntimeMutationHashCombine64;
	using nri_runtime_mutation::SceneViewHasSectorDrivenWallBands;
	using nri_runtime_mutation::SceneViewUsesHardwareCanvasTexture;
	using nri_runtime_mutation::TryBuildMergedSectorMaterialOnlyBridge;
	using nri_runtime_mutation::TryBuildMergedSectorMaterialOnlySceneView;

	static uint32_t ScoreRuntimeSectorDirtyTruthEntry(const NRIRenderer::RuntimeSectorDirtyTruthTraceEntry& entry)
	{
		uint32_t score = 0;
		if (entry.forceTopology) score += 1u << 18;
		if (entry.baselineChanged) score += 1u << 17;
		if (entry.geometryChanged) score += 1u << 16;
		if (entry.materialChanged) score += 1u << 15;
		score += entry.liveTriangleCount * 8u;
		score += entry.liveSurfaceCount * 4u;
		return score;
	}

	static uint32_t ScoreRuntimeAnimatedChurnTraceEntry(const NRIRenderer::RuntimeAnimatedChurnTraceEntry& entry)
	{
		uint32_t score = entry.materialRefreshes * 96u +
			entry.runtimeAttempts * 64u +
			entry.residentApplies * 48u +
			entry.syncSkips * 32u +
			entry.suppressionEmits * 16u;
		if (entry.suppressed)
		{
			score += 1u << 20;
		}
		return score;
	}

	static uint32_t ScoreRuntimeMaterialOnlyMismatchTraceEntry(const NRIRenderer::RuntimeMaterialOnlyMismatchTraceEntry& entry)
	{
		const uint32_t materialDelta =
			entry.residentMaterialCount > entry.filteredMaterialCount ?
			entry.residentMaterialCount - entry.filteredMaterialCount :
			entry.filteredMaterialCount - entry.residentMaterialCount;
		uint32_t score = materialDelta * 128u;
		score += entry.residentMaterialCount * 16u;
		score += entry.filteredSurfaceCount * 8u;
		score += (entry.filteredWallCount + entry.filteredFlatCount) * 4u;
		if (entry.filteredWallCount != 0 && entry.filteredFlatCount != 0) score += 1u << 18;
		if (entry.residentWallCount != 0 && entry.residentFlatCount != 0) score += 1u << 17;
		return score;
	}

	enum RuntimeStructuralRebuildTriggerBits : uint32_t
	{
		RuntimeStructuralRebuildTrigger_ReplacementDelta = 1u << 0,
		RuntimeStructuralRebuildTrigger_ViewChanged = 1u << 1,
		RuntimeStructuralRebuildTrigger_StaticAnimatedFlip = 1u << 2,
		RuntimeStructuralRebuildTrigger_ExcludeStaticFlip = 1u << 3,
		RuntimeStructuralRebuildTrigger_ForceTopology = 1u << 4,
		RuntimeStructuralRebuildTrigger_Invalid = 1u << 5,
	};

	static uint32_t ScoreRuntimeStructuralRebuildTraceEntry(const NRIRenderer::RuntimeStructuralRebuildTraceEntry& entry)
	{
		uint32_t score = entry.triangleCount * 8u;
		score += entry.surfaceCount * 4u;
		score += entry.materialCount * 2u;
		if ((entry.triggerMask & RuntimeStructuralRebuildTrigger_ForceTopology) != 0) score += 1u << 20;
		if ((entry.triggerMask & RuntimeStructuralRebuildTrigger_StaticAnimatedFlip) != 0) score += 1u << 19;
		if ((entry.triggerMask & RuntimeStructuralRebuildTrigger_ExcludeStaticFlip) != 0) score += 1u << 18;
		if (entry.mixedMaterialOnly) score += 1u << 17;
		if (entry.geometryOrDirty) score += 1u << 16;
		return score;
	}

	static uint32_t ScoreRuntimeRecurringChunkTraceEntry(const NRIRenderer::RuntimeRecurringChunkTraceEntry& entry)
	{
		uint32_t score = entry.repeatedStateHitCount * 256u;
		score += entry.abaRecurrenceCount * 192u;
		score += entry.transitionCount * 64u;
		score += entry.uniqueStateCount * 32u;
		score += entry.visitCount * 8u;
		score += entry.lastTriangleCount * 4u;
		score += entry.lastMaterialCount * 2u;
		return score;
	}

	enum RuntimeGeometryDirtyFamilyBits : uint32_t
	{
		RuntimeGeometryDirtyFamily_SectorGeometryOnly = 1u << 0,
		RuntimeGeometryDirtyFamily_WallGeometryOnly = 1u << 1,
		RuntimeGeometryDirtyFamily_SectorWallGeometry = 1u << 2,
		RuntimeGeometryDirtyFamily_DirtyOnly = 1u << 3,
		RuntimeGeometryDirtyFamily_GeometryDirtyMixed = 1u << 4,
	};

	static uint32_t ScoreRuntimeGeometryDirtyTraceEntry(const NRIRenderer::RuntimeGeometryDirtyTraceEntry& entry)
	{
		const uint32_t triangleDelta =
			entry.previousTriangleCount > entry.liveTriangleCount ?
			entry.previousTriangleCount - entry.liveTriangleCount :
			entry.liveTriangleCount - entry.previousTriangleCount;
		const uint32_t materialDelta =
			entry.previousMaterialCount > entry.liveMaterialCount ?
			entry.previousMaterialCount - entry.liveMaterialCount :
			entry.liveMaterialCount - entry.previousMaterialCount;
		uint32_t score = triangleDelta * 32u;
		score += materialDelta * 16u;
		score += entry.liveTriangleCount * 4u;
		score += entry.liveMaterialCount * 2u;
		if ((entry.familyMask & RuntimeGeometryDirtyFamily_GeometryDirtyMixed) != 0) score += 1u << 20;
		if ((entry.familyMask & RuntimeGeometryDirtyFamily_SectorWallGeometry) != 0) score += 1u << 19;
		if (entry.forceTopology) score += 1u << 18;
		if (entry.countChanged) score += 1u << 17;
		if (entry.wallsChanged && entry.flatsChanged) score += 1u << 16;
		return score;
	}

	template <typename Entry, size_t N, typename ScoreFn>
	void InsertRankedTraceEntry(std::array<Entry, N>& entries, Entry entry, ScoreFn scoreFn)
	{
		entry.score = scoreFn(entry);
		size_t insertIndex = N;
		for (size_t i = 0; i < N; ++i)
		{
			if (!entries[i].valid || entry.score > entries[i].score)
			{
				insertIndex = i;
				break;
			}
		}

		if (insertIndex >= N)
		{
			return;
		}

		for (size_t i = N - 1; i > insertIndex; --i)
		{
			entries[i] = entries[i - 1];
		}
		entries[insertIndex] = entry;
		entries[insertIndex].valid = true;
	}
}

namespace
{
	static nri::AccessStage NRIComputeShaderResourceAccess()
	{
		return { nri::AccessBits::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER };
	}

	static bool ShouldTraceResidentGeometryOrderHash()
	{
		return (int)perf_looptraceframes > 0;
	}

	static int32_t FindMapChunkIndexForSector(const nri_scene::PTMapWorld& mapWorld, int32_t sectorIndex)
	{
		if (!mapWorld.valid || sectorIndex < 0)
		{
			return -1;
		}
		if ((size_t)sectorIndex < mapWorld.sectorChunkLookup.size())
		{
			const uint32_t chunkIndex = mapWorld.sectorChunkLookup[(size_t)sectorIndex];
			if (chunkIndex != UINT32_MAX)
			{
				return (int32_t)chunkIndex;
			}
		}

		for (const auto& chunk : mapWorld.chunks)
		{
			if (chunk.kind == nri_scene::PTMapChunkKind::Sector && chunk.sectorIndex == sectorIndex)
			{
				return (int32_t)chunk.chunkIndex;
			}
		}

		return -1;
	}

	static int32_t ResolveVisibilityChunkIndexForProvenance(const nri_scene::PTMapWorld& mapWorld, const nri_scene::SurfaceProvenance& provenance)
	{
		if (provenance.mapChunkIndex >= 0)
		{
			return provenance.mapChunkIndex;
		}
		return FindMapChunkIndexForSector(mapWorld, provenance.sectorIndex);
	}

	static uint32_t NormalizeResidentAtlasIndex(uint32_t value, uint32_t base)
	{
		return value >= base ? value - base : UINT32_MAX;
	}

	static uint64_t HashResidentGeometryVertexPayload(uint64_t hash, const nri_scene::SceneVertex& vertex)
	{
		for (float component : vertex.position)
		{
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(component));
		}
		for (float component : vertex.prevPosition)
		{
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(component));
		}
		for (float component : vertex.uv)
		{
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(component));
		}
		return hash;
	}

	static uint64_t HashResidentGeometryProvenancePayload(
		uint64_t hash,
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::GeometryData& geometry,
		const nri_scene::PrimitiveData& primitive,
		uint32_t primitiveIndex)
	{
		const bool hasProvenance = primitiveIndex < geometry.primitiveProvenance.size();
		const uint32_t visibilityChunk =
			hasProvenance ?
			(uint32_t)ResolveVisibilityChunkIndexForProvenance(mapWorld, geometry.primitiveProvenance[primitiveIndex]) :
			primitive.reserved0;
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)visibilityChunk);
		if (hasProvenance)
		{
			const auto& provenance = geometry.primitiveProvenance[primitiveIndex];
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)provenance.sourceType);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)provenance.sectorIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)provenance.wallIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)provenance.sectionIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)provenance.mapChunkIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)provenance.nextSectorIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)provenance.actorIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)provenance.drawListType);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)provenance.cstat);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)provenance.materialFlags);
		}
		else
		{
			hash = RuntimeMutationHashCombine64(hash, UINT64_MAX);
		}
		return hash;
	}

	static uint64_t HashResidentMaterialPayload(const nri_scene::MaterialBridgeData& materials)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)materials.materials.size());
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)materials.lightMetadata.size());
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)materials.textures.size());
		for (const auto& material : materials.materials)
		{
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.textureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.paletteIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.flags);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.materialClass);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.lightingFlags);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.normalTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.metallicTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.roughnessTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.sectorIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.emissiveTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(material.lightLevel));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(material.alpha));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(material.roughnessHint));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(material.metalnessHint));
			for (float color : material.emissiveColor)
			{
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(color));
			}
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(material.emissiveIntensity));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(material.emissiveMaskScale));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.emissiveMode);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(material.emissiveReserved));
		}

		for (const auto& metadata : materials.lightMetadata)
		{
			hash = RuntimeMutationHashCombine64(hash, metadata.materialKey);
			hash = RuntimeMutationHashCombine64(hash, metadata.textureContentKey);
			hash = RuntimeMutationHashCombine64(hash, metadata.glowmapContentKey);
			hash = RuntimeMutationHashCombine64(hash, metadata.normalContentKey);
			hash = RuntimeMutationHashCombine64(hash, metadata.metallicContentKey);
			hash = RuntimeMutationHashCombine64(hash, metadata.roughnessContentKey);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.textureId);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.textureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.glowmapTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.normalTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.metallicTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.roughnessTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.emissiveTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.paletteIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.materialFlags);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.lightingFlags);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.materialClass);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.emissiveMode);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)metadata.sourceType);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)metadata.sectorIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)metadata.actorIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)metadata.shade);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(metadata.alpha));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(metadata.lightLevel));
			for (float color : metadata.averageColor)
			{
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(color));
			}
			for (float color : metadata.glowColor)
			{
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(color));
			}
			for (float color : metadata.emissiveColor)
			{
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(color));
			}
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(metadata.emissiveIntensity));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(metadata.emissiveMaskScale));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(metadata.visibleFullbrightBoost));
		}

		for (const auto& texture : materials.textures)
		{
			hash = RuntimeMutationHashCombine64(hash, texture.key);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)texture.width);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)texture.height);
			hash = RuntimeMutationHashCombine64(hash, texture.indexed ? 1ull : 0ull);
		}

		return hash != 0 ? hash : 1;
	}

	static uint64_t HashResidentGeometryPayload(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::GeometryData& geometry,
		uint32_t vertexOffset,
		uint32_t vertexCount,
		uint32_t indexOffset,
		uint32_t indexCount,
		uint32_t primitiveOffset,
		uint32_t primitiveCount,
		uint32_t materialOffset,
		uint32_t materialCount)
	{
		if (vertexOffset + vertexCount > geometry.vertices.size() ||
			indexOffset + indexCount > geometry.indices.size() ||
			primitiveOffset + primitiveCount > geometry.primitives.size())
		{
			return 0;
		}

		uint64_t hash = 1469598103934665603ull;
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)vertexCount);
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)indexCount);
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)primitiveCount);
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)materialCount);
		for (uint32_t i = 0; i < vertexCount; ++i)
		{
			hash = HashResidentGeometryVertexPayload(hash, geometry.vertices[vertexOffset + i]);
		}

		for (uint32_t i = 0; i < indexCount; ++i)
		{
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)NormalizeResidentAtlasIndex(geometry.indices[indexOffset + i], vertexOffset));
		}

		for (uint32_t i = 0; i < primitiveCount; ++i)
		{
			const uint32_t primitiveIndex = primitiveOffset + i;
			const auto& primitive = geometry.primitives[primitiveIndex];
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)NormalizeResidentAtlasIndex(primitive.indices[0], vertexOffset));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)NormalizeResidentAtlasIndex(primitive.indices[1], vertexOffset));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)NormalizeResidentAtlasIndex(primitive.indices[2], vertexOffset));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)NormalizeResidentAtlasIndex(primitive.materialIndex, materialOffset));
			for (float component : primitive.uv0)
			{
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(component));
			}
			for (float component : primitive.uv1)
			{
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(component));
			}
			for (float component : primitive.uv2)
			{
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(component));
			}
			for (float component : primitive.normal)
			{
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(component));
			}
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)primitive.flags);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)primitive.portalIndex);
			hash = HashResidentGeometryProvenancePayload(hash, mapWorld, geometry, primitive, primitiveIndex);
		}

		return hash != 0 ? hash : 1;
	}

	static uint64_t HashResidentGeometryPayloadOrderIndependent(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::GeometryData& geometry,
		uint32_t vertexOffset,
		uint32_t vertexCount,
		uint32_t primitiveOffset,
		uint32_t primitiveCount,
		uint32_t materialOffset,
		uint32_t materialCount)
	{
		if (vertexOffset + vertexCount > geometry.vertices.size() ||
			primitiveOffset + primitiveCount > geometry.primitives.size())
		{
			return 0;
		}

		std::vector<uint64_t> primitiveHashes;
		primitiveHashes.reserve(primitiveCount);
		for (uint32_t i = 0; i < primitiveCount; ++i)
		{
			const uint32_t primitiveIndex = primitiveOffset + i;
			const auto& primitive = geometry.primitives[primitiveIndex];
			uint64_t primitiveHash = 1469598103934665603ull;
			primitiveHash = RuntimeMutationHashCombine64(primitiveHash, (uint64_t)NormalizeResidentAtlasIndex(primitive.materialIndex, materialOffset));
			for (float component : primitive.normal)
			{
				primitiveHash = RuntimeMutationHashCombine64(primitiveHash, (uint64_t)RuntimeMutationFloatBits(component));
			}
			primitiveHash = RuntimeMutationHashCombine64(primitiveHash, (uint64_t)primitive.flags);
			primitiveHash = RuntimeMutationHashCombine64(primitiveHash, (uint64_t)primitive.portalIndex);
			primitiveHash = HashResidentGeometryProvenancePayload(primitiveHash, mapWorld, geometry, primitive, primitiveIndex);

			const float* primitiveUvs[3] = { primitive.uv0, primitive.uv1, primitive.uv2 };
			for (uint32_t corner = 0; corner < 3; ++corner)
			{
				const uint32_t vertexIndex = primitive.indices[corner];
				if (vertexIndex < vertexOffset || vertexIndex >= vertexOffset + vertexCount)
				{
					return 0;
				}
				primitiveHash = HashResidentGeometryVertexPayload(primitiveHash, geometry.vertices[vertexIndex]);
				for (uint32_t component = 0; component < 2; ++component)
				{
					primitiveHash = RuntimeMutationHashCombine64(primitiveHash, (uint64_t)RuntimeMutationFloatBits(primitiveUvs[corner][component]));
				}
			}
			primitiveHashes.push_back(primitiveHash);
		}

		std::sort(primitiveHashes.begin(), primitiveHashes.end());
		uint64_t hash = 1469598103934665603ull;
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)vertexCount);
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)primitiveCount);
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)materialCount);
		for (uint64_t primitiveHash : primitiveHashes)
		{
			hash = RuntimeMutationHashCombine64(hash, primitiveHash);
		}
		return hash != 0 ? hash : 1;
	}

	static uint64_t ComputeGeometryTopologySignature(const nri_scene::GeometryData& geometry)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)geometry.vertices.size());
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)geometry.indices.size());
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)geometry.primitives.size());
		for (uint32_t index : geometry.indices)
		{
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)index);
		}
		return hash;
	}

	static uint64_t ComputePrimitiveLayoutSignature(const nri_scene::GeometryData& geometry)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)geometry.primitives.size());
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)geometry.primitiveProvenance.size());
		for (size_t i = 0; i < geometry.primitives.size(); ++i)
		{
			const nri_scene::PrimitiveData& primitive = geometry.primitives[i];
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)primitive.indices[0]);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)primitive.indices[1]);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)primitive.indices[2]);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)primitive.materialIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(primitive.uv0[0]));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(primitive.uv0[1]));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(primitive.uv1[0]));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(primitive.uv1[1]));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(primitive.uv2[0]));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(primitive.uv2[1]));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(primitive.normal[0]));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(primitive.normal[1]));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(primitive.normal[2]));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)primitive.flags);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)primitive.portalIndex);
			if (i < geometry.primitiveProvenance.size())
			{
				const nri_scene::SurfaceProvenance& provenance = geometry.primitiveProvenance[i];
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)provenance.sourceType);
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(provenance.sectorIndex + 1));
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(provenance.wallIndex + 1));
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(provenance.sectionIndex + 1));
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(provenance.mapChunkIndex + 1));
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(provenance.nextSectorIndex + 1));
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(provenance.actorIndex + 1));
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)provenance.drawListType);
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)provenance.cstat);
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)provenance.materialFlags);
			}
		}
		return hash;
	}

	static FTextureID ResolveAuthoredTextureIdForStaticMapSurface(const nri_scene::PTMapSurface& surface)
	{
		switch (surface.kind)
		{
		case nri_scene::PTMapSurfaceKind::Floor:
		{
			const int32_t sectorIndex = surface.surface.provenance.sectorIndex;
			return sectorIndex >= 0 && (unsigned)sectorIndex < sector.Size() ? sector[(unsigned)sectorIndex].floortexture : FNullTextureID();
		}
		case nri_scene::PTMapSurfaceKind::Ceiling:
		{
			const int32_t sectorIndex = surface.surface.provenance.sectorIndex;
			return sectorIndex >= 0 && (unsigned)sectorIndex < sector.Size() ? sector[(unsigned)sectorIndex].ceilingtexture : FNullTextureID();
		}
		case nri_scene::PTMapSurfaceKind::WallOneSided:
		{
			const int32_t wallIndex = surface.surface.provenance.wallIndex;
			if (wallIndex < 0 || (unsigned)wallIndex >= wall.Size())
			{
				return FNullTextureID();
			}
			const walltype& wal = wall[(unsigned)wallIndex];
			return ((wal.cstat & CSTAT_WALL_1WAY) != 0 && wal.nextwall != -1) ? wal.overtexture : wal.walltexture;
		}
		case nri_scene::PTMapSurfaceKind::WallUpper:
		case nri_scene::PTMapSurfaceKind::WallMiddle:
		{
			const int32_t wallIndex = surface.surface.provenance.wallIndex;
			return wallIndex >= 0 && (unsigned)wallIndex < wall.Size() ? wall[(unsigned)wallIndex].overtexture : FNullTextureID();
		}
		case nri_scene::PTMapSurfaceKind::WallLower:
		{
			const int32_t wallIndex = surface.surface.provenance.wallIndex;
			if (wallIndex < 0 || (unsigned)wallIndex >= wall.Size())
			{
				return FNullTextureID();
			}
			const walltype& wal = wall[(unsigned)wallIndex];
			if ((wal.cstat & CSTAT_WALL_BOTTOM_SWAP) != 0 && wal.nextwall >= 0 && (unsigned)wal.nextwall < wall.Size())
			{
				return wall[(unsigned)wal.nextwall].walltexture;
			}
			return wal.walltexture;
		}
		default:
			return FNullTextureID();
		}
	}

	static bool IsAnimatedStaticMapSurfaceCandidate(const nri_scene::PTMapSurface& surface)
	{
		const FTextureID textureId = ResolveAuthoredTextureIdForStaticMapSurface(surface);
		return textureId.isValid() && GetExtInfo(textureId).picanm.type() != 0;
	}

	static bool ChunkHasAnimatedStaticMapSurfaceCandidates(const nri_scene::PTMapWorld& mapWorld, const nri_scene::PTMapChunk& chunk)
	{
		const uint32_t endSurface = std::min<uint32_t>(chunk.firstSurface + chunk.surfaceCount, (uint32_t)mapWorld.surfaces.size());
		for (uint32_t surfaceIndex = chunk.firstSurface; surfaceIndex < endSurface; ++surfaceIndex)
		{
			if (IsAnimatedStaticMapSurfaceCandidate(mapWorld.surfaces[surfaceIndex]))
			{
				return true;
			}
		}
		return false;
	}

	enum RuntimeResidentBlasRefitRejectBits : uint32_t
	{
		RuntimeResidentBlasRefitReject_NoPreviousAs = 1u << 0,
		RuntimeResidentBlasRefitReject_IndexCountMismatch = 1u << 1,
		RuntimeResidentBlasRefitReject_PrimitiveCountMismatch = 1u << 2,
		RuntimeResidentBlasRefitReject_ZeroIndexCount = 1u << 3,
		RuntimeResidentBlasRefitReject_ZeroPrimitiveCount = 1u << 4,
	};

	static uint32_t ScoreRuntimeResidentBlasRefitRejectTraceEntry(const NRIRenderer::RuntimeResidentBlasRefitRejectTraceEntry& entry)
	{
		const uint32_t indexDelta =
			entry.previousIndexCount > entry.liveIndexCount ?
			entry.previousIndexCount - entry.liveIndexCount :
			entry.liveIndexCount - entry.previousIndexCount;
		const uint32_t primitiveDelta =
			entry.previousPrimitiveCount > entry.livePrimitiveCount ?
			entry.previousPrimitiveCount - entry.livePrimitiveCount :
			entry.livePrimitiveCount - entry.previousPrimitiveCount;
		uint32_t score = indexDelta * 16u;
		score += primitiveDelta * 16u;
		score += entry.livePrimitiveCount * 4u;
		score += entry.liveIndexCount * 2u;
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_IndexCountMismatch) != 0) score += 1u << 20;
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_PrimitiveCountMismatch) != 0) score += 1u << 19;
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_NoPreviousAs) != 0) score += 1u << 18;
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_ZeroIndexCount) != 0) score += 1u << 17;
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_ZeroPrimitiveCount) != 0) score += 1u << 16;
		return score;
	}
}

// Runtime mutation resident-apply renderer-service implementation.
bool NRIRenderer::TryApplyRuntimeMutationChunkToResidentSceneImpl(
	const nri_scene::PTMapChunk& mapChunk,
	RuntimeMapMutationCache::ChunkReplacement& replacement,
	RuntimeMutationResidentApplyResult& outResult)
{
	outResult = {};
	ScopedPtPerfTimer residentApplyPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyMs);
	mLastPerfShellTraceStats.runtimeMutationResidentApplyCount++;
	if (!mStaticMapScene.valid ||
		!mStaticMapScene.buffersResident ||
		!mStaticMapScene.accelerationResident ||
		!mStaticMapChunkAtlas.valid ||
		mapChunk.chunkIndex >= mResidentMapChunkRegistry.entries.size())
	{
		return false;
	}

	auto& entry = mResidentMapChunkRegistry.entries[mapChunk.chunkIndex];
	if (!entry.valid)
	{
		return false;
	}
	const uint32_t resolvedChunkListIndex =
		entry.staticSceneChunkListIndex < mStaticMapScene.chunks.size() &&
		mStaticMapScene.chunks[entry.staticSceneChunkListIndex].chunkIndex == mapChunk.chunkIndex ?
		entry.staticSceneChunkListIndex :
		FindPreferredStaticSceneChunkListIndex(mapChunk.chunkIndex);
	const bool hasResidentChunk =
		entry.active &&
		entry.mappedInStaticScene &&
		resolvedChunkListIndex < mStaticMapScene.chunks.size() &&
		resolvedChunkListIndex < mStaticMapChunkAtlas.chunks.size() &&
		mStaticMapScene.chunks[resolvedChunkListIndex].active;
	const bool hasChunkSlot =
		resolvedChunkListIndex < mStaticMapScene.chunks.size() &&
		resolvedChunkListIndex < mStaticMapChunkAtlas.chunks.size();
	if (hasChunkSlot)
	{
		entry.staticSceneChunkListIndex = resolvedChunkListIndex;
	}

	const bool hasResolvedAtlasChunk = resolvedChunkListIndex < mStaticMapChunkAtlas.chunks.size();
	const uint32_t resolvedAtlasMaterialCount =
		hasResolvedAtlasChunk ? mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].materialCount : 0u;
	const RuntimeMutationResidentApplyMode applyMode =
		mRuntimeMutation.ClassifyResidentApplyMode(replacement, hasResidentChunk, hasResolvedAtlasChunk, resolvedAtlasMaterialCount);
	const RuntimeMutationResidentApplyModeStats applyModeStats =
		mRuntimeMutation.BuildResidentApplyModeStats(applyMode, replacement, hasResidentChunk, hasResolvedAtlasChunk, resolvedAtlasMaterialCount);
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyCount += applyModeStats.materialOnlyCount;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyFastMaterialOnlyCount += applyModeStats.fastMaterialOnlyCount;
	mLastPerfShellTraceStats.runtimeMutationResidentApplySlowMaterialOnlyCount += applyModeStats.slowMaterialOnlyCount;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyExclusiveCount += applyModeStats.materialOnlyExclusiveCount;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyNoResidentChunkCount += applyModeStats.materialOnlyNoResidentChunkCount;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyInvalidReplacementCount += applyModeStats.materialOnlyInvalidReplacementCount;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyMaterialCountMismatchCount += applyModeStats.materialOnlyMaterialCountMismatchCount;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStructuralCount += applyModeStats.structuralCount;
	const bool materialOnlyReplacement = applyMode.materialOnlyReplacement;
	const bool exclusiveMaterialOnlyReplacement = applyMode.exclusiveMaterialOnlyReplacement;
	const bool fastResidentMaterialOnlyUpdate = applyMode.fastResidentMaterialOnlyUpdate;

	nri_scene::SceneView residentSceneView;
	nri_scene::GeometryData residentGeometry;
	nri_scene::MaterialBridgeData residentMaterials;
	nri_scene::PTMapChunkMutationBaseline appliedBaseline;

	if (fastResidentMaterialOnlyUpdate)
	{
		const bool mergedSectorMaterialOnlyFastPath =
			IsPureSectorRuntimeMutationMaterialOnlyReasonMask(replacement.reasonMask) &&
			hasResidentChunk &&
			resolvedChunkListIndex < mStaticMapScene.lightChunkViews.size() &&
			TryBuildMergedSectorMaterialOnlySceneView(
				mStaticMapScene.lightChunkViews[resolvedChunkListIndex],
				replacement.sceneView,
				residentSceneView);
		if (!mergedSectorMaterialOnlyFastPath)
		{
			residentSceneView = replacement.sceneView;
		}
		residentMaterials = replacement.materialBridge;
		appliedBaseline = replacement.replacementBaseline;
	}
	else if (materialOnlyReplacement && !exclusiveMaterialOnlyReplacement)
	{
		nri_scene::PTMapWorld liveWorld = {};
		nri_scene::PTMapWorldStats ignoredStats = {};
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyLiveBuildMs);
			if (!nri_scene::BuildLiveMapChunkWorld(mapChunk, liveWorld, &ignoredStats))
			{
				return false;
			}

			nri_scene::BuildMapChunkSceneView(liveWorld, liveWorld.chunks[0], residentSceneView);
			const bool blindSpotReplacementNudged = replacement.blindSpot && replacement.dragged;
			if (blindSpotReplacementNudged)
			{
				NudgeBlindSpotReplacementFlats(residentSceneView);
			}
			else if (nri_ptceilingnudge)
			{
				NudgeMapCeilingSections(residentSceneView, (float)nri_ptceilingnudgedistance);
			}
		}
		{
			Clocker clock(NriPTGeometryBuild);
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryBuildMs);
			ScopedPtPerfTimer siteTimer(mLastPerfShellTraceStats.geometryBuildResidentApplyMs);
			nri_scene::BuildGeometry(residentSceneView, residentGeometry);
			AssignGeometryPortalIndices(mMapWorld, residentGeometry);
		}
		mLastPerfShellTraceStats.geometryBuildResidentApplyCalls++;
		mLastPerfShellTraceStats.geometryBuildResidentPrimitives += (uint32_t)residentGeometry.primitives.size();
		{
			Clocker clock(NriPTMaterialBuild);
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialBuildMs);
			BuildMaterialsWithActorOverrides(residentSceneView, residentMaterials, "resident_runtime_mutation_chunk");
		}
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyBaselineCaptureMs);
			if (!nri_scene::CaptureMapChunkMutationBaseline(mapChunk, appliedBaseline))
			{
				return false;
			}
		}
	}
	else
	{
		residentSceneView = replacement.sceneView;
		residentGeometry = replacement.geometry;
		residentMaterials = replacement.materialBridge;
		appliedBaseline = replacement.replacementBaseline;
	}
	outResult.surfaceCount = CountSceneViewSurfaces(residentSceneView);
	outResult.triangleCount = (uint32_t)residentGeometry.primitives.size();
	outResult.materialCount = (uint32_t)residentMaterials.materials.size();

	const uint32_t appliedReasonMask = replacement.reasonMask;
	const bool appliedStaticAnimatedReplacement = replacement.staticAnimatedReplacement;
	const bool appliedAnimationOnlyRefreshed = replacement.animationOnlyRefreshed;
	uint64_t residentGeometryTopologySignature = ComputeGeometryTopologySignature(residentGeometry);
	uint64_t residentPrimitiveLayoutSignature = ComputePrimitiveLayoutSignature(residentGeometry);
	const uint32_t materialReasonMask =
		nri_scene::PTMapChunkMutationReason_SectorMaterial |
		nri_scene::PTMapChunkMutationReason_WallMaterial;
	const bool appliedForceTopology =
		(appliedReasonMask & (
			nri_scene::PTMapChunkMutationReason_SectorGeometry |
			nri_scene::PTMapChunkMutationReason_WallGeometry |
			nri_scene::PTMapChunkMutationReason_SectorDirty |
			nri_scene::PTMapChunkMutationReason_SectionDirty |
			nri_scene::PTMapChunkMutationReason_Dragged)) != 0;
	const bool hasResidentGeometry =
		hasResidentChunk &&
		mStaticMapScene.chunks[resolvedChunkListIndex].active &&
		mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].valid;
	const uint32_t chunkListIndex = hasChunkSlot ?
		resolvedChunkListIndex :
		(uint32_t)mStaticMapScene.chunks.size();
	uint32_t residentVertexCount = 0;
	uint32_t residentIndexCount = 0;
	uint32_t residentPrimitiveCount = 0;
	uint32_t residentMaterialCount = 0;
	bool preserveResidentGeometryForMaterialOnlyUpdate = false;
	uint32_t effectiveResidentVertexCount = 0;
	uint32_t effectiveResidentIndexCount = 0;
	uint32_t effectiveResidentPrimitiveCount = 0;
	bool chunkBecomesEmpty = false;
	bool recoveredFullLiveResidentChunk = false;
	const auto recomputeResidentCounts = [&]()
	{
		residentVertexCount =
			fastResidentMaterialOnlyUpdate && !recoveredFullLiveResidentChunk && hasResidentChunk ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].vertexCount :
			(uint32_t)residentGeometry.vertices.size();
		residentIndexCount =
			fastResidentMaterialOnlyUpdate && !recoveredFullLiveResidentChunk && hasResidentChunk ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].indexCount :
			(uint32_t)residentGeometry.indices.size();
		residentPrimitiveCount =
			fastResidentMaterialOnlyUpdate && !recoveredFullLiveResidentChunk && hasResidentChunk ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].primitiveCount :
			(uint32_t)residentGeometry.primitives.size();
		residentMaterialCount = (uint32_t)residentMaterials.materials.size();
		preserveResidentGeometryForMaterialOnlyUpdate =
			materialOnlyReplacement &&
			!exclusiveMaterialOnlyReplacement &&
			hasResidentChunk &&
			residentPrimitiveCount == 0 &&
			residentMaterialCount != 0 &&
			resolvedChunkListIndex < mStaticMapChunkAtlas.chunks.size() &&
			residentMaterialCount == mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].materialCount;
		effectiveResidentVertexCount =
			preserveResidentGeometryForMaterialOnlyUpdate ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].vertexCount :
			residentVertexCount;
		effectiveResidentIndexCount =
			preserveResidentGeometryForMaterialOnlyUpdate ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].indexCount :
			residentIndexCount;
		effectiveResidentPrimitiveCount =
			preserveResidentGeometryForMaterialOnlyUpdate ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].primitiveCount :
			residentPrimitiveCount;
		chunkBecomesEmpty = effectiveResidentPrimitiveCount == 0 || residentMaterialCount == 0;
	};
	recomputeResidentCounts();
	if (preserveResidentGeometryForMaterialOnlyUpdate)
	{
		mLastPerfShellTraceStats.runtimeMutationResidentApplyPreserveGeometryCount++;
	}
	const uint32_t residentChunkRemovalReasonMask =
		nri_scene::PTMapChunkMutationReason_SectorGeometry |
		nri_scene::PTMapChunkMutationReason_WallGeometry |
		nri_scene::PTMapChunkMutationReason_SectorDirty |
		nri_scene::PTMapChunkMutationReason_SectionDirty |
		nri_scene::PTMapChunkMutationReason_Dragged;
	const auto rebuildFullLiveResidentChunk = [&]() -> bool
	{
		nri_scene::PTMapWorld liveWorld = {};
		nri_scene::PTMapWorldStats ignoredStats = {};
		nri_scene::SceneView fullLiveSceneView;
		nri_scene::GeometryData fullLiveGeometry;
		nri_scene::MaterialBridgeData fullLiveMaterials;
		nri_scene::PTMapChunkMutationBaseline fullLiveBaseline;
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyLiveBuildMs);
			if (!nri_scene::BuildLiveMapChunkWorld(mapChunk, liveWorld, &ignoredStats))
			{
				return false;
			}

			nri_scene::BuildMapChunkSceneView(liveWorld, liveWorld.chunks[0], fullLiveSceneView);
			if (nri_ptceilingnudge)
			{
				NudgeMapCeilingSections(fullLiveSceneView, (float)nri_ptceilingnudgedistance);
			}
		}
		{
			Clocker clock(NriPTGeometryBuild);
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryBuildMs);
			ScopedPtPerfTimer siteTimer(mLastPerfShellTraceStats.geometryBuildResidentRecoverMs);
			nri_scene::BuildGeometry(fullLiveSceneView, fullLiveGeometry);
			AssignGeometryPortalIndices(mMapWorld, fullLiveGeometry);
		}
		mLastPerfShellTraceStats.geometryBuildResidentRecoverCalls++;
		mLastPerfShellTraceStats.geometryBuildResidentPrimitives += (uint32_t)fullLiveGeometry.primitives.size();
		{
			Clocker clock(NriPTMaterialBuild);
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialBuildMs);
			BuildMaterialsWithActorOverrides(fullLiveSceneView, fullLiveMaterials, "resident_runtime_mutation_chunk_recover");
		}
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyBaselineCaptureMs);
			if (!nri_scene::CaptureMapChunkMutationBaseline(mapChunk, fullLiveBaseline))
			{
				return false;
			}
		}

		if (fullLiveGeometry.primitives.empty() || fullLiveMaterials.materials.empty())
		{
			return false;
		}

		if (ShouldTracePtPerf())
		{
			Printf("NRI PT resident chunk trace: event=recover-empty chunk=%u reason_mask=0x%x recovered_prims=%u recovered_mats=%u\n",
				mapChunk.chunkIndex,
				appliedReasonMask,
				(uint32_t)fullLiveGeometry.primitives.size(),
				(uint32_t)fullLiveMaterials.materials.size());
		}

		residentSceneView = std::move(fullLiveSceneView);
		residentGeometry = std::move(fullLiveGeometry);
		residentMaterials = std::move(fullLiveMaterials);
		appliedBaseline = std::move(fullLiveBaseline);
		recoveredFullLiveResidentChunk = true;
		outResult.recoveredEmpty = true;
		mLastPerfShellTraceStats.runtimeMutationResidentApplyRecoverSuccessCount++;
		return true;
	};
	if (chunkBecomesEmpty)
	{
		mLastPerfShellTraceStats.runtimeMutationResidentApplyRecoverAttemptCount++;
		if (rebuildFullLiveResidentChunk())
		{
			residentGeometryTopologySignature = ComputeGeometryTopologySignature(residentGeometry);
			residentPrimitiveLayoutSignature = ComputePrimitiveLayoutSignature(residentGeometry);
			recomputeResidentCounts();
			if (preserveResidentGeometryForMaterialOnlyUpdate)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyPreserveGeometryCount++;
			}
		}
	}
	const bool suspiciousNonStructuralChunkRemoval =
		chunkBecomesEmpty &&
		hasResidentChunk &&
		(appliedReasonMask & residentChunkRemovalReasonMask) == 0;
	if (suspiciousNonStructuralChunkRemoval)
	{
		if (ShouldTracePtPerf())
		{
			Printf("NRI PT resident chunk trace: event=reject-empty-nonstructural chunk=%u reason_mask=0x%x resident_prims=%u resident_mats=%u\n",
				mapChunk.chunkIndex,
				appliedReasonMask,
				residentPrimitiveCount,
				residentMaterialCount);
		}
		return false;
	}
	const uint64_t residentMaterialPayloadHash =
		!chunkBecomesEmpty ? HashResidentMaterialPayload(residentMaterials) : 0;
	uint64_t residentGeometryPayloadHash = 0;
	bool residentMaterialPayloadHashSkip = false;
	bool residentGeometryPayloadHashSkip = false;
	bool appliedPreservedResidentMaterialSlice = false;
	bool appliedPreservedResidentGeometryPayload = false;

	if (chunkBecomesEmpty)
	{
		mLastPerfShellTraceStats.runtimeMutationResidentApplyEmptyRemovalCount++;
		if (hasResidentChunk)
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyAtlasMs);
			auto& mutableChunk = mStaticMapScene.chunks[chunkListIndex];
			const auto& oldAtlasChunk = mStaticMapChunkAtlas.chunks[chunkListIndex];
			const uint32_t oldMaterialOffset = oldAtlasChunk.materialOffset;
			const uint32_t oldMaterialCount = oldAtlasChunk.materialCount;
			RetireResidentAccelerationStructure(mutableChunk.accelerationStructure);
			ReleaseChunkAtlasRange(mStaticMapChunkAtlas.freeVertexRanges, oldAtlasChunk.vertexOffset, oldAtlasChunk.vertexCount);
			ReleaseChunkAtlasRange(mStaticMapChunkAtlas.freeIndexRanges, oldAtlasChunk.indexOffset, oldAtlasChunk.indexCount);
			ReleaseChunkAtlasRange(mStaticMapChunkAtlas.freePrimitiveRanges, oldAtlasChunk.primitiveOffset, oldAtlasChunk.primitiveCount);
			ReleaseChunkAtlasRange(mStaticMapChunkAtlas.freeMaterialRanges, oldAtlasChunk.materialOffset, oldAtlasChunk.materialCount);
			mStaticMapChunkAtlas.chunks[chunkListIndex] = {};
			mStaticMapChunkAtlas.chunks[chunkListIndex].chunkIndex = mapChunk.chunkIndex;
			mStaticMapChunkAtlas.chunks[chunkListIndex].staticSceneChunkListIndex = chunkListIndex;
			mutableChunk.active = false;
			mutableChunk.blasUpdateEligible = false;
			mutableChunk.lastResidentBlasReasonMask = 0;
			mutableChunk.lastResidentBlasSurfaceCount = 0;
			mutableChunk.lastResidentBlasTriangleCount = 0;
			mutableChunk.lastResidentBlasMaterialCount = 0;
			mutableChunk.lastResidentBlasForceTopology = false;
			mutableChunk.lastResidentBlasRecoveredEmpty = false;
			mutableChunk.lastResidentBlasKeptGeometrySlice = false;
			mutableChunk.lastResidentBlasTopologyChanged = false;
			mutableChunk.residentBlasScratchSizeCacheKey = nullptr;
			mutableChunk.residentBlasBuildScratchSize = 0;
			mutableChunk.residentBlasUpdateScratchSize = 0;
			mutableChunk.vertexCount = 0;
			mutableChunk.indexCount = 0;
			mutableChunk.primitiveCount = 0;
			mutableChunk.materialCount = 0;
			mutableChunk.materialBridge = {};
			mutableChunk.geometryTopologySignature = 0;
			mutableChunk.primitiveLayoutSignature = 0;
			mutableChunk.exactGeometrySignature = 0;
			mutableChunk.geometryPayloadHash = 0;
			mutableChunk.animatedMaterialSignature = 0;
			mutableChunk.animatedGeometrySignature = 0;
			mutableChunk.hasAnimatedTextureCandidates = false;
			mutableChunk.animatedRefreshSuppressed = false;
			if (chunkListIndex < mStaticMapScene.lightChunkViews.size())
			{
				mStaticMapScene.lightChunkViews[chunkListIndex] = {};
			}
			if (oldMaterialOffset + oldMaterialCount <= mStaticMapScene.materialBridge.materials.size())
			{
				std::fill_n(
					mStaticMapScene.materialBridge.materials.begin() + oldMaterialOffset,
					oldMaterialCount,
					nri_scene::MaterialData{});
			}
			if (oldMaterialOffset + oldMaterialCount <= mStaticMapScene.materialBridge.lightMetadata.size())
			{
				std::fill_n(
					mStaticMapScene.materialBridge.lightMetadata.begin() + oldMaterialOffset,
					oldMaterialCount,
					nri_scene::MaterialLightingMetadata{});
			}
			if (oldMaterialOffset + oldMaterialCount <= mStaticMapScene.gpuMaterials.size())
			{
				std::fill_n(
					mStaticMapScene.gpuMaterials.begin() + oldMaterialOffset,
					oldMaterialCount,
					nri_scene::MaterialData{});
			}
			if (oldMaterialCount > 0)
			{
				std::vector<nri_scene::MaterialData> clearedMaterials(oldMaterialCount);
				if (!StageResidentBufferCopyRange(
						mStaticMaterialBuffer,
						(uint64_t)oldMaterialOffset * sizeof(nri_scene::MaterialData),
						clearedMaterials.data(),
						(uint64_t)oldMaterialCount * sizeof(nri_scene::MaterialData),
						NRIComputeShaderResourceAccess(),
						ResidentUploadKind_Material))
				{
					return false;
				}
				mLastPerfResourceTraceStats.residentChunkBatchMaterialBytes +=
					(uint64_t)oldMaterialCount * sizeof(nri_scene::MaterialData);
			}
			outResult.geometryDirty = true;
			outResult.materialDirty = true;
			outResult.staticSceneChunkListIndex = chunkListIndex;
		}
	}
	else
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyAtlasMs);
		StaticMapSceneCache::ChunkCache sourceChunk = {};
		StaticMapChunkAtlas nextAtlasState = {};
		StaticMapChunkAtlas::ChunkEntry nextAtlasChunk = {};
		bool keptGeometrySlices = false;
		bool keptMaterialSlice = false;
		const uint64_t previousAnimatedMaterialSignature =
			hasResidentChunk ?
			mStaticMapScene.chunks[chunkListIndex].animatedMaterialSignature :
			0;
		const bool previousAnimatedRefreshSuppressed =
			hasResidentChunk &&
			mStaticMapScene.chunks[chunkListIndex].animatedRefreshSuppressed;
		const uint64_t previousAnimatedGeometrySignature =
			hasResidentChunk ?
				mStaticMapScene.chunks[chunkListIndex].animatedGeometrySignature :
				0;
		const uint64_t previousGeometryTopologySignature =
			hasResidentChunk ?
				mStaticMapScene.chunks[chunkListIndex].geometryTopologySignature :
				0;
		const uint64_t previousPrimitiveLayoutSignature =
			hasResidentChunk ?
				mStaticMapScene.chunks[chunkListIndex].primitiveLayoutSignature :
				0;
		bool preserveResidentMaterialSlice = false;
		bool preserveResidentIndexSlice = false;
		bool preserveResidentPrimitiveSlice = false;
		bool preserveResidentBlasRefitOnly = false;
		{
			ScopedPtPerfTimer detailPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyAtlasBookkeepingMs);
			sourceChunk.vertexOffset = 0;
			sourceChunk.vertexCount = effectiveResidentVertexCount;
			sourceChunk.indexOffset = 0;
			sourceChunk.indexCount = effectiveResidentIndexCount;
			sourceChunk.primitiveOffset = 0;
			sourceChunk.primitiveCount = effectiveResidentPrimitiveCount;
			sourceChunk.materialOffset = 0;
			sourceChunk.materialCount = (uint32_t)residentMaterials.materials.size();

			for (;;)
			{
				nextAtlasState = mStaticMapChunkAtlas;
				if (chunkListIndex >= nextAtlasState.chunks.size())
				{
					nextAtlasState.chunks.resize(chunkListIndex + 1);
				}
				nextAtlasState.chunkCount = (uint32_t)nextAtlasState.chunks.size();
				const auto oldAtlasChunk = hasResidentChunk ? nextAtlasState.chunks[chunkListIndex] : StaticMapChunkAtlas::ChunkEntry{};
				const bool keepGeometrySlices =
					hasResidentGeometry &&
					oldAtlasChunk.vertexCount == effectiveResidentVertexCount &&
					oldAtlasChunk.indexCount == effectiveResidentIndexCount &&
					oldAtlasChunk.primitiveCount == effectiveResidentPrimitiveCount;
				const bool keepMaterialSlice =
					hasResidentGeometry &&
					oldAtlasChunk.materialCount == residentMaterialCount;

				if (hasResidentGeometry && !keepGeometrySlices)
				{
					ReleaseChunkAtlasRange(nextAtlasState.freeVertexRanges, oldAtlasChunk.vertexOffset, oldAtlasChunk.vertexCount);
					ReleaseChunkAtlasRange(nextAtlasState.freeIndexRanges, oldAtlasChunk.indexOffset, oldAtlasChunk.indexCount);
					ReleaseChunkAtlasRange(nextAtlasState.freePrimitiveRanges, oldAtlasChunk.primitiveOffset, oldAtlasChunk.primitiveCount);
				}
				if (hasResidentGeometry && !keepMaterialSlice)
				{
					ReleaseChunkAtlasRange(nextAtlasState.freeMaterialRanges, oldAtlasChunk.materialOffset, oldAtlasChunk.materialCount);
				}

				nextAtlasChunk = {};
				nextAtlasChunk.valid = true;
				nextAtlasChunk.chunkIndex = mapChunk.chunkIndex;
				nextAtlasChunk.staticSceneChunkListIndex = chunkListIndex;
				nextAtlasChunk.vertexCount = effectiveResidentVertexCount;
				nextAtlasChunk.indexCount = effectiveResidentIndexCount;
				nextAtlasChunk.primitiveCount = effectiveResidentPrimitiveCount;
				nextAtlasChunk.materialCount = residentMaterialCount;
				nextAtlasChunk.vertexOffset = keepGeometrySlices ?
					oldAtlasChunk.vertexOffset :
					AllocateChunkAtlasRange(effectiveResidentVertexCount, nextAtlasState.vertexCapacity, nextAtlasState.freeVertexRanges, nextAtlasState.vertexCount);
				nextAtlasChunk.indexOffset = keepGeometrySlices ?
					oldAtlasChunk.indexOffset :
					AllocateChunkAtlasRange(effectiveResidentIndexCount, nextAtlasState.indexCapacity, nextAtlasState.freeIndexRanges, nextAtlasState.indexCount);
				nextAtlasChunk.primitiveOffset = keepGeometrySlices ?
					oldAtlasChunk.primitiveOffset :
					AllocateChunkAtlasRange(effectiveResidentPrimitiveCount, nextAtlasState.primitiveCapacity, nextAtlasState.freePrimitiveRanges, nextAtlasState.primitiveCount);
				nextAtlasChunk.materialOffset = keepMaterialSlice ?
					oldAtlasChunk.materialOffset :
					AllocateChunkAtlasRange(residentMaterialCount, nextAtlasState.materialCapacity, nextAtlasState.freeMaterialRanges, nextAtlasState.materialCount);
				if (nextAtlasChunk.vertexOffset != UINT32_MAX &&
					nextAtlasChunk.indexOffset != UINT32_MAX &&
					nextAtlasChunk.primitiveOffset != UINT32_MAX &&
					nextAtlasChunk.materialOffset != UINT32_MAX)
				{
					keptGeometrySlices = keepGeometrySlices;
					keptMaterialSlice = keepMaterialSlice;
					break;
				}

				if (nextAtlasChunk.vertexOffset == UINT32_MAX)
				{
					nextAtlasState.vertexCapacity = GetChunkAtlasCapacity(nextAtlasState.vertexCount + effectiveResidentVertexCount);
				}
				if (nextAtlasChunk.indexOffset == UINT32_MAX)
				{
					nextAtlasState.indexCapacity = GetChunkAtlasCapacity(nextAtlasState.indexCount + effectiveResidentIndexCount);
				}
				if (nextAtlasChunk.primitiveOffset == UINT32_MAX)
				{
					nextAtlasState.primitiveCapacity = GetChunkAtlasCapacity(nextAtlasState.primitiveCount + effectiveResidentPrimitiveCount);
				}
				if (nextAtlasChunk.materialOffset == UINT32_MAX)
				{
					nextAtlasState.materialCapacity = GetChunkAtlasCapacity(nextAtlasState.materialCount + residentMaterialCount);
				}
				mLastPerfShellTraceStats.runtimeMutationResidentApplyAtlasGrowCount++;
				if (!EnsureResidentStaticMapChunkAtlasBufferCapacity(nextAtlasState))
				{
					return false;
				}
			}
			if (keptGeometrySlices)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyKeepGeometrySliceCount++;
			}
			if (keptMaterialSlice)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyKeepMaterialSliceCount++;
			}
			if (hasResidentChunk && residentMaterialCount != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashCheckCount++;
				if (keptMaterialSlice && entry.materialPayloadHash != 0)
				{
					if (entry.materialPayloadHash == residentMaterialPayloadHash)
					{
						residentMaterialPayloadHashSkip = true;
						mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashSkipCount++;
					}
					else
					{
						mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashMissCount++;
					}
				}
				else
				{
					mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashRejectCount++;
				}
			}
			if (!fastResidentMaterialOnlyUpdate &&
				!preserveResidentGeometryForMaterialOnlyUpdate &&
				hasResidentChunk &&
				effectiveResidentVertexCount != 0 &&
				effectiveResidentIndexCount != 0 &&
				effectiveResidentPrimitiveCount != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashCheckCount++;
				if (keptGeometrySlices && keptMaterialSlice && entry.geometryPayloadHash != 0)
				{
					residentGeometryPayloadHash = HashResidentGeometryPayload(
						mMapWorld,
						residentGeometry,
						sourceChunk.vertexOffset,
						sourceChunk.vertexCount,
						sourceChunk.indexOffset,
						sourceChunk.indexCount,
						sourceChunk.primitiveOffset,
						sourceChunk.primitiveCount,
						sourceChunk.materialOffset,
						sourceChunk.materialCount);
					if (residentGeometryPayloadHash != 0 &&
						entry.geometryPayloadHash == residentGeometryPayloadHash)
					{
						residentGeometryPayloadHashSkip = true;
						mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashSkipCount++;
						mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashBlasSkipCount++;
					}
					else
					{
						mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashMissCount++;
						if (ShouldTraceResidentGeometryOrderHash())
						{
							ScopedPtPerfTimer orderHashTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryOrderHashMs);
							mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderCheckCount++;
							const uint64_t residentGeometryOrderHash = HashResidentGeometryPayloadOrderIndependent(
								mMapWorld,
								residentGeometry,
								sourceChunk.vertexOffset,
								sourceChunk.vertexCount,
								sourceChunk.primitiveOffset,
								sourceChunk.primitiveCount,
								sourceChunk.materialOffset,
								sourceChunk.materialCount);
							const uint64_t currentGeometryOrderHash = HashResidentGeometryPayloadOrderIndependent(
								mMapWorld,
								mStaticMapScene.geometry,
								nextAtlasChunk.vertexOffset,
								nextAtlasChunk.vertexCount,
								nextAtlasChunk.primitiveOffset,
								nextAtlasChunk.primitiveCount,
								nextAtlasChunk.materialOffset,
								nextAtlasChunk.materialCount);
							if (residentGeometryOrderHash != 0 && currentGeometryOrderHash != 0)
							{
								if (residentGeometryOrderHash == currentGeometryOrderHash)
								{
									mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderEquivalentCount++;
								}
								else
								{
									mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderMissCount++;
								}
							}
							else
							{
								mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderRejectCount++;
							}
						}
					}
				}
				else
				{
					mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashRejectCount++;
				}
			}

			if (chunkListIndex >= mStaticMapScene.chunks.size())
			{
				mStaticMapScene.chunks.resize(chunkListIndex + 1);
			}
			if (chunkListIndex >= mStaticMapScene.lightChunkViews.size())
			{
				mStaticMapScene.lightChunkViews.resize(chunkListIndex + 1);
			}
			preserveResidentMaterialSlice =
				!materialOnlyReplacement &&
				(appliedReasonMask & materialReasonMask) == 0 &&
				!appliedStaticAnimatedReplacement &&
				!appliedAnimationOnlyRefreshed &&
				hasResidentChunk &&
				keptMaterialSlice &&
				previousAnimatedMaterialSignature == ComputeAnimatedMaterialSignature(residentSceneView);
			preserveResidentMaterialSlice = preserveResidentMaterialSlice || residentMaterialPayloadHashSkip;
			preserveResidentIndexSlice =
				!materialOnlyReplacement &&
				hasResidentChunk &&
				keptGeometrySlices &&
				previousGeometryTopologySignature == residentGeometryTopologySignature;
			preserveResidentPrimitiveSlice =
				!materialOnlyReplacement &&
				hasResidentChunk &&
				preserveResidentIndexSlice &&
				keptMaterialSlice &&
				previousPrimitiveLayoutSignature == residentPrimitiveLayoutSignature;
			const bool probeResidentBlasRefitOnly =
				!preserveResidentIndexSlice &&
				!materialOnlyReplacement &&
				hasResidentChunk;
			if (probeResidentBlasRefitOnly)
			{
				uint32_t rejectMask = 0;
				const bool hadAccelerationStructure =
					mStaticMapScene.chunks[chunkListIndex].accelerationStructure.accelerationStructure != nullptr;
				mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitProbeCount++;
				if (!hadAccelerationStructure)
				{
					rejectMask |= RuntimeResidentBlasRefitReject_NoPreviousAs;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectNoPreviousAsCount++;
				}
				if (entry.indexCount != effectiveResidentIndexCount)
				{
					rejectMask |= RuntimeResidentBlasRefitReject_IndexCountMismatch;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectIndexCountMismatchCount++;
				}
				if (entry.primitiveCount != effectiveResidentPrimitiveCount)
				{
					rejectMask |= RuntimeResidentBlasRefitReject_PrimitiveCountMismatch;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectPrimitiveCountMismatchCount++;
				}
				if (entry.indexCount == 0 || effectiveResidentIndexCount == 0)
				{
					rejectMask |= RuntimeResidentBlasRefitReject_ZeroIndexCount;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectZeroIndexCount++;
				}
				if (entry.primitiveCount == 0 || effectiveResidentPrimitiveCount == 0)
				{
					rejectMask |= RuntimeResidentBlasRefitReject_ZeroPrimitiveCount;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectZeroPrimitiveCount++;
				}
				preserveResidentBlasRefitOnly = rejectMask == 0;
				if (rejectMask != 0 && ShouldTracePtPerf())
				{
					RuntimeResidentBlasRefitRejectTraceEntry traceEntry = {};
					traceEntry.valid = true;
					traceEntry.chunkIndex = mapChunk.chunkIndex;
					traceEntry.sectorIndex = mapChunk.sectorIndex;
					traceEntry.reasonMask = appliedReasonMask;
					traceEntry.rejectMask = rejectMask;
					traceEntry.previousIndexCount = entry.indexCount;
					traceEntry.liveIndexCount = effectiveResidentIndexCount;
					traceEntry.previousPrimitiveCount = entry.primitiveCount;
					traceEntry.livePrimitiveCount = effectiveResidentPrimitiveCount;
					traceEntry.hadAccelerationStructure = hadAccelerationStructure;
					InsertRankedTraceEntry(
						mLastPerfShellTraceStats.runtimeResidentBlasRefitRejectEntries,
						traceEntry,
						ScoreRuntimeResidentBlasRefitRejectTraceEntry);
				}
			}
			if (preserveResidentIndexSlice)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyPreserveIndexCount++;
			}
			if (preserveResidentPrimitiveSlice)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyPreservePrimitiveCount++;
			}
			if (preserveResidentBlasRefitOnly)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitOnlyCount++;
			}

			mStaticMapChunkAtlas = std::move(nextAtlasState);
			mStaticMapChunkAtlas.chunks[chunkListIndex] = nextAtlasChunk;
			mStaticMapScene.geometry.vertices.resize(std::max<size_t>(mStaticMapScene.geometry.vertices.size(), mStaticMapChunkAtlas.vertexCount));
			mStaticMapScene.geometry.indices.resize(std::max<size_t>(mStaticMapScene.geometry.indices.size(), mStaticMapChunkAtlas.indexCount));
			mStaticMapScene.geometry.primitives.resize(std::max<size_t>(mStaticMapScene.geometry.primitives.size(), mStaticMapChunkAtlas.primitiveCount));
			mStaticMapScene.geometry.primitiveProvenance.resize(std::max<size_t>(mStaticMapScene.geometry.primitiveProvenance.size(), mStaticMapChunkAtlas.primitiveCount));
			mStaticMapScene.materialBridge.materials.resize(std::max<size_t>(mStaticMapScene.materialBridge.materials.size(), mStaticMapChunkAtlas.materialCount));
			mStaticMapScene.materialBridge.lightMetadata.resize(std::max<size_t>(mStaticMapScene.materialBridge.lightMetadata.size(), mStaticMapChunkAtlas.materialCount));
			mStaticMapScene.gpuMaterials.resize(std::max<size_t>(mStaticMapScene.gpuMaterials.size(), mStaticMapChunkAtlas.materialCount));
		}

		if (!fastResidentMaterialOnlyUpdate &&
			!preserveResidentGeometryForMaterialOnlyUpdate &&
			!residentGeometryPayloadHashSkip)
		{
			{
				ScopedPtPerfTimer detailPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexIndexCopyMs);
				if (preserveResidentIndexSlice)
				{
					{
						ScopedPtPerfTimer vertexCpuTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexCpuCopyMs);
						UploadChunkVertexDataToAtlas(
							residentGeometry,
							sourceChunk,
							nextAtlasChunk,
							mStaticMapScene.geometry.vertices);
					}
					const uint64_t vertexBytes = (uint64_t)nextAtlasChunk.vertexCount * sizeof(nri_scene::SceneVertex);
					if (!mRuntimeMutation.QueueResidentGeometryUploadRange(
							ResidentUploadKind_Vertex,
							(uint64_t)nextAtlasChunk.vertexOffset * sizeof(nri_scene::SceneVertex),
							vertexBytes,
							BuildRuntimeMutationResidentUploadServices()))
					{
						return false;
					}
				}
				else
				{
					{
						ScopedPtPerfTimer vertexCpuTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexCpuCopyMs);
						UploadChunkVertexDataToAtlas(
							residentGeometry,
							sourceChunk,
							nextAtlasChunk,
							mStaticMapScene.geometry.vertices);
					}
					{
						ScopedPtPerfTimer indexCpuTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyIndexCpuCopyMs);
						UploadChunkIndexDataToAtlas(
							residentGeometry,
							sourceChunk,
							nextAtlasChunk,
							mStaticMapScene.geometry.indices);
					}

					const uint64_t vertexBytes = (uint64_t)nextAtlasChunk.vertexCount * sizeof(nri_scene::SceneVertex);
					const uint64_t indexBytes = (uint64_t)nextAtlasChunk.indexCount * sizeof(uint32_t);
					if (!mRuntimeMutation.QueueResidentGeometryUploadRange(
							ResidentUploadKind_Vertex,
							(uint64_t)nextAtlasChunk.vertexOffset * sizeof(nri_scene::SceneVertex),
							vertexBytes,
							BuildRuntimeMutationResidentUploadServices()))
					{
						return false;
					}

					if (!mRuntimeMutation.QueueResidentGeometryUploadRange(
							ResidentUploadKind_Index,
							(uint64_t)nextAtlasChunk.indexOffset * sizeof(uint32_t),
							indexBytes,
							BuildRuntimeMutationResidentUploadServices()))
					{
						return false;
					}
				}
			}
			{
				ScopedPtPerfTimer detailPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveRewriteMs);
				if (!preserveResidentPrimitiveSlice)
				{
					{
						ScopedPtPerfTimer primitiveCpuTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveCpuRewriteMs);
						UploadChunkPrimitiveDataToAtlas(
							residentGeometry,
							sourceChunk,
							nextAtlasChunk,
							mStaticMapScene.geometry.primitives);
						if (residentGeometry.primitiveProvenance.size() >= nextAtlasChunk.primitiveCount &&
							nextAtlasChunk.primitiveOffset + nextAtlasChunk.primitiveCount <= mStaticMapScene.geometry.primitiveProvenance.size())
						{
							std::copy_n(
								residentGeometry.primitiveProvenance.data(),
								nextAtlasChunk.primitiveCount,
								mStaticMapScene.geometry.primitiveProvenance.data() + nextAtlasChunk.primitiveOffset);
						}
					}
					const uint64_t primitiveBytes = (uint64_t)nextAtlasChunk.primitiveCount * sizeof(nri_scene::PrimitiveData);
					if (!mRuntimeMutation.QueueResidentGeometryUploadRange(
							ResidentUploadKind_Primitive,
							(uint64_t)nextAtlasChunk.primitiveOffset * sizeof(nri_scene::PrimitiveData),
							primitiveBytes,
							BuildRuntimeMutationResidentUploadServices()))
					{
						return false;
					}
				}
			}
			mLastPerfResourceTraceStats.residentChunkBatchVertexBytes +=
				(uint64_t)nextAtlasChunk.vertexCount * sizeof(nri_scene::SceneVertex);
			if (!preserveResidentIndexSlice)
			{
				mLastPerfResourceTraceStats.residentChunkBatchIndexBytes +=
					(uint64_t)nextAtlasChunk.indexCount * sizeof(uint32_t);
			}
			if (!preserveResidentPrimitiveSlice)
			{
				mLastPerfResourceTraceStats.residentChunkBatchPrimitiveBytes +=
					(uint64_t)nextAtlasChunk.primitiveCount * sizeof(nri_scene::PrimitiveData);
			}
		}

		auto& mutableChunk = mStaticMapScene.chunks[chunkListIndex];
		mutableChunk.chunkIndex = mapChunk.chunkIndex;
		mutableChunk.vertexOffset = nextAtlasChunk.vertexOffset;
		mutableChunk.vertexCount = nextAtlasChunk.vertexCount;
		mutableChunk.indexOffset = nextAtlasChunk.indexOffset;
		mutableChunk.indexCount = nextAtlasChunk.indexCount;
		mutableChunk.primitiveOffset = nextAtlasChunk.primitiveOffset;
		mutableChunk.primitiveCount = nextAtlasChunk.primitiveCount;
		mutableChunk.materialOffset = nextAtlasChunk.materialOffset;
		mutableChunk.materialCount = nextAtlasChunk.materialCount;
		mutableChunk.active = true;
		mutableChunk.blasUpdateEligible = preserveResidentIndexSlice || preserveResidentBlasRefitOnly;
		mutableChunk.lastResidentBlasReasonMask = appliedReasonMask;
		mutableChunk.lastResidentBlasSurfaceCount = outResult.surfaceCount;
		mutableChunk.lastResidentBlasTriangleCount = outResult.triangleCount;
		mutableChunk.lastResidentBlasMaterialCount = outResult.materialCount;
		mutableChunk.lastResidentBlasForceTopology = appliedForceTopology;
		mutableChunk.lastResidentBlasRecoveredEmpty = outResult.recoveredEmpty;
		mutableChunk.lastResidentBlasKeptGeometrySlice = keptGeometrySlices;
		mutableChunk.lastResidentBlasTopologyChanged =
			hasResidentChunk &&
			previousGeometryTopologySignature != residentGeometryTopologySignature;
		if (!preserveResidentMaterialSlice)
		{
			mutableChunk.materialBridge = residentMaterials;
		}
		mutableChunk.geometryTopologySignature = residentGeometryTopologySignature;
		mutableChunk.primitiveLayoutSignature = residentPrimitiveLayoutSignature;
		mutableChunk.exactGeometrySignature = ComputeExactGeometrySignature(residentSceneView);
		if (!residentGeometryPayloadHashSkip &&
			!fastResidentMaterialOnlyUpdate &&
			!preserveResidentGeometryForMaterialOnlyUpdate)
		{
			if (residentGeometryPayloadHash == 0)
			{
				residentGeometryPayloadHash = HashResidentGeometryPayload(
					mMapWorld,
					residentGeometry,
					sourceChunk.vertexOffset,
					sourceChunk.vertexCount,
					sourceChunk.indexOffset,
					sourceChunk.indexCount,
					sourceChunk.primitiveOffset,
					sourceChunk.primitiveCount,
					sourceChunk.materialOffset,
					sourceChunk.materialCount);
			}
			mutableChunk.geometryPayloadHash = residentGeometryPayloadHash;
		}
		mutableChunk.animatedMaterialSignature = ComputeAnimatedMaterialSignature(residentSceneView);
		mutableChunk.animatedGeometrySignature = ComputeAnimatedGeometrySignature(residentSceneView);
		mutableChunk.hasAnimatedTextureCandidates = ChunkHasAnimatedStaticMapSurfaceCandidates(mMapWorld, mapChunk);
		mutableChunk.animatedRefreshSuppressed =
			previousAnimatedRefreshSuppressed &&
			(appliedStaticAnimatedReplacement ||
			 appliedAnimationOnlyRefreshed ||
			 mutableChunk.animatedGeometrySignature == previousAnimatedGeometrySignature);
		mStaticMapScene.lightChunkViews[chunkListIndex] = residentSceneView;
		outResult.staticSceneChunkListIndex = chunkListIndex;
		outResult.materialDirty = !preserveResidentMaterialSlice;
		appliedPreservedResidentMaterialSlice = preserveResidentMaterialSlice;
		appliedPreservedResidentGeometryPayload =
			residentGeometryPayloadHashSkip ||
			fastResidentMaterialOnlyUpdate ||
			preserveResidentGeometryForMaterialOnlyUpdate;
		outResult.geometryDirty =
			!residentGeometryPayloadHashSkip &&
			((!preserveResidentGeometryForMaterialOnlyUpdate && !keptGeometrySlices) ||
			(appliedReasonMask & (
				nri_scene::PTMapChunkMutationReason_SectorGeometry |
				nri_scene::PTMapChunkMutationReason_WallGeometry |
				nri_scene::PTMapChunkMutationReason_SectorDirty |
				nri_scene::PTMapChunkMutationReason_SectionDirty)) != 0);
	}

	replacement.baseline = appliedBaseline;
	replacement.replacementBaseline = appliedBaseline;
	replacement.baselineSignature = appliedBaseline.signature;
	replacement.liveSignature = appliedBaseline.signature;
	replacement.reasonMask = nri_scene::PTMapChunkMutationReason_None;
	replacement.sectionDirtyCount = 0;
	replacement.stableMutationFrameCount = 0;
	replacement.sectorDirty = false;
	replacement.dragged = false;
	replacement.blindSpot = false;
	replacement.excludeStaticChunk = false;
	replacement.staticAnimatedReplacement = false;
	replacement.active = false;
	replacement.valid = false;
	replacement.residentAuthoritative = true;
	replacement.animationOnlyRefreshed = false;
	replacement.animatedMaterialSignature = ComputeAnimatedMaterialSignature(residentSceneView);
	replacement.surfaceCount = 0;
	replacement.triangleCount = 0;
	mRuntimeMutation.ClearReplacementPayload(replacement, true);

	mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].appliedBaseline = appliedBaseline;
	mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].baselineSignature = appliedBaseline.signature;
	mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].liveSignature = appliedBaseline.signature;
	mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].staticSceneChunkListIndex = chunkListIndex;
	mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].active = !chunkBecomesEmpty;
	mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].mappedInStaticScene = !chunkBecomesEmpty;
	mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].exactGeometrySignature =
		!chunkBecomesEmpty ? mStaticMapScene.chunks[chunkListIndex].exactGeometrySignature : 0;
	mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].geometryTopologySignature =
		!chunkBecomesEmpty ? mStaticMapScene.chunks[chunkListIndex].geometryTopologySignature : 0;
	if (!chunkBecomesEmpty)
	{
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].vertexOffset = mStaticMapChunkAtlas.chunks[chunkListIndex].vertexOffset;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].vertexCount = mStaticMapChunkAtlas.chunks[chunkListIndex].vertexCount;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].indexOffset = mStaticMapChunkAtlas.chunks[chunkListIndex].indexOffset;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].indexCount = mStaticMapChunkAtlas.chunks[chunkListIndex].indexCount;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].primitiveOffset = mStaticMapChunkAtlas.chunks[chunkListIndex].primitiveOffset;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].primitiveCount = mStaticMapChunkAtlas.chunks[chunkListIndex].primitiveCount;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].materialOffset = mStaticMapChunkAtlas.chunks[chunkListIndex].materialOffset;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].materialCount = mStaticMapChunkAtlas.chunks[chunkListIndex].materialCount;
		if (!appliedPreservedResidentMaterialSlice)
		{
			mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].materialPayloadHash = residentMaterialPayloadHash;
		}
		if (!appliedPreservedResidentGeometryPayload)
		{
			mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].geometryPayloadHash = residentGeometryPayloadHash;
		}
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].accelerationResident =
			mStaticMapScene.chunks[chunkListIndex].accelerationStructure.accelerationStructure != nullptr;
	}
	else
	{
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].vertexOffset = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].vertexCount = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].indexOffset = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].indexCount = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].primitiveOffset = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].primitiveCount = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].materialOffset = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].materialCount = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].materialPayloadHash = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].geometryPayloadHash = 0;
		mResidentMapChunkRegistry.entries[mapChunk.chunkIndex].accelerationResident = false;
	}
	if (!outResult.materialDirty && !residentMaterialPayloadHashSkip)
	{
		outResult.materialDirty =
			(appliedReasonMask & (nri_scene::PTMapChunkMutationReason_SectorMaterial | nri_scene::PTMapChunkMutationReason_WallMaterial)) != 0 ||
			appliedStaticAnimatedReplacement ||
			appliedAnimationOnlyRefreshed ||
			materialOnlyReplacement;
	}
	if (!outResult.geometryDirty && !residentGeometryPayloadHashSkip)
	{
		outResult.geometryDirty =
			(appliedReasonMask & (
				nri_scene::PTMapChunkMutationReason_SectorGeometry |
				nri_scene::PTMapChunkMutationReason_WallGeometry |
				nri_scene::PTMapChunkMutationReason_SectorDirty |
				nri_scene::PTMapChunkMutationReason_SectionDirty)) != 0;
	}
	return true;
}
