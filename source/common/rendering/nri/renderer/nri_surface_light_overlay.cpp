#include "nri_surface_light_overlay.h"

#include "../scene/nri_scene_bridge.h"
#include "lightoverlay.h"
#include "texinfo.h"
#include "texturemanager.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace
{
	static uint64_t HashLightOverlayText(uint64_t hash, const char* text)
	{
		if (text == nullptr)
		{
			return hash;
		}

		for (const unsigned char* cursor = (const unsigned char*)text; *cursor != '\0'; ++cursor)
		{
			hash ^= (uint64_t)(*cursor);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	static uint32_t BuildResolvedLightOverlayRuleId(const char* id, const char* classOrMapName, const LightOverlaySourceLocation& source)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashLightOverlayText(hash, id);
		hash = HashLightOverlayText(hash, classOrMapName);
		hash = HashLightOverlayText(hash, source.sourceName.GetChars());
		hash ^= (uint64_t)source.orderIndex + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		const uint32_t ruleId = (uint32_t)(hash ^ (hash >> 32));
		return ruleId != 0 ? ruleId : 1u;
	}

	static uint32_t BuildSurfaceLightRuleId(const ResolvedLightOverlaySurfaceLightRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.mapName.GetChars(), rule.source);
	}

	static void RebuildSurfaceLightSceneViewStats(nri_scene::SceneView& sceneView)
	{
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
		sceneView.stats = stats;
	}
}

namespace nri_surface_light_overlay
{
bool BuildSurfaceLightOverlay(
	const ResolvedLightOverlaySet& resolved,
	nri_scene::GeometryData& outGeometry,
	nri_scene::MaterialBridgeData& outMaterials)
{
	outGeometry = {};
	outMaterials = {};

	if (resolved.surfaceLightRules.Size() == 0)
	{
		return false;
	}

	auto resolveFixtureTexture = [](const ResolvedLightOverlaySurfaceLightRule& rule) -> FGameTexture*
	{
		if (rule.hasFixtureTexture && rule.fixtureTexture.IsNotEmpty())
		{
			FTextureID textureId = TexMan.CheckForTexture(
				rule.fixtureTexture.GetChars(),
				ETextureType::Any,
				FTextureManager::TEXMAN_TryAny | FTextureManager::TEXMAN_ForceLookup);
			if (textureId.isValid())
			{
				if (FGameTexture* texture = TexMan.GetGameTexture(textureId, true))
				{
					return texture;
				}
			}
		}
		return nullptr;
	};

	auto normalize3 = [](float vector[3]) -> bool
	{
		const float lengthSq = vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2];
		if (!std::isfinite(lengthSq) || lengthSq <= 0.000001f)
		{
			return false;
		}
		const float invLength = 1.0f / std::sqrt(lengthSq);
		vector[0] *= invLength;
		vector[1] *= invLength;
		vector[2] *= invLength;
		return true;
	};

	auto cross3 = [](const float a[3], const float b[3], float out[3])
	{
		out[0] = a[1] * b[2] - a[2] * b[1];
		out[1] = a[2] * b[0] - a[0] * b[2];
		out[2] = a[0] * b[1] - a[1] * b[0];
	};

	nri_scene::SceneView surfaceLightView = {};
	for (const auto& rule : resolved.surfaceLightRules)
	{
		if (!rule.hasPosition || !rule.hasNormal)
		{
			continue;
		}

		float normal[3] = { rule.normal[0], rule.normal[1], rule.normal[2] };
		if (!normalize3(normal))
		{
			continue;
		}

		const float worldUp[3] = { 0.0f, 1.0f, 0.0f };
		const float worldRight[3] = { 1.0f, 0.0f, 0.0f };
		float tangent[3] = {};
		cross3(worldUp, normal, tangent);
		if (!normalize3(tangent))
		{
			cross3(worldRight, normal, tangent);
			if (!normalize3(tangent))
			{
				continue;
			}
		}

		float bitangent[3] = {};
		cross3(normal, tangent, bitangent);
		if (!normalize3(bitangent))
		{
			continue;
		}
		if (rule.hasRotation && std::isfinite(rule.rotation) && rule.rotation != 0.0f)
		{
			const float radians = rule.rotation * 0.017453292519943295f;
			const float c = std::cos(radians);
			const float s = std::sin(radians);
			const float rotatedTangent[3] =
			{
				tangent[0] * c + bitangent[0] * s,
				tangent[1] * c + bitangent[1] * s,
				tangent[2] * c + bitangent[2] * s,
			};
			const float rotatedBitangent[3] =
			{
				bitangent[0] * c - tangent[0] * s,
				bitangent[1] * c - tangent[1] * s,
				bitangent[2] * c - tangent[2] * s,
			};
			tangent[0] = rotatedTangent[0];
			tangent[1] = rotatedTangent[1];
			tangent[2] = rotatedTangent[2];
			bitangent[0] = rotatedBitangent[0];
			bitangent[1] = rotatedBitangent[1];
			bitangent[2] = rotatedBitangent[2];
		}

		const float offset = rule.hasOffset ? std::max(0.0f, rule.offset) : 0.5f;
		const float halfWidth = std::max(1.0f, rule.hasSize ? rule.size[0] : 32.0f) * 0.5f;
		const float halfHeight = std::max(1.0f, rule.hasSize ? rule.size[1] : 32.0f) * 0.5f;
		const float center[3] =
		{
			rule.position[0] + normal[0] * offset,
			rule.position[1] + normal[1] * offset,
			rule.position[2] + normal[2] * offset,
		};

		auto makeVertex = [&](float tangentScale, float bitangentScale, float u, float v) -> nri_scene::CapturedVertex
		{
			nri_scene::CapturedVertex vertex = {};
			vertex.position[0] = center[0] + tangent[0] * tangentScale + bitangent[0] * bitangentScale;
			vertex.position[1] = center[1] + tangent[1] * tangentScale + bitangent[1] * bitangentScale;
			vertex.position[2] = center[2] + tangent[2] * tangentScale + bitangent[2] * bitangentScale;
			vertex.prevPosition[0] = vertex.position[0];
			vertex.prevPosition[1] = vertex.position[1];
			vertex.prevPosition[2] = vertex.position[2];
			vertex.uv[0] = u;
			vertex.uv[1] = v;
			return vertex;
		};

		nri_scene::SurfaceRef surface = {};
		surface.vertices.reserve(6u);
		const nri_scene::CapturedVertex v00 = makeVertex(-halfWidth, -halfHeight, 0.0f, 1.0f);
		const nri_scene::CapturedVertex v10 = makeVertex(halfWidth, -halfHeight, 1.0f, 1.0f);
		const nri_scene::CapturedVertex v11 = makeVertex(halfWidth, halfHeight, 1.0f, 0.0f);
		const nri_scene::CapturedVertex v01 = makeVertex(-halfWidth, halfHeight, 0.0f, 0.0f);
		surface.vertices.push_back(v00);
		surface.vertices.push_back(v10);
		surface.vertices.push_back(v11);
		surface.vertices.push_back(v00);
		surface.vertices.push_back(v11);
		surface.vertices.push_back(v01);
		surface.material.texture = resolveFixtureTexture(rule);
		surface.material.emissiveSourceTexture = surface.material.texture;
		surface.material.palette = 0;
		surface.material.shade = 0;
		surface.material.alpha = 1.0f;
		surface.material.flags = nri_scene::MaterialFlag_Fullbright | nri_scene::MaterialFlag_Flat;
		surface.provenance.sourceType = nri_scene::SurfaceSourceType::SurfaceLightOverlay;
		surface.provenance.sectorIndex = rule.hasSector ? rule.sector : -1;
		surface.provenance.wallIndex = rule.hasWall ? rule.wall : -1;
		surface.provenance.mapChunkIndex = -1;
		surface.provenance.cstat = BuildSurfaceLightRuleId(rule);
		surfaceLightView.opaqueFlats.push_back(std::move(surface));
	}

	if (surfaceLightView.opaqueFlats.empty())
	{
		return false;
	}

	RebuildSurfaceLightSceneViewStats(surfaceLightView);
	nri_scene::BuildGeometry(surfaceLightView, outGeometry);
	nri_scene::BuildMaterials(surfaceLightView, outMaterials);
	for (size_t i = 0; i < outMaterials.materials.size(); ++i)
	{
		nri_scene::MaterialData& material = outMaterials.materials[i];
		material.flags |= nri_scene::MaterialFlag_Fullbright | nri_scene::MaterialFlag_Flat;
		material.lightingFlags |=
			nri_scene::MaterialLightingFlag_MaterialFullbright |
			nri_scene::MaterialLightingFlag_NoShadowReceive |
			nri_scene::MaterialLightingFlag_NoShadowCast;
		material.lightLevel = 1.0f;
		material.emissiveMode = nri_scene::MaterialEmissiveMode_UseBaseTexture;
		material.emissiveTextureIndex = material.textureIndex;
		material.emissiveIntensity = 1.0f;
		material.emissiveColor[0] = 1.0f;
		material.emissiveColor[1] = 1.0f;
		material.emissiveColor[2] = 1.0f;
		if (i < outMaterials.lightMetadata.size())
		{
			nri_scene::MaterialLightingMetadata& metadata = outMaterials.lightMetadata[i];
			metadata.materialFlags = material.flags;
			metadata.lightingFlags |=
				nri_scene::MaterialLightingFlag_MaterialFullbright |
				nri_scene::MaterialLightingFlag_NoShadowReceive |
				nri_scene::MaterialLightingFlag_NoShadowCast;
			metadata.lightLevel = 1.0f;
			metadata.emissiveMode = nri_scene::MaterialEmissiveMode_None;
			metadata.emissiveTextureIndex = UINT32_MAX;
			metadata.emissiveIntensity = 0.0f;
			metadata.emissiveColor[0] = 1.0f;
			metadata.emissiveColor[1] = 1.0f;
			metadata.emissiveColor[2] = 1.0f;
		}
	}

	return !outGeometry.primitives.empty() && !outMaterials.materials.empty();
}
}
