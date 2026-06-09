#include "nri_scene_stats.h"

#include <cstring>

namespace nri_scene
{
namespace
{
	template<size_t Count>
	void AppendVoxelDuplicateTraceEntries(
		std::array<VoxelDuplicateVariantTraceEntry, Count>& targetEntries,
		unsigned int& targetCount,
		const std::array<VoxelDuplicateVariantTraceEntry, Count>& sourceEntries,
		unsigned int sourceCount)
	{
		for (unsigned int i = 0; i < sourceCount && i < Count && targetCount < Count; ++i)
		{
			if (!sourceEntries[i].valid)
			{
				continue;
			}
			targetEntries[targetCount++] = sourceEntries[i];
		}
	}

	template<size_t Count>
	void AppendDynamicVoxelEscapeTraceEntries(
		std::array<DynamicVoxelEscapeTraceEntry, Count>& targetEntries,
		unsigned int& targetCount,
		const std::array<DynamicVoxelEscapeTraceEntry, Count>& sourceEntries,
		unsigned int sourceCount)
	{
		for (unsigned int i = 0; i < sourceCount && i < Count && targetCount < Count; ++i)
		{
			if (!sourceEntries[i].valid)
			{
				continue;
			}
			targetEntries[targetCount++] = sourceEntries[i];
		}
	}
}

void AccumulateSceneDebugStats(SceneDebugStats& target, const SceneDebugStats& source)
{
	target.totalDrawItems += source.totalDrawItems;
	target.wallDrawItems += source.wallDrawItems;
	target.flatDrawItems += source.flatDrawItems;
	target.spriteDrawItems += source.spriteDrawItems;
	target.translucentDrawItems += source.translucentDrawItems;
	target.triangleEstimate += source.triangleEstimate;
	target.materialRefs += source.materialRefs;
	target.mirrorSurfaces += source.mirrorSurfaces;
	target.skySurfaces += source.skySurfaces;
	target.portalViews += source.portalViews;
	target.portalCapturesSkipped += source.portalCapturesSkipped;
	target.modelDrawItems += source.modelDrawItems;
	target.voxelProxyDrawItems += source.voxelProxyDrawItems;
	target.unsupportedModelDrawItems += source.unsupportedModelDrawItems;
	target.voxelStableCandidates += source.voxelStableCandidates;
	target.voxelStableUncacheable += source.voxelStableUncacheable;
	target.voxelStableSignatureHits += source.voxelStableSignatureHits;
	target.voxelStableSignatureMisses += source.voxelStableSignatureMisses;
	target.voxelStableSignatureChanges += source.voxelStableSignatureChanges;
	target.voxelStableSplitStable += source.voxelStableSplitStable;
	target.voxelStableSplitLive += source.voxelStableSplitLive;
	target.voxelCacheEntries += source.voxelCacheEntries;
	target.voxelCacheSurfaceHits += source.voxelCacheSurfaceHits;
	target.voxelCacheSurfaceStores += source.voxelCacheSurfaceStores;
	target.voxelCacheSurfaceRebuilds += source.voxelCacheSurfaceRebuilds;
	target.voxelCacheTransformRebakes += source.voxelCacheTransformRebakes;
	target.voxelCacheSurfaceRemoves += source.voxelCacheSurfaceRemoves;
	target.voxelCacheNotCaptured += source.voxelCacheNotCaptured;
	target.voxelCacheDeferred += source.voxelCacheDeferred;
	target.voxelCachePrimitives += source.voxelCachePrimitives;
	target.voxelCacheActorSurfaces += source.voxelCacheActorSurfaces;
	target.voxelCacheUniqueMeshKeys += source.voxelCacheUniqueMeshKeys;
	target.voxelCacheUniqueMaterialKeys += source.voxelCacheUniqueMaterialKeys;
	target.voxelCacheLocalSpaceSurfaces += source.voxelCacheLocalSpaceSurfaces;
	target.voxelCacheBakedTransformSurfaces += source.voxelCacheBakedTransformSurfaces;
	target.voxelCacheUnknownSpaceSurfaces += source.voxelCacheUnknownSpaceSurfaces;
	target.voxelCacheTransformKeyedSurfaces += source.voxelCacheTransformKeyedSurfaces;
	target.voxelCacheUniqueTransformBases += source.voxelCacheUniqueTransformBases;
	target.voxelCacheInvariantWarnings += source.voxelCacheInvariantWarnings;
	target.voxelCacheDuplicatedVertexBytes += source.voxelCacheDuplicatedVertexBytes;
	target.voxelCacheDuplicatedIndexBytes += source.voxelCacheDuplicatedIndexBytes;
	target.voxelCacheDuplicatedPrimitiveBytes += source.voxelCacheDuplicatedPrimitiveBytes;
	target.voxelCacheDuplicatedTotalBytes += source.voxelCacheDuplicatedTotalBytes;
	AppendVoxelDuplicateTraceEntries(target.voxelCacheDuplicateTopEntries, target.voxelCacheDuplicateTopCount, source.voxelCacheDuplicateTopEntries, source.voxelCacheDuplicateTopCount);

	target.dynamicVoxelEscapeActorCount += source.dynamicVoxelEscapeActorCount;
	target.dynamicVoxelEscapeEligibleActorCount += source.dynamicVoxelEscapeEligibleActorCount;
	target.dynamicVoxelEscapeForcedActorCount += source.dynamicVoxelEscapeForcedActorCount;
	target.dynamicVoxelEscapePrimitiveCount += source.dynamicVoxelEscapePrimitiveCount;
	target.dynamicVoxelEscapeVertexBytes += source.dynamicVoxelEscapeVertexBytes;
	target.dynamicVoxelEscapeIndexBytes += source.dynamicVoxelEscapeIndexBytes;
	target.dynamicVoxelEscapePrimitiveBytes += source.dynamicVoxelEscapePrimitiveBytes;
	target.dynamicVoxelEscapeMaterialBytes += source.dynamicVoxelEscapeMaterialBytes;
	target.dynamicVoxelEscapeTotalBytes += source.dynamicVoxelEscapeTotalBytes;
	target.dynamicVoxelExpectedEscapeActorCount += source.dynamicVoxelExpectedEscapeActorCount;
	target.dynamicVoxelUnexpectedEscapeActorCount += source.dynamicVoxelUnexpectedEscapeActorCount;
	target.dynamicVoxelExpectedEscapePrimitiveCount += source.dynamicVoxelExpectedEscapePrimitiveCount;
	target.dynamicVoxelUnexpectedEscapePrimitiveCount += source.dynamicVoxelUnexpectedEscapePrimitiveCount;
	target.dynamicVoxelExpectedEscapeTotalBytes += source.dynamicVoxelExpectedEscapeTotalBytes;
	target.dynamicVoxelUnexpectedEscapeTotalBytes += source.dynamicVoxelUnexpectedEscapeTotalBytes;
	AppendDynamicVoxelEscapeTraceEntries(target.dynamicVoxelEscapeTopEntries, target.dynamicVoxelEscapeTopCount, source.dynamicVoxelEscapeTopEntries, source.dynamicVoxelEscapeTopCount);
	AppendDynamicVoxelEscapeTraceEntries(target.dynamicVoxelUnexpectedEscapeTopEntries, target.dynamicVoxelUnexpectedEscapeTopCount, source.dynamicVoxelUnexpectedEscapeTopEntries, source.dynamicVoxelUnexpectedEscapeTopCount);
}

SceneDebugStats MergeSceneDebugStats(const SceneDebugStats& a, const SceneDebugStats& b)
{
	SceneDebugStats merged = {};
	AccumulateSceneDebugStats(merged, a);
	AccumulateSceneDebugStats(merged, b);
	return merged;
}

bool SceneDebugStatsDiffer(const SceneDebugStats& a, const SceneDebugStats& b)
{
	return std::memcmp(&a, &b, sizeof(a)) != 0;
}
}
