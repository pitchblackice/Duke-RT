#include "nri_renderer.h"
#include "nri_cvars.h"

#include "../framegen/nri_framegen.h"
#include "nri_acceleration.h"
#include "nri_diagnostic_names.h"
#include "nri_frame_resources.h"
#include "nri_material_policy.h"
#include "nri_pass_dispatch.h"
#include "nri_persistent_voxel_services.h"
#include "nri_ray_scene_builder.h"
#include "nri_render_geometry_helpers.h"
#include "nri_renderer_settings.h"
#include "nri_scene_frame_builder.h"
#include "nri_scene_frame_diagnostics.h"
#include "nri_scene_frame_coordinator_types.h"
#include "nri_scene_frame_mirrors.h"
#include "nri_scene_frame_overlay.h"
#include "nri_scene_frame_selection.h"
#include "nri_scene_frame_state.h"
#include "nri_scene_upload.h"
#include "nri_shader_contracts.h"
#include "nri_static_scene_geometry.h"
#include "nri_surface_light_overlay.h"
#include "nri_runtime_mutation_shared.h"
#include "nri_sky_environment.h"
#include "../scene/nri_hash.h"
#include "../scene/nri_scene_stats.h"
#include "../system/nri_renderdevice.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "coreactor.h"
#include "coreplayer.h"
#include "hw_voxels.h"
#include "gamecontrol.h"
#include "gamestate.h"
#include "hw_sections.h"
#include "lightoverlay.h"
#include "mapinfo.h"
#include "printf.h"
#include "gamestruct.h"
#include "hw_portal.h"
#include "texinfo.h"
#include "texturemanager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>


namespace
{
	static uint32_t GetBootstrapMode()
	{
		return (uint32_t)std::max(0, std::min((int)nri_ptbootstrapmode, 13));
	}

	static bool HasPlainMirrorMaterialSurfaces(const nri_scene::PTMapWorld& mapWorld)
	{
		for (const nri_scene::PTMapSurface& surface : mapWorld.surfaces)
		{
			if ((surface.surface.material.flags & nri_scene::MaterialFlag_PlainMirror) != 0)
			{
				return true;
			}
		}

		return false;
	}

	static void RecordDynamicOverlayBlasModelStats(
		NRIRenderer::PerfShellTraceStats& stats,
		const std::vector<NRIRenderer::SceneBufferUploadDomainSpan>& uploadSpans)
	{
		stats.dynamicOverlayBlasBuildEnabled = (bool)nri_ptdynamicoverlayblasbuild;
		stats.dynamicOverlayBlasRouteEnabled = (bool)nri_ptdynamicoverlayblasroute;
		stats.dynamicOverlayBlasBuildBudget = (uint32_t)std::max(0, (int)nri_ptdynamicoverlayblasbuilds);

		for (const NRIRenderer::SceneBufferUploadDomainSpan& span : uploadSpans)
		{
			if (span.vertexCount == 0 &&
				span.indexCount == 0 &&
				span.primitiveCount == 0 &&
				span.materialCount == 0)
			{
				continue;
			}

			stats.dynamicOverlayBlasDomainCount++;
			stats.dynamicOverlayBlasVertexCount += span.vertexCount;
			stats.dynamicOverlayBlasIndexCount += span.indexCount;
			stats.dynamicOverlayBlasPrimitiveCount += span.primitiveCount;
			stats.dynamicOverlayBlasMaterialCount += span.materialCount;

			if (!stats.dynamicOverlayBlasBuildEnabled && !stats.dynamicOverlayBlasRouteEnabled)
			{
				stats.dynamicOverlayBlasRejectDisabled += span.primitiveCount;
			}

			switch (span.domain)
			{
			case NRIRenderer::SceneBufferUploadDomain::Dynamic:
				stats.dynamicOverlayBlasEligibleDomains++;
				stats.dynamicOverlayBlasEligiblePrimitives += span.primitiveCount;
				if ((stats.dynamicOverlayBlasBuildEnabled || stats.dynamicOverlayBlasRouteEnabled) && span.materialCount == 0)
				{
					stats.dynamicOverlayBlasRejectMaterialBase += span.primitiveCount;
				}
				break;
			case NRIRenderer::SceneBufferUploadDomain::RuntimeSpaceLink:
				stats.dynamicOverlayBlasRejectRuntimeSpaceLink += span.primitiveCount;
				break;
			case NRIRenderer::SceneBufferUploadDomain::RuntimeMutation:
				stats.dynamicOverlayBlasRejectRuntimeMutation += span.primitiveCount;
				break;
			case NRIRenderer::SceneBufferUploadDomain::LocalPlayerReflection:
				stats.dynamicOverlayBlasRejectLocalPlayerReflection += span.primitiveCount;
				break;
			case NRIRenderer::SceneBufferUploadDomain::RuntimeDebugSphere:
				stats.dynamicOverlayBlasRejectRuntimeDebugSphere += span.primitiveCount;
				break;
			case NRIRenderer::SceneBufferUploadDomain::SurfaceLightOverlay:
				stats.dynamicOverlayBlasRejectSurfaceLightOverlay += span.primitiveCount;
				break;
			case NRIRenderer::SceneBufferUploadDomain::StaticOverlay:
			case NRIRenderer::SceneBufferUploadDomain::PersistentVoxelMaterial:
			case NRIRenderer::SceneBufferUploadDomain::Count:
			default:
				stats.dynamicOverlayBlasRejectStaticOverlay += span.primitiveCount;
				break;
			}
		}

		stats.dynamicOverlayBlasFallbackDomains = stats.dynamicOverlayBlasDomainCount;
		stats.dynamicOverlayBlasFallbackPrimitives = stats.dynamicOverlayBlasPrimitiveCount;
		stats.dynamicOverlayBlasMonolithicRefs = stats.dynamicOverlayBlasPrimitiveCount != 0 ? 1u : 0u;
	}

	static const NRIRenderer::SceneBufferUploadDomainSpan* FindUploadDomainSpan(
		const std::vector<NRIRenderer::SceneBufferUploadDomainSpan>& uploadSpans,
		NRIRenderer::SceneBufferUploadDomain domain)
	{
		for (const NRIRenderer::SceneBufferUploadDomainSpan& span : uploadSpans)
		{
			if (span.domain == domain)
			{
				return &span;
			}
		}
		return nullptr;
	}

	static bool TryComputeCapturedSurfaceNormal(const nri_scene::SurfaceRef& surface, float outNormal[3])
	{
		if (surface.vertices.size() < 3)
		{
			return false;
		}

		const nri_scene::CapturedVertex& a = surface.vertices[0];
		const nri_scene::CapturedVertex& b = surface.vertices[1];
		const nri_scene::CapturedVertex& c = surface.vertices[2];
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float nx = aby * acz - abz * acy;
		const float ny = abz * acx - abx * acz;
		const float nz = abx * acy - aby * acx;
		const float lengthSq = nx * nx + ny * ny + nz * nz;
		if (lengthSq <= 1.0e-8f)
		{
			return false;
		}

		const float invLength = 1.0f / std::sqrt(lengthSq);
		outNormal[0] = nx * invLength;
		outNormal[1] = ny * invLength;
		outNormal[2] = nz * invLength;
		return true;
	}

	static void NudgeBlindSpotReplacementFlats(nri_scene::SceneView& sceneView)
	{
		static constexpr float kBlindSpotFlatDepthNudge = 0.01f;

		for (nri_scene::SurfaceRef& surface : sceneView.opaqueFlats)
		{
			if (surface.provenance.sourceType != nri_scene::SurfaceSourceType::MapFloorSection &&
				surface.provenance.sourceType != nri_scene::SurfaceSourceType::MapCeilingSection)
			{
				continue;
			}

			float normal[3] = {};
			if (!TryComputeCapturedSurfaceNormal(surface, normal))
			{
				continue;
			}

			for (nri_scene::CapturedVertex& vertex : surface.vertices)
			{
				vertex.position[0] += normal[0] * kBlindSpotFlatDepthNudge;
				vertex.position[1] += normal[1] * kBlindSpotFlatDepthNudge;
				vertex.position[2] += normal[2] * kBlindSpotFlatDepthNudge;
				vertex.prevPosition[0] += normal[0] * kBlindSpotFlatDepthNudge;
				vertex.prevPosition[1] += normal[1] * kBlindSpotFlatDepthNudge;
				vertex.prevPosition[2] += normal[2] * kBlindSpotFlatDepthNudge;
			}
		}
	}

	static void NudgeCapturedSurface(nri_scene::SurfaceRef& surface, float depthNudge)
	{
		float normal[3] = {};
		if (!TryComputeCapturedSurfaceNormal(surface, normal))
		{
			return;
		}

		for (nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			vertex.position[0] += normal[0] * depthNudge;
			vertex.position[1] += normal[1] * depthNudge;
			vertex.position[2] += normal[2] * depthNudge;
			vertex.prevPosition[0] += normal[0] * depthNudge;
			vertex.prevPosition[1] += normal[1] * depthNudge;
			vertex.prevPosition[2] += normal[2] * depthNudge;
		}
	}


	static void RebuildSceneViewStats(nri_scene::SceneView& sceneView)
	{
		const nri_scene::SceneDebugStats preservedStats = sceneView.stats;
		nri_scene::SceneDebugStats stats = {};
		stats.wallDrawItems = (uint32_t)sceneView.opaqueWalls.size();
		stats.flatDrawItems = (uint32_t)sceneView.opaqueFlats.size();
		stats.spriteDrawItems = (uint32_t)sceneView.opaqueSprites.size();

		for (const nri_scene::SurfaceRef& wall : sceneView.opaqueWalls)
		{
			stats.triangleEstimate += !wall.indices.empty() ? (uint32_t)(wall.indices.size() / 3u) : (wall.vertices.size() >= 3 ? (uint32_t)wall.vertices.size() - 2u : 0u);
			stats.materialRefs++;
			if (wall.provenance.sourceType == nri_scene::SurfaceSourceType::MirrorWall)
			{
				stats.mirrorSurfaces++;
			}
		}

		for (const nri_scene::SurfaceRef& flat : sceneView.opaqueFlats)
		{
			stats.triangleEstimate += !flat.indices.empty() ? (uint32_t)(flat.indices.size() / 3u) : (uint32_t)(flat.vertices.size() / 3u);
			stats.materialRefs++;
		}

		for (const nri_scene::SurfaceRef& sprite : sceneView.opaqueSprites)
		{
			stats.triangleEstimate += !sprite.indices.empty() ? (uint32_t)(sprite.indices.size() / 3u) : (sprite.vertices.size() >= 3 ? (uint32_t)sprite.vertices.size() - 2u : 0u);
			stats.materialRefs++;
			if (sprite.provenance.sourceType == nri_scene::SurfaceSourceType::VoxelProxySprite)
			{
				stats.modelDrawItems++;
				stats.voxelProxyDrawItems++;
			}
			else
			{
				stats.translucentDrawItems++;
			}
		}

		stats.totalDrawItems = stats.wallDrawItems + stats.flatDrawItems + stats.spriteDrawItems;
		stats.voxelStableCandidates = preservedStats.voxelStableCandidates;
		stats.voxelStableUncacheable = preservedStats.voxelStableUncacheable;
		stats.voxelStableSignatureHits = preservedStats.voxelStableSignatureHits;
		stats.voxelStableSignatureMisses = preservedStats.voxelStableSignatureMisses;
		stats.voxelStableSignatureChanges = preservedStats.voxelStableSignatureChanges;
		stats.voxelStableSplitStable = preservedStats.voxelStableSplitStable;
		stats.voxelStableSplitLive = preservedStats.voxelStableSplitLive;
		stats.voxelCacheEntries = preservedStats.voxelCacheEntries;
		stats.voxelCacheSurfaceHits = preservedStats.voxelCacheSurfaceHits;
		stats.voxelCacheSurfaceStores = preservedStats.voxelCacheSurfaceStores;
		stats.voxelCacheSurfaceRebuilds = preservedStats.voxelCacheSurfaceRebuilds;
		stats.voxelCacheTransformRebakes = preservedStats.voxelCacheTransformRebakes;
		stats.voxelCacheSurfaceRemoves = preservedStats.voxelCacheSurfaceRemoves;
		stats.voxelCacheNotCaptured = preservedStats.voxelCacheNotCaptured;
		stats.voxelCachePrimitives = preservedStats.voxelCachePrimitives;
		sceneView.stats = stats;
	}


	struct MaterialTextureAttributionCounts
	{
		uint32_t materialCount = 0;
		uint32_t actorMaterialCount = 0;
		uint32_t textureCount = 0;
		uint32_t baseTextureCount = 0;
		uint32_t glowTextureCount = 0;
		uint32_t normalTextureCount = 0;
		uint32_t metallicTextureCount = 0;
		uint32_t roughnessTextureCount = 0;
		uint32_t emissiveTextureCount = 0;
	};

	static bool ShouldTraceActorOverflow()
	{
		return (int)perf_looptraceframes > 0;
	}

	static bool ShouldTraceResidentGeometryOrderHash()
	{
		return (int)perf_looptraceframes > 0;
	}

	static bool ShouldTraceSceneBufferDirtyRanges()
	{
		return (int)perf_looptraceframes > 0;
	}


	static constexpr uint32_t NRI_MAX_ACTOR_OVERFLOW_TRACE_LINES = 16;

	static MaterialTextureAttributionCounts GatherMaterialTextureAttribution(
		const std::vector<nri_scene::MaterialData>& materials,
		const std::vector<nri_scene::MaterialLightingMetadata>& lightMetadata,
		size_t textureCount)
	{
		MaterialTextureAttributionCounts counts = {};
		counts.materialCount = (uint32_t)materials.size();
		counts.textureCount = (uint32_t)textureCount;

		std::unordered_set<uint32_t> baseTextures;
		std::unordered_set<uint32_t> glowTextures;
		std::unordered_set<uint32_t> normalTextures;
		std::unordered_set<uint32_t> metallicTextures;
		std::unordered_set<uint32_t> roughnessTextures;
		std::unordered_set<uint32_t> emissiveTextures;
		baseTextures.reserve(materials.size());
		glowTextures.reserve(lightMetadata.size());
		normalTextures.reserve(materials.size());
		metallicTextures.reserve(materials.size());
		roughnessTextures.reserve(materials.size());
		emissiveTextures.reserve(materials.size());

		const auto addTextureIndex = [textureCount](std::unordered_set<uint32_t>& destination, uint32_t textureIndex)
		{
			if (textureIndex != UINT32_MAX && (size_t)textureIndex < textureCount)
			{
				destination.insert(textureIndex);
			}
		};

		for (uint32_t materialIndex = 0; materialIndex < (uint32_t)materials.size(); ++materialIndex)
		{
			const auto& material = materials[materialIndex];
			addTextureIndex(baseTextures, material.textureIndex);
			addTextureIndex(normalTextures, material.normalTextureIndex);
			addTextureIndex(metallicTextures, material.metallicTextureIndex);
			addTextureIndex(roughnessTextures, material.roughnessTextureIndex);
			addTextureIndex(emissiveTextures, material.emissiveTextureIndex);
			if (materialIndex < lightMetadata.size())
			{
				const auto& metadata = lightMetadata[materialIndex];
				addTextureIndex(glowTextures, metadata.glowmapTextureIndex);
				if (metadata.actorIndex >= 0)
				{
					counts.actorMaterialCount++;
				}
			}
		}

		counts.baseTextureCount = (uint32_t)baseTextures.size();
		counts.glowTextureCount = (uint32_t)glowTextures.size();
		counts.normalTextureCount = (uint32_t)normalTextures.size();
		counts.metallicTextureCount = (uint32_t)metallicTextures.size();
		counts.roughnessTextureCount = (uint32_t)roughnessTextures.size();
		counts.emissiveTextureCount = (uint32_t)emissiveTextures.size();
		return counts;
	}

	static void AccumulateMaterialTextureAttribution(NRIRenderer::MaterialBuildTraceEntry& entry, const MaterialTextureAttributionCounts& counts)
	{
		entry.materialCount += counts.materialCount;
		entry.actorMaterialCount += counts.actorMaterialCount;
		entry.textureCount += counts.textureCount;
		entry.baseTextureCount += counts.baseTextureCount;
		entry.glowTextureCount += counts.glowTextureCount;
		entry.normalTextureCount += counts.normalTextureCount;
		entry.metallicTextureCount += counts.metallicTextureCount;
		entry.roughnessTextureCount += counts.roughnessTextureCount;
		entry.emissiveTextureCount += counts.emissiveTextureCount;
	}

static void Copy3(const float* src, float* dst)
{
	std::memcpy(dst, src, sizeof(float) * 3);
}

static void Copy2(const float* src, float* dst)
{
	std::memcpy(dst, src, sizeof(float) * 2);
}

static const char* YesNo(bool value)
{
	return value ? "yes" : "no";
}

static double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
{
	return std::chrono::duration<double, std::milli>(end - start).count();
}

static bool ShouldTracePtPerf()
{
	return PerfLoopTraceActive() || ShouldEmitRendererTemporalTraceLogs();
}

static bool ShouldCollectPtPerfTiming()
{
	return ShouldTracePtPerf() || (bool)nri_ptslowdowntrace;
}

class ScopedPtPerfTimer
{
public:
	explicit ScopedPtPerfTimer(double& targetMs)
		: mTarget(ShouldCollectPtPerfTiming() ? &targetMs : nullptr)
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

}


bool NRIRenderer::BuildRenderSceneFrame(HWDrawInfo& di, const RenderSceneFrameBuildInputs& inputs, const RenderSceneHistorySnapshot& history, RenderSceneFrameBuildResult& frame)
{
	const uint32_t bootstrapMode = inputs.bootstrapMode;
	const bool bootstrapCapturedView = inputs.bootstrapCapturedView;
	const bool bootstrapCapturedDiagnostics = inputs.bootstrapCapturedDiagnostics;
	const bool bootstrapCapturedFlat = inputs.bootstrapCapturedFlat;
	const bool bootstrapCapturedBaseColor = inputs.bootstrapCapturedBaseColor;
	const bool rawTraceDirectScene = inputs.rawTraceDirectScene;
	const bool preserveHistory = inputs.preserveHistory;
	const NRIPersistentVoxelSettings persistentVoxelSettings = BuildNRIPersistentVoxelSettingsFromCVars();
	const bool allowStaticMapScene = !bootstrapCapturedView && !rawTraceDirectScene && mMapWorld.valid;
	nri_scene::SceneView& capturedSceneView = frame.capturedSceneView;
	nri_scene::SceneView& dynamicSceneView = frame.dynamicSceneView;
	nri_scene::GeometryData& capturedGeometry = frame.capturedGeometry;
	NRIRuntimeMutationFrameOutput& runtimeMutationFrame = frame.runtimeMutationFrame;
	nri_scene::GeometryData& runtimeSpaceLinkGeometry = frame.runtimeSpaceLinkGeometry;
	nri_scene::GeometryData& dynamicGeometry = frame.dynamicGeometry;
	nri_scene::GeometryData& mergedDynamicGeometry = frame.mergedDynamicGeometry;
	nri_scene::GeometryData& debugSphereGeometry = frame.debugSphereGeometry;
	nri_scene::GeometryData& surfaceLightGeometry = frame.surfaceLightGeometry;
	nri_scene::MaterialBridgeData& materialBridge = frame.materialBridge;
	nri_scene::MaterialBridgeData& runtimeSpaceLinkMaterialBridge = frame.runtimeSpaceLinkMaterialBridge;
	nri_scene::MaterialBridgeData& dynamicMaterialBridge = frame.dynamicMaterialBridge;
	nri_scene::MaterialBridgeData& localPlayerReflectionMaterialBridge = frame.localPlayerReflectionMaterialBridge;
	nri_scene::MaterialBridgeData& sceneLightMergedDynamicMaterialBridge = frame.sceneLightMergedDynamicMaterialBridge;
	nri_scene::MaterialBridgeData& mergedDynamicMaterialBridge = frame.mergedDynamicMaterialBridge;
	nri_scene::MaterialBridgeData& debugSphereMaterialBridge = frame.debugSphereMaterialBridge;
	nri_scene::MaterialBridgeData& surfaceLightMaterialBridge = frame.surfaceLightMaterialBridge;
	nri_scene::GeometryData& overlayGeometry = mSelectOverlayGeometryScratch;
	nri_scene::MaterialBridgeData& overlayMaterialBridge = mSelectOverlayMaterialBridgeScratch;
	nri_scene::MaterialBridgeData& combinedMaterialBridge = frame.combinedMaterialBridge;
	auto& capturedGpuMaterials = mSelectCapturedGpuMaterialScratch;
	auto& dynamicGpuMaterials = mSelectDynamicGpuMaterialScratch;
	auto& persistentVoxelGpuMaterials = mSelectPersistentVoxelGpuMaterialScratch;
	auto& combinedGpuMaterials = mSelectCombinedGpuMaterialScratch;
	auto& refreshedCombinedGpuMaterials = mSelectRefreshedCombinedGpuMaterialScratch;
	capturedGpuMaterials.clear();
	dynamicGpuMaterials.clear();
	persistentVoxelGpuMaterials.clear();
	combinedGpuMaterials.clear();
	refreshedCombinedGpuMaterials.clear();
	nri_scene::ClearGeometryRetainingCapacity(mSelectLocalPlayerReflectionGeometryScratch);
	nri_scene::ClearGeometryRetainingCapacity(mSelectOverlayGeometryScratch);
	nri_scene::ClearMaterialBridgeRetainingCapacity(mSelectOverlayMaterialBridgeScratch);
	mSelectTopLevelInstanceScratch.clear();
	mSelectSceneInstanceScratch.clear();
	mSelectCapturedTopLevelInstanceScratch.clear();
	mSelectCapturedSceneInstanceScratch.clear();
	const nri_scene::SceneView*& activeSceneView = frame.activeSceneView;
	const nri_scene::GeometryData*& activeGeometry = frame.activeGeometry;
	const std::vector<nri_scene::MaterialData>*& activeGpuMaterials = frame.activeGpuMaterials;
	const nri_scene::MaterialBridgeData*& activeMaterialBridge = frame.activeMaterialBridge;
	const nri_scene::SceneView* sceneLightCapturedView = nullptr;
	const nri_scene::MaterialBridgeData* sceneLightCapturedMaterials = nullptr;
	const nri_scene::SceneView* sceneLightDynamicView = nullptr;
	const nri_scene::MaterialBridgeData* sceneLightDynamicMaterials = nullptr;
	nri_scene::SceneView& localPlayerReflectionSceneView = frame.localPlayerReflectionSceneView;
	nri_scene::SceneView& surfaceLightSceneView = frame.surfaceLightSceneView;
	nri_scene::SceneView& sceneLightMergedDynamicSceneView = frame.sceneLightMergedDynamicSceneView;
	nri_scene::SceneView& mergedDynamicSceneView = frame.mergedDynamicSceneView;
	const nri_scene::SceneView*& activeDynamicSceneView = frame.activeDynamicSceneView;
	const nri_scene::GeometryData*& activeDynamicGeometry = frame.activeDynamicGeometry;
	const nri_scene::MaterialBridgeData*& activeDynamicMaterials = frame.activeDynamicMaterials;
	nri_scene::GeometryData& localPlayerReflectionGeometry = mSelectLocalPlayerReflectionGeometryScratch;
	NRILocalPlayerReflectionCaptureStats localPlayerReflectionCaptureStats = {};
	nri_scene::GeometryBuildTraceStats localPlayerReflectionGeometryTraceStats = {};
	std::vector<SceneBufferUploadDomainSpan> sceneUploadDomainSpans;
	uint32_t activeStaticProbePrimitiveCount = 0;
	EmissiveSamplingBuildContext emissiveSamplingContext = {};
	bool sceneLightUsesStaticMapScene = false;
	bool hasSurfaceLightOverlayForFrame = false;
	nri_scene::SceneDebugStats& activeStats = frame.activeStats;
	bool& paletteReady = frame.paletteReady;
	bool& texturesReady = frame.texturesReady;
	bool& buffersReady = frame.buffersReady;
	bool& accelerationReady = frame.accelerationReady;
	uint32_t combinedOverlayMaterialOffset = 0;
	bool& usingPersistentDynamicEmissiveCache = frame.usingPersistentDynamicEmissiveCache;
	bool liveDynamicHasEmissive = false;
	bool hasPersistentVoxelBatch = false;
	bool appendPersistentVoxelSceneLights = false;
	uint32_t selectedStaticSceneInstanceCount = 0;
	uint32_t selectedDynamicSceneInstanceCount = 0;
	uint32_t selectedPersistentVoxelSceneInstanceCount = 0;
	uint32_t selectedSceneInstanceCount = 0;
	uint32_t selectedTlasInstanceCount = 0;
	{
		ScopedPtPerfTimer sceneSelectTimer(mLastPerfShellTraceStats.sceneSelectMs);
		const bool staticMapSceneReady = allowStaticMapScene && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectStaticMapMs);
			return EnsureStaticMapScene();
		}();
		NRISceneFramePathSelectionInputs pathSelectionInputs = {};
		pathSelectionInputs.allowStaticMapScene = allowStaticMapScene;
		pathSelectionInputs.staticMapSceneReady = staticMapSceneReady;
		const NRISceneFramePathSelectionResult pathSelection = SelectNRISceneFramePath(pathSelectionInputs);
		const bool hasStaticMapScene = pathSelection.hasStaticMapScene;
		if (hasStaticMapScene)
		{
			sceneLightUsesStaticMapScene = true;
			emissiveSamplingContext.staticGeometry = &mStaticMapScene.geometry;
			mUsedStaticMapSceneLastFrame = true;
			activeSceneView = &mStaticMapScene.sceneView;
			activeGeometry = &mStaticMapScene.geometry;
			activeGpuMaterials = &mStaticMapScene.gpuMaterials;
			activeMaterialBridge = &mStaticMapScene.materialBridge;
			activeStaticProbePrimitiveCount = (uint32_t)mStaticMapScene.geometry.primitives.size();
			activeStats = mStaticMapScene.sceneView.stats;

			bool residentStaticWorldGeometryChanged = false;
			NRISceneFrameOverlayDeferralInputs deferralInputs = {};
			deferralInputs.uploadedStaticMapSceneLastFrame = mUploadedStaticMapSceneLastFrame;
			deferralInputs.builtStaticMapSceneASLastFrame = mBuiltStaticMapSceneASLastFrame;
			const NRISceneFrameOverlayDeferralResult deferral = SelectNRISceneFrameOverlayDeferral(deferralInputs);
			const bool deferOverlayThisFrame = deferral.deferOverlayThisFrame;
			const bool hasRuntimeSpaceLinkOverlay = !deferOverlayThisFrame && [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeSpaceLinkMs);
				return BuildRuntimeSpaceLinkOverlay(di, runtimeSpaceLinkGeometry, runtimeSpaceLinkMaterialBridge);
			}();
			mLastPerfShellTraceStats.runtimeSpaceLinkPrimitiveCount = (uint32_t)runtimeSpaceLinkGeometry.primitives.size();
			mLastPerfShellTraceStats.runtimeSpaceLinkMaterialCount = (uint32_t)runtimeSpaceLinkMaterialBridge.materials.size();
			const bool hasRuntimeMutationOverlay = [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationMs);
				const bool hasOverlay = mRuntimeMutation.BuildFrameOverlay(
					NRIRuntimeMutationSystem::BuildOverlayServices(*this),
					runtimeMutationFrame);
				residentStaticWorldGeometryChanged = runtimeMutationFrame.residentStaticSceneChanged;
				return hasOverlay;
			}();
			const bool hasDynamicScene = !deferOverlayThisFrame && [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.dynamicCaptureMs);
				(void)nri_scene::ConsumeDynamicCapturePerfStats();
				const bool captured = nri_scene::CaptureDynamicScene(di, dynamicSceneView);
				const nri_scene::DynamicCapturePerfStats captureStats = nri_scene::ConsumeDynamicCapturePerfStats();
				mLastPerfShellTraceStats.dynamicCaptureCalls += captureStats.calls;
				mLastPerfShellTraceStats.dynamicCaptureWallSurfaces += captureStats.wallSurfaces;
				mLastPerfShellTraceStats.dynamicCaptureFlatSurfaces += captureStats.flatSurfaces;
				mLastPerfShellTraceStats.dynamicCaptureSpriteSurfaces += captureStats.spriteSurfaces;
				mLastPerfShellTraceStats.dynamicCaptureVoxelProxySurfaces += captureStats.voxelProxySurfaces;
				mLastPerfShellTraceStats.dynamicCaptureUnsupportedModelSurfaces += captureStats.unsupportedModelSurfaces;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCacheStores += captureStats.voxelCacheStores;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCacheRebuilds += captureStats.voxelCacheRebuilds;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCacheDeferred += captureStats.voxelCacheDeferred;
				mLastPerfShellTraceStats.dynamicCaptureVoxelMeshBuilds += captureStats.voxelMeshCacheBuilds;
				mLastPerfShellTraceStats.dynamicCaptureVoxelMeshDeferred += captureStats.voxelMeshCacheDeferred;
				mLastPerfShellTraceStats.dynamicCaptureVoxelMeshHits += captureStats.voxelMeshCacheHits;
				mLastPerfShellTraceStats.dynamicCaptureVoxelMeshMisses += captureStats.voxelMeshCacheMisses;
				mLastPerfShellTraceStats.dynamicCaptureVoxelMeshInvalid += captureStats.voxelMeshCacheInvalid;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCanonicalSurfaceBuilds += captureStats.voxelCanonicalSurfaceBuilds;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCanonicalSurfaceHits += captureStats.voxelCanonicalSurfaceHits;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCanonicalSurfaceInvalid += captureStats.voxelCanonicalSurfaceInvalid;
				mLastPerfShellTraceStats.dynamicCaptureModelActorCandidates += captureStats.modelActorCandidates;
				mLastPerfShellTraceStats.dynamicCaptureModelActorSorted += captureStats.modelActorSorted;
				mLastPerfShellTraceStats.dynamicCaptureModelActorSortSkipped += captureStats.modelActorSortSkipped;
				mLastPerfShellTraceStats.dynamicCaptureModelScratchReuses += captureStats.modelScratchReuses;
				mLastPerfShellTraceStats.dynamicCaptureModelScratchGrows += captureStats.modelScratchGrows;
				mLastPerfShellTraceStats.dynamicCaptureModelScratchFallbacks += captureStats.modelScratchFallbacks;
				mLastPerfShellTraceStats.dynamicCaptureModelBudgetTruncations += captureStats.modelBudgetTruncations;
				mLastPerfShellTraceStats.dynamicCaptureModelSurfaceBuilds += captureStats.modelSurfaceBuilds;
				mLastPerfShellTraceStats.voxelCacheActorEntries = dynamicSceneView.stats.voxelCacheEntries;
				mLastPerfShellTraceStats.voxelCacheActorSurfaces = dynamicSceneView.stats.voxelCacheActorSurfaces;
				mLastPerfShellTraceStats.voxelCacheUniqueMeshKeys = dynamicSceneView.stats.voxelCacheUniqueMeshKeys;
				mLastPerfShellTraceStats.voxelCacheUniqueMaterialKeys = dynamicSceneView.stats.voxelCacheUniqueMaterialKeys;
				mLastPerfShellTraceStats.voxelCacheLocalSpaceSurfaces = dynamicSceneView.stats.voxelCacheLocalSpaceSurfaces;
				mLastPerfShellTraceStats.voxelCacheBakedTransformSurfaces = dynamicSceneView.stats.voxelCacheBakedTransformSurfaces;
				mLastPerfShellTraceStats.voxelCacheUnknownSpaceSurfaces = dynamicSceneView.stats.voxelCacheUnknownSpaceSurfaces;
				mLastPerfShellTraceStats.voxelCacheTransformKeyedSurfaces = dynamicSceneView.stats.voxelCacheTransformKeyedSurfaces;
				mLastPerfShellTraceStats.voxelCacheUniqueTransformBases = dynamicSceneView.stats.voxelCacheUniqueTransformBases;
				mLastPerfShellTraceStats.voxelCacheInvariantWarnings = dynamicSceneView.stats.voxelCacheInvariantWarnings;
				mLastPerfShellTraceStats.voxelCacheActorPrimitives = dynamicSceneView.stats.voxelCachePrimitives;
				mLastPerfShellTraceStats.voxelCacheDuplicatedVertexBytes = dynamicSceneView.stats.voxelCacheDuplicatedVertexBytes;
				mLastPerfShellTraceStats.voxelCacheDuplicatedIndexBytes = dynamicSceneView.stats.voxelCacheDuplicatedIndexBytes;
				mLastPerfShellTraceStats.voxelCacheDuplicatedPrimitiveBytes = dynamicSceneView.stats.voxelCacheDuplicatedPrimitiveBytes;
				mLastPerfShellTraceStats.voxelCacheDuplicatedTotalBytes = dynamicSceneView.stats.voxelCacheDuplicatedTotalBytes;
				mLastPerfShellTraceStats.voxelCacheDuplicateTopCount = dynamicSceneView.stats.voxelCacheDuplicateTopCount;
				mLastPerfShellTraceStats.voxelCacheDuplicateTopEntries = dynamicSceneView.stats.voxelCacheDuplicateTopEntries;
				mLastPerfShellTraceStats.dynamicVoxelEscapeActorCount = dynamicSceneView.stats.dynamicVoxelEscapeActorCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapeEligibleActorCount = dynamicSceneView.stats.dynamicVoxelEscapeEligibleActorCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapeForcedActorCount = dynamicSceneView.stats.dynamicVoxelEscapeForcedActorCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapePrimitiveCount = dynamicSceneView.stats.dynamicVoxelEscapePrimitiveCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapeVertexBytes = dynamicSceneView.stats.dynamicVoxelEscapeVertexBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapeIndexBytes = dynamicSceneView.stats.dynamicVoxelEscapeIndexBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapePrimitiveBytes = dynamicSceneView.stats.dynamicVoxelEscapePrimitiveBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapeMaterialBytes = dynamicSceneView.stats.dynamicVoxelEscapeMaterialBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapeTotalBytes = dynamicSceneView.stats.dynamicVoxelEscapeTotalBytes;
				mLastPerfShellTraceStats.dynamicVoxelExpectedEscapeActorCount = dynamicSceneView.stats.dynamicVoxelExpectedEscapeActorCount;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapeActorCount = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapeActorCount;
				mLastPerfShellTraceStats.dynamicVoxelExpectedEscapePrimitiveCount = dynamicSceneView.stats.dynamicVoxelExpectedEscapePrimitiveCount;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapePrimitiveCount = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapePrimitiveCount;
				mLastPerfShellTraceStats.dynamicVoxelExpectedEscapeTotalBytes = dynamicSceneView.stats.dynamicVoxelExpectedEscapeTotalBytes;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapeTotalBytes = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapeTotalBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapeTopCount = dynamicSceneView.stats.dynamicVoxelEscapeTopCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapeTopEntries = dynamicSceneView.stats.dynamicVoxelEscapeTopEntries;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapeTopCount = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapeTopCount;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapeTopEntries = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapeTopEntries;
				mLastPerfShellTraceStats.dynamicCaptureCountMs += captureStats.countMs;
				mLastPerfShellTraceStats.dynamicCaptureWallsMs += captureStats.wallsMs;
				mLastPerfShellTraceStats.dynamicCaptureFlatsMs += captureStats.flatsMs;
				mLastPerfShellTraceStats.dynamicCaptureFacingSpritesMs += captureStats.facingSpritesMs;
				mLastPerfShellTraceStats.dynamicCaptureModelSpritesMs += captureStats.modelSpritesMs;
				mLastPerfShellTraceStats.dynamicCaptureModelClassifyMs += captureStats.modelClassifyMs;
				mLastPerfShellTraceStats.dynamicCaptureModelMeshMs += captureStats.modelMeshMs;
				mLastPerfShellTraceStats.dynamicCaptureModelMeshBuildMs += captureStats.modelMeshBuildMs;
				mLastPerfShellTraceStats.dynamicCaptureModelSurfaceMs += captureStats.modelSurfaceMs;
				mLastPerfShellTraceStats.dynamicCaptureModelSortMs += captureStats.modelSortMs;
				mLastPerfShellTraceStats.dynamicCaptureModelStoreMs += captureStats.modelStoreMs;
				mLastPerfShellTraceStats.dynamicCaptureVoxelFrameMs += captureStats.voxelFrameMs;
				mLastPerfShellTraceStats.dynamicCaptureStatsMs += captureStats.statsMs;
				return captured;
			}();
		const bool shouldCaptureReflectionPlayer =
			!deferOverlayThisFrame &&
			HasPlainMirrorMaterialSurfaces(mMapWorld);
		const bool hasLocalPlayerReflectionScene = shouldCaptureReflectionPlayer && [&]()
		{
			ScopedPtPerfTimer localPlayerReflectionTimer(mLastPerfShellTraceStats.localPlayerReflectionCaptureMs);
			NRILocalPlayerReflectionCaptureRequest request = {};
			request.drawInfo = &di;
			request.rebuildSceneViewStats = RebuildSceneViewStats;
			const NRILocalPlayerReflectionCaptureResult result =
				CaptureNRILocalPlayerReflectionDynamicScene(request, localPlayerReflectionSceneView);
			localPlayerReflectionCaptureStats = result.stats;
			return result.captured;
		}();
		if (hasDynamicScene)
		{
			{
				Clocker clock(NriPTGeometryBuild);
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildDynamicLiveMs);
				nri_scene::BuildGeometry(dynamicSceneView, dynamicGeometry);
				AssignGeometryPortalIndices(mMapWorld, dynamicGeometry);
			}
			mLastPerfShellTraceStats.geometryBuildDynamicLivePrimitives += (uint32_t)dynamicGeometry.primitives.size();

			if (!dynamicGeometry.primitives.empty())
			{
				{
					Clocker clock(NriPTMaterialBuild);
					BuildMaterialsWithActorOverrides(dynamicSceneView, dynamicMaterialBridge, "dynamic_live");
				}
			}

			sceneLightDynamicView = &dynamicSceneView;
			sceneLightDynamicMaterials = &dynamicMaterialBridge;
			activeDynamicSceneView = &dynamicSceneView;
			activeDynamicGeometry = &dynamicGeometry;
			activeDynamicMaterials = &dynamicMaterialBridge;
			liveDynamicHasEmissive = [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.persistentDynamicMs);
				return mSceneLights.RebuildPersistentDynamicEmissiveCache(
					dynamicSceneView,
					dynamicMaterialBridge,
					BuildPersistentDynamicEmissiveCacheServices());
			}();
		}
		if (hasLocalPlayerReflectionScene)
		{
			{
				Clocker clock(NriPTGeometryBuild);
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildLocalPlayerReflectionMs);
				{
					ScopedPtPerfTimer buildTimer(mLastPerfShellTraceStats.localPlayerReflectionGeometryBuildMs);
					nri_scene::BuildGeometry(localPlayerReflectionSceneView, localPlayerReflectionGeometry, &localPlayerReflectionGeometryTraceStats, true);
				}
				{
					ScopedPtPerfTimer portalTimer(mLastPerfShellTraceStats.localPlayerReflectionPortalAssignMs);
					AssignGeometryPortalIndices(mMapWorld, localPlayerReflectionGeometry);
				}
				mLastPerfShellTraceStats.localPlayerReflectionGeometryBuildWallMs = localPlayerReflectionGeometryTraceStats.wallMs;
				mLastPerfShellTraceStats.localPlayerReflectionGeometryBuildFlatMs = localPlayerReflectionGeometryTraceStats.flatMs;
				mLastPerfShellTraceStats.localPlayerReflectionGeometryBuildSpriteMs = localPlayerReflectionGeometryTraceStats.spriteMs;
				mLastPerfShellTraceStats.localPlayerReflectionCaptureRawFacingSprites = localPlayerReflectionCaptureStats.rawFacingSprites;
				mLastPerfShellTraceStats.localPlayerReflectionCaptureRawVoxelSprites = localPlayerReflectionCaptureStats.rawVoxelSprites;
				mLastPerfShellTraceStats.localPlayerReflectionCaptureSurfaces = localPlayerReflectionCaptureStats.capturedSurfaceCount;
				mLastPerfShellTraceStats.localPlayerReflectionCaptureMatchingActorSurfaces = localPlayerReflectionCaptureStats.capturedMatchingActorSurfaces;
				mLastPerfShellTraceStats.localPlayerReflectionCaptureOtherActorSurfaces = localPlayerReflectionCaptureStats.capturedOtherActorSurfaces;
				mLastPerfShellTraceStats.localPlayerReflectionCaptureActorlessSurfaces = localPlayerReflectionCaptureStats.capturedActorlessSurfaces;
				mLastPerfShellTraceStats.localPlayerReflectionCaptureFilteredSurfaces = localPlayerReflectionCaptureStats.filteredSurfaceCount;
				mLastPerfShellTraceStats.localPlayerReflectionGeometryWallSurfaces = localPlayerReflectionGeometryTraceStats.wallSurfaces;
				mLastPerfShellTraceStats.localPlayerReflectionGeometryFlatSurfaces = localPlayerReflectionGeometryTraceStats.flatSurfaces;
				mLastPerfShellTraceStats.localPlayerReflectionGeometrySpriteSurfaces = localPlayerReflectionGeometryTraceStats.spriteSurfaces;
				mLastPerfShellTraceStats.localPlayerReflectionGeometryIndexedSurfaces = localPlayerReflectionGeometryTraceStats.indexedSurfaces;
				mLastPerfShellTraceStats.localPlayerReflectionGeometryTriangleFanSurfaces = localPlayerReflectionGeometryTraceStats.triangleFanSurfaces;
				mLastPerfShellTraceStats.localPlayerReflectionGeometrySpriteStripSurfaces = localPlayerReflectionGeometryTraceStats.spriteStripSurfaces;
				mLastPerfShellTraceStats.localPlayerReflectionGeometrySkippedSurfaces = localPlayerReflectionGeometryTraceStats.skippedSurfaces;
				mLastPerfShellTraceStats.localPlayerReflectionGeometrySourceVertices = localPlayerReflectionGeometryTraceStats.sourceVertexCount;
				mLastPerfShellTraceStats.localPlayerReflectionGeometrySourceIndices = localPlayerReflectionGeometryTraceStats.sourceIndexCount;
				mLastPerfShellTraceStats.localPlayerReflectionGeometryVertexGrowths = localPlayerReflectionGeometryTraceStats.vertexCapacityGrowths;
				mLastPerfShellTraceStats.localPlayerReflectionGeometryIndexGrowths = localPlayerReflectionGeometryTraceStats.indexCapacityGrowths;
				mLastPerfShellTraceStats.localPlayerReflectionGeometryPrimitiveGrowths = localPlayerReflectionGeometryTraceStats.primitiveCapacityGrowths;
				mLastPerfShellTraceStats.localPlayerReflectionGeometryProvenanceGrowths = localPlayerReflectionGeometryTraceStats.provenanceCapacityGrowths;
			}

			if (!localPlayerReflectionGeometry.primitives.empty())
			{
				Clocker clock(NriPTMaterialBuild);
				ScopedPtPerfTimer materialTimer(mLastPerfShellTraceStats.localPlayerReflectionMaterialBuildMs);
				BuildMaterialsWithActorOverrides(localPlayerReflectionSceneView, localPlayerReflectionMaterialBridge, "local_player_reflection");
			}
		}

		hasPersistentVoxelBatch = !deferOverlayThisFrame && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectPersistentVoxelBatchMs);
			const MemoryTelemetry telemetry = GetMemoryTelemetry();
			{
				ScopedPtPerfTimer admissionPumpTimer(mLastPerfShellTraceStats.sceneSelectPersistentVoxelAdmissionPumpMs);
				mPersistentVoxels.PumpAdmissionQueue(
					"runtime",
					mMapWorld.buildSerial,
					mFrameIndex,
					persistentVoxelSettings,
					telemetry.totalTrackedBytes,
					mFrameBuffer != nullptr ? mFrameBuffer->GetAdapterLocalBudgetBytes() : 0ull,
					(int)nri_ptloadingtrace,
					(bool)nri_voxelstats,
					BuildNRIPersistentVoxelResetServices(*this),
					BuildNRIPersistentVoxelAdmissionServices(*this));
			}
			return EnsurePersistentVoxelBatch();
		}();

		PersistentDynamicSurfaceStats persistentDynamicStats = {};
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectPersistentEmissiveMs);
			mSceneLights.PrunePersistentDynamicEmissiveCacheToLiveActors(BuildPersistentDynamicEmissiveCacheServices());
			persistentDynamicStats = mSceneLights.GatherPersistentDynamicEmissiveSurfaceStats();
			mSceneLights.UpdatePersistentDynamicEmissiveHighWaterStats(persistentDynamicStats);
		}
		mLastPerfShellTraceStats.persistentDynamicActorSurfaceCount = persistentDynamicStats.actorSurfaceCount;
		mLastPerfShellTraceStats.persistentDynamicNonActorSurfaceCount = persistentDynamicStats.nonActorSurfaceCount;
		mLastPerfShellTraceStats.persistentDynamicWallSurfaceCount = persistentDynamicStats.wallSurfaceCount;
		mLastPerfShellTraceStats.persistentDynamicFlatSurfaceCount = persistentDynamicStats.flatSurfaceCount;
		mLastPerfShellTraceStats.persistentDynamicSpriteSurfaceCount = persistentDynamicStats.spriteSurfaceCount;
		const PersistentDynamicEmissiveHighWaterStats& persistentDynamicHighWater = mSceneLights.GetPersistentDynamicEmissiveHighWaterStats();
		mLastPerfShellTraceStats.persistentDynamicHighWaterSurfaceCount = persistentDynamicHighWater.surfaceCount;
		mLastPerfShellTraceStats.persistentDynamicHighWaterPrimitiveCount = persistentDynamicHighWater.primitiveCount;
		mLastPerfShellTraceStats.persistentDynamicHighWaterMaterialCount = persistentDynamicHighWater.materialCount;
		mLastPerfShellTraceStats.persistentDynamicHighWaterActorSurfaceCount = persistentDynamicHighWater.surfaceStats.actorSurfaceCount;
		mLastPerfShellTraceStats.persistentDynamicHighWaterSpriteSurfaceCount = persistentDynamicHighWater.surfaceStats.spriteSurfaceCount;

		const PersistentDynamicEmissiveCache& persistentDynamicCache = mSceneLights.GetPersistentDynamicEmissiveCache();
		const bool shouldUsePersistentDynamicEmissive = persistentDynamicCache.valid;
		if (shouldUsePersistentDynamicEmissive)
		{
			usingPersistentDynamicEmissiveCache = true;
			if (hasDynamicScene)
			{
				ScopedPtPerfTimer mergePerfTimer(mLastPerfShellTraceStats.sceneSelectDynamicMergeMs);
				{
					ScopedPtPerfTimer copyPerfTimer(mLastPerfShellTraceStats.sceneSelectDynamicMergeCopyMs);
					mergedDynamicSceneView = dynamicSceneView;
				}
				const SceneLightSystem::PersistentDynamicMergeStats mergeStats = [&]()
				{
					ScopedPtPerfTimer appendPerfTimer(mLastPerfShellTraceStats.sceneSelectDynamicMergeAppendMs);
					return mSceneLights.MergePersistentDynamicEmissiveCacheIntoSceneView(mergedDynamicSceneView);
				}();
				mLastPerfShellTraceStats.dynamicMergeLiveSurfaceCount = mergeStats.LiveSurfaceCount();
				mLastPerfShellTraceStats.dynamicMergePersistentCacheSurfaceCount = mergeStats.CacheSurfaceCount();
				mLastPerfShellTraceStats.dynamicMergeAppendedPersistentSurfaceCount = mergeStats.AppendedSurfaceCount();
				mLastPerfShellTraceStats.dynamicMergeDuplicatePersistentSurfaceCount = mergeStats.DuplicateSurfaceCount();
				mLastPerfShellTraceStats.dynamicMergeAppendedPersistentPrimitiveCount = persistentDynamicCache.primitiveCount;
				mLastPerfShellTraceStats.dynamicMergeAppendedPersistentMaterialCount = persistentDynamicCache.materialCount;

				mLastPerfShellTraceStats.dynamicMergeDeltaAppendAttempts++;
				if (mergeStats.AppendedSurfaceCount() == 0)
				{
					mLastPerfShellTraceStats.dynamicMergeDeltaAppendUsed++;
				}
				else
				{
					mLastPerfShellTraceStats.dynamicMergeDeltaAppendFallbacks++;
					mLastPerfShellTraceStats.dynamicMergeDeltaAppendFallbackNonZeroSurfaces++;
					{
						ScopedPtPerfTimer statsPerfTimer(mLastPerfShellTraceStats.sceneSelectDynamicMergeStatsMs);
						RebuildSceneViewStats(mergedDynamicSceneView);
					}
					{
						Clocker clock(NriPTGeometryBuild);
						ScopedPtPerfTimer geometryPerfTimer(mLastPerfShellTraceStats.sceneSelectDynamicMergeGeometryMs);
						ScopedPtPerfTimer legacyGeometryPerfTimer(mLastPerfShellTraceStats.geometryBuildMergedDynamicMs);
						nri_scene::BuildGeometry(mergedDynamicSceneView, mergedDynamicGeometry);
					}
					{
						ScopedPtPerfTimer portalPerfTimer(mLastPerfShellTraceStats.sceneSelectDynamicMergePortalAssignMs);
						AssignGeometryPortalIndices(mMapWorld, mergedDynamicGeometry);
					}
					{
						Clocker clock(NriPTMaterialBuild);
						ScopedPtPerfTimer materialPerfTimer(mLastPerfShellTraceStats.sceneSelectDynamicMergeMaterialMs);
						BuildMaterialsWithActorOverrides(mergedDynamicSceneView, mergedDynamicMaterialBridge, "dynamic_with_persistent_emissive");
					}

					if (!mergedDynamicGeometry.primitives.empty())
					{
						activeDynamicSceneView = &mergedDynamicSceneView;
						activeDynamicGeometry = &mergedDynamicGeometry;
						activeDynamicMaterials = &mergedDynamicMaterialBridge;
					}
				}
			}
			else
			{
				activeDynamicSceneView = &persistentDynamicCache.sceneView;
				activeDynamicGeometry = &persistentDynamicCache.geometry;
				activeDynamicMaterials = &persistentDynamicCache.materialBridge;
			}

			if (activeDynamicSceneView != nullptr && activeDynamicMaterials != nullptr)
			{
				sceneLightDynamicView = activeDynamicSceneView;
				sceneLightDynamicMaterials = activeDynamicMaterials;
			}
		}

		if (hasPersistentVoxelBatch && mPersistentVoxels.HasValidBatch())
		{
			appendPersistentVoxelSceneLights = true;
		}

		const bool runtimeDebugSphereBuilt = !deferOverlayThisFrame && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeDebugSphereMs);
			NRIDebugOverlayBuildTelemetry debugOverlayTelemetry = {};
			const bool built = mDebugOverlays.BuildRuntimeDebugSphereOverlay(
				debugSphereGeometry,
				debugSphereMaterialBridge,
				debugOverlayTelemetry,
				ShouldCollectPtPerfTiming());
			mLastPerfShellTraceStats.runtimeDebugSphereViewMs += debugOverlayTelemetry.runtimeDebugSphereViewMs;
			mLastPerfShellTraceStats.runtimeDebugSphereGeoMs += debugOverlayTelemetry.runtimeDebugSphereGeoMs;
			mLastPerfShellTraceStats.runtimeDebugSphereMaterialMs += debugOverlayTelemetry.runtimeDebugSphereMaterialMs;
			mLastPerfShellTraceStats.geometryBuildDebugSphereMs += debugOverlayTelemetry.geometryBuildDebugSphereMs;
			mLastPerfShellTraceStats.runtimeDebugSphereCount = debugOverlayTelemetry.runtimeDebugSphereCount;
			mLastPerfShellTraceStats.runtimeDebugSphereLongitudeSegments = debugOverlayTelemetry.runtimeDebugSphereLongitudeSegments;
			mLastPerfShellTraceStats.runtimeDebugSphereLatitudeSegments = debugOverlayTelemetry.runtimeDebugSphereLatitudeSegments;
			mLastPerfShellTraceStats.runtimeDebugSpherePrimitiveCount = debugOverlayTelemetry.runtimeDebugSpherePrimitiveCount;
			mLastPerfShellTraceStats.runtimeDebugSphereMaterialCount = debugOverlayTelemetry.runtimeDebugSphereMaterialCount;
			return built;
		}();
		const bool surfaceLightBuilt = !deferOverlayThisFrame &&
			BuildSurfaceLightOverlay(surfaceLightSceneView, surfaceLightGeometry, surfaceLightMaterialBridge);
		NRISceneFrameOverlayEligibilityInputs overlayEligibilityInputs = {};
		overlayEligibilityInputs.deferOverlayThisFrame = deferOverlayThisFrame;
		overlayEligibilityInputs.runtimeSpaceLinkBuilt = hasRuntimeSpaceLinkOverlay;
		overlayEligibilityInputs.runtimeMutationBuilt = hasRuntimeMutationOverlay;
		overlayEligibilityInputs.hasPersistentVoxelBatch = hasPersistentVoxelBatch;
		overlayEligibilityInputs.persistentVoxelRenderable = hasPersistentVoxelBatch ? mPersistentVoxels.HasRenderableOverlay() : false;
		overlayEligibilityInputs.activeDynamicGeometry = activeDynamicGeometry;
		overlayEligibilityInputs.activeDynamicMaterials = activeDynamicMaterials;
		overlayEligibilityInputs.hasLocalPlayerReflectionScene = hasLocalPlayerReflectionScene;
		overlayEligibilityInputs.localPlayerReflectionGeometry = &localPlayerReflectionGeometry;
		overlayEligibilityInputs.localPlayerReflectionMaterials = &localPlayerReflectionMaterialBridge;
		overlayEligibilityInputs.runtimeDebugSphereBuilt = runtimeDebugSphereBuilt;
		overlayEligibilityInputs.surfaceLightBuilt = surfaceLightBuilt;
		const NRISceneFrameOverlayEligibilityResult overlayEligibility =
			SelectNRISceneFrameOverlayEligibility(overlayEligibilityInputs);
		const bool hasRuntimeSpaceLinkOverlayForAssembly = overlayEligibility.hasRuntimeSpaceLinkOverlay;
		const bool hasRuntimeMutationOverlayForAssembly = overlayEligibility.hasRuntimeMutationOverlay;
		const bool hasPersistentVoxelOverlay = overlayEligibility.hasPersistentVoxelOverlay;
		const bool hasActiveDynamicOverlay = overlayEligibility.hasActiveDynamicOverlay;
		const bool hasLocalPlayerReflectionOverlay = overlayEligibility.hasLocalPlayerReflectionOverlay;
		const bool hasRuntimeDebugSphereOverlay = overlayEligibility.hasRuntimeDebugSphereOverlay;
		const bool hasSurfaceLightOverlay = overlayEligibility.hasSurfaceLightOverlay;
		hasSurfaceLightOverlayForFrame = hasSurfaceLightOverlay;

		if (overlayEligibility.hasAnyOverlay)
		{
			NRIPersistentVoxelOverlayStats persistentVoxelOverlayStats = {};
			if (hasPersistentVoxelOverlay)
			{
				persistentVoxelOverlayStats = mPersistentVoxels.BuildOverlayStats();
			}

			NRISceneFrameOverlayBuildInputs overlayInputs = {};
			overlayInputs.collectTiming = ShouldCollectPtPerfTiming();
			overlayInputs.mapWorldBuildSerial = mMapWorld.buildSerial;
			overlayInputs.frameIndex = mFrameIndex;
			overlayInputs.stats = &mLastPerfShellTraceStats;
			overlayInputs.hasPersistentVoxelOverlay = hasPersistentVoxelOverlay;
			overlayInputs.persistentVoxelOverlayStats = hasPersistentVoxelOverlay ? &persistentVoxelOverlayStats : nullptr;

			overlayInputs.hasRuntimeSpaceLinkOverlay = hasRuntimeSpaceLinkOverlayForAssembly;
			overlayInputs.runtimeSpaceLinkGeometry = &runtimeSpaceLinkGeometry;
			overlayInputs.runtimeSpaceLinkMaterials = &runtimeSpaceLinkMaterialBridge;
			overlayInputs.runtimeSpaceLinkTelemetry = {
				&mLastPerfShellTraceStats.overlayRuntimeSpaceLinkMs,
				&mLastPerfShellTraceStats.overlayRuntimeSpaceLinkGeometryMs,
				&mLastPerfShellTraceStats.overlayRuntimeSpaceLinkMaterialMs,
				&mLastPerfShellTraceStats.overlayRuntimeSpaceLinkPrimitiveCount,
				&mLastPerfShellTraceStats.overlayRuntimeSpaceLinkMaterialCount,
				&mLastPerfShellTraceStats.overlayRuntimeSpaceLinkAppend };

			overlayInputs.hasRuntimeMutationOverlay = hasRuntimeMutationOverlayForAssembly;
			overlayInputs.runtimeMutationGeometry = &runtimeMutationFrame.geometry;
			overlayInputs.runtimeMutationMaterials = &runtimeMutationFrame.materialBridge;
			overlayInputs.runtimeMutationTelemetry = {
				&mLastPerfShellTraceStats.overlayRuntimeMutationMs,
				&mLastPerfShellTraceStats.overlayRuntimeMutationGeometryMs,
				&mLastPerfShellTraceStats.overlayRuntimeMutationMaterialMs,
				&mLastPerfShellTraceStats.overlayRuntimeMutationPrimitiveCount,
				&mLastPerfShellTraceStats.overlayRuntimeMutationMaterialCount,
				&mLastPerfShellTraceStats.overlayRuntimeMutationAppend };

			overlayInputs.hasActiveDynamicOverlay = hasActiveDynamicOverlay;
			overlayInputs.activeDynamicSceneView = activeDynamicSceneView;
			overlayInputs.activeDynamicGeometry = activeDynamicGeometry;
			overlayInputs.activeDynamicMaterials = activeDynamicMaterials;
			overlayInputs.activeDynamicTelemetry = {
				&mLastPerfShellTraceStats.overlayDynamicMs,
				&mLastPerfShellTraceStats.overlayDynamicGeometryMs,
				&mLastPerfShellTraceStats.overlayDynamicMaterialMs,
				&mLastPerfShellTraceStats.overlayDynamicPrimitiveCount,
				&mLastPerfShellTraceStats.overlayDynamicMaterialCount,
				&mLastPerfShellTraceStats.overlayDynamicAppend };

			overlayInputs.hasLocalPlayerReflectionOverlay = hasLocalPlayerReflectionOverlay;
			overlayInputs.localPlayerReflectionGeometry = &localPlayerReflectionGeometry;
			overlayInputs.localPlayerReflectionMaterials = &localPlayerReflectionMaterialBridge;
			overlayInputs.localPlayerReflectionTelemetry = {
				&mLastPerfShellTraceStats.overlayLocalPlayerReflectionMs,
				&mLastPerfShellTraceStats.overlayLocalPlayerReflectionGeometryMs,
				&mLastPerfShellTraceStats.overlayLocalPlayerReflectionMaterialMs,
				&mLastPerfShellTraceStats.overlayLocalPlayerReflectionPrimitiveCount,
				&mLastPerfShellTraceStats.overlayLocalPlayerReflectionMaterialCount,
				&mLastPerfShellTraceStats.overlayLocalPlayerReflectionAppend };

			overlayInputs.hasRuntimeDebugSphereOverlay = hasRuntimeDebugSphereOverlay;
			overlayInputs.runtimeDebugSphereGeometry = &debugSphereGeometry;
			overlayInputs.runtimeDebugSphereMaterials = &debugSphereMaterialBridge;
			overlayInputs.runtimeDebugSphereTelemetry = {
				&mLastPerfShellTraceStats.overlayDebugSphereMs,
				&mLastPerfShellTraceStats.overlayDebugSphereGeometryMs,
				&mLastPerfShellTraceStats.overlayDebugSphereMaterialMs,
				&mLastPerfShellTraceStats.overlayDebugSpherePrimitiveCount,
				&mLastPerfShellTraceStats.overlayDebugSphereMaterialCount,
				&mLastPerfShellTraceStats.overlayDebugSphereAppend };

			double surfaceLightOverlayMs = 0.0;
			double surfaceLightGeometryMs = 0.0;
			double surfaceLightMaterialMs = 0.0;
			uint32_t surfaceLightPrimitiveCount = 0;
			uint32_t surfaceLightMaterialCount = 0;
			PerfShellTraceStats::OverlayAppendSourceTraceEntry surfaceLightAppend = {};
			overlayInputs.hasSurfaceLightOverlay = hasSurfaceLightOverlay;
			overlayInputs.surfaceLightGeometry = &surfaceLightGeometry;
			overlayInputs.surfaceLightMaterials = &surfaceLightMaterialBridge;
			overlayInputs.surfaceLightTelemetry = {
				&surfaceLightOverlayMs,
				&surfaceLightGeometryMs,
				&surfaceLightMaterialMs,
				&surfaceLightPrimitiveCount,
				&surfaceLightMaterialCount,
				&surfaceLightAppend };

			NRISceneFrameOverlayBuildOutputs overlayOutputs = {};
			overlayOutputs.overlayGeometry = &overlayGeometry;
			overlayOutputs.overlayMaterialBridge = &overlayMaterialBridge;
			overlayOutputs.uploadSpans = &sceneUploadDomainSpans;
			BuildNRISceneFrameOverlay(overlayInputs, overlayOutputs);

			auto& instances = mSelectTopLevelInstanceScratch;
			auto& sceneInstances = mSelectSceneInstanceScratch;
			instances.clear();
			sceneInstances.clear();
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectStaticInstancesMs);
				BuildStaticMapInstances(instances, sceneInstances);
			}
			const uint32_t staticSceneInstanceBaselineCount = (uint32_t)sceneInstances.size();
			selectedStaticSceneInstanceCount = staticSceneInstanceBaselineCount;
			selectedSceneInstanceCount = (uint32_t)sceneInstances.size();
			selectedTlasInstanceCount = (uint32_t)instances.size();
			bool selectedSceneHasDynamicOverlay = false;

			if (overlayGeometry.primitives.empty() && !hasPersistentVoxelOverlay)
			{
				accelerationReady =
					BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static) &&
					NRISceneUploadManager::UpdateSceneDataSet(*this,
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
						"static_only_scene");
				if (accelerationReady && hasRuntimeMutationOverlay)
				{
					mBuiltStaticMapSceneASLastFrame = false;
				}
			}
			else
			{
				{
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectMaterialBridgeMs);
					combinedMaterialBridge = mStaticMapScene.materialBridge;
					combinedOverlayMaterialOffset = (uint32_t)combinedMaterialBridge.materials.size();
					if (hasPersistentVoxelOverlay)
					{
						mPersistentVoxels.AppendMaterialBridgeTo(combinedMaterialBridge);
						combinedOverlayMaterialOffset = (uint32_t)combinedMaterialBridge.materials.size();
					}
					nri_scene::AppendMaterialBridge(overlayMaterialBridge, combinedMaterialBridge);
				}
				paletteReady = [&]()
				{
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectPaletteMs);
					return EnsurePaletteTexture(combinedMaterialBridge);
				}();
				if (ShouldTraceSkyPerf())
				{
					gRendererSkyPerfTraceStats.combinedOverlayTextureBuilds++;
				}
				texturesReady = paletteReady && [&]()
				{
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectTexturesMs);
					return EnsureSceneTextures(mStaticMapScene.sceneView, combinedMaterialBridge, combinedGpuMaterials, false, "static_map_overlay_combined");
				}();
				dynamicGpuMaterials.clear();
				persistentVoxelGpuMaterials.clear();
				if (texturesReady)
				{
					{
						ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectMaterialSplitMs);
						const size_t staticMaterialCount = mStaticMapScene.gpuMaterials.size();
						const size_t persistentVoxelMaterialCount = hasPersistentVoxelOverlay ? mPersistentVoxels.OverlayMaterialCount() : 0u;
						if (combinedGpuMaterials.size() < staticMaterialCount + persistentVoxelMaterialCount)
						{
							texturesReady = false;
						}
						else
						{
							persistentVoxelGpuMaterials.assign(
								combinedGpuMaterials.begin() + staticMaterialCount,
								combinedGpuMaterials.begin() + staticMaterialCount + persistentVoxelMaterialCount);
							dynamicGpuMaterials.assign(combinedGpuMaterials.begin() + staticMaterialCount + persistentVoxelMaterialCount, combinedGpuMaterials.end());
						}
					}
				}
				buffersReady = texturesReady && [&]()
				{
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadMs);
					return UploadSceneBuffers(overlayGeometry, dynamicGpuMaterials, &sceneUploadDomainSpans) &&
						(!hasPersistentVoxelOverlay || UploadPersistentVoxelArenaMaterialBuffers(persistentVoxelGpuMaterials));
				}();
				accelerationReady = false;
				const uint32_t liveOverlayPrimitiveCount = (uint32_t)overlayGeometry.primitives.size();
				const uint32_t liveOverlayIndexOffset = 0u;
				const uint32_t liveOverlayIndexCount = (uint32_t)overlayGeometry.indices.size();
				NRIAccelerationStructureResource& dynamicBottomLevelAS = GetCurrentDynamicBottomLevelAS();
				DynamicOverlayBlasRoute dynamicOverlayBlasRoute = {};
				bool useDynamicOverlayBlasRoute = false;
				if (buffersReady)
				{
					RecordDynamicOverlayBlasModelStats(mLastPerfShellTraceStats, sceneUploadDomainSpans);
					const bool dynamicOverlayBlasRouteReady =
						BuildDynamicOverlayBlasRoute(overlayGeometry, sceneUploadDomainSpans, dynamicOverlayBlasRoute);
					bool persistentVoxelAsReady = true;
					bool dynamicAsReady = dynamicOverlayBlasRouteReady;
					if (hasPersistentVoxelOverlay)
					{
						ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.persistentVoxelAsMs);
						NRIPersistentVoxelAccelerationBuildStats persistentVoxelAsStats = {};
						persistentVoxelAsReady = mPersistentVoxels.BuildAccelerationStructures(
							mFrameIndex,
							persistentVoxelSettings,
							(bool)nri_voxelstats,
							BuildNRIPersistentVoxelResetServices(*this),
							BuildNRIPersistentVoxelAccelerationServices(*this),
							persistentVoxelAsStats);
						mLastPerfShellTraceStats.persistentVoxelAsCalls += persistentVoxelAsStats.calls;
						mLastPerfShellTraceStats.persistentVoxelAsBuilds += persistentVoxelAsStats.builds;
						mLastPerfShellTraceStats.persistentVoxelAsUniqueMeshBuilds += persistentVoxelAsStats.uniqueMeshBuilds;
						mLastPerfShellTraceStats.persistentVoxelAsInstances += persistentVoxelAsStats.instances;
					}
					useDynamicOverlayBlasRoute =
						dynamicOverlayBlasRoute.routeAllOverlay &&
						dynamicOverlayBlasRoute.accelerationStructure != nullptr &&
						dynamicOverlayBlasRoute.accelerationStructure->accelerationStructure != nullptr;
					if (liveOverlayPrimitiveCount > 0 && !useDynamicOverlayBlasRoute)
					{
						mLastPerfShellTraceStats.dynamicAsRuntimeSpaceLinkPrimitives = mLastPerfShellTraceStats.overlayRuntimeSpaceLinkAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsRuntimeMutationPrimitives = mLastPerfShellTraceStats.overlayRuntimeMutationAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsDynamicPrimitives = mLastPerfShellTraceStats.overlayDynamicAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsLocalPlayerReflectionPrimitives = mLastPerfShellTraceStats.overlayLocalPlayerReflectionAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsDebugSpherePrimitives = mLastPerfShellTraceStats.overlayDebugSphereAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsRuntimeSpaceLinkBytes = mLastPerfShellTraceStats.overlayRuntimeSpaceLinkAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsRuntimeMutationBytes = mLastPerfShellTraceStats.overlayRuntimeMutationAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsDynamicBytes = mLastPerfShellTraceStats.overlayDynamicAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsLocalPlayerReflectionBytes = mLastPerfShellTraceStats.overlayLocalPlayerReflectionAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsDebugSphereBytes = mLastPerfShellTraceStats.overlayDebugSphereAppend.byteCount;
						dynamicAsReady =
							BuildDynamicAccelerationStructure(
								overlayGeometry,
								liveOverlayIndexOffset,
								liveOverlayIndexCount,
								liveOverlayPrimitiveCount,
								dynamicBottomLevelAS,
								true) &&
							dynamicBottomLevelAS.accelerationStructure != nullptr;
					}
					else
					{
						mLastPerfShellTraceStats.dynamicAsPrimitiveCount = 0;
						mLastPerfShellTraceStats.dynamicAsVertexCount = 0;
						mLastPerfShellTraceStats.dynamicAsIndexCount = 0;
					}
					accelerationReady = persistentVoxelAsReady && dynamicAsReady;
				}
				emissiveSamplingContext.runtimeMutationGeometry = hasRuntimeMutationOverlay ? &runtimeMutationFrame.geometry : nullptr;
				emissiveSamplingContext.runtimeMutationPrimitiveBaseOffset = (uint32_t)runtimeSpaceLinkGeometry.primitives.size();
				emissiveSamplingContext.dynamicGeometry = hasActiveDynamicOverlay ? activeDynamicGeometry : nullptr;
				emissiveSamplingContext.dynamicPrimitiveBaseOffset = (uint32_t)(runtimeSpaceLinkGeometry.primitives.size() + runtimeMutationFrame.geometry.primitives.size());
				if (hasSurfaceLightOverlay)
				{
					const SceneBufferUploadDomainSpan* surfaceLightSpan =
						FindUploadDomainSpan(sceneUploadDomainSpans, SceneBufferUploadDomain::SurfaceLightOverlay);
					if (surfaceLightSpan != nullptr)
					{
						emissiveSamplingContext.surfaceLightOverlayGeometry = &surfaceLightGeometry;
						emissiveSamplingContext.surfaceLightOverlayPrimitiveBaseOffset = surfaceLightSpan->primitiveOffset;
					}
				}
				if (accelerationReady)
				{
					if (hasPersistentVoxelOverlay)
					{
						ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectInstanceHandlesMs);
						ScopedPtPerfTimer persistentVoxelTlasTimer(mLastPerfShellTraceStats.persistentVoxelTlasInstanceMs);
						NRIPersistentVoxelTlasServices persistentVoxelTlasServices = {};
						persistentVoxelTlasServices.user = this;
						persistentVoxelTlasServices.getAccelerationStructureHandle = [](void* user, const NRIAccelerationStructureResource& resource) -> uint64_t
						{
							NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
							return resource.accelerationStructure != nullptr ?
								renderer->mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*resource.accelerationStructure) :
								0ull;
						};
						NRIPersistentVoxelTlasBuildStats persistentVoxelTlasStats = {};
						if (!mPersistentVoxels.AppendTlasInstances(
							instances,
							sceneInstances,
							mFrameIndex,
							persistentVoxelSettings,
							(bool)nri_voxelstats,
							persistentVoxelTlasServices,
							persistentVoxelTlasStats))
						{
							accelerationReady = false;
						}
						mLastPerfShellTraceStats.persistentVoxelSharedMeshResources = persistentVoxelTlasStats.sharedMeshResourceCount;
						mLastPerfShellTraceStats.persistentVoxelTlasInstances += persistentVoxelTlasStats.instanceCount;
						mLastPerfShellTraceStats.persistentVoxelBakedFallbackInstances += persistentVoxelTlasStats.bakedFallbackInstanceCount;
					}

					if (useDynamicOverlayBlasRoute)
					{
						ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectInstanceHandlesMs);
						nri::TopLevelInstance dynamicInstance = {};
						dynamicInstance.transform[0][0] = 1.0f;
						dynamicInstance.transform[1][1] = 1.0f;
						dynamicInstance.transform[2][2] = 1.0f;
						dynamicInstance.mask = NRI_TLAS_MASK_ALL_WORKLOADS;
						dynamicInstance.shaderBindingTableLocalOffset = 0;
						dynamicInstance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
						dynamicInstance.accelerationStructureHandle =
							mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*dynamicOverlayBlasRoute.accelerationStructure->accelerationStructure);
						NRIRaySceneBuilder builder(instances, sceneInstances);
						SceneInstanceData sceneRecord = {};
						sceneRecord.primitiveBase = dynamicOverlayBlasRoute.span.primitiveOffset;
						sceneRecord.dataSource = nri_diag::SceneDataSourceDynamic;
						sceneRecord.materialBase = dynamicOverlayBlasRoute.span.materialOffset;
						sceneRecord.materialCount = dynamicOverlayBlasRoute.span.materialCount;
						sceneRecord.visibilityChunk = UINT32_MAX;
						builder.AddInstance(dynamicInstance, sceneRecord);
					}
					else if (liveOverlayPrimitiveCount > 0 && dynamicBottomLevelAS.accelerationStructure != nullptr)
					{
						ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectInstanceHandlesMs);
						nri::TopLevelInstance dynamicInstance = {};
						dynamicInstance.transform[0][0] = 1.0f;
						dynamicInstance.transform[1][1] = 1.0f;
						dynamicInstance.transform[2][2] = 1.0f;
						dynamicInstance.mask = NRI_TLAS_MASK_ALL_WORKLOADS;
						dynamicInstance.shaderBindingTableLocalOffset = 0;
						dynamicInstance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
						dynamicInstance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*dynamicBottomLevelAS.accelerationStructure);
						NRIRaySceneBuilder builder(instances, sceneInstances);
						SceneInstanceData sceneRecord = {};
						sceneRecord.primitiveBase = 0u;
						sceneRecord.dataSource = nri_diag::SceneDataSourceDynamic;
						sceneRecord.materialBase = 0u;
						sceneRecord.materialCount = activeGpuMaterials != nullptr ? (uint32_t)activeGpuMaterials->size() : UINT32_MAX;
						sceneRecord.visibilityChunk = UINT32_MAX;
						builder.AddLegacyInstance(dynamicInstance, sceneRecord);
					}

					selectedStaticSceneInstanceCount = 0;
					selectedDynamicSceneInstanceCount = 0;
					selectedPersistentVoxelSceneInstanceCount = 0;
					for (const SceneInstanceData& sceneInstance : sceneInstances)
					{
						if (sceneInstance.dataSource == nri_diag::SceneDataSourceStatic)
						{
							selectedStaticSceneInstanceCount++;
						}
						else if (sceneInstance.dataSource == nri_diag::SceneDataSourceDynamic)
						{
							selectedDynamicSceneInstanceCount++;
						}
						else if (sceneInstance.dataSource == nri_diag::SceneDataSourcePersistentVoxel)
						{
							selectedPersistentVoxelSceneInstanceCount++;
						}
					}
					selectedSceneInstanceCount = (uint32_t)sceneInstances.size();
					selectedTlasInstanceCount = (uint32_t)instances.size();
					const bool hasEffectiveOverlayInstances = sceneInstances.size() > staticSceneInstanceBaselineCount;
					selectedSceneHasDynamicOverlay =
						liveOverlayPrimitiveCount > 0 ||
						selectedDynamicSceneInstanceCount > 0 ||
						selectedPersistentVoxelSceneInstanceCount > 0 ||
						hasEffectiveOverlayInstances;
					if (selectedSceneHasDynamicOverlay)
					{
						accelerationReady =
							BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static | SceneDataBufferMask_Dynamic) &&
							NRISceneUploadManager::UpdateSceneDataSet(*this,
								mStaticVertexBuffer,
								mStaticIndexBuffer,
								mStaticPrimitiveBuffer,
								mStaticMaterialBuffer,
								GetCurrentDynamicVertexBuffer(),
								GetCurrentDynamicIndexBuffer(),
								GetCurrentDynamicPrimitiveBuffer(),
								GetCurrentDynamicMaterialBuffer(),
								sceneInstances,
								(uint32_t)mStaticMapScene.geometry.primitives.size(),
								(uint32_t)overlayGeometry.primitives.size(),
								(uint32_t)mStaticMapScene.gpuMaterials.size(),
								(uint32_t)dynamicGpuMaterials.size(),
								"static_plus_overlay_scene");
					}
					else
					{
						accelerationReady =
							BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static) &&
							NRISceneUploadManager::UpdateSceneDataSet(*this,
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
								"static_only_effective_scene");
					}
				}
			}

			if (overlayGeometry.primitives.empty() || texturesReady)
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectTexturePrepMs);
				PrepareSceneTextureInputsForCompute();
			}

			if (paletteReady && texturesReady && buffersReady && accelerationReady)
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectStateCommitMs);
				{
					ScopedPtPerfTimer stateFlagsTimer(mLastPerfShellTraceStats.sceneSelectStateCommitFlagsMs);
					mUsedDynamicSceneLastFrame = selectedSceneHasDynamicOverlay;
					mGpuSceneHasDynamicOverlay = selectedSceneHasDynamicOverlay;
					mLastPerfShellTraceStats.sceneSelectStateCommitSelectedDynamic = selectedSceneHasDynamicOverlay ? 1u : 0u;
				}
				{
					NRISceneFrameDynamicStateBuildRequest dynamicStateRequest = {};
					dynamicStateRequest.activeDynamicSceneView = activeDynamicSceneView;
					dynamicStateRequest.activeDynamicGeometry = activeDynamicGeometry;
					dynamicStateRequest.activeDynamicMaterials = activeDynamicMaterials;
					dynamicStateRequest.hasLocalPlayerReflectionScene = hasLocalPlayerReflectionScene;
					dynamicStateRequest.localPlayerReflectionSceneView = &localPlayerReflectionSceneView;
					dynamicStateRequest.localPlayerReflectionGeometry = &localPlayerReflectionGeometry;
					dynamicStateRequest.localPlayerReflectionMaterials = &localPlayerReflectionMaterialBridge;
					dynamicStateRequest.totalMs = &mLastPerfShellTraceStats.sceneSelectStateCommitDynamicStateMs;
					dynamicStateRequest.dynamicCoreMs = &mLastPerfShellTraceStats.sceneSelectStateCommitDynamicCoreMs;
					dynamicStateRequest.localPlayerReflectionMs = &mLastPerfShellTraceStats.sceneSelectStateCommitDynamicLocalPlayerReflectionMs;
					const NRISceneFrameDynamicStateInputs dynamicStateInputs =
						MakeNRISceneFrameDynamicStateInputs(dynamicStateRequest);
					mDynamicSceneLastFrame = BuildNRISceneFrameDynamicState(dynamicStateInputs, mDynamicSceneLastFrame, mLastPerfShellTraceStats);
				}
				{
					NRISceneFrameGeometrySelectionInputs geometrySelectionInputs = {};
					geometrySelectionInputs.staticBuildSerial = mStaticMapScene.buildSerial;
					geometrySelectionInputs.staticGeometry = &mStaticMapScene.geometry;
					geometrySelectionInputs.staticMaterialBridge = &mStaticMapScene.materialBridge;
					geometrySelectionInputs.staticGpuMaterials = &mStaticMapScene.gpuMaterials;
					geometrySelectionInputs.overlayGeometry = &overlayGeometry;
					geometrySelectionInputs.overlayMaterialOffset = combinedOverlayMaterialOffset;
					geometrySelectionInputs.combinedMaterialBridge = &combinedMaterialBridge;
					geometrySelectionInputs.combinedGpuMaterials = &combinedGpuMaterials;
					geometrySelectionInputs.totalMs = &mLastPerfShellTraceStats.sceneSelectStateCommitGeometryStateMs;
					geometrySelectionInputs.staticCopyMs = &mLastPerfShellTraceStats.sceneSelectStateCommitGeometryStaticCopyMs;
					geometrySelectionInputs.overlayAppendMs = &mLastPerfShellTraceStats.sceneSelectStateCommitGeometryAppendMs;
					geometrySelectionInputs.selectMs = &mLastPerfShellTraceStats.sceneSelectStateCommitGeometrySelectMs;
					const NRISceneFrameGeometrySelection geometrySelection = mSceneFrameGeometry.SelectActiveGeometry(geometrySelectionInputs);
					if (geometrySelection.usedCombinedGeometry)
					{
						mLastPerfShellTraceStats.sceneSelectStateCommitGeometryCombined = 1;
					}
					if (geometrySelection.usedStaticOnlyGeometry)
					{
						mLastPerfShellTraceStats.sceneSelectStateCommitGeometryStaticOnly = 1;
					}
					activeStaticProbePrimitiveCount = geometrySelection.staticProbePrimitiveCount;
					activeGeometry = geometrySelection.geometry;
					activeGpuMaterials = geometrySelection.gpuMaterials;
					activeMaterialBridge = geometrySelection.materialBridge;
					mLastPerfShellTraceStats.sceneSelectStateCommitCombinedPrimitiveCount = geometrySelection.combinedPrimitiveCount;
					mLastPerfShellTraceStats.sceneSelectStateCommitCombinedMaterialCount = geometrySelection.combinedMaterialCount;
				}

				{
					ScopedPtPerfTimer statsTimer(mLastPerfShellTraceStats.sceneSelectStateCommitStatsMs);
					nri_scene::SceneDebugStats persistentVoxelOverlayStats;
					if (hasPersistentVoxelOverlay)
					{
						ScopedPtPerfTimer persistentVoxelStatsTimer(mLastPerfShellTraceStats.sceneSelectStateCommitStatsPersistentVoxelMs);
						persistentVoxelOverlayStats = mPersistentVoxels.BuildOverlayDebugStats();
					}
					NRISceneFrameDebugStatsBuildRequest debugStatsRequest = {};
					debugStatsRequest.staticMapStats = &mStaticMapScene.sceneView.stats;
					debugStatsRequest.deferOverlayThisFrame = deferOverlayThisFrame;
					debugStatsRequest.deferredDynamicSceneView = &dynamicSceneView;
					debugStatsRequest.activeDynamicSceneView = activeDynamicSceneView;
					debugStatsRequest.persistentVoxelStats = hasPersistentVoxelOverlay ? &persistentVoxelOverlayStats : nullptr;
					debugStatsRequest.hasLocalPlayerReflectionScene = hasLocalPlayerReflectionScene;
					debugStatsRequest.localPlayerReflectionSceneView = &localPlayerReflectionSceneView;
					debugStatsRequest.baseMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsBaseMs;
					debugStatsRequest.persistentVoxelMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsPersistentVoxelMs;
					debugStatsRequest.localPlayerReflectionMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsLocalPlayerReflectionMs;
					debugStatsRequest.mergeMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsMergeMs;
					const NRISceneFrameDebugStatsInputs debugStatsInputs =
						MakeNRISceneFrameDebugStatsInputs(debugStatsRequest);
					activeStats = BuildNRISceneFrameDebugStats(debugStatsInputs, mLastPerfShellTraceStats);
				}

				{
					NRISceneFrameGenerationBuildRequest generationRequest = {};
					generationRequest.staticMapBuildSerial = mStaticMapScene.buildSerial;
					generationRequest.runtimeMutationGeneration = mRuntimeMutation.BuildFrameGenerationHash(hasRuntimeMutationOverlay);
					generationRequest.persistentVoxelGeneration = hasPersistentVoxelOverlay ? mPersistentVoxels.BuildSceneGenerationHash() : 0ull;
					generationRequest.frameIndex = mFrameIndex;
					generationRequest.staticAccelerationBuildSerial = mStaticAccelerationBuildSerial;
					generationRequest.renderWidth = mRenderWidth;
					generationRequest.renderHeight = mRenderHeight;
					generationRequest.currentCameraPos = mCurrentCameraPos;
					generationRequest.currentCameraForward = mCurrentCameraForward;
					generationRequest.currentCameraRight = mCurrentCameraRight;
					generationRequest.currentCameraUp = mCurrentCameraUp;
					generationRequest.currentTanHalfFovX = mCurrentTanHalfFovX;
					generationRequest.currentTanHalfFovY = mCurrentTanHalfFovY;
					generationRequest.selectedSceneHasDynamicOverlay = selectedSceneHasDynamicOverlay;
					generationRequest.activeDynamicSceneView = activeDynamicSceneView;
					generationRequest.activeDynamicGeometry = activeDynamicGeometry;
					generationRequest.activeDynamicMaterials = activeDynamicMaterials;
					generationRequest.hasLocalPlayerReflectionScene = hasLocalPlayerReflectionScene;
					generationRequest.localPlayerReflectionGeometry = &localPlayerReflectionGeometry;
					generationRequest.localPlayerReflectionMaterials = &localPlayerReflectionMaterialBridge;
					generationRequest.activeMaterialBridge = activeMaterialBridge;
					generationRequest.activeGpuMaterials = activeGpuMaterials;
					generationRequest.sceneTextureCacheCount = mSceneTextures.CacheCount();
					generationRequest.selectedTlasInstanceCount = selectedTlasInstanceCount;
					generationRequest.selectedSceneInstanceCount = selectedSceneInstanceCount;
					generationRequest.selectedStaticSceneInstanceCount = selectedStaticSceneInstanceCount;
					generationRequest.selectedDynamicSceneInstanceCount = selectedDynamicSceneInstanceCount;
					generationRequest.selectedPersistentVoxelSceneInstanceCount = selectedPersistentVoxelSceneInstanceCount;
					const NRISceneFrameGenerationInputs generationInputs =
						MakeNRISceneFrameGenerationInputs(generationRequest);
					const NRISceneFrameGenerationResult generationResult =
						BuildNRISceneFrameGenerationResult(generationInputs, mLastStateCommitDomainGenerations, mHasLastStateCommitDomainGenerations);
					WriteNRISceneFrameGenerationTraceStats(generationResult, mLastPerfShellTraceStats);
					mLastStateCommitDomainGenerations = generationResult.current;
					mHasLastStateCommitDomainGenerations = true;
				}
			}
			else
			{
				LogFallback("PT runtime/dynamic overlay update failed; tracing the resident static world only.");
				if (mGpuSceneHasDynamicOverlay)
				{
					RestoreStaticTopLevelScene();
				}
				paletteReady = true;
				texturesReady = true;
				buffersReady = true;
				accelerationReady = true;
			}
		}
		else if (mGpuSceneHasDynamicOverlay || residentStaticWorldGeometryChanged)
		{
			if (!RestoreStaticTopLevelScene())
			{
				LogFallback("PT static scene restore failed after dynamic overlay or resident chunk rebuild.");
				if (preserveHistory)
				{
					RestoreRenderSceneHistorySnapshot(history);
				}
				return false;
			}

			mGpuSceneHasDynamicOverlay = false;
			mUsedStaticMapSceneLastFrame = true;
			activeSceneView = &mStaticMapScene.sceneView;
			activeGeometry = &mStaticMapScene.geometry;
			activeGpuMaterials = &mStaticMapScene.gpuMaterials;
			activeMaterialBridge = &mStaticMapScene.materialBridge;
			activeStats = mStaticMapScene.sceneView.stats;
		}
		else if (deferOverlayThisFrame)
		{
			Printf("NRI PT dynamic scene deferred: skipping non-map dynamic overlay on the same frame that rebuilt resident static map assets.\n");
		}
		else
		{
			mGpuSceneHasDynamicOverlay = false;
		}
	}
	else
	{
		ResetPersistentDynamicEmissiveCache();
		Clocker clock(NriPTSceneCapture);
		if (!nri_scene::CaptureScene(di, capturedSceneView))
		{
			LogFallback("PT scene capture failed.");
			if (preserveHistory)
			{
				RestoreRenderSceneHistorySnapshot(history);
			}
			return false;
		}

		activeSceneView = &capturedSceneView;
		activeMaterialBridge = &materialBridge;
		sceneLightCapturedView = &capturedSceneView;
		activeStats = capturedSceneView.stats;

		{
			Clocker clock(NriPTGeometryBuild);
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildCapturedMs);
			nri_scene::BuildGeometry(capturedSceneView, capturedGeometry);
			AssignGeometryPortalIndices(mMapWorld, capturedGeometry);
		}

		{
			Clocker clock(NriPTMaterialBuild);
			BuildMaterialsWithActorOverrides(capturedSceneView, materialBridge, "captured_scene");
		}
		sceneLightCapturedMaterials = &materialBridge;

		const bool needsFallbackMaterials = bootstrapCapturedDiagnostics || bootstrapCapturedFlat;
		const bool needsRealTextures = !nri_ptbootstrap || bootstrapCapturedBaseColor || bootstrapMode >= 13u;
		paletteReady = needsRealTextures ? EnsurePaletteTexture(materialBridge) : true;
		texturesReady = needsFallbackMaterials ? UseFallbackSceneTextures(preserveHistory, "captured_scene_fallback") : (needsRealTextures ? (paletteReady && EnsureSceneTextures(capturedSceneView, materialBridge, capturedGpuMaterials, preserveHistory, "captured_scene")) : EnsureSkyTexture(capturedSceneView, preserveHistory));
		if (needsFallbackMaterials)
		{
			capturedGpuMaterials = materialBridge.materials;
			for (auto& material : capturedGpuMaterials)
			{
				material.textureIndex = 0;
				material.paletteIndex = 0;
				material.flags = 0;
				material.normalTextureIndex = UINT32_MAX;
				material.metallicTextureIndex = UINT32_MAX;
				material.roughnessTextureIndex = UINT32_MAX;
				material.emissiveTextureIndex = UINT32_MAX;
				material.lightLevel = 1.0f;
				material.alpha = 1.0f;
			}
		}
		else if (!needsRealTextures)
		{
			capturedGpuMaterials = materialBridge.materials;
		}

		buffersReady = texturesReady && UploadSceneBuffers(capturedGeometry, capturedGpuMaterials);
		auto& sceneInstances = mSelectCapturedSceneInstanceScratch;
		sceneInstances.clear();
		if (buffersReady)
		{
			SceneInstanceData sceneRecord = {};
			sceneRecord.primitiveBase = 0u;
			sceneRecord.dataSource = nri_diag::SceneDataSourceDynamic;
			sceneRecord.materialBase = 0u;
			sceneRecord.materialCount = (uint32_t)capturedGpuMaterials.size();
			sceneRecord.visibilityChunk = UINT32_MAX;
			sceneInstances.push_back(sceneRecord);
			buffersReady = NRISceneUploadManager::UpdateSceneDataSet(*this,
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				sceneInstances,
				0u,
				(uint32_t)capturedGeometry.primitives.size(),
				0u,
				(uint32_t)capturedGpuMaterials.size(),
				"captured_scene");
		}
		if (texturesReady)
		{
			PrepareSceneTextureInputsForCompute();
		}
		if (bootstrapCapturedView || rawTraceDirectScene)
		{
			accelerationReady = true;
		}
		else if (buffersReady)
		{
			NRIAccelerationStructureResource& dynamicBottomLevelAS = GetCurrentDynamicBottomLevelAS();
			accelerationReady =
				BuildDynamicAccelerationStructure(capturedGeometry) &&
				dynamicBottomLevelAS.accelerationStructure != nullptr;
			if (accelerationReady)
			{
				nri::TopLevelInstance instance = {};
				instance.transform[0][0] = 1.0f;
				instance.transform[1][1] = 1.0f;
				instance.transform[2][2] = 1.0f;
				instance.instanceId = 0;
				instance.mask = NRI_TLAS_MASK_ALL_WORKLOADS;
				instance.shaderBindingTableLocalOffset = 0;
				instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
				instance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*dynamicBottomLevelAS.accelerationStructure);

				auto& instances = mSelectCapturedTopLevelInstanceScratch;
				instances.clear();
				instances.push_back(instance);
				accelerationReady = BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Dynamic);
			}
		}
		else
		{
			accelerationReady = false;
		}
		activeGeometry = &capturedGeometry;
		activeGpuMaterials = &capturedGpuMaterials;
		emissiveSamplingContext.capturedGeometry = &capturedGeometry;
		}
	}

	if (activeSceneView == nullptr || activeGeometry == nullptr || activeGpuMaterials == nullptr || activeMaterialBridge == nullptr)
	{
		LogFallback("PT scene selection failed.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}

	RefreshSceneLightSystem(
		sceneLightUsesStaticMapScene,
		sceneLightCapturedView,
		sceneLightCapturedMaterials,
		sceneLightDynamicView,
		sceneLightDynamicMaterials,
		hasSurfaceLightOverlayForFrame ? &surfaceLightSceneView : nullptr,
		hasSurfaceLightOverlayForFrame ? &surfaceLightMaterialBridge : nullptr,
		appendPersistentVoxelSceneLights);

	bool refreshedSceneDataAfterLightRebuild = false;
	if (mGpuSceneHasDynamicOverlay &&
		activeMaterialBridge == &combinedMaterialBridge &&
		!overlayGeometry.primitives.empty())
	{
		refreshedCombinedGpuMaterials = combinedMaterialBridge.materials;
		ApplyEmissiveMaterialOverrides(combinedMaterialBridge, refreshedCombinedGpuMaterials);
		ApplyActorShadowMaterialOverrides(combinedMaterialBridge, refreshedCombinedGpuMaterials);
		if (!nri_material_policy::MaterialDataVectorEqual(refreshedCombinedGpuMaterials, combinedGpuMaterials))
		{
			const size_t staticMaterialCount = mStaticMapScene.gpuMaterials.size();
			if (refreshedCombinedGpuMaterials.size() < staticMaterialCount)
			{
				LogFallback("PT runtime overlay material refresh produced an invalid material slice.");
				if (preserveHistory)
				{
					RestoreRenderSceneHistorySnapshot(history);
				}
				return false;
			}

			combinedGpuMaterials.swap(refreshedCombinedGpuMaterials);
			dynamicGpuMaterials.assign(combinedGpuMaterials.begin() + staticMaterialCount, combinedGpuMaterials.end());
			if (!UploadSceneBuffers(overlayGeometry, dynamicGpuMaterials) ||
				!NRISceneUploadManager::UpdateSceneDataSet(*this,
					mStaticVertexBuffer,
					mStaticIndexBuffer,
					mStaticPrimitiveBuffer,
					mStaticMaterialBuffer,
					GetCurrentDynamicVertexBuffer(),
					GetCurrentDynamicIndexBuffer(),
					GetCurrentDynamicPrimitiveBuffer(),
					GetCurrentDynamicMaterialBuffer(),
					mBoundSceneInstances,
					(uint32_t)mStaticMapScene.geometry.primitives.size(),
					(uint32_t)overlayGeometry.primitives.size(),
					(uint32_t)mStaticMapScene.gpuMaterials.size(),
					(uint32_t)dynamicGpuMaterials.size(),
					"resident_overlay_material_refresh"))
			{
				LogFallback("PT runtime overlay material refresh failed after scene-light rebuild.");
				if (preserveHistory)
				{
					RestoreRenderSceneHistorySnapshot(history);
				}
				return false;
			}

			activeGpuMaterials = &combinedGpuMaterials;
			refreshedSceneDataAfterLightRebuild = true;
		}
	}

	if (mRuntimeLightSceneDataDirty && !refreshedSceneDataAfterLightRebuild)
	{
		if (mGpuSceneHasDynamicOverlay)
		{
			if (!NRISceneUploadManager::UpdateSceneDataSet(*this,
				mStaticVertexBuffer,
				mStaticIndexBuffer,
				mStaticPrimitiveBuffer,
				mStaticMaterialBuffer,
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				mBoundSceneInstances,
				(uint32_t)mStaticMapScene.geometry.primitives.size(),
				(uint32_t)overlayGeometry.primitives.size(),
				(uint32_t)mStaticMapScene.gpuMaterials.size(),
				(uint32_t)dynamicGpuMaterials.size(),
				"runtime_overlay_light_refresh"))
			{
				LogFallback("PT runtime overlay light refresh failed after scene-light rebuild.");
				if (preserveHistory)
				{
					RestoreRenderSceneHistorySnapshot(history);
				}
				return false;
			}
		}
		else if (!sceneLightUsesStaticMapScene)
		{
			if (!NRISceneUploadManager::UpdateSceneDataSet(*this,
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				mBoundSceneInstances,
				0u,
				(uint32_t)capturedGeometry.primitives.size(),
				0u,
				(uint32_t)capturedGpuMaterials.size(),
				"captured_scene_light_refresh"))
			{
				LogFallback("PT captured scene light refresh failed after scene-light rebuild.");
				if (preserveHistory)
				{
					RestoreRenderSceneHistorySnapshot(history);
				}
				return false;
			}
		}
	}

	if (sceneLightUsesStaticMapScene && !mGpuSceneHasDynamicOverlay)
	{
		const bool needsResidentStaticLightRefresh =
			mRuntimeLightSceneDataDirty ||
			!mSceneLights.GetAnalyticLights().activeLights.empty() ||
			mBoundRuntimeLightCount != 0 ||
			mSceneLights.GetSectorLighting().activeSectorCount > 0 ||
			mBoundSectorLightActiveCount != 0;
		if (needsResidentStaticLightRefresh)
		{
			if (!RefreshResidentStaticSceneDataSet())
			{
				LogFallback("PT static scene light refresh failed.");
				if (preserveHistory)
				{
					RestoreRenderSceneHistorySnapshot(history);
				}
				return false;
			}
		}
	}

	if (!UpdateEmissiveSamplingBuffers(emissiveSamplingContext))
	{
		LogFallback("PT emissive primitive update failed.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}
	if (!BuildEmissiveTopLevelAccelerationStructure())
	{
		LogFallback("PT emissive TLAS update failed.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}

	TraceRuntimeLinkEvents(di);
	LogBridgeStats(activeStats);
	if (activeStats.unsupportedModelDrawItems > 0)
	{
		LogFallback("generic GLDL_MODELS content is unsupported in the PT bridge; rendering the supported PT scene without those model draws.");
	}

	Copy3(activeSceneView->skyColor, mSkyColor);
	Copy3(activeSceneView->groundColor, mGroundColor);
	NRISceneSurfaceProbeFrameBuildRequest surfaceProbeFrameRequest = {};
	surfaceProbeFrameRequest.usesStaticMapScene = mUsedStaticMapSceneLastFrame;
	surfaceProbeFrameRequest.activeStaticProbePrimitiveCount = activeStaticProbePrimitiveCount;
	surfaceProbeFrameRequest.runtimeSpaceLinkGeometry = &runtimeSpaceLinkGeometry;
	surfaceProbeFrameRequest.runtimeMutationGeometry = &runtimeMutationFrame.geometry;
	surfaceProbeFrameRequest.overlayGeometry = &overlayGeometry;
	surfaceProbeFrameRequest.activeDynamicGeometry = activeDynamicGeometry;
	const NRISceneSurfaceProbeFrameInputs surfaceProbeFrameInputs =
		MakeNRISceneSurfaceProbeFrameInputs(surfaceProbeFrameRequest);
	mSurfaceProbeFrame = BuildNRISceneSurfaceProbeFrameState(surfaceProbeFrameInputs);

	if (!preserveHistory)
	{
		UpdateSurfaceProbe(*activeGeometry, activeMaterialBridge, true);
	}
	if (activeGeometry->primitives.empty())
	{
		LogFallback("PT scene path produced no supported opaque geometry.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}

	if (mUsedStaticMapSceneLastFrame)
	{
		PrepareSceneTextureInputsForCompute();
	}


	return true;
}
