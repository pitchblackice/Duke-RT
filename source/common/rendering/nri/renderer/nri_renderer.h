#pragma once

#include "../nri_output.h"
#include "nri_exposure.h"
#include "nri_frame_resources.h"
#include "nri_nrd.h"
#include "nri_resources.h"
#include "nri_scene_lights.h"
#include "nri_static_scene.h"
#include "nri_upscaler.h"
#include "../framegen/nri_framegen.h"

#include "../scene/nri_map_builder.h"
#include "../scene/nri_map_world.h"
#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"
#include "lightoverlay.h"

#include <chrono>
#include <cstdint>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class NRIRenderDevice;
struct MapRecord;
struct LevelTransitionInfo;
struct PathTracingActorSpriteTraceEvent;
struct PathTracingEmissiveLightEditTarget;

struct NRIDirectionalLightState
{
	bool enabled = false;
	bool shadow = true;
	bool fromOverlay = false;
	uint32_t ruleId = 0;
	uint64_t stateHash = 0;
	float direction[3] = { 0.3f, 0.85f, -0.4f };
	float color[3] = { 1.0f, 1.0f, 1.0f };
	float angularSize = 0.03f;
};

enum class NRIPTNightVisionMode : uint32_t
{
	None = 0,
	Duke = 1
};

struct NRIPTNightVisionState
{
	NRIPTNightVisionMode mode = NRIPTNightVisionMode::None;
	bool viewEligible = false;
	bool enabled = false;
	float strength01 = 0.0f;
	float remainingSeconds = 0.0f;
};

class NRIRenderer
{
public:
	enum class MaterialBuildTraceSlot : uint32_t
	{
		DynamicLive = 0,
		MirrorExtended,
		SceneLightMergedDynamic,
		MirrorPlayer,
		DynamicWithPersistentEmissive,
		SceneLightMergedPersistent,
		CapturedScene,
		PersistentEmissiveCachePrune,
		PersistentEmissiveCacheRebuild,
		StaticMapAnimChunk,
		StaticMapChunk,
		RuntimeMutationChunk,
		ResidentRuntimeMutationChunk,
		ResidentRuntimeMutationChunkRecover,
		RuntimeSpaceLinkChunk,
		Unknown,
		Count,
	};

	enum class SceneBufferUploadDomain : uint32_t
	{
		StaticOverlay = 0,
		RuntimeMutation,
		Dynamic,
		MirrorExtended,
		MirrorPlayer,
		PersistentVoxelMaterial,
		Count,
	};

	struct MaterialBuildTraceEntry
	{
		uint32_t calls = 0;
		uint32_t overrideBuildCalls = 0;
		uint32_t materialCount = 0;
		uint32_t actorMaterialCount = 0;
		uint32_t textureCount = 0;
		uint32_t baseTextureCount = 0;
		uint32_t glowTextureCount = 0;
		uint32_t normalTextureCount = 0;
		uint32_t metallicTextureCount = 0;
		uint32_t roughnessTextureCount = 0;
		uint32_t emissiveTextureCount = 0;
		double overrideBuildMs = 0.0;
		double materialBuildMs = 0.0;
	};

	static constexpr size_t MaterialBuildTraceSlotCount = (size_t)MaterialBuildTraceSlot::Count;
	static constexpr size_t SceneBufferUploadDomainCount = (size_t)SceneBufferUploadDomain::Count;
	static constexpr size_t RuntimeMutationTopTraceCount = 8;
	static constexpr size_t RuntimeSectorDirtyTruthTraceCount = 8;
	static constexpr size_t RuntimeAnimatedChurnTraceCount = 4;
	static constexpr size_t RuntimeMaterialOnlyMismatchTraceCount = 8;
	static constexpr size_t RuntimeResidentBlasRecreateTraceCount = 8;
	static constexpr size_t RuntimeResidentBlasRefitRejectTraceCount = 8;
	static constexpr size_t RuntimeStructuralRebuildTraceCount = 8;
	static constexpr size_t RuntimeGeometryDirtyTraceCount = 8;
	static constexpr size_t RuntimeRecurringChunkTraceCount = 8;

	enum class RuntimeMutationTraceAction : uint8_t
	{
		None,
		StructuralRebuild,
		MaterialRefresh,
		ResidentApply,
		ResidentNoopSkip,
		ResidentFallback,
		Held,
		SyncSkip,
		DeferredMaterialRefresh,
		DeferredStructuralRebuild,
		Failed
	};

	struct RuntimeMutationTopTraceEntry
	{
		bool valid = false;
		uint32_t score = 0;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		uint32_t reasonMask = 0;
		uint32_t sectionDirtyCount = 0;
		uint32_t surfaceCount = 0;
		uint32_t triangleCount = 0;
		uint32_t materialCount = 0;
		RuntimeMutationTraceAction action = RuntimeMutationTraceAction::None;
		bool forceTopology = false;
		bool residentMaterialDirty = false;
		bool residentGeometryDirty = false;
		bool recoveredEmpty = false;
	};

	struct RuntimeSectorDirtyTruthTraceEntry
	{
		enum class PreviousStateSource : uint8_t
		{
			None,
			Replacement,
			Resident,
		};

		bool valid = false;
		uint32_t score = 0;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		uint32_t reasonMask = 0;
		PreviousStateSource previousStateSource = PreviousStateSource::None;
		bool forceTopology = false;
		bool baselineChanged = false;
		bool geometryChanged = false;
		bool materialChanged = false;
		uint32_t previousSurfaceCount = 0;
		uint32_t liveSurfaceCount = 0;
		uint32_t previousTriangleCount = 0;
		uint32_t liveTriangleCount = 0;
	};

	struct RuntimeAnimatedChurnTraceEntry
	{
		bool valid = false;
		uint32_t score = 0;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		bool suppressed = false;
		uint32_t suppressionEmits = 0;
		uint32_t materialRefreshes = 0;
		uint32_t runtimeAttempts = 0;
		uint32_t residentApplies = 0;
		uint32_t syncSkips = 0;
	};

	struct RuntimeMaterialOnlyMismatchTraceEntry
	{
		bool valid = false;
		uint32_t score = 0;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		bool refreshPath = false;
		uint32_t reasonMask = 0;
		uint32_t filteredSurfaceCount = 0;
		uint32_t filteredMaterialCount = 0;
		uint32_t residentMaterialCount = 0;
		uint32_t filteredWallCount = 0;
		uint32_t filteredFlatCount = 0;
		uint32_t residentWallCount = 0;
		uint32_t residentFlatCount = 0;
	};

	struct RuntimeResidentBlasRecreateTraceEntry
	{
		bool valid = false;
		uint32_t score = 0;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		uint32_t reasonMask = 0;
		uint32_t fallbackMask = 0;
		uint32_t surfaceCount = 0;
		uint32_t triangleCount = 0;
		uint32_t materialCount = 0;
		bool forceTopology = false;
		bool recoveredEmpty = false;
		bool keptGeometrySlice = false;
		bool topologyChanged = false;
		bool hadAccelerationStructure = false;
	};

	struct RuntimeResidentBlasRefitRejectTraceEntry
	{
		bool valid = false;
		uint32_t score = 0;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		uint32_t reasonMask = 0;
		uint32_t rejectMask = 0;
		uint32_t previousIndexCount = 0;
		uint32_t liveIndexCount = 0;
		uint32_t previousPrimitiveCount = 0;
		uint32_t livePrimitiveCount = 0;
		bool hadAccelerationStructure = false;
	};

	struct RuntimeStructuralRebuildTraceEntry
	{
		bool valid = false;
		uint32_t score = 0;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		uint32_t reasonMask = 0;
		uint32_t triggerMask = 0;
		uint32_t surfaceCount = 0;
		uint32_t triangleCount = 0;
		uint32_t materialCount = 0;
		RuntimeMutationTraceAction action = RuntimeMutationTraceAction::None;
		bool materialOnly = false;
		bool sectorMaterialOnly = false;
		bool wallMaterialOnly = false;
		bool mixedMaterialOnly = false;
		bool geometryOrDirty = false;
	};

	struct RuntimeGeometryDirtyTraceEntry
	{
		bool valid = false;
		uint32_t score = 0;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		uint32_t reasonMask = 0;
		uint32_t familyMask = 0;
		uint32_t previousWallCount = 0;
		uint32_t liveWallCount = 0;
		uint32_t previousFlatCount = 0;
		uint32_t liveFlatCount = 0;
		uint32_t previousTriangleCount = 0;
		uint32_t liveTriangleCount = 0;
		uint32_t previousMaterialCount = 0;
		uint32_t liveMaterialCount = 0;
		bool forceTopology = false;
		bool countChanged = false;
		bool wallsChanged = false;
		bool flatsChanged = false;
	};

	struct RuntimeRecurringChunkTraceEntry
	{
		bool valid = false;
		uint32_t score = 0;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		uint32_t lastReasonMask = 0;
		uint32_t visitCount = 0;
		uint32_t uniqueStateCount = 0;
		uint32_t transitionCount = 0;
		uint32_t repeatedStateHitCount = 0;
		uint32_t abaRecurrenceCount = 0;
		uint32_t lastWallCount = 0;
		uint32_t lastFlatCount = 0;
		uint32_t lastTriangleCount = 0;
		uint32_t lastMaterialCount = 0;
		uint64_t previousStateSignature = 0;
		uint64_t lastStateSignature = 0;
	};

	struct PerfShellTraceStats
	{
		struct SceneBufferUploadDomainTraceEntry
		{
			uint64_t payloadBytes = 0;
			uint64_t vertexPayloadBytes = 0;
			uint64_t indexPayloadBytes = 0;
			uint64_t primitivePayloadBytes = 0;
			uint64_t materialPayloadBytes = 0;
			uint64_t uploadedBytes = 0;
			uint64_t primitiveUploadedBytes = 0;
			uint64_t materialUploadedBytes = 0;
			uint64_t growthRequestedBytes = 0;
			uint64_t growthAllocatedBytes = 0;
			uint64_t dirtyChangedBytes = 0;
			uint64_t dirtyUploadedBytes = 0;
			uint32_t hashChecks = 0;
			uint32_t hashMisses = 0;
			uint32_t stampChecks = 0;
			uint32_t stampMisses = 0;
			uint32_t growthEvents = 0;
			uint32_t dirtyRanges = 0;
			double waitMs = 0.0;
		};

		struct OverlayAppendSourceTraceEntry
		{
			uint64_t byteCount = 0;
			uint64_t vertexBytes = 0;
			uint64_t indexBytes = 0;
			uint64_t primitiveBytes = 0;
			uint64_t materialBytes = 0;
			uint32_t vertexCount = 0;
			uint32_t indexCount = 0;
			uint32_t primitiveCount = 0;
			uint32_t materialCount = 0;
			uint32_t geometryGrowthEvents = 0;
			uint32_t materialGrowthEvents = 0;
		};

		double totalMs = 0.0;
		double initResourcesMs = 0.0;
		double mapWorldMs = 0.0;
		double updateStateMs = 0.0;
		double sceneSelectMs = 0.0;
		double sceneLightsMs = 0.0;
		double residentLightRefreshMs = 0.0;
		double emissiveUpdateMs = 0.0;
		double emissiveTlasMs = 0.0;
		double surfaceProbeMs = 0.0;
		double frameGraphMs = 0.0;
		double traceOpaqueMs = 0.0;
		double traceOpaqueReadbackMs = 0.0;
		double traceOpaqueCommandMs = 0.0;
		double traceOpaqueStatsCopyMs = 0.0;
		double otherMs = 0.0;
		double staticSceneMs = 0.0;
		double runtimeMutationMs = 0.0;
		double runtimeMutationAnalyzeMs = 0.0;
		uint32_t runtimeMutationCandidateChunks = 0;
		uint32_t runtimeMutationAnalyzedChunks = 0;
		uint32_t runtimeMutationBackgroundSweepChunks = 0;
		double runtimeMutationRebuildMs = 0.0;
		double runtimeMutationStructuralRebuildMs = 0.0;
		double runtimeMutationMaterialRefreshMs = 0.0;
		double runtimeMutationResidentApplyMs = 0.0;
		double runtimeMutationStructuralRebuildVisibleMs = 0.0;
		double runtimeMutationStructuralRebuildInvisibleMs = 0.0;
		double runtimeMutationStructuralRebuildNearMs = 0.0;
		double runtimeMutationStructuralRebuildFarMs = 0.0;
		double runtimeMutationStructuralRebuildUnknownDistanceMs = 0.0;
		double runtimeMutationMaterialRefreshVisibleMs = 0.0;
		double runtimeMutationMaterialRefreshInvisibleMs = 0.0;
		double runtimeMutationMaterialRefreshNearMs = 0.0;
		double runtimeMutationMaterialRefreshFarMs = 0.0;
		double runtimeMutationMaterialRefreshUnknownDistanceMs = 0.0;
		double runtimeMutationResidentApplyVisibleMs = 0.0;
		double runtimeMutationResidentApplyInvisibleMs = 0.0;
		double runtimeMutationResidentApplyNearMs = 0.0;
		double runtimeMutationResidentApplyFarMs = 0.0;
		double runtimeMutationResidentApplyUnknownDistanceMs = 0.0;
		float runtimeMutationNearDistance = 0.0f;
		double runtimeMutationResidentApplyLiveBuildMs = 0.0;
		double runtimeMutationResidentApplyGeometryBuildMs = 0.0;
		double runtimeMutationResidentApplyMaterialBuildMs = 0.0;
		double runtimeMutationResidentApplyBaselineCaptureMs = 0.0;
		double runtimeMutationResidentApplyAtlasMs = 0.0;
		double runtimeMutationResidentApplyAtlasBookkeepingMs = 0.0;
		double runtimeMutationResidentApplyVertexIndexCopyMs = 0.0;
		double runtimeMutationResidentApplyVertexCpuCopyMs = 0.0;
		double runtimeMutationResidentApplyIndexCpuCopyMs = 0.0;
		double runtimeMutationResidentApplyVertexStageMs = 0.0;
		double runtimeMutationResidentApplyIndexStageMs = 0.0;
		double runtimeMutationResidentApplyPrimitiveRewriteMs = 0.0;
		double runtimeMutationResidentApplyPrimitiveCpuRewriteMs = 0.0;
		double runtimeMutationResidentApplyPrimitiveStageMs = 0.0;
		double runtimeMutationResidentApplyGeometryOrderHashMs = 0.0;
		double runtimeMutationResidentApplyDownstreamBlasMs = 0.0;
		double runtimeMutationResidentApplyDownstreamBlasSetupMs = 0.0;
		double runtimeMutationResidentApplyDownstreamBlasFilterMs = 0.0;
		double runtimeMutationResidentApplyDownstreamBlasCreateMs = 0.0;
		double runtimeMutationResidentApplyDownstreamBlasScratchMs = 0.0;
		double runtimeMutationResidentApplyDownstreamBlasBarrierMs = 0.0;
		double runtimeMutationResidentApplyDownstreamBlasBuildMs = 0.0;
		double runtimeMutationAppendMs = 0.0;
		double sceneLightStaticAppendMs = 0.0;
		double sceneLightRuntimeMutationAppendMs = 0.0;
		double sceneLightCapturedAppendMs = 0.0;
		double sceneLightDynamicAppendMs = 0.0;
		double sceneLightPersistentVoxelAppendMs = 0.0;
		double sceneLightAnalyticMs = 0.0;
		double sceneLightEmissiveMs = 0.0;
		double sceneLightSectorMs = 0.0;
		double runtimeSpaceLinkMs = 0.0;
		double runtimeDebugSphereMs = 0.0;
		double runtimeDebugSphereViewMs = 0.0;
		double runtimeDebugSphereGeoMs = 0.0;
		double runtimeDebugSphereMaterialMs = 0.0;
		double runtimeDebugSphereTuneMs = 0.0;
		double overlayAssembleMs = 0.0;
		double overlayAppendMs = 0.0;
		double overlayAppendResetMs = 0.0;
		double overlayAppendSourcesMs = 0.0;
		double overlayAppendProducerStampMs = 0.0;
		double overlayAppendDynamicStampMs = 0.0;
		double overlayAppendMirrorExtendedStampMs = 0.0;
		double overlayAppendMirrorPlayerStampMs = 0.0;
		double overlayAppendBookkeepingMs = 0.0;
		double overlayRuntimeSpaceLinkMs = 0.0;
		double overlayRuntimeSpaceLinkGeometryMs = 0.0;
		double overlayRuntimeSpaceLinkMaterialMs = 0.0;
		double overlayRuntimeMutationMs = 0.0;
		double overlayRuntimeMutationGeometryMs = 0.0;
		double overlayRuntimeMutationMaterialMs = 0.0;
		double overlayDynamicMs = 0.0;
		double overlayDynamicGeometryMs = 0.0;
		double overlayDynamicMaterialMs = 0.0;
		double overlayMirrorExtendedMs = 0.0;
		double overlayMirrorExtendedGeometryMs = 0.0;
		double overlayMirrorExtendedMaterialMs = 0.0;
		double overlayMirrorPlayerMs = 0.0;
		double overlayMirrorPlayerGeometryMs = 0.0;
		double overlayMirrorPlayerMaterialMs = 0.0;
		double overlayDebugSphereMs = 0.0;
		double overlayDebugSphereGeometryMs = 0.0;
		double overlayDebugSphereMaterialMs = 0.0;
		double dynamicCaptureMs = 0.0;
		double mirrorPlayerCaptureMs = 0.0;
		double mirrorPlayerGeometryBuildMs = 0.0;
		double mirrorPlayerGeometryBuildWallMs = 0.0;
		double mirrorPlayerGeometryBuildFlatMs = 0.0;
		double mirrorPlayerGeometryBuildSpriteMs = 0.0;
		double mirrorPlayerPortalAssignMs = 0.0;
		double mirrorPlayerMaterialBuildMs = 0.0;
		double sceneSelectStaticMapMs = 0.0;
		double sceneSelectMirrorPortalMs = 0.0;
		double sceneSelectMirrorCaptureMs = 0.0;
		double sceneSelectPersistentVoxelBatchMs = 0.0;
		double sceneSelectPersistentEmissiveMs = 0.0;
		double sceneSelectDynamicMergeMs = 0.0;
		double sceneSelectLightMergeMs = 0.0;
		double sceneSelectStaticInstancesMs = 0.0;
		double sceneSelectMaterialBridgeMs = 0.0;
		double sceneSelectPaletteMs = 0.0;
		double sceneSelectTexturesMs = 0.0;
		double sceneSelectMaterialSplitMs = 0.0;
		double sceneSelectBufferUploadMs = 0.0;
		double sceneSelectBufferUploadPrimitiveRewriteMs = 0.0;
		double sceneSelectBufferUploadPrimitiveRewritePrimitiveHashMs = 0.0;
		double sceneSelectBufferUploadPrimitiveRewriteProvenanceHashMs = 0.0;
		double sceneSelectBufferUploadPrimitiveRewriteVisibilityHashMs = 0.0;
		double sceneSelectBufferUploadPrimitiveRewriteCopyMs = 0.0;
		double sceneSelectBufferUploadPrimitiveRewriteResolveMs = 0.0;
		double sceneSelectBufferUploadPrimitiveRewriteStoreMs = 0.0;
		double sceneSelectBufferUploadPayloadHashMs = 0.0;
		double sceneSelectBufferUploadDirtyRangeMs = 0.0;
		double sceneSelectBufferUploadWaitCheckMs = 0.0;
		double sceneSelectBufferUploadWaitMs = 0.0;
		double sceneSelectBufferUploadVertexMs = 0.0;
		double sceneSelectBufferUploadIndexMs = 0.0;
		double sceneSelectBufferUploadPrimitiveMs = 0.0;
		double sceneSelectBufferUploadMaterialMs = 0.0;
		double sceneSelectBufferUploadPersistentVoxelMaterialMs = 0.0;
		uint32_t sceneSelectBufferUploadWaitCount = 0;
		uint32_t sceneSelectBufferUploadVertexGrowEvents = 0;
		uint32_t sceneSelectBufferUploadIndexGrowEvents = 0;
		uint32_t sceneSelectBufferUploadPrimitiveGrowEvents = 0;
		uint32_t sceneSelectBufferUploadMaterialGrowEvents = 0;
		uint32_t sceneSelectBufferUploadGrowthEvents = 0;
		uint32_t sceneSelectBufferUploadVertexOverwriteEvents = 0;
		uint32_t sceneSelectBufferUploadIndexOverwriteEvents = 0;
		uint32_t sceneSelectBufferUploadPrimitiveOverwriteEvents = 0;
		uint32_t sceneSelectBufferUploadMaterialOverwriteEvents = 0;
		uint32_t sceneSelectBufferUploadPersistentVoxelMaterialUploads = 0;
		uint32_t sceneSelectBufferUploadPersistentVoxelMaterialBatches = 0;
		uint32_t sceneSelectBufferUploadPersistentVoxelMaterialBatchRanges = 0;
		uint32_t sceneSelectBufferUploadPersistentVoxelMaterialBatchRejects = 0;
		uint32_t sceneSelectBufferUploadPersistentVoxelMaterialBatchCopyCommands = 0;
		uint32_t sceneSelectBufferUploadPersistentVoxelMaterialBatchBarrierCommands = 0;
		uint32_t sceneSelectBufferUploadPayloadHashChecks = 0;
		uint32_t sceneSelectBufferUploadPayloadHashHits = 0;
		uint32_t sceneSelectBufferUploadPayloadHashSkips = 0;
		uint32_t sceneSelectBufferUploadPayloadHashMisses = 0;
		uint32_t sceneSelectBufferUploadPayloadHashUploads = 0;
		uint32_t sceneSelectBufferUploadPayloadHashRejectMissing = 0;
		uint32_t sceneSelectBufferUploadPayloadHashRejectSize = 0;
		uint32_t sceneSelectBufferUploadPayloadHashRejectStride = 0;
		uint32_t sceneSelectBufferUploadPayloadHashRejectForced = 0;
		uint32_t sceneSelectBufferUploadPayloadHashVertexHits = 0;
		uint32_t sceneSelectBufferUploadPayloadHashIndexHits = 0;
		uint32_t sceneSelectBufferUploadPayloadHashPrimitiveHits = 0;
		uint32_t sceneSelectBufferUploadPayloadHashMaterialHits = 0;
		uint32_t sceneSelectBufferUploadPayloadHashVertexSkips = 0;
		uint32_t sceneSelectBufferUploadPayloadHashIndexSkips = 0;
		uint32_t sceneSelectBufferUploadPayloadHashPrimitiveSkips = 0;
		uint32_t sceneSelectBufferUploadPayloadHashMaterialSkips = 0;
		uint32_t sceneSelectBufferUploadPayloadHashVertexMisses = 0;
		uint32_t sceneSelectBufferUploadPayloadHashIndexMisses = 0;
		uint32_t sceneSelectBufferUploadPayloadHashPrimitiveMisses = 0;
		uint32_t sceneSelectBufferUploadPayloadHashMaterialMisses = 0;
		uint32_t sceneSelectBufferUploadProducerStampChecks = 0;
		uint32_t sceneSelectBufferUploadProducerStampUses = 0;
		uint32_t sceneSelectBufferUploadProducerStampFallbacks = 0;
		uint32_t sceneSelectBufferUploadProducerStampRewritePrimitiveUses = 0;
		uint32_t sceneSelectBufferUploadProducerStampRewriteProvenanceUses = 0;
		uint32_t sceneSelectBufferUploadProducerStampVertexUses = 0;
		uint32_t sceneSelectBufferUploadProducerStampIndexUses = 0;
		uint32_t sceneSelectBufferUploadProducerStampPrimitiveUses = 0;
		uint32_t sceneSelectBufferUploadProducerStampMaterialUses = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeChecks = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeSkips = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeForcedFull = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeMissingMirror = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeSizeMismatch = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeRawRanges = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeCoalescedRanges = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeRejectedCoalesces = 0;
		uint32_t sceneSelectBufferUploadVertexDirtyRanges = 0;
		uint32_t sceneSelectBufferUploadIndexDirtyRanges = 0;
		uint32_t sceneSelectBufferUploadPrimitiveDirtyRanges = 0;
		uint32_t sceneSelectBufferUploadMaterialDirtyRanges = 0;
		uint32_t sceneSelectBufferUploadRangeUploads = 0;
		uint32_t sceneSelectBufferUploadRangeFallbacks = 0;
		uint32_t sceneSelectBufferUploadRangeFallbackFragmented = 0;
		uint32_t sceneSelectBufferUploadRangeFallbackLarge = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRangeUploads = 0;
		uint32_t sceneSelectBufferUploadMaterialRangeUploads = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteCacheChecks = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteCacheHits = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteCacheMisses = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteCacheRejectInvalid = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteCacheRejectPrimitive = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteCacheRejectProvenance = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteCacheRejectVisibility = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteCacheRejectCount = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteResolvePrimitives = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteResolveMapChunk = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteResolveSectorFallback = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteResolveSectorMiss = 0;
		uint64_t sceneSelectBufferUploadVertexRequestedBytes = 0;
		uint64_t sceneSelectBufferUploadIndexRequestedBytes = 0;
		uint64_t sceneSelectBufferUploadPrimitiveRequestedBytes = 0;
		uint64_t sceneSelectBufferUploadMaterialRequestedBytes = 0;
		uint64_t sceneSelectBufferUploadPersistentVoxelMaterialRequestedBytes = 0;
		uint64_t sceneSelectBufferUploadPersistentVoxelMaterialDirtyBytes = 0;
		uint64_t sceneSelectBufferUploadVertexUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadIndexUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadPrimitiveUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadMaterialUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadPersistentVoxelMaterialUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadPersistentVoxelMaterialBatchGapBytes = 0;
		uint64_t sceneSelectBufferUploadGrowthOldBytes = 0;
		uint64_t sceneSelectBufferUploadGrowthRequestedBytes = 0;
		uint64_t sceneSelectBufferUploadGrowthAllocatedBytes = 0;
		uint64_t sceneSelectBufferUploadGrowthHeadroomBytes = 0;
		uint64_t sceneSelectBufferUploadDirtyRangeChangedBytes = 0;
		uint64_t sceneSelectBufferUploadDirtyRangeUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadDirtyRangeGapBytes = 0;
		uint64_t sceneSelectBufferUploadVertexDirtyChangedBytes = 0;
		uint64_t sceneSelectBufferUploadIndexDirtyChangedBytes = 0;
		uint64_t sceneSelectBufferUploadPrimitiveDirtyChangedBytes = 0;
		uint64_t sceneSelectBufferUploadMaterialDirtyChangedBytes = 0;
		uint64_t sceneSelectBufferUploadVertexDirtyUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadIndexDirtyUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadPrimitiveDirtyUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadMaterialDirtyUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadRangeUploadedBytes = 0;
		std::array<SceneBufferUploadDomainTraceEntry, SceneBufferUploadDomainCount> sceneSelectBufferUploadDomains = {};
		double sceneSelectInstanceHandlesMs = 0.0;
		double sceneSelectTexturePrepMs = 0.0;
		double sceneSelectStateCommitMs = 0.0;
		double sceneSelectStateCommitFlagsMs = 0.0;
		double sceneSelectStateCommitDynamicStateMs = 0.0;
		double sceneSelectStateCommitGeometryStateMs = 0.0;
		double sceneSelectStateCommitStatsMs = 0.0;
		double sceneSelectStateCommitDynamicCoreMs = 0.0;
		double sceneSelectStateCommitDynamicMirrorExtendedMs = 0.0;
		double sceneSelectStateCommitDynamicMirrorPlayerMs = 0.0;
		double sceneSelectStateCommitGeometrySelectMs = 0.0;
		double sceneSelectStateCommitGeometryStaticCopyMs = 0.0;
		double sceneSelectStateCommitGeometryAppendMs = 0.0;
		double sceneSelectStateCommitStatsBaseMs = 0.0;
		double sceneSelectStateCommitStatsPersistentVoxelMs = 0.0;
		double sceneSelectStateCommitStatsMirrorExtendedMs = 0.0;
		double sceneSelectStateCommitStatsMirrorPlayerMs = 0.0;
		double sceneSelectStateCommitStatsMergeMs = 0.0;
		uint32_t sceneSelectStateCommitSelectedDynamic = 0;
		uint32_t sceneSelectStateCommitActiveDynamic = 0;
		uint32_t sceneSelectStateCommitMirrorExtended = 0;
		uint32_t sceneSelectStateCommitMirrorPlayer = 0;
		uint32_t sceneSelectStateCommitGeometryCombined = 0;
		uint32_t sceneSelectStateCommitGeometryStaticOnly = 0;
		uint32_t sceneSelectStateCommitStatsPersistentVoxel = 0;
		uint32_t sceneSelectStateCommitStatsMirrorExtended = 0;
		uint32_t sceneSelectStateCommitStatsMirrorPlayer = 0;
		uint32_t sceneSelectStateCommitCombinedPrimitiveCount = 0;
		uint32_t sceneSelectStateCommitCombinedMaterialCount = 0;
		uint64_t sceneSelectStateCommitGenStaticMap = 0;
		uint64_t sceneSelectStateCommitGenRuntimeMutation = 0;
		uint64_t sceneSelectStateCommitGenDynamicActors = 0;
		uint64_t sceneSelectStateCommitGenMirrorPlayer = 0;
		uint64_t sceneSelectStateCommitGenPersistentVoxels = 0;
		uint64_t sceneSelectStateCommitGenMaterialBridge = 0;
		uint64_t sceneSelectStateCommitGenTextures = 0;
		uint64_t sceneSelectStateCommitGenTlasInstances = 0;
		uint64_t sceneSelectStateCommitGenSceneConstants = 0;
		uint32_t sceneSelectStateCommitChangedStaticMap = 0;
		uint32_t sceneSelectStateCommitChangedRuntimeMutation = 0;
		uint32_t sceneSelectStateCommitChangedDynamicActors = 0;
		uint32_t sceneSelectStateCommitChangedMirrorPlayer = 0;
		uint32_t sceneSelectStateCommitChangedPersistentVoxels = 0;
		uint32_t sceneSelectStateCommitChangedMaterialBridge = 0;
		uint32_t sceneSelectStateCommitChangedTextures = 0;
		uint32_t sceneSelectStateCommitChangedTlasInstances = 0;
		uint32_t sceneSelectStateCommitChangedSceneConstants = 0;
		uint32_t sceneSelectStateCommitChangedDomainCount = 0;
		double dynamicCaptureCountMs = 0.0;
		double dynamicCaptureWallsMs = 0.0;
		double dynamicCaptureFlatsMs = 0.0;
		double dynamicCaptureFacingSpritesMs = 0.0;
		double dynamicCaptureModelSpritesMs = 0.0;
		double dynamicCaptureModelClassifyMs = 0.0;
		double dynamicCaptureModelMeshMs = 0.0;
		double dynamicCaptureModelSurfaceMs = 0.0;
		double dynamicCaptureModelStoreMs = 0.0;
		double dynamicCaptureVoxelFrameMs = 0.0;
		double dynamicCaptureStatsMs = 0.0;
		double persistentDynamicMs = 0.0;
		double dynamicAsMs = 0.0;
		double dynamicAsSetupMs = 0.0;
		double dynamicAsCreateMs = 0.0;
		double dynamicAsScratchMs = 0.0;
		double dynamicAsBuildMs = 0.0;
		double dynamicAsBarrierMs = 0.0;
		double persistentVoxelAsMs = 0.0;
		double persistentVoxelTlasInstanceMs = 0.0;
		double worldTlasMs = 0.0;
		double sceneDataSetMs = 0.0;
		double sceneDataSetWaitCheckMs = 0.0;
		double sceneDataSetWaitMs = 0.0;
		double sceneDataSetReprojectionMs = 0.0;
		double sceneDataSetVisibleFlatPlaneMs = 0.0;
		double sceneDataSetVisibleChunkMs = 0.0;
		double sceneDataSetSceneInstanceMs = 0.0;
		double sceneDataSetPortalMs = 0.0;
		double sceneDataSetRuntimeLightHashMs = 0.0;
		double sceneDataSetRuntimeLightUploadMs = 0.0;
		double sceneDataSetRuntimeLightClusterMs = 0.0;
		double sceneDataSetEmissiveMs = 0.0;
		double sceneDataSetSectorLightMs = 0.0;
		double sceneDataSetDescriptorBuildMs = 0.0;
		double sceneDataSetDescriptorValidateMs = 0.0;
		double sceneDataSetDescriptorUpdateMs = 0.0;
		double sceneDataSetDescriptorHashMs = 0.0;
		uint32_t sceneDataSetWaitCount = 0;
		uint32_t sceneDataSetDescriptorUpdateCount = 0;
		uint32_t sceneDataSetDescriptorNullCount = 0;
		uint32_t sceneDataSetRuntimeLightUploads = 0;
		uint32_t sceneDataSetRuntimeLightCacheHits = 0;
		uint32_t sceneDataSetRuntimeLightClusterUploads = 0;
		uint32_t sceneDataSetRuntimeLightClusterCacheHits = 0;
		uint32_t sceneDataSetEmissiveUploads = 0;
		uint32_t sceneDataSetEmissiveCacheHits = 0;
		uint32_t sceneDataSetSectorLightUploads = 0;
		uint32_t sceneDataSetSectorLightCacheHits = 0;
		uint32_t sceneDataSetResourceGrowEvents = 0;
		uint32_t sceneDataSetResourceOverwriteEvents = 0;
		uint64_t sceneDataSetSceneInstanceRequestedBytes = 0;
		uint64_t sceneDataSetSceneInstanceUploadedBytes = 0;
		uint64_t sceneDataSetPortalRequestedBytes = 0;
		uint64_t sceneDataSetPortalUploadedBytes = 0;
		uint64_t sceneDataSetRuntimeLightRequestedBytes = 0;
		uint64_t sceneDataSetRuntimeLightUploadedBytes = 0;
		uint64_t sceneDataSetRuntimeLightClusterRequestedBytes = 0;
		uint64_t sceneDataSetRuntimeLightClusterUploadedBytes = 0;
		uint64_t sceneDataSetEmissiveRequestedBytes = 0;
		uint64_t sceneDataSetEmissiveUploadedBytes = 0;
		uint64_t sceneDataSetSectorLightRequestedBytes = 0;
		uint64_t sceneDataSetSectorLightUploadedBytes = 0;
		double restoreStaticSceneMs = 0.0;
		double copyFinalMs = 0.0;
		double sceneTextureLookupMs = 0.0;
		double sceneTextureRealizeMs = 0.0;
		double sceneTextureDescriptorMs = 0.0;
		double sceneTextureTransitionMs = 0.0;
		double actorOverrideMapBuildMs = 0.0;
		double materialBuildMs = 0.0;
		double geometryBuildDynamicLiveMs = 0.0;
		double geometryBuildMirrorExtendedMs = 0.0;
		double geometryBuildMirrorPlayerMs = 0.0;
		double geometryBuildMergedDynamicMs = 0.0;
		double geometryBuildCapturedMs = 0.0;
		double geometryBuildPersistentVoxelVariantMs = 0.0;
		double geometryBuildPersistentVoxelAppendMs = 0.0;
		double geometryBuildPersistentVoxelRebuildMs = 0.0;
		double geometryBuildPersistentEmissivePruneMs = 0.0;
		double geometryBuildPersistentEmissiveRebuildMs = 0.0;
		double geometryBuildStaticChunkMs = 0.0;
		double geometryBuildDebugSphereMs = 0.0;
		double geometryBuildRuntimeMutationTruthMs = 0.0;
		double geometryBuildRuntimeMutationRebuildMs = 0.0;
		double geometryBuildRuntimeMutationMaterialOnlyMs = 0.0;
		double geometryBuildRuntimeSpaceLinkMs = 0.0;
		double geometryBuildResidentApplyMs = 0.0;
		double geometryBuildResidentRecoverMs = 0.0;
		double sceneInstanceStatsMs = 0.0;
		double persistentVoxelResourceStatsMs = 0.0;
		double persistentVoxelBatchStatsMs = 0.0;
		uint32_t runtimeMutationDirtyChunks = 0;
		uint32_t runtimeMutationRebuiltChunks = 0;
		uint32_t runtimeMutationHeldChunks = 0;
		uint32_t runtimeMutationStructuralRebuildChunks = 0;
		uint32_t runtimeMutationMaterialRefreshChunks = 0;
		uint32_t runtimeMutationCandidateVisibleChunks = 0;
		uint32_t runtimeMutationCandidateInvisibleChunks = 0;
		uint32_t runtimeMutationCandidateNearChunks = 0;
		uint32_t runtimeMutationCandidateFarChunks = 0;
		uint32_t runtimeMutationCandidateUnknownDistanceChunks = 0;
		uint32_t runtimeMutationCandidateBoundsValidChunks = 0;
		uint32_t runtimeMutationCandidateBoundsInvalidChunks = 0;
		uint32_t runtimeMutationCandidateActiveReplacementChunks = 0;
		uint32_t runtimeMutationCandidateVisibleResidentValidationChunks = 0;
		uint32_t runtimeMutationCandidateStartupVisibleValidationChunks = 0;
		uint32_t runtimeMutationCandidateUnresolvedTextureChunks = 0;
		uint32_t runtimeMutationCandidateStaticAnimatedSuppressedChunks = 0;
		uint32_t runtimeMutationCandidateSectorDirtyChunks = 0;
		uint32_t runtimeMutationCandidateSectionDirtyChunks = 0;
		uint32_t runtimeMutationCandidateDraggedChunks = 0;
		uint32_t runtimeMutationCandidateSignatureWatchChunks = 0;
		uint32_t runtimeMutationCandidateBackgroundSweepSourceChunks = 0;
		uint32_t runtimeMutationCandidateDeferredStructuralChunks = 0;
		uint32_t runtimeMutationDirtyVisibleChunks = 0;
		uint32_t runtimeMutationDirtyInvisibleChunks = 0;
		uint32_t runtimeMutationDirtyNearChunks = 0;
		uint32_t runtimeMutationDirtyFarChunks = 0;
		uint32_t runtimeMutationDirtyUnknownDistanceChunks = 0;
		uint32_t runtimeMutationDirtyActiveReplacementChunks = 0;
		uint32_t runtimeMutationDirtyBackgroundSweepChunks = 0;
		uint32_t runtimeMutationStructuralRebuildVisibleChunks = 0;
		uint32_t runtimeMutationStructuralRebuildInvisibleChunks = 0;
		uint32_t runtimeMutationStructuralRebuildNearChunks = 0;
		uint32_t runtimeMutationStructuralRebuildFarChunks = 0;
		uint32_t runtimeMutationStructuralRebuildUnknownDistanceChunks = 0;
		uint32_t runtimeMutationStructuralRebuildActiveReplacementChunks = 0;
		uint32_t runtimeMutationStructuralRebuildBackgroundSweepChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredCoalescedChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredFlushedChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredPromotedChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredPendingChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredBudget = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredNearChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredNearCoalescedChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredNearFlushedChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredNearPendingChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredNearBudget = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredFarChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredFarCoalescedChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredFarFlushedChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredFarPendingChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredFarBudget = 0;
		uint32_t runtimeMutationMaterialRefreshVisibleChunks = 0;
		uint32_t runtimeMutationMaterialRefreshInvisibleChunks = 0;
		uint32_t runtimeMutationMaterialRefreshNearChunks = 0;
		uint32_t runtimeMutationMaterialRefreshFarChunks = 0;
		uint32_t runtimeMutationMaterialRefreshUnknownDistanceChunks = 0;
		uint32_t runtimeMutationMaterialRefreshDeferredChunks = 0;
		uint32_t runtimeMutationMaterialRefreshDeferredCoalescedChunks = 0;
		uint32_t runtimeMutationMaterialRefreshDeferredFlushedChunks = 0;
		uint32_t runtimeMutationMaterialRefreshDeferredPendingChunks = 0;
		uint32_t runtimeMutationMaterialRefreshDeferredNearChunks = 0;
		uint32_t runtimeMutationMaterialRefreshDeferredNearCoalescedChunks = 0;
		uint32_t runtimeMutationMaterialRefreshDeferredNearFlushedChunks = 0;
		uint32_t runtimeMutationMaterialRefreshDeferredNearPendingChunks = 0;
		uint32_t runtimeMutationMaterialRefreshDeferredNearBudget = 0;
		uint32_t runtimeMutationMaterialRefreshActiveReplacementChunks = 0;
		uint32_t runtimeMutationMaterialRefreshBackgroundSweepChunks = 0;
		uint32_t runtimeMutationResidentApplyVisibleChunks = 0;
		uint32_t runtimeMutationResidentApplyInvisibleChunks = 0;
		uint32_t runtimeMutationResidentApplyNearChunks = 0;
		uint32_t runtimeMutationResidentApplyFarChunks = 0;
		uint32_t runtimeMutationResidentApplyUnknownDistanceChunks = 0;
		uint32_t runtimeMutationResidentApplyActiveReplacementChunks = 0;
		uint32_t runtimeMutationResidentApplyBackgroundSweepChunks = 0;
		uint32_t runtimeMutationMaterialRefreshAnimatedChunks = 0;
		uint32_t runtimeMutationMaterialRefreshReplacementDeltaChunks = 0;
		uint32_t runtimeMutationMaterialRefreshHardwareCanvasChunks = 0;
		uint32_t runtimeMutationStructuralReplacementDeltaChunks = 0;
		uint32_t runtimeMutationStructuralReplacementViewChangedChunks = 0;
		uint32_t runtimeMutationStructuralStaticAnimatedModeFlipChunks = 0;
		uint32_t runtimeMutationStructuralExcludeStaticFlipChunks = 0;
		uint32_t runtimeMutationStructuralForcedTopologyChunks = 0;
		uint32_t runtimeMutationStructuralInvalidChunks = 0;
		uint32_t runtimeMutationStructuralMaterialOnlyChunks = 0;
		uint32_t runtimeMutationStructuralSectorMaterialOnlyChunks = 0;
		uint32_t runtimeMutationStructuralWallMaterialOnlyChunks = 0;
		uint32_t runtimeMutationStructuralMixedMaterialOnlyChunks = 0;
		uint32_t runtimeMutationStructuralGeometryOrDirtyChunks = 0;
		uint32_t runtimeMutationGeometryDirtySectorGeometryOnlyChunks = 0;
		uint32_t runtimeMutationGeometryDirtyWallGeometryOnlyChunks = 0;
		uint32_t runtimeMutationGeometryDirtySectorWallGeometryChunks = 0;
		uint32_t runtimeMutationGeometryDirtyDirtyOnlyChunks = 0;
		uint32_t runtimeMutationGeometryDirtyGeometryDirtyMixedChunks = 0;
		uint32_t runtimeMutationGeometryDirtyForceTopologyOnlyChunks = 0;
		uint32_t runtimeMutationGeometryDirtyRealCountChangeChunks = 0;
		uint32_t runtimeMutationGeometryDirtyWallsOnlyChangedChunks = 0;
		uint32_t runtimeMutationGeometryDirtyFlatsOnlyChangedChunks = 0;
		uint32_t runtimeMutationGeometryDirtyWallsAndFlatsChangedChunks = 0;
		uint32_t runtimeRecurringChunkTrackedCount = 0;
		uint32_t runtimeRecurringChunkRecurringCount = 0;
		uint32_t runtimeRecurringChunkVisitCount = 0;
		uint32_t runtimeRecurringChunkUniqueStateCount = 0;
		uint32_t runtimeRecurringChunkTransitionCount = 0;
		uint32_t runtimeRecurringChunkRepeatedStateHitCount = 0;
		uint32_t runtimeRecurringChunkAbaRecurrenceCount = 0;
		uint32_t runtimeRecurringChunkMaxUniqueStateCount = 0;
		uint32_t runtimeMutationHardwareCanvasChunkCount = 0;
		uint32_t runtimeMutationStructuralReplacementDeltaReasonMaskOr = 0;
		uint32_t runtimeMutationMaterialRefreshReasonMaskOr = 0;
		uint32_t runtimeMutationInvalidForceTopologyCount = 0;
		uint32_t runtimeMutationInvalidAppliedCount = 0;
		uint32_t runtimeMutationResidentNoopSkipCount = 0;
		uint32_t runtimeMutationInvalidFailedCount = 0;
		uint32_t runtimeMutationInvalidSyncSkipCount = 0;
		uint32_t runtimeMutationResidentNoopCandidateCount = 0;
		uint32_t runtimeMutationResidentNoopCandidateReasonMaskOr = 0;
		uint32_t runtimeMutationResidentNoopBlockNotAuthoritativeCount = 0;
		uint32_t runtimeMutationResidentNoopBlockResidentUnavailableCount = 0;
		uint32_t runtimeMutationResidentNoopBlockReplacementInvalidCount = 0;
		uint32_t runtimeMutationResidentNoopBlockExcludeStaticCount = 0;
		uint32_t runtimeMutationResidentNoopBlockSurfaceCountMismatch = 0;
		uint32_t runtimeMutationResidentNoopBlockMaterialCountMismatch = 0;
		uint32_t runtimeMutationResidentNoopBlockPrimitiveCountMismatch = 0;
		uint32_t runtimeMutationValidMaterialCount = 0;
		uint32_t runtimeMutationValidStructuralCount = 0;
		uint32_t runtimeMutationResidentApplyCount = 0;
		uint32_t runtimeMutationResidentApplyMaterialOnlyCount = 0;
		uint32_t runtimeMutationResidentApplyStructuralCount = 0;
		uint32_t runtimeMutationResidentApplyFastMaterialOnlyCount = 0;
		uint32_t runtimeMutationResidentApplySlowMaterialOnlyCount = 0;
		uint32_t runtimeMutationResidentApplyMaterialOnlyExclusiveCount = 0;
		uint32_t runtimeMutationResidentApplyMaterialOnlyNoResidentChunkCount = 0;
		uint32_t runtimeMutationResidentApplyMaterialOnlyInvalidReplacementCount = 0;
		uint32_t runtimeMutationResidentApplyMaterialOnlyMaterialCountMismatchCount = 0;
		uint32_t runtimeMutationResidentApplyMaterialPayloadHashCheckCount = 0;
		uint32_t runtimeMutationResidentApplyMaterialPayloadHashSkipCount = 0;
		uint32_t runtimeMutationResidentApplyMaterialPayloadHashMissCount = 0;
		uint32_t runtimeMutationResidentApplyMaterialPayloadHashRejectCount = 0;
		uint32_t runtimeMutationResidentApplyGeometryPayloadHashCheckCount = 0;
		uint32_t runtimeMutationResidentApplyGeometryPayloadHashSkipCount = 0;
		uint32_t runtimeMutationResidentApplyGeometryPayloadHashMissCount = 0;
		uint32_t runtimeMutationResidentApplyGeometryPayloadHashRejectCount = 0;
		uint32_t runtimeMutationResidentApplyGeometryPayloadHashBlasSkipCount = 0;
		uint32_t runtimeMutationResidentApplyGeometryPayloadOrderCheckCount = 0;
		uint32_t runtimeMutationResidentApplyGeometryPayloadOrderEquivalentCount = 0;
		uint32_t runtimeMutationResidentApplyGeometryPayloadOrderMissCount = 0;
		uint32_t runtimeMutationResidentApplyGeometryPayloadOrderRejectCount = 0;
		uint32_t runtimeMutationResidentApplyVertexStageRangeCount = 0;
		uint32_t runtimeMutationResidentApplyIndexStageRangeCount = 0;
		uint32_t runtimeMutationResidentApplyPrimitiveStageRangeCount = 0;
		uint64_t runtimeMutationResidentApplyVertexStageBytes = 0;
		uint64_t runtimeMutationResidentApplyIndexStageBytes = 0;
		uint64_t runtimeMutationResidentApplyPrimitiveStageBytes = 0;
		uint32_t runtimeMutationResidentApplyCoalescedStageRangeCount = 0;
		uint32_t runtimeMutationResidentApplyCoalescedStageRejectCount = 0;
		uint64_t runtimeMutationResidentApplyCoalescedStageBytes = 0;
		uint64_t runtimeMutationResidentApplyCoalescedStageGapBytes = 0;
		double runtimeMutationResidentApplyStageMapMs = 0.0;
		double runtimeMutationResidentApplyStageMemcpyMs = 0.0;
		double runtimeMutationResidentApplyStageCommandMs = 0.0;
		uint32_t runtimeMutationResidentApplyStageBatchCount = 0;
		uint32_t runtimeMutationResidentApplyStageBatchRangeCount = 0;
		uint32_t runtimeMutationResidentApplyStageCopyCommandCount = 0;
		uint32_t runtimeMutationResidentApplyStageBarrierCommandCount = 0;
		uint32_t runtimeMutationResidentApplyStageScratchGrowCount = 0;
		uint64_t runtimeMutationResidentApplyStageScratchGrowBytes = 0;
		uint32_t runtimeMutationResidentApplyPreserveGeometryCount = 0;
		uint32_t runtimeMutationResidentApplyPreserveIndexCount = 0;
		uint32_t runtimeMutationResidentApplyPreservePrimitiveCount = 0;
		uint32_t runtimeMutationResidentApplyBlasReuseCount = 0;
		uint32_t runtimeMutationResidentApplyBlasUpdateCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRefitOnlyCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRefitProbeCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRefitRejectNoPreviousAsCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRefitRejectIndexCountMismatchCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRefitRejectPrimitiveCountMismatchCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRefitRejectZeroIndexCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRefitRejectZeroPrimitiveCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRecreateCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRecreateNoPreviousAsCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRecreateRecoveredEmptyCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRecreateSliceMovedCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRecreateTopologyChangedCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRecreateForceTopologyCount = 0;
		uint32_t runtimeMutationResidentApplyBlasScratchQueryCount = 0;
		uint32_t runtimeMutationResidentApplyBlasScratchCacheHitCount = 0;
		uint32_t runtimeMutationResidentApplyBlasScratchCacheMissCount = 0;
		uint32_t runtimeMutationResidentApplyBlasScratchGrowCount = 0;
		uint32_t runtimeMutationResidentApplyBlasBuildCommandCount = 0;
		uint32_t runtimeMutationResidentApplyBlasScratchBarrierCount = 0;
		uint32_t runtimeMutationResidentApplyKeepGeometrySliceCount = 0;
		uint32_t runtimeMutationResidentApplyKeepMaterialSliceCount = 0;
		uint32_t runtimeMutationResidentApplyEmptyRemovalCount = 0;
		uint32_t runtimeMutationResidentApplyRecoverAttemptCount = 0;
		uint32_t runtimeMutationResidentApplyRecoverSuccessCount = 0;
		uint32_t runtimeMutationResidentApplyAtlasGrowCount = 0;
		uint32_t runtimeMutationMaterialOnlyMismatchCount = 0;
		uint32_t runtimeMutationMaterialOnlyMismatchRefreshCount = 0;
		uint32_t runtimeMutationMaterialOnlyMismatchRebuildCount = 0;
		uint32_t runtimeMutationMaterialOnlyMismatchFilteredWallOnlyCount = 0;
		uint32_t runtimeMutationMaterialOnlyMismatchFilteredFlatOnlyCount = 0;
		uint32_t runtimeMutationMaterialOnlyMismatchFilteredMixedCount = 0;
		uint32_t runtimeMutationMaterialOnlyMismatchResidentWallOnlyCount = 0;
		uint32_t runtimeMutationMaterialOnlyMismatchResidentFlatOnlyCount = 0;
		uint32_t runtimeMutationMaterialOnlyMismatchResidentMixedCount = 0;
		uint32_t runtimeAnimatedSuppressedActiveCount = 0;
		uint32_t runtimeAnimatedSuppressionEmitCount = 0;
		uint32_t runtimeAnimatedUniqueTouchedCount = 0;
		uint32_t runtimeAnimatedMaterialRefreshCount = 0;
		uint32_t runtimeAnimatedAttemptCount = 0;
		uint32_t runtimeAnimatedSuppressedAttemptCount = 0;
		uint32_t runtimeAnimatedUnsuppressedAttemptCount = 0;
		uint32_t runtimeAnimatedResidentApplyCount = 0;
		uint32_t runtimeAnimatedSuppressedResidentApplyCount = 0;
		uint32_t runtimeAnimatedUnsuppressedResidentApplyCount = 0;
		uint32_t runtimeAnimatedSyncSkipCount = 0;
		uint32_t runtimeMutationActiveChunkCount = 0;
		uint32_t runtimeMutationValidChunkCount = 0;
		uint32_t runtimeMutationExcludedStaticChunkCount = 0;
		uint32_t runtimeMutationCachedSurfaceCount = 0;
		uint32_t runtimeMutationCachedTriangleCount = 0;
		uint32_t runtimeMutationCachedMaterialCount = 0;
		uint32_t runtimeMutationCachedMaterialStateCount = 0;
		uint32_t runtimeMutationMaterialCacheHitCount = 0;
		uint32_t runtimeMutationMaterialCacheMissCount = 0;
		uint32_t runtimeMutationMaterialCacheStoreCount = 0;
		uint32_t staticAnimatedResidentSliceCacheEntryCount = 0;
		uint32_t staticAnimatedResidentSliceCacheHitCount = 0;
		uint32_t staticAnimatedResidentSliceCacheMissCount = 0;
		uint32_t staticAnimatedResidentSliceCacheStoreCount = 0;
		uint32_t staticAnimatedResidentSliceApplyHitCount = 0;
		uint32_t staticAnimatedResidentSliceApplyMissCount = 0;
		uint32_t staticAnimatedResidentSliceSyncSkipHitCount = 0;
		uint32_t staticAnimatedResidentGpuPayloadCacheHitCount = 0;
		uint32_t staticAnimatedResidentGpuPayloadCacheMissCount = 0;
		uint32_t staticAnimatedResidentGpuPayloadCacheStoreCount = 0;
		uint32_t runtimeMutationPrimitiveCount = 0;
		uint32_t runtimeMutationMaterialCount = 0;
		uint32_t sceneLightSurfaceRecordCount = 0;
		uint32_t sceneLightStaticRecordCount = 0;
		uint32_t sceneLightRuntimeMutationRecordCount = 0;
		uint32_t sceneLightDynamicRecordCount = 0;
		uint32_t sceneLightCapturedRecordCount = 0;
		uint32_t sceneLightPersistentVoxelRecordCount = 0;
		uint32_t persistentVoxelOnboardingCandidateCount = 0;
		uint32_t persistentVoxelOnboardingAdmittedCount = 0;
		uint32_t persistentVoxelOnboardingDeferredCount = 0;
		uint32_t persistentVoxelOnboardingActorBudgetHits = 0;
		uint32_t persistentVoxelOnboardingPrimitiveBudgetHits = 0;
		uint32_t persistentVoxelOnboardingByteBudgetHits = 0;
		uint32_t persistentVoxelOnboardingTextureBudgetHits = 0;
		uint64_t persistentVoxelOnboardingEstimatedBytes = 0;
		uint64_t persistentVoxelOnboardingAdmittedBytes = 0;
		uint64_t persistentVoxelOnboardingDeferredBytes = 0;
		uint64_t persistentVoxelOnboardingByteBudget = 0;
		uint32_t persistentVoxelTexturePrewarmQueuedCount = 0;
		uint32_t persistentVoxelTexturePrewarmProcessedCount = 0;
		uint32_t persistentVoxelTexturePrewarmDeferredCount = 0;
		uint32_t persistentVoxelTexturePrewarmHitCount = 0;
		uint32_t persistentVoxelTexturePrewarmMissCount = 0;
		uint64_t persistentVoxelTexturePrewarmEstimatedBytes = 0;
		uint64_t persistentVoxelTexturePrewarmProcessedBytes = 0;
		uint64_t persistentVoxelTexturePrewarmDeferredBytes = 0;
		uint64_t persistentVoxelTexturePrewarmByteBudget = 0;
		double persistentVoxelTexturePrewarmMs = 0.0;
		uint32_t runtimeSpaceLinkPrimitiveCount = 0;
		uint32_t runtimeSpaceLinkMaterialCount = 0;
		uint32_t runtimeDebugSphereCount = 0;
		uint32_t runtimeDebugSphereLongitudeSegments = 0;
		uint32_t runtimeDebugSphereLatitudeSegments = 0;
		uint32_t runtimeDebugSpherePrimitiveCount = 0;
		uint32_t runtimeDebugSphereMaterialCount = 0;
		uint32_t overlayPrimitiveCount = 0;
		uint32_t overlayMaterialCount = 0;
		uint32_t overlayRuntimeSpaceLinkPrimitiveCount = 0;
		uint32_t overlayRuntimeSpaceLinkMaterialCount = 0;
		uint32_t overlayRuntimeMutationPrimitiveCount = 0;
		uint32_t overlayRuntimeMutationMaterialCount = 0;
		uint32_t overlayDynamicPrimitiveCount = 0;
		uint32_t overlayDynamicMaterialCount = 0;
		uint32_t overlayMirrorExtendedPrimitiveCount = 0;
		uint32_t overlayMirrorExtendedMaterialCount = 0;
		uint32_t overlayMirrorPlayerPrimitiveCount = 0;
		uint32_t overlayMirrorPlayerMaterialCount = 0;
		uint32_t overlayDebugSpherePrimitiveCount = 0;
		uint32_t overlayDebugSphereMaterialCount = 0;
		uint32_t overlayPersistentVoxelActorCount = 0;
		uint32_t overlayPersistentVoxelPrimitiveCount = 0;
		uint32_t overlayPersistentVoxelMaterialCount = 0;
		OverlayAppendSourceTraceEntry overlayRuntimeSpaceLinkAppend = {};
		OverlayAppendSourceTraceEntry overlayRuntimeMutationAppend = {};
		OverlayAppendSourceTraceEntry overlayDynamicAppend = {};
		OverlayAppendSourceTraceEntry overlayMirrorExtendedAppend = {};
		OverlayAppendSourceTraceEntry overlayMirrorPlayerAppend = {};
		OverlayAppendSourceTraceEntry overlayDebugSphereAppend = {};
		OverlayAppendSourceTraceEntry overlayPersistentVoxelAppend = {};
		uint32_t mirrorPlayerCaptureRawFacingSprites = 0;
		uint32_t mirrorPlayerCaptureRawVoxelSprites = 0;
		uint32_t mirrorPlayerCaptureSurfaces = 0;
		uint32_t mirrorPlayerCaptureMatchingActorSurfaces = 0;
		uint32_t mirrorPlayerCaptureOtherActorSurfaces = 0;
		uint32_t mirrorPlayerCaptureActorlessSurfaces = 0;
		uint32_t mirrorPlayerCaptureFilteredSurfaces = 0;
		uint32_t mirrorPlayerGeometryWallSurfaces = 0;
		uint32_t mirrorPlayerGeometryFlatSurfaces = 0;
		uint32_t mirrorPlayerGeometrySpriteSurfaces = 0;
		uint32_t mirrorPlayerGeometryIndexedSurfaces = 0;
		uint32_t mirrorPlayerGeometryTriangleFanSurfaces = 0;
		uint32_t mirrorPlayerGeometrySpriteStripSurfaces = 0;
		uint32_t mirrorPlayerGeometrySkippedSurfaces = 0;
		uint32_t mirrorPlayerGeometrySourceVertices = 0;
		uint32_t mirrorPlayerGeometrySourceIndices = 0;
		uint32_t mirrorPlayerGeometryVertexGrowths = 0;
		uint32_t mirrorPlayerGeometryIndexGrowths = 0;
		uint32_t mirrorPlayerGeometryPrimitiveGrowths = 0;
		uint32_t mirrorPlayerGeometryProvenanceGrowths = 0;
		uint32_t dynamicCaptureCalls = 0;
		uint32_t dynamicCaptureWallSurfaces = 0;
		uint32_t dynamicCaptureFlatSurfaces = 0;
		uint32_t dynamicCaptureSpriteSurfaces = 0;
		uint32_t dynamicCaptureVoxelProxySurfaces = 0;
		uint32_t dynamicCaptureUnsupportedModelSurfaces = 0;
		uint32_t dynamicCaptureVoxelCacheStores = 0;
		uint32_t dynamicCaptureVoxelCacheRebuilds = 0;
		uint32_t dynamicCaptureVoxelCacheDeferred = 0;
		uint32_t dynamicCaptureVoxelMeshBuilds = 0;
		uint32_t dynamicCaptureVoxelMeshDeferred = 0;
		uint32_t dynamicCaptureVoxelMeshInvalid = 0;
		uint32_t dynamicCaptureVoxelCanonicalSurfaceBuilds = 0;
		uint32_t dynamicCaptureVoxelCanonicalSurfaceHits = 0;
		uint32_t dynamicCaptureVoxelCanonicalSurfaceInvalid = 0;
		uint32_t voxelCacheActorEntries = 0;
		uint32_t voxelCacheActorSurfaces = 0;
		uint32_t voxelCacheUniqueMeshKeys = 0;
		uint32_t voxelCacheUniqueMaterialKeys = 0;
		uint32_t voxelCacheLocalSpaceSurfaces = 0;
		uint32_t voxelCacheBakedTransformSurfaces = 0;
		uint32_t voxelCacheUnknownSpaceSurfaces = 0;
		uint32_t voxelCacheTransformKeyedSurfaces = 0;
		uint32_t voxelCacheUniqueTransformBases = 0;
		uint32_t voxelCacheInvariantWarnings = 0;
		uint32_t voxelCacheActorPrimitives = 0;
		uint64_t voxelCacheDuplicatedVertexBytes = 0;
		uint64_t voxelCacheDuplicatedIndexBytes = 0;
		uint64_t voxelCacheDuplicatedPrimitiveBytes = 0;
		uint64_t voxelCacheDuplicatedTotalBytes = 0;
		uint32_t voxelCacheDuplicateTopCount = 0;
		uint32_t dynamicAsPrimitiveCount = 0;
		uint32_t dynamicAsVertexCount = 0;
		uint32_t dynamicAsIndexCount = 0;
		uint32_t dynamicAsRuntimeSpaceLinkPrimitives = 0;
		uint32_t dynamicAsRuntimeMutationPrimitives = 0;
		uint32_t dynamicAsDynamicPrimitives = 0;
		uint32_t dynamicAsMirrorExtendedPrimitives = 0;
		uint32_t dynamicAsMirrorPlayerPrimitives = 0;
		uint32_t dynamicAsDebugSpherePrimitives = 0;
		uint32_t dynamicAsCreateCalls = 0;
		uint32_t dynamicAsReuseCount = 0;
		uint32_t dynamicAsScratchQueries = 0;
		uint32_t dynamicAsScratchGrowCount = 0;
		uint64_t dynamicAsRuntimeSpaceLinkBytes = 0;
		uint64_t dynamicAsRuntimeMutationBytes = 0;
		uint64_t dynamicAsDynamicBytes = 0;
		uint64_t dynamicAsMirrorExtendedBytes = 0;
		uint64_t dynamicAsMirrorPlayerBytes = 0;
		uint64_t dynamicAsDebugSphereBytes = 0;
		uint64_t dynamicAsScratchRequestedBytes = 0;
		uint64_t dynamicAsMemoryBytes = 0;
		uint32_t persistentVoxelAsCalls = 0;
		uint32_t persistentVoxelAsBuilds = 0;
		uint32_t persistentVoxelAsUniqueMeshBuilds = 0;
		uint32_t persistentVoxelAsInstances = 0;
		uint32_t persistentVoxelSharedMeshResources = 0;
		uint32_t persistentVoxelTlasInstances = 0;
		uint32_t persistentVoxelInstanceTransformUpdates = 0;
		uint32_t persistentVoxelBakedFallbackInstances = 0;
		uint32_t persistentVoxelBatchActorCount = 0;
		uint32_t persistentVoxelInstanceRecordCount = 0;
		uint32_t persistentVoxelPendingInstanceCount = 0;
		uint32_t persistentVoxelMaterialVariantResourceCount = 0;
		uint32_t persistentVoxelZeroRefMeshResourceCount = 0;
		uint32_t persistentVoxelZeroRefMaterialResourceCount = 0;
		uint32_t persistentVoxelAdmissionQueueCount = 0;
		uint64_t persistentVoxelResidentResourceBytes = 0;
		uint64_t persistentVoxelZeroRefResourceBytes = 0;
		uint32_t worldTlasBuildCalls = 0;
		uint32_t worldTlasInstanceCount = 0;
		uint32_t sceneDataSetCalls = 0;
		uint32_t sceneTextureCacheCount = 0;
		uint32_t sceneTextureCacheMisses = 0;
		uint32_t sceneTextureCacheInserts = 0;
		uint32_t sceneTextureTransitionCount = 0;
		uint32_t sceneTextureRequestedCount = 0;
		uint32_t sceneTextureReferencedActorMaterialCount = 0;
		uint32_t sceneTextureReferencedBaseCount = 0;
		uint32_t sceneTextureReferencedGlowCount = 0;
		uint32_t sceneTextureReferencedNormalCount = 0;
		uint32_t sceneTextureReferencedMetallicCount = 0;
		uint32_t sceneTextureReferencedRoughnessCount = 0;
		uint32_t sceneTextureReferencedEmissiveCount = 0;
		uint32_t materialBuildCalls = 0;
		uint32_t actorOverrideMapBuildCalls = 0;
		uint32_t actorOverflowMaterialCount = 0;
		uint32_t actorOverflowBaseClampCount = 0;
		uint32_t actorOverflowNormalClampCount = 0;
		uint32_t actorOverflowMetallicClampCount = 0;
		uint32_t actorOverflowRoughnessClampCount = 0;
		uint32_t actorOverflowEmissiveClampCount = 0;
		uint32_t actorOverflowTraceOmittedCount = 0;
		uint32_t persistentDynamicActorSurfaceCount = 0;
		uint32_t persistentDynamicNonActorSurfaceCount = 0;
		uint32_t persistentDynamicWallSurfaceCount = 0;
		uint32_t persistentDynamicFlatSurfaceCount = 0;
		uint32_t persistentDynamicSpriteSurfaceCount = 0;
		uint32_t traceOpaqueDispatchX = 0;
		uint32_t traceOpaqueDispatchY = 0;
		uint32_t traceOpaqueDispatchZ = 0;
		uint32_t activePrimitiveCount = 0;
		uint32_t dynamicPrimitiveCount = 0;
		uint32_t activeMaterialCount = 0;
		uint32_t sceneInstanceCount = 0;
		uint32_t sceneInstanceStaticCount = 0;
		uint32_t sceneInstanceDynamicCount = 0;
		uint32_t sceneInstancePersistentVoxelCount = 0;
		uint32_t persistentVoxelMeshVariantResourceCount = 0;
		uint32_t persistentVoxelInstanceActiveCount = 0;
		uint32_t persistentVoxelInstancePrimitiveCount = 0;
		uint32_t persistentVoxelInstanceMaterialCount = 0;
		uint32_t persistentVoxelInstanceMinPrimitiveCount = 0;
		uint32_t persistentVoxelInstanceMaxPrimitiveCount = 0;
		uint32_t geometryBuildDynamicLivePrimitives = 0;
		uint32_t geometryBuildPersistentVoxelVariantCalls = 0;
		uint32_t geometryBuildPersistentVoxelVariantPrimitives = 0;
		uint32_t geometryBuildStaticChunkCalls = 0;
		uint32_t geometryBuildStaticChunkPrimitives = 0;
		uint32_t geometryBuildRuntimeMutationTruthCalls = 0;
		uint32_t geometryBuildRuntimeMutationRebuildCalls = 0;
		uint32_t geometryBuildRuntimeMutationMaterialOnlyCalls = 0;
		uint32_t geometryBuildRuntimeMutationPrimitives = 0;
		uint32_t geometryBuildRuntimeSpaceLinkCalls = 0;
		uint32_t geometryBuildRuntimeSpaceLinkPrimitives = 0;
		uint32_t geometryBuildResidentApplyCalls = 0;
		uint32_t geometryBuildResidentRecoverCalls = 0;
		uint32_t geometryBuildResidentPrimitives = 0;
		uint32_t dynamicVoxelEscapeActorCount = 0;
		uint32_t dynamicVoxelEscapeEligibleActorCount = 0;
		uint32_t dynamicVoxelEscapeForcedActorCount = 0;
		uint32_t dynamicVoxelEscapePrimitiveCount = 0;
		uint64_t dynamicVoxelEscapeVertexBytes = 0;
		uint64_t dynamicVoxelEscapeIndexBytes = 0;
		uint64_t dynamicVoxelEscapePrimitiveBytes = 0;
		uint64_t dynamicVoxelEscapeMaterialBytes = 0;
		uint64_t dynamicVoxelEscapeTotalBytes = 0;
		uint32_t dynamicVoxelExpectedEscapeActorCount = 0;
		uint32_t dynamicVoxelUnexpectedEscapeActorCount = 0;
		uint32_t dynamicVoxelExpectedEscapePrimitiveCount = 0;
		uint32_t dynamicVoxelUnexpectedEscapePrimitiveCount = 0;
		uint64_t dynamicVoxelExpectedEscapeTotalBytes = 0;
		uint64_t dynamicVoxelUnexpectedEscapeTotalBytes = 0;
		uint32_t dynamicVoxelEscapeTopCount = 0;
		uint32_t dynamicVoxelUnexpectedEscapeTopCount = 0;
		bool usedStaticMapScene = false;
		bool usedDynamicOverlay = false;
		bool usedPersistentDynamicEmissiveCache = false;
		std::string sceneTextureReason;
		std::array<MaterialBuildTraceEntry, MaterialBuildTraceSlotCount> materialBuildByLabel = {};
		std::array<RuntimeMutationTopTraceEntry, RuntimeMutationTopTraceCount> runtimeMutationTopEntries = {};
		std::array<RuntimeSectorDirtyTruthTraceEntry, RuntimeSectorDirtyTruthTraceCount> runtimeSectorDirtyTruthEntries = {};
		std::array<RuntimeAnimatedChurnTraceEntry, RuntimeAnimatedChurnTraceCount> runtimeAnimatedChurnEntries = {};
		std::array<RuntimeMaterialOnlyMismatchTraceEntry, RuntimeMaterialOnlyMismatchTraceCount> runtimeMaterialOnlyMismatchEntries = {};
		std::array<RuntimeResidentBlasRecreateTraceEntry, RuntimeResidentBlasRecreateTraceCount> runtimeResidentBlasRecreateEntries = {};
		std::array<RuntimeResidentBlasRefitRejectTraceEntry, RuntimeResidentBlasRefitRejectTraceCount> runtimeResidentBlasRefitRejectEntries = {};
		std::array<RuntimeStructuralRebuildTraceEntry, RuntimeStructuralRebuildTraceCount> runtimeStructuralRebuildEntries = {};
		std::array<RuntimeGeometryDirtyTraceEntry, RuntimeGeometryDirtyTraceCount> runtimeGeometryDirtyEntries = {};
		std::array<RuntimeRecurringChunkTraceEntry, RuntimeRecurringChunkTraceCount> runtimeRecurringChunkEntries = {};
		std::array<nri_scene::VoxelDuplicateVariantTraceEntry, nri_scene::VoxelDuplicateVariantTraceCount> voxelCacheDuplicateTopEntries = {};
		std::array<nri_scene::DynamicVoxelEscapeTraceEntry, nri_scene::DynamicVoxelEscapeTraceCount> dynamicVoxelEscapeTopEntries = {};
		std::array<nri_scene::DynamicVoxelEscapeTraceEntry, nri_scene::DynamicVoxelEscapeTraceCount> dynamicVoxelUnexpectedEscapeTopEntries = {};
	};

	struct PerfResourceTraceStats
	{
		uint32_t waitCalls = 0;
		double waitMs = 0.0;
		uint32_t residentChunkWriteWaitCalls = 0;
		double residentChunkWriteWaitMs = 0.0;
		uint32_t residentChunkBlasRebuildWaitCalls = 0;
		double residentChunkBlasRebuildWaitMs = 0.0;
		uint32_t sceneDataUploadWaitCalls = 0;
		double sceneDataUploadWaitMs = 0.0;
		uint32_t sceneBufferUploadWaitCalls = 0;
		double sceneBufferUploadWaitMs = 0.0;
		uint32_t emissiveSamplingUploadWaitCalls = 0;
		double emissiveSamplingUploadWaitMs = 0.0;
		uint32_t worldTlasInstanceUploadWaitCalls = 0;
		double worldTlasInstanceUploadWaitMs = 0.0;
		uint32_t worldTlasScratchResizeWaitCalls = 0;
		double worldTlasScratchResizeWaitMs = 0.0;
		uint32_t emissiveTlasInstanceUploadWaitCalls = 0;
		double emissiveTlasInstanceUploadWaitMs = 0.0;
		uint32_t emissiveTlasScratchResizeWaitCalls = 0;
		double emissiveTlasScratchResizeWaitMs = 0.0;
		uint32_t otherWaitCalls = 0;
		double otherWaitMs = 0.0;
		uint32_t growEvents = 0;
		uint32_t overwriteEvents = 0;
		uint32_t sceneUploadCalls = 0;
		uint32_t sceneDynamicUploadCalls = 0;
		uint32_t sceneResidentChunkUploadCalls = 0;
		uint32_t scenePersistentVoxelUploadCalls = 0;
		uint32_t scenePersistentVoxelVariantUploadCalls = 0;
		uint32_t sceneStaticRefreshUploadCalls = 0;
		uint32_t sceneOtherUploadCalls = 0;
		uint32_t sceneDataUploadCalls = 0;
		uint32_t emissiveUploadCalls = 0;
		uint32_t residentChunkBatchChunkCount = 0;
		uint32_t residentChunkBatchGeometryDirtyCount = 0;
		uint32_t residentChunkBatchMaterialDirtyCount = 0;
		uint32_t residentChunkBatchRecoverEmptyCount = 0;
		uint32_t residentChunkBatchMaterialFallbackCount = 0;
		uint32_t residentChunkBatchBlasRebuildCount = 0;
		uint64_t sceneUploadBytes = 0;
		uint64_t sceneDynamicUploadBytes = 0;
		uint64_t sceneResidentChunkUploadBytes = 0;
		uint64_t scenePersistentVoxelUploadBytes = 0;
		uint64_t scenePersistentVoxelVariantUploadBytes = 0;
		uint64_t sceneStaticRefreshUploadBytes = 0;
		uint64_t sceneOtherUploadBytes = 0;
		uint64_t sceneVertexUploadBytes = 0;
		uint64_t sceneIndexUploadBytes = 0;
		uint64_t scenePrimitiveUploadBytes = 0;
		uint64_t sceneMaterialUploadBytes = 0;
		uint64_t sceneDataUploadBytes = 0;
		uint64_t emissiveUploadBytes = 0;
		uint64_t residentChunkBatchVertexBytes = 0;
		uint64_t residentChunkBatchIndexBytes = 0;
		uint64_t residentChunkBatchPrimitiveBytes = 0;
		uint64_t residentChunkBatchMaterialBytes = 0;
	};

	static constexpr uint32_t TraceShaderScalarStatCount = 64;
	static constexpr uint32_t TraceShaderInstanceBucketCount = 1024;
	static constexpr uint32_t TraceShaderRayKindCount = 6;
	static constexpr uint32_t TraceShaderInstanceCommittedBase = TraceShaderScalarStatCount;
	static constexpr uint32_t TraceShaderInstanceAcceptedBase = TraceShaderInstanceCommittedBase + TraceShaderInstanceBucketCount;
	static constexpr uint32_t TraceShaderInstanceKindCommittedBase = TraceShaderInstanceAcceptedBase + TraceShaderInstanceBucketCount;
	static constexpr uint32_t TraceShaderStatCount = TraceShaderInstanceKindCommittedBase + TraceShaderRayKindCount * TraceShaderInstanceBucketCount;
	static constexpr uint32_t TraceShaderHotInstanceCount = 8;
	struct PerfTraceShaderHotInstance
	{
		uint32_t instanceId = 0;
		uint32_t dataSource = 0;
		uint32_t primitiveOffset = 0;
		uint32_t primitiveCount = 0;
		uint32_t metadata0 = 0;
		uint32_t metadata1 = 0;
		uint32_t committed = 0;
		uint32_t accepted = 0;
		uint32_t primaryCommitted = 0;
		uint32_t ungatedCommitted = 0;
		uint32_t sunCommitted = 0;
		uint32_t pointCommitted = 0;
		uint32_t emissiveCommitted = 0;
		uint32_t fastEmissiveCommitted = 0;
	};

	struct PerfTraceShaderStats
	{
		bool valid = false;
		uint64_t frameNumber = 0;
		std::array<uint32_t, TraceShaderStatCount> counters = {};
		uint32_t hotInstanceCount = 0;
		std::array<PerfTraceShaderHotInstance, TraceShaderHotInstanceCount> hotInstances = {};
	};

	struct MemoryTelemetry
	{
		uint64_t frameTextureBytes = 0;
		uint64_t sceneTextureBytes = 0;
		uint64_t skyTextureBytes = 0;
		uint64_t sceneBufferBytes = 0;
		uint64_t accelerationStructureBytes = 0;
		uint64_t totalTrackedBytes = 0;
		uint32_t renderWidth = 0;
		uint32_t renderHeight = 0;
		uint32_t outputWidth = 0;
		uint32_t outputHeight = 0;
	};

	struct LevelTransitionSnapshot
	{
		bool mapWorldValid = false;
		uint64_t mapWorldBuildSerial = 0;
		uint32_t mapWorldChunkCount = 0;
		uint32_t mapWorldSurfaceCount = 0;
		bool staticSceneValid = false;
		bool staticSceneTexturesResident = false;
		bool staticSceneBuffersResident = false;
		bool staticSceneAccelerationResident = false;
		uint64_t staticSceneBuildSerial = 0;
		uint32_t staticSceneChunkCount = 0;
		uint32_t staticSceneMaterialCount = 0;
		uint32_t textureCacheCount = 0;
		uint32_t skyTextureCacheCount = 0;
		uint32_t runtimeMutationChunkCount = 0;
		uint32_t runtimeMutationActiveChunkCount = 0;
		uint32_t runtimeMutationValidChunkCount = 0;
		bool residentChunkRegistryValid = false;
		uint32_t residentChunkRegistryEntryCount = 0;
		uint32_t residentChunkRegistryChunkCount = 0;
		uint32_t residentChunkRegistryActiveChunkCount = 0;
		uint32_t residentChunkRegistryMappedChunkCount = 0;
		uint32_t residentChunkRegistryAccelerationResidentChunkCount = 0;
		bool pendingStaticMapLightingInvalidation = false;
		bool surfaceProbeValid = false;
		bool surfaceProbeHit = false;
		int32_t surfaceProbeWallIndex = -1;
		int32_t surfaceProbeMapChunkIndex = -1;
		uint32_t transientMuzzleFlashSlotCount = 0;
		uint32_t transientMuzzleFlashActiveCount = 0;
		uint32_t analyticLightCount = 0;
		uint32_t manualLightCount = 0;
		uint32_t emissiveSurfaceCount = 0;
		uint32_t activeSectorLightCount = 0;
		uint32_t runtimeDebugSphereCount = 0;
		uint32_t runtimeTestLightCount = 0;
	};

	explicit NRIRenderer(NRIRenderDevice* frameBuffer);
	~NRIRenderer();

	bool Initialize();
	void Shutdown();
	bool RenderScene(HWDrawInfo& di, int drawmode, bool portal);
	bool PreloadLevelScene(uint32_t outputWidth, uint32_t outputHeight, uint32_t targetWidth, uint32_t targetHeight);
	void ResetHistory();
	void RequestAutoExposureReset(const char* reason);
	LevelTransitionSnapshot BuildLevelTransitionSnapshot() const;
	void OnLevelUnloadBegin(const LevelTransitionInfo& info);
	void OnLevelUnloadComplete(const LevelTransitionInfo& info);
	void OnLevelLoadBegin(const LevelTransitionInfo& info);
	void NotifyCameraCut(const char* reason);
	void SetGuiCaptureState(bool active);
	void PrintStatus();
	void PrintSwapChainRenderConfig() const;
	void PrintSceneBufferStatus() const;
	void PrintSceneLightDump(float radius, uint32_t limit) const;
	bool AddRuntimePointLight(const float position[3], const float color[3], float intensity, float radius, uint32_t& outId);
	bool UpdateRuntimePointLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius);
	bool RemoveRuntimePointLight(uint32_t id);
	void ClearRuntimePointLights();
	void PrintRuntimePointLights() const;
	void PrintRuntimeLightClusterStatus() const;
	uint32_t GetRuntimePointLightCount() const;
	bool AddRuntimeDebugSphere(const float center[3], float diameter, float metalness, float roughness, uint32_t& outId);
	bool RemoveRuntimeDebugSphere(uint32_t id);
	void ClearRuntimeDebugSpheres();
	void PrintRuntimeDebugSpheres() const;
	uint32_t GetRuntimeDebugSphereCount() const;
	bool AddSpriteTileLightHeuristic(uint32_t textureId, const float color[3], float intensity, float radius, uint32_t flickerFrames, uint32_t& outRuleId);
	void ClearSpriteTileLightHeuristics();
	void PrintSpriteTileLightHeuristics() const;
	bool AddTextureEmissiveHeuristic(uint32_t textureId, uint32_t emissiveMode, float intensityScale, const float* emissiveColor, bool hasExplicitColor, uint32_t& outRuleId);
	void ClearTextureEmissiveHeuristics();
	void PrintTextureEmissiveHeuristics() const;
	void NotifyGlowControlChange();
	void NotifyMaterialLightingCalibrationChange();
	void NotifyDebugSphereTessellationChange();
	void PrintEmissiveSurfaceDump(float radius, uint32_t limit) const;
	void PrintSectorLightDump(float radius, uint32_t limit) const;
	void PrintSurfaceProbeStatus() const;
	bool BuildEmissiveLightEditTarget(PathTracingEmissiveLightEditTarget& outTarget) const;
	bool BuildSurfaceLightEditTarget(PathTracingEmissiveLightEditTarget& outTarget) const;
	void PrintMapChunkDump(int32_t chunkIndex) const;
	void PrintMapChunkCompare(int32_t chunkIndex) const;
	void TraceActorSpriteEvent(const PathTracingActorSpriteTraceEvent& event);
	bool IsPathTracingSupported() const { return mPathTracingSupported; }
	bool RefreshPathTracingAvailability();
	const char* GetAvailabilityReason() const;
	const PerfShellTraceStats& GetLastPerfShellTraceStats() const { return mLastPerfShellTraceStats; }
	const PerfResourceTraceStats& GetLastPerfResourceTraceStats() const { return mLastPerfResourceTraceStats; }
	const PerfTraceShaderStats& GetLastPerfTraceShaderStats() const { return mLastPerfTraceShaderStats; }
	MemoryTelemetry GetMemoryTelemetry() const;
	static const char* GetMaterialBuildTraceSlotName(MaterialBuildTraceSlot slot);
private:
	enum class FrameTextureSlot : uint32_t
	{
		ViewZ,
		Motion,
		NormalRoughness,
		BaseColorMetalness,
		UnfilteredDiffuse,
		UnfilteredSpecular,
		UnfilteredPenumbra,
		DenoisedDiffuse,
		DenoisedSpecular,
		DenoisedShadow,
		Composed,
		TraceTransparentOutput,
		DirectLighting,
		DirectEmission,
		TaaHistoryPing,
		TaaHistoryPong,
		Validation,
		SrInput,
		RrInput,
		UpscalerDepth,
		RrGuideDiffuseAlbedo,
		RrGuideSpecularAlbedo,
		RrGuideSpecularHitDistance,
		RrGuideNormalRoughness,
		VendorOutput,
		PostSharpenOutput,
		Final,
		Count
	};

	enum class ExposureDomain : uint32_t
	{
		SceneHDR,
		PreExposedHDR,
		DisplayMappedOutput
	};

	struct ExposureRoute
	{
		ExposureDomain inputDomain = ExposureDomain::SceneHDR;
		float temporalExposure = 1.0f;
		float presentExposure = 1.0f;
	};

	enum class PipelineSlot : uint32_t
	{
		TraceOpaque,
		Composition,
		TraceTransparent,
		ExposureHistogramClear,
		ExposureHistogramBuild,
		ExposureResolve,
		Taa,
		RawPresent,
		FinalPresent,
		DlssSrBefore,
		DlssBefore,
		DlssAfter,
		Final,
		Count
	};

	struct CachedTexture
	{
		uint64_t key = 0;
		NRITextureResource resource;
	};

	struct CachedSkyTexture
	{
		uint64_t key = 0;
		nri_scene::PTSkyMode mode = nri_scene::PTSkyMode::None;
		NRITextureResource resource;
	};

	struct SkyState
	{
		nri_scene::PTSkyMode mode = nri_scene::PTSkyMode::None;
		nri_scene::PTSkySourceType sourceType = nri_scene::PTSkySourceType::None;
		FGameTexture* texture = nullptr;
		uint32_t faceMask = 0;
		float brightness = 1.0f;
		bool flipTop = false;
	};

	struct SelfTestRouteSnapshot
	{
		const char* routeName = "unknown";
		const char* presenterName = "unknown";
		const char* ownerName = "unknown";
		const char* passes = "unknown";
		bool denoiserRun = false;
		bool upscalerRun = false;
		bool exposureRun = false;
	};

	struct PreservedStaticMapSkyState
	{
		bool valid = false;
		uint64_t buildSerial = 0;
		nri_scene::SceneView sceneView;
	};

	using SceneBufferDebugStats = ::SceneBufferDebugStats;

	struct SelectPrimitiveRewriteCache
	{
		bool valid = false;
		uint64_t primitivePayloadHash = 0;
		uint64_t primitiveProvenanceHash = 0;
		uint64_t visibilityIdentityHash = 0;
		uint64_t primitiveCount = 0;
		std::vector<nri_scene::PrimitiveData> primitives;
	};

	struct StateCommitCombinedGeometryCache
	{
		bool staticPrefixValid = false;
		uint64_t staticBuildSerial = 0;
		uint32_t staticVertexCount = 0;
		uint32_t staticIndexCount = 0;
		uint32_t staticPrimitiveCount = 0;
		uint32_t staticPrimitiveProvenanceCount = 0;
		uint32_t staticMaterialCount = 0;
		nri_scene::GeometryData geometry;
	};

	struct SceneBufferUploadProducerStamp
	{
		uint64_t vertexPayloadStamp = 0;
		uint64_t indexPayloadStamp = 0;
		uint64_t primitivePayloadStamp = 0;
		uint64_t primitiveProvenanceStamp = 0;
		uint64_t materialPayloadStamp = 0;
	};

	using SceneUploadDirtyRange = ::SceneUploadDirtyRange;

	struct SceneBufferUploadDomainSpan
	{
		SceneBufferUploadDomain domain = SceneBufferUploadDomain::StaticOverlay;
		uint32_t vertexOffset = 0;
		uint32_t vertexCount = 0;
		uint32_t indexOffset = 0;
		uint32_t indexCount = 0;
		uint32_t primitiveOffset = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialOffset = 0;
		uint32_t materialCount = 0;
		SceneBufferUploadProducerStamp stamp = {};
	};

	struct SurfaceProbeResult
	{
		bool valid = false;
		bool hit = false;
		uint32_t sceneDataSource = UINT32_MAX;
		uint32_t sceneOwner = 0;
		uint32_t primitiveIndex = UINT32_MAX;
		uint32_t materialIndex = UINT32_MAX;
		uint32_t primitiveFlags = 0;
		uint32_t materialLightingFlags = 0;
		uint32_t textureId = 0;
		uint32_t baseTextureId = 0;
		uint32_t materialClass = 0;
		uint32_t emissiveMode = 0;
		uint32_t emissiveTextureIndex = UINT32_MAX;
		uint32_t normalTextureIndex = UINT32_MAX;
		uint32_t metallicTextureIndex = UINT32_MAX;
		uint32_t roughnessTextureIndex = UINT32_MAX;
		float lightLevel = 0.0f;
		float alpha = 1.0f;
		float metalnessHint = 0.0f;
		float roughnessHint = 0.45f;
		float averageColor[3] = { 1.0f, 1.0f, 1.0f };
		float emissiveColor[3] = {};
		float glowColor[3] = {};
		float distance = 0.0f;
		float position[3] = {};
		float normal[3] = {};
		nri_scene::SurfaceProvenance provenance = {};
	};

	struct SurfaceProbeEmissiveDiagnostics
	{
		bool sceneLightSurfaceMatch = false;
		bool activeEmissiveSurfaceMatch = false;
		bool exactEmissivePrimitiveMatch = false;
		uint32_t sceneLightMaterialIndex = UINT32_MAX;
		uint32_t emissivePrimitiveMatchCount = 0;
		uint32_t emissiveSourceFlags = 0;
		uint32_t emissiveSourceRuleId = 0;
		uint32_t emissiveOverrideRuleId = 0;
		int32_t emissiveSectorIndex = -1;
		float emissivePrimitiveArea = 0.0f;
		float emissivePowerEstimate = 0.0f;
		float emissiveSelectionWeight = 0.0f;
		float emissiveSelectionPdf = 0.0f;
		float emissiveIntensity = 0.0f;
		float sectorResponseScale = 1.0f;
		float sectorReachScale = 1.0f;
		bool sectorResponseApplied = false;
		bool materialResponseEnabled = false;
		float materialResponseScale = 1.0f;
	};

	struct SurfaceProbeFrameState
	{
		bool valid = false;
		bool usesStaticMapScene = false;
		uint32_t staticPrimitiveCount = 0;
		uint32_t runtimeSpaceLinkPrimitiveCount = 0;
		uint32_t runtimeMutationPrimitiveCount = 0;
		uint32_t dynamicPrimitiveCount = 0;
	};

	struct RuntimePointLightGpuData
	{
		float position[3] = {};
		float radius = 0.0f;
		float color[3] = { 1.0f, 1.0f, 1.0f };
		float intensity = 1.0f;
		uint32_t flags = 0;
		uint32_t reserved[3] = {};
	};

	struct RuntimeDebugSphere
	{
		uint32_t id = 0;
		float center[3] = {};
		float diameter = 0.0f;
		float metalness = 1.0f;
		float roughness = 0.05f;
		uint32_t cachedLongitudeSegments = 0;
		uint32_t cachedLatitudeSegments = 0;
		bool cacheValid = false;
		nri_scene::GeometryData geometry;
		nri_scene::MaterialBridgeData materialBridge;
	};

	struct TransientMuzzleFlashSlot
	{
		uint64_t stableKey = 0;
		uint32_t slotIndex = 0;
		uint32_t ruleId = 0;
		uint64_t sourceEventSerial = 0;
		int32_t emitterActorIndex = -1;
		float renderPosition[3] = {};
		float color[3] = { 1.0f, 1.0f, 1.0f };
		float peakIntensity = 0.0f;
		float radius = 0.0f;
		double activationTimeSeconds = 0.0;
		double endTimeSeconds = 0.0;
		bool occupied = false;
	};

	struct RuntimeLightTileHeaderGpuData
	{
		uint32_t indexOffset = 0;
		uint32_t indexCount = 0;
	};

	struct EmissivePrimitiveHeaderGpuData
	{
		uint32_t activeCount = 0;
		uint32_t dominantIndex = UINT32_MAX;
		uint32_t flags = 0;
		float totalPower = 0.0f;
	};

	struct EmissivePrimitiveGpuData
	{
		uint32_t dataSource = 0;
		uint32_t primitiveIndex = UINT32_MAX;
		uint32_t sourceFlags = 0;
		uint32_t textureId = 0;
		float primitiveArea = 0.0f;
		float powerEstimate = 0.0f;
		float selectionWeight = 0.0f;
		float selectionPdf = 0.0f;
		float emissionScale = 1.0f;
		uint32_t stableKeyLo = 0;
		uint32_t stableKeyHi = 0;
	};

	struct EmissiveMaterialResponseGpuData
	{
		uint32_t dataSource = 0;
		uint32_t primitiveIndex = UINT32_MAX;
		float materialScale = 1.0f;
		uint32_t flags = 0;
	};

	struct EmissivePrimitiveDebugRecord
	{
		uint64_t stableKey = 0;
		uint64_t surfaceStableKey = 0;
		uint32_t dataSource = 0;
		uint32_t primitiveIndex = UINT32_MAX;
		uint32_t materialIndex = UINT32_MAX;
		uint32_t sourceFlags = 0;
		uint32_t sourceRuleId = 0;
		uint32_t overrideRuleId = 0;
		uint32_t textureId = 0;
		uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_None;
		uint32_t emissiveTextureIndex = UINT32_MAX;
		int32_t actorIndex = -1;
		int32_t sectorIndex = -1;
		float center[3] = {};
		float primitiveArea = 0.0f;
		float powerEstimate = 0.0f;
		float selectionWeight = 0.0f;
		float selectionPdf = 0.0f;
		float emissiveColor[3] = {};
		float emissiveIntensity = 0.0f;
		float sectorResponseScale = 1.0f;
		float sectorReachScale = 1.0f;
		float materialResponseScale = 1.0f;
		bool materialResponseEnabled = false;
		bool sectorResponseApplied = false;
	};

	struct EmissiveSamplingBuildContext
	{
		const nri_scene::GeometryData* staticGeometry = nullptr;
		const nri_scene::GeometryData* capturedGeometry = nullptr;
		const nri_scene::GeometryData* runtimeMutationGeometry = nullptr;
		uint32_t runtimeMutationPrimitiveBaseOffset = 0;
		const nri_scene::GeometryData* dynamicGeometry = nullptr;
		uint32_t dynamicPrimitiveBaseOffset = 0;
	};

	struct SectorLightHeaderGpuData
	{
		uint32_t sectorCount = 0;
		uint32_t activeCount = 0;
		uint32_t pulsingCount = 0;
		uint32_t flags = 0;
	};

	struct SectorLightGpuData
	{
		float ambientColor[3] = {};
		float ambientIntensity = 0.0f;
		float hemisphereColor[3] = {};
		float hemisphereAmount = 0.0f;
		float fogAmount = 0.0f;
		float pulseScale = 1.0f;
		uint32_t sourceFlags = 0;
		int32_t paletteIndex = -1;
		int32_t lotag = 0;
		int32_t hitag = 0;
	};

	struct StaticMapSceneCache
	{
		struct ChunkCache
		{
			struct ResidentMaterialSliceCacheEntry
			{
				uint64_t animatedGeometrySignature = 0;
				uint64_t animatedMaterialSignature = 0;
				uint64_t materialBridgeHash = 0;
				uint64_t actorOverrideHash = 0;
				uint64_t emissiveOverrideHash = 0;
				uint32_t materialCount = 0;
				nri_scene::MaterialBridgeData remappedMaterialBridge;
				std::vector<nri_scene::MaterialData> gpuMaterials;
			};

			uint32_t chunkIndex = UINT32_MAX;
			uint32_t vertexOffset = 0;
			uint32_t vertexCount = 0;
			uint32_t indexOffset = 0;
			uint32_t indexCount = 0;
			uint32_t primitiveOffset = 0;
			uint32_t primitiveCount = 0;
			uint32_t materialOffset = 0;
			uint32_t materialCount = 0;
			uint64_t geometryTopologySignature = 0;
			uint64_t primitiveLayoutSignature = 0;
			uint64_t exactGeometrySignature = 0;
			uint64_t geometryPayloadHash = 0;
			uint64_t animatedMaterialSignature = 0;
			uint64_t animatedGeometrySignature = 0;
			bool active = true;
			bool blasUpdateEligible = false;
			uint32_t lastResidentBlasReasonMask = 0;
			uint32_t lastResidentBlasSurfaceCount = 0;
			uint32_t lastResidentBlasTriangleCount = 0;
			uint32_t lastResidentBlasMaterialCount = 0;
			bool lastResidentBlasForceTopology = false;
			bool lastResidentBlasRecoveredEmpty = false;
			bool lastResidentBlasKeptGeometrySlice = false;
			bool lastResidentBlasTopologyChanged = false;
			nri::AccelerationStructure* residentBlasScratchSizeCacheKey = nullptr;
			uint64_t residentBlasBuildScratchSize = 0;
			uint64_t residentBlasUpdateScratchSize = 0;
			bool hasAnimatedTextureCandidates = false;
			bool animatedRefreshSuppressed = false;
			nri_scene::MaterialBridgeData materialBridge;
			std::vector<ResidentMaterialSliceCacheEntry> residentMaterialSliceCache;
			NRIAccelerationStructureResource accelerationStructure;
		};

		bool valid = false;
		bool texturesResident = false;
		bool buffersResident = false;
		bool accelerationResident = false;
		uint64_t buildSerial = 0;
		uint32_t sceneBuildCount = 0;
		uint32_t gpuUploadCount = 0;
		uint32_t accelerationBuildCount = 0;
		uint32_t animatedCandidateChunkCount = 0;
		uint32_t animatedRefreshCount = 0;
		uint32_t animatedRefreshUploadCount = 0;
		uint32_t animatedGeometryFallbackCount = 0;
		uint32_t animatedRefreshSuppressedChunkCount = 0;
		uint32_t reuseCount = 0;
		nri_scene::SceneView sceneView;
		std::vector<nri_scene::SceneView> lightChunkViews;
		nri_scene::GeometryData geometry;
		nri_scene::MaterialBridgeData materialBridge;
		std::vector<nri_scene::MaterialData> gpuMaterials;
		std::vector<ChunkCache> chunks;
		uint32_t tlasInstanceCount = 0;
	};

	struct ResidentMapChunkRegistry
	{
		struct Entry
		{
			uint32_t chunkIndex = UINT32_MAX;
			uint32_t staticSceneChunkListIndex = UINT32_MAX;
			uint32_t vertexOffset = 0;
			uint32_t vertexCount = 0;
			uint32_t indexOffset = 0;
			uint32_t indexCount = 0;
			uint32_t primitiveOffset = 0;
			uint32_t primitiveCount = 0;
			uint32_t materialOffset = 0;
			uint32_t materialCount = 0;
			uint64_t geometryTopologySignature = 0;
			uint64_t baselineSignature = 0;
			uint64_t liveSignature = 0;
			uint64_t exactGeometrySignature = 0;
			uint64_t animatedMaterialSignature = 0;
			uint64_t materialPayloadHash = 0;
			uint64_t geometryPayloadHash = 0;
			uint64_t animatedGeometrySignature = 0;
			bool valid = false;
			bool active = false;
			bool mappedInStaticScene = false;
			bool accelerationResident = false;
			bool hasAnimatedTextureCandidates = false;
			bool animatedRefreshSuppressed = false;
			bool wasVisibleLastFrame = false;
			bool visibleValidationTraceEmitted = false;
			uint8_t visibleValidationFramesRemaining = 0;
			uint32_t animatedSuppressionEmitCount = 0;
			uint32_t runtimeAnimatedAttemptCount = 0;
			uint32_t runtimeAnimatedResidentApplyCount = 0;
			uint32_t runtimeAnimatedSyncSkipCount = 0;
			nri_scene::PTMapChunkMutationBaseline appliedBaseline;
		};

		bool valid = false;
		uint64_t buildSerial = 0;
		uint32_t chunkCount = 0;
		uint32_t activeChunkCount = 0;
		uint32_t mappedChunkCount = 0;
		uint32_t accelerationResidentChunkCount = 0;
		uint32_t animatedCandidateChunkCount = 0;
		uint32_t animatedRefreshSuppressedChunkCount = 0;
		std::vector<Entry> entries;
	};

	using StaticMapChunkAtlas = ::StaticMapChunkAtlas;

	struct DynamicSceneFrameState
	{
		uint32_t spriteSurfaceCount = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialCount = 0;
		uint32_t modelCount = 0;
		uint32_t unsupportedModelCount = 0;
		uint32_t mirrorExtendedSurfaceCount = 0;
		uint32_t mirrorExtendedPrimitiveCount = 0;
		uint32_t mirrorExtendedMaterialCount = 0;
		uint32_t mirrorExtendedModelCount = 0;
		uint32_t mirrorExtendedUnsupportedModelCount = 0;
		uint32_t mirrorPlayerSurfaceCount = 0;
		uint32_t mirrorPlayerPrimitiveCount = 0;
		uint32_t mirrorPlayerMaterialCount = 0;
		uint32_t mirrorPlayerModelCount = 0;
		uint32_t mirrorPlayerUnsupportedModelCount = 0;
		uint32_t asBuildCount = 0;
	};

	struct PersistentDynamicEmissiveCache
	{
		bool valid = false;
		uint32_t surfaceCount = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialCount = 0;
		nri_scene::SceneView sceneView;
		nri_scene::GeometryData geometry;
		nri_scene::MaterialBridgeData materialBridge;
	};

	struct PersistentVoxelBatch
	{
		struct ActorEntry
		{
			uint64_t identityKey = 0;
			uint64_t signature = 0;
			uint64_t geometrySignature = 0;
			uint64_t surfaceSignature = 0;
			uint64_t bakedSurfaceSignature = 0;
			uint64_t materialSignature = 0;
			uint64_t meshResourceKey = 0;
			uint64_t meshKeyHash = 0;
			uint64_t materialKeyHash = 0;
			uint64_t lastSeenFrame = 0;
			uint64_t retainedFrameAge = 0;
			int32_t sourcePicnum = -1;
			int32_t resolvedVoxelIndex = -1;
			uint32_t visibilityChunkIndex = UINT32_MAX;
			bool capturedThisFrame = false;
			bool inWorldTlasThisFrame = false;
			bool active = true;
			uint32_t primitiveOffset = 0;
			uint32_t primitiveCount = 0;
			uint32_t indexOffset = 0;
			uint32_t indexCount = 0;
			uint32_t materialOffset = 0;
			uint32_t materialCount = 0;
			std::array<float, 12> instanceTransform = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
			std::array<float, 12> previousInstanceTransform = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
			nri_scene::MaterialBridgeData materialBridge;
			std::vector<SceneLightSystem::SurfaceRecord> lightRecords;
		};

		bool valid = false;
		uint64_t sourceSerial = 0;
		uint32_t surfaceCount = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialCount = 0;
		uint32_t activeActorCount = 0;
		uint32_t rebuildCount = 0;
		nri_scene::SceneDebugStats stats;
		nri_scene::MaterialBridgeData materialBridge;
		std::vector<ActorEntry> actors;
	};

	struct StateCommitDomainGenerations
	{
		uint64_t staticMap = 0;
		uint64_t runtimeMutation = 0;
		uint64_t dynamicActors = 0;
		uint64_t mirrorPlayer = 0;
		uint64_t persistentVoxels = 0;
		uint64_t materialBridge = 0;
		uint64_t textures = 0;
		uint64_t tlasInstances = 0;
		uint64_t sceneConstants = 0;
	};

	struct PersistentVoxelMeshVariantResource
	{
		uint64_t resourceKey = 0;
		uint64_t meshKeyHash = 0;
		uint64_t transformBasisSignature = 0;
		nri_scene::VoxelMeshBakeSpace meshBakeSpace = nri_scene::VoxelMeshBakeSpace::Unknown;
		uint32_t primitiveCount = 0;
		uint32_t indexCount = 0;
		uint32_t vertexCount = 0;
		uint32_t vertexOffset = 0;
		uint32_t vertexCapacity = 0;
		uint32_t indexOffset = 0;
		uint32_t indexCapacity = 0;
		uint32_t primitiveOffset = 0;
		uint32_t primitiveCapacity = 0;
		uint32_t tlasReadyFrame = 0;
		uint32_t lastDesiredMapGeneration = 0;
		uint32_t lastUsedMapGeneration = 0;
		uint32_t lastUsedFrame = 0;
		uint32_t sourceBits = 0;
		uint32_t activeActorReferences = 0;
		int32_t priority = 0;
		uint64_t residentBytes = 0;
		bool tlasPublished = false;
		bool cold = false;
		bool gpuForce = false;
		bool gpuPrefer = false;
		bool lightTemplateValid = false;
		float lightTemplateCenter[3] = {};
		float lightTemplateBoundsRadius = 0.0f;
		float lightTemplateSurfaceArea = 0.0f;
		float bakedTranslation[3] = {};
		NRIBufferResource vertexBuffer;
		NRIBufferResource indexBuffer;
		NRIAccelerationStructureResource accelerationStructure;
	};

	struct PersistentVoxelMaterialVariantResource
	{
		uint64_t materialKeyHash = 0;
		uint64_t materialSignature = 0;
		uint64_t materialPayloadHash = 0;
		uint32_t materialOffset = 0;
		uint32_t materialCount = 0;
		uint32_t materialCapacity = 0;
		uint32_t lastDesiredMapGeneration = 0;
		uint32_t lastUsedMapGeneration = 0;
		uint32_t lastUsedFrame = 0;
		uint32_t sourceBits = 0;
		uint32_t activeActorReferences = 0;
		int32_t priority = 0;
		uint64_t residentBytes = 0;
		uint64_t materialUploadHash = 0;
		bool cold = false;
		bool gpuForce = false;
		bool gpuPrefer = false;
		nri_scene::MaterialBridgeData materialBridge;
	};

	enum class PersistentVoxelAdmissionState : uint8_t
	{
		Pending,
		UploadingVertices,
		UploadingIndices,
		UploadingPrimitives,
		BuildingBlas,
		Ready,
		Deferred,
		Failed,
	};

	struct PersistentVoxelAdmissionEntry
	{
		uint64_t pairKey = 0;
		nri_scene::PrecachedVoxelVariantView variant;
		PersistentVoxelAdmissionState state = PersistentVoxelAdmissionState::Pending;
		uint32_t sourceBits = 0;
		int32_t priority = 0;
		int32_t admissionRank = 0;
		bool gpuForce = false;
		bool gpuPrefer = false;
		bool runtimeRequested = false;
		uint32_t retryCount = 0;
		uint32_t mapGeneration = 0;
		uint64_t estimatedBytes = 0;
		uint64_t bytesUploaded = 0;
		bool uploadPrepared = false;
		uint32_t shaderVertexOffset = 0;
		uint32_t shaderIndexOffset = 0;
		uint32_t shaderPrimitiveOffset = 0;
		uint32_t savedVertexCursor = 0;
		uint32_t savedIndexCursor = 0;
		uint32_t savedPrimitiveCursor = 0;
		uint32_t savedMaterialCursor = 0;
		uint64_t vertexBytesUploaded = 0;
		uint64_t vertexArenaBytesUploaded = 0;
		uint64_t indexBytesUploaded = 0;
		uint64_t indexArenaBytesUploaded = 0;
		uint64_t primitiveBytesUploaded = 0;
		bool uploadSubmittedBeforeBlas = false;
		nri_scene::GeometryData uploadGeometry;
		std::vector<uint32_t> uploadGpuIndices;
		std::vector<nri_scene::PrimitiveData> uploadGpuPrimitives;
		PersistentVoxelMeshVariantResource uploadMeshResource;
		PersistentVoxelMaterialVariantResource uploadMaterialResource;
		const char* lastReason = "none";
	};

	struct PersistentVoxelReadinessStatus
	{
		const char* reason = "ready";
		bool ready = false;
		bool meshPresent = false;
		bool meshPublished = false;
		bool meshKeyMatches = false;
		bool meshCountsValid = false;
		bool meshPrivateBuffersReady = false;
		bool meshArenaBuffersReady = false;
		bool blasReady = false;
		bool materialPresent = false;
		bool materialPublished = false;
		bool materialKeyMatches = false;
		bool materialCountValid = false;
		bool materialBridgeReady = false;
		uint64_t meshResourceKey = 0;
		uint32_t meshVertexCount = 0;
		uint32_t meshIndexCount = 0;
		uint32_t meshPrimitiveCount = 0;
		uint32_t materialCount = 0;
		uint32_t materialBridgeCount = 0;
	};

	struct PersistentVoxelAdmissionStats
	{
		uint32_t queued = 0;
		uint32_t ready = 0;
		uint32_t deferred = 0;
		uint32_t failed = 0;
		uint32_t enqueued = 0;
		uint32_t deduped = 0;
		uint32_t promoted = 0;
		uint32_t uploaded = 0;
		uint32_t force = 0;
		uint32_t prefer = 0;
		uint32_t runtime = 0;
		uint32_t skippedBudget = 0;
		uint32_t failedThisPump = 0;
		uint64_t bytesPending = 0;
		uint64_t bytesUploaded = 0;
	};

	struct PersistentVoxelInstanceRecord
	{
		uint64_t identityKey = 0;
		uint64_t signature = 0;
		uint64_t geometrySignature = 0;
		uint64_t surfaceSignature = 0;
		uint64_t bakedSurfaceSignature = 0;
		uint64_t materialSignature = 0;
		uint64_t meshKeyHash = 0;
		uint64_t materialKeyHash = 0;
		uint64_t meshVariantHash = 0;
		uint64_t materialVariantHash = 0;
		uint64_t meshResourceKey = 0;
		uint32_t primitiveCount = 0;
		uint32_t lastSeenFrame = 0;
		bool active = false;
		bool pending = false;
		std::array<float, 12> currentTransform = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
		std::array<float, 12> previousTransform = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
	};

	struct ActorSpriteDebugStats
	{
		uint32_t lastPruneChecks = 0;
		uint32_t lastPruneMatches = 0;
		uint32_t lastPruneDroppedMissingActor = 0;
		uint32_t lastPruneDroppedMissingActorIndex = 0;
		uint32_t lastPruneDroppedNullLiveTexture = 0;
		uint32_t lastPruneDroppedTextureMismatch = 0;
		uint32_t lastPruneDroppedPaletteMismatch = 0;
	};

	struct PersistentDynamicSurfaceStats
	{
		uint32_t actorSurfaceCount = 0;
		uint32_t nonActorSurfaceCount = 0;
		uint32_t wallSurfaceCount = 0;
		uint32_t flatSurfaceCount = 0;
		uint32_t spriteSurfaceCount = 0;
		uint32_t actorFacingSpriteCount = 0;
		uint32_t actorVoxelSpriteCount = 0;
	};

	struct RuntimeMutationCacheStats
	{
		uint32_t activeChunkCount = 0;
		uint32_t validChunkCount = 0;
		uint32_t excludedStaticChunkCount = 0;
		uint32_t cachedSurfaceCount = 0;
		uint32_t cachedTriangleCount = 0;
		uint32_t cachedMaterialCount = 0;
		uint32_t cachedMaterialStateCount = 0;
	};

	struct ActorMaterialOverrideCache
	{
		bool valid = false;
		uint32_t frameIndex = UINT32_MAX;
		uint32_t resolvedGeneration = 0;
		bool hasFullbrightOverrides = false;
		std::unordered_map<int32_t, uint32_t> overrides;
	};

	struct SceneTextureOverflowDebugStats
	{
		uint32_t textureCountLastBuild = 0;
		uint32_t truncatedTextureCountLastBuild = 0;
		uint32_t baseTextureClampCountLastBuild = 0;
		uint32_t normalTextureClampCountLastBuild = 0;
		uint32_t metallicTextureClampCountLastBuild = 0;
		uint32_t roughnessTextureClampCountLastBuild = 0;
		uint32_t emissiveTextureClampCountLastBuild = 0;
		uint64_t totalOverflowBuilds = 0;
		bool warningLogged = false;
	};

	struct SceneTextureCacheDebugStats
	{
		uint32_t cacheEntriesLastBuild = 0;
		uint32_t cacheEntriesHighWater = 0;
		uint32_t lookupMissesLastBuild = 0;
		uint32_t insertCountLastBuild = 0;
		uint32_t transitionCountLastFrame = 0;
		double lookupMsLastBuild = 0.0;
		double realizeMsLastBuild = 0.0;
		double descriptorMsLastBuild = 0.0;
		double transitionMsLastFrame = 0.0;
	};

	struct DescriptorCoherencyDebugStats
	{
		uint64_t actorMaterialBuilds = 0;
		uint64_t sceneTextureSetUpdates = 0;
		uint64_t sceneDataSetUpdates = 0;
		uint64_t lastMaterialBridgeHash = 0;
		uint64_t lastActorSpriteMaterialHash = 0;
		uint64_t lastSceneTextureDescriptorHash = 0;
		uint64_t lastSceneDataDescriptorHash = 0;
		uint32_t lastMaterialCount = 0;
		uint32_t lastTextureCount = 0;
		uint32_t lastActorSpriteSurfaceCount = 0;
		uint32_t lastActorSpriteActorCount = 0;
		uint32_t lastSceneTextureDescriptorCount = 0;
		uint32_t lastSceneDataDescriptorCount = 0;
		uint32_t lastSceneTextureQueuedFrameIndex = 0;
		uint32_t lastSceneDataQueuedFrameIndex = 0;
		uint32_t lastSceneTextureOutstandingQueuedFrames = 0;
		uint32_t lastSceneDataOutstandingQueuedFrames = 0;
		uint64_t lastSceneTextureQueuedFrameFence = 0;
		uint64_t lastSceneDataQueuedFrameFence = 0;
		uint64_t lastSceneTextureSubmittedFence = 0;
		uint64_t lastSceneDataSubmittedFence = 0;
		std::string lastMaterialBuildLabel;
		std::string lastSceneTextureReason;
		std::string lastSceneDataReason;
	};

	struct RuntimeMapMutationCache
	{
		struct ChunkReplacement
		{
			struct MaterialStateCacheEntry
			{
				uint64_t animatedGeometrySignature = 0;
				uint64_t animatedMaterialSignature = 0;
				uint32_t surfaceCount = 0;
				nri_scene::MaterialBridgeData materialBridge;
			};

			nri_scene::PTMapChunkMutationBaseline baseline;
			nri_scene::PTMapChunkMutationBaseline replacementBaseline;
			uint64_t baselineSignature = 0;
			uint64_t liveSignature = 0;
			uint64_t animatedMaterialSignature = 0;
			uint64_t lastTraceSignature = UINT64_MAX;
			uint64_t lastTraceAnimatedMaterialSignature = UINT64_MAX;
			uint32_t reasonMask = 0;
			uint32_t sectionDirtyCount = 0;
			uint32_t stableMutationFrameCount = 0;
			uint32_t lastTraceReasonMask = UINT32_MAX;
			uint32_t traceCount = 0;
			bool active = false;
			bool valid = false;
			bool residentAuthoritative = false;
			bool sectorDirty = false;
			bool dragged = false;
			bool blindSpot = false;
			bool excludeStaticChunk = false;
			bool staticAnimatedReplacement = false;
			bool lastTraceActive = false;
			bool lastTraceBlindSpot = false;
			bool animationOnlyRefreshed = false;
			bool lastTraceAnimationOnlyRefreshed = false;
			bool lastTraceStaticAnimatedReplacement = false;
			bool deferredMaterialRefresh = false;
			uint64_t deferredMaterialFrame = 0;
			bool deferredStructuralRebuild = false;
			uint64_t deferredStructuralFrame = 0;
			uint32_t surfaceCount = 0;
			uint32_t triangleCount = 0;
			SceneLightSystem::SurfaceIdentityOverrides lightIdentityOverrides;
			nri_scene::SceneView sceneView;
			nri_scene::GeometryData geometry;
			nri_scene::MaterialBridgeData materialBridge;
			std::vector<MaterialStateCacheEntry> materialStateCache;
		};

		std::vector<ChunkReplacement> chunks;
	};

	struct RuntimeMutationResidentApplyMode
	{
		bool materialOnlyReplacement = false;
		bool exclusiveMaterialOnlyReplacement = false;
		bool fastResidentMaterialOnlyUpdate = false;
	};

	struct ResidentBufferUploadScratch
	{
		NRIBufferResource buffer;
		uint64_t cursor = 0;
		bool copySourceActive = false;
	};

	struct ResidentUploadScratchFrame
	{
		uint64_t frameIndex = UINT64_MAX;
		ResidentBufferUploadScratch vertex;
		ResidentBufferUploadScratch index;
		ResidentBufferUploadScratch primitive;
		ResidentBufferUploadScratch material;
		std::vector<NRIBufferResource> retiredBuffers;
		std::vector<NRIAccelerationStructureResource> retiredAccelerationStructures;
	};

	struct RuntimeMutationResidentUploadRange
	{
		int uploadKind = 0;
		uint64_t byteOffset = 0;
		uint64_t size = 0;
		uint64_t dirtySize = 0;
	};

	struct RuntimeMapMutationFrameState
	{
		bool active = false;
		uint32_t dirtyChunkCount = 0;
		uint32_t residentAppliedChunkCount = 0;
		uint32_t residentGeometryChunkCount = 0;
		uint32_t residentMaterialChunkCount = 0;
		uint32_t residentAtlasGrowCount = 0;
		uint32_t residentFallbackChunkCount = 0;
		uint32_t rebuiltChunkCount = 0;
		uint32_t heldChunkCount = 0;
		uint32_t blindSpotChunkCount = 0;
		uint32_t sectorGeometryChunkCount = 0;
		uint32_t sectorMaterialChunkCount = 0;
		uint32_t wallGeometryChunkCount = 0;
		uint32_t wallMaterialChunkCount = 0;
		uint32_t sectorDirtyChunkCount = 0;
		uint32_t sectionDirtyChunkCount = 0;
		uint32_t draggedChunkCount = 0;
		uint32_t animatedRefreshChunkCount = 0;
		uint32_t replacementSurfaceCount = 0;
		uint32_t replacementTriangleCount = 0;
		uint32_t materialCount = 0;
	};

	struct RuntimeSpaceLinkFrameState
	{
		bool active = false;
		bool geoEffectActive = false;
		bool topologyChanged = false;
		bool queryAttempted = false;
		bool queryRejected = false;
		int32_t candidateSectorIndex = -1;
		int32_t candidateSectorLotag = -1;
		int32_t sourceSectorIndex = -1;
		int32_t reportedGeoCount = 0;
		uint32_t viewRootSectorCount = 0;
		uint32_t visibleSectorCount = 0;
		uint32_t providerSectorCount = 0;
		uint32_t geoProviderCount = 0;
		uint32_t providerGroupCount = 0;
		uint32_t localSpaceMatchedProviderCount = 0;
		uint32_t visibleMatchedProviderCount = 0;
		uint32_t linkCount = 0;
		uint32_t translatedChunkCount = 0;
		uint32_t orphanLocalSpaceCount = 0;
		uint32_t unresolvedRuntimePortalCount = 0;
		uint32_t surfaceCount = 0;
		uint32_t triangleCount = 0;
		uint32_t materialCount = 0;
	};

	struct RuntimeChunkTranslationState
	{
		uint32_t chunkIndex = UINT32_MAX;
		float dx = 0.0f;
		float dz = 0.0f;
	};

	struct RuntimeLinkTraceState
	{
		bool valid = false;
		int32_t candidateSectorIndex = -1;
		int32_t sourceSectorIndex = -1;
		bool geoEffectActive = false;
		uint32_t visibleTaggedSectorCount = 0;
		uint32_t visible848SectorCount = 0;
		uint32_t visibleTeleportSectorCount = 0;
		uint32_t taggedVisibleSectorStoredCount = 0;
		std::array<RuntimeTaggedSectorDebugInfo, 8> taggedVisibleSectors = {};
		uint32_t nearbyControlSectorStoredCount = 0;
		std::array<RuntimeTaggedSectorDebugInfo, 12> nearbyControlSectors = {};
		RuntimeLinkDebugState game = {};
	};

	using SceneInstanceData = ::SceneInstanceData;

	using StaticMapSceneResources = ::StaticMapSceneResources;

	using SceneUploadBufferRingSlot = ::SceneUploadBufferRingSlot;

	enum SceneDataBufferMask : uint32_t
	{
		SceneDataBufferMask_None = 0,
		SceneDataBufferMask_Static = 1 << 0,
		SceneDataBufferMask_Dynamic = 1 << 1,
	};

	bool CreatePipelineLayout();
	bool CreateTaaPipelineLayout();
	bool CreatePresentPipelineLayout();
	bool CreateExposurePipelineLayout();
	bool CreatePipelines();
	bool AllocateDescriptorSets();
	bool EnsureFrameResources(uint32_t outputWidth, uint32_t outputHeight, uint32_t targetWidth, uint32_t targetHeight);
	bool DispatchBootstrapView();
	bool UseFallbackSceneTextures(bool preserveExistingSky, const char* reason = nullptr);
	bool EnsurePaletteTexture(const nri_scene::MaterialBridgeData& materials);
	uint32_t FindSceneTextureCacheIndex(uint64_t key) const;
	bool EnsureSceneTextureCacheEntry(const nri_scene::TextureUpload& upload, double* outRealizeMs = nullptr);
	bool EnsureSceneTextures(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& outGpuMaterials, bool preserveExistingSky, const char* reason = nullptr);
	bool EnsureSkyTexture(const nri_scene::SceneView& sceneView, bool preserveExistingSky);
	bool EnsureStaticMapScene();
	bool BuildStaticMapSceneCache(
		const nri_scene::PTMapWorld& mapWorld,
		const PreservedStaticMapSkyState* preservedSkyState,
		StaticMapSceneCache& outStaticScene,
		RuntimeMapMutationCache& outRuntimeMutations);
	void InitializeStaticMapSceneCacheBuild(
		const nri_scene::PTMapWorld& mapWorld,
		const PreservedStaticMapSkyState* preservedSkyState,
		StaticMapSceneCache& outStaticScene,
		RuntimeMapMutationCache& outRuntimeMutations);
	void AppendStaticMapSceneCacheChunk(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& chunk,
		const nri_scene::SceneView* preservedSkyView,
		StaticMapSceneCache& outStaticScene,
		RuntimeMapMutationCache& outRuntimeMutations);
	void ResetResidentMapChunkRegistry();
	void SyncResidentMapChunkRegistryFromStaticScene();
	uint32_t GetStaticSceneChunkSlotPreference(uint32_t chunkListIndex) const;
	uint32_t FindPreferredStaticSceneChunkListIndex(uint32_t chunkIndex) const;
	uint32_t CountStaticSceneChunkSlots(uint32_t chunkIndex) const;
	void ResetStaticMapChunkAtlas(StaticMapChunkAtlas& atlas) const;
	uint32_t GetChunkAtlasCapacity(uint32_t usedCount) const;
	uint32_t AllocateChunkAtlasSlice(uint32_t count, uint32_t alignment, uint32_t& cursor) const;
	uint32_t AllocateChunkAtlasRange(uint32_t count, uint32_t capacity, std::vector<StaticMapChunkAtlas::FreeRange>& freeRanges, uint32_t& cursor) const;
	void ReleaseChunkAtlasRange(std::vector<StaticMapChunkAtlas::FreeRange>& freeRanges, uint32_t offset, uint32_t count) const;
	bool BuildStaticMapChunkAtlasLayout(const StaticMapSceneCache& staticScene, StaticMapChunkAtlas& outAtlas) const;
	bool EnsureResidentStaticMapChunkAtlasBufferCapacity(const StaticMapChunkAtlas& atlas);
	bool RebuildResidentStaticCpuAtlasMirror(StaticMapSceneCache& staticScene, const StaticMapChunkAtlas& atlas) const;
	bool RebuildResidentStaticMaterialBridgeFromChunks();
	bool RefreshResidentStaticMaterialSlices(
		const std::vector<uint32_t>& chunkListIndices,
		const char* reason,
		const std::vector<uint32_t>* animatedApplyChunkListIndices = nullptr);
	void UploadChunkVertexDataToAtlas(
		const nri_scene::GeometryData& sourceGeometry,
		const StaticMapSceneCache::ChunkCache& sourceChunk,
		const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
		std::vector<nri_scene::SceneVertex>& outVertices) const;
	void UploadChunkIndexDataToAtlas(
		const nri_scene::GeometryData& sourceGeometry,
		const StaticMapSceneCache::ChunkCache& sourceChunk,
		const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
		std::vector<uint32_t>& outIndices) const;
	void UploadChunkVertexAndIndexDataToAtlas(
		const nri_scene::GeometryData& sourceGeometry,
		const StaticMapSceneCache::ChunkCache& sourceChunk,
		const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
		std::vector<nri_scene::SceneVertex>& outVertices,
		std::vector<uint32_t>& outIndices) const;
	void UploadChunkPrimitiveDataToAtlas(
		const nri_scene::GeometryData& sourceGeometry,
		const StaticMapSceneCache::ChunkCache& sourceChunk,
		const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
		std::vector<nri_scene::PrimitiveData>& outPrimitives) const;
	void UploadChunkGeometryToAtlas(
		const nri_scene::GeometryData& sourceGeometry,
		const StaticMapSceneCache::ChunkCache& sourceChunk,
		const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
		std::vector<nri_scene::SceneVertex>& outVertices,
		std::vector<uint32_t>& outIndices,
		std::vector<nri_scene::PrimitiveData>& outPrimitives) const;
	void UploadChunkMaterialsToAtlas(
		const std::vector<nri_scene::MaterialData>& sourceMaterials,
		const StaticMapSceneCache::ChunkCache& sourceChunk,
		const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
		std::vector<nri_scene::MaterialData>& outMaterials) const;
	bool UploadStaticMapChunkAtlas(
		NRIBufferResource& vertexBuffer,
		NRIBufferResource& indexBuffer,
		NRIBufferResource& primitiveBuffer,
		NRIBufferResource& materialBuffer,
		StaticMapChunkAtlas& atlas,
		const StaticMapSceneCache& staticScene,
		const std::vector<nri_scene::MaterialData>& gpuMaterials);
	bool UploadStaticMapChunkMaterialAtlas(
		NRIBufferResource& materialBuffer,
		const StaticMapChunkAtlas& atlas,
		const StaticMapSceneCache& staticScene,
		const std::vector<nri_scene::MaterialData>& gpuMaterials);
	bool RefreshStaticMapAnimatedMaterials();
	bool UploadSceneBuffers(
		const nri_scene::GeometryData& geometry,
		const std::vector<nri_scene::MaterialData>& materials,
		const std::vector<SceneBufferUploadDomainSpan>* domainSpans = nullptr);
	bool UploadSceneBuffers(
		SceneUploadBufferRingSlot& uploadSlot,
		const nri_scene::GeometryData& geometry,
		const std::vector<nri_scene::MaterialData>& materials,
		const std::vector<SceneBufferUploadDomainSpan>* domainSpans = nullptr);
	bool BuildStaticMapAccelerationStructures();
	bool BuildStaticMapAccelerationStructures(
		StaticMapSceneCache& staticScene,
		StaticMapSceneResources& staticResources,
		bool updateLiveState);
	bool BuildTopLevelAccelerationStructure(const std::vector<nri::TopLevelInstance>& instances, uint32_t sceneBufferMask);
	bool BuildTopLevelAccelerationStructure(
		const std::vector<nri::TopLevelInstance>& instances,
		uint32_t sceneBufferMask,
		NRIAccelerationStructureResource& topLevelAS,
		NRIBufferResource& tlasInstanceBuffer,
		NRIBufferResource& topLevelScratchBuffer,
		const NRIBufferResource* staticVertexBuffer,
		const NRIBufferResource* staticIndexBuffer,
		uint32_t* outTlasInstanceCount,
		bool updateLiveState,
		bool tlasInstanceWritesQuiesced);
	bool BuildEmissiveTopLevelAccelerationStructure();
	bool BuildDynamicAccelerationStructure(const nri_scene::GeometryData& geometry);
	bool BuildDynamicAccelerationStructure(
		const nri_scene::GeometryData& geometry,
		uint32_t indexOffset,
		uint32_t indexCount,
		uint32_t primitiveCount,
		NRIAccelerationStructureResource& outAccelerationStructure,
		bool updateDynamicPerfStats);
	bool BuildBottomLevelAccelerationStructure(
		const NRIBufferResource& vertexBuffer,
		const NRIBufferResource& indexBuffer,
		uint32_t vertexCount,
		uint32_t indexOffset,
		uint32_t indexCount,
		uint32_t primitiveCount,
		NRIAccelerationStructureResource& outAccelerationStructure,
		bool updateDynamicPerfStats);
	bool PreloadStaticMapResources();
	bool PreloadPersistentVoxelResources();
	bool SyncPersistentVoxelResidencyMapGeneration(const char* reason);
	void ReconcilePersistentVoxelResidency(
		const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
		const std::vector<nri_scene::PersistentVoxelCacheEntryView>& cacheEntries);
	bool PreloadPersistentVoxelVariantResources(const std::vector<nri_scene::PrecachedVoxelVariantView>& variants);
	bool EnqueuePersistentVoxelAdmission(
		const nri_scene::PrecachedVoxelVariantView& variant,
		bool runtimeRequested,
		const char* sourceLabel);
	void DiscardPersistentVoxelAdmissionEntry(PersistentVoxelAdmissionEntry& entry);
	bool IsRequiredPersistentVoxelAdmission(const PersistentVoxelAdmissionEntry& entry) const;
	void CountPersistentVoxelAdmissionWork(uint32_t& requiredPending, uint32_t& requiredReady, uint32_t& optionalPending, uint32_t& failed) const;
	void ApplyPersistentVoxelResidencyPressurePolicy(const char* phase);
	bool PumpPersistentVoxelAdmissionQueue(const char* phase);
	PersistentVoxelReadinessStatus GetPersistentVoxelSharedVariantReadiness(uint64_t meshResourceKey, uint64_t materialKeyHash) const;
	void TracePersistentVoxelReadiness(
		const char* event,
		const char* phase,
		const PersistentVoxelAdmissionEntry* entry,
		uint64_t meshResourceKey,
		uint64_t materialKeyHash,
		const PersistentVoxelReadinessStatus& status) const;
	bool AdmitPersistentVoxelVariantResource(
		PersistentVoxelAdmissionEntry& entry,
		uint64_t byteBudget,
		uint32_t& blasBudget,
		uint64_t& outUploadBytes,
		bool& outReusedMesh,
		bool& outReusedMaterial,
		bool& outInProgress,
		bool isolateBlasBuild,
		const char*& outFailureReason);
	bool IsPersistentVoxelSharedVariantReady(uint64_t meshResourceKey, uint64_t materialKeyHash) const;
	bool PreloadMaterialResources();
	bool EnsurePersistentVoxelBatch();
	void ResetPersistentVoxelBatch(const char* reason = "batch-reset", bool clearSharedResources = true);
	bool BuildPersistentVoxelVariantAccelerationStructures(const nri_scene::GeometryData& geometry);
	bool UploadPersistentVoxelArenaMaterialBuffers(const std::vector<nri_scene::MaterialData>& materials);
	void InvalidateRuntimeLightSceneData();
	bool RefreshResidentStaticSceneDataSet();
	bool BuildRuntimeMapMutationOverlay(nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials, bool* outResidentStaticSceneChanged = nullptr);
	bool TryApplyRuntimeMutationChunkToResidentScene(
		const nri_scene::PTMapChunk& mapChunk,
		RuntimeMapMutationCache::ChunkReplacement& replacement,
		uint32_t& outStaticSceneChunkListIndex,
		bool& outMaterialDirty,
		bool& outGeometryDirty,
		uint32_t& outSurfaceCount,
		uint32_t& outTriangleCount,
		uint32_t& outMaterialCount,
		bool& outRecoveredEmpty);
	RuntimeMutationResidentApplyMode ClassifyRuntimeMutationResidentApplyMode(
		const RuntimeMapMutationCache::ChunkReplacement& replacement,
		bool hasResidentChunk,
		uint32_t resolvedChunkListIndex) const;
	void RecordRuntimeMutationResidentApplyMode(
		const RuntimeMutationResidentApplyMode& mode,
		const RuntimeMapMutationCache::ChunkReplacement& replacement,
		bool hasResidentChunk,
		uint32_t resolvedChunkListIndex);
	bool RebuildResidentStaticMaterialState(const char* reason);
	bool RebuildResidentStaticMapChunkBlases(const std::vector<uint32_t>& chunkListIndices);
	bool BuildRuntimeSpaceLinkOverlay(HWDrawInfo& di, nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials);
	void BuildRuntimePointLightUpload(std::vector<RuntimePointLightGpuData>& outLights) const;
	uint64_t BuildRuntimeLightPayloadHash() const;
	uint64_t BuildRuntimeLightClusterCameraHash() const;
	uint64_t BuildEmissiveSamplingPayloadHash(const EmissiveSamplingBuildContext& context) const;
	uint64_t BuildEmissiveSectorResponsePayloadHash() const;
	uint64_t BuildSectorLightingPayloadHash() const;
	void TraceEmissiveSectorResponseChange();
	void NotifyEmissiveSectorResponseEditModeChanges();
	void BuildEmissiveSamplingUpload(
		const EmissiveSamplingBuildContext& context,
		EmissivePrimitiveHeaderGpuData& outHeader,
		std::vector<EmissivePrimitiveGpuData>& outPrimitives,
		std::vector<float>& outCdf,
		std::vector<EmissiveMaterialResponseGpuData>& outMaterialResponses,
		std::vector<EmissivePrimitiveDebugRecord>& outDebugRecords) const;
	bool UpdateEmissiveSamplingBuffers(const EmissiveSamplingBuildContext& context, bool* ioWaitedForWrites = nullptr);
	void UpdateBoundSectorLightingState();
	void BuildSectorLightingUpload(
		SectorLightHeaderGpuData& outHeader,
		std::vector<SectorLightGpuData>& outSectors);
	bool UpdateReprojectionBuffer(bool* ioWaitedForWrites = nullptr);
	bool UpdateVisibleChunkBuffer(bool* ioWaitedForWrites = nullptr);
	bool UpdateVisibleFlatPlaneBuffer(bool* ioWaitedForWrites = nullptr);
	bool UpdateSceneDataSet(
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
		const char* reason = nullptr);
	bool CommitSceneDataDescriptors(const char* reason);
	void TraceActorSpriteMaterialAssignments(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& outMaterials, const char* traceLabel);
	void TraceSharedDescriptorRewrite(const char* setName, const char* reason, uint64_t descriptorHash, uint32_t descriptorCount, bool sceneTextureSet);
	uint32_t CountPotentialOutstandingQueuedFrames() const;
	uint32_t GetCurrentQueuedFrameIndex() const;
	SceneUploadBufferRingSlot& GetCurrentSceneUploadBufferRingSlot();
	const SceneUploadBufferRingSlot* GetCurrentSceneUploadBufferRingSlot() const;
	NRIBufferResource& GetCurrentDynamicVertexBuffer();
	NRIBufferResource& GetCurrentDynamicIndexBuffer();
	NRIBufferResource& GetCurrentDynamicPrimitiveBuffer();
	NRIBufferResource& GetCurrentDynamicMaterialBuffer();
	NRIAccelerationStructureResource& GetCurrentDynamicBottomLevelAS();
	NRIBufferResource& GetCurrentTlasInstanceBuffer();
	const NRIBufferResource& GetCurrentDynamicVertexBuffer() const;
	const NRIBufferResource& GetCurrentDynamicIndexBuffer() const;
	const NRIBufferResource& GetCurrentDynamicPrimitiveBuffer() const;
	const NRIBufferResource& GetCurrentDynamicMaterialBuffer() const;
	const NRIAccelerationStructureResource* GetCurrentDynamicBottomLevelAS() const;
	const NRIBufferResource& GetCurrentTlasInstanceBuffer() const;
	bool HasAnyDynamicBottomLevelAS() const;
	void DestroyDynamicBottomLevelAccelerationStructures();
	ResidentUploadScratchFrame& GetResidentUploadScratchFrame();
	nri::DescriptorSet* GetCurrentSceneTextureSet() const;
	nri::DescriptorSet* GetCurrentSceneDataSet() const;
	bool IsCurrentSceneDataDescriptorsInitialized() const;
	void SetCurrentSceneDataDescriptorsInitialized(bool value);
	void BuildStaticMapInstances(std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances) const;
	void BuildStaticMapInstances(const StaticMapSceneCache& staticScene, std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances) const;
	void BuildStaticMapInstances(const StaticMapSceneCache& staticScene, const StaticMapChunkAtlas& atlas, std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances) const;
	bool RestoreStaticTopLevelScene();
	bool DispatchFrameGraph(HWDrawInfo& di, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials, int drawmode);
	bool DispatchTraceOpaque(HWDrawInfo& di, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials);
	bool DispatchDenoiser();
	bool DispatchComposition(FrameTextureSlot outputSlot = FrameTextureSlot::Composed);
	bool DispatchTraceTransparent();
	bool DispatchUpscalerPrepass(NRIMainUpscalerKind mainKind);
	bool DispatchRawPresent(FrameTextureSlot inputSlot, FrameTextureSlot secondarySlot = FrameTextureSlot::Count, FrameTextureSlot tertiarySlot = FrameTextureSlot::Count);
	bool DispatchFinalPresent(FrameTextureSlot inputSlot);
	bool DispatchUpscaleChain();
	bool DispatchFinal();
	void RefreshMapWorld();
	bool ApplyStartupMapWorldCorrectionIfNeeded(const char* trigger);
	void RebuildStartupMutationBaseline();
	bool CheckPathTracingSupport();
	void UpdatePerFrameState(HWDrawInfo& di);
	void UpdateNightVisionState();
	void ResetSceneBufferFrameStats();
	void LogBridgeStats(const nri_scene::SceneDebugStats& stats);
	void PrintMapWorldStatus() const;
	void PrintPortalTraversalStatus() const;
	void PrintStaticMapSceneStatus() const;
	void PrintResidentMapChunkRegistryStatus() const;
	void PrintDynamicSceneStatus() const;
	void PrintTemporalStatus() const;
	void PrintRuntimeMapMutationStatus() const;
	void PrintRuntimeSpaceLinkStatus() const;
	void RequestHistoryReset(const char* reason, bool clearPreviousCameraState = false, bool clearRuntimeChunkTranslationHistory = false);
	void NoteLightHistoryChange(const char* reason);
	void ArmTemporalTraceBudget(const char* reason);
	void TraceTemporalState(const char* stage, NRIMainUpscalerKind resolvedMainUpscaler, NRIPostSharpenKind resolvedPostSharpen, bool runAppTaa, FrameTextureSlot primarySlot, FrameTextureSlot secondarySlot) const;
	ExposureDomain ResolveFrameTextureExposureDomain(FrameTextureSlot slot, NRIMainUpscalerKind mainKind, NRIPostSharpenKind postSharpenKind) const;
	ExposureRoute ResolveExposureRoute(FrameTextureSlot inputSlot, const NRIPTOutputPolicy& outputPolicy, NRIMainUpscalerKind mainKind, NRIPostSharpenKind postSharpenKind) const;
	const char* GetExposureDomainName(ExposureDomain domain) const;
	void ResetSelfTestRouteSnapshot();
	void SetSelfTestRouteSnapshot(const char* routeName, const char* presenterName, const char* ownerName, const char* passes, bool denoiserRun, bool upscalerRun, bool exposureRun);
	void EmitSelfTestSummary(uint32_t traceFrameIndex, int drawmode, bool portal) const;
	void TraceRuntimeLinkEvents(HWDrawInfo& di);
	void ClearRuntimeMapMutationReplacementPayload(RuntimeMapMutationCache::ChunkReplacement& replacement, bool clearMaterialStateCache);
	void TraceRuntimeMapMutationChunk(const nri_scene::PTMapChunk& mapChunk, RuntimeMapMutationCache::ChunkReplacement& replacement);
	void TraceSkyState(const nri_scene::SceneView& sceneView, const char* action, uint64_t resolvedKey);
	void UpdateSurfaceProbe(const nri_scene::GeometryData& geometry, const nri_scene::MaterialBridgeData* materials, bool allowLogging);
	SurfaceProbeEmissiveDiagnostics BuildSurfaceProbeEmissiveDiagnostics(const SurfaceProbeResult& probe) const;
	bool BuildRuntimeDebugSphereOverlay(nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials);
	bool BuildSurfaceLightOverlay(nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials);
	bool EnsureRuntimeDebugSphereCache(RuntimeDebugSphere& sphere);
	void AppendRuntimeDebugSphereToSceneView(const RuntimeDebugSphere& sphere, nri_scene::SceneView& sceneView) const;
	void RefreshSceneLightSystem(
		bool usedStaticMapScene,
		const nri_scene::SceneView* capturedSceneView,
		const nri_scene::MaterialBridgeData* capturedMaterials,
		const nri_scene::SceneView* dynamicSceneView,
		const nri_scene::MaterialBridgeData* dynamicMaterials,
		bool appendPersistentVoxelSceneLights);
	void RefreshResolvedMuzzleFlashRuleLookup(const ResolvedLightOverlaySet& resolvedLightOverlays);
	void ResetMuzzleFlashOverlayState(const char* reason);
	const ResolvedLightOverlayMuzzleFlashRule* FindResolvedMuzzleFlashRule(const FString& eventId) const;
	void RefreshTransientMuzzleFlashLights(double currentTimeSeconds);
	void ResetPersistentDynamicEmissiveCache();
	void PrunePersistentDynamicEmissiveCacheToLiveActors();
	bool RebuildPersistentDynamicEmissiveCache(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials);
	void BuildMaterialsWithActorOverrides(nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& outMaterials, const char* traceLabel = nullptr);
	void ApplyEmissiveMaterialOverrides(const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& inOutGpuMaterials) const;
	void ApplyActorShadowMaterialOverrides(const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& inOutGpuMaterials);
	uint64_t ComputeChunkActorOverrideHash(const nri_scene::MaterialBridgeData& materials);
	uint64_t ComputeChunkEmissiveOverrideHash(const nri_scene::MaterialBridgeData& materials) const;
	void QueueStaticMapSceneLightingInvalidation();
	void InvalidateStaticMapSceneForMaterialLighting();
	PersistentDynamicSurfaceStats GatherPersistentDynamicEmissiveSurfaceStats() const;
	RuntimeMutationCacheStats GatherRuntimeMutationCacheStats() const;
	static MaterialBuildTraceSlot ResolveMaterialBuildTraceSlot(const char* traceLabel);
	const std::unordered_map<int32_t, uint32_t>& GetActorMaterialOverrideMapForFrame(MaterialBuildTraceSlot traceSlot = MaterialBuildTraceSlot::Unknown);
	void LogFallback(const char* reason);
	void CopyFinalToActiveTarget();
	void UpdateFrameGenerationFrameDesc();
	void UpdateFrameGenerationHistoryPolicy(int debugMode, const NRIFrameGenerationPolicy& frameGenPolicy, bool preserveHistory);
	void NoteSuccessfulRealFrame();
	void CopyTexture(NRITextureResource& source, NRITextureResource& destination);
	void CopyTextureToActiveTarget(NRITextureResource& source);

	void DestroyCachedTextures();
	void DestroyFrameTextures();
	void DestroySceneBuffers();
	void DestroyAccelerationStructures();
	void DestroyStaticMapSceneCache(const char* reason = nullptr);
	void DestroyStaticMapSceneResources(StaticMapSceneCache& staticScene, StaticMapSceneResources& staticResources, bool waitForCommands);
	void DestroyBufferResource(NRIBufferResource& resource);
	void DestroyAccelerationStructureResource(NRIAccelerationStructureResource& resource);
	const NRIBufferResource& GetActiveVertexBuffer() const;
	const NRIBufferResource& GetActiveIndexBuffer() const;
	const NRIBufferResource& GetActivePrimitiveBuffer() const;
	const NRIBufferResource& GetActiveMaterialBuffer() const;
	void BindSceneRootDescriptors();

	bool CreateStructuredBuffer(NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after);
	bool EnsureStructuredBuffer(NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, bool writesQuiesced = false, const char* waitReason = nullptr);
	bool UpdateStructuredBufferRange(NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after);
	bool CreateBufferWithoutView(NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage);
	bool CreateBufferWithoutViewAtLocation(NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::MemoryLocation memoryLocation);
	bool EnsureResidentArenaBuffer(NRIBufferResource& resource, uint64_t requiredSize, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after);
	bool EnsureResidentUploadScratchBuffer(ResidentBufferUploadScratch& scratch, ResidentUploadScratchFrame& frameScratch, uint64_t requiredSize);
	bool EnsureResidentStructuredBuffer(NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, const char* waitReason, int uploadKind);
	bool StageResidentBufferCopyRange(NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after, int uploadKind);
	bool QueueRuntimeMutationResidentGeometryUploadRange(int uploadKind, uint64_t byteOffset, uint64_t size);
	bool FlushRuntimeMutationResidentGeometryUploadRanges();
	bool StageRuntimeMutationResidentGeometryUploadRanges(const std::vector<RuntimeMutationResidentUploadRange>& ranges);
	bool StagePersistentVoxelMaterialUploadRanges(const std::vector<RuntimeMutationResidentUploadRange>& ranges, const uint8_t* data, uint64_t availableBytes);
	void RefreshStateCommitCombinedGeometryStaticPrefixForResidentUpdate(const std::vector<uint32_t>& changedGeometryChunkListIndices);
	void RetireResidentBufferResource(NRIBufferResource& resource);
	void RetireResidentAccelerationStructure(NRIAccelerationStructureResource& resource);
	void RetireTopLevelAccelerationStructure(NRIAccelerationStructureResource& resource);
	void BuildRuntimeLightClusterUpload(
		std::vector<RuntimeLightTileHeaderGpuData>& outHeaders,
		std::vector<uint32_t>& outIndices,
		uint32_t& outTileCountX,
		uint32_t& outTileCountY,
		uint32_t& outTileIndexCount,
		uint32_t& outMaxTileOccupancy) const;
	bool UpdateSamplerSet();
	bool UpdateSceneTextureSet(const std::vector<nri::Descriptor*>& descriptors, const char* reason = nullptr);
	bool UpdateFrameTextureSet();
	bool UpdateFrameTextureSet(nri::DescriptorSet* set, const std::array<nri::Descriptor*, 14>& descriptors);
	bool UpdateOutputSet();
	bool UpdateOutputSet(nri::DescriptorSet* set, const std::array<nri::Descriptor*, 15>& descriptors);
	bool EnsureTraceShaderStatsResources();
	void ResetTraceShaderStatsBuffer();
	void CopyTraceShaderStatsForReadback(uint64_t frameNumber);
	void ReadbackTraceShaderStats();
	bool EnsureAutoExposureResources(const NRIAutoExposureSettings& settings);
	void DestroyAutoExposureResources();
	bool UpdateAutoExposureDescriptorSets(FrameTextureSlot sourceSlot);
	bool DispatchAutoExposure(FrameTextureSlot sourceSlot);
	void CopyAutoExposureStatsForReadback(uint64_t frameNumber);
	void ReadbackAutoExposureStats();
	bool CreateFrameTexture(FrameTextureSlot slot, uint32_t width, uint32_t height, nri::Format format);
	void PrepareSceneTextureInputsForCompute();
	void TrackLiveSceneTextureResource(NRITextureResource& resource);
	nri::Format ResolveFinalSceneFormat() const;
	void ResetPerfTraceStats();
	void WaitForCommandsTracked(const char* reason = nullptr);
	void ReleaseWorldAccelerationBuildScratch(const char* reason = nullptr);
	void NotePerfBufferUpload(const SceneBufferDebugStats* stats, uint64_t size, bool growth, const char* reason, int uploadKind);
	NRITextureResource& GetFrameTexture(FrameTextureSlot slot) { return mFrameTextures[(size_t)slot]; }
	const NRITextureResource& GetFrameTexture(FrameTextureSlot slot) const { return mFrameTextures[(size_t)slot]; }
	nri::Pipeline* GetPipeline(PipelineSlot slot) const { return mPipelines[(size_t)slot]; }
	NRIMainUpscalerKind GetSelectedMainUpscalerKind() const;
	NRIMainUpscalerKind ResolveMainUpscalerKind(bool logFallback);
	NRIMainUpscalerKind GetResolvedMainUpscalerKindForStatus() const;
	NRIPostSharpenKind GetSelectedPostSharpenKind() const;
	NRIPostSharpenKind ResolvePostSharpenKind(bool logFallback);
	NRIPostSharpenKind GetResolvedPostSharpenKindForStatus() const;
	nri::UpscalerMode GetSelectedUpscalerMode() const;
	bool IsMainUpscalerSupported(NRIMainUpscalerKind kind) const;
	bool IsPostSharpenSupported(NRIPostSharpenKind kind) const;
	void FillMatrix(float* outMatrix, const VSMatrix& matrix) const;
	const char* GetFrameTextureSlotName(FrameTextureSlot slot) const;

	NRIRenderDevice* mFrameBuffer = nullptr;
	nri::PipelineLayout* mPipelineLayout = nullptr;
	nri::PipelineLayout* mTaaPipelineLayout = nullptr;
	nri::PipelineLayout* mPresentPipelineLayout = nullptr;
	nri::PipelineLayout* mExposurePipelineLayout = nullptr;
	std::array<nri::Pipeline*, (size_t)PipelineSlot::Count> mPipelines = {};
	nri::DescriptorSet* mSamplerSet = nullptr;
	std::vector<nri::DescriptorSet*> mSceneTextureSets;
	std::vector<nri::DescriptorSet*> mSceneDataSets;
	nri::DescriptorSet* mFrameTextureSet = nullptr;
	nri::DescriptorSet* mOutputSet = nullptr;
	nri::DescriptorSet* mCompositionFrameTextureSet = nullptr;
	nri::DescriptorSet* mCompositionOutputSet = nullptr;
	nri::DescriptorSet* mUpscalerPrepassFrameTextureSet = nullptr;
	nri::DescriptorSet* mUpscalerPrepassOutputSet = nullptr;
	nri::DescriptorSet* mTaaFrameTextureSet = nullptr;
	nri::DescriptorSet* mTaaOutputSet = nullptr;
	nri::DescriptorSet* mRawPresentFrameTextureSet = nullptr;
	nri::DescriptorSet* mRawPresentOutputSet = nullptr;
	nri::DescriptorSet* mFinalPresentFrameTextureSet = nullptr;
	nri::DescriptorSet* mFinalPresentOutputSet = nullptr;
	std::array<nri::DescriptorSet*, 2> mExposureInputSets = {};
	std::array<nri::DescriptorSet*, 2> mExposureOutputSets = {};
	FrameTextureSlot mAutoExposureInputSourceSlot = FrameTextureSlot::Count;

	NRITextureResource* GetActiveSkyTexture() { return mActiveSkyTextureIndex < mSkyTextureCache.size() ? &mSkyTextureCache[mActiveSkyTextureIndex].resource : nullptr; }
	const NRITextureResource* GetActiveSkyTexture() const { return mActiveSkyTextureIndex < mSkyTextureCache.size() ? &mSkyTextureCache[mActiveSkyTextureIndex].resource : nullptr; }

	NRITextureResource mPaletteTexture;
	std::array<NRITextureResource, (size_t)FrameTextureSlot::Count> mFrameTextures = {};
	NRIExposureController mExposure;

	NRIBufferResource mVertexBuffer;
	NRIBufferResource mIndexBuffer;
	NRIBufferResource mPrimitiveBuffer;
	NRIBufferResource mMaterialBuffer;
	NRIBufferResource mStaticVertexBuffer;
	NRIBufferResource mStaticIndexBuffer;
	NRIBufferResource mStaticPrimitiveBuffer;
	NRIBufferResource mStaticMaterialBuffer;
	NRIBufferResource mPersistentVoxelVertexBuffer;
	NRIBufferResource mPersistentVoxelIndexBuffer;
	NRIBufferResource mPersistentVoxelPrimitiveBuffer;
	NRIBufferResource mPersistentVoxelMaterialBuffer;
	NRIBufferResource mTlasInstanceBuffer;
	std::vector<NRIBufferResource> mTlasInstanceBufferRing;
	NRIBufferResource mSceneInstanceBuffer;
	NRIBufferResource mPortalBuffer;
	NRIBufferResource mRuntimeLightBuffer;
	NRIBufferResource mRuntimeLightTileHeaderBuffer;
	NRIBufferResource mRuntimeLightTileIndexBuffer;
	NRIBufferResource mEmissivePrimitiveHeaderBuffer;
	NRIBufferResource mEmissivePrimitiveBuffer;
	NRIBufferResource mEmissivePrimitiveCdfBuffer;
	NRIBufferResource mEmissiveMaterialResponseBuffer;
	NRIBufferResource mEmissiveTlasInstanceBuffer;
	NRIBufferResource mSectorLightHeaderBuffer;
	NRIBufferResource mSectorLightBuffer;
	NRIBufferResource mReprojectionBuffer;
	NRIBufferResource mVisibleChunkBuffer;
	NRIBufferResource mVisibleFlatPlaneBuffer;
	NRIBufferResource mTraceShaderStatsBuffer;
	NRIBufferResource mTraceShaderStatsReadbackBuffer;
	NRIBufferResource mTraceShaderStatsZeroBuffer;
	NRIBufferResource mScratchBuffer;
	NRIBufferResource mResidentStaticBlasScratchBuffer;
	NRIBufferResource mTopLevelScratchBuffer;
	NRIBufferResource mEmissiveTopLevelScratchBuffer;
	SelectPrimitiveRewriteCache mSelectPrimitiveRewriteCache = {};
	std::vector<nri_scene::MaterialData> mSelectCapturedGpuMaterialScratch;
	std::vector<nri_scene::MaterialData> mSelectDynamicGpuMaterialScratch;
	std::vector<nri_scene::MaterialData> mSelectPersistentVoxelGpuMaterialScratch;
	std::vector<nri_scene::MaterialData> mSelectCombinedGpuMaterialScratch;
	std::vector<nri_scene::MaterialData> mSelectRefreshedCombinedGpuMaterialScratch;
	nri_scene::GeometryData mSelectMirrorPlayerGeometryScratch;
	nri_scene::GeometryData mSelectOverlayGeometryScratch;
	nri_scene::MaterialBridgeData mSelectOverlayMaterialBridgeScratch;
	StateCommitCombinedGeometryCache mStateCommitCombinedGeometryCache = {};
	std::vector<nri::TopLevelInstance> mSelectTopLevelInstanceScratch;
	std::vector<SceneInstanceData> mSelectSceneInstanceScratch;
	std::vector<nri::TopLevelInstance> mSelectCapturedTopLevelInstanceScratch;
	std::vector<SceneInstanceData> mSelectCapturedSceneInstanceScratch;
	std::vector<SceneUploadBufferRingSlot> mSceneUploadBufferRing;
	std::vector<SceneUploadDirtyRange> mSceneUploadPrimitiveDirtyRangeScratch;
	std::vector<SceneUploadDirtyRange> mSceneUploadMaterialDirtyRangeScratch;
	std::array<ResidentUploadScratchFrame, 3> mResidentUploadScratchFrames = {};
	std::vector<RuntimeMutationResidentUploadRange> mRuntimeMutationResidentGeometryUploadRanges;
	std::vector<uint32_t> mResidentStaticBlasActiveChunkListIndices;
	std::vector<nri::BufferBarrierDesc> mResidentStaticBlasBarriers;
	SceneBufferDebugStats mVertexBufferStats = { "Vertex" };
	SceneBufferDebugStats mIndexBufferStats = { "Index" };
	SceneBufferDebugStats mPrimitiveBufferStats = { "Primitive" };
	SceneBufferDebugStats mMaterialBufferStats = { "Material" };
	SceneBufferDebugStats mSceneInstanceBufferStats = { "SceneInstance" };
	SceneBufferDebugStats mPortalBufferStats = { "Portal" };
	SceneBufferDebugStats mRuntimeLightBufferStats = { "RuntimeLight" };
	SceneBufferDebugStats mRuntimeLightTileHeaderBufferStats = { "RuntimeLightTileHeader" };
	SceneBufferDebugStats mRuntimeLightTileIndexBufferStats = { "RuntimeLightTileIndex" };
	SceneBufferDebugStats mEmissivePrimitiveHeaderBufferStats = { "EmissivePrimitiveHeader" };
	SceneBufferDebugStats mEmissivePrimitiveBufferStats = { "EmissivePrimitive" };
	SceneBufferDebugStats mEmissivePrimitiveCdfBufferStats = { "EmissivePrimitiveCdf" };
	SceneBufferDebugStats mEmissiveMaterialResponseBufferStats = { "EmissiveMaterialResponse" };
	SceneBufferDebugStats mEmissiveTlasInstanceBufferStats = { "EmissiveTLASInstance" };
	SceneBufferDebugStats mSectorLightHeaderBufferStats = { "SectorLightHeader" };
	SceneBufferDebugStats mSectorLightBufferStats = { "SectorLight" };
	SceneBufferDebugStats mReprojectionBufferStats = { "Reprojection" };
	SceneBufferDebugStats mVisibleChunkBufferStats = { "VisibleChunk" };
	SceneBufferDebugStats mVisibleFlatPlaneBufferStats = { "VisibleFlatPlane" };
	PerfShellTraceStats mLastPerfShellTraceStats = {};
	PerfResourceTraceStats mLastPerfResourceTraceStats = {};
	PerfTraceShaderStats mLastPerfTraceShaderStats = {};
	uint64_t mPendingTraceShaderStatsFrame = 0;
	uint64_t mPendingAutoExposureStatsFrame = 0;

	NRIAccelerationStructureResource mTopLevelAS;
	NRIAccelerationStructureResource mEmissiveTopLevelAS;

	std::vector<CachedTexture> mTextureCache;
	std::unordered_map<uint64_t, uint32_t> mTextureCacheKeyIndex;
	std::vector<NRITextureResource*> mLiveSceneTextureResources;
	std::vector<CachedSkyTexture> mSkyTextureCache;
	NRINrdContext mNrd;
	NRIUpscalerContext mUpscaler;
	nri_scene::PTMapWorld mMapWorld;
	StaticMapSceneCache mStaticMapScene;
	StaticMapChunkAtlas mStaticMapChunkAtlas = {};
	ResidentMapChunkRegistry mResidentMapChunkRegistry = {};
	RuntimeMapMutationCache mRuntimeMapMutations;
	DynamicSceneFrameState mDynamicSceneLastFrame = {};
	PersistentDynamicEmissiveCache mPersistentDynamicEmissiveCache = {};
	PersistentVoxelBatch mPersistentVoxelBatch = {};
	std::unordered_map<uint64_t, PersistentVoxelMeshVariantResource> mPersistentVoxelMeshVariantResources;
	std::unordered_map<uint64_t, PersistentVoxelMaterialVariantResource> mPersistentVoxelMaterialVariantResources;
	std::unordered_map<uint64_t, PersistentVoxelInstanceRecord> mPersistentVoxelInstances;
	std::unordered_map<uint64_t, uint64_t> mPersistentVoxelActorRejectedSignatures;
	std::unordered_map<uint64_t, PersistentVoxelAdmissionEntry> mPersistentVoxelAdmissionQueue;
	std::unordered_set<uint64_t> mPersistentVoxelPublishedMeshKeys;
	std::unordered_set<uint64_t> mPersistentVoxelPublishedMaterialKeys;
	uint32_t mPersistentVoxelArenaVertexCursor = 0;
	uint32_t mPersistentVoxelArenaIndexCursor = 0;
	uint32_t mPersistentVoxelArenaPrimitiveCursor = 0;
	uint32_t mPersistentVoxelArenaMaterialCursor = 0;
	uint32_t mPersistentVoxelResidencyMapGeneration = 0;
	uint64_t mPersistentVoxelResidencyLastBuildSerial = 0;
	bool mPersistentVoxelLoadingWarmupActive = false;
	bool mPersistentVoxelPreloadPending = false;
	StateCommitDomainGenerations mLastStateCommitDomainGenerations = {};
	bool mHasLastStateCommitDomainGenerations = false;
	ActorSpriteDebugStats mActorSpriteDebugStats = {};
	ActorMaterialOverrideCache mActorMaterialOverrideCache = {};
	SceneTextureOverflowDebugStats mSceneTextureOverflowStats = {};
	SceneTextureCacheDebugStats mSceneTextureCacheDebugStats = {};
	DescriptorCoherencyDebugStats mDescriptorCoherencyDebugStats = {};
	RuntimeMapMutationFrameState mRuntimeMapLastFrame = {};
	RuntimeSpaceLinkFrameState mRuntimeSpaceLinkLastFrame = {};
	RuntimeLinkTraceState mLastRuntimeLinkTraceState = {};
	std::vector<RuntimeChunkTranslationState> mRuntimeChunkTranslationHistory;
	struct RuntimeRecurringChunkTracker
	{
		bool valid = false;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		uint32_t lastReasonMask = 0;
		uint32_t visitCount = 0;
		uint32_t uniqueStateCount = 0;
		uint32_t transitionCount = 0;
		uint32_t repeatedStateHitCount = 0;
		uint32_t abaRecurrenceCount = 0;
		uint32_t lastWallCount = 0;
		uint32_t lastFlatCount = 0;
		uint32_t lastTriangleCount = 0;
		uint32_t lastMaterialCount = 0;
		uint64_t previousStateSignature = 0;
		uint64_t lastStateSignature = 0;
		std::array<uint64_t, 8> seenStateSignatures = {};
	};
	uint64_t mRuntimeRecurringChunkTrackerBuildSerial = 0;
	std::vector<RuntimeRecurringChunkTracker> mRuntimeRecurringChunkTrackers;
	PersistentDynamicSurfaceStats mPersistentDynamicEmissiveHighWaterStats = {};
	uint32_t mPersistentDynamicEmissiveHighWaterSurfaceCount = 0;
	uint32_t mPersistentDynamicEmissiveHighWaterPrimitiveCount = 0;
	uint32_t mPersistentDynamicEmissiveHighWaterMaterialCount = 0;
	RuntimeMutationCacheStats mRuntimeMutationCacheHighWaterStats = {};
	nri_scene::SceneDebugStats mLastStats = {};
	SceneLightSystem mSceneLights;
	NRIDirectionalLightState mDirectionalLightState = {};
	NRIPTNightVisionState mNightVisionState = {};
	std::unordered_map<std::string, ResolvedLightOverlayMuzzleFlashRule> mResolvedMuzzleFlashRuleLookup;
	std::vector<TransientMuzzleFlashSlot> mTransientMuzzleFlashSlots;
	std::vector<SceneLightSystem::SceneAnalyticLight> mTransientMuzzleFlashLights;
	std::array<nri::Descriptor*, 26> mSceneDataDescriptors = {};
	std::array<nri::Descriptor*, 14> mFrameInputDescriptors = {};
	std::array<nri::Descriptor*, 15> mOutputDescriptors = {};
	std::vector<SceneInstanceData> mBoundSceneInstances;
	std::vector<uint32_t> mCurrentVisibleChunkWords;
	std::vector<uint32_t> mCurrentVisibleFlatPlaneWords;
	uint32_t mLastResolvedLightOverlayGeneration = 0;
	uint32_t mFrameIndex = 0;
	uint64_t mFrameGenerationFrameId = 0;
	uint32_t mRenderWidth = 0;
	uint32_t mRenderHeight = 0;
	uint32_t mOutputWidth = 0;
	uint32_t mOutputHeight = 0;
	uint32_t mTargetWidth = 0;
	uint32_t mTargetHeight = 0;
	int32_t mSceneLeft = 0;
	int32_t mSceneTop = 0;
	nri::Format mFinalSceneFormat = nri::Format::UNKNOWN;
	float mCurrentCameraPos[3] = {};
	float mCurrentCameraForward[3] = {};
	float mCurrentCameraRight[3] = {};
	float mCurrentCameraUp[3] = {};
	float mPreviousCameraPos[3] = {};
	float mPreviousCameraForward[3] = {};
	float mPreviousCameraRight[3] = {};
	float mPreviousCameraUp[3] = {};
	float mCurrentTanHalfFovX = 1.0f;
	float mCurrentTanHalfFovY = 1.0f;
	float mPreviousTanHalfFovX = 1.0f;
	float mPreviousTanHalfFovY = 1.0f;
	float mCurrentJitter[2] = {};
	float mPreviousJitter[2] = {};
	float mCurrentViewToClip[16] = {};
	float mPreviousViewToClip[16] = {};
	float mCurrentWorldToView[16] = {};
	float mPreviousWorldToView[16] = {};
	float mSkyColor[3] = { 0.38f, 0.48f, 0.65f };
	float mGroundColor[3] = { 0.08f, 0.08f, 0.08f };
	uint64_t mSkyTextureKey = 0;
	uint32_t mActiveSkyTextureIndex = UINT32_MAX;
	MapRecord* mSkyLevel = nullptr;
	SkyState mSkyState = {};
	SkyState mLastTracedSkyState = {};
	SelfTestRouteSnapshot mSelfTestRoute = {};
	uint64_t mLastTracedSkyResolvedKey = 0;
	bool mHasTracedSkyState = false;
	PreservedStaticMapSkyState mPreservedStaticMapSky = {};
	bool mHasLoggedStats = false;
	bool mHasPreviousCameraState = false;
	bool mHasFrameGenerationRealFrameTime = false;
	bool mHasPendingFrameGenerationRealFrameTime = false;
	bool mHasFrameGenerationTimestamp = false;
	bool mHasFrameGenerationConfigState = false;
	bool mSceneTextureLimitLogPrinted = false;
	bool mHasDirectionalLightState = false;
	bool mPathTracingSupported = true;
	bool mGuiCaptureActive = false;
	bool mHasOutputPolicyState = false;
	bool mHasRuntimeLinkTraceState = false;
	SurfaceProbeFrameState mSurfaceProbeFrame = {};
	bool mResetHistory = true;
	std::string mLastHistoryResetReason = "startup";
	float mLastFrameGenerationRealFrameTimeMs = 0.0f;
	float mPendingFrameGenerationRealFrameTimeMs = 0.0f;
	std::chrono::steady_clock::time_point mLastFrameGenerationTimestamp = {};
	std::chrono::steady_clock::time_point mPendingFrameGenerationTimestamp = {};
	bool mLastFrameGenerationRequestedEnabled = false;
	NRIFrameGenerationProvider mLastFrameGenerationRequestedProvider = NRIFrameGenerationProvider::Off;
	NRIFrameGenerationUiMode mLastFrameGenerationResolvedUiMode = NRIFrameGenerationUiMode::Auto;
	bool mUseUpscaledInFinal = false;
	bool mLastTemporalAppTaaEnabled = false;
	bool mHasTemporalExposureState = false;
	bool mHasAutoExposureSettingsState = false;
	bool mUseDenoisedCompositionInputs = false;
	bool mUseSplitShadowDenoiser = false;
	bool mHasLoggedFallback = false;
	bool mUsedStaticMapSceneLastFrame = false;
	bool mUsedDynamicSceneLastFrame = false;
	bool mHasVisibleMirrorPortalLastFrame = false;
	bool mGpuSceneHasDynamicOverlay = false;
	bool mUploadedStaticMapSceneLastFrame = false;
	bool mBuiltStaticMapSceneASLastFrame = false;
	bool mBuiltDynamicSceneASLastFrame = false;
	bool mPendingStaticMapLightingInvalidation = false;
	bool mAllowStartupMapWorldCorrection = false;
	bool mAllowStartupMutationRebaseline = false;
	bool mPendingStartupMutationRebaseline = false;
	std::vector<uint8_t> mPendingStartupVisibleChunkValidation;
	std::vector<uint8_t> mRuntimeMutationSignatureWatchlist;
	uint64_t mRuntimeMutationSignatureWatchlistBuildSerial = 0;
	uint32_t mRuntimeMutationWorklistSweepCursor = 0;
	uint64_t mObservedMapWorldBuildSerial = 0;
	uint64_t mStaticAccelerationBuildSerial = 0;
	uint32_t mStartupMapWorldCorrectionDeadlineFrame = 0;
	uint32_t mStartupMutationRebaselineDeadlineFrame = 0;
	uint32_t mActiveTlasInstanceCount = 0;
	uint32_t mBoundStaticPrimitiveCount = 0;
	uint32_t mBoundDynamicPrimitiveCount = 0;
	uint32_t mBoundStaticMaterialCount = 0;
	uint32_t mBoundDynamicMaterialCount = 0;
	uint32_t mBoundPersistentVoxelPrimitiveCount = 0;
	uint32_t mBoundPersistentVoxelMaterialCount = 0;
	uint32_t mBoundPortalCount = 0;
	uint32_t mBoundRuntimeLightCount = 0;
	uint32_t mBoundRuntimeLightTileCountX = 0;
	uint32_t mBoundRuntimeLightTileCountY = 0;
	uint32_t mBoundRuntimeLightTileSize = 0;
	uint32_t mBoundRuntimeLightTileIndexCount = 0;
	uint32_t mBoundRuntimeLightMaxTileOccupancy = 0;
	std::vector<uint8_t> mSceneDataDescriptorsInitialized;
	bool mRuntimeLightPayloadCacheValid = false;
	uint64_t mRuntimeLightPayloadHash = 0;
	bool mRuntimeLightClusterCacheValid = false;
	uint64_t mRuntimeLightClusterPayloadHash = 0;
	uint64_t mRuntimeLightClusterCameraHash = 0;
	bool mRuntimeLightSceneDataDirty = false;
	uint32_t mBoundEmissivePrimitiveCount = 0;
	uint32_t mBoundEmissiveDominantPrimitive = UINT32_MAX;
	uint32_t mBoundEmissiveDominantTile = 0;
	uint32_t mBoundEmissiveDominantFlags = 0;
	uint32_t mBoundEmissiveDominantDataSource = 0;
	bool mEmissiveSamplingPayloadCacheValid = false;
	uint64_t mEmissiveSamplingPayloadHash = 0;
	bool mEmissiveSectorResponsePayloadCacheValid = false;
	uint64_t mEmissiveSectorResponsePayloadHash = 0;
	bool mEmissiveSectorResponseTraceCacheValid = false;
	uint64_t mEmissiveSectorResponseTraceHash = 0;
	bool mEmissiveSectorResponseNotifyCacheValid = false;
	uint32_t mLastEmissiveSectorResponseNotifyFrame = 0;
	std::vector<float> mEmissiveSectorResponseNotifyScales;
	bool mSectorLightingEditNotifyCacheValid = false;
	uint32_t mLastSectorLightingEditNotifyFrame = 0;
	std::vector<uint64_t> mSectorLightingEditNotifyHashes;
	uint32_t mEmissiveTlasInstanceCount = 0;
	uint32_t mEmissiveTlasStaticInstanceCount = 0;
	uint32_t mEmissiveTlasDynamicInstanceCount = 0;
	uint32_t mEmissiveTlasBuildCount = 0;
	bool mEmissiveTlasInstancePayloadCacheValid = false;
	uint64_t mEmissiveTlasInstancePayloadHash = 0;
	float mBoundEmissiveTotalPower = 0.0f;
	float mBoundEmissiveDominantPower = 0.0f;
	std::vector<EmissivePrimitiveDebugRecord> mBoundEmissivePrimitiveRecords;
	bool mSectorLightingPayloadCacheValid = false;
	uint64_t mSectorLightingPayloadHash = 0;
	uint32_t mBoundSectorLightSectorCount = 0;
	uint32_t mBoundSectorLightActiveCount = 0;
	uint32_t mBoundSectorLightPulsingCount = 0;
	uint32_t mBoundSectorLightDominantSector = UINT32_MAX;
	float mBoundSectorLightDominantContribution = 0.0f;
	uint32_t mNextRuntimePointLightId = 1;
	std::vector<RuntimeDebugSphere> mRuntimeDebugSpheres;
	uint32_t mNextRuntimeDebugSphereId = 1;
	SurfaceProbeResult mLastSurfaceProbe = {};
	SurfaceProbeResult mLastLoggedSurfaceProbe = {};
	int mLastDebugMode = -1;
	int mLastMainUpscalerRequest = -1;
	int mLastPostSharpenRequest = -1;
	NRIPTOutputMode mLastOutputRequestedMode = NRIPTOutputMode::SDR;
	NRIPTOutputMode mLastOutputResolvedMode = NRIPTOutputMode::SDR;
	float mLastTemporalExposure = 1.0f;
	NRIAutoExposureSettings mLastAutoExposureSettings = {};
	NRIMainUpscalerKind mLastMainUpscalerResolved = NRIMainUpscalerKind::Off;
	NRIPostSharpenKind mLastPostSharpenResolved = NRIPostSharpenKind::Off;
	NRIMainUpscalerKind mLastTemporalHistoryMainUpscaler = NRIMainUpscalerKind::Off;
	NRIPostSharpenKind mLastTemporalPostSharpen = NRIPostSharpenKind::Off;
	FrameTextureSlot mHistoryInputSlot = FrameTextureSlot::TaaHistoryPing;
	FrameTextureSlot mHistoryOutputSlot = FrameTextureSlot::TaaHistoryPong;
	FrameTextureSlot mUpscaledInputSlot = FrameTextureSlot::PostSharpenOutput;
};
