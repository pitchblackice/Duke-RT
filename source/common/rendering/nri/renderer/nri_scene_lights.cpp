#include "nri_scene_lights.h"

#include "c_cvars.h"
#include "maptypes.h"
#include "palette.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

EXTERN_CVAR(Bool, nri_ptemissiveheuristics)
EXTERN_CVAR(Float, nri_ptemissiveminpower)
EXTERN_CVAR(Float, nri_ptemissiveminsurface)
EXTERN_CVAR(Bool, nri_ptsectorlighting)
EXTERN_CVAR(Float, nri_ptsectorambientscale)
EXTERN_CVAR(Float, nri_ptsectorhemiscale)
EXTERN_CVAR(Float, nri_ptsectorfogscale)
EXTERN_CVAR(Float, nri_ptsectorclamp)
EXTERN_CVAR(Int, nri_ptsectorfilterpal)
EXTERN_CVAR(Int, nri_ptsectorfilterminshade)
EXTERN_CVAR(Int, nri_ptsectorfiltermaxshade)
EXTERN_CVAR(Int, nri_ptsectorfilterlotag)
EXTERN_CVAR(Int, nri_ptsectorpulseframes)
EXTERN_CVAR(Float, nri_ptsectorpulseamount)

namespace
{
	constexpr float TwoPi = 6.28318530717958647692f;

	void Copy3f(const float* source, float* destination)
	{
		destination[0] = source[0];
		destination[1] = source[1];
		destination[2] = source[2];
	}

	void ComputeSurfaceBounds(const nri_scene::SurfaceRef& surface, float outCenter[3], float& outRadius)
	{
		outCenter[0] = 0.0f;
		outCenter[1] = 0.0f;
		outCenter[2] = 0.0f;
		outRadius = 0.0f;

		if (surface.vertices.empty())
		{
			return;
		}

		for (const nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			outCenter[0] += vertex.position[0];
			outCenter[1] += vertex.position[1];
			outCenter[2] += vertex.position[2];
		}

		const float invCount = 1.0f / (float)surface.vertices.size();
		outCenter[0] *= invCount;
		outCenter[1] *= invCount;
		outCenter[2] *= invCount;

		for (const nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			const float dx = vertex.position[0] - outCenter[0];
			const float dy = vertex.position[1] - outCenter[1];
			const float dz = vertex.position[2] - outCenter[2];
			outRadius = std::max(outRadius, std::sqrt(dx * dx + dy * dy + dz * dz));
		}
	}

	float ComputeTriangleArea(const nri_scene::CapturedVertex& a, const nri_scene::CapturedVertex& b, const nri_scene::CapturedVertex& c)
	{
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float crossX = aby * acz - abz * acy;
		const float crossY = abz * acx - abx * acz;
		const float crossZ = abx * acy - aby * acx;
		return 0.5f * std::sqrt(crossX * crossX + crossY * crossY + crossZ * crossZ);
	}

	float ComputeSurfaceArea(const nri_scene::SurfaceRef& surface)
	{
		if (surface.vertices.size() < 3)
		{
			return 0.0f;
		}

		float area = 0.0f;
		if ((surface.material.flags & nri_scene::MaterialFlag_Flat) != 0)
		{
			for (uint32_t i = 0; i + 2 < surface.vertices.size(); i += 3)
			{
				area += ComputeTriangleArea(surface.vertices[i], surface.vertices[i + 1], surface.vertices[i + 2]);
			}
		}
		else
		{
			const nri_scene::CapturedVertex& root = surface.vertices[0];
			for (uint32_t i = 1; i + 1 < surface.vertices.size(); ++i)
			{
				area += ComputeTriangleArea(root, surface.vertices[i], surface.vertices[i + 1]);
			}
		}

		return area;
	}

	uint64_t HashCombine64(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
	}

	uint64_t QuantizePositionKey(const float position[3])
	{
		const int64_t x = (int64_t)std::llround(position[0] * 16.0f);
		const int64_t y = (int64_t)std::llround(position[1] * 16.0f);
		const int64_t z = (int64_t)std::llround(position[2] * 16.0f);
		uint64_t key = 1469598103934665603ull;
		key = HashCombine64(key, (uint64_t)x);
		key = HashCombine64(key, (uint64_t)y);
		key = HashCombine64(key, (uint64_t)z);
		return key;
	}

	uint64_t BuildHeuristicStableKey(const SceneLightSystem::AnalyticLightHeuristicRule& rule, const SceneLightSystem::SurfaceRecord& record)
	{
		uint64_t key = 1469598103934665603ull;
		key = HashCombine64(key, (uint64_t)rule.ruleId);
		key = HashCombine64(key, (uint64_t)record.material.textureId);
		key = HashCombine64(key, (uint64_t)(uint32_t)record.provenance.sourceType);
		if (record.provenance.actorIndex >= 0)
		{
			key = HashCombine64(key, 0xA11C700000000000ull | (uint64_t)(uint32_t)record.provenance.actorIndex);
		}
		else
		{
			key = HashCombine64(key, record.material.materialKey);
			key = HashCombine64(key, QuantizePositionKey(record.center));
		}
		return key;
	}

	float EvaluateFlickerScale(uint64_t stableKey, uint32_t frameIndex, uint32_t flickerFrames)
	{
		if (flickerFrames <= 1)
		{
			return 1.0f;
		}

		const uint32_t seed = (uint32_t)(stableKey ^ (stableKey >> 32));
		const uint32_t phaseFrame = (frameIndex + seed) % flickerFrames;
		const float phase = ((float)phaseFrame / (float)flickerFrames) * TwoPi;
		return 0.35f + 0.65f * (0.5f + 0.5f * std::sin(phase));
	}

	float EvaluatePulseScale(uint64_t stableKey, uint32_t frameIndex, uint32_t pulseFrames, float pulseAmount)
	{
		if (pulseFrames <= 1 || pulseAmount <= 0.0f)
		{
			return 1.0f;
		}

		const float clampedAmount = std::clamp(pulseAmount, 0.0f, 0.95f);
		const float baseScale = 1.0f - clampedAmount;
		return baseScale + clampedAmount * EvaluateFlickerScale(stableKey ^ 0x5EC70B5E00000000ull, frameIndex, pulseFrames);
	}

	float ComputeColorLuminance(const float color[3])
	{
		return color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
	}

	float ComputeBuildLightLevel(int shade, int paletteIndex)
	{
		const int clampedPalette = clamp(paletteIndex, 0, MAXPALOOKUPS - 1);
		const float shadeDiv = lookups.tables[clampedPalette].ShadeFactor;
		const bool fullbright = shadeDiv < 1.0f / 1000.0f || shade < -numshades;
		if (fullbright)
		{
			return 1.0f;
		}

		float inverseLight = (float)shade * 255.0f / (float)numshades;
		inverseLight /= shadeDiv;
		const float lightLevel = clamp(255.0f - inverseLight, 0.0f, 255.0f);
		return lightLevel / 255.0f;
	}

	bool HasPalEntryColor(const PalEntry& color)
	{
		return color.r != 0 || color.g != 0 || color.b != 0;
	}

	void ResolveSectorTint(const sectortype& sec, int paletteIndex, float outTint[3], float& outFogStrength)
	{
		(void)paletteIndex;

		outTint[0] = 1.0f;
		outTint[1] = 1.0f;
		outTint[2] = 1.0f;
		outFogStrength = 0.0f;

		const float visibilityStrength = std::clamp((float)sec.visibility / 128.0f, 0.0f, 1.0f);
		const bool hasExplicitFogPalette = sec.fogpal > 0;
		outFogStrength = hasExplicitFogPalette ? std::max(visibilityStrength, 0.35f) : visibilityStrength;
		if (!hasExplicitFogPalette)
		{
			return;
		}

		PalEntry fade = {};
		fade = lookups.getFade(clamp((int)sec.fogpal, 0, MAXPALOOKUPS - 1));

		const bool hasFogTint = HasPalEntryColor(fade);
		if (!hasFogTint)
		{
			return;
		}

		const float tint[3] = {
			(float)fade.r / 255.0f,
			(float)fade.g / 255.0f,
			(float)fade.b / 255.0f,
		};
		const float tintWeight = std::clamp((hasExplicitFogPalette ? 0.20f : 0.10f) + outFogStrength * 0.35f, 0.0f, 0.65f);
		outTint[0] = 1.0f + (tint[0] - 1.0f) * tintWeight;
		outTint[1] = 1.0f + (tint[1] - 1.0f) * tintWeight;
		outTint[2] = 1.0f + (tint[2] - 1.0f) * tintWeight;
	}

	uint64_t BuildEmissiveStableKey(const SceneLightSystem::SurfaceRecord& record)
	{
		uint64_t key = 1469598103934665603ull;
		key = HashCombine64(key, record.material.materialKey);
		key = HashCombine64(key, (uint64_t)(uint32_t)record.provenance.sourceType);
		if (record.provenance.actorIndex >= 0)
		{
			key = HashCombine64(key, 0xE611551000000000ull | (uint64_t)(uint32_t)record.provenance.actorIndex);
		}
		else
		{
			key = HashCombine64(key, QuantizePositionKey(record.center));
		}
		return key;
	}

	bool EvaluateEmissiveMaterial(
		const SceneLightSystem::EmissiveSurfaceRegistry& registry,
		const nri_scene::MaterialLightingMetadata& metadata,
		uint32_t& outSourceFlags,
		uint32_t& outRuleId,
		float outColor[3],
		float& outIntensity,
		uint32_t& outMode,
		uint32_t& outTextureIndex)
	{
		outSourceFlags = SceneEmissiveSurfaceSourceFlag_None;
		outRuleId = 0;
		outColor[0] = 0.0f;
		outColor[1] = 0.0f;
		outColor[2] = 0.0f;
		outIntensity = 0.0f;
		outMode = nri_scene::MaterialEmissiveMode_None;
		outTextureIndex = UINT32_MAX;

		if (nri_ptemissiveheuristics)
		{
			if ((metadata.lightingFlags & (nri_scene::MaterialLightingFlag_MaterialFullbright | nri_scene::MaterialLightingFlag_TextureFullbright)) != 0)
			{
				outSourceFlags |= SceneEmissiveSurfaceSourceFlag_AutoFullbright;
			}
			if ((metadata.lightingFlags & (nri_scene::MaterialLightingFlag_TextureGlowing | nri_scene::MaterialLightingFlag_TextureAutoGlowing)) != 0)
			{
				outSourceFlags |= SceneEmissiveSurfaceSourceFlag_AutoTextureGlow;
			}
			if ((metadata.lightingFlags & nri_scene::MaterialLightingFlag_HasGlowmap) != 0)
			{
				outSourceFlags |= SceneEmissiveSurfaceSourceFlag_AutoGlowmap;
			}

			if (metadata.emissiveMode != nri_scene::MaterialEmissiveMode_None && metadata.emissiveIntensity > 0.0f)
			{
				outMode = metadata.emissiveMode;
				outTextureIndex = metadata.emissiveTextureIndex;
				outIntensity = metadata.emissiveIntensity;
				Copy3f(metadata.emissiveColor, outColor);
			}
		}

		for (const auto& rule : registry.textureRules)
		{
			if (metadata.textureId != rule.textureId)
			{
				continue;
			}

			outSourceFlags |= SceneEmissiveSurfaceSourceFlag_ExplicitTextureRule;
			outRuleId = rule.ruleId;
			const float baseIntensity = outIntensity > 0.0f ? outIntensity : 1.0f;
			switch (rule.emissiveMode)
			{
			case nri_scene::MaterialEmissiveMode_UseBaseTexture:
				outMode = nri_scene::MaterialEmissiveMode_UseBaseTexture;
				outTextureIndex = metadata.textureIndex;
				outColor[0] = metadata.averageColor[0];
				outColor[1] = metadata.averageColor[1];
				outColor[2] = metadata.averageColor[2];
				break;
			case nri_scene::MaterialEmissiveMode_UseGlowmapTexture:
				if (metadata.glowmapTextureIndex != UINT32_MAX)
				{
					outMode = nri_scene::MaterialEmissiveMode_UseGlowmapTexture;
					outTextureIndex = metadata.glowmapTextureIndex;
				}
				else
				{
					outMode = nri_scene::MaterialEmissiveMode_UseConstantColor;
				}
				outColor[0] = metadata.glowColor[0] > 0.0f ? metadata.glowColor[0] : metadata.averageColor[0];
				outColor[1] = metadata.glowColor[1] > 0.0f ? metadata.glowColor[1] : metadata.averageColor[1];
				outColor[2] = metadata.glowColor[2] > 0.0f ? metadata.glowColor[2] : metadata.averageColor[2];
				break;
			case nri_scene::MaterialEmissiveMode_UseConstantColor:
				outMode = nri_scene::MaterialEmissiveMode_UseConstantColor;
				if (rule.hasExplicitColor)
				{
					Copy3f(rule.emissiveColor, outColor);
				}
				else
				{
					outColor[0] = metadata.glowColor[0] > 0.0f ? metadata.glowColor[0] : metadata.averageColor[0];
					outColor[1] = metadata.glowColor[1] > 0.0f ? metadata.glowColor[1] : metadata.averageColor[1];
					outColor[2] = metadata.glowColor[2] > 0.0f ? metadata.glowColor[2] : metadata.averageColor[2];
				}
				break;
			default:
				break;
			}
			outIntensity = baseIntensity * std::max(rule.intensityScale, 0.0f);
			break;
		}

		return outMode != nri_scene::MaterialEmissiveMode_None && outIntensity > 0.0f;
	}
}

void SceneLightSystem::Reset()
{
	mAnalyticLights = {};
	mEmissiveSurfaces = {};
	mSectorLighting = {};
	mEnvironmentLighting = {};
	mSurfaceRecords.clear();
	mFrameSerial = 0;
}

void SceneLightSystem::BeginFrame(uint64_t frameSerial)
{
	mFrameSerial = frameSerial;
	mSurfaceRecords.clear();
	mAnalyticLights.matchedSurfaceCount = 0;
	mAnalyticLights.dedupedMatchCount = 0;
	mAnalyticLights.truncatedLightCount = 0;
	mAnalyticLights.topologyChanged = false;
	mEmissiveSurfaces.totalPowerEstimate = 0.0f;
	mEmissiveSurfaces.autoTaggedCount = 0;
	mEmissiveSurfaces.explicitRuleMatchCount = 0;
	mEmissiveSurfaces.truncatedSurfaceCount = 0;
	mEmissiveSurfaces.topologyChanged = false;
	mSectorLighting.eligibleSectorCount = 0;
	mSectorLighting.activeSectorCount = 0;
	mSectorLighting.fogSectorCount = 0;
	mSectorLighting.pulsingSectorCount = 0;
	mSectorLighting.topologyChanged = false;
}

void SceneLightSystem::AppendSceneView(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials, SceneLightRecordSource source, uint32_t materialIndexBase)
{
	uint32_t materialIndex = materialIndexBase;
	AppendSurfaceList(sceneView.opaqueWalls, materials, source, materialIndex);
	AppendSurfaceList(sceneView.opaqueFlats, materials, source, materialIndex);
	AppendSurfaceList(sceneView.opaqueSprites, materials, source, materialIndex);
}

void SceneLightSystem::RebuildAnalyticLights(uint32_t frameIndex, uint32_t maxActiveLights)
{
	std::vector<SceneAnalyticLight> nextLights;
	nextLights.reserve(mAnalyticLights.manualLights.size() + mAnalyticLights.spriteTileRules.size());
	std::unordered_map<uint64_t, size_t> keyToLightIndex;
	keyToLightIndex.reserve(mAnalyticLights.manualLights.size() + mAnalyticLights.spriteTileRules.size() * 4u);

	auto tryAppendLight = [this, &nextLights, &keyToLightIndex, maxActiveLights](const SceneAnalyticLight& light)
	{
		if (keyToLightIndex.find(light.stableKey) != keyToLightIndex.end())
		{
			mAnalyticLights.dedupedMatchCount++;
			return;
		}

		if (nextLights.size() >= maxActiveLights)
		{
			mAnalyticLights.truncatedLightCount++;
			return;
		}

		keyToLightIndex.emplace(light.stableKey, nextLights.size());
		nextLights.push_back(light);
	};

	for (const SceneAnalyticLight& manualLight : mAnalyticLights.manualLights)
	{
		tryAppendLight(manualLight);
	}

	for (const AnalyticLightHeuristicRule& rule : mAnalyticLights.spriteTileRules)
	{
		for (const SurfaceRecord& record : mSurfaceRecords)
		{
			if ((record.material.materialFlags & nri_scene::MaterialFlag_Sprite) == 0)
			{
				continue;
			}
			if (record.material.textureId != rule.textureId)
			{
				continue;
			}

			mAnalyticLights.matchedSurfaceCount++;

			SceneAnalyticLight light = {};
			light.stableKey = BuildHeuristicStableKey(rule, record);
			light.id = 0;
			light.sourceFlags = SceneAnalyticLightSourceFlag_SpriteTileHeuristic;
			light.sourceRuleId = rule.ruleId;
			light.source = record.source;
			light.actorIndex = record.provenance.actorIndex;
			light.textureId = record.material.textureId;
			Copy3f(record.center, light.position);
			Copy3f(rule.color, light.color);
			light.intensity = rule.intensity * EvaluateFlickerScale(light.stableKey, frameIndex, rule.flickerFrames);
			light.radius = rule.radius;
			tryAppendLight(light);
		}
	}

	std::vector<uint64_t> nextTopologyKeys;
	nextTopologyKeys.reserve(nextLights.size());
	for (const SceneAnalyticLight& light : nextLights)
	{
		nextTopologyKeys.push_back(light.stableKey);
	}
	std::sort(nextTopologyKeys.begin(), nextTopologyKeys.end());
	mAnalyticLights.topologyChanged = nextTopologyKeys != mAnalyticLights.activeTopologyKeys;
	mAnalyticLights.activeTopologyKeys = std::move(nextTopologyKeys);
	mAnalyticLights.activeLights = std::move(nextLights);
}

void SceneLightSystem::RebuildEmissiveSurfaces(uint32_t maxActiveSurfaces)
{
	mEmissiveSurfaces.totalPowerEstimate = 0.0f;
	mEmissiveSurfaces.autoTaggedCount = 0;
	mEmissiveSurfaces.explicitRuleMatchCount = 0;
	mEmissiveSurfaces.truncatedSurfaceCount = 0;

	std::vector<EmissiveSurfaceRegistry::EmissiveSurfaceRecord> nextSurfaces;
	nextSurfaces.reserve(std::min<uint32_t>((uint32_t)mSurfaceRecords.size(), maxActiveSurfaces));

	const float minSurfaceArea = std::max((float)nri_ptemissiveminsurface, 0.0f);
	const float minPower = std::max((float)nri_ptemissiveminpower, 0.0f);

	for (const SurfaceRecord& record : mSurfaceRecords)
	{
		uint32_t sourceFlags = SceneEmissiveSurfaceSourceFlag_None;
		uint32_t sourceRuleId = 0;
		float emissiveColor[3] = {};
		float emissiveIntensity = 0.0f;
		uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_None;
		uint32_t emissiveTextureIndex = UINT32_MAX;
		if (!EvaluateEmissiveMaterial(mEmissiveSurfaces, record.material, sourceFlags, sourceRuleId, emissiveColor, emissiveIntensity, emissiveMode, emissiveTextureIndex))
		{
			continue;
		}

		if (record.surfaceArea < minSurfaceArea)
		{
			continue;
		}

		const float resolvedLuminance = emissiveMode == nri_scene::MaterialEmissiveMode_UseBaseTexture ?
			ComputeColorLuminance(record.material.averageColor) :
			ComputeColorLuminance(emissiveColor);
		const float powerEstimate = record.surfaceArea * resolvedLuminance * emissiveIntensity;
		if (powerEstimate < minPower)
		{
			continue;
		}

		if (nextSurfaces.size() >= maxActiveSurfaces)
		{
			mEmissiveSurfaces.truncatedSurfaceCount++;
			continue;
		}

		EmissiveSurfaceRegistry::EmissiveSurfaceRecord emissive = {};
		emissive.stableKey = BuildEmissiveStableKey(record);
		emissive.sourceFlags = sourceFlags;
		emissive.sourceRuleId = sourceRuleId;
		emissive.source = record.source;
		emissive.actorIndex = record.provenance.actorIndex;
		emissive.textureId = record.material.textureId;
		emissive.emissiveTextureIndex = emissiveTextureIndex;
		emissive.materialIndex = record.materialIndex;
		emissive.emissiveMode = emissiveMode;
		emissive.surfaceArea = record.surfaceArea;
		emissive.boundsRadius = record.boundsRadius;
		emissive.powerEstimate = powerEstimate;
		Copy3f(record.center, emissive.center);
		Copy3f(emissiveColor, emissive.emissiveColor);
		emissive.emissiveIntensity = emissiveIntensity;
		nextSurfaces.push_back(emissive);

		if ((sourceFlags & (SceneEmissiveSurfaceSourceFlag_AutoFullbright | SceneEmissiveSurfaceSourceFlag_AutoTextureGlow | SceneEmissiveSurfaceSourceFlag_AutoGlowmap)) != 0)
		{
			mEmissiveSurfaces.autoTaggedCount++;
		}
		if ((sourceFlags & SceneEmissiveSurfaceSourceFlag_ExplicitTextureRule) != 0)
		{
			mEmissiveSurfaces.explicitRuleMatchCount++;
		}
		mEmissiveSurfaces.totalPowerEstimate += powerEstimate;
	}

	std::vector<uint64_t> nextTopologyKeys;
	nextTopologyKeys.reserve(nextSurfaces.size());
	for (const auto& emissive : nextSurfaces)
	{
		nextTopologyKeys.push_back(emissive.stableKey);
	}
	std::sort(nextTopologyKeys.begin(), nextTopologyKeys.end());
	mEmissiveSurfaces.topologyChanged = nextTopologyKeys != mEmissiveSurfaces.activeTopologyKeys;
	mEmissiveSurfaces.activeTopologyKeys = std::move(nextTopologyKeys);
	mEmissiveSurfaces.activeSurfaces = std::move(nextSurfaces);
}

void SceneLightSystem::RebuildSectorLighting(uint32_t frameIndex, uint32_t sectorCount)
{
	mSectorLighting.sectorCount = sectorCount;
	mSectorLighting.eligibleSectorCount = 0;
	mSectorLighting.activeSectorCount = 0;
	mSectorLighting.fogSectorCount = 0;
	mSectorLighting.pulsingSectorCount = 0;
	mSectorLighting.sectors.assign(sectorCount, {});

	std::vector<uint8_t> seenSectors(sectorCount, 0u);
	for (const SurfaceRecord& record : mSurfaceRecords)
	{
		if (record.provenance.sectorIndex < 0)
		{
			continue;
		}

		const uint32_t sectorIndex = (uint32_t)record.provenance.sectorIndex;
		if (sectorIndex >= sectorCount)
		{
			continue;
		}

		seenSectors[sectorIndex] = 1u;
	}

	mSectorLighting.activeSectorIndices.clear();
	mSectorLighting.activeSectorIndices.reserve(sectorCount);

	if (!nri_ptsectorlighting || sectorCount == 0)
	{
		mSectorLighting.topologyChanged = !mSectorLighting.activeTopologyKeys.empty();
		mSectorLighting.activeTopologyKeys.clear();
		return;
	}

	const int paletteFilter = (int)nri_ptsectorfilterpal;
	const int minShadeFilter = (int)nri_ptsectorfilterminshade;
	const int maxShadeFilter = std::max(minShadeFilter, (int)nri_ptsectorfiltermaxshade);
		const int lotagFilter = (int)nri_ptsectorfilterlotag;
		const uint32_t pulseFrames = std::max(0, (int)nri_ptsectorpulseframes);
		const float pulseAmount = std::max(0.0f, (float)nri_ptsectorpulseamount);
		const bool pulseSelectionFiltered =
			paletteFilter >= 0 ||
			lotagFilter >= 0 ||
			minShadeFilter > -128 ||
			maxShadeFilter < 127;
		const float ambientScale = std::max(0.0f, (float)nri_ptsectorambientscale);
		const float hemisphereScale = std::max(0.0f, (float)nri_ptsectorhemiscale);
		const float fogScale = std::max(0.0f, (float)nri_ptsectorfogscale);
	const float sectorClamp = std::max(0.0f, (float)nri_ptsectorclamp);

	for (uint32_t sectorIndex = 0; sectorIndex < sectorCount; ++sectorIndex)
	{
		if (sectorIndex >= seenSectors.size() || seenSectors[sectorIndex] == 0u)
		{
			continue;
		}

		mSectorLighting.eligibleSectorCount++;

		const auto& sec = sector[sectorIndex];
		const int resolvedPalette = sec.floorpal != 0 ? (int)sec.floorpal : (int)sec.ceilingpal;
		const int averageShade = ((int)sec.floorshade + (int)sec.ceilingshade) / 2;
		if ((paletteFilter >= 0 && resolvedPalette != paletteFilter) ||
			(averageShade < minShadeFilter || averageShade > maxShadeFilter) ||
			(lotagFilter >= 0 && sec.lotag != lotagFilter))
		{
			continue;
		}

		const float lightLevel = ComputeBuildLightLevel(averageShade, resolvedPalette);
		const float floorLight = ComputeBuildLightLevel((int)sec.floorshade, resolvedPalette);
		const float ceilingLight = ComputeBuildLightLevel((int)sec.ceilingshade, resolvedPalette);
		const float hemisphereBias = clamp(ceilingLight - floorLight, -1.0f, 1.0f);
		float tint[3] = {};
		float fogStrength = 0.0f;
		ResolveSectorTint(sec, resolvedPalette, tint, fogStrength);
		const bool sectorPulseEnabled = pulseSelectionFiltered && pulseFrames > 1 && pulseAmount > 0.0f;
		const float pulseScale = sectorPulseEnabled ? EvaluatePulseScale(0x5EC70B5E00000000ull ^ (uint64_t)sectorIndex, frameIndex, pulseFrames, pulseAmount) : 1.0f;
		const float clampedAmbient = std::min(sectorClamp, ambientScale * (0.10f + lightLevel * 0.55f) * pulseScale);
		const float clampedHemisphere = std::min(sectorClamp, hemisphereScale * (0.08f + (0.5f + 0.5f * std::abs(hemisphereBias)) * 0.45f) * pulseScale);
		const float clampedFog = std::min(sectorClamp, fogScale * fogStrength * pulseScale);
		if (clampedAmbient <= 0.0f && clampedHemisphere <= 0.0f && clampedFog <= 0.0f)
		{
			continue;
		}

		SectorLightingRegistry::SectorLightRecord entry = {};
		entry.sectorIndex = sectorIndex;
		entry.sourceFlags = SceneSectorLightSourceFlag_Heuristic;
		if (paletteFilter >= 0)
		{
			entry.sourceFlags |= SceneSectorLightSourceFlag_PaletteFilter;
		}
		if (lotagFilter >= 0)
		{
			entry.sourceFlags |= SceneSectorLightSourceFlag_LotagFilter;
		}
		if (fogStrength > 0.0f)
		{
			entry.sourceFlags |= SceneSectorLightSourceFlag_FogPresent;
			mSectorLighting.fogSectorCount++;
		}
		if (sectorPulseEnabled)
		{
			entry.sourceFlags |= SceneSectorLightSourceFlag_Pulsing;
			mSectorLighting.pulsingSectorCount++;
		}

		entry.paletteIndex = resolvedPalette;
		entry.lotag = sec.lotag;
		entry.hitag = sec.hitag;
		entry.averageShade = averageShade;
		entry.ambientColor[0] = tint[0];
		entry.ambientColor[1] = tint[1];
		entry.ambientColor[2] = tint[2];
		entry.ambientIntensity = clampedAmbient;
		entry.hemisphereAmount = hemisphereBias * clampedHemisphere;
		entry.fogAmount = clampedFog;
		entry.pulseScale = pulseScale;

		mSectorLighting.sectors[sectorIndex] = entry;
		mSectorLighting.activeSectorIndices.push_back(sectorIndex);
	}

	mSectorLighting.activeSectorCount = (uint32_t)mSectorLighting.activeSectorIndices.size();
	std::vector<uint32_t> nextTopologyKeys = mSectorLighting.activeSectorIndices;
	std::sort(nextTopologyKeys.begin(), nextTopologyKeys.end());
	mSectorLighting.topologyChanged = nextTopologyKeys != mSectorLighting.activeTopologyKeys;
	mSectorLighting.activeTopologyKeys = std::move(nextTopologyKeys);
}

bool SceneLightSystem::AddManualAnalyticLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius)
{
	if (position == nullptr || color == nullptr || intensity <= 0.0f || radius <= 0.0f)
	{
		return false;
	}

	SceneAnalyticLight light = {};
	light.id = id;
	light.stableKey = 0x4d414e55414c0000ull | (uint64_t)id;
	light.sourceFlags = SceneAnalyticLightSourceFlag_Manual;
	light.textureId = 0;
	light.position[0] = position[0];
	light.position[1] = position[1];
	light.position[2] = position[2];
	light.color[0] = std::max(color[0], 0.0f);
	light.color[1] = std::max(color[1], 0.0f);
	light.color[2] = std::max(color[2], 0.0f);
	light.intensity = intensity;
	light.radius = radius;
	mAnalyticLights.manualLights.push_back(light);
	return true;
}

bool SceneLightSystem::RemoveManualAnalyticLight(uint32_t id)
{
	const auto it = std::find_if(mAnalyticLights.manualLights.begin(), mAnalyticLights.manualLights.end(), [id](const SceneAnalyticLight& light)
	{
		return light.id == id;
	});
	if (it == mAnalyticLights.manualLights.end())
	{
		return false;
	}

	mAnalyticLights.manualLights.erase(it);
	return true;
}

void SceneLightSystem::ClearManualAnalyticLights()
{
	mAnalyticLights.manualLights.clear();
}

bool SceneLightSystem::AddSpriteTileHeuristic(uint32_t textureId, const float color[3], float intensity, float radius, uint32_t flickerFrames, uint32_t& outRuleId)
{
	if (color == nullptr || intensity <= 0.0f || radius <= 0.0f)
	{
		return false;
	}

	AnalyticLightHeuristicRule rule = {};
	rule.ruleId = mAnalyticLights.nextRuleId++;
	rule.textureId = textureId;
	rule.color[0] = std::max(color[0], 0.0f);
	rule.color[1] = std::max(color[1], 0.0f);
	rule.color[2] = std::max(color[2], 0.0f);
	rule.intensity = intensity;
	rule.radius = radius;
	rule.flickerFrames = flickerFrames;
	mAnalyticLights.spriteTileRules.push_back(rule);
	outRuleId = rule.ruleId;
	return true;
}

void SceneLightSystem::ClearSpriteTileHeuristics()
{
	mAnalyticLights.spriteTileRules.clear();
}

bool SceneLightSystem::AddTextureEmissiveHeuristic(uint32_t textureId, uint32_t emissiveMode, float intensityScale, const float* emissiveColor, bool hasExplicitColor, uint32_t& outRuleId)
{
	if (textureId == 0 || intensityScale <= 0.0f || emissiveMode == nri_scene::MaterialEmissiveMode_None)
	{
		return false;
	}

	EmissiveSurfaceRegistry::EmissiveHeuristicRule rule = {};
	rule.ruleId = mEmissiveSurfaces.nextRuleId++;
	rule.textureId = textureId;
	rule.emissiveMode = emissiveMode;
	rule.intensityScale = intensityScale;
	rule.hasExplicitColor = hasExplicitColor && emissiveColor != nullptr;
	if (rule.hasExplicitColor)
	{
		rule.emissiveColor[0] = std::max(emissiveColor[0], 0.0f);
		rule.emissiveColor[1] = std::max(emissiveColor[1], 0.0f);
		rule.emissiveColor[2] = std::max(emissiveColor[2], 0.0f);
	}
	mEmissiveSurfaces.textureRules.push_back(rule);
	mEmissiveSurfaces.materialsDirty = true;
	outRuleId = rule.ruleId;
	return true;
}

void SceneLightSystem::ClearTextureEmissiveHeuristics()
{
	if (mEmissiveSurfaces.textureRules.empty())
	{
		return;
	}

	mEmissiveSurfaces.textureRules.clear();
	mEmissiveSurfaces.materialsDirty = true;
}

bool SceneLightSystem::ApplyEmissiveMaterialSettings(const nri_scene::MaterialLightingMetadata& metadata, nri_scene::MaterialData& inOutMaterial) const
{
	uint32_t sourceFlags = SceneEmissiveSurfaceSourceFlag_None;
	uint32_t sourceRuleId = 0;
	float emissiveColor[3] = {};
	float emissiveIntensity = 0.0f;
	uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_None;
	uint32_t emissiveTextureIndex = UINT32_MAX;
	if (!EvaluateEmissiveMaterial(mEmissiveSurfaces, metadata, sourceFlags, sourceRuleId, emissiveColor, emissiveIntensity, emissiveMode, emissiveTextureIndex))
	{
		inOutMaterial.emissiveColor[0] = 0.0f;
		inOutMaterial.emissiveColor[1] = 0.0f;
		inOutMaterial.emissiveColor[2] = 0.0f;
		inOutMaterial.emissiveIntensity = 0.0f;
		inOutMaterial.emissiveMaskScale = 0.0f;
		inOutMaterial.emissiveMode = nri_scene::MaterialEmissiveMode_None;
		inOutMaterial.emissiveTextureIndex = UINT32_MAX;
		return false;
	}

	inOutMaterial.emissiveColor[0] = emissiveColor[0];
	inOutMaterial.emissiveColor[1] = emissiveColor[1];
	inOutMaterial.emissiveColor[2] = emissiveColor[2];
	inOutMaterial.emissiveIntensity = emissiveIntensity;
	inOutMaterial.emissiveMaskScale = metadata.emissiveMaskScale > 0.0f ? metadata.emissiveMaskScale : 1.0f;
	inOutMaterial.emissiveMode = emissiveMode;
	inOutMaterial.emissiveTextureIndex = emissiveTextureIndex;
	if (inOutMaterial.materialClass != 3u)
	{
		inOutMaterial.materialClass = 2u;
	}
	return true;
}

bool SceneLightSystem::ConsumeAnalyticLightTopologyChanged()
{
	const bool changed = mAnalyticLights.topologyChanged;
	mAnalyticLights.topologyChanged = false;
	return changed;
}

bool SceneLightSystem::ConsumeEmissiveSurfaceTopologyChanged()
{
	const bool changed = mEmissiveSurfaces.topologyChanged;
	mEmissiveSurfaces.topologyChanged = false;
	return changed;
}

bool SceneLightSystem::ConsumeEmissiveMaterialsDirty()
{
	const bool dirty = mEmissiveSurfaces.materialsDirty;
	mEmissiveSurfaces.materialsDirty = false;
	return dirty;
}

bool SceneLightSystem::ConsumeSectorLightingTopologyChanged()
{
	const bool changed = mSectorLighting.topologyChanged;
	mSectorLighting.topologyChanged = false;
	return changed;
}

void SceneLightSystem::AppendSurfaceList(const std::vector<nri_scene::SurfaceRef>& surfaces, const nri_scene::MaterialBridgeData& materials, SceneLightRecordSource source, uint32_t& inOutMaterialIndex)
{
	for (const nri_scene::SurfaceRef& surface : surfaces)
	{
		SurfaceRecord record = {};
		record.source = source;
		record.materialIndex = inOutMaterialIndex;
		record.provenance = surface.provenance;
		ComputeSurfaceBounds(surface, record.center, record.boundsRadius);
		record.surfaceArea = ComputeSurfaceArea(surface);

		if (inOutMaterialIndex < materials.lightMetadata.size())
		{
			record.material = materials.lightMetadata[inOutMaterialIndex];
		}
		else if (inOutMaterialIndex < materials.materials.size())
		{
			record.material.sectorIndex = materials.materials[inOutMaterialIndex].sectorIndex != UINT32_MAX ? (int32_t)materials.materials[inOutMaterialIndex].sectorIndex : -1;
			record.material.paletteIndex = materials.materials[inOutMaterialIndex].paletteIndex;
			record.material.materialFlags = materials.materials[inOutMaterialIndex].flags;
			record.material.alpha = materials.materials[inOutMaterialIndex].alpha;
			record.material.lightLevel = materials.materials[inOutMaterialIndex].lightLevel;
		}

		mSurfaceRecords.push_back(record);
		++inOutMaterialIndex;
	}
}
