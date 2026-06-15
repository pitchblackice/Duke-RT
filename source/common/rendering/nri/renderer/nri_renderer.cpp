#include "nri_renderer.h"
#include "nri_cvars.h"

#include "../framegen/nri_framegen.h"
#include "nri_acceleration.h"
#include "nri_debug_reporters.h"
#include "nri_descriptor_sets.h"
#include "nri_diagnostic_names.h"
#include "nri_frame_graph.h"
#include "nri_material_policy.h"
#include "nri_pass_dispatch.h"
#include "nri_pipeline_state.h"
#include "nri_preload_coordinator.h"
#include "nri_renderstate.h"
#include "nri_render_geometry_helpers.h"
#include "nri_renderer_settings.h"
#include "nri_scene_frame_builder.h"
#include "nri_scene_upload.h"
#include "nri_shader_contracts.h"
#include "nri_static_scene_geometry.h"
#include "nri_surface_light_overlay.h"
#include "nri_upload_hash.h"
#include "nri_runtime_mutation_shared.h"
#include "../scene/nri_hash.h"
#include "../scene/nri_map_builder.h"
#include "../scene/nri_scene_math.h"
#include "../scene/nri_scene_stats.h"
#include "../system/nri_hwtexture.h"
#include "../system/nri_renderdevice.h"
#include "skyboxtexture.h"
#include "image.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "coreactor.h"
#include "coreplayer.h"
#include "hw_voxels.h"
#include "gamecontrol.h"
#include "gamestate.h"
#include "menustate.h"
#include "hw_sections.h"
#include "lightoverlay.h"
#include "mapinfo.h"
#include "printf.h"
#include "gamestruct.h"
#include "hw_portal.h"
#include "texinfo.h"
#include "texturemanager.h"
#include "d_eventbase.h"
#include "v_video.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>

namespace
{
	static constexpr double BuildTickSeconds = 1.0 / 120.0;

	static const char* GetNightVisionModeName(NRIPTNightVisionMode mode)
	{
		switch (mode)
		{
		case NRIPTNightVisionMode::Duke: return "duke";
		default: return "none";
		}
	}

	static uint32_t PackNightVisionControls(float contrast, float saturation)
	{
		const uint32_t contrastBits = (uint32_t)std::lround(std::clamp(contrast, 0.0f, 2.0f) * (65535.0f / 2.0f));
		const uint32_t saturationBits = (uint32_t)std::lround(std::clamp(saturation, 0.0f, 2.0f) * (65535.0f / 2.0f));
		return contrastBits | (saturationBits << 16);
	}

	static uint32_t PackNightVisionModeAndTint(NRIPTNightVisionMode mode, float red, float green, float blue)
	{
		const uint32_t redBits = (uint32_t)std::lround(std::clamp(red, 0.0f, 2.0f) * (255.0f / 2.0f));
		const uint32_t greenBits = (uint32_t)std::lround(std::clamp(green, 0.0f, 2.0f) * (255.0f / 2.0f));
		const uint32_t blueBits = (uint32_t)std::lround(std::clamp(blue, 0.0f, 2.0f) * (255.0f / 2.0f));
		return (uint32_t)mode | (redBits << 8) | (greenBits << 16) | (blueBits << 24);
	}

	static uint64_t HashCombineLightOverlay(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
	}

	static uint64_t QuantizeLightOverlayPositionKey(const float position[3])
	{
		const int64_t x = (int64_t)std::llround(position[0] * 16.0f);
		const int64_t y = (int64_t)std::llround(position[1] * 16.0f);
		const int64_t z = (int64_t)std::llround(position[2] * 16.0f);
		uint64_t key = 1469598103934665603ull;
		key = HashCombineLightOverlay(key, (uint64_t)x);
		key = HashCombineLightOverlay(key, (uint64_t)y);
		key = HashCombineLightOverlay(key, (uint64_t)z);
		return key;
	}

	static uint32_t GetGameplayLightTimeIndex()
	{
		return PlayClock > 0 ? (uint32_t)(PlayClock / 4) : 0u;
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

	static uint32_t CoherencyFloatBits(float value)
	{
		static_assert(sizeof(uint32_t) == sizeof(float), "unexpected float size");
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	static uint64_t HashUploadPayloadBytes(const void* data, uint64_t size)
	{
		return NRIHashUploadPayloadBytes(data, size);
	}

	static uint64_t HashMaterialPayloadData(const nri_scene::MaterialBridgeData& materialBridge)
	{
		const uint64_t materialSize = (uint64_t)materialBridge.materials.size() * sizeof(nri_scene::MaterialData);
		return HashUploadPayloadBytes(
			materialBridge.materials.empty() ? nullptr : materialBridge.materials.data(),
			materialSize);
	}

	struct SceneViewUploadStampBuildResult
	{
		uint64_t vertexPayloadStamp = 0;
		uint64_t indexPayloadStamp = 0;
		uint64_t primitivePayloadStamp = 0;
		uint64_t primitiveProvenanceStamp = 0;
		uint64_t materialPayloadStamp = 0;
	};

	static uint64_t HashSurfaceProvenanceStamp(uint64_t hash, const nri_scene::SurfaceProvenance& provenance)
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

	static uint64_t HashCapturedVertexStamp(uint64_t hash, const nri_scene::CapturedVertex& vertex)
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

	static uint64_t HashMaterialRefStamp(uint64_t hash, const nri_scene::MaterialRef& material)
	{
		hash = nri_scene::HashCombine64(hash, material.texture != nullptr ? (uint64_t)(uint32_t)material.texture->GetID().GetIndex() + 1ull : 0ull);
		hash = nri_scene::HashCombine64(hash, material.emissiveSourceTexture != nullptr ? (uint64_t)(uint32_t)material.emissiveSourceTexture->GetID().GetIndex() + 1ull : 0ull);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(material.palette + 1));
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(material.shade + 1));
		hash = nri_scene::HashCombine64(hash, CoherencyFloatBits(material.alpha));
		hash = nri_scene::HashCombine64(hash, (uint64_t)material.flags);
		return hash;
	}

	static uint32_t CountStampedSurfacePrimitives(const nri_scene::SurfaceRef& surface, bool triangleList)
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

	static SceneViewUploadStampBuildResult BuildSceneViewUploadProducerStamp(const nri_scene::SceneView& sceneView, uint64_t mapWorldBuildSerial)
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




	static void FilterMaterialOnlyReplacementSceneView(nri_scene::SceneView& sceneView, uint32_t reasonMask)
	{
		static constexpr float kMaterialOnlyReplacementDepthNudge = 0.01f;
		const bool keepWalls = (reasonMask & nri_scene::PTMapChunkMutationReason_WallMaterial) != 0;
		const bool keepFlats = (reasonMask & nri_scene::PTMapChunkMutationReason_SectorMaterial) != 0;

		if (!keepWalls)
		{
			sceneView.opaqueWalls.clear();
		}

		if (!keepFlats)
		{
			sceneView.opaqueFlats.clear();
		}

		for (nri_scene::SurfaceRef& surface : sceneView.opaqueWalls)
		{
			NudgeCapturedSurface(surface, kMaterialOnlyReplacementDepthNudge);
		}

		for (nri_scene::SurfaceRef& surface : sceneView.opaqueFlats)
		{
			NudgeCapturedSurface(surface, kMaterialOnlyReplacementDepthNudge);
		}
	}

	static bool SceneViewHasSectorDrivenWallBands(const nri_scene::SceneView& sceneView)
	{
		for (const nri_scene::SurfaceRef& surface : sceneView.opaqueWalls)
		{
			if (surface.provenance.sourceType == nri_scene::SurfaceSourceType::MapWallBand)
			{
				return true;
			}
		}

		return false;
	}

	static float ClampDirectionalAngularSize(float angularSize)
	{
		if (!std::isfinite(angularSize))
		{
			return 0.03f;
		}

		return std::clamp(angularSize, 0.001f, 1.2f);
	}

	static uint32_t PackDirectionalLightColor24(const float color[3])
	{
		auto packChannel = [](float value) -> uint32_t
		{
			const float clamped = std::clamp(value, 0.0f, 8.0f);
			return (uint32_t)std::clamp((int)std::lround((double)(clamped * (255.0f / 8.0f))), 0, 255);
		};

		const uint32_t r = packChannel(color[0]);
		const uint32_t g = packChannel(color[1]);
		const uint32_t b = packChannel(color[2]);
		return r | (g << 8u) | (b << 16u);
	}

	static uint32_t PackDirectionalAngularSize16(float angularSize)
	{
		const float normalized = ClampDirectionalAngularSize(angularSize) / 1.2f;
		return (uint32_t)std::clamp((int)std::lround((double)(normalized * 65535.0f)), 0, 65535);
	}

	static const char* GetDirectionalLightSourceName(const NRIDirectionalLightState& state)
	{
		if (!state.enabled)
		{
			return "off";
		}

		return state.fromOverlay ? "overlay" : "default";
	}

	static bool ResolveSurfaceProbeTextureDebugInfo(uint32_t textureId, FString& outTextureName, int32_t& outLegacyTile)
	{
		outTextureName = "(none)";
		outLegacyTile = -1;
		if (textureId == 0)
		{
			return false;
		}

		auto texture = TexMan.GameByIndex((int)textureId);
		if (texture == nullptr)
		{
			return false;
		}

		outTextureName = texture->GetName();
		if (textureId >= (uint32_t)firstarttile && textureId <= (uint32_t)(firstarttile + maxarttile))
		{
			outLegacyTile = legacyTileNum(FSetTextureID((int)textureId));
		}
		return true;
	}

}





namespace
{
	constexpr uint32_t NRI_TRACE_SHADER_STATS_COUNTER_COUNT = NRIRenderer::TraceShaderStatCount;
	constexpr uint32_t NRI_MAX_EMISSIVE_SURFACES = 4096;

	struct NriSceneTextureLimitValidation
	{
		uint32_t requiredSceneTextureCap = NRI_MAX_SCENE_TEXTURES;
		uint32_t requiredSceneTextureSetDescriptors = NRI_SCENE_DESCRIPTOR_NUM;
		uint32_t requiredStageTextureDescriptors = NRI_SCENE_DESCRIPTOR_NUM + NRI_INPUT_DESCRIPTOR_NUM;
		bool descriptorSetTextureLimitOk = false;
		bool descriptorSetUpdateAfterSetTextureLimitOk = false;
		bool shaderStageTextureLimitOk = false;
		bool shaderStageUpdateAfterSetTextureLimitOk = false;
	};

	static NriSceneTextureLimitValidation ValidateSceneTextureDescriptorLimits(const nri::DeviceDesc& deviceDesc)
	{
		NriSceneTextureLimitValidation validation = {};
		validation.descriptorSetTextureLimitOk =
			deviceDesc.descriptorSet.textureMaxNum >= validation.requiredSceneTextureSetDescriptors;
		validation.descriptorSetUpdateAfterSetTextureLimitOk =
			deviceDesc.descriptorSet.updateAfterSet.textureMaxNum >= validation.requiredSceneTextureSetDescriptors;
		validation.shaderStageTextureLimitOk =
			deviceDesc.shaderStage.descriptorTextureMaxNum >= validation.requiredStageTextureDescriptors;
		validation.shaderStageUpdateAfterSetTextureLimitOk =
			deviceDesc.shaderStage.updateAfterSet.descriptorTextureMaxNum >= validation.requiredStageTextureDescriptors;
		return validation;
	}

	static const char* GetSceneTextureDescriptorLimitFailureReason(const nri::DeviceDesc& deviceDesc)
	{
		const NriSceneTextureLimitValidation validation = ValidateSceneTextureDescriptorLimits(deviceDesc);
		if (!validation.descriptorSetTextureLimitOk)
		{
			return "descriptor-set texture limit is below the NRI PT 1024-scene-texture requirement";
		}
		if (!validation.descriptorSetUpdateAfterSetTextureLimitOk)
		{
			return "update-after-set texture limit is below the NRI PT 1024-scene-texture requirement";
		}
		if (!validation.shaderStageTextureLimitOk)
		{
			return "per-stage texture descriptor limit is below the NRI PT 1024-scene-texture requirement";
		}
		if (!validation.shaderStageUpdateAfterSetTextureLimitOk)
		{
			return "per-stage update-after-set texture descriptor limit is below the NRI PT 1024-scene-texture requirement";
		}
		return nullptr;
	}

	static void LogSceneTextureDescriptorLimits(const nri::DeviceDesc& deviceDesc)
	{
		const NriSceneTextureLimitValidation validation = ValidateSceneTextureDescriptorLimits(deviceDesc);
		Printf(
			"NRI PT scene texture cap: cap=%u scene_set=%u stage_textures=%u limits=set:%u set_uas:%u stage:%u stage_uas:%u supported=%s\n",
			validation.requiredSceneTextureCap,
			validation.requiredSceneTextureSetDescriptors,
			validation.requiredStageTextureDescriptors,
			deviceDesc.descriptorSet.textureMaxNum,
			deviceDesc.descriptorSet.updateAfterSet.textureMaxNum,
			deviceDesc.shaderStage.descriptorTextureMaxNum,
			deviceDesc.shaderStage.updateAfterSet.descriptorTextureMaxNum,
			GetSceneTextureDescriptorLimitFailureReason(deviceDesc) == nullptr ? "yes" : "no");
	}
	constexpr int NRI_TEMPORAL_TRACE_REARM_FRAME_COUNT = 8;
	constexpr float NRI_TAA_EXPOSURE_RESET_THRESHOLD_STOPS = 0.5f;
	constexpr uint32_t NRI_SECTOR_LIGHTING_FLAG_ENABLED = 0x1u;


	bool ShouldTracePtPerf()
	{
		return PerfLoopTraceActive() || ShouldEmitRendererTemporalTraceLogs();
	}

	bool ShouldCollectPtPerfTiming()
	{
		return ShouldTracePtPerf() || (bool)nri_ptslowdowntrace;
	}

	bool ShouldCollectTraceShaderStats()
	{
		return !!nri_ptshaderstats && ShouldTracePtPerf();
	}

	uint32_t ScoreRuntimeSectorDirtyTruthEntry(const NRIRenderer::RuntimeSectorDirtyTruthTraceEntry& entry)
	{
		uint32_t score = 0;
		if (entry.forceTopology)
		{
			score += 1u << 18;
		}
		if (entry.baselineChanged)
		{
			score += 1u << 17;
		}
		if (entry.geometryChanged)
		{
			score += 1u << 16;
		}
		if (entry.materialChanged)
		{
			score += 1u << 15;
		}
		score += entry.liveTriangleCount * 8u;
		score += entry.liveSurfaceCount * 4u;
		return score;
	}

	uint32_t ScoreRuntimeAnimatedChurnTraceEntry(const NRIRenderer::RuntimeAnimatedChurnTraceEntry& entry)
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

	uint32_t ScoreRuntimeMaterialOnlyMismatchTraceEntry(const NRIRenderer::RuntimeMaterialOnlyMismatchTraceEntry& entry)
	{
		const uint32_t materialDelta =
			entry.residentMaterialCount > entry.filteredMaterialCount ?
			entry.residentMaterialCount - entry.filteredMaterialCount :
			entry.filteredMaterialCount - entry.residentMaterialCount;
		uint32_t score = materialDelta * 128u;
		score += entry.residentMaterialCount * 16u;
		score += entry.filteredSurfaceCount * 8u;
		score += (entry.filteredWallCount + entry.filteredFlatCount) * 4u;
		if (entry.filteredWallCount != 0 && entry.filteredFlatCount != 0)
		{
			score += 1u << 18;
		}
		if (entry.residentWallCount != 0 && entry.residentFlatCount != 0)
		{
			score += 1u << 17;
		}
		return score;
	}

	enum RuntimeResidentBlasRecreateFallbackBits : uint32_t
	{
		RuntimeResidentBlasRecreateFallback_NoPreviousAs = 1u << 0,
		RuntimeResidentBlasRecreateFallback_RecoveredEmpty = 1u << 1,
		RuntimeResidentBlasRecreateFallback_SliceMoved = 1u << 2,
		RuntimeResidentBlasRecreateFallback_TopologyChanged = 1u << 3,
		RuntimeResidentBlasRecreateFallback_ForceTopology = 1u << 4,
	};

	uint32_t ScoreRuntimeResidentBlasRecreateTraceEntry(const NRIRenderer::RuntimeResidentBlasRecreateTraceEntry& entry)
	{
		uint32_t score = entry.triangleCount * 8u;
		score += entry.surfaceCount * 4u;
		score += entry.materialCount * 2u;
		if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_TopologyChanged) != 0)
		{
			score += 1u << 20;
		}
		if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_ForceTopology) != 0)
		{
			score += 1u << 19;
		}
		if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_SliceMoved) != 0)
		{
			score += 1u << 18;
		}
		if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_NoPreviousAs) != 0)
		{
			score += 1u << 17;
		}
		if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_RecoveredEmpty) != 0)
		{
			score += 1u << 16;
		}
		return score;
	}

	enum RuntimeResidentBlasRefitRejectBits : uint32_t
	{
		RuntimeResidentBlasRefitReject_NoPreviousAs = 1u << 0,
		RuntimeResidentBlasRefitReject_IndexCountMismatch = 1u << 1,
		RuntimeResidentBlasRefitReject_PrimitiveCountMismatch = 1u << 2,
		RuntimeResidentBlasRefitReject_ZeroIndexCount = 1u << 3,
		RuntimeResidentBlasRefitReject_ZeroPrimitiveCount = 1u << 4,
	};

	uint32_t ScoreRuntimeResidentBlasRefitRejectTraceEntry(const NRIRenderer::RuntimeResidentBlasRefitRejectTraceEntry& entry)
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
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_IndexCountMismatch) != 0)
		{
			score += 1u << 20;
		}
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_PrimitiveCountMismatch) != 0)
		{
			score += 1u << 19;
		}
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_NoPreviousAs) != 0)
		{
			score += 1u << 18;
		}
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_ZeroIndexCount) != 0)
		{
			score += 1u << 17;
		}
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_ZeroPrimitiveCount) != 0)
		{
			score += 1u << 16;
		}
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

	uint32_t ScoreRuntimeStructuralRebuildTraceEntry(const NRIRenderer::RuntimeStructuralRebuildTraceEntry& entry)
	{
		uint32_t score = entry.triangleCount * 8u;
		score += entry.surfaceCount * 4u;
		score += entry.materialCount * 2u;
		if ((entry.triggerMask & RuntimeStructuralRebuildTrigger_ForceTopology) != 0)
		{
			score += 1u << 20;
		}
		if ((entry.triggerMask & RuntimeStructuralRebuildTrigger_StaticAnimatedFlip) != 0)
		{
			score += 1u << 19;
		}
		if ((entry.triggerMask & RuntimeStructuralRebuildTrigger_ExcludeStaticFlip) != 0)
		{
			score += 1u << 18;
		}
		if (entry.mixedMaterialOnly)
		{
			score += 1u << 17;
		}
		if (entry.geometryOrDirty)
		{
			score += 1u << 16;
		}
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

	uint32_t ScoreRuntimeGeometryDirtyTraceEntry(const NRIRenderer::RuntimeGeometryDirtyTraceEntry& entry)
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
		if ((entry.familyMask & RuntimeGeometryDirtyFamily_GeometryDirtyMixed) != 0)
		{
			score += 1u << 20;
		}
		if ((entry.familyMask & RuntimeGeometryDirtyFamily_SectorWallGeometry) != 0)
		{
			score += 1u << 19;
		}
		if (entry.forceTopology)
		{
			score += 1u << 18;
		}
		if (entry.countChanged)
		{
			score += 1u << 17;
		}
		if (entry.wallsChanged && entry.flatsChanged)
		{
			score += 1u << 16;
		}
		return score;
	}

	uint32_t ScoreRuntimeRecurringChunkTraceEntry(const NRIRenderer::RuntimeRecurringChunkTraceEntry& entry)
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

	std::string FormatTopologyKeyList(const std::vector<uint64_t>& keys, size_t limit = 8)
	{
		if (keys.empty())
		{
			return "none";
		}

		std::string result;
		const size_t printCount = std::min(keys.size(), limit);
		char buffer[32] = {};
		for (size_t i = 0; i < printCount; ++i)
		{
			if (!result.empty())
			{
				result += ",";
			}

			std::snprintf(buffer, sizeof(buffer), "0x%016llx", (unsigned long long)keys[i]);
			result += buffer;
		}

		if (printCount < keys.size())
		{
			result += ",...";
		}

		return result;
	}

	double GetCurrentGameplayTimeSeconds()
	{
		return PlayClock > 0 ? (double)PlayClock * BuildTickSeconds : 0.0;
	}

	double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
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

	struct ScenePortalData
	{
		uint32_t traversalClass = 0;
		uint32_t kind = 0;
		uint32_t targetLocalSpaceIndex = UINT32_MAX;
		uint32_t flags = 0;
		float delta[3] = {};
		uint32_t reserved0 = 0;
	};

	static nri::StageBits NRIComputeStage()
	{
		return nri::StageBits::COMPUTE_SHADER;
	}

	static const char* GetNrdHitDistanceReconstructionModeName(uint32_t mode)
	{
		switch (mode)
		{
		case 1: return "area_3x3";
		case 2: return "area_5x5";
		default: return "off";
		}
	}

	static bool IsSupportedPtDebugMode(uint32_t debugMode)
	{
		return IsNRIFrameGraphSupportedDebugMode(debugMode);
	}

	static uint32_t GetEffectivePtDebugMode()
	{
		if (nri_ptdebug < 0 || nri_ptdebug > (int)nri_diag::PtDebugTaaPreExposedInput)
		{
			return 0u;
		}

		const uint32_t debugMode = (uint32_t)nri_ptdebug;
		return IsSupportedPtDebugMode(debugMode) ? debugMode : 0u;
	}

	static float GetTemporalExposure(const NRIPTOutputPolicy& outputPolicy)
	{
		return std::max(outputPolicy.exposure, 0.125f);
	}

	static float GetFullbrightBoostScale()
	{
		return std::clamp((float)nri_ptfullbrightboost, 0.50f, 8.00f);
	}

	static float GetGlowmapVisibleBlendScale()
	{
		return std::clamp((float)nri_ptglowblend, 0.0f, 3.0f);
	}

	static float GetExposureDeltaStops(float previousExposure, float currentExposure)
	{
		const float safePrevious = std::max(previousExposure, 0.125f);
		const float safeCurrent = std::max(currentExposure, 0.125f);
		return std::abs(std::log2(safeCurrent) - std::log2(safePrevious));
	}

	static uint32_t GetBootstrapMode();

	static NRIPresentRouteInfo ResolvePresentRouteInfo(uint32_t debugMode, bool bootstrap)
	{
		NRIFrameRouteRequest request = {};
		request.debugMode = debugMode;
		request.bootstrap = bootstrap;
		request.bootstrapMode = bootstrap ? GetBootstrapMode() : 0u;
		return ResolveNRIFrameRoute(request);
	}

	static NRINrdDenoiserMode GetSelectedNrdDenoiserMode()
	{
		return (NRINrdDenoiserMode)std::clamp((int)nri_nrddenoiser, 0, 1);
	}

	static const char* GetNrdDenoiserModeName(NRINrdDenoiserMode mode)
	{
		switch (mode)
		{
		case NRINrdDenoiserMode::Relax: return "RELAX_DIFFUSE_SPECULAR";
		default: return "REBLUR_DIFFUSE_SPECULAR";
		}
	}

	static const char* GetNrdInputSplitModeName(uint32_t mode)
	{
		switch (mode)
		{
		case 1: return "raw_left_denoised_right";
		case 2: return "denoised_left_raw_right";
		default: return "off";
		}
	}

	static bool SameRuntimeLinkDebugState(const RuntimeLinkDebugState& a, const RuntimeLinkDebugState& b)
	{
		return
			a.available == b.available &&
			a.specialWaterSector == b.specialWaterSector &&
			a.playerSectorIndex == b.playerSectorIndex &&
			a.playerSectorLotag == b.playerSectorLotag &&
			a.playerSectorHitag == b.playerSectorHitag &&
			a.effectiveSectorLotag == b.effectiveSectorLotag &&
			a.actorSectorIndex == b.actorSectorIndex &&
			a.actorSectorLotag == b.actorSectorLotag &&
			a.actorSectorHitag == b.actorSectorHitag &&
			a.onWarpingSector == b.onWarpingSector &&
			a.transporterHold == b.transporterHold &&
			a.rrGeoCount == b.rrGeoCount;
	}

	static bool SameRuntimeTaggedSectorDebugInfo(const RuntimeTaggedSectorDebugInfo& a, const RuntimeTaggedSectorDebugInfo& b)
	{
		if (a.available != b.available ||
			a.sectorIndex != b.sectorIndex ||
			a.lotag != b.lotag ||
			a.hitag != b.hitag ||
			a.effectorCount != b.effectorCount)
		{
			return false;
		}

		for (size_t i = 0; i < countof(a.effectorLotags); ++i)
		{
			if (a.effectorLotags[i] != b.effectorLotags[i] || a.effectorHitags[i] != b.effectorHitags[i])
			{
				return false;
			}
		}

		return true;
	}

	static bool ShouldStoreRuntimeSectorControlInfo(const RuntimeTaggedSectorDebugInfo& info)
	{
		return info.available && (info.lotag != 0 || info.hitag != 0 || info.effectorCount > 0);
	}

	static bool AppendRuntimeSectorControlInfo(std::array<RuntimeTaggedSectorDebugInfo, 12>& infos, uint32_t& infoCount, const RuntimeTaggedSectorDebugInfo& info)
	{
		if (!ShouldStoreRuntimeSectorControlInfo(info))
		{
			return false;
		}

		for (uint32_t i = 0; i < infoCount; ++i)
		{
			if (infos[i].sectorIndex == info.sectorIndex)
			{
				return false;
			}
		}

		if (infoCount >= infos.size())
		{
			return false;
		}

		infos[infoCount++] = info;
		return true;
	}

	static bool GetRuntimeSectorControlInfo(int sectorIndex, RuntimeTaggedSectorDebugInfo& info)
	{
		if (!validSectorIndex(sectorIndex))
		{
			return false;
		}

		info = {};
		if (gi != nullptr && gi->GetRuntimeLinkDebugTaggedSectorInfo(sectorIndex, &info))
		{
			return true;
		}

		const auto& sec = sector[(unsigned)sectorIndex];
		info.available = true;
		info.sectorIndex = sectorIndex;
		info.lotag = sec.lotag;
		info.hitag = sec.hitag;
		return true;
	}


	static uint32_t GetDispatchSize(uint32_t value)
	{
		return (value + 7u) / 8u;
	}

	static uint64_t HashPrimitiveRewriteProvenancePayload(const std::vector<nri_scene::SurfaceProvenance>& provenanceList)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, (uint64_t)provenanceList.size());
		for (const nri_scene::SurfaceProvenance& provenance : provenanceList)
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
		}
		return hash != 0 ? hash : 1;
	}

	static uint64_t HashPrimitiveRewriteVisibilityIdentity(const nri_scene::PTMapWorld& mapWorld)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, mapWorld.valid ? 1ull : 0ull);
		hash = nri_scene::HashCombine64(hash, mapWorld.buildSerial);
		hash = nri_scene::HashCombine64(hash, (uint64_t)mapWorld.chunks.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)mapWorld.stats.chunkCount);
		for (const nri_scene::PTMapChunk& chunk : mapWorld.chunks)
		{
			hash = nri_scene::HashCombine64(hash, (uint64_t)chunk.chunkIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(chunk.sectorIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)chunk.firstSurface);
			hash = nri_scene::HashCombine64(hash, (uint64_t)chunk.surfaceCount);
		}
		return hash != 0 ? hash : 1;
	}

	static void MarkChunkVisible(std::vector<uint32_t>& visibleChunkWords, uint32_t chunkIndex)
	{
		const size_t wordIndex = (size_t)(chunkIndex >> 5u);
		if (wordIndex >= visibleChunkWords.size())
		{
			return;
		}

		visibleChunkWords[wordIndex] |= 1u << (chunkIndex & 31u);
	}

	static bool IsChunkMarkedVisible(const std::vector<uint32_t>& visibleChunkWords, uint32_t chunkIndex)
	{
		const size_t wordIndex = (size_t)(chunkIndex >> 5u);
		if (wordIndex >= visibleChunkWords.size())
		{
			return false;
		}

		return (visibleChunkWords[wordIndex] & (1u << (chunkIndex & 31u))) != 0u;
	}

	static uint32_t GetFlatPlaneVisibilityIndex(int32_t sectorIndex, bool ceiling)
	{
		return (uint32_t)sectorIndex * 2u + (ceiling ? 1u : 0u);
	}

	static void MarkFlatPlaneVisible(std::vector<uint32_t>& visibleFlatPlaneWords, int32_t sectorIndex, bool ceiling)
	{
		if (sectorIndex < 0)
		{
			return;
		}

		const uint32_t flatPlaneIndex = GetFlatPlaneVisibilityIndex(sectorIndex, ceiling);
		const size_t wordIndex = (size_t)(flatPlaneIndex >> 5u);
		if (wordIndex >= visibleFlatPlaneWords.size())
		{
			return;
		}

		visibleFlatPlaneWords[wordIndex] |= 1u << (flatPlaneIndex & 31u);
	}

	static bool IsFlatPlaneMarkedVisible(const std::vector<uint32_t>& visibleFlatPlaneWords, int32_t sectorIndex, bool ceiling)
	{
		if (sectorIndex < 0)
		{
			return false;
		}

		const uint32_t flatPlaneIndex = GetFlatPlaneVisibilityIndex(sectorIndex, ceiling);
		const size_t wordIndex = (size_t)(flatPlaneIndex >> 5u);
		if (wordIndex >= visibleFlatPlaneWords.size())
		{
			return false;
		}

		return (visibleFlatPlaneWords[wordIndex] & (1u << (flatPlaneIndex & 31u))) != 0u;
	}

	static void MarkVisibleChunkForSector(const nri_scene::PTMapWorld& mapWorld, int32_t sectorIndex, std::vector<uint32_t>& visibleChunkWords)
	{
		const int32_t chunkIndex = nri_static_scene_geometry::FindMapChunkIndexForSector(mapWorld, sectorIndex);
		if (chunkIndex >= 0)
		{
			MarkChunkVisible(visibleChunkWords, (uint32_t)chunkIndex);
		}
	}

	static void AccumulateVisibleChunksFromViewRoots(const HWDrawInfo& di, const nri_scene::PTMapWorld& mapWorld, std::vector<uint32_t>& visibleChunkWords)
	{
		if (di.Viewpoint.SectNums != nullptr)
		{
			for (int i = 0; i < di.Viewpoint.SectCount; ++i)
			{
				MarkVisibleChunkForSector(mapWorld, di.Viewpoint.SectNums[i], visibleChunkWords);
			}
		}
		else
		{
			MarkVisibleChunkForSector(mapWorld, di.Viewpoint.SectCount, visibleChunkWords);
		}
	}

	static void AccumulateVisibleChunksFromDrawLists(const HWDrawInfo& di, const nri_scene::PTMapWorld& mapWorld, std::vector<uint32_t>& visibleChunkWords)
	{
		for (int drawListType = 0; drawListType < GLDL_TYPES; ++drawListType)
		{
			const HWDrawList& drawList = di.drawlists[drawListType];

			for (const HWWall* wall : drawList.walls)
			{
				if (wall != nullptr && wall->seg != nullptr)
				{
					MarkVisibleChunkForSector(mapWorld, wall->seg->sector, visibleChunkWords);
				}
			}

			for (const HWFlat* flat : drawList.flats)
			{
				if (flat != nullptr && flat->sec != nullptr)
				{
					MarkVisibleChunkForSector(mapWorld, sector.IndexOf(flat->sec), visibleChunkWords);
				}
			}
		}
	}

	static void AccumulateVisibleFlatPlanesFromDrawLists(const HWDrawInfo& di, std::vector<uint32_t>& visibleFlatPlaneWords)
	{
		for (int drawListType = 0; drawListType < GLDL_TYPES; ++drawListType)
		{
			const HWDrawList& drawList = di.drawlists[drawListType];
			for (const HWFlat* flat : drawList.flats)
			{
				if (flat == nullptr || flat->sec == nullptr || flat->Sprite != nullptr)
				{
					continue;
				}

				MarkFlatPlaneVisible(
					visibleFlatPlaneWords,
					sector.IndexOf(flat->sec),
					flat->plane != 0);
			}
		}
	}

	static float Clamp01(float value)
	{
		return std::max(0.0f, std::min(value, 1.0f));
	}

	static uint32_t ClampTraceBounceCount(int value, uint32_t maxValue)
	{
		return (uint32_t)std::max(0, std::min(value, (int)maxValue));
	}

	static float GetBaseAmbient()
	{
		return std::max(0.0f, (float)nri_ptbaseambient);
	}

	static float GetMetalAmbient()
	{
		return std::max(0.0f, (float)nri_ptmetalambient);
	}

	static uint32_t PackAmbientMultiplier12(float value)
	{
		return (uint32_t)std::min(4095.0f, std::max(0.0f, value) * 1024.0f + 0.5f);
	}

	static uint32_t PackPortalDepthAndAmbientMultipliers(uint32_t portalDepth, float baseAmbient, float metalAmbient)
	{
		return
			(portalDepth & 0xffu) |
			(PackAmbientMultiplier12(baseAmbient) << 8u) |
			(PackAmbientMultiplier12(metalAmbient) << 20u);
	}

	static uint32_t PackTraceBounceCounts(uint32_t lightBounceCount, uint32_t mirrorBounceCount, const float directionalColor[3])
	{
		return
			(lightBounceCount & 0xfu) |
			((mirrorBounceCount & 0xfu) << 4u) |
			(PackDirectionalLightColor24(directionalColor) << 8u);
	}

	static uint32_t PackTraceAux1(uint32_t denoiserMode, uint32_t emissiveSampleCount, float directionalAngularSize)
	{
		return
			(denoiserMode & 0xffu) |
			((emissiveSampleCount & 0xffu) << 8u) |
			(PackDirectionalAngularSize16(directionalAngularSize) << 16u);
	}

	static uint32_t PackDenoiserAux1(uint32_t denoiserMode, float directionalAngularSize)
	{
		return (denoiserMode & 0xffu) | (PackDirectionalAngularSize16(directionalAngularSize) << 16u);
	}

	static uint32_t PackUInt16Pair(uint32_t lo, uint32_t hi)
	{
		return (lo & 0xffffu) | ((hi & 0xffffu) << 16u);
	}

	static uint32_t GetPortalTraversalClass(nri_scene::PTPortalKind kind)
	{
		switch (kind)
		{
		case nri_scene::PTPortalKind::WallMirror:
		case nri_scene::PTPortalKind::SectorFloorMirror:
		case nri_scene::PTPortalKind::SectorCeilingMirror:
			return NRI_PORTAL_TRAVERSAL_CLASS_REFLECTIVE;

		case nri_scene::PTPortalKind::WallView:
		case nri_scene::PTPortalKind::SectorFloorStack:
		case nri_scene::PTPortalKind::SectorCeilingStack:
			return NRI_PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER;

		case nri_scene::PTPortalKind::WallToSprite:
			return NRI_PORTAL_TRAVERSAL_CLASS_RUNTIME_BOUND;

		default:
			return NRI_PORTAL_TRAVERSAL_CLASS_NONE;
		}
	}

	static uint32_t CountPortalTraversalClass(const nri_scene::PTMapWorld& mapWorld, uint32_t traversalClass)
	{
		uint32_t count = 0;
		for (const auto& portal : mapWorld.portals)
		{
			if (GetPortalTraversalClass(portal.kind) == traversalClass)
			{
				count++;
			}
		}
		return count;
	}

	static uint32_t CountPendingPlanePortals(const nri_scene::PTMapWorld& mapWorld)
	{
		uint32_t count = 0;
		for (const auto& portal : mapWorld.portals)
		{
			switch (portal.kind)
			{
			case nri_scene::PTPortalKind::SectorFloorStack:
			case nri_scene::PTPortalKind::SectorCeilingStack:
			case nri_scene::PTPortalKind::SectorFloorMirror:
			case nri_scene::PTPortalKind::SectorCeilingMirror:
				if (portal.sourceSurfaceIndex == UINT32_MAX)
				{
					count++;
				}
				break;
			default:
				break;
			}
		}
		return count;
	}

	static std::vector<ScenePortalData> BuildScenePortalData(const nri_scene::PTMapWorld& mapWorld)
	{
		std::vector<ScenePortalData> portals;
		portals.reserve(std::max<size_t>(mapWorld.portals.size(), 1u));

		for (const auto& portal : mapWorld.portals)
		{
			ScenePortalData data = {};
			data.traversalClass = GetPortalTraversalClass(portal.kind);
			data.kind = (uint32_t)portal.kind;
			data.flags = portal.runtimeBoundTarget ? NRI_PORTAL_FLAG_RUNTIME_BOUND : 0u;
			if (portal.targetCount > 0 && portal.firstTarget < mapWorld.portalTargets.size())
			{
				data.targetLocalSpaceIndex = mapWorld.portalTargets[portal.firstTarget].localSpaceIndex;
			}
			data.delta[0] = (float)portal.delta[0];
			data.delta[1] = (float)portal.delta[1];
			data.delta[2] = (float)portal.delta[2];
			portals.push_back(data);
		}

		if (portals.empty())
		{
			portals.push_back({});
		}

		return portals;
	}

	struct StartupMapWorldChunkDiffSample
	{
		uint32_t chunkIndex = 0;
		int32_t currentSectorIndex = -1;
		int32_t rebuiltSectorIndex = -1;
		uint32_t currentSurfaceCount = 0;
		uint32_t rebuiltSurfaceCount = 0;
		uint32_t currentTriangleCount = 0;
		uint32_t rebuiltTriangleCount = 0;
		uint32_t surfaceDiffCount = 0;
	};

	struct StartupMapWorldDiffDetails
	{
		bool validMismatch = false;
		bool chunkCountMismatch = false;
		bool surfaceCountMismatch = false;
		bool portalCountMismatch = false;
		bool portalTargetCountMismatch = false;
		bool statsMismatch = false;
		std::vector<StartupMapWorldChunkDiffSample> chunkSamples;
		std::vector<uint32_t> lateVisibleValidationChunks;
	};

	static bool StartupMapWorldSurfaceDiffers(
		const nri_scene::PTMapSurface& currentSurface,
		const nri_scene::PTMapSurface& rebuiltSurface)
	{
		const auto& a = currentSurface.surface;
		const auto& b = rebuiltSurface.surface;
		return currentSurface.kind != rebuiltSurface.kind ||
			a.vertices.size() != b.vertices.size() ||
			a.material.flags != b.material.flags ||
			a.material.texture != b.material.texture ||
			a.material.palette != b.material.palette ||
			a.provenance.sourceType != b.provenance.sourceType ||
			a.provenance.sectorIndex != b.provenance.sectorIndex ||
			a.provenance.wallIndex != b.provenance.wallIndex ||
			a.provenance.sectionIndex != b.provenance.sectionIndex;
	}

	static std::string BuildStartupMapWorldDiffReasonSummary(const StartupMapWorldDiffDetails& details)
	{
		std::string summary;
		auto appendReason = [&summary](const char* reason)
		{
			if (reason == nullptr || reason[0] == '\0')
			{
				return;
			}
			if (!summary.empty())
			{
				summary += ",";
			}
			summary += reason;
		};

		appendReason(details.validMismatch ? "valid" : nullptr);
		appendReason(details.chunkCountMismatch ? "chunk-count" : nullptr);
		appendReason(details.surfaceCountMismatch ? "surface-count" : nullptr);
		appendReason(details.portalCountMismatch ? "portal-count" : nullptr);
		appendReason(details.portalTargetCountMismatch ? "portal-target-count" : nullptr);
		appendReason(details.statsMismatch ? "stats" : nullptr);
		return summary.empty() ? "chunk-or-surface" : summary;
	}

	static bool StartupMapWorldStructureDiffers(
		const nri_scene::PTMapWorld& currentWorld,
		const nri_scene::PTMapWorld& rebuiltWorld,
		uint32_t& outChunkDiffCount,
		uint32_t& outSurfaceDiffCount,
		StartupMapWorldDiffDetails* outDetails = nullptr)
	{
		outChunkDiffCount = 0;
		outSurfaceDiffCount = 0;

		StartupMapWorldDiffDetails localDetails = {};
		StartupMapWorldDiffDetails& details = outDetails != nullptr ? *outDetails : localDetails;
		details = {};

		details.validMismatch = currentWorld.valid != rebuiltWorld.valid;
		details.chunkCountMismatch = currentWorld.chunks.size() != rebuiltWorld.chunks.size();
		details.surfaceCountMismatch = currentWorld.surfaces.size() != rebuiltWorld.surfaces.size();
		details.portalCountMismatch = currentWorld.portals.size() != rebuiltWorld.portals.size();
		details.portalTargetCountMismatch = currentWorld.portalTargets.size() != rebuiltWorld.portalTargets.size();
		details.statsMismatch = std::memcmp(&currentWorld.stats, &rebuiltWorld.stats, sizeof(currentWorld.stats)) != 0;

		const size_t chunkCount = std::min(currentWorld.chunks.size(), rebuiltWorld.chunks.size());
		details.chunkSamples.reserve(std::min<size_t>(chunkCount, 12u));
		details.lateVisibleValidationChunks.reserve(std::min<size_t>(chunkCount, 64u));
		for (size_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
		{
			const auto& currentChunk = currentWorld.chunks[chunkIndex];
			const auto& rebuiltChunk = rebuiltWorld.chunks[chunkIndex];
			const bool chunkMetadataDiff =
				currentChunk.kind != rebuiltChunk.kind ||
				currentChunk.chunkIndex != rebuiltChunk.chunkIndex ||
				currentChunk.sectorIndex != rebuiltChunk.sectorIndex ||
				currentChunk.localSpaceIndex != rebuiltChunk.localSpaceIndex ||
				currentChunk.firstSurface != rebuiltChunk.firstSurface ||
				currentChunk.surfaceCount != rebuiltChunk.surfaceCount ||
				currentChunk.triangleCount != rebuiltChunk.triangleCount;

			const uint32_t currentAvailableSurfaceCount =
				currentChunk.firstSurface < currentWorld.surfaces.size() ?
				(uint32_t)std::min<size_t>(currentChunk.surfaceCount, currentWorld.surfaces.size() - currentChunk.firstSurface) :
				0u;
			const uint32_t rebuiltAvailableSurfaceCount =
				rebuiltChunk.firstSurface < rebuiltWorld.surfaces.size() ?
				(uint32_t)std::min<size_t>(rebuiltChunk.surfaceCount, rebuiltWorld.surfaces.size() - rebuiltChunk.firstSurface) :
				0u;
			const uint32_t comparableSurfaceCount = std::min(currentAvailableSurfaceCount, rebuiltAvailableSurfaceCount);

			uint32_t chunkSurfaceDiffCount = 0;
			for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < comparableSurfaceCount; ++localSurfaceIndex)
			{
				const auto& currentSurface = currentWorld.surfaces[currentChunk.firstSurface + localSurfaceIndex];
				const auto& rebuiltSurface = rebuiltWorld.surfaces[rebuiltChunk.firstSurface + localSurfaceIndex];
				if (StartupMapWorldSurfaceDiffers(currentSurface, rebuiltSurface))
				{
					chunkSurfaceDiffCount++;
				}
			}

			chunkSurfaceDiffCount +=
				currentAvailableSurfaceCount > rebuiltAvailableSurfaceCount ?
				(currentAvailableSurfaceCount - rebuiltAvailableSurfaceCount) :
				(rebuiltAvailableSurfaceCount - currentAvailableSurfaceCount);

			outSurfaceDiffCount += chunkSurfaceDiffCount;
			if (chunkMetadataDiff || chunkSurfaceDiffCount > 0u)
			{
				outChunkDiffCount++;
				const bool lateVisibleValidationCandidate =
					chunkSurfaceDiffCount > 0u &&
					currentAvailableSurfaceCount == rebuiltAvailableSurfaceCount &&
					currentChunk.triangleCount == rebuiltChunk.triangleCount &&
					currentChunk.chunkIndex == rebuiltChunk.chunkIndex;
				if (lateVisibleValidationCandidate)
				{
					details.lateVisibleValidationChunks.push_back(currentChunk.chunkIndex);
				}
				if (details.chunkSamples.size() < 12u)
				{
					StartupMapWorldChunkDiffSample sample = {};
					sample.chunkIndex = (uint32_t)chunkIndex;
					sample.currentSectorIndex = currentChunk.sectorIndex;
					sample.rebuiltSectorIndex = rebuiltChunk.sectorIndex;
					sample.currentSurfaceCount = currentAvailableSurfaceCount;
					sample.rebuiltSurfaceCount = rebuiltAvailableSurfaceCount;
					sample.currentTriangleCount = currentChunk.triangleCount;
					sample.rebuiltTriangleCount = rebuiltChunk.triangleCount;
					sample.surfaceDiffCount = chunkSurfaceDiffCount;
					details.chunkSamples.push_back(sample);
				}
			}
		}

		return details.validMismatch ||
			details.chunkCountMismatch ||
			details.surfaceCountMismatch ||
			details.portalCountMismatch ||
			details.portalTargetCountMismatch ||
			outChunkDiffCount > 0 ||
			outSurfaceDiffCount > 0;
	}

	static void AppendGeometryChunk(
		const nri_scene::GeometryData& source,
		uint32_t sourceVertexOffset,
		uint32_t sourceVertexCount,
		uint32_t sourceIndexOffset,
		uint32_t sourceIndexCount,
		uint32_t sourcePrimitiveOffset,
		uint32_t sourcePrimitiveCount,
		nri_scene::GeometryData& destination)
	{
		if (sourceVertexOffset >= source.vertices.size() ||
			sourcePrimitiveOffset >= source.primitives.size() ||
			sourceVertexCount == 0 ||
			sourcePrimitiveCount == 0)
		{
			return;
		}

		sourceVertexCount = std::min(sourceVertexCount, (uint32_t)source.vertices.size() - sourceVertexOffset);
		if (sourceIndexOffset >= source.indices.size())
		{
			sourceIndexCount = 0;
		}
		else
		{
			sourceIndexCount = std::min(sourceIndexCount, (uint32_t)source.indices.size() - sourceIndexOffset);
		}
		sourcePrimitiveCount = std::min(sourcePrimitiveCount, (uint32_t)source.primitives.size() - sourcePrimitiveOffset);
		const uint32_t sourcePrimitiveProvenanceCount =
			sourcePrimitiveOffset < source.primitiveProvenance.size() ?
			std::min(sourcePrimitiveCount, (uint32_t)source.primitiveProvenance.size() - sourcePrimitiveOffset) :
			0u;

		const uint32_t vertexBase = (uint32_t)destination.vertices.size();
		destination.vertices.insert(
			destination.vertices.end(),
			source.vertices.begin() + sourceVertexOffset,
			source.vertices.begin() + sourceVertexOffset + sourceVertexCount);

		if (sourceIndexCount > 0)
		{
			destination.indices.reserve(destination.indices.size() + sourceIndexCount);
			for (uint32_t i = 0; i < sourceIndexCount; ++i)
			{
				destination.indices.push_back(vertexBase + source.indices[sourceIndexOffset + i] - sourceVertexOffset);
			}
		}

		destination.primitives.reserve(destination.primitives.size() + sourcePrimitiveCount);
		for (uint32_t i = 0; i < sourcePrimitiveCount; ++i)
		{
			nri_scene::PrimitiveData copy = source.primitives[sourcePrimitiveOffset + i];
			copy.indices[0] = vertexBase + copy.indices[0] - sourceVertexOffset;
			copy.indices[1] = vertexBase + copy.indices[1] - sourceVertexOffset;
			copy.indices[2] = vertexBase + copy.indices[2] - sourceVertexOffset;
			destination.primitives.push_back(copy);
		}

		if (sourcePrimitiveProvenanceCount > 0)
		{
			destination.primitiveProvenance.insert(
				destination.primitiveProvenance.end(),
				source.primitiveProvenance.begin() + sourcePrimitiveOffset,
				source.primitiveProvenance.begin() + sourcePrimitiveOffset + sourcePrimitiveProvenanceCount);
		}
	}

	static void RemapMaterialBridgeAgainstTextureTable(
		const nri_scene::MaterialBridgeData& source,
		nri_scene::MaterialBridgeData& inOutTextureTable,
		nri_scene::MaterialBridgeData& outRemapped,
		bool* outTextureTableGrew = nullptr)
	{
		if (outTextureTableGrew != nullptr)
		{
			*outTextureTableGrew = false;
		}

		std::unordered_map<uint64_t, uint32_t> textureLookup;
		textureLookup.reserve(inOutTextureTable.textures.size() + source.textures.size());
		for (uint32_t i = 0; i < (uint32_t)inOutTextureTable.textures.size(); ++i)
		{
			textureLookup.emplace(inOutTextureTable.textures[i].key, i);
		}

		auto remapTextureIndex = [&source, &inOutTextureTable, &textureLookup, outTextureTableGrew](uint32_t textureIndex) -> uint32_t
		{
			if (textureIndex == UINT32_MAX)
			{
				return UINT32_MAX;
			}
			if (textureIndex >= source.textures.size())
			{
				return textureIndex;
			}

			const auto& texture = source.textures[textureIndex];
			auto it = textureLookup.find(texture.key);
			if (it != textureLookup.end())
			{
				return it->second;
			}

			const uint32_t newIndex = (uint32_t)inOutTextureTable.textures.size();
			textureLookup.emplace(texture.key, newIndex);
			inOutTextureTable.textures.push_back(texture);
			if (outTextureTableGrew != nullptr)
			{
				*outTextureTableGrew = true;
			}
			return newIndex;
		};

		outRemapped = {};
		outRemapped.materials.reserve(source.materials.size());
		outRemapped.lightMetadata.reserve(source.lightMetadata.size());

		for (size_t materialIndex = 0; materialIndex < source.materials.size(); ++materialIndex)
		{
			const auto& material = source.materials[materialIndex];
			nri_scene::MaterialData copy = material;
			copy.textureIndex = remapTextureIndex(material.textureIndex);
			copy.normalTextureIndex = remapTextureIndex(material.normalTextureIndex);
			copy.metallicTextureIndex = remapTextureIndex(material.metallicTextureIndex);
			copy.roughnessTextureIndex = remapTextureIndex(material.roughnessTextureIndex);
			copy.emissiveTextureIndex = remapTextureIndex(material.emissiveTextureIndex);
			outRemapped.materials.push_back(copy);

			if (materialIndex < source.lightMetadata.size())
			{
				nri_scene::MaterialLightingMetadata metadata = source.lightMetadata[materialIndex];
				metadata.textureIndex = remapTextureIndex(metadata.textureIndex);
				metadata.glowmapTextureIndex = remapTextureIndex(metadata.glowmapTextureIndex);
				metadata.normalTextureIndex = remapTextureIndex(metadata.normalTextureIndex);
				metadata.metallicTextureIndex = remapTextureIndex(metadata.metallicTextureIndex);
				metadata.roughnessTextureIndex = remapTextureIndex(metadata.roughnessTextureIndex);
				metadata.emissiveTextureIndex = remapTextureIndex(metadata.emissiveTextureIndex);
				outRemapped.lightMetadata.push_back(metadata);
			}
		}

		if (!source.paletteLookup.empty())
		{
			outRemapped.paletteLookup = source.paletteLookup;
			outRemapped.paletteWidth = source.paletteWidth;
			outRemapped.paletteHeight = source.paletteHeight;
			if (inOutTextureTable.paletteLookup.empty())
			{
				inOutTextureTable.paletteLookup = source.paletteLookup;
				inOutTextureTable.paletteWidth = source.paletteWidth;
				inOutTextureTable.paletteHeight = source.paletteHeight;
			}
		}
	}

	static bool TryBuildMergedSectorMaterialOnlyBridge(
		const nri_scene::SceneView& residentChunkView,
		const nri_scene::MaterialBridgeData& residentChunkMaterials,
		const nri_scene::SceneView& filteredLiveChunkView,
		const nri_scene::MaterialBridgeData& filteredLiveMaterials,
		nri_scene::MaterialBridgeData& outMergedMaterials)
	{
		if (!filteredLiveChunkView.opaqueWalls.empty())
		{
			return false;
		}

		const uint32_t residentWallCount = (uint32_t)residentChunkView.opaqueWalls.size();
		const uint32_t residentFlatCount = (uint32_t)residentChunkView.opaqueFlats.size();
		if (residentFlatCount == 0 ||
			filteredLiveChunkView.opaqueFlats.size() != residentFlatCount ||
			filteredLiveMaterials.materials.size() != residentFlatCount ||
			filteredLiveMaterials.lightMetadata.size() != residentFlatCount)
		{
			return false;
		}

		if (residentChunkMaterials.materials.size() != residentChunkMaterials.lightMetadata.size() ||
			residentWallCount + residentFlatCount > residentChunkMaterials.materials.size())
		{
			return false;
		}

		outMergedMaterials = residentChunkMaterials;
		nri_scene::MaterialBridgeData remappedFlatMaterials;
		RemapMaterialBridgeAgainstTextureTable(
			filteredLiveMaterials,
			outMergedMaterials,
			remappedFlatMaterials);
		if (remappedFlatMaterials.materials.size() != residentFlatCount ||
			remappedFlatMaterials.lightMetadata.size() != residentFlatCount)
		{
			return false;
		}

		std::copy_n(
			remappedFlatMaterials.materials.data(),
			residentFlatCount,
			outMergedMaterials.materials.begin() + residentWallCount);
		std::copy_n(
			remappedFlatMaterials.lightMetadata.data(),
			residentFlatCount,
			outMergedMaterials.lightMetadata.begin() + residentWallCount);
		return true;
	}

	static bool TryBuildMergedSectorMaterialOnlySceneView(
		const nri_scene::SceneView& residentChunkView,
		const nri_scene::SceneView& filteredLiveChunkView,
		nri_scene::SceneView& outMergedSceneView)
	{
		if (!filteredLiveChunkView.opaqueWalls.empty() ||
			residentChunkView.opaqueFlats.size() != filteredLiveChunkView.opaqueFlats.size())
		{
			return false;
		}

		outMergedSceneView = filteredLiveChunkView;
		outMergedSceneView.opaqueWalls = residentChunkView.opaqueWalls;
		outMergedSceneView.opaqueSprites = residentChunkView.opaqueSprites;
		outMergedSceneView.stats.totalDrawItems =
			(unsigned)(outMergedSceneView.opaqueWalls.size() +
				outMergedSceneView.opaqueFlats.size() +
				outMergedSceneView.opaqueSprites.size());
		outMergedSceneView.stats.wallDrawItems = (unsigned)outMergedSceneView.opaqueWalls.size();
		outMergedSceneView.stats.flatDrawItems = (unsigned)outMergedSceneView.opaqueFlats.size();
		outMergedSceneView.stats.spriteDrawItems = (unsigned)outMergedSceneView.opaqueSprites.size();
		outMergedSceneView.stats.materialRefs = outMergedSceneView.stats.totalDrawItems;
		return true;
	}

	static bool StructuredBufferUpdateNeedsWait(
		const NRIBufferResource& resource,
		const void* data,
		uint64_t size,
		uint32_t stride)
	{
		const uint64_t requiredSize = std::max<uint64_t>(size, stride);
		const bool needsGrowth =
			resource.buffer == nullptr ||
			resource.shaderView == nullptr ||
			resource.stride != stride ||
			resource.size < requiredSize;
		if (needsGrowth)
		{
			return resource.buffer != nullptr || resource.shaderView != nullptr;
		}

		return data != nullptr && size != 0;
	}

	static const char* GetUpscalerFamilyName(NRIMainUpscalerKind kind, bool runAppTaa)
	{
		switch (kind)
		{
		case NRIMainUpscalerKind::DLSR: return "vendor-sr";
		case NRIMainUpscalerKind::DLRR: return "vendor-rr";
		default: return runAppTaa ? "native-taa" : "native";
		}
	}

	static uint32_t PackPresentSceneOrigin(int sceneLeft, int sceneTop)
	{
		return (uint16_t)(int16_t)sceneLeft | ((uint32_t)(uint16_t)(int16_t)sceneTop << 16);
	}

	static void ApplyOutputPolicyToPresentConstants(const NRIPTOutputPolicy& policy, NRIPresentConstants& constants)
	{
		constants.OutputMode = (uint32_t)policy.resolvedMode;
		constants.TonemapMode = (uint32_t)policy.tonemapMode;
		constants.OutputFlags =
			(policy.displayInfoAvailable ? NRI_PRESENT_OUTPUT_FLAG_DISPLAY_INFO_AVAILABLE : 0u) |
			(policy.displayHdrSupported ? NRI_PRESENT_OUTPUT_FLAG_DISPLAY_HDR_SUPPORTED : 0u) |
			(policy.hdrSwapChainActive ? NRI_PRESENT_OUTPUT_FLAG_HDR_SWAPCHAIN_ACTIVE : 0u) |
			(policy.offscreenHdrTarget ? NRI_PRESENT_OUTPUT_FLAG_OFFSCREEN_HDR_TARGET : 0u);
		constants.Exposure = policy.exposure;
		constants.Contrast = policy.contrast;
		constants.Saturation = policy.saturation;
		constants.Shoulder = policy.shoulder;
		constants.Toe = policy.toe;
		constants.PaperWhiteNits = policy.paperWhiteNits;
		constants.DisplayMaxLuminance = policy.displayMaxLuminance;
		constants.DisplaySdrLuminance = policy.displaySdrLuminance;
	}

	static void ApplyNightVisionStateToPresentConstants(const NRIPTNightVisionState& state, NRIPresentConstants& constants)
	{
		constants.NightVisionPackedModeTint = PackNightVisionModeAndTint(
			state.mode,
			(float)nri_ptnightvisionred,
			(float)nri_ptnightvisiongreen,
			(float)nri_ptnightvisionblue);
		constants.NightVisionStrength = nri_ptnightvision ? state.strength01 : 0.0f;
		constants.NightVisionExposure = (float)nri_ptnightvisionexposure;
		constants.NightVisionPackedControls = PackNightVisionControls(
			(float)nri_ptnightvisioncontrast,
			(float)nri_ptnightvisionsaturation);
	}

	static float GetHaltonSample(uint32_t index, uint32_t base)
	{
		float inverseBase = 1.0f / (float)base;
		float fraction = inverseBase;
		float result = 0.0f;

		while (index > 0)
		{
			result += fraction * (float)(index % base);
			index /= base;
			fraction *= inverseBase;
		}

		return result;
	}

	static void ComputeTemporalJitter(uint32_t frameIndex, uint32_t jitterPhaseCount, float outJitter[2])
	{
		jitterPhaseCount = std::max(jitterPhaseCount, 1u);
		const uint32_t sampleIndex = (frameIndex % jitterPhaseCount) + 1u;
		outJitter[0] = GetHaltonSample(sampleIndex, 2u) - 0.5f;
		outJitter[1] = GetHaltonSample(sampleIndex, 3u) - 0.5f;
	}

	static void Normalize3(float v[3])
	{
		const float length = std::max(0.0001f, sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}

	static void ApplyDirectionalLightStateToConstants(const NRIDirectionalLightState& state, NRITraceSceneConstants& constants)
	{
		constants.LightDirection[0] = state.direction[0];
		constants.LightDirection[1] = state.direction[1];
		constants.LightDirection[2] = state.direction[2];
		Normalize3(constants.LightDirection);
	}

	static void TransformPoint(const VSMatrix& matrix, float x, float y, float z, float out[4])
	{
		float point[4] = { x, y, z, 1.0f };
		VSMatrix copy = matrix;
		copy.multMatrixPoint(point, out);
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

	static const char* GetGraphicsApiName(nri::GraphicsAPI api)
	{
		switch (api)
		{
		case nri::GraphicsAPI::D3D12: return "d3d12";
		case nri::GraphicsAPI::VK: return "vulkan";
		default: return "unknown";
		}
	}

	static uint64_t HashBytes64(const uint8_t* data, size_t size)
	{
		uint64_t hash = 1469598103934665603ull;
		for (size_t i = 0; i < size; ++i)
		{
			hash ^= (uint64_t)data[i];
			hash *= 1099511628211ull;
		}
		return hash;
	}

	static uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	template <typename SurfaceContainer>
	static bool SurfaceContainerUsesHardwareCanvasTexture(const SurfaceContainer& surfaces)
	{
		for (const auto& surface : surfaces)
		{
			if (surface.material.texture != nullptr &&
				surface.material.texture->isHardwareCanvas())
			{
				return true;
			}
		}

		return false;
	}

	static bool SceneViewUsesHardwareCanvasTexture(const nri_scene::SceneView& sceneView)
	{
		return
			SurfaceContainerUsesHardwareCanvasTexture(sceneView.opaqueWalls) ||
			SurfaceContainerUsesHardwareCanvasTexture(sceneView.opaqueFlats) ||
			SurfaceContainerUsesHardwareCanvasTexture(sceneView.opaqueSprites);
	}

	static bool IsAuthoredTextureCurrentlyUnresolved(FTextureID textureId)
	{
		if (!textureId.isValid())
		{
			return false;
		}

		FGameTexture* texture = TexMan.GetGameTexture(textureId, true);
		return texture == nullptr || !texture->isValid();
	}

	static bool ChunkHasUnresolvedAuthoredTextures(const nri_scene::PTMapChunk& chunk)
	{
		if (chunk.kind != nri_scene::PTMapChunkKind::Sector ||
			chunk.sectorIndex < 0 ||
			(unsigned)chunk.sectorIndex >= sector.Size())
		{
			return false;
		}

		const sectortype& sec = sector[(unsigned)chunk.sectorIndex];
		if (IsAuthoredTextureCurrentlyUnresolved(sec.floortexture) ||
			IsAuthoredTextureCurrentlyUnresolved(sec.ceilingtexture))
		{
			return true;
		}

		for (const walltype& wal : sec.walls)
		{
			if (IsAuthoredTextureCurrentlyUnresolved(wal.walltexture) ||
				IsAuthoredTextureCurrentlyUnresolved(wal.overtexture))
			{
				return true;
			}

			if (wal.nextwall >= 0 && (unsigned)wal.nextwall < wall.Size())
			{
				const walltype& nextWall = wall[(unsigned)wal.nextwall];
				if (IsAuthoredTextureCurrentlyUnresolved(nextWall.walltexture) ||
					IsAuthoredTextureCurrentlyUnresolved(nextWall.overtexture))
				{
					return true;
				}
			}
		}

		return false;
	}

	static void RemapToPTSpace(const float* src, float* dst)
	{
		dst[0] = src[0];
		dst[1] = src[2];
		dst[2] = src[1];
	}

	static uint32_t GetBootstrapMode()
	{
		return (uint32_t)std::max(0, std::min((int)nri_ptbootstrapmode, 13));
	}

	static bool IntersectProbeTriangle(const nri_scene::SceneVertex& v0, const nri_scene::SceneVertex& v1, const nri_scene::SceneVertex& v2, const float origin[3], const float direction[3], float& outT)
	{
		outT = 0.0f;
		const float edge1[3] = {
			v1.position[0] - v0.position[0],
			v1.position[1] - v0.position[1],
			v1.position[2] - v0.position[2]
		};
		const float edge2[3] = {
			v2.position[0] - v0.position[0],
			v2.position[1] - v0.position[1],
			v2.position[2] - v0.position[2]
		};
		const float p[3] = {
			direction[1] * edge2[2] - direction[2] * edge2[1],
			direction[2] * edge2[0] - direction[0] * edge2[2],
			direction[0] * edge2[1] - direction[1] * edge2[0]
		};
		const float det = edge1[0] * p[0] + edge1[1] * p[1] + edge1[2] * p[2];
		if (fabsf(det) < 1e-5f)
		{
			return false;
		}

		const float invDet = 1.0f / det;
		const float t[3] = {
			origin[0] - v0.position[0],
			origin[1] - v0.position[1],
			origin[2] - v0.position[2]
		};
		const float u = (t[0] * p[0] + t[1] * p[1] + t[2] * p[2]) * invDet;
		if (u < 0.0f || u > 1.0f)
		{
			return false;
		}

		const float q[3] = {
			t[1] * edge1[2] - t[2] * edge1[1],
			t[2] * edge1[0] - t[0] * edge1[2],
			t[0] * edge1[1] - t[1] * edge1[0]
		};
		const float v = (direction[0] * q[0] + direction[1] * q[1] + direction[2] * q[2]) * invDet;
		if (v < 0.0f || (u + v) > 1.0f)
		{
			return false;
		}

		const float hitT = (edge2[0] * q[0] + edge2[1] * q[1] + edge2[2] * q[2]) * invDet;
		if (hitT <= 0.001f)
		{
			return false;
		}

		outT = hitT;
		return true;
	}

	static const char* GetSceneLightRecordSourceName(SceneLightRecordSource source)
	{
		switch (source)
		{
		case SceneLightRecordSource::CapturedScene: return "captured_scene";
		case SceneLightRecordSource::StaticMapScene: return "static_map_scene";
		case SceneLightRecordSource::RuntimeMutationScene: return "runtime_mutation_scene";
		case SceneLightRecordSource::DynamicScene: return "dynamic_scene";
		case SceneLightRecordSource::PersistentVoxelScene: return "persistent_voxel_scene";
		default: return "none";
	}
}

}

NRIRenderer::NRIRenderer(NRIRenderDevice* frameBuffer)
	: mFrameBuffer(frameBuffer)
{
}

NRIRenderer::~NRIRenderer()
{
	Shutdown();
}

bool NRIRenderer::Initialize()
{
	Clocker clock(NriPTInitialize);

	if (mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return false;
	}

	if (!mSceneTextures.LimitLogPrinted())
	{
		LogSceneTextureDescriptorLimits(mFrameBuffer->mCore.GetDeviceDesc(*mFrameBuffer->mDevice));
		mSceneTextures.LimitLogPrinted() = true;
	}

	if (!CheckPathTracingSupport())
	{
		return true;
	}

	if (mPipelineLayout != nullptr)
	{
		return true;
	}

	return
		NRIPipelineStateManager::CreatePipelineLayout(*this) &&
		NRIPipelineStateManager::CreateTaaPipelineLayout(*this) &&
		NRIPipelineStateManager::CreatePresentPipelineLayout(*this) &&
		NRIPipelineStateManager::CreateExposurePipelineLayout(*this) &&
		NRIDescriptorSetManager::AllocateDescriptorSets(*this) &&
		NRIDescriptorSetManager::UpdateSamplerSet(*this) &&
		NRIPipelineStateManager::CreatePipelines(*this);
}

void NRIRenderer::Shutdown()
{
	ResetMuzzleFlashOverlayState("renderer-shutdown");
	mLastResolvedLightOverlayGeneration = 0;

	if (mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return;
	}

	mNrd.Shutdown();
	mUpscaler.Shutdown(*mFrameBuffer);
	DestroyAccelerationStructures();
	ClearRuntimePointLights();
	DestroySceneBuffers();
	NRIFrameResources::DestroyFrameTextures(*this);
	mFrameBuffer->DestroyTextureResource(mSceneTextures.PaletteTexture());
	DestroyCachedTextures();
	mFrameGenerationFrameId = 0;
	mHasFrameGenerationRealFrameTime = false;
	mHasPendingFrameGenerationRealFrameTime = false;
	mHasFrameGenerationTimestamp = false;
	mHasFrameGenerationConfigState = false;
	mLastFrameGenerationRealFrameTimeMs = 0.0f;
	mPendingFrameGenerationRealFrameTimeMs = 0.0f;
	mLastFrameGenerationTimestamp = {};
	mPendingFrameGenerationTimestamp = {};
	mSceneTextures.LimitLogPrinted() = false;
	mLastFrameGenerationRequestedEnabled = false;
	mLastFrameGenerationRequestedProvider = NRIFrameGenerationProvider::Off;
	mLastFrameGenerationResolvedUiMode = NRIFrameGenerationUiMode::Auto;

	for (nri::Pipeline*& pipeline : mPipelines)
	{
		if (pipeline != nullptr)
		{
			mFrameBuffer->mCore.DestroyPipeline(pipeline);
			pipeline = nullptr;
		}
	}

	if (mPipelineLayout != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipelineLayout(mPipelineLayout);
		mPipelineLayout = nullptr;
	}
	if (mTaaPipelineLayout != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipelineLayout(mTaaPipelineLayout);
		mTaaPipelineLayout = nullptr;
	}
	if (mPresentPipelineLayout != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipelineLayout(mPresentPipelineLayout);
		mPresentPipelineLayout = nullptr;
	}
	if (mExposurePipelineLayout != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipelineLayout(mExposurePipelineLayout);
		mExposurePipelineLayout = nullptr;
	}

	mSamplerSet = nullptr;
	mSceneTextureSets.clear();
	mSceneDataSets.clear();
	mFrameTextureSet = nullptr;
	mOutputSet = nullptr;
	mCompositionFrameTextureSet = nullptr;
	mCompositionOutputSet = nullptr;
	mUpscalerPrepassFrameTextureSet = nullptr;
	mUpscalerPrepassOutputSet = nullptr;
	mTaaFrameTextureSet = nullptr;
	mTaaOutputSet = nullptr;
	mRawPresentFrameTextureSet = nullptr;
	mRawPresentOutputSet = nullptr;
	mFinalPresentFrameTextureSet = nullptr;
	mFinalPresentOutputSet = nullptr;
	mExposureInputSets = {};
	mExposureOutputSets = {};
	mAutoExposureInputSourceSlot = FrameTextureSlot::Count;
	mSceneDataDescriptorsInitialized.clear();
}

void NRIRenderer::OnLevelUnloadBegin(const LevelTransitionInfo& info)
{
	WaitForCommandsTracked("level-unload");
	RequestHistoryReset("level-unload", true, true);

	DestroyStaticMapSceneCache("level-unload");
	mStaticMapScene = {};
	mStaticAccelerationBuildSerial = 0;
	mSkyEnvironment.PreservedStaticMapSky() = {};

	mMapWorld = {};
	mObservedMapWorldBuildSerial = 0;

	DestroyCachedTextures();
	ResetPersistentDynamicEmissiveCache();
	ResetMuzzleFlashOverlayState("level-unload");
	mLastResolvedLightOverlayGeneration = 0;

	ClearRuntimePointLights();
	ClearRuntimeDebugSpheres();
	mSceneLights.ResetLevelState();

	mPendingStaticMapLightingInvalidation = false;
	mAllowStartupMapWorldCorrection = false;
	mAllowStartupMutationRebaseline = false;
	mPendingStartupMutationRebaseline = false;
	mPendingStartupVisibleChunkValidation.clear();
	mRuntimeMutation.ResetLevelLifecycleState();
	mStartupMapWorldCorrectionDeadlineFrame = 0;
	mStartupMutationRebaselineDeadlineFrame = 0;

	mCurrentVisibleChunkWords.clear();
	mCurrentVisibleFlatPlaneWords.clear();
	mLastSurfaceProbe = {};
	mLastLoggedSurfaceProbe = {};
	mSurfaceProbeFrame = {};
	mDynamicSceneLastFrame = {};
	mRuntimeSpaceLinkLastFrame = {};
	mLastRuntimeLinkTraceState = {};
	mHasRuntimeLinkTraceState = false;
	mRuntimeChunkTranslationHistory.clear();
	mLastStats = {};
	mHasLoggedStats = false;

	mSceneTextures.CacheStats() = {};
	mSceneLights.ResetPersistentDynamicEmissiveHighWaterStats();

	mUsedStaticMapSceneLastFrame = false;
	mUsedDynamicSceneLastFrame = false;
	mHasVisibleMirrorPortalLastFrame = false;
	mGpuSceneHasDynamicOverlay = false;
	mUploadedStaticMapSceneLastFrame = false;
	mBuiltStaticMapSceneASLastFrame = false;
	mBuiltDynamicSceneASLastFrame = false;

	mActiveTlasInstanceCount = 0;
	mBoundStaticPrimitiveCount = 0;
	mBoundDynamicPrimitiveCount = 0;
	mBoundStaticMaterialCount = 0;
	mBoundDynamicMaterialCount = 0;
	mBoundPortalCount = 0;
	mBoundRuntimeLightCount = 0;
	mBoundRuntimeLightTileCountX = 0;
	mBoundRuntimeLightTileCountY = 0;
	mBoundRuntimeLightTileSize = 0;
	mBoundRuntimeLightTileIndexCount = 0;
	mBoundRuntimeLightMaxTileOccupancy = 0;
	mRuntimeLightPayloadCacheValid = false;
	mRuntimeLightPayloadHash = 0;
	mRuntimeLightClusterCacheValid = false;
	mRuntimeLightClusterPayloadHash = 0;
	mRuntimeLightClusterCameraHash = 0;
	mRuntimeLightSceneDataDirty = false;
	mBoundEmissivePrimitiveCount = 0;
	mBoundEmissiveDominantPrimitive = UINT32_MAX;
	mBoundEmissiveDominantTile = 0;
	mBoundEmissiveDominantFlags = 0;
	mBoundEmissiveDominantDataSource = 0;
	mEmissiveSamplingPayloadCacheValid = false;
	mEmissiveSamplingPayloadHash = 0;
	mEmissiveSectorResponsePayloadCacheValid = false;
	mEmissiveSectorResponsePayloadHash = 0;
	mSceneLights.ResetEmissiveSectorResponseCaches();
	mEmissiveTlasInstanceCount = 0;
	mEmissiveTlasStaticInstanceCount = 0;
	mEmissiveTlasDynamicInstanceCount = 0;
	mEmissiveTlasBuildCount = 0;
	mEmissiveTlasInstancePayloadCacheValid = false;
	mEmissiveTlasInstancePayloadHash = 0;
	mBoundEmissiveTotalPower = 0.0f;
	mBoundEmissiveDominantPower = 0.0f;
	mBoundEmissivePrimitiveRecords.clear();
	mSectorLightingPayloadCacheValid = false;
	mSectorLightingPayloadHash = 0;
	mBoundSectorLightSectorCount = 0;
	mBoundSectorLightActiveCount = 0;
	mBoundSectorLightPulsingCount = 0;
	mBoundSectorLightDominantSector = UINT32_MAX;
	mBoundSectorLightDominantContribution = 0.0f;

	if (info.newLevel == nullptr)
	{
		mSceneLights.ResetRuntimePointLights();
		mDebugOverlays.ResetRuntimeDebugSphereIds();
	}
}

void NRIRenderer::OnLevelUnloadComplete(const LevelTransitionInfo& info)
{
	assert(!mMapWorld.valid);
	assert(!mStaticMapScene.valid);
	assert(!mStaticMapScene.texturesResident);
	assert(!mStaticMapScene.buffersResident);
	assert(!mStaticMapScene.accelerationResident);
	assert(mStaticMapScene.chunks.empty());
	assert(!mStaticMapChunkAtlas.valid);
	assert(mStaticMapChunkAtlas.chunks.empty());
	assert(!mStaticSceneResidency.Registry().valid);
	assert(mStaticSceneResidency.Registry().entries.empty());
	assert(mRuntimeMutation.IsCacheEmpty());
	assert(!mPendingStaticMapLightingInvalidation);
	assert(!mLastSurfaceProbe.valid);
	assert(!mLastLoggedSurfaceProbe.valid);
	assert(mDebugOverlays.Empty());
	assert(mSceneLights.GetManualAnalyticLightCount() == 0);

	if (info.newLevel == nullptr)
	{
		mCurrentVisibleChunkWords.clear();
		mCurrentVisibleFlatPlaneWords.clear();
	}
}

void NRIRenderer::OnLevelLoadBegin(const LevelTransitionInfo& info)
{
	mMapWorld = {};
	mObservedMapWorldBuildSerial = 0;
	mAllowStartupMapWorldCorrection = false;
	mAllowStartupMutationRebaseline = false;
	mPendingStartupMutationRebaseline = false;
	mPendingStartupVisibleChunkValidation.clear();
	mRuntimeMutation.ResetLevelLifecycleState();
	mStartupMapWorldCorrectionDeadlineFrame = 0;
	mStartupMutationRebaselineDeadlineFrame = 0;
	mLastSurfaceProbe = {};
	mLastLoggedSurfaceProbe = {};
	mSurfaceProbeFrame = {};
	mDynamicSceneLastFrame = {};
	mRuntimeSpaceLinkLastFrame = {};
	mRuntimeChunkTranslationHistory.clear();
	mSceneTextures.CacheStats() = {};
	mSceneLights.ResetPersistentDynamicEmissiveHighWaterStats();
	mLastStats = {};
	mHasLoggedStats = false;
	mLastRuntimeLinkTraceState = {};
	mHasRuntimeLinkTraceState = false;
	mSceneLights.ResetRuntimePointLights();
	mDebugOverlays.ResetRuntimeDebugSphereIds();

	if (info.newLevel == nullptr)
	{
		mCurrentVisibleChunkWords.clear();
		mCurrentVisibleFlatPlaneWords.clear();
	}
}

NRIRenderer::LevelTransitionSnapshot NRIRenderer::BuildLevelTransitionSnapshot() const
{
	LevelTransitionSnapshot snapshot = {};
	snapshot.mapWorldValid = mMapWorld.valid;
	snapshot.mapWorldBuildSerial = mMapWorld.buildSerial;
	snapshot.mapWorldChunkCount = (uint32_t)mMapWorld.chunks.size();
	snapshot.mapWorldSurfaceCount = (uint32_t)mMapWorld.surfaces.size();
	snapshot.staticSceneValid = mStaticMapScene.valid;
	snapshot.staticSceneTexturesResident = mStaticMapScene.texturesResident;
	snapshot.staticSceneBuffersResident = mStaticMapScene.buffersResident;
	snapshot.staticSceneAccelerationResident = mStaticMapScene.accelerationResident;
	snapshot.staticSceneBuildSerial = mStaticMapScene.buildSerial;
	snapshot.staticSceneChunkCount = (uint32_t)mStaticMapScene.chunks.size();
	snapshot.staticSceneMaterialCount = (uint32_t)mStaticMapScene.gpuMaterials.size();
	snapshot.textureCacheCount = mSceneTextures.CacheCount();
	snapshot.skyTextureCacheCount = (uint32_t)mSkyEnvironment.CachedTextures().size();
	const RuntimeMutationCacheStats runtimeMutationCacheStats = mRuntimeMutation.GatherCacheStats();
	snapshot.runtimeMutationChunkCount = mRuntimeMutation.GetCacheChunkCount();
	snapshot.runtimeMutationActiveChunkCount = runtimeMutationCacheStats.activeChunkCount;
	snapshot.runtimeMutationValidChunkCount = runtimeMutationCacheStats.validChunkCount;
	snapshot.residentChunkRegistryValid = mStaticSceneResidency.Registry().valid;
	snapshot.residentChunkRegistryEntryCount = (uint32_t)mStaticSceneResidency.Registry().entries.size();
	snapshot.residentChunkRegistryChunkCount = mStaticSceneResidency.Registry().chunkCount;
	snapshot.residentChunkRegistryActiveChunkCount = mStaticSceneResidency.Registry().activeChunkCount;
	snapshot.residentChunkRegistryMappedChunkCount = mStaticSceneResidency.Registry().mappedChunkCount;
	snapshot.residentChunkRegistryAccelerationResidentChunkCount = mStaticSceneResidency.Registry().accelerationResidentChunkCount;
	snapshot.pendingStaticMapLightingInvalidation = mPendingStaticMapLightingInvalidation;
	snapshot.surfaceProbeValid = mLastSurfaceProbe.valid;
	snapshot.surfaceProbeHit = mLastSurfaceProbe.hit;
	snapshot.surfaceProbeWallIndex = mLastSurfaceProbe.provenance.wallIndex;
	snapshot.surfaceProbeMapChunkIndex = mLastSurfaceProbe.provenance.mapChunkIndex;
	snapshot.transientMuzzleFlashSlotCount = mSceneLights.GetAnalyticLights().transientMuzzleSlotCount;
	snapshot.transientMuzzleFlashActiveCount = mSceneLights.GetAnalyticLights().transientMuzzleActiveCount;
	snapshot.analyticLightCount = (uint32_t)mSceneLights.GetAnalyticLights().activeLights.size();
	snapshot.manualLightCount = mSceneLights.GetManualAnalyticLightCount();
	snapshot.emissiveSurfaceCount = (uint32_t)mSceneLights.GetEmissiveSurfaces().activeSurfaces.size();
	snapshot.activeSectorLightCount = mSceneLights.GetSectorLighting().activeSectorCount;
	snapshot.runtimeDebugSphereCount = mDebugOverlays.GetRuntimeDebugSphereCount();
	snapshot.runtimeTestLightCount = mSceneLights.GetManualAnalyticLightCount();
	return snapshot;
}

void NRIRenderer::ResetMuzzleFlashOverlayState(const char* reason)
{
	uint32_t discardedEventCount = 0;
	if (mFrameBuffer != nullptr)
	{
		TArray<PathTracingWeaponLightEvent> discardedEvents;
		mFrameBuffer->ConsumePathTracingWeaponLightEvents(discardedEvents);
		discardedEventCount = (uint32_t)discardedEvents.Size();
	}

	mSceneLights.ResetMuzzleFlashOverlayState(reason, discardedEventCount, nri_ptdebug > 0);
}

void NRIRenderer::ResetPerfTraceStats()
{
	mLastPerfShellTraceStats = {};
	mLastPerfResourceTraceStats = {};
}

void NRIRenderer::NotePerfBufferUpload(const SceneBufferDebugStats* stats, uint64_t size, bool growth, const char* reason, int uploadKind)
{
	if (!ShouldTracePtPerf() || stats == nullptr)
	{
		return;
	}

	auto& perf = mLastPerfResourceTraceStats;
	if (growth)
	{
		perf.growEvents++;
	}
	else
	{
		perf.overwriteEvents++;
	}

	auto noteBytes = [&](uint32_t& callCount, uint64_t& byteCount)
	{
		callCount++;
		byteCount += size;
	};

	if (stats == &mVertexBufferStats || stats == &mIndexBufferStats || stats == &mPrimitiveBufferStats || stats == &mMaterialBufferStats)
	{
		noteBytes(perf.sceneUploadCalls, perf.sceneUploadBytes);
		int effectiveUploadKind = uploadKind;
		if (effectiveUploadKind < 0)
		{
			if (stats == &mVertexBufferStats)
			{
				effectiveUploadKind = ResidentUploadKind_Vertex;
			}
			else if (stats == &mIndexBufferStats)
			{
				effectiveUploadKind = ResidentUploadKind_Index;
			}
			else if (stats == &mPrimitiveBufferStats)
			{
				effectiveUploadKind = ResidentUploadKind_Primitive;
			}
			else if (stats == &mMaterialBufferStats)
			{
				effectiveUploadKind = ResidentUploadKind_Material;
			}
		}
		switch (effectiveUploadKind)
		{
		case ResidentUploadKind_Vertex: perf.sceneVertexUploadBytes += size; break;
		case ResidentUploadKind_Index: perf.sceneIndexUploadBytes += size; break;
		case ResidentUploadKind_Primitive: perf.scenePrimitiveUploadBytes += size; break;
		case ResidentUploadKind_Material: perf.sceneMaterialUploadBytes += size; break;
		default: break;
		}

		if (reason != nullptr && std::strcmp(reason, "scene_buffer_upload") == 0)
		{
			noteBytes(perf.sceneDynamicUploadCalls, perf.sceneDynamicUploadBytes);
		}
		else if (reason != nullptr && std::strcmp(reason, "resident_chunk_write") == 0)
		{
			noteBytes(perf.sceneResidentChunkUploadCalls, perf.sceneResidentChunkUploadBytes);
		}
		else if (reason != nullptr && std::strcmp(reason, "persistent_voxel_scene_upload") == 0)
		{
			noteBytes(perf.scenePersistentVoxelUploadCalls, perf.scenePersistentVoxelUploadBytes);
		}
		else if (reason != nullptr &&
			(std::strcmp(reason, "persistent_voxel_mesh_vertex") == 0 ||
				std::strcmp(reason, "persistent_voxel_mesh_index") == 0 ||
				std::strcmp(reason, "persistent_voxel_mesh_primitive") == 0 ||
				std::strcmp(reason, "persistent_voxel_material_variant") == 0))
		{
			noteBytes(perf.scenePersistentVoxelVariantUploadCalls, perf.scenePersistentVoxelVariantUploadBytes);
		}
		else if (reason == nullptr)
		{
			noteBytes(perf.sceneStaticRefreshUploadCalls, perf.sceneStaticRefreshUploadBytes);
		}
		else
		{
			noteBytes(perf.sceneOtherUploadCalls, perf.sceneOtherUploadBytes);
		}
	}
	else if (stats == &mEmissivePrimitiveHeaderBufferStats || stats == &mEmissivePrimitiveBufferStats || stats == &mEmissivePrimitiveCdfBufferStats || stats == &mEmissiveMaterialResponseBufferStats || stats == &mEmissiveTlasInstanceBufferStats)
	{
		noteBytes(perf.emissiveUploadCalls, perf.emissiveUploadBytes);
	}
	else
	{
		noteBytes(perf.sceneDataUploadCalls, perf.sceneDataUploadBytes);
	}
}

NRIRendererFrameContext NRIRenderer::BuildFrameContext(int drawmode, bool portal, int debugMode, bool preserveHistory) const
{
	NRIRendererFrameContext context = {};
	context.frameIndex = mFrameIndex;
	context.drawMode = drawmode;
	context.debugMode = debugMode;
	context.portal = portal;
	context.preserveHistory = preserveHistory;
	if (mFrameBuffer != nullptr)
	{
		context.outputWidth = std::max<uint32_t>((uint32_t)mFrameBuffer->mSceneViewport.width, 1u);
		context.outputHeight = std::max<uint32_t>((uint32_t)mFrameBuffer->mSceneViewport.height, 1u);
		if (mFrameBuffer->mActiveTarget != nullptr)
		{
			context.targetWidth = mFrameBuffer->mActiveTarget->width;
			context.targetHeight = mFrameBuffer->mActiveTarget->height;
		}
	}
	return context;
}

NRIPassDispatchContext NRIRenderer::BuildPassDispatchContext()
{
	return BuildNRIPassDispatchContext(*this);
}

bool NRIRenderer::PreloadLevelScene(uint32_t outputWidth, uint32_t outputHeight, uint32_t targetWidth, uint32_t targetHeight)
{
	NRIPreloadLevelSceneInputs inputs = {};
	inputs.outputWidth = outputWidth;
	inputs.outputHeight = outputHeight;
	inputs.targetWidth = targetWidth;
	inputs.targetHeight = targetHeight;
	return NRIPreloadCoordinator::Run(*this, inputs);
}

void NRIRenderer::ResetHistory()
{
	RequestHistoryReset("history-reset", true, true);
}

void NRIRenderer::RequestAutoExposureReset(const char* reason)
{
	const char* safeReason = reason != nullptr && *reason != '\0' ? reason : "unspecified";
	mExposure.RequestReset(safeReason, (uint64_t)mFrameIndex);
	if (nri_ptautoexposurestats)
	{
		const NRIAutoExposureStatus& status = mExposure.GetStatus();
		Printf("NRI PT auto exposure reset: reason=%s frame=%u serial=%llu\n",
			safeReason,
			mFrameIndex,
			(unsigned long long)status.resetSerial);
	}
}

void NRIRenderer::NotifyCameraCut(const char* reason)
{
	RequestHistoryReset((reason != nullptr && *reason != '\0') ? reason : "camera-cut", true, false);
}

void NRIRenderer::SetGuiCaptureState(bool active)
{
	if (mGuiCaptureActive == active)
	{
		return;
	}

	mGuiCaptureActive = active;
	if (nri_ptscenestats)
	{
		const NRIMainUpscalerKind resolvedMain = ResolveMainUpscalerKind(false);
		const nri::UpscalerMode resolvedUpscalerMode = NRIResolveUpscalerModeForMain(resolvedMain, GetSelectedUpscalerMode());
		Printf("NRI PT gui capture: frame=%u active=%s jitter=%s phases=%u\n",
			mFrameIndex,
			mGuiCaptureActive ? "yes" : "no",
			NRIGetTemporalJitterModeName(resolvedMain, mGuiCaptureActive),
			NRIGetTemporalJitterPhaseCount(resolvedMain, resolvedUpscalerMode, mGuiCaptureActive));
	}
}

void NRIRenderer::RequestHistoryReset(const char* reason, bool clearPreviousCameraState, bool clearRuntimeChunkTranslationHistory)
{
	ArmTemporalTraceBudget(reason);
	mResetHistory = true;
	mLastHistoryResetReason = (reason != nullptr && *reason != '\0') ? reason : "unspecified";
	RequestAutoExposureReset(mLastHistoryResetReason.c_str());
	if (clearPreviousCameraState)
	{
		mHasPreviousCameraState = false;
	}
	if (clearRuntimeChunkTranslationHistory)
	{
		mRuntimeChunkTranslationHistory.clear();
	}
}

void NRIRenderer::NoteLightHistoryChange(const char* reason)
{
	ArmTemporalTraceBudget(reason);
	if (ShouldEmitRendererTemporalTraceLogs())
	{
		Printf("NRI PT light change: reason=%s frame=%u reset=no\n",
			(reason != nullptr && *reason != '\0') ? reason : "unspecified",
			mFrameIndex);
	}
}

void NRIRenderer::InvalidateRuntimeLightSceneData()
{
	mBoundRuntimeLightCount = 0;
	mBoundRuntimeLightTileCountX = 0;
	mBoundRuntimeLightTileCountY = 0;
	mBoundRuntimeLightTileSize = 0;
	mBoundRuntimeLightTileIndexCount = 0;
	mBoundRuntimeLightMaxTileOccupancy = 0;
	mRuntimeLightPayloadCacheValid = false;
	mRuntimeLightPayloadHash = 0;
	mRuntimeLightClusterCacheValid = false;
	mRuntimeLightClusterPayloadHash = 0;
	mRuntimeLightClusterCameraHash = 0;
	mRuntimeLightSceneDataDirty = true;
}

void NRIRenderer::PrintRuntimeLightClusterStatus() const
{
	const uint32_t tileCount = mBoundRuntimeLightTileCountX * mBoundRuntimeLightTileCountY;
	const uint32_t centerTileX = mBoundRuntimeLightTileCountX > 0 ? (mBoundRuntimeLightTileCountX - 1) / 2u : 0u;
	const uint32_t centerTileY = mBoundRuntimeLightTileCountY > 0 ? (mBoundRuntimeLightTileCountY - 1) / 2u : 0u;
	uint32_t centerTileCount = 0;
	if (mRuntimeLightTileHeaderBuffer.buffer != nullptr &&
		mBoundRuntimeLightTileCountX > 0 &&
		mBoundRuntimeLightTileCountY > 0)
	{
		void* mapped = mFrameBuffer->mCore.MapBuffer(*mRuntimeLightTileHeaderBuffer.buffer, 0, mRuntimeLightTileHeaderBuffer.usedSize);
		if (mapped != nullptr)
		{
			const auto* headers = reinterpret_cast<const RuntimeLightTileHeaderGpuData*>(mapped);
			const uint32_t centerIndex = centerTileY * mBoundRuntimeLightTileCountX + centerTileX;
			if ((uint64_t)(centerIndex + 1) * sizeof(RuntimeLightTileHeaderGpuData) <= mRuntimeLightTileHeaderBuffer.usedSize)
			{
				centerTileCount = headers[centerIndex].indexCount;
			}
			mFrameBuffer->mCore.UnmapBuffer(*mRuntimeLightTileHeaderBuffer.buffer);
		}
	}

	Printf("NRI PT light clusters: tile_size=%u grid=%ux%u tiles=%u active_lights=%u used_indices=%u max_occupancy=%u center_tile=(%u,%u) center_count=%u debug_mode=%u\n",
		mBoundRuntimeLightTileSize,
		mBoundRuntimeLightTileCountX,
		mBoundRuntimeLightTileCountY,
		tileCount,
		mBoundRuntimeLightCount,
		mBoundRuntimeLightTileIndexCount,
		mBoundRuntimeLightMaxTileOccupancy,
		centerTileX,
		centerTileY,
		centerTileCount,
		nri_diag::PtDebugAnalyticDirect);
}

void NRIRenderer::UpdateNightVisionState()
{
	mNightVisionState = {};

	if (gi == nullptr)
	{
		return;
	}

	RuntimeNightVisionState runtimeState = {};
	if (!gi->GetNightVisionState(&runtimeState) || !runtimeState.available)
	{
		return;
	}

	switch (runtimeState.mode)
	{
	case RuntimeNightVisionMode::Duke:
		mNightVisionState.mode = NRIPTNightVisionMode::Duke;
		break;
	default:
		mNightVisionState.mode = NRIPTNightVisionMode::None;
		break;
	}

	mNightVisionState.viewEligible = runtimeState.viewEligible;
	mNightVisionState.enabled = runtimeState.enabled;
	mNightVisionState.strength01 = runtimeState.strength01;
	mNightVisionState.remainingSeconds = runtimeState.remainingSeconds;
}

void NRIRenderer::PrintSectorLightDump(float radius, uint32_t limit) const
{
	mSceneLights.PrintSectorLightDump(mCurrentCameraPos, NRIGetSectorLightMultiplier(), radius, limit);
}

NRIRenderer::MemoryTelemetry NRIRenderer::GetMemoryTelemetry() const
{
	MemoryTelemetry telemetry = {};
	telemetry.renderWidth = mRenderWidth;
	telemetry.renderHeight = mRenderHeight;
	telemetry.outputWidth = mOutputWidth;
	telemetry.outputHeight = mOutputHeight;

	const auto accumulateTexture = [](const NRITextureResource& resource, uint64_t& total)
	{
		total += resource.memorySize;
	};
	const auto accumulateBuffer = [](const NRIBufferResource& resource, uint64_t& total)
	{
		total += resource.memorySize;
	};
	const auto accumulateAs = [](const NRIAccelerationStructureResource& resource, uint64_t& total)
	{
		total += resource.memorySize;
	};

	for (const NRITextureResource& texture : mFrameTextures)
	{
		accumulateTexture(texture, telemetry.frameTextureBytes);
	}

	accumulateTexture(mSceneTextures.PaletteTexture(), telemetry.sceneTextureBytes);
	for (const NRISceneCachedTexture& texture : mSceneTextures.CachedTextures())
	{
		accumulateTexture(texture.resource, telemetry.sceneTextureBytes);
	}

	for (const NRICachedSkyTexture& texture : mSkyEnvironment.CachedTextures())
	{
		accumulateTexture(texture.resource, telemetry.skyTextureBytes);
	}

	accumulateBuffer(mVertexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mIndexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mPrimitiveBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mMaterialBuffer, telemetry.sceneBufferBytes);
	for (const SceneUploadBufferRingSlot& slot : mSceneUploadBufferRing)
	{
		accumulateBuffer(slot.vertexBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.indexBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.primitiveBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.materialBuffer, telemetry.sceneBufferBytes);
		accumulateAs(slot.dynamicBottomLevelAS, telemetry.accelerationStructureBytes);
	}
	accumulateBuffer(mStaticVertexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mStaticIndexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mStaticPrimitiveBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mStaticMaterialBuffer, telemetry.sceneBufferBytes);
	const NRIPersistentVoxelMemoryUsage persistentVoxelMemory = mPersistentVoxels.GetMemoryUsage();
	telemetry.sceneBufferBytes += persistentVoxelMemory.sceneBufferBytes;
	telemetry.accelerationStructureBytes += persistentVoxelMemory.accelerationStructureBytes;
	accumulateBuffer(mTlasInstanceBuffer, telemetry.sceneBufferBytes);
	for (const NRIBufferResource& tlasInstanceBuffer : mTlasInstanceBufferRing)
	{
		accumulateBuffer(tlasInstanceBuffer, telemetry.sceneBufferBytes);
	}
	accumulateBuffer(mSceneInstanceBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mPortalBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mRuntimeLightBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mRuntimeLightTileHeaderBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mRuntimeLightTileIndexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissivePrimitiveHeaderBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissivePrimitiveBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissivePrimitiveCdfBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissiveMaterialResponseBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissiveTlasInstanceBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mSectorLightHeaderBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mSectorLightBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mReprojectionBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mVisibleChunkBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mVisibleFlatPlaneBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mScratchBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mResidentStaticBlasScratchBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mTopLevelScratchBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissiveTopLevelScratchBuffer, telemetry.sceneBufferBytes);

	accumulateAs(mTopLevelAS, telemetry.accelerationStructureBytes);
	accumulateAs(mEmissiveTopLevelAS, telemetry.accelerationStructureBytes);
	for (const auto& chunk : mStaticMapScene.chunks)
	{
		accumulateAs(chunk.accelerationStructure, telemetry.accelerationStructureBytes);
	}

	telemetry.totalTrackedBytes =
		telemetry.frameTextureBytes +
		telemetry.sceneTextureBytes +
		telemetry.skyTextureBytes +
		telemetry.sceneBufferBytes +
		telemetry.accelerationStructureBytes;
	return telemetry;
}

void NRIRenderer::PrintSwapChainRenderConfig() const
{
	NRISyncLegacyUpscalerConfig(false);
	const NRIMainUpscalerKind requestedMain = GetSelectedMainUpscalerKind();
	const NRIMainUpscalerKind resolvedMain = GetResolvedMainUpscalerKindForStatus();
	const NRIPostSharpenKind requestedPost = GetSelectedPostSharpenKind();
	const NRIPostSharpenKind resolvedPost = GetResolvedPostSharpenKindForStatus();
	const nri::UpscalerMode requestedUpscalerMode = GetSelectedUpscalerMode();
	const nri::UpscalerMode resolvedUpscalerMode = NRIResolveUpscalerModeForMain(resolvedMain, requestedUpscalerMode);
	const bool runAppTaa = NRIShouldRunAppTaa(resolvedMain);
	const float requestedRenderScale = std::max(0.33f, std::min((float)nri_renderscale, 1.0f));
	const float resolvedRenderScale = NRIResolveRenderScaleForMain(resolvedMain, requestedUpscalerMode, requestedRenderScale);
	const bool beautyDenoiseActive = !!nri_denoise && resolvedMain != NRIMainUpscalerKind::DLRR;
	NRIPTOutputPolicy outputPolicy = {};
	if (mFrameBuffer != nullptr)
	{
		outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	}
	const bool nisSupported =
		mFrameBuffer != nullptr &&
		mFrameBuffer->mDevice != nullptr &&
		mFrameBuffer->mUpscaler.IsUpscalerSupported(*mFrameBuffer->mDevice, nri::UpscalerType::NIS);
	const bool dlsrSupported = IsMainUpscalerSupported(NRIMainUpscalerKind::DLSR);
	const bool dlrrSupported = IsMainUpscalerSupported(NRIMainUpscalerKind::DLRR);

	Printf("NRI swapchain render config: main_upscaler=%s->%s mode=%s->%s post_sharpen=%s->%s support=NIS:%s DLSS-SR:%s DLRR:%s app_taa=requested:%s active:%s denoise=requested:%s beauty_active:%s nrd=%s render_scale=%.3f->%.3f jitter=%s phases=%u output=%s->%s hdr_swapchain=%s display_hdr=%s tonemap=%s sharpness=%.3f\n",
		NRIGetMainUpscalerName(requestedMain),
		NRIGetMainUpscalerName(resolvedMain),
		NRIGetUpscalerModeName(requestedUpscalerMode),
		NRIGetUpscalerModeName(resolvedUpscalerMode),
		NRIGetPostSharpenName(requestedPost),
		NRIGetPostSharpenName(resolvedPost),
		nisSupported ? "yes" : "no",
		dlsrSupported ? "yes" : "no",
		dlrrSupported ? "yes" : "no",
		nri_pttaa ? "on" : "off",
		runAppTaa ? "on" : "off",
		nri_denoise ? "on" : "off",
		beautyDenoiseActive ? "on" : "off",
		GetNrdDenoiserModeName(GetSelectedNrdDenoiserMode()),
		requestedRenderScale,
		resolvedRenderScale,
		NRIGetTemporalJitterModeName(resolvedMain, mGuiCaptureActive),
		NRIGetTemporalJitterPhaseCount(resolvedMain, resolvedUpscalerMode, mGuiCaptureActive),
		GetNRIPTOutputModeName(outputPolicy.requestedMode),
		GetNRIPTOutputModeName(outputPolicy.resolvedMode),
		outputPolicy.hdrSwapChainActive ? "yes" : "no",
		!outputPolicy.displayInfoAvailable ? "unknown" : (outputPolicy.displayHdrSupported ? "yes" : "no"),
		GetNRIPTTonemapModeName(outputPolicy.tonemapMode),
		(float)nri_sharpness);
}

const char* NRIRenderer::GetExposureDomainName(ExposureDomain domain) const
{
	switch (domain)
	{
	case ExposureDomain::SceneHDR: return "scene_hdr";
	case ExposureDomain::PreExposedHDR: return "pre_exposed_hdr";
	case ExposureDomain::DisplayMappedOutput: return "display_mapped_output";
	default: return "unknown";
	}
}

NRIRenderer::ExposureDomain NRIRenderer::ResolveFrameTextureExposureDomain(FrameTextureSlot slot, NRIMainUpscalerKind mainKind, NRIPostSharpenKind postSharpenKind) const
{
	switch (slot)
	{
	case FrameTextureSlot::Composed:
	case FrameTextureSlot::TraceTransparentOutput:
	case FrameTextureSlot::SrInput:
	case FrameTextureSlot::RrInput:
		return ExposureDomain::SceneHDR;
	case FrameTextureSlot::TaaHistoryPing:
	case FrameTextureSlot::TaaHistoryPong:
		return NRIShouldRunAppTaa(mainKind) ? ExposureDomain::PreExposedHDR : ExposureDomain::SceneHDR;
	case FrameTextureSlot::VendorOutput:
		return ExposureDomain::SceneHDR;
	case FrameTextureSlot::PostSharpenOutput:
		if (postSharpenKind == NRIPostSharpenKind::Off)
		{
			return ExposureDomain::SceneHDR;
		}
		if (mainKind != NRIMainUpscalerKind::Off)
		{
			return ExposureDomain::SceneHDR;
		}
		return NRIShouldRunAppTaa(mainKind) ? ExposureDomain::PreExposedHDR : ExposureDomain::SceneHDR;
	case FrameTextureSlot::Final:
		return ExposureDomain::DisplayMappedOutput;
	default:
		return ExposureDomain::SceneHDR;
	}
}

NRIRenderer::ExposureRoute NRIRenderer::ResolveExposureRoute(FrameTextureSlot inputSlot, const NRIPTOutputPolicy& outputPolicy, NRIMainUpscalerKind mainKind, NRIPostSharpenKind postSharpenKind) const
{
	ExposureRoute route = {};
	route.inputDomain = ResolveFrameTextureExposureDomain(inputSlot, mainKind, postSharpenKind);
	route.temporalExposure = GetTemporalExposure(outputPolicy);
	route.presentExposure =
		route.inputDomain == ExposureDomain::PreExposedHDR ?
		1.0f :
		outputPolicy.exposure;
	return route;
}

void NRIRenderer::EmitSelfTestSummary(uint32_t traceFrameIndex, int drawmode, bool portal) const
{
	if (!nri_ptselftest)
	{
		return;
	}

	const PerfShellTraceStats& shell = mLastPerfShellTraceStats;
	const NRIPTOutputPolicy outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	const NRIAutoExposureSettings& exposureSettings = mExposure.GetSettings();
	const NRIAutoExposureStatus& exposureStatus = mExposure.GetStatus();
	const NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	const bool finalTextureValid = final.texture != nullptr && final.shaderView != nullptr;
	const bool worldActive = gamestate == GS_LEVEL && currentLevel != nullptr;
	const bool gameplayFrame = worldActive && drawmode == DM_MAINVIEW && !portal;
	const uint64_t sceneSignature = nri_scene::HashCombine64(
		nri_scene::HashCombine64(
			nri_scene::HashCombine64(mVertexBuffer.payloadHash, mIndexBuffer.payloadHash),
			mPrimitiveBuffer.payloadHash),
		mSceneInstanceBuffer.payloadHash);
	const uint64_t materialSignature = mMaterialBuffer.payloadHash;
	const uint64_t instanceSignature = mSceneInstanceBuffer.payloadHash;
	const uint64_t skySignature = nri_scene::HashCombine64(mSkyEnvironment.ActiveKey(), (uint64_t)mSkyEnvironment.ActiveState().faceMask);
	const NRIBufferResource& vertexBuffer = mVertexBuffer;
	const NRIBufferResource& indexBuffer = mIndexBuffer;
	const NRIBufferResource& primitiveBuffer = mPrimitiveBuffer;
	const NRIBufferResource& materialBuffer = mMaterialBuffer;
	const uint64_t vertexBytes = vertexBuffer.payloadSize != 0 ? vertexBuffer.payloadSize : vertexBuffer.usedSize;
	const uint64_t indexBytes = indexBuffer.payloadSize != 0 ? indexBuffer.payloadSize : indexBuffer.usedSize;
	const uint64_t primitiveBytes = primitiveBuffer.payloadSize != 0 ? primitiveBuffer.payloadSize : primitiveBuffer.usedSize;
	const uint64_t materialBytes = materialBuffer.payloadSize != 0 ? materialBuffer.payloadSize : materialBuffer.usedSize;

	NRISelfTestSummarySnapshot summary = {};
	summary.traceFrameIndex = traceFrameIndex;
	summary.engineFrameIndex = mFrameIndex;
	summary.mapName = currentLevel != nullptr ? currentLevel->labelName.GetChars() : "none";
	summary.levelName = mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "none";
	summary.graphicsApiName = GetGraphicsApiName(mFrameBuffer->GetLiveAPI());
	summary.worldActive = worldActive;
	summary.menuActive = menuactive != MENU_Off;
	summary.gameplayFrame = gameplayFrame;
	summary.portal = portal;
	summary.drawmode = drawmode;
	summary.route = mDiagnostics.GetSelfTestRouteSnapshot();
	summary.debugMode = (int)GetEffectivePtDebugMode();
	summary.presentKind = summary.route.presenterName;
	summary.renderWidth = mRenderWidth;
	summary.renderHeight = mRenderHeight;
	summary.outputWidth = mOutputWidth;
	summary.outputHeight = mOutputHeight;
	summary.swapchainFormat = (uint32_t)mFrameBuffer->mCreatedSwapChainFormat;
	summary.hdr = outputPolicy.hdrSwapChainActive;
	summary.primitiveCount = shell.activePrimitiveCount;
	summary.materialCount = shell.activeMaterialCount;
	summary.sceneInstanceCount = shell.sceneInstanceCount;
	summary.staticInstanceCount = shell.sceneInstanceStaticCount;
	summary.dynamicInstanceCount = shell.sceneInstanceDynamicCount;
	summary.persistentVoxelInstanceCount = shell.sceneInstancePersistentVoxelCount;
	summary.emissiveInstanceCount = mBoundEmissivePrimitiveCount;
	summary.vertexCount = mVertexBuffer.stride != 0 ? (uint32_t)(vertexBytes / mVertexBuffer.stride) : 0u;
	summary.indexCount = mIndexBuffer.stride != 0 ? (uint32_t)(indexBytes / mIndexBuffer.stride) : 0u;
	summary.vertexBytes = vertexBytes;
	summary.indexBytes = indexBytes;
	summary.primitiveBytes = primitiveBytes;
	summary.materialBytes = materialBytes;
	summary.instanceBytes = mSceneInstanceBuffer.payloadSize != 0 ? mSceneInstanceBuffer.payloadSize : mSceneInstanceBuffer.usedSize;
	summary.sceneSignature = sceneSignature;
	summary.materialSignature = materialSignature;
	summary.instanceSignature = instanceSignature;
	summary.skySignature = skySignature;
	summary.skyMode = GetSkyModeName(mSkyEnvironment.ActiveState().mode);
	summary.skySource = GetSkySourceTypeName(mSkyEnvironment.ActiveState().sourceType);
	summary.skyKey = mSkyEnvironment.ActiveKey();
	summary.skyBrightness = mSkyEnvironment.ActiveState().brightness;
	summary.skyAction = mSkyEnvironment.HasTracedState() ? "traced" : "untraced";
	summary.autoExposure = exposureSettings.enabled;
	summary.exposureTexture = mExposure.HasExposureStateTextures();
	summary.exposure = outputPolicy.exposure;
	summary.targetExposure = exposureStatus.targetExposure;
	summary.adaptedExposure = exposureStatus.adaptedExposure;
	summary.meteredLogLuminance = exposureStatus.meteredLogLuminance;
	summary.exposureStatsValid = exposureStatus.debugValid;
	summary.exposureStatsFrame = exposureStatus.debugFrameIndex;
	summary.finalValid = finalTextureValid;
	summary.exposureReason = exposureSettings.enabled ? "ok" : "disabled";
	mDiagnostics.EmitSelfTestSummary(summary);
}

void NRIRenderer::PrintTemporalStatus() const
{
	NRISyncLegacyUpscalerConfig(false);
	const auto buildTextureSnapshot = [this](FrameTextureSlot slot)
	{
		const NRITextureResource& texture = GetFrameTexture(slot);
		NRITextureStatusSnapshot snapshot = {};
		snapshot.slotName = GetFrameTextureSlotName(slot);
		snapshot.width = texture.width;
		snapshot.height = texture.height;
		snapshot.access = (uint32_t)texture.state.access;
		snapshot.layout = (uint32_t)texture.state.layout;
		snapshot.stages = (uint32_t)texture.state.stages;
		return snapshot;
	};

	const NRIPTOutputPolicy outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	const NRIMainUpscalerKind requestedMain = GetSelectedMainUpscalerKind();
	const NRIMainUpscalerKind resolvedMain = GetResolvedMainUpscalerKindForStatus();
	const NRIPostSharpenKind requestedPost = GetSelectedPostSharpenKind();
	const NRIPostSharpenKind resolvedPost = GetResolvedPostSharpenKindForStatus();
	const bool runAppTaa = NRIShouldRunAppTaa(resolvedMain);
	const float exposure = GetTemporalExposure(outputPolicy);
	const float exposureStops = std::log2(std::max(exposure, 0.125f));
	const FrameTextureSlot presentSlot = mUseUpscaledInFinal ? mUpscaledInputSlot : mHistoryOutputSlot;
	const ExposureRoute exposureRoute = ResolveExposureRoute(presentSlot, outputPolicy, resolvedMain, resolvedPost);
	const NRIAutoExposureSettings& autoExposureSettings = mExposure.GetSettings();
	const NRITextureResource* autoExposureStateTexture = mExposure.GetExposureStateTexture(mFrameIndex & 1u);
	const bool autoExposureTextureValid =
		autoExposureStateTexture != nullptr &&
		autoExposureStateTexture->shaderView != nullptr;
	const bool autoExposureTaaApply =
		runAppTaa &&
		autoExposureSettings.enabled &&
		autoExposureTextureValid;

	NRITemporalStatusSnapshot snapshot = {};
	snapshot.debugMode = (int)nri_ptdebug;
	snapshot.requestedMainUpscaler = NRIGetMainUpscalerName(requestedMain);
	snapshot.resolvedMainUpscaler = NRIGetMainUpscalerName(resolvedMain);
	snapshot.requestedPostSharpen = NRIGetPostSharpenName(requestedPost);
	snapshot.resolvedPostSharpen = NRIGetPostSharpenName(resolvedPost);
	snapshot.taa = !!nri_pttaa;
	snapshot.guiCapture = mGuiCaptureActive;
	snapshot.lastDebugMode = mLastDebugMode;
	snapshot.lastMainUpscaler = NRIGetMainUpscalerName(mLastTemporalHistoryMainUpscaler);
	snapshot.lastPostSharpen = NRIGetPostSharpenName(mLastTemporalPostSharpen);
	snapshot.resetHistory = mResetHistory;
	snapshot.previousCamera = mHasPreviousCameraState;
	snapshot.historyInput = buildTextureSnapshot(mHistoryInputSlot);
	snapshot.historyOutput = buildTextureSnapshot(mHistoryOutputSlot);
	snapshot.presentSlotName = GetFrameTextureSlotName(presentSlot);
	snapshot.upscaledSlotName = GetFrameTextureSlotName(mUpscaledInputSlot);
	snapshot.useUpscaled = mUseUpscaledInFinal;
	snapshot.historyDomain = GetExposureDomainName(ResolveFrameTextureExposureDomain(mHistoryOutputSlot, resolvedMain, resolvedPost));
	snapshot.presentDomain = GetExposureDomainName(exposureRoute.inputDomain);
	snapshot.temporalExposure = exposure;
	snapshot.presentExposure = exposureRoute.presentExposure;
	snapshot.exposureStops = exposureStops;
	snapshot.resetThresholdStops = NRI_TAA_EXPOSURE_RESET_THRESHOLD_STOPS;
	snapshot.autoExposure = autoExposureSettings.enabled;
	snapshot.exposureTexture = autoExposureTextureValid;
	snapshot.taaApply = autoExposureTaaApply;
	PrintNRITemporalStatusSnapshot(snapshot);
}

void NRIRenderer::ArmTemporalTraceBudget(const char* reason)
{
	if (!nri_pttemporaltrace)
	{
		return;
	}

	if ((int)nri_pttraceframes >= NRI_TEMPORAL_TRACE_REARM_FRAME_COUNT)
	{
		return;
	}

	nri_pttraceframes = NRI_TEMPORAL_TRACE_REARM_FRAME_COUNT;
	const NRIMainUpscalerKind resolvedMain = ResolveMainUpscalerKind(false);
	const NRIPostSharpenKind resolvedPost = ResolvePostSharpenKind(false);
	Printf("NRI PT temporal trace: armed=%d reason=%s frame=%u debug=%d resolved_main=%s resolved_post=%s\n",
		(int)nri_pttraceframes,
		reason != nullptr ? reason : "unspecified",
		mFrameIndex,
		(int)nri_ptdebug,
		NRIGetMainUpscalerName(resolvedMain),
		NRIGetPostSharpenName(resolvedPost));
}

void NRIRenderer::TraceTemporalState(const char* stage, NRIMainUpscalerKind resolvedMainUpscaler, NRIPostSharpenKind resolvedPostSharpen, bool runAppTaa, FrameTextureSlot primarySlot, FrameTextureSlot secondarySlot) const
{
	if (!ShouldEmitRendererTemporalTraceLogs())
	{
		return;
	}

	const auto buildTextureSnapshot = [this](FrameTextureSlot slot)
	{
		const NRITextureResource& texture = GetFrameTexture(slot);
		NRITextureStatusSnapshot snapshot = {};
		snapshot.slotName = GetFrameTextureSlotName(slot);
		snapshot.width = texture.width;
		snapshot.height = texture.height;
		snapshot.access = (uint32_t)texture.state.access;
		snapshot.layout = (uint32_t)texture.state.layout;
		snapshot.stages = (uint32_t)texture.state.stages;
		return snapshot;
	};

	const FrameTextureSlot resolvedSecondarySlot = secondarySlot == FrameTextureSlot::Count ? mHistoryOutputSlot : secondarySlot;
	const NRIPTOutputPolicy outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	const ExposureRoute primaryExposureRoute = ResolveExposureRoute(primarySlot, outputPolicy, resolvedMainUpscaler, resolvedPostSharpen);
	const ExposureRoute secondaryExposureRoute = ResolveExposureRoute(resolvedSecondarySlot, outputPolicy, resolvedMainUpscaler, resolvedPostSharpen);
	NRITemporalTraceSnapshot snapshot = {};
	snapshot.stage = stage != nullptr ? stage : "unknown";
	snapshot.frameIndex = mFrameIndex;
	snapshot.debugMode = (int)nri_ptdebug;
	snapshot.resolvedMainUpscaler = NRIGetMainUpscalerName(resolvedMainUpscaler);
	snapshot.resolvedPostSharpen = NRIGetPostSharpenName(resolvedPostSharpen);
	snapshot.runAppTaa = runAppTaa;
	snapshot.guiCapture = mGuiCaptureActive;
	snapshot.primaryDomain = GetExposureDomainName(primaryExposureRoute.inputDomain);
	snapshot.secondaryDomain = GetExposureDomainName(secondaryExposureRoute.inputDomain);
	snapshot.temporalExposure = primaryExposureRoute.temporalExposure;
	snapshot.primaryPresentExposure = primaryExposureRoute.presentExposure;
	snapshot.secondaryPresentExposure = secondaryExposureRoute.presentExposure;
	snapshot.resetHistory = mResetHistory;
	snapshot.resetReason = mLastHistoryResetReason.c_str();
	snapshot.previousCamera = mHasPreviousCameraState;
	snapshot.historyInput = buildTextureSnapshot(mHistoryInputSlot);
	snapshot.historyOutput = buildTextureSnapshot(mHistoryOutputSlot);
	snapshot.primary = buildTextureSnapshot(primarySlot);
	snapshot.secondary = buildTextureSnapshot(resolvedSecondarySlot);
	snapshot.useUpscaled = mUseUpscaledInFinal;
	PrintNRITemporalTraceSnapshot(snapshot);
}

void NRIRenderer::PrintPortalTraversalStatus() const
{
	NRIPortalTraversalStatusSnapshot snapshot = {};
	if (!mMapWorld.valid)
	{
		PrintNRIPortalTraversalStatusSnapshot(snapshot);
		return;
	}

	snapshot.available = true;
	snapshot.depth = ClampTraceBounceCount((int)nri_ptportaldepth, 8u);
	snapshot.reflective = CountPortalTraversalClass(mMapWorld, NRI_PORTAL_TRAVERSAL_CLASS_REFLECTIVE);
	snapshot.transfer = CountPortalTraversalClass(mMapWorld, NRI_PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER);
	snapshot.runtimeBound = CountPortalTraversalClass(mMapWorld, NRI_PORTAL_TRAVERSAL_CLASS_RUNTIME_BOUND);
	snapshot.hittableSurfaces = mMapWorld.stats.portalSurfaceCount;
	snapshot.pendingPlanePortals = CountPendingPlanePortals(mMapWorld);
	PrintNRIPortalTraversalStatusSnapshot(snapshot);
}

void NRIRenderer::PrintResidentMapChunkRegistryStatus() const
{
	NRIResidentMapChunkRegistryStatusSnapshot snapshot = {};
	if (!mStaticSceneResidency.Registry().valid)
	{
		PrintNRIResidentMapChunkRegistryStatusSnapshot(snapshot);
		return;
	}

	snapshot.available = true;
	snapshot.buildSerial = mStaticSceneResidency.Registry().buildSerial;
	snapshot.chunkCount = mStaticSceneResidency.Registry().chunkCount;
	snapshot.activeChunkCount = mStaticSceneResidency.Registry().activeChunkCount;
	snapshot.mappedChunkCount = mStaticSceneResidency.Registry().mappedChunkCount;
	snapshot.accelerationResidentChunkCount = mStaticSceneResidency.Registry().accelerationResidentChunkCount;
	snapshot.animatedCandidateChunkCount = mStaticSceneResidency.Registry().animatedCandidateChunkCount;
	snapshot.animatedRefreshSuppressedChunkCount = mStaticSceneResidency.Registry().animatedRefreshSuppressedChunkCount;

	const NRIRuntimeMutationSettings runtimeMutationSettings = BuildNRIRuntimeMutationSettingsFromCVars();
	const float nearDistance = runtimeMutationSettings.nearDistance;
	const float nearDistanceSquared = nearDistance * nearDistance;
	const nri_scene::PTMapChunk* sampleChunk = nullptr;
	snapshot.nearDistance = nearDistance;
	snapshot.mapWorldChunkCount = (uint32_t)mMapWorld.chunks.size();
	const auto computeChunkDistanceSquared = [&](const nri_scene::PTMapChunk& chunk, float& outDistanceSquared) -> bool
	{
		if (!chunk.bounds.valid)
		{
			outDistanceSquared = 0.0f;
			return false;
		}

		outDistanceSquared = 0.0f;
		for (int axis = 0; axis < 3; ++axis)
		{
			float distance = 0.0f;
			if (mCurrentCameraPos[axis] < chunk.bounds.min[axis])
			{
				distance = chunk.bounds.min[axis] - mCurrentCameraPos[axis];
			}
			else if (mCurrentCameraPos[axis] > chunk.bounds.max[axis])
			{
				distance = mCurrentCameraPos[axis] - chunk.bounds.max[axis];
			}
			outDistanceSquared += distance * distance;
		}
		return true;
	};
	for (const nri_scene::PTMapChunk& chunk : mMapWorld.chunks)
	{
		float distanceSquared = 0.0f;
		const bool boundsValid = computeChunkDistanceSquared(chunk, distanceSquared);
		if (boundsValid)
		{
			snapshot.boundsValidCount++;
		}
		else
		{
			snapshot.boundsInvalidCount++;
		}

		const bool visible = IsChunkMarkedVisible(mCurrentVisibleChunkWords, chunk.chunkIndex);
		if (visible)
		{
			snapshot.visibleCount++;
		}
		else if (!boundsValid)
		{
			snapshot.invisibleUnknownCount++;
		}
		else if (distanceSquared <= nearDistanceSquared)
		{
			snapshot.invisibleNearCount++;
		}
		else
		{
			snapshot.invisibleFarCount++;
		}

		if (sampleChunk == nullptr && boundsValid)
		{
			sampleChunk = &chunk;
			snapshot.sampleDistance = sqrtf(distanceSquared);
			snapshot.sampleTier =
				visible ? "visible" :
				(distanceSquared <= nearDistanceSquared ? "near" : "far");
		}
	}
	if (sampleChunk != nullptr)
	{
		snapshot.sampleChunkIndex = sampleChunk->chunkIndex;
		snapshot.sampleCenter[0] = sampleChunk->bounds.center[0];
		snapshot.sampleCenter[1] = sampleChunk->bounds.center[1];
		snapshot.sampleCenter[2] = sampleChunk->bounds.center[2];
		snapshot.sampleRadius = sampleChunk->bounds.radius;
	}
	PrintNRIResidentMapChunkRegistryStatusSnapshot(snapshot);
}

bool NRIRenderer::UploadPersistentVoxelArenaMaterialBuffers(const std::vector<nri_scene::MaterialData>& materials)
{
	if (!mPersistentVoxels.HasValidBatch())
	{
		return true;
	}
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialMs);

	NRIPersistentVoxelMaterialUploadServices services = {};
	services.user = this;
	services.ensureMaterialArenaBuffer = [](void* user, NRIBufferResource& resource, uint64_t sizeBytes) -> bool
	{
		return static_cast<NRIRenderer*>(user)->EnsureResidentArenaBuffer(
			resource,
			sizeBytes,
			sizeof(nri_scene::MaterialData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess());
	};
	services.stageMaterialRanges = [](
		void* user,
		const NRIBufferResource& targetBuffer,
		const std::vector<RuntimeMutationResidentUploadRange>& ranges,
		const uint8_t* data,
		uint64_t availableBytes) -> bool
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		return renderer->StageResidentMaterialUploadRanges(
			targetBuffer,
			ranges,
			data,
			availableBytes,
			renderer->mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialBatches,
			renderer->mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialBatchRanges,
			renderer->mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialBatchBarrierCommands,
			renderer->mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialBatchCopyCommands);
	};
	services.noteMaterialUpload = [](void* user, uint64_t sizeBytes)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		renderer->NotePerfBufferUpload(
			&renderer->mMaterialBufferStats,
			sizeBytes,
			false,
			"persistent_voxel_material_variant",
			ResidentUploadKind_Material);
	};

	NRIPersistentVoxelMaterialUploadStats uploadStats = {};
	const bool uploaded = mPersistentVoxels.UploadArenaMaterialBuffers(
		materials,
		services,
		mFrameIndex,
		(bool)nri_voxelstats,
		uploadStats);

	auto& persistentVoxelDomain =
		mLastPerfShellTraceStats.sceneSelectBufferUploadDomains[(size_t)SceneBufferUploadDomain::PersistentVoxelMaterial];
	mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialRequestedBytes += uploadStats.requestedBytes;
	mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialUploads += uploadStats.uploads;
	mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialDirtyBytes += uploadStats.dirtyBytes;
	mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialBatchRejects += uploadStats.batchRejects;
	mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialBatchGapBytes += uploadStats.batchGapBytes;
	mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialUploadedBytes += uploadStats.uploadedBytes;
	persistentVoxelDomain.payloadBytes += uploadStats.domainPayloadBytes;
	persistentVoxelDomain.materialPayloadBytes += uploadStats.domainMaterialPayloadBytes;
	persistentVoxelDomain.hashChecks += uploadStats.domainHashChecks;
	persistentVoxelDomain.hashMisses += uploadStats.domainHashMisses;
	persistentVoxelDomain.uploadedBytes += uploadStats.domainUploadedBytes;
	persistentVoxelDomain.materialUploadedBytes += uploadStats.domainMaterialUploadedBytes;
	return uploaded;
}

void NRIRenderer::TraceRuntimeLinkEvents(HWDrawInfo& di)
{
	if (!nri_ptruntimelinktrace)
	{
		mHasRuntimeLinkTraceState = false;
		mLastRuntimeLinkTraceState = {};
		return;
	}

	RuntimeLinkTraceState current = {};
	current.valid = true;
	current.candidateSectorIndex = mRuntimeSpaceLinkLastFrame.candidateSectorIndex;
	current.sourceSectorIndex = mRuntimeSpaceLinkLastFrame.sourceSectorIndex;
	current.geoEffectActive = mRuntimeSpaceLinkLastFrame.geoEffectActive;

	const BitArray& visibleSectors = di.GetVisibleSectors();
	for (unsigned sectorIndex = 0; sectorIndex < visibleSectors.Size(); ++sectorIndex)
	{
		if (!visibleSectors.Check(sectorIndex))
		{
			continue;
		}

		const auto& sec = sector[sectorIndex];
		if (sec.lotag != 0)
		{
			current.visibleTaggedSectorCount++;
			if (current.taggedVisibleSectorStoredCount < current.taggedVisibleSectors.size())
			{
				RuntimeTaggedSectorDebugInfo info = {};
				if (gi != nullptr && gi->GetRuntimeLinkDebugTaggedSectorInfo((int)sectorIndex, &info))
				{
					current.taggedVisibleSectors[current.taggedVisibleSectorStoredCount++] = info;
				}
				else
				{
					info.available = true;
					info.sectorIndex = (int32_t)sectorIndex;
					info.lotag = sec.lotag;
					info.hitag = sec.hitag;
					current.taggedVisibleSectors[current.taggedVisibleSectorStoredCount++] = info;
				}
			}
		}
		if (sec.lotag == 848)
		{
			current.visible848SectorCount++;
		}
		if (sec.lotag == 160 || sec.lotag == 161)
		{
			current.visibleTeleportSectorCount++;
		}
	}

	if (gi != nullptr)
	{
		gi->GetRuntimeLinkDebugState(&current.game);
	}

	std::array<int32_t, 4> controlRoots =
	{
		current.candidateSectorIndex,
		current.sourceSectorIndex,
		current.game.playerSectorIndex,
		current.game.actorSectorIndex
	};

	for (const int32_t rootSectorIndex : controlRoots)
	{
		if (!validSectorIndex(rootSectorIndex))
		{
			continue;
		}

		RuntimeTaggedSectorDebugInfo rootInfo = {};
		if (GetRuntimeSectorControlInfo(rootSectorIndex, rootInfo))
		{
			AppendRuntimeSectorControlInfo(current.nearbyControlSectors, current.nearbyControlSectorStoredCount, rootInfo);
		}

		const auto& rootSector = sector[(unsigned)rootSectorIndex];
		for (const auto& wal : rootSector.walls)
		{
			if (!wal.twoSided())
			{
				continue;
			}

			const int32_t adjacentSectorIndex = wal.nextsector;
			RuntimeTaggedSectorDebugInfo adjacentInfo = {};
			if (GetRuntimeSectorControlInfo(adjacentSectorIndex, adjacentInfo))
			{
				AppendRuntimeSectorControlInfo(current.nearbyControlSectors, current.nearbyControlSectorStoredCount, adjacentInfo);
			}
		}
	}

	const bool sameAsLast =
		mHasRuntimeLinkTraceState &&
		mLastRuntimeLinkTraceState.valid == current.valid &&
		mLastRuntimeLinkTraceState.candidateSectorIndex == current.candidateSectorIndex &&
		mLastRuntimeLinkTraceState.sourceSectorIndex == current.sourceSectorIndex &&
		mLastRuntimeLinkTraceState.geoEffectActive == current.geoEffectActive &&
		mLastRuntimeLinkTraceState.visibleTaggedSectorCount == current.visibleTaggedSectorCount &&
		mLastRuntimeLinkTraceState.visible848SectorCount == current.visible848SectorCount &&
		mLastRuntimeLinkTraceState.visibleTeleportSectorCount == current.visibleTeleportSectorCount &&
		mLastRuntimeLinkTraceState.taggedVisibleSectorStoredCount == current.taggedVisibleSectorStoredCount &&
		mLastRuntimeLinkTraceState.nearbyControlSectorStoredCount == current.nearbyControlSectorStoredCount &&
		SameRuntimeLinkDebugState(mLastRuntimeLinkTraceState.game, current.game);

	bool sameTaggedSectors = true;
	if (sameAsLast)
	{
		for (uint32_t i = 0; i < current.taggedVisibleSectorStoredCount; ++i)
		{
			if (!SameRuntimeTaggedSectorDebugInfo(mLastRuntimeLinkTraceState.taggedVisibleSectors[i], current.taggedVisibleSectors[i]))
			{
				sameTaggedSectors = false;
				break;
			}
		}
	}

	bool sameNearbyControlSectors = true;
	if (sameAsLast)
	{
		for (uint32_t i = 0; i < current.nearbyControlSectorStoredCount; ++i)
		{
			if (!SameRuntimeTaggedSectorDebugInfo(mLastRuntimeLinkTraceState.nearbyControlSectors[i], current.nearbyControlSectors[i]))
			{
				sameNearbyControlSectors = false;
				break;
			}
		}
	}

	if (sameAsLast && sameTaggedSectors && sameNearbyControlSectors)
	{
		return;
	}

	mLastRuntimeLinkTraceState = current;
	mHasRuntimeLinkTraceState = true;

	Printf("NRI PT runtime link event: geo_effect=%s candidate_sector=%d source_sector=%d player_sector=%d lotag=%d hitag=%d effective_lotag=%d actor_sector=%d actor_lotag=%d actor_hitag=%d on_warp=%d transporter_hold=%d rr_geo_count=%d special_water=%s visible_tagged=%u visible_848=%u visible_teleport=%u\n",
		current.geoEffectActive ? "yes" : "no",
		current.candidateSectorIndex,
		current.sourceSectorIndex,
		current.game.playerSectorIndex,
		current.game.playerSectorLotag,
		current.game.playerSectorHitag,
		current.game.effectiveSectorLotag,
		current.game.actorSectorIndex,
		current.game.actorSectorLotag,
		current.game.actorSectorHitag,
		current.game.onWarpingSector,
		current.game.transporterHold,
		current.game.rrGeoCount,
		current.game.specialWaterSector ? "yes" : "no",
		current.visibleTaggedSectorCount,
		current.visible848SectorCount,
		current.visibleTeleportSectorCount);

	if (current.taggedVisibleSectorStoredCount > 0)
	{
		std::string taggedLine = "NRI PT runtime tagged sectors:";
		for (uint32_t i = 0; i < current.taggedVisibleSectorStoredCount; ++i)
		{
			const auto& info = current.taggedVisibleSectors[i];
			taggedLine += " [sector=" + std::to_string(info.sectorIndex) +
				" lotag=" + std::to_string(info.lotag) +
				" hitag=" + std::to_string(info.hitag);
			if (info.effectorCount > 0)
			{
				taggedLine += " effectors=";
				const uint32_t storedEffectors = std::min<uint32_t>(info.effectorCount, (uint32_t)countof(info.effectorLotags));
				for (uint32_t effectorIndex = 0; effectorIndex < storedEffectors; ++effectorIndex)
				{
					if (effectorIndex > 0)
					{
						taggedLine += ",";
					}
					taggedLine += std::to_string(info.effectorLotags[effectorIndex]) +
						"/" + std::to_string(info.effectorHitags[effectorIndex]);
				}
				if (info.effectorCount > storedEffectors)
				{
					taggedLine += ",...";
				}
			}
			taggedLine += "]";
		}
		Printf("%s\n", taggedLine.c_str());
	}

	if (current.nearbyControlSectorStoredCount > 0)
	{
		std::string controlLine = "NRI PT runtime nearby controls:";
		for (uint32_t i = 0; i < current.nearbyControlSectorStoredCount; ++i)
		{
			const auto& info = current.nearbyControlSectors[i];
			controlLine += " [sector=" + std::to_string(info.sectorIndex) +
				" lotag=" + std::to_string(info.lotag) +
				" hitag=" + std::to_string(info.hitag);
			if (info.effectorCount > 0)
			{
				controlLine += " effectors=";
				const uint32_t storedEffectors = std::min<uint32_t>(info.effectorCount, (uint32_t)countof(info.effectorLotags));
				for (uint32_t effectorIndex = 0; effectorIndex < storedEffectors; ++effectorIndex)
				{
					if (effectorIndex > 0)
					{
						controlLine += ",";
					}
					controlLine += std::to_string(info.effectorLotags[effectorIndex]) +
						"/" + std::to_string(info.effectorHitags[effectorIndex]);
				}
				if (info.effectorCount > storedEffectors)
				{
					controlLine += ",...";
				}
			}
			controlLine += "]";
		}
		Printf("%s\n", controlLine.c_str());
	}
}

void NRIRenderer::UpdateSurfaceProbe(const nri_scene::GeometryData& geometry, const nri_scene::MaterialBridgeData* materials, bool allowLogging)
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.surfaceProbeMs);
	const bool logSurfaceProbe = allowLogging && nri_ptsurfaceprobe > 0;
	if (geometry.primitives.empty())
	{
		mLastSurfaceProbe = {};
		mLastSurfaceProbe.valid = true;
		if (!logSurfaceProbe)
		{
			return;
		}

		const bool logOnChangeOnly = nri_ptsurfaceprobe >= 2;
		if (logOnChangeOnly && mLastLoggedSurfaceProbe.valid && !mLastLoggedSurfaceProbe.hit)
		{
			return;
		}

		Printf("NRI PT surface probe: miss\n");
		mLastLoggedSurfaceProbe = mLastSurfaceProbe;
		return;
	}

	SurfaceProbeResult result = {};
	result.valid = true;

	float direction[3] = { mCurrentCameraForward[0], mCurrentCameraForward[1], mCurrentCameraForward[2] };
	Normalize3(direction);

	float bestDistance = std::numeric_limits<float>::infinity();
	for (uint32_t primitiveIndex = 0; primitiveIndex < geometry.primitives.size(); ++primitiveIndex)
	{
		const auto& primitive = geometry.primitives[primitiveIndex];
		if ((primitive.flags & nri_scene::PrimitiveFlag_ReflectionOnly) != 0)
		{
			continue;
		}

		const auto& v0 = geometry.vertices[primitive.indices[0]];
		const auto& v1 = geometry.vertices[primitive.indices[1]];
		const auto& v2 = geometry.vertices[primitive.indices[2]];
		float hitT = 0.0f;
		if (!IntersectProbeTriangle(v0, v1, v2, mCurrentCameraPos, direction, hitT) || hitT >= bestDistance)
		{
			continue;
		}

		bestDistance = hitT;
		result.hit = true;
		result.primitiveIndex = primitiveIndex;
		result.materialIndex = primitive.materialIndex;
		result.primitiveFlags = primitive.flags;
		result.distance = hitT;
		result.position[0] = mCurrentCameraPos[0] + direction[0] * hitT;
		result.position[1] = mCurrentCameraPos[1] + direction[1] * hitT;
		result.position[2] = mCurrentCameraPos[2] + direction[2] * hitT;
		result.normal[0] = primitive.normal[0];
		result.normal[1] = primitive.normal[1];
		result.normal[2] = primitive.normal[2];
		if (primitiveIndex < geometry.primitiveProvenance.size())
		{
			result.provenance = geometry.primitiveProvenance[primitiveIndex];
		}
	}

	if (result.hit &&
		materials != nullptr &&
		result.materialIndex < materials->lightMetadata.size() &&
		result.materialIndex < materials->materials.size())
	{
		const auto& metadata = materials->lightMetadata[result.materialIndex];
		const auto& materialData = materials->materials[result.materialIndex];
		result.materialLightingFlags = metadata.lightingFlags;
		result.textureId = metadata.textureId;
		result.baseTextureId = metadata.baseTextureId != 0 ? metadata.baseTextureId : metadata.textureId;
		result.materialClass = metadata.materialClass;
		result.lightLevel = metadata.lightLevel;
		result.alpha = metadata.alpha;
		result.normalTextureIndex = metadata.normalTextureIndex;
		result.metallicTextureIndex = metadata.metallicTextureIndex;
		result.roughnessTextureIndex = metadata.roughnessTextureIndex;
		result.metalnessHint = materialData.metalnessHint;
		result.roughnessHint = materialData.roughnessHint;
		Copy3(metadata.averageColor, result.averageColor);
		Copy3(metadata.emissiveColor, result.emissiveColor);
		Copy3(metadata.glowColor, result.glowColor);

		nri_scene::MaterialData effectiveMaterial = {};
		effectiveMaterial.textureIndex = metadata.textureIndex;
		effectiveMaterial.paletteIndex = metadata.paletteIndex;
		effectiveMaterial.flags = metadata.materialFlags;
		effectiveMaterial.materialClass = metadata.materialClass;
		effectiveMaterial.lightLevel = metadata.lightLevel;
		effectiveMaterial.alpha = metadata.alpha;
		effectiveMaterial.normalTextureIndex = materialData.normalTextureIndex;
		effectiveMaterial.metallicTextureIndex = materialData.metallicTextureIndex;
		effectiveMaterial.roughnessTextureIndex = materialData.roughnessTextureIndex;
		effectiveMaterial.metalnessHint = materialData.metalnessHint;
		effectiveMaterial.roughnessHint = materialData.roughnessHint;
		effectiveMaterial.emissiveTextureIndex = metadata.emissiveTextureIndex;
		mSceneLights.ApplyEmissiveMaterialSettings(metadata, effectiveMaterial);
		result.emissiveMode = effectiveMaterial.emissiveMode;
		result.emissiveTextureIndex = effectiveMaterial.emissiveTextureIndex;
	}

	if (result.hit)
	{
		if (!mSurfaceProbeFrame.valid)
		{
			result.sceneDataSource = UINT32_MAX;
			result.sceneOwner = nri_diag::SurfaceProbeOwnerUnknown;
		}
		else if (!mSurfaceProbeFrame.usesStaticMapScene)
		{
			result.sceneDataSource = nri_diag::SceneDataSourceDynamic;
			result.sceneOwner = nri_diag::SurfaceProbeOwnerCapturedScene;
		}
		else if (result.primitiveIndex < mSurfaceProbeFrame.staticPrimitiveCount)
		{
			result.sceneDataSource = nri_diag::SceneDataSourceStatic;
			result.sceneOwner = nri_diag::SurfaceProbeOwnerStaticMap;
		}
		else
		{
			uint32_t overlayPrimitiveIndex = result.primitiveIndex - mSurfaceProbeFrame.staticPrimitiveCount;
			result.sceneDataSource = nri_diag::SceneDataSourceDynamic;
			if (overlayPrimitiveIndex < mSurfaceProbeFrame.runtimeSpaceLinkPrimitiveCount)
			{
				result.sceneOwner = nri_diag::SurfaceProbeOwnerRuntimeLink;
			}
			else
			{
				overlayPrimitiveIndex -= std::min(overlayPrimitiveIndex, mSurfaceProbeFrame.runtimeSpaceLinkPrimitiveCount);
				if (overlayPrimitiveIndex < mSurfaceProbeFrame.runtimeMutationPrimitiveCount)
				{
					result.sceneOwner = nri_diag::SurfaceProbeOwnerRuntimeMutation;
				}
				else
				{
					overlayPrimitiveIndex -= std::min(overlayPrimitiveIndex, mSurfaceProbeFrame.runtimeMutationPrimitiveCount);
					result.sceneOwner = overlayPrimitiveIndex < mSurfaceProbeFrame.dynamicPrimitiveCount ?
						nri_diag::SurfaceProbeOwnerDynamicOverlay :
						nri_diag::SurfaceProbeOwnerUnknown;
				}
			}
		}
	}

	auto sameIdentity = [](const SurfaceProbeResult& a, const SurfaceProbeResult& b)
	{
		if (a.valid != b.valid || a.hit != b.hit)
		{
			return false;
		}
		if (!a.valid || !a.hit)
		{
			return true;
		}

		return
			a.provenance.sourceType == b.provenance.sourceType &&
			a.provenance.sectorIndex == b.provenance.sectorIndex &&
			a.provenance.wallIndex == b.provenance.wallIndex &&
			a.provenance.nextSectorIndex == b.provenance.nextSectorIndex &&
			a.provenance.actorIndex == b.provenance.actorIndex &&
			a.provenance.drawListType == b.provenance.drawListType &&
			a.provenance.cstat == b.provenance.cstat &&
			a.textureId == b.textureId &&
			a.baseTextureId == b.baseTextureId &&
			a.materialLightingFlags == b.materialLightingFlags &&
			a.primitiveFlags == b.primitiveFlags &&
			a.sceneDataSource == b.sceneDataSource &&
			a.sceneOwner == b.sceneOwner &&
			a.materialIndex == b.materialIndex &&
			(a.provenance.sourceType != nri_scene::SurfaceSourceType::Unknown || a.primitiveIndex == b.primitiveIndex);
	};

	mLastSurfaceProbe = result;
	if (!logSurfaceProbe)
	{
		return;
	}

	const bool logOnChangeOnly = nri_ptsurfaceprobe >= 2;
	if (logOnChangeOnly && sameIdentity(mLastLoggedSurfaceProbe, result))
	{
		return;
	}

	if (!result.hit)
	{
		Printf("NRI PT surface probe: miss\n");
		mLastLoggedSurfaceProbe = result;
		return;
	}

	const SurfaceProbeEmissiveDiagnostics emissiveDiagnostics = BuildSurfaceProbeEmissiveDiagnostics(result);

	const uint32_t flags = result.primitiveFlags;
	const uint32_t lightingFlags = result.materialLightingFlags;
	const int32_t localSpaceIndex = result.provenance.mapChunkIndex >= 0 ? nri_scene::FindMapWorldLocalSpaceIndex(mMapWorld, (uint32_t)result.provenance.mapChunkIndex) : -1;
	const int32_t portalGraphIndex = nri_scene::FindMapWorldPortalIndex(mMapWorld, result.provenance);
	bool chunkResidentStatic = false;
	bool chunkStaticTlasInstanced = false;
	bool chunkStaticProbeIncluded = false;
	bool chunkVisibleGate = false;
	bool flatPlaneVisibilityRelevant = false;
	bool flatPlaneVisible = false;
	bool chunkReplaced = false;
	bool chunkSectorDirty = false;
	bool chunkDragged = false;
	bool chunkBlindSpot = false;
	uint32_t chunkReasonMask = 0;
	uint32_t chunkSectionDirtyCount = 0;
	uint32_t replacementSurfaceCount = 0;
	uint32_t replacementTriangleCount = 0;
	if (result.provenance.mapChunkIndex >= 0)
	{
		const uint32_t chunkIndex = (uint32_t)result.provenance.mapChunkIndex;
		chunkVisibleGate = IsChunkMarkedVisible(mCurrentVisibleChunkWords, chunkIndex);
		const uint32_t preferredChunkListIndex = NRIStaticSceneResidency::FindPreferredStaticSceneChunkListIndex(
			mStaticMapScene,
			mStaticMapChunkAtlas,
			chunkIndex);
		if (preferredChunkListIndex != UINT32_MAX)
		{
			chunkResidentStatic = true;
			const auto& chunkCache = mStaticMapScene.chunks[preferredChunkListIndex];
			chunkStaticTlasInstanced =
				chunkCache.active &&
				chunkCache.accelerationStructure.accelerationStructure != nullptr;
			chunkStaticProbeIncluded = chunkCache.active;
		}
		if (const auto* replacement = mRuntimeMutation.FindReplacement(chunkIndex))
		{
			chunkReplaced = replacement->active;
			chunkSectorDirty = replacement->sectorDirty;
			chunkDragged = replacement->dragged;
			chunkBlindSpot = replacement->blindSpot;
			chunkReasonMask = replacement->reasonMask;
			chunkSectionDirtyCount = replacement->sectionDirtyCount;
			replacementSurfaceCount = replacement->surfaceCount;
			replacementTriangleCount = replacement->triangleCount;
		}
	}
	if ((flags & nri_scene::MaterialFlag_Flat) != 0 &&
		(flags & (nri_scene::MaterialFlag_Sprite | nri_scene::MaterialFlag_Mirror | nri_scene::MaterialFlag_Sky | nri_scene::MaterialFlag_Portal)) == 0 &&
		result.provenance.sectorIndex >= 0)
	{
		flatPlaneVisibilityRelevant = true;
		flatPlaneVisible = IsFlatPlaneMarkedVisible(mCurrentVisibleFlatPlaneWords, result.provenance.sectorIndex, result.normal[1] < 0.0f);
	}
	const std::string chunkReasons = GetRuntimeMapMutationReasonSummary(chunkReasonMask);
	FString textureName;
	int32_t legacyTile = -1;
	ResolveSurfaceProbeTextureDebugInfo(result.textureId, textureName, legacyTile);
	FString materialTextureName;
	int32_t materialLegacyTile = -1;
	ResolveSurfaceProbeTextureDebugInfo(result.baseTextureId, materialTextureName, materialLegacyTile);
	Printf("NRI PT surface probe: hit source=%s drawlist=%s owner=%s data_source=%s chunk=%d gate_visible=%s flat_drawlist_visible=%s static_resident=%s static_tlas_instanced=%s static_probe_included=%s chunk_replaced=%s chunk_reasons=%s section_dirty=%u sector_dirty=%s dragged=%s blind_spot=%s replacement_surfaces=%u replacement_tris=%u local_space=%d portal_graph=%d sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x primitive=%u material=%u texid=%u legacy_tile=%d texture_name=%s material_texid=%u material_legacy_tile=%d material_texture_name=%s distance=%.2f pos=(%.2f, %.2f, %.2f) normal=(%.3f, %.3f, %.3f) flags=0x%x indexed=%s fullbright=%s flat=%s sprite=%s mirror=%s sky=%s portal=%s facing_billboard=%s point_sampled=%s tex_fullbright=%s glowing=%s auto_glow=%s glowmap=%s normalmap=%s metallic=%s roughness=%s normal_tex=%u metallic_tex=%u roughness_tex=%u metalness_hint=%.3f roughness_hint=%.3f material_class=%u emissive_mode=%s emissive_tex=%u light_surface=%s light_mat=%u emissive_surface=%s emissive_prims=%u emissive_hit=%s emissive_flags=0x%x emissive_rule=%u emissive_override=%u emissive_sector=%d sector_scale=%.3f sector_reach=%.3f sector_applied=%s emissive_area=%.2f emissive_power=%.3f emissive_sample_weight=%.3f emissive_pdf=%.6f emissive_intensity=%.3f material_response=%s material_scale=%.3f light=%.3f alpha=%.3f avg=(%.2f, %.2f, %.2f) emissive=(%.2f, %.2f, %.2f) glow=(%.2f, %.2f, %.2f)\n",
		nri_diag::GetSurfaceSourceTypeName(result.provenance.sourceType),
		nri_diag::GetDrawListTypeName(result.provenance.drawListType),
		nri_diag::GetSurfaceProbeSceneOwnerName(result.sceneOwner),
		nri_diag::GetSceneDataSourceName(result.sceneDataSource),
		result.provenance.mapChunkIndex,
		YesNo(chunkVisibleGate),
		flatPlaneVisibilityRelevant ? YesNo(flatPlaneVisible) : "n/a",
		YesNo(chunkResidentStatic),
		YesNo(chunkStaticTlasInstanced),
		YesNo(chunkStaticProbeIncluded),
		YesNo(chunkReplaced),
		chunkReasons.c_str(),
		chunkSectionDirtyCount,
		YesNo(chunkSectorDirty),
		YesNo(chunkDragged),
		YesNo(chunkBlindSpot),
		replacementSurfaceCount,
		replacementTriangleCount,
		localSpaceIndex,
		portalGraphIndex,
		result.provenance.sectorIndex,
		result.provenance.wallIndex,
		result.provenance.nextSectorIndex,
		result.provenance.actorIndex,
		result.provenance.cstat,
		result.primitiveIndex,
		result.materialIndex,
		result.textureId,
		legacyTile,
		textureName.GetChars(),
		result.baseTextureId,
		materialLegacyTile,
		materialTextureName.GetChars(),
		result.distance,
		result.position[0], result.position[1], result.position[2],
		result.normal[0], result.normal[1], result.normal[2],
		flags,
		YesNo((flags & nri_scene::MaterialFlag_Indexed) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Fullbright) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Flat) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Sprite) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Mirror) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Sky) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Portal) != 0),
		YesNo((flags & nri_scene::MaterialFlag_FacingBillboard) != 0),
		YesNo((flags & nri_scene::MaterialFlag_PointSampled) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureFullbright) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureGlowing) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureAutoGlowing) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_HasGlowmap) != 0),
		YesNo(result.normalTextureIndex != UINT32_MAX),
		YesNo(result.metallicTextureIndex != UINT32_MAX),
		YesNo(result.roughnessTextureIndex != UINT32_MAX),
		result.normalTextureIndex != UINT32_MAX ? result.normalTextureIndex : 0u,
		result.metallicTextureIndex != UINT32_MAX ? result.metallicTextureIndex : 0u,
		result.roughnessTextureIndex != UINT32_MAX ? result.roughnessTextureIndex : 0u,
		result.metalnessHint,
		result.roughnessHint,
		result.materialClass,
		nri_diag::GetMaterialEmissiveModeName(result.emissiveMode),
		result.emissiveTextureIndex != UINT32_MAX ? result.emissiveTextureIndex : 0u,
		YesNo(emissiveDiagnostics.sceneLightSurfaceMatch),
		emissiveDiagnostics.sceneLightMaterialIndex != UINT32_MAX ? emissiveDiagnostics.sceneLightMaterialIndex : 0u,
		YesNo(emissiveDiagnostics.activeEmissiveSurfaceMatch),
		emissiveDiagnostics.emissivePrimitiveMatchCount,
		YesNo(emissiveDiagnostics.exactEmissivePrimitiveMatch),
		emissiveDiagnostics.emissiveSourceFlags,
		emissiveDiagnostics.emissiveSourceRuleId,
		emissiveDiagnostics.emissiveOverrideRuleId,
		emissiveDiagnostics.emissiveSectorIndex,
		emissiveDiagnostics.sectorResponseScale,
		emissiveDiagnostics.sectorReachScale,
		YesNo(emissiveDiagnostics.sectorResponseApplied),
		emissiveDiagnostics.emissivePrimitiveArea,
		emissiveDiagnostics.emissivePowerEstimate,
		emissiveDiagnostics.emissiveSelectionWeight,
		emissiveDiagnostics.emissiveSelectionPdf,
		emissiveDiagnostics.emissiveIntensity,
		YesNo(emissiveDiagnostics.materialResponseEnabled),
		emissiveDiagnostics.materialResponseScale,
		result.lightLevel,
		result.alpha,
		result.averageColor[0], result.averageColor[1], result.averageColor[2],
		result.emissiveColor[0], result.emissiveColor[1], result.emissiveColor[2],
		result.glowColor[0], result.glowColor[1], result.glowColor[2]);
	mLastLoggedSurfaceProbe = result;
}

NRIRenderer::SurfaceProbeEmissiveDiagnostics NRIRenderer::BuildSurfaceProbeEmissiveDiagnostics(const SurfaceProbeResult& probe) const
{
	SurfaceProbeEmissiveDiagnostics diagnostics = {};
	if (!probe.valid || !probe.hit)
	{
		return diagnostics;
	}

	const auto provenanceMatches = [](const nri_scene::SurfaceProvenance& a, const nri_scene::SurfaceProvenance& b)
	{
		return
			a.sourceType == b.sourceType &&
			a.drawListType == b.drawListType &&
			a.mapChunkIndex == b.mapChunkIndex &&
			a.sectionIndex == b.sectionIndex &&
			a.sectorIndex == b.sectorIndex &&
			a.wallIndex == b.wallIndex &&
			a.nextSectorIndex == b.nextSectorIndex &&
			a.actorIndex == b.actorIndex &&
			a.cstat == b.cstat;
	};

	const auto mapOwnerToLightSource = [](uint32_t owner) -> SceneLightRecordSource
	{
		switch (owner)
		{
		case nri_diag::SurfaceProbeOwnerStaticMap: return SceneLightRecordSource::StaticMapScene;
		case nri_diag::SurfaceProbeOwnerCapturedScene: return SceneLightRecordSource::CapturedScene;
		case nri_diag::SurfaceProbeOwnerRuntimeMutation: return SceneLightRecordSource::RuntimeMutationScene;
		case nri_diag::SurfaceProbeOwnerDynamicOverlay: return SceneLightRecordSource::DynamicScene;
		default: return SceneLightRecordSource::CapturedScene;
		}
	};

	const SceneLightRecordSource expectedSource = mapOwnerToLightSource(probe.sceneOwner);
	const SceneLightSystem::SurfaceRecord* matchedSurface = nullptr;
	for (const auto& record : mSceneLights.GetSurfaceRecords())
	{
		if (!provenanceMatches(record.provenance, probe.provenance))
		{
			continue;
		}
		if (record.material.textureId != probe.textureId)
		{
			continue;
		}
		if (record.source == expectedSource)
		{
			matchedSurface = &record;
			break;
		}
		if (matchedSurface == nullptr)
		{
			matchedSurface = &record;
		}
	}

	if (matchedSurface == nullptr)
	{
		return diagnostics;
	}

	diagnostics.sceneLightSurfaceMatch = true;
	diagnostics.sceneLightMaterialIndex = matchedSurface->materialIndex;

	for (const auto& surface : mSceneLights.GetEmissiveSurfaces().activeSurfaces)
	{
		if (surface.source == matchedSurface->source &&
			surface.materialIndex == matchedSurface->materialIndex &&
			surface.textureId == matchedSurface->material.textureId &&
			surface.actorIndex == matchedSurface->provenance.actorIndex)
		{
			diagnostics.activeEmissiveSurfaceMatch = true;
			break;
		}
	}

	const uint32_t expectedDataSource =
		matchedSurface->source == SceneLightRecordSource::StaticMapScene ?
			nri_diag::SceneDataSourceStatic :
			nri_diag::SceneDataSourceDynamic;
	for (const auto& record : mBoundEmissivePrimitiveRecords)
	{
		if (record.dataSource == expectedDataSource &&
			record.materialIndex == matchedSurface->materialIndex &&
			record.textureId == matchedSurface->material.textureId &&
			record.actorIndex == matchedSurface->provenance.actorIndex)
		{
			diagnostics.emissivePrimitiveMatchCount++;
		}
		if (record.dataSource == probe.sceneDataSource &&
			record.primitiveIndex == probe.primitiveIndex)
		{
			diagnostics.exactEmissivePrimitiveMatch = true;
			diagnostics.emissiveSourceFlags = record.sourceFlags;
			diagnostics.emissiveSourceRuleId = record.sourceRuleId;
			diagnostics.emissiveOverrideRuleId = record.overrideRuleId;
			diagnostics.emissiveSectorIndex = record.sectorIndex;
			diagnostics.emissivePrimitiveArea = record.primitiveArea;
			diagnostics.emissivePowerEstimate = record.powerEstimate;
			diagnostics.emissiveSelectionWeight = record.selectionWeight;
			diagnostics.emissiveSelectionPdf = record.selectionPdf;
			diagnostics.emissiveIntensity = record.emissiveIntensity;
			diagnostics.sectorResponseScale = record.sectorResponseScale;
			diagnostics.sectorReachScale = record.sectorReachScale;
			diagnostics.sectorResponseApplied = record.sectorResponseApplied;
			diagnostics.materialResponseEnabled = record.materialResponseEnabled;
			diagnostics.materialResponseScale = record.materialResponseScale;
		}
	}

	return diagnostics;
}

bool NRIRenderer::BuildEmissiveLightEditTarget(PathTracingEmissiveLightEditTarget& outTarget) const
{
	outTarget = {};
	if (!mLastSurfaceProbe.valid)
	{
		outTarget.failureReason = "no sampled center hit has been recorded yet";
		return false;
	}

	if (!mLastSurfaceProbe.hit)
	{
		outTarget.valid = true;
		outTarget.failureReason = "last sampled center ray missed translated PT geometry";
		return false;
	}

	const SurfaceProbeEmissiveDiagnostics emissiveDiagnostics = BuildSurfaceProbeEmissiveDiagnostics(mLastSurfaceProbe);
	outTarget.valid = true;
	outTarget.hit = true;
	outTarget.emissive = emissiveDiagnostics.activeEmissiveSurfaceMatch;
	outTarget.sectorIndex = mLastSurfaceProbe.provenance.sectorIndex;
	outTarget.wallIndex = mLastSurfaceProbe.provenance.wallIndex;
	outTarget.textureId = (int)mLastSurfaceProbe.textureId;
	outTarget.baseTextureId = (int)mLastSurfaceProbe.baseTextureId;
	outTarget.materialIndex = (int)mLastSurfaceProbe.materialIndex;
	outTarget.surfaceLightOverlay = mLastSurfaceProbe.provenance.sourceType == nri_scene::SurfaceSourceType::SurfaceLightOverlay;
	outTarget.surfaceLightRuleId = outTarget.surfaceLightOverlay ? mLastSurfaceProbe.provenance.cstat : 0u;
	outTarget.position[0] = mLastSurfaceProbe.position[0];
	outTarget.position[1] = mLastSurfaceProbe.position[1];
	outTarget.position[2] = mLastSurfaceProbe.position[2];
	outTarget.normal[0] = mLastSurfaceProbe.normal[0];
	outTarget.normal[1] = mLastSurfaceProbe.normal[1];
	outTarget.normal[2] = mLastSurfaceProbe.normal[2];
	int32_t legacyTile = -1;
	ResolveSurfaceProbeTextureDebugInfo(mLastSurfaceProbe.textureId, outTarget.textureName, legacyTile);
	ResolveSurfaceProbeTextureDebugInfo(mLastSurfaceProbe.baseTextureId, outTarget.materialTextureName, legacyTile);
	outTarget.sectorResponseIntensity = std::max(0.0f, (float)nri_ptsectoremissionsignalstrength);
	outTarget.sectorResponseMin = std::max(0.0f, (float)nri_ptsectoremissionresponsemin);
	outTarget.sectorResponseMax = std::max(outTarget.sectorResponseMin, (float)nri_ptsectoremissionresponsemax);
	if (!outTarget.emissive)
	{
		outTarget.failureReason = emissiveDiagnostics.sceneLightSurfaceMatch ?
			"aimed surface is not currently an active emissive surface" :
			"aimed surface is not present in the scene-light surface registry";
		return false;
	}

	return true;
}

bool NRIRenderer::BuildSurfaceLightEditTarget(PathTracingEmissiveLightEditTarget& outTarget) const
{
	outTarget = {};
	if (!mLastSurfaceProbe.valid)
	{
		outTarget.failureReason = "no sampled center hit has been recorded yet";
		return false;
	}

	if (!mLastSurfaceProbe.hit)
	{
		outTarget.valid = true;
		outTarget.failureReason = "last sampled center ray missed translated PT geometry";
		return false;
	}

	const SurfaceProbeEmissiveDiagnostics emissiveDiagnostics = BuildSurfaceProbeEmissiveDiagnostics(mLastSurfaceProbe);
	outTarget.valid = true;
	outTarget.hit = true;
	outTarget.emissive = emissiveDiagnostics.activeEmissiveSurfaceMatch;
	outTarget.sectorIndex = mLastSurfaceProbe.provenance.sectorIndex;
	outTarget.wallIndex = mLastSurfaceProbe.provenance.wallIndex;
	outTarget.textureId = (int)mLastSurfaceProbe.textureId;
	outTarget.baseTextureId = (int)mLastSurfaceProbe.baseTextureId;
	outTarget.materialIndex = (int)mLastSurfaceProbe.materialIndex;
	outTarget.position[0] = mLastSurfaceProbe.position[0];
	outTarget.position[1] = mLastSurfaceProbe.position[1];
	outTarget.position[2] = mLastSurfaceProbe.position[2];
	outTarget.normal[0] = mLastSurfaceProbe.normal[0];
	outTarget.normal[1] = mLastSurfaceProbe.normal[1];
	outTarget.normal[2] = mLastSurfaceProbe.normal[2];
	float viewDirection[3] = { mCurrentCameraForward[0], mCurrentCameraForward[1], mCurrentCameraForward[2] };
	Normalize3(viewDirection);
	if (outTarget.normal[0] * viewDirection[0] + outTarget.normal[1] * viewDirection[1] + outTarget.normal[2] * viewDirection[2] > 0.0f)
	{
		outTarget.normal[0] = -outTarget.normal[0];
		outTarget.normal[1] = -outTarget.normal[1];
		outTarget.normal[2] = -outTarget.normal[2];
	}
	int32_t legacyTile = -1;
	ResolveSurfaceProbeTextureDebugInfo(mLastSurfaceProbe.textureId, outTarget.textureName, legacyTile);
	ResolveSurfaceProbeTextureDebugInfo(mLastSurfaceProbe.baseTextureId, outTarget.materialTextureName, legacyTile);
	outTarget.sectorResponseIntensity = std::max(0.0f, (float)nri_ptsectoremissionsignalstrength);
	outTarget.sectorResponseMin = std::max(0.0f, (float)nri_ptsectoremissionresponsemin);
	outTarget.sectorResponseMax = std::max(outTarget.sectorResponseMin, (float)nri_ptsectoremissionresponsemax);
	return true;
}

const char* NRIRenderer::GetAvailabilityReason() const
{
	if (mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return "renderer device is not initialized";
	}

	const nri::DeviceDesc& deviceDesc = mFrameBuffer->mCore.GetDeviceDesc(*mFrameBuffer->mDevice);
	if (deviceDesc.tiers.rayTracing == 0)
	{
		return "required ray tracing capability is unavailable on this device/API";
	}

	const size_t requiredRootConstantSize = std::max({ sizeof(NRITraceSceneConstants), sizeof(NRITemporalConstants), sizeof(NRIPresentConstants), sizeof(NRIExposureConstants) });
	if (deviceDesc.pipelineLayout.rootConstantMaxSize < requiredRootConstantSize ||
		deviceDesc.pipelineLayout.rootDescriptorMaxNum < 1 ||
		deviceDesc.pipelineLayout.descriptorSetMaxNum < 5)
	{
		return "device pipeline layout limits are below the NRI PT backend requirements";
	}

	if (const char* sceneTextureLimitReason = GetSceneTextureDescriptorLimitFailureReason(deviceDesc))
	{
		return sceneTextureLimitReason;
	}

	return "path tracing is unavailable";
}

bool NRIRenderer::RefreshPathTracingAvailability()
{
	return CheckPathTracingSupport();
}

bool NRIRenderer::CheckPathTracingSupport()
{
	mPathTracingSupported = mFrameBuffer != nullptr && mFrameBuffer->mDevice != nullptr;
	if (!mPathTracingSupported)
	{
		return false;
	}

	const nri::DeviceDesc& deviceDesc = mFrameBuffer->mCore.GetDeviceDesc(*mFrameBuffer->mDevice);
	const size_t requiredRootConstantSize = std::max({ sizeof(NRITraceSceneConstants), sizeof(NRITemporalConstants), sizeof(NRIPresentConstants), sizeof(NRIExposureConstants) });
	if (deviceDesc.tiers.rayTracing == 0 ||
		deviceDesc.pipelineLayout.rootConstantMaxSize < requiredRootConstantSize ||
		deviceDesc.pipelineLayout.rootDescriptorMaxNum < 1 ||
		deviceDesc.pipelineLayout.descriptorSetMaxNum < 5)
	{
		mPathTracingSupported = false;
		LogFallback(GetAvailabilityReason());
	}
	else if (const char* sceneTextureLimitReason = GetSceneTextureDescriptorLimitFailureReason(deviceDesc))
	{
		mPathTracingSupported = false;
		LogFallback(sceneTextureLimitReason);
	}

	return mPathTracingSupported;
}

void NRIRenderer::RefreshMapWorld()
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.mapWorldMs);
	const uint64_t pendingBuildSerial = nri_scene::GetPendingLevelGeometryBuildSerial();
	const bool levelChanged = mMapWorld.level != currentLevel;
	if (levelChanged)
	{
		RequestHistoryReset("map-load", true, true);
		mSceneTextures.CacheStats() = {};
		mSceneLights.ResetPersistentDynamicEmissiveHighWaterStats();
		mRuntimeMutation.ResetLevelHighWaterStats();
	}
	const bool needsBuild = !mMapWorld.valid || levelChanged || pendingBuildSerial != mObservedMapWorldBuildSerial;
	if (!needsBuild)
	{
		if (mAllowStartupMapWorldCorrection && mFrameIndex > mStartupMapWorldCorrectionDeadlineFrame)
		{
			mAllowStartupMapWorldCorrection = false;
		}
		return;
	}

	ResetPersistentDynamicEmissiveCache();
	mAllowStartupMapWorldCorrection = true;
	mStartupMapWorldCorrectionDeadlineFrame = mFrameIndex + 8u;
	mAllowStartupMutationRebaseline = false;
	mPendingStartupMutationRebaseline = false;
	mStartupMutationRebaselineDeadlineFrame = 0;

	nri_scene::PTMapWorld world;
	if (!nri_scene::BuildMapWorld(world))
	{
		if (pendingBuildSerial != mObservedMapWorldBuildSerial || levelChanged)
		{
			Printf(TEXTCOLOR_RED "NRI PT map world: authoritative level-load build failed for %s.\n",
				currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)");
		}
		mMapWorld.Reset();
		mMapWorld.level = currentLevel;
		mObservedMapWorldBuildSerial = pendingBuildSerial;
		mPendingStartupVisibleChunkValidation.clear();
		mRuntimeMutation.ResetForMapWorldBuildFailure();
		mAllowStartupMapWorldCorrection = false;
		mStartupMapWorldCorrectionDeadlineFrame = 0;
		mAllowStartupMutationRebaseline = false;
		mPendingStartupMutationRebaseline = false;
		mStartupMutationRebaselineDeadlineFrame = 0;
		return;
	}

	mMapWorld = std::move(world);
	mObservedMapWorldBuildSerial = pendingBuildSerial;
	mPendingStartupVisibleChunkValidation.clear();
	mPendingStartupVisibleChunkValidation.resize(mMapWorld.chunks.size(), 0u);
	mRuntimeMutation.PrepareStartupBaseline(mMapWorld.buildSerial, (uint32_t)mMapWorld.chunks.size());
	const auto& stats = mMapWorld.stats;
	Printf("NRI PT map world built: level=%s build_serial=%llu chunks=%u surfaces=%u walls=%u flats=%u portals=%u skies=%u tris=%u\n",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)mMapWorld.buildSerial,
		stats.chunkCount,
		stats.surfaceCount,
		stats.wallSurfaceCount,
		stats.flatSurfaceCount,
		stats.portalSurfaceCount,
		stats.skySurfaceCount,
		stats.triangleCount);
}

void NRIRenderer::RebuildStartupMutationBaseline()
{
	mPendingStartupMutationRebaseline = false;
	mAllowStartupMutationRebaseline = false;
}

bool NRIRenderer::ApplyStartupMapWorldCorrectionIfNeeded(const char* trigger)
{
	if (!mAllowStartupMapWorldCorrection)
	{
		return true;
	}

	if (mFrameIndex > mStartupMapWorldCorrectionDeadlineFrame)
	{
		mAllowStartupMapWorldCorrection = false;
		return true;
	}

	if (!mMapWorld.valid ||
		!mStaticMapScene.valid ||
		!mRuntimeMutation.CanApplyStartupCorrection((uint32_t)mMapWorld.chunks.size()))
	{
		return true;
	}

	nri_scene::PTMapWorld correctedWorld = {};
	if (!nri_scene::BuildMapWorld(correctedWorld))
	{
		Printf(TEXTCOLOR_RED "NRI PT startup world correction: authoritative rebuild failed for %s trigger=%s frame=%u.\n",
			currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)",
			trigger != nullptr ? trigger : "unknown",
			mFrameIndex);
		mAllowStartupMapWorldCorrection = false;
		mStartupMapWorldCorrectionDeadlineFrame = 0;
		return true;
	}

	uint32_t chunkDiffCount = 0;
	uint32_t surfaceDiffCount = 0;
	StartupMapWorldDiffDetails diffDetails = {};
	if (!StartupMapWorldStructureDiffers(mMapWorld, correctedWorld, chunkDiffCount, surfaceDiffCount, &diffDetails))
	{
		return true;
	}

	DestroyStaticMapSceneCache("startup-world-correction");
	mStaticMapScene = {};
	mStaticAccelerationBuildSerial = 0;
	mSkyEnvironment.PreservedStaticMapSky() = {};
	mMapWorld = std::move(correctedWorld);
	mObservedMapWorldBuildSerial = nri_scene::GetPendingLevelGeometryBuildSerial();
	mAllowStartupMapWorldCorrection = false;
	mStartupMapWorldCorrectionDeadlineFrame = 0;
	if (mPendingStartupVisibleChunkValidation.size() < mMapWorld.chunks.size())
	{
		mPendingStartupVisibleChunkValidation.resize(mMapWorld.chunks.size(), 0u);
	}
	mRuntimeMutation.PrepareStartupBaseline(mMapWorld.buildSerial, (uint32_t)mMapWorld.chunks.size());
	for (uint32_t chunkIndex : diffDetails.lateVisibleValidationChunks)
	{
		if (chunkIndex < mPendingStartupVisibleChunkValidation.size())
		{
			mPendingStartupVisibleChunkValidation[chunkIndex] = 1u;
		}
	}
	RequestHistoryReset("startup-world-correction");

	Printf("NRI PT startup world correction: trigger=%s level=%s frame=%u build_serial=%llu chunk_diffs=%u surface_diffs=%u chunks=%u surfaces=%u tris=%u\n",
		trigger != nullptr ? trigger : "unknown",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		mFrameIndex,
		(unsigned long long)mMapWorld.buildSerial,
		chunkDiffCount,
		surfaceDiffCount,
		mMapWorld.stats.chunkCount,
		mMapWorld.stats.surfaceCount,
		mMapWorld.stats.triangleCount);

	if (nri_ptscenestats)
	{
		const auto& stats = mMapWorld.stats;
		Printf("NRI PT startup world correction: trigger=%s level=%s frame=%u build_serial=%llu chunk_diffs=%u surface_diffs=%u chunks=%u surfaces=%u walls=%u flats=%u portals=%u skies=%u tris=%u\n",
			trigger != nullptr ? trigger : "unknown",
			mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
			mFrameIndex,
			(unsigned long long)mMapWorld.buildSerial,
			chunkDiffCount,
			surfaceDiffCount,
			stats.chunkCount,
			stats.surfaceCount,
			stats.wallSurfaceCount,
			stats.flatSurfaceCount,
			stats.portalSurfaceCount,
			stats.skySurfaceCount,
			stats.triangleCount);
		const std::string reasonSummary = BuildStartupMapWorldDiffReasonSummary(diffDetails);
		Printf("NRI PT startup world correction detail: trigger=%s frame=%u reasons=%s sampled_chunks=%u/%u\n",
			trigger != nullptr ? trigger : "unknown",
			mFrameIndex,
			reasonSummary.c_str(),
			(uint32_t)diffDetails.chunkSamples.size(),
			chunkDiffCount);
		if (!diffDetails.lateVisibleValidationChunks.empty())
		{
			Printf("NRI PT startup world correction late-visible: trigger=%s frame=%u chunks=%u\n",
				trigger != nullptr ? trigger : "unknown",
				mFrameIndex,
				(uint32_t)diffDetails.lateVisibleValidationChunks.size());
		}
		for (size_t sampleIndex = 0; sampleIndex < diffDetails.chunkSamples.size(); ++sampleIndex)
		{
			const auto& sample = diffDetails.chunkSamples[sampleIndex];
			Printf("NRI PT startup world correction chunk: trigger=%s frame=%u sample=%u/%u chunk=%u sector=%d->%d surfaces=%u->%u tris=%u->%u surface_diffs=%u\n",
				trigger != nullptr ? trigger : "unknown",
				mFrameIndex,
				(uint32_t)(sampleIndex + 1u),
				(uint32_t)diffDetails.chunkSamples.size(),
				sample.chunkIndex,
				sample.currentSectorIndex,
				sample.rebuiltSectorIndex,
				sample.currentSurfaceCount,
				sample.rebuiltSurfaceCount,
				sample.currentTriangleCount,
				sample.rebuiltTriangleCount,
				sample.surfaceDiffCount);
		}
	}
	return true;
}








SceneLightSystem::RuntimeLightClusterBuildInput NRIRenderer::BuildRuntimeLightClusterInput() const
{
	SceneLightSystem::RuntimeLightClusterBuildInput input = {};
	input.renderWidth = mRenderWidth;
	input.renderHeight = mRenderHeight;
	input.tileSize = NRI_RUNTIME_LIGHT_TILE_SIZE;
	input.maxRuntimeLights = NRI_MAX_RUNTIME_POINT_LIGHTS;
	Copy3(mCurrentCameraPos, input.currentCameraPos);
	Copy3(mCurrentCameraForward, input.currentCameraForward);
	Copy3(mCurrentCameraRight, input.currentCameraRight);
	Copy3(mCurrentCameraUp, input.currentCameraUp);
	input.tanHalfFovX = mCurrentTanHalfFovX;
	input.tanHalfFovY = mCurrentTanHalfFovY;
	input.mirrorExtendedLightCoverage =
		mHasVisibleMirrorPortalLastFrame &&
		nri_ptmirrordynamicdistance > 0.0f;
	input.mirrorExtendedLightDistance = input.mirrorExtendedLightCoverage ? (float)nri_ptmirrordynamicdistance : 0.0f;
	return input;
}











bool NRIRenderer::EnsureAutoExposureResources(const NRIAutoExposureSettings& settings)
{
	return EnsureNRIRendererAutoExposureResources(*this, settings);
}

void NRIRenderer::DestroyAutoExposureResources()
{
	DestroyNRIRendererAutoExposureResources(*this);
}

bool NRIRenderer::UpdateAutoExposureDescriptorSets(FrameTextureSlot sourceSlot)
{
	return UpdateNRIRendererAutoExposureDescriptorSets(*this, (uint32_t)sourceSlot);
}

bool NRIRenderer::DispatchAutoExposure(FrameTextureSlot sourceSlot)
{
	return DispatchNRIRendererAutoExposure(*this, (uint32_t)sourceSlot);
}

void NRIRenderer::CopyAutoExposureStatsForReadback(uint64_t frameNumber)
{
	CopyNRIRendererAutoExposureStatsForReadback(*this, frameNumber);
}

void NRIRenderer::ReadbackAutoExposureStats()
{
	ReadbackNRIRendererAutoExposureStats(*this);
}



void NRIRenderer::BindSceneRootDescriptors()
{
	if (mTopLevelAS.descriptor != nullptr)
	{
		mFrameBuffer->mCore.CmdSetRootDescriptor(*mFrameBuffer->mCommandBuffer, { 0, mTopLevelAS.descriptor, 0, nri::BindPoint::COMPUTE });
	}
}

uint32_t NRIRenderer::CountPotentialOutstandingQueuedFrames() const
{
	if (mFrameBuffer == nullptr)
	{
		return 0;
	}

	uint32_t count = 0;
	for (uint32_t i = 0; i < (uint32_t)mFrameBuffer->mQueuedFrames.size(); ++i)
	{
		if (i == mFrameBuffer->mCurrentQueuedFrameIndex)
		{
			continue;
		}

		const auto& queuedFrame = mFrameBuffer->mQueuedFrames[i];
		if (queuedFrame.hasSubmittedWork && queuedFrame.lastSubmittedFenceValue != 0)
		{
			count++;
		}
	}

	return count;
}

uint32_t NRIRenderer::GetCurrentQueuedFrameIndex() const
{
	if (mFrameBuffer == nullptr || mFrameBuffer->mQueuedFrames.empty())
	{
		return 0;
	}

	return std::min<uint32_t>(mFrameBuffer->mCurrentQueuedFrameIndex, (uint32_t)mFrameBuffer->mQueuedFrames.size() - 1u);
}

void NRIRenderer::UpdatePerFrameState(HWDrawInfo& di)
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.updateStateMs);
	Clocker clock(NriPTUpdateState);

	if (mHasPreviousCameraState)
	{
		Copy3(mCurrentCameraPos, mPreviousCameraPos);
		Copy3(mCurrentCameraForward, mPreviousCameraForward);
		Copy3(mCurrentCameraRight, mPreviousCameraRight);
		Copy3(mCurrentCameraUp, mPreviousCameraUp);
		mPreviousTanHalfFovX = mCurrentTanHalfFovX;
		mPreviousTanHalfFovY = mCurrentTanHalfFovY;
		Copy2(mCurrentJitter, mPreviousJitter);
		std::memcpy(mPreviousViewToClip, mCurrentViewToClip, sizeof(mPreviousViewToClip));
		std::memcpy(mPreviousWorldToView, mCurrentWorldToView, sizeof(mPreviousWorldToView));
	}

	VSMatrix inverseView;
	if (!di.VPUniforms.mViewMatrix.inverseMatrix(inverseView))
	{
		std::memset(mCurrentCameraPos, 0, sizeof(mCurrentCameraPos));
		std::memset(mCurrentCameraForward, 0, sizeof(mCurrentCameraForward));
		std::memset(mCurrentCameraRight, 0, sizeof(mCurrentCameraRight));
		std::memset(mCurrentCameraUp, 0, sizeof(mCurrentCameraUp));
		mCurrentCameraForward[2] = -1.0f;
		mCurrentCameraRight[0] = 1.0f;
		mCurrentCameraUp[1] = 1.0f;
	}
	else
	{
		float origin[4] = {};
		float rightPoint[4] = {};
		float upPoint[4] = {};
		float forwardPoint[4] = {};
		TransformPoint(inverseView, 0.0f, 0.0f, 0.0f, origin);
		TransformPoint(inverseView, 1.0f, 0.0f, 0.0f, rightPoint);
		TransformPoint(inverseView, 0.0f, 1.0f, 0.0f, upPoint);
		TransformPoint(inverseView, 0.0f, 0.0f, -1.0f, forwardPoint);

		const float cameraPos[3] = {
			origin[0],
			origin[1],
			origin[2]
		};
		const float rightDelta[3] = {
			rightPoint[0] - origin[0],
			rightPoint[1] - origin[1],
			rightPoint[2] - origin[2]
		};
		const float upDelta[3] = {
			upPoint[0] - origin[0],
			upPoint[1] - origin[1],
			upPoint[2] - origin[2]
		};
		const float forwardDelta[3] = {
			forwardPoint[0] - origin[0],
			forwardPoint[1] - origin[1],
			forwardPoint[2] - origin[2]
		};

		Copy3(cameraPos, mCurrentCameraPos);
		Copy3(rightDelta, mCurrentCameraRight);
		Copy3(upDelta, mCurrentCameraUp);
		Copy3(forwardDelta, mCurrentCameraForward);

		Normalize3(mCurrentCameraRight);
		Normalize3(mCurrentCameraUp);
		Normalize3(mCurrentCameraForward);
	}

	const float* projection = di.VPUniforms.mProjectionMatrix.get();
	const float projectionScaleX = projection != nullptr ? std::fabs(projection[0]) : 0.0f;
	const float projectionScaleY = projection != nullptr ? std::fabs(projection[5]) : 0.0f;
	if (projectionScaleX > 0.0001f && projectionScaleY > 0.0001f)
	{
		// Match the hardware backend frustum exactly instead of rebuilding Y-FOV from the PT render dimensions.
		mCurrentTanHalfFovX = 1.0f / projectionScaleX;
		mCurrentTanHalfFovY = 1.0f / projectionScaleY;
	}
	else
	{
		const float tanHalfFovX = tanf((float)di.Viewpoint.FieldOfView.Radians() * 0.5f);
		mCurrentTanHalfFovX = tanHalfFovX;
		mCurrentTanHalfFovY = tanHalfFovX * ((float)mRenderHeight / std::max(1.0f, (float)mRenderWidth));
	}
	const NRIMainUpscalerKind resolvedMainUpscaler = ResolveMainUpscalerKind(false);
	if (!nri_ptbootstrap && !mGuiCaptureActive && NRIShouldUseTemporalJitter(resolvedMainUpscaler))
	{
		const nri::UpscalerMode resolvedUpscalerMode = NRIResolveUpscalerModeForMain(resolvedMainUpscaler, GetSelectedUpscalerMode());
		const uint32_t jitterPhaseCount = NRIGetTemporalJitterPhaseCount(resolvedMainUpscaler, resolvedUpscalerMode, mGuiCaptureActive);
		ComputeTemporalJitter(mFrameIndex, jitterPhaseCount, mCurrentJitter);
	}
	else
	{
		mCurrentJitter[0] = 0.0f;
		mCurrentJitter[1] = 0.0f;
	}
	FillMatrix(mCurrentViewToClip, di.VPUniforms.mProjectionMatrix);
	FillMatrix(mCurrentWorldToView, di.VPUniforms.mViewMatrix);
	const BitArray& visibleSectors = di.GetVisibleSectors();
	const size_t visibleChunkWordCount = std::max<size_t>((mMapWorld.chunks.size() + 31u) / 32u, 1u);
	const size_t visibleFlatPlaneWordCount = std::max<size_t>(((size_t)sector.Size() * 2u + 31u) / 32u, 1u);
	mCurrentVisibleChunkWords.assign(visibleChunkWordCount, 0u);
	mCurrentVisibleFlatPlaneWords.assign(visibleFlatPlaneWordCount, 0u);
	for (unsigned sectorIndex = 0; sectorIndex < visibleSectors.Size(); ++sectorIndex)
	{
		if (!visibleSectors.Check(sectorIndex))
		{
			continue;
		}

		MarkVisibleChunkForSector(mMapWorld, (int32_t)sectorIndex, mCurrentVisibleChunkWords);
	}
	// HWDrawInfo can accumulate geometry from multiple RenderScene passes
	// while its final visible-sector bitset only reflects the last traversal.
	// Union the root sectors and accumulated drawlists so the PT chunk gate
	// tracks the scene the HAL actually built this frame.
	AccumulateVisibleChunksFromViewRoots(di, mMapWorld, mCurrentVisibleChunkWords);
	AccumulateVisibleChunksFromDrawLists(di, mMapWorld, mCurrentVisibleChunkWords);
	// Chunk visibility is still too coarse for overlapping static floors and ceilings.
	// Track the exact floor/ceiling sectors backed by the accumulated flat drawlists
	// so the RT primary path can reject hidden coplanar static flat sections.
	AccumulateVisibleFlatPlanesFromDrawLists(di, mCurrentVisibleFlatPlaneWords);
	if (ShouldEmitRendererTemporalTraceLogs())
	{
		const uint32_t targetWidth = mFrameBuffer->mActiveTarget != nullptr ? mFrameBuffer->mActiveTarget->width : 0u;
		const uint32_t targetHeight = mFrameBuffer->mActiveTarget != nullptr ? mFrameBuffer->mActiveTarget->height : 0u;
		const int32_t sceneLeft = mFrameBuffer->mSceneViewport.left;
		const int32_t sceneBottom = mFrameBuffer->mSceneViewport.top;
		const int32_t sceneWidth = mFrameBuffer->mSceneViewport.width;
		const int32_t sceneHeight = mFrameBuffer->mSceneViewport.height;
		const int32_t sceneTop = (int32_t)targetHeight - sceneBottom - sceneHeight;
		const auto& uniformCameraPos = di.VPUniforms.mCameraPos;
		const FVector3 hwForward(di.Viewpoint.HWAngles);
		Printf("NRI PT camera: frame=%u hw_pitch=%.3f hw_yaw=%.3f hw_roll=%.3f scene_bl=(%d,%d %dx%d) scene_tl=(%u,%u %ux%u) target=%ux%u uniform_pos=(%.3f,%.3f,%.3f) inverse_pos=(%.3f,%.3f,%.3f) hw_forward=(%.3f,%.3f,%.3f) basis_fwd=(%.3f,%.3f,%.3f) basis_right=(%.3f,%.3f,%.3f) basis_up=(%.3f,%.3f,%.3f) tan=(%.6f,%.6f) proj=(%.6f,%.6f,%.6f,%.6f)\n",
			mFrameIndex,
			di.Viewpoint.HWAngles.Pitch.Degrees(),
			di.Viewpoint.HWAngles.Yaw.Degrees(),
			di.Viewpoint.HWAngles.Roll.Degrees(),
			mFrameBuffer->mSceneViewport.left,
			mFrameBuffer->mSceneViewport.top,
			mFrameBuffer->mSceneViewport.width,
			mFrameBuffer->mSceneViewport.height,
			sceneLeft,
			sceneTop,
			sceneWidth,
			sceneHeight,
			targetWidth,
			targetHeight,
			uniformCameraPos.X,
			uniformCameraPos.Y,
			uniformCameraPos.Z,
			mCurrentCameraPos[0],
			mCurrentCameraPos[1],
			mCurrentCameraPos[2],
			hwForward.X,
			hwForward.Y,
			hwForward.Z,
			mCurrentCameraForward[0],
			mCurrentCameraForward[1],
			mCurrentCameraForward[2],
			mCurrentCameraRight[0],
			mCurrentCameraRight[1],
			mCurrentCameraRight[2],
			mCurrentCameraUp[0],
			mCurrentCameraUp[1],
			mCurrentCameraUp[2],
			mCurrentTanHalfFovX,
			mCurrentTanHalfFovY,
			projection != nullptr ? projection[0] : 0.0f,
			projection != nullptr ? projection[5] : 0.0f,
			projection != nullptr ? projection[8] : 0.0f,
			projection != nullptr ? projection[9] : 0.0f);
	}

	if (mHasPreviousCameraState && !mResetHistory)
	{
		const float dx = mCurrentCameraPos[0] - mPreviousCameraPos[0];
		const float dy = mCurrentCameraPos[1] - mPreviousCameraPos[1];
		const float dz = mCurrentCameraPos[2] - mPreviousCameraPos[2];
		const float distanceSq = dx * dx + dy * dy + dz * dz;
		static constexpr float TeleportDistanceThreshold = 2048.0f;
		if (distanceSq > TeleportDistanceThreshold * TeleportDistanceThreshold)
		{
			RequestHistoryReset("camera-teleport", true, false);
		}
	}

	if (!mHasPreviousCameraState)
	{
		Copy3(mCurrentCameraPos, mPreviousCameraPos);
		Copy3(mCurrentCameraForward, mPreviousCameraForward);
		Copy3(mCurrentCameraRight, mPreviousCameraRight);
		Copy3(mCurrentCameraUp, mPreviousCameraUp);
		mPreviousTanHalfFovX = mCurrentTanHalfFovX;
		mPreviousTanHalfFovY = mCurrentTanHalfFovY;
		Copy2(mCurrentJitter, mPreviousJitter);
		std::memcpy(mPreviousViewToClip, mCurrentViewToClip, sizeof(mPreviousViewToClip));
		std::memcpy(mPreviousWorldToView, mCurrentWorldToView, sizeof(mPreviousWorldToView));
	}

	UpdateNightVisionState();
}
