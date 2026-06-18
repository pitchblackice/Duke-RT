#include "nri_scene_frame_overlay.h"

#include "nri_scene_frame_mirrors.h"
#include "nri_upload_hash.h"
#include "../scene/nri_hash.h"

#include <chrono>
#include <cstring>

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

	struct SceneViewUploadStampBuildResult
	{
		uint64_t vertexPayloadStamp = 0;
		uint64_t indexPayloadStamp = 0;
		uint64_t primitivePayloadStamp = 0;
		uint64_t primitiveProvenanceStamp = 0;
		uint64_t materialPayloadStamp = 0;
	};

	uint32_t CoherencyFloatBits(float value)
	{
		static_assert(sizeof(uint32_t) == sizeof(float), "unexpected float size");
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	uint64_t HashMaterialPayloadData(const nri_scene::MaterialBridgeData& materialBridge)
	{
		const uint64_t materialSize = (uint64_t)materialBridge.materials.size() * sizeof(nri_scene::MaterialData);
		return NRIHashUploadPayloadBytes(
			materialBridge.materials.empty() ? nullptr : materialBridge.materials.data(),
			materialSize);
	}

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

	uint64_t HashCapturedVertexStamp(uint64_t hash, const nri_scene::CapturedVertex& vertex)
	{
		for (int i = 0; i < 3; ++i)
		{
			hash = nri_scene::HashCombine64(hash, CoherencyFloatBits(vertex.position[i]));
		}
		for (int i = 0; i < 3; ++i)
		{
			hash = nri_scene::HashCombine64(hash, CoherencyFloatBits(vertex.prevPosition[i]));
		}
		hash = nri_scene::HashCombine64(hash, CoherencyFloatBits(vertex.uv[0]));
		hash = nri_scene::HashCombine64(hash, CoherencyFloatBits(vertex.uv[1]));
		return hash;
	}

	uint64_t HashMaterialRefStamp(uint64_t hash, const nri_scene::MaterialRef& material)
	{
		hash = nri_scene::HashCombine64(hash, material.texture != nullptr ? (uint64_t)(uint32_t)material.texture->GetID().GetIndex() + 1ull : 0ull);
		hash = nri_scene::HashCombine64(hash, material.emissiveSourceTexture != nullptr ? (uint64_t)(uint32_t)material.emissiveSourceTexture->GetID().GetIndex() + 1ull : 0ull);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(material.palette + 1));
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(material.shade + 1));
		hash = nri_scene::HashCombine64(hash, CoherencyFloatBits(material.alpha));
		hash = nri_scene::HashCombine64(hash, (uint64_t)material.flags);
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

	SceneViewUploadStampBuildResult BuildSceneViewUploadProducerStamp(const nri_scene::SceneView& sceneView, uint64_t mapWorldBuildSerial)
	{
		SceneViewUploadStampBuildResult result = {};
		result.vertexPayloadStamp = 1469598103934665603ull;
		result.indexPayloadStamp = 1469598103934665603ull;
		result.primitivePayloadStamp = 1469598103934665603ull;
		result.primitiveProvenanceStamp = 1469598103934665603ull;
		result.materialPayloadStamp = 1469598103934665603ull;
		auto appendSurface =
			[&](const nri_scene::SurfaceRef& surface, uint32_t surfaceKind, bool triangleList, uint32_t materialIndex)
		{
			const uint32_t primitiveCount = CountStampedSurfacePrimitives(surface, triangleList);
			const uint64_t surfaceHeader =
				nri_scene::HashCombine64(
					nri_scene::HashCombine64(
						nri_scene::HashCombine64(1469598103934665603ull, (uint64_t)surfaceKind),
						(uint64_t)materialIndex),
					(uint64_t)primitiveCount);
			result.vertexPayloadStamp = nri_scene::HashCombine64(result.vertexPayloadStamp, surfaceHeader);
			result.indexPayloadStamp = nri_scene::HashCombine64(result.indexPayloadStamp, surfaceHeader);
			result.primitivePayloadStamp = nri_scene::HashCombine64(result.primitivePayloadStamp, surfaceHeader);
			result.primitiveProvenanceStamp = nri_scene::HashCombine64(result.primitiveProvenanceStamp, surfaceHeader);
			result.materialPayloadStamp = nri_scene::HashCombine64(result.materialPayloadStamp, surfaceHeader);
			result.vertexPayloadStamp = nri_scene::HashCombine64(result.vertexPayloadStamp, (uint64_t)surface.vertices.size());
			result.indexPayloadStamp = nri_scene::HashCombine64(result.indexPayloadStamp, (uint64_t)surface.indices.size());
			result.primitivePayloadStamp = nri_scene::HashCombine64(result.primitivePayloadStamp, (uint64_t)sceneView.primitiveFlags);
			result.primitivePayloadStamp = nri_scene::HashCombine64(result.primitivePayloadStamp, (uint64_t)surface.material.flags);
			result.primitivePayloadStamp = nri_scene::HashCombine64(result.primitivePayloadStamp, mapWorldBuildSerial);
			result.primitiveProvenanceStamp = HashSurfaceProvenanceStamp(result.primitiveProvenanceStamp, surface.provenance);
			result.materialPayloadStamp = HashMaterialRefStamp(result.materialPayloadStamp, surface.material);
			for (const nri_scene::CapturedVertex& vertex : surface.vertices)
			{
				result.vertexPayloadStamp = HashCapturedVertexStamp(result.vertexPayloadStamp, vertex);
				result.primitivePayloadStamp = HashCapturedVertexStamp(result.primitivePayloadStamp, vertex);
			}
			for (uint32_t index : surface.indices)
			{
				result.indexPayloadStamp = nri_scene::HashCombine64(result.indexPayloadStamp, (uint64_t)index);
				result.primitivePayloadStamp = nri_scene::HashCombine64(result.primitivePayloadStamp, (uint64_t)index);
			}
		};

		uint32_t materialIndex = 0;
		for (const nri_scene::SurfaceRef& surface : sceneView.opaqueWalls)
		{
			appendSurface(surface, 0u, false, materialIndex++);
		}
		for (const nri_scene::SurfaceRef& surface : sceneView.opaqueFlats)
		{
			appendSurface(surface, 1u, true, materialIndex++);
		}
		for (const nri_scene::SurfaceRef& surface : sceneView.opaqueSprites)
		{
			appendSurface(surface, 2u, false, materialIndex++);
		}
		result.vertexPayloadStamp = nri_scene::HashCombine64(result.vertexPayloadStamp, (uint64_t)materialIndex);
		result.indexPayloadStamp = nri_scene::HashCombine64(result.indexPayloadStamp, (uint64_t)materialIndex);
		result.primitivePayloadStamp = nri_scene::HashCombine64(result.primitivePayloadStamp, (uint64_t)materialIndex);
		result.primitiveProvenanceStamp = nri_scene::HashCombine64(result.primitiveProvenanceStamp, (uint64_t)materialIndex);
		result.materialPayloadStamp = nri_scene::HashCombine64(result.materialPayloadStamp, (uint64_t)materialIndex);
		result.vertexPayloadStamp = result.vertexPayloadStamp != 0 ? result.vertexPayloadStamp : 1;
		result.indexPayloadStamp = result.indexPayloadStamp != 0 ? result.indexPayloadStamp : 1;
		result.primitivePayloadStamp = result.primitivePayloadStamp != 0 ? result.primitivePayloadStamp : 1;
		result.primitiveProvenanceStamp = result.primitiveProvenanceStamp != 0 ? result.primitiveProvenanceStamp : 1;
		result.materialPayloadStamp = result.materialPayloadStamp != 0 ? result.materialPayloadStamp : 1;
		return result;
	}

	NRIRenderer::SceneBufferUploadProducerStamp ToProducerStamp(const SceneViewUploadStampBuildResult& built)
	{
		NRIRenderer::SceneBufferUploadProducerStamp stamp = {};
		stamp.vertexPayloadStamp = built.vertexPayloadStamp;
		stamp.indexPayloadStamp = built.indexPayloadStamp;
		stamp.primitivePayloadStamp = built.primitivePayloadStamp;
		stamp.primitiveProvenanceStamp = built.primitiveProvenanceStamp;
		stamp.materialPayloadStamp = built.materialPayloadStamp;
		return stamp;
	}

	NRIRenderer::SceneBufferUploadProducerStamp ToProducerStamp(const NRILocalPlayerReflectionUploadStamp& built)
	{
		NRIRenderer::SceneBufferUploadProducerStamp stamp = {};
		stamp.vertexPayloadStamp = built.vertexPayloadStamp;
		stamp.indexPayloadStamp = built.indexPayloadStamp;
		stamp.primitivePayloadStamp = built.primitivePayloadStamp;
		stamp.primitiveProvenanceStamp = built.primitiveProvenanceStamp;
		stamp.materialPayloadStamp = built.materialPayloadStamp;
		return stamp;
	}

	NRIRenderer::SceneBufferUploadProducerStamp BuildSceneViewProducerStamp(
		const NRISceneFrameOverlayBuildInputs& inputs,
		const nri_scene::SceneView& sceneView,
		double* timerMs)
	{
		ScopedOverlayTimer aggregateTimer(inputs.collectTiming, &inputs.stats->overlayAppendProducerStampMs);
		ScopedOverlayTimer sourceTimer(inputs.collectTiming, timerMs);
		return ToProducerStamp(BuildSceneViewUploadProducerStamp(sceneView, inputs.mapWorldBuildSerial));
	}

	NRIRenderer::SceneBufferUploadProducerStamp BuildLocalPlayerReflectionProducerStamp(const NRISceneFrameOverlayBuildInputs& inputs)
	{
		ScopedOverlayTimer aggregateTimer(inputs.collectTiming, &inputs.stats->overlayAppendProducerStampMs);
		ScopedOverlayTimer sourceTimer(inputs.collectTiming, &inputs.stats->overlayAppendLocalPlayerReflectionStampMs);
		return ToProducerStamp(BuildNRILocalPlayerReflectionUploadProducerStamp(
			*inputs.localPlayerReflectionGeometry,
			*inputs.localPlayerReflectionMaterials,
			inputs.frameIndex,
			inputs.mapWorldBuildSerial));
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
		const NRIRenderer::SceneBufferUploadProducerStamp dynamicStamp =
			inputs.hasActiveDynamicOverlay && inputs.activeDynamicSceneView != nullptr ?
				BuildSceneViewProducerStamp(inputs, *inputs.activeDynamicSceneView, &stats.overlayAppendDynamicStampMs) :
				NRIRenderer::SceneBufferUploadProducerStamp {};
		const NRIRenderer::SceneBufferUploadProducerStamp localPlayerReflectionStamp =
			inputs.hasLocalPlayerReflectionOverlay && inputs.localPlayerReflectionGeometry != nullptr && inputs.localPlayerReflectionMaterials != nullptr ?
				BuildLocalPlayerReflectionProducerStamp(inputs) :
				NRIRenderer::SceneBufferUploadProducerStamp {};

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
				nullptr,
				inputs.runtimeSpaceLinkMaterials,
				inputs.runtimeSpaceLinkTelemetry,
				NRIRenderer::SceneBufferUploadDomain::StaticOverlay,
				overlayGeometry,
				overlayMaterialBridge,
				uploadSpans);
		}
		if (inputs.hasRuntimeMutationOverlay)
		{
			AppendOverlaySource(
				inputs.runtimeMutationGeometry,
				nullptr,
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
				&dynamicStamp,
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
				&localPlayerReflectionStamp,
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
				nullptr,
				inputs.runtimeDebugSphereMaterials,
				inputs.runtimeDebugSphereTelemetry,
				NRIRenderer::SceneBufferUploadDomain::StaticOverlay,
				overlayGeometry,
				overlayMaterialBridge,
				uploadSpans);
		}
		if (inputs.hasSurfaceLightOverlay)
		{
			AppendOverlaySource(
				inputs.surfaceLightGeometry,
				nullptr,
				inputs.surfaceLightMaterials,
				inputs.surfaceLightTelemetry,
				NRIRenderer::SceneBufferUploadDomain::StaticOverlay,
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
