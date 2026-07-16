#include "nri_scene_frame_overlay.h"

#include "../scene/nri_hash.h"

#include <chrono>

namespace
{
	class ScopedOverlayTimer
	{
	public:
		ScopedOverlayTimer(bool enabled, double* targetMs)
			: mTarget(enabled ? targetMs : nullptr)
		{
			if (mTarget != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedOverlayTimer()
		{
			if (mTarget != nullptr)
			{
				const auto end = std::chrono::steady_clock::now();
				*mTarget += std::chrono::duration<double, std::milli>(end - mStart).count();
			}
		}

	private:
		double* mTarget = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};

	uint64_t HashSurfaceProvenanceStamp(uint64_t hash, const nri_scene::SurfaceProvenance& provenance)
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
		return hash;
	}

	uint32_t CountStampedSurfacePrimitives(const nri_scene::SurfaceRef& surface, bool triangleList)
	{
		if (!surface.indices.empty())
		{
			return (uint32_t)(surface.indices.size() / 3u);
		}
		if (triangleList)
		{
			return (uint32_t)(surface.vertices.size() / 3u);
		}
		return surface.vertices.size() >= 3 ? (uint32_t)surface.vertices.size() - 2u : 0u;
	}

	void AddOverlayReserve(
		const nri_scene::GeometryData* geometry,
		const nri_scene::MaterialBridgeData* materials,
		NRISceneContributionReserve& overlayReserve)
	{
		NRISceneContribution contribution = {};
		contribution.geometry = geometry;
		contribution.materials = materials;
		AccumulateNRISceneContributionReserve(contribution, overlayReserve);
	}

	void AppendOverlaySource(
		const nri_scene::GeometryData* geometry,
		const NRIRenderer::SceneBufferUploadProducerStamp* producerStamp,
		const nri_scene::MaterialBridgeData* materials,
		const NRISceneFrameOverlaySourceTelemetry& telemetry,
		NRIRenderer::SceneBufferUploadDomain uploadDomain,
		nri_scene::GeometryData& overlayGeometry,
		nri_scene::MaterialBridgeData& overlayMaterialBridge,
		std::vector<NRIRenderer::SceneBufferUploadDomainSpan>& uploadSpans)
	{
		NRISceneContribution contribution = {};
		contribution.geometry = geometry;
		contribution.producerStamp = producerStamp;
		contribution.materials = materials;
		contribution.uploadDomain = uploadDomain;
		NRISceneContributionAppendStats appendStats = {};
		appendStats.totalMs = telemetry.totalMs;
		appendStats.geometryMs = telemetry.geometryMs;
		appendStats.materialMs = telemetry.materialMs;
		appendStats.primitiveCount = telemetry.primitiveCount;
		appendStats.materialCount = telemetry.materialCount;
		appendStats.sourceTrace = telemetry.sourceTrace;
		AppendNRISceneContribution(contribution, appendStats, overlayGeometry, overlayMaterialBridge, uploadSpans);
	}
}

uint64_t BuildNRISceneViewUploadLayoutKey(const nri_scene::SceneView& sceneView, uint64_t mapWorldBuildSerial)
{
	uint64_t hash = 1469598103934665603ull;
	hash = nri_scene::HashCombine64(hash, mapWorldBuildSerial);
	hash = nri_scene::HashCombine64(hash, (uint64_t)sceneView.primitiveFlags);
	auto appendSurfaces = [&](const std::vector<nri_scene::SurfaceRef>& surfaces, uint64_t kind, bool triangleList)
	{
		hash = nri_scene::HashCombine64(hash, kind);
		hash = nri_scene::HashCombine64(hash, (uint64_t)surfaces.size());
		for (const nri_scene::SurfaceRef& surface : surfaces)
		{
			hash = nri_scene::HashCombine64(hash, (uint64_t)surface.vertices.size());
			hash = nri_scene::HashCombine64(hash, (uint64_t)surface.indices.size());
			hash = nri_scene::HashCombine64(hash, (uint64_t)CountStampedSurfacePrimitives(surface, triangleList));
			for (uint32_t index : surface.indices)
			{
				hash = nri_scene::HashCombine64(hash, (uint64_t)index);
			}
			hash = HashSurfaceProvenanceStamp(hash, surface.provenance);
			hash = nri_scene::HashCombine64(hash, (uint64_t)surface.provenance.actorOverlayRuleCount);
			for (uint32_t ruleId : surface.provenance.actorOverlayRuleIds)
			{
				hash = nri_scene::HashCombine64(hash, (uint64_t)ruleId);
			}
		}
	};
	appendSurfaces(sceneView.opaqueWalls, 1u, false);
	appendSurfaces(sceneView.opaqueFlats, 2u, true);
	appendSurfaces(sceneView.opaqueSprites, 3u, false);
	return hash != 0 ? hash : 1;
}

void BuildNRISceneFrameOverlay(
	const NRISceneFrameOverlayBuildInputs& inputs,
	const NRISceneFrameOverlayBuildOutputs& outputs)
{
	if (inputs.stats == nullptr ||
		outputs.overlayGeometry == nullptr ||
		outputs.overlayMaterialBridge == nullptr ||
		outputs.uploadSpans == nullptr)
	{
		return;
	}

	NRIRenderer::PerfShellTraceStats& stats = *inputs.stats;
	nri_scene::GeometryData& overlayGeometry = *outputs.overlayGeometry;
	nri_scene::MaterialBridgeData& overlayMaterialBridge = *outputs.overlayMaterialBridge;
	std::vector<NRIRenderer::SceneBufferUploadDomainSpan>& uploadSpans = *outputs.uploadSpans;
	ScopedOverlayTimer perfTimer(inputs.collectTiming, &stats.overlayAssembleMs);
	ScopedOverlayTimer appendTimer(inputs.collectTiming, &stats.overlayAppendMs);

	{
		ScopedOverlayTimer resetTimer(inputs.collectTiming, &stats.overlayAppendResetMs);
		nri_scene::ClearGeometryRetainingCapacity(overlayGeometry);
		nri_scene::ClearMaterialBridgeRetainingCapacity(overlayMaterialBridge);
	}

	{
		ScopedOverlayTimer sourceAggregateTimer(inputs.collectTiming, &stats.overlayAppendSourcesMs);
		NRISceneContributionReserve overlayReserve = {};
		if (inputs.hasRuntimeSpaceLinkOverlay)
		{
			AddOverlayReserve(inputs.runtimeSpaceLinkGeometry, inputs.runtimeSpaceLinkMaterials, overlayReserve);
		}
		if (inputs.hasRuntimeMutationOverlay)
		{
			AddOverlayReserve(inputs.runtimeMutationGeometry, inputs.runtimeMutationMaterials, overlayReserve);
		}
		if (inputs.hasActiveDynamicOverlay)
		{
			AddOverlayReserve(inputs.activeDynamicGeometry, inputs.activeDynamicMaterials, overlayReserve);
		}
		if (inputs.hasLocalPlayerReflectionOverlay)
		{
			AddOverlayReserve(inputs.localPlayerReflectionGeometry, inputs.localPlayerReflectionMaterials, overlayReserve);
		}
		if (inputs.hasRuntimeDebugSphereOverlay)
		{
			AddOverlayReserve(inputs.runtimeDebugSphereGeometry, inputs.runtimeDebugSphereMaterials, overlayReserve);
		}
		if (inputs.hasSurfaceLightOverlay)
		{
			AddOverlayReserve(inputs.surfaceLightGeometry, inputs.surfaceLightMaterials, overlayReserve);
		}
		ReserveNRISceneContributionCapacity(overlayReserve, overlayGeometry, overlayMaterialBridge);

		if (inputs.hasRuntimeSpaceLinkOverlay)
		{
			AppendOverlaySource(
				inputs.runtimeSpaceLinkGeometry,
				&inputs.runtimeSpaceLinkStamp,
				inputs.runtimeSpaceLinkMaterials,
				inputs.runtimeSpaceLinkTelemetry,
				NRIRenderer::SceneBufferUploadDomain::RuntimeSpaceLink,
				overlayGeometry,
				overlayMaterialBridge,
				uploadSpans);
		}
		if (inputs.hasRuntimeMutationOverlay)
		{
			AppendOverlaySource(
				inputs.runtimeMutationGeometry,
				&inputs.runtimeMutationStamp,
				inputs.runtimeMutationMaterials,
				inputs.runtimeMutationTelemetry,
				NRIRenderer::SceneBufferUploadDomain::RuntimeMutation,
				overlayGeometry,
				overlayMaterialBridge,
				uploadSpans);
		}
		if (inputs.hasActiveDynamicOverlay)
		{
			AppendOverlaySource(
				inputs.activeDynamicGeometry,
				&inputs.activeDynamicStamp,
				inputs.activeDynamicMaterials,
				inputs.activeDynamicTelemetry,
				NRIRenderer::SceneBufferUploadDomain::Dynamic,
				overlayGeometry,
				overlayMaterialBridge,
				uploadSpans);
		}
		if (inputs.hasLocalPlayerReflectionOverlay)
		{
			AppendOverlaySource(
				inputs.localPlayerReflectionGeometry,
				&inputs.localPlayerReflectionStamp,
				inputs.localPlayerReflectionMaterials,
				inputs.localPlayerReflectionTelemetry,
				NRIRenderer::SceneBufferUploadDomain::LocalPlayerReflection,
				overlayGeometry,
				overlayMaterialBridge,
				uploadSpans);
		}
		if (inputs.hasRuntimeDebugSphereOverlay)
		{
			AppendOverlaySource(
				inputs.runtimeDebugSphereGeometry,
				&inputs.runtimeDebugSphereStamp,
				inputs.runtimeDebugSphereMaterials,
				inputs.runtimeDebugSphereTelemetry,
				NRIRenderer::SceneBufferUploadDomain::RuntimeDebugSphere,
				overlayGeometry,
				overlayMaterialBridge,
				uploadSpans);
		}
		if (inputs.hasSurfaceLightOverlay)
		{
			AppendOverlaySource(
				inputs.surfaceLightGeometry,
				&inputs.surfaceLightStamp,
				inputs.surfaceLightMaterials,
				inputs.surfaceLightTelemetry,
				NRIRenderer::SceneBufferUploadDomain::SurfaceLightOverlay,
				overlayGeometry,
				overlayMaterialBridge,
				uploadSpans);
		}
	}

	{
		ScopedOverlayTimer bookkeepingTimer(inputs.collectTiming, &stats.overlayAppendBookkeepingMs);
		if (inputs.hasPersistentVoxelOverlay && inputs.persistentVoxelOverlayStats != nullptr)
		{
			const NRIPersistentVoxelOverlayStats& persistentVoxelOverlayStats = *inputs.persistentVoxelOverlayStats;
			stats.overlayPersistentVoxelActorCount = persistentVoxelOverlayStats.actorCount;
			stats.overlayPersistentVoxelPrimitiveCount = persistentVoxelOverlayStats.primitiveCount;
			stats.overlayPersistentVoxelMaterialCount = persistentVoxelOverlayStats.materialCount;
			stats.overlayPersistentVoxelAppend.primitiveCount = persistentVoxelOverlayStats.primitiveCount;
			stats.overlayPersistentVoxelAppend.materialCount = persistentVoxelOverlayStats.materialCount;
			stats.overlayPersistentVoxelAppend.indexCount = persistentVoxelOverlayStats.indexCount;
			stats.overlayPersistentVoxelAppend.byteCount = persistentVoxelOverlayStats.byteCount;
		}
		stats.overlayPrimitiveCount = (uint32_t)overlayGeometry.primitives.size();
		stats.overlayMaterialCount = (uint32_t)overlayMaterialBridge.materials.size();
	}
}
