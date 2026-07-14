#pragma once

#include <cstdint>
#include <unordered_map>

namespace nri_scene
{
struct PTMapWorld;
}

struct ResidentMapChunkRegistry;
struct StaticMapChunkAtlas;
struct StaticMapSceneCache;

struct NRIStaticSceneDiagnosticsInput
{
	const nri_scene::PTMapWorld* mapWorld = nullptr;
	const StaticMapSceneCache* staticScene = nullptr;
	const StaticMapChunkAtlas* atlas = nullptr;
	const ResidentMapChunkRegistry* registry = nullptr;
	bool builtStaticMapSceneASLastFrame = false;
};

// The small subset of one resident chunk that affects the cached structural audit.
// Runtime deformers commonly change only reuseSignature; that value can be maintained
// incrementally without rebuilding the map-sized surface classification.
struct NRIStaticSceneDiagnosticsChunkState
{
	bool valid = false;
	bool active = false;
	bool accelerationResident = false;
	bool animated = false;
	bool atlasEligible = false;
	uint32_t chunkIndex = UINT32_MAX;
	uint32_t primitiveCount = 0;
	uint64_t reuseSignature = 0;
	uint32_t candidateChunks = 0;
	uint32_t portalChunks = 0;
	uint32_t localSpaceChunks = 0;
	uint32_t animatedChunks = 0;
	uint32_t atlasEligibleChunks = 0;
	uint32_t candidateSurfaces = 0;
	uint32_t wallSurfaces = 0;
	uint32_t floorSurfaces = 0;
	uint32_t ceilingSurfaces = 0;
	uint32_t portalSurfaces = 0;
	uint32_t localSpaceSurfaces = 0;
	uint32_t animatedSurfaces = 0;
	uint32_t materialRiskSurfaces = 0;
	uint32_t contiguousChunkSurfaces = 0;
	uint32_t surfaceRowsInspected = 0;
};

// Structural fields are cached by build identity and explicit invalidation. Segment-cache
// and route fields are live overlays because routing changes independently of scene builds.
struct NRIStaticSceneDiagnosticsSnapshot
{
	bool valid = false;
	bool cacheHit = false;
	uint64_t diagnosticsGeneration = 0;
	uint64_t mapBuildSerial = 0;
	uint64_t staticSceneBuildSerial = 0;

	uint32_t asBlasStatic = 0;
	uint32_t asStaticChunkOwnedBlas = 0;
	uint32_t asStaticUniqueGeometrySignatures = 0;
	uint32_t asStaticSegmentBlas = 0;
	uint32_t asStaticSegmentCandidateChunks = 0;
	uint32_t asStaticSegmentUniqueGeometrySignatures = 0;
	uint32_t asStaticSegmentDuplicateKeys = 0;
	uint32_t asStaticSegmentDuplicateRefs = 0;
	uint32_t asStaticSegmentPortalChunks = 0;
	uint32_t asStaticSegmentLocalSpaceChunks = 0;
	uint32_t asStaticSegmentAnimatedChunks = 0;
	uint32_t asStaticSegmentAtlasEligibleChunks = 0;
	uint32_t asStaticSegmentCandidateSurfaces = 0;
	uint32_t asStaticSegmentWallCandidates = 0;
	uint32_t asStaticSegmentFloorCandidates = 0;
	uint32_t asStaticSegmentCeilingCandidates = 0;
	uint32_t asStaticSegmentPortalCandidates = 0;
	uint32_t asStaticSegmentLocalSpaceSurfaces = 0;
	uint32_t asStaticSegmentAnimatedSurfaces = 0;
	uint32_t asStaticSegmentMaterialRiskSurfaces = 0;
	uint32_t asStaticSegmentContiguousChunkSurfaces = 0;
	uint32_t asStaticSegmentRegistryMappedChunks = 0;

	uint32_t asStaticSegmentCacheCandidates = 0;
	uint32_t asStaticSegmentCacheEntries = 0;
	uint32_t asStaticSegmentCacheHits = 0;
	uint32_t asStaticSegmentCacheMisses = 0;
	uint32_t asStaticSegmentCacheDuplicateRefs = 0;
	uint32_t asStaticSegmentCacheResidentBlas = 0;
	uint32_t asStaticSegmentCacheBuildsThisFrame = 0;
	uint32_t asStaticSegmentCacheBuildsLastRebuild = 0;
	uint32_t asStaticSegmentCacheInvalidations = 0;
	uint64_t asStaticSegmentCacheResidentBytes = 0;
	bool asStaticSegmentCacheBlasBuildEnabled = false;
	uint32_t asStaticSegmentRouteRouted = 0;
	uint32_t asStaticSegmentRouteChunkFallback = 0;
	uint32_t asStaticSegmentRouteRejectDisabled = 0;
	uint32_t asStaticSegmentRouteRejectMissingCache = 0;
	uint32_t asStaticSegmentRouteRejectMissingBlas = 0;
	uint32_t asStaticSegmentRouteSegmentBlasRefs = 0;
	uint32_t asStaticSegmentRouteChunkBlasRefs = 0;

	// Proof telemetry: a cache hit must leave all four fields at zero.
	uint32_t diagnosticChunkRowsScanned = 0;
	uint32_t diagnosticSurfaceRowsScanned = 0;
	uint32_t diagnosticRegistryRowsScanned = 0;
	uint32_t diagnosticContainersBuilt = 0;
	uint32_t diagnosticChunkRowsIncrementallyUpdated = 0;
	uint32_t diagnosticSurfaceRowsIncrementallyUpdated = 0;
};

class NRIStaticSceneDiagnosticsCache
{
public:
	NRIStaticSceneDiagnosticsSnapshot Build(const NRIStaticSceneDiagnosticsInput& input);
	static NRIStaticSceneDiagnosticsChunkState CaptureChunkState(
		const NRIStaticSceneDiagnosticsInput& input,
		uint32_t chunkListIndex);
	void NotifyChunkMutation(
		const NRIStaticSceneDiagnosticsInput& input,
		uint32_t chunkListIndex,
		const NRIStaticSceneDiagnosticsChunkState& before);
	void Invalidate();
	void Discard();
	uint64_t Generation() const { return mGeneration; }
	bool HasCachedStructuralSnapshot() const
	{
		return mHasCachedStructuralSnapshot && mCachedGeneration == mGeneration;
	}

private:
	struct BuildIdentity
	{
		const void* mapWorld = nullptr;
		const void* staticScene = nullptr;
		const void* atlas = nullptr;
		const void* registry = nullptr;
		uint64_t mapBuildSerial = 0;
		uint64_t staticSceneBuildSerial = 0;
		uint64_t atlasBuildSerial = 0;
		uint64_t registryBuildSerial = 0;
		uint64_t mapChunkCount = 0;
		uint64_t mapSurfaceCount = 0;
		uint64_t mapPortalCount = 0;
		uint64_t mapPortalTargetCount = 0;
		uint64_t staticChunkCount = 0;
		uint64_t atlasChunkCount = 0;
		uint64_t registryEntryCount = 0;
		bool mapValid = false;
		bool staticSceneValid = false;
		bool atlasValid = false;
		bool registryValid = false;

		bool operator==(const BuildIdentity& other) const;
	};

	static BuildIdentity MakeBuildIdentity(const NRIStaticSceneDiagnosticsInput& input);

	uint64_t mGeneration = 1;
	uint64_t mCachedGeneration = 0;
	bool mHasCachedStructuralSnapshot = false;
	BuildIdentity mCachedIdentity = {};
	NRIStaticSceneDiagnosticsSnapshot mCachedStructuralSnapshot = {};
	std::unordered_map<uint64_t, uint32_t> mStaticSegmentSignatureRefs;
	uint32_t mPendingChunkRowsIncrementallyUpdated = 0;
	uint32_t mPendingSurfaceRowsIncrementallyUpdated = 0;
};
