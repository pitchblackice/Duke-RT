#include "nri_light_rule_product_cache.h"

#include "../scene/nri_hash.h"

#include <cctype>
#include <cmath>
#include <string>

namespace
{
	uint64_t HashLightOverlayText(uint64_t hash, const char* text)
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

	uint32_t BuildResolvedLightOverlayRuleId(
		const char* id,
		const char* classOrMapName,
		const LightOverlaySourceLocation& source)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashLightOverlayText(hash, id);
		hash = HashLightOverlayText(hash, classOrMapName);
		hash = HashLightOverlayText(hash, source.sourceName.GetChars());
		hash ^= (uint64_t)source.orderIndex + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		const uint32_t ruleId = (uint32_t)(hash ^ (hash >> 32));
		return ruleId != 0 ? ruleId : 1u;
	}

	uint32_t BuildMapOverlayRuleId(const ResolvedLightOverlayMapLightRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.mapName.GetChars(), rule.source);
	}

	uint32_t BuildSurfaceLightRuleId(const ResolvedLightOverlaySurfaceLightRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.mapName.GetChars(), rule.source);
	}

	uint32_t BuildEmissiveOverrideRuleId(const ResolvedLightOverlayEmissiveOverrideRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.mapName.GetChars(), rule.source);
	}

	std::string NormalizeLightOverlayTextureSelector(const char* value)
	{
		std::string normalized = value != nullptr ? value : "";
		for (char& c : normalized)
		{
			c = (char)std::tolower((unsigned char)c);
		}

		const size_t slash = normalized.find_last_of("/\\");
		const size_t dot = normalized.find_last_of('.');
		if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
		{
			normalized.erase(dot);
		}
		return normalized;
	}

	uint64_t QuantizeLightOverlayPositionKey(const float position[3])
	{
		const int64_t x = (int64_t)std::llround(position[0] * 16.0f);
		const int64_t y = (int64_t)std::llround(position[1] * 16.0f);
		const int64_t z = (int64_t)std::llround(position[2] * 16.0f);
		uint64_t key = 1469598103934665603ull;
		key = nri_scene::HashCombine64(key, (uint64_t)x);
		key = nri_scene::HashCombine64(key, (uint64_t)y);
		key = nri_scene::HashCombine64(key, (uint64_t)z);
		return key;
	}

	uint64_t BuildMapOverlayStableKey(uint32_t ruleId, const float position[3])
	{
		uint64_t key = 1469598103934665603ull;
		key = nri_scene::HashCombine64(key, (uint64_t)ruleId);
		key = nri_scene::HashCombine64(key, QuantizeLightOverlayPositionKey(position));
		return key;
	}

	void ConvertMapOverlayWorldVectorToPathTracing(const float source[3], float destination[3])
	{
		destination[0] = source[0];
		destination[1] = -source[2];
		destination[2] = -source[1];
	}

	void ComputeCapturedSurfaceCenter(const nri_scene::SurfaceRef& surface, float outCenter[3])
	{
		outCenter[0] = 0.0f;
		outCenter[1] = 0.0f;
		outCenter[2] = 0.0f;
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
	}

	bool TryResolveSectorMapOverlayAnchorPosition(
		const nri_scene::PTMapWorld& mapWorld,
		int32_t sectorIndex,
		float outPosition[3])
	{
		const nri_scene::PTMapChunk* matchedChunk = nullptr;
		for (const auto& chunk : mapWorld.chunks)
		{
			if (chunk.sectorIndex == sectorIndex)
			{
				matchedChunk = &chunk;
				break;
			}
		}
		if (matchedChunk == nullptr)
		{
			return false;
		}

		float flatCenterSum[3] = {};
		int flatCenterCount = 0;
		float anyCenterSum[3] = {};
		int anyCenterCount = 0;
		const uint32_t endSurface = matchedChunk->firstSurface + matchedChunk->surfaceCount;
		for (uint32_t surfaceIndex = matchedChunk->firstSurface;
			surfaceIndex < endSurface && surfaceIndex < mapWorld.surfaces.size();
			++surfaceIndex)
		{
			const auto& surface = mapWorld.surfaces[surfaceIndex].surface;
			if (surface.provenance.sectorIndex != sectorIndex)
			{
				continue;
			}

			float center[3] = {};
			ComputeCapturedSurfaceCenter(surface, center);
			anyCenterSum[0] += center[0];
			anyCenterSum[1] += center[1];
			anyCenterSum[2] += center[2];
			anyCenterCount++;

			if (surface.provenance.sourceType == nri_scene::SurfaceSourceType::MapFloorSection ||
				surface.provenance.sourceType == nri_scene::SurfaceSourceType::MapCeilingSection)
			{
				flatCenterSum[0] += center[0];
				flatCenterSum[1] += center[1];
				flatCenterSum[2] += center[2];
				flatCenterCount++;
			}
		}

		const float* sum = flatCenterCount > 0 ? flatCenterSum : anyCenterSum;
		const int count = flatCenterCount > 0 ? flatCenterCount : anyCenterCount;
		if (count <= 0)
		{
			return false;
		}

		const float invCount = 1.0f / (float)count;
		outPosition[0] = sum[0] * invCount;
		outPosition[1] = sum[1] * invCount;
		outPosition[2] = sum[2] * invCount;
		return true;
	}

	bool TryResolveWallMapOverlayAnchorPosition(
		const nri_scene::PTMapWorld& mapWorld,
		int32_t wallIndex,
		float outPosition[3])
	{
		float centerSum[3] = {};
		int centerCount = 0;
		for (const auto& mapSurface : mapWorld.surfaces)
		{
			if (mapSurface.surface.provenance.wallIndex != wallIndex)
			{
				continue;
			}

			float center[3] = {};
			ComputeCapturedSurfaceCenter(mapSurface.surface, center);
			centerSum[0] += center[0];
			centerSum[1] += center[1];
			centerSum[2] += center[2];
			centerCount++;
		}

		if (centerCount <= 0)
		{
			return false;
		}

		const float invCount = 1.0f / (float)centerCount;
		outPosition[0] = centerSum[0] * invCount;
		outPosition[1] = centerSum[1] * invCount;
		outPosition[2] = centerSum[2] * invCount;
		return true;
	}

	bool TryResolveMapOverlayAnchorPosition(
		const nri_scene::PTMapWorld& mapWorld,
		const ResolvedLightOverlayMapLightRule& rule,
		float outPosition[3])
	{
		switch (rule.anchorType)
		{
		case LightOverlayAnchorType::Position:
			if (!rule.hasAnchorPosition)
			{
				return false;
			}
			ConvertMapOverlayWorldVectorToPathTracing(rule.anchorPosition, outPosition);
			return true;

		case LightOverlayAnchorType::Sector:
			return rule.anchorIndex >= 0 &&
				TryResolveSectorMapOverlayAnchorPosition(mapWorld, rule.anchorIndex, outPosition);

		case LightOverlayAnchorType::Wall:
			return rule.anchorIndex >= 0 &&
				TryResolveWallMapOverlayAnchorPosition(mapWorld, rule.anchorIndex, outPosition);

		default:
			return false;
		}
	}

	bool TryBuildMapOverlayRule(
		const nri_scene::PTMapWorld& mapWorld,
		const ResolvedLightOverlayMapLightRule& resolvedRule,
		SceneLightSystem::AnalyticLightRegistry::MapOverlayRule& overlayRule)
	{
		const bool supported = resolvedRule.lightType.IsEmpty() || resolvedRule.lightType.CompareNoCase("point") == 0;
		if (!supported || resolvedRule.intensity <= 0.0f || resolvedRule.radius <= 0.0f)
		{
			return false;
		}

		float anchorPosition[3] = {};
		if (!TryResolveMapOverlayAnchorPosition(mapWorld, resolvedRule, anchorPosition))
		{
			return false;
		}

		float offset[3] = {};
		ConvertMapOverlayWorldVectorToPathTracing(resolvedRule.offset, offset);
		overlayRule.ruleId = BuildMapOverlayRuleId(resolvedRule);
		overlayRule.source = SceneLightRecordSource::StaticMapScene;
		overlayRule.position[0] = anchorPosition[0] + offset[0];
		overlayRule.position[1] = anchorPosition[1] + offset[1];
		overlayRule.position[2] = anchorPosition[2] + offset[2];
		overlayRule.stableKey = BuildMapOverlayStableKey(overlayRule.ruleId, overlayRule.position);
		overlayRule.color[0] = resolvedRule.color[0];
		overlayRule.color[1] = resolvedRule.color[1];
		overlayRule.color[2] = resolvedRule.color[2];
		overlayRule.intensity = resolvedRule.intensity;
		overlayRule.radius = resolvedRule.radius;
		overlayRule.flickerFrames = resolvedRule.flickerFrames;
		return true;
	}

	bool TryBuildSurfaceLightAnalyticRule(
		const ResolvedLightOverlaySurfaceLightRule& resolvedRule,
		SceneLightSystem::AnalyticLightRegistry::MapOverlayRule& overlayRule)
	{
		const bool supported = resolvedRule.lightType.IsEmpty() ||
			resolvedRule.lightType.CompareNoCase("point") == 0 ||
			resolvedRule.lightType.CompareNoCase("rect") == 0;
		if (!supported ||
			!resolvedRule.hasPosition ||
			!resolvedRule.hasNormal ||
			resolvedRule.intensity <= 0.0f ||
			resolvedRule.radius <= 0.0f)
		{
			return false;
		}

		const float offset = resolvedRule.hasOffset ? resolvedRule.offset : 0.0f;
		overlayRule.ruleId = BuildSurfaceLightRuleId(resolvedRule);
		overlayRule.source = SceneLightRecordSource::DynamicScene;
		overlayRule.position[0] = resolvedRule.position[0] + resolvedRule.normal[0] * offset;
		overlayRule.position[1] = resolvedRule.position[1] + resolvedRule.normal[1] * offset;
		overlayRule.position[2] = resolvedRule.position[2] + resolvedRule.normal[2] * offset;
		overlayRule.stableKey = BuildMapOverlayStableKey(overlayRule.ruleId, overlayRule.position);
		overlayRule.color[0] = resolvedRule.color[0];
		overlayRule.color[1] = resolvedRule.color[1];
		overlayRule.color[2] = resolvedRule.color[2];
		overlayRule.intensity = resolvedRule.intensity;
		overlayRule.radius = resolvedRule.radius;
		overlayRule.hasSectorResponse = resolvedRule.hasSectorResponse;
		overlayRule.sectorResponse = resolvedRule.sectorResponse;
		overlayRule.hasSignalSector = resolvedRule.hasSignalSector;
		overlayRule.signalSector = resolvedRule.signalSector;
		overlayRule.hasResponseIntensity = resolvedRule.hasResponseIntensity;
		overlayRule.responseIntensity = resolvedRule.responseIntensity;
		overlayRule.hasResponseMin = resolvedRule.hasResponseMin;
		overlayRule.responseMin = resolvedRule.responseMin;
		overlayRule.hasResponseMax = resolvedRule.hasResponseMax;
		overlayRule.responseMax = resolvedRule.responseMax;
		overlayRule.hasResponseInputMin = resolvedRule.hasResponseInputMin;
		overlayRule.responseInputMin = resolvedRule.responseInputMin;
		overlayRule.hasResponseInputMax = resolvedRule.hasResponseInputMax;
		overlayRule.responseInputMax = resolvedRule.responseInputMax;
		return true;
	}

	void BuildStaticMapAnalyticOverlayRules(
		const ResolvedLightOverlaySet& resolved,
		const nri_scene::PTMapWorld& mapWorld,
		std::vector<SceneLightSystem::AnalyticLightRegistry::MapOverlayRule>& outRules)
	{
		outRules.clear();
		outRules.reserve((size_t)resolved.mapLightRules.Size() + (size_t)resolved.surfaceLightRules.Size());
		nri_light_rule_product_cache::AppendAcceptedProductsInSourceOrder(
			resolved.mapLightRules,
			outRules,
			[&mapWorld](const auto& rule, auto& product)
			{
				return TryBuildMapOverlayRule(mapWorld, rule, product);
			});
		nri_light_rule_product_cache::AppendAcceptedProductsInSourceOrder(
			resolved.surfaceLightRules,
			outRules,
			[](const auto& rule, auto& product)
			{
				return TryBuildSurfaceLightAnalyticRule(rule, product);
			});
	}

	bool TryBuildEmissiveOverrideRule(
		const ResolvedLightOverlayEmissiveOverrideRule& resolvedRule,
		SceneLightSystem::EmissiveOverrideRule& rule)
	{
		if (!resolvedRule.hasSectorFilter && !resolvedRule.hasWallFilter && !resolvedRule.hasTileFilter)
		{
			return false;
		}

		rule.ruleId = BuildEmissiveOverrideRuleId(resolvedRule);
		rule.hasSectorFilter = resolvedRule.hasSectorFilter;
		rule.sectorFilter = resolvedRule.sectorFilter;
		rule.hasWallFilter = resolvedRule.hasWallFilter;
		rule.wallFilter = resolvedRule.wallFilter;
		rule.hasTileFilter = resolvedRule.hasTileFilter && resolvedRule.tileFilter >= 0;
		rule.tileFilter = rule.hasTileFilter ? (uint32_t)resolvedRule.tileFilter : 0u;
		rule.hasIntensityScale = resolvedRule.hasIntensityScale;
		rule.intensityScale = resolvedRule.intensityScale;
		rule.hasReachScale = resolvedRule.hasReachScale;
		rule.reachScale = resolvedRule.reachScale;
		rule.hasSectorResponse = resolvedRule.hasSectorResponse;
		rule.sectorResponse = resolvedRule.sectorResponse;
		rule.hasSignalSector = resolvedRule.hasSignalSector && resolvedRule.signalSector >= 0;
		rule.signalSector = rule.hasSignalSector ? resolvedRule.signalSector : -1;
		rule.hasResponseIntensity = resolvedRule.hasResponseIntensity;
		rule.responseIntensity = resolvedRule.responseIntensity;
		rule.hasResponseMin = resolvedRule.hasResponseMin;
		rule.responseMin = resolvedRule.responseMin;
		rule.hasResponseMax = resolvedRule.hasResponseMax;
		rule.responseMax = resolvedRule.responseMax;
		rule.hasResponseInputMin = resolvedRule.hasResponseInputMin;
		rule.responseInputMin = resolvedRule.responseInputMin;
		rule.hasResponseInputMax = resolvedRule.hasResponseInputMax;
		rule.responseInputMax = resolvedRule.responseInputMax;
		rule.hasResponseIntensityMin = resolvedRule.hasResponseIntensityMin;
		rule.responseIntensityMin = resolvedRule.responseIntensityMin;
		rule.hasResponseIntensityMax = resolvedRule.hasResponseIntensityMax;
		rule.responseIntensityMax = resolvedRule.responseIntensityMax;
		rule.hasResponseReachMin = resolvedRule.hasResponseReachMin;
		rule.responseReachMin = resolvedRule.responseReachMin;
		rule.hasResponseReachMax = resolvedRule.hasResponseReachMax;
		rule.responseReachMax = resolvedRule.responseReachMax;
		rule.hasMaterialResponse = resolvedRule.hasMaterialResponse;
		rule.materialResponse = resolvedRule.materialResponse;
		rule.hasMaterialResponseMin = resolvedRule.hasMaterialResponseMin;
		rule.materialResponseMin = resolvedRule.materialResponseMin;
		rule.hasMaterialResponseMax = resolvedRule.hasMaterialResponseMax;
		rule.materialResponseMax = resolvedRule.materialResponseMax;
		return true;
	}

	bool TryBuildSurfaceLightFixtureResponseRule(
		const ResolvedLightOverlaySurfaceLightRule& resolvedRule,
		SceneLightSystem::EmissiveOverrideRule& rule)
	{
		if (!resolvedRule.hasPosition || !resolvedRule.hasNormal)
		{
			return false;
		}

		const bool sectorResponseEnabled = resolvedRule.hasSectorResponse && resolvedRule.sectorResponse;
		rule.ruleId = BuildSurfaceLightRuleId(resolvedRule);
		rule.hasSectorResponse = true;
		rule.sectorResponse = sectorResponseEnabled;
		rule.hasSignalSector = resolvedRule.hasSignalSector && resolvedRule.signalSector >= 0;
		rule.signalSector = rule.hasSignalSector ? resolvedRule.signalSector : -1;
		rule.hasResponseIntensity = resolvedRule.hasResponseIntensity;
		rule.responseIntensity = resolvedRule.responseIntensity;
		rule.hasResponseMin = resolvedRule.hasResponseMin;
		rule.responseMin = resolvedRule.responseMin;
		rule.hasResponseMax = resolvedRule.hasResponseMax;
		rule.responseMax = resolvedRule.responseMax;
		rule.hasResponseInputMin = resolvedRule.hasResponseInputMin;
		rule.responseInputMin = resolvedRule.responseInputMin;
		rule.hasResponseInputMax = resolvedRule.hasResponseInputMax;
		rule.responseInputMax = resolvedRule.responseInputMax;
		if (resolvedRule.fixtureMaterialResponse && sectorResponseEnabled)
		{
			rule.hasMaterialResponse = true;
			rule.materialResponse = true;
			rule.hasMaterialResponseMin = resolvedRule.hasMaterialResponseMin;
			rule.materialResponseMin = resolvedRule.materialResponseMin;
			rule.hasMaterialResponseMax = resolvedRule.hasMaterialResponseMax;
			rule.materialResponseMax = resolvedRule.materialResponseMax;
		}
		return true;
	}

	bool TryBuildEmissiveMaterialResponseRule(
		const ResolvedLightOverlayEmissiveMaterialResponseRule& resolvedRule,
		SceneLightSystem::EmissiveMaterialResponseRule& rule)
	{
		rule.ruleId = BuildResolvedLightOverlayRuleId(resolvedRule.id.GetChars(), "", resolvedRule.source);
		rule.textureIds.reserve((size_t)resolvedRule.tileFilters.Size() + (size_t)resolvedRule.textureNames.Size());
		for (int tile : resolvedRule.tileFilters)
		{
			if (tile >= 0)
			{
				rule.textureIds.push_back((uint32_t)tile);
			}
		}
		rule.textureRanges.reserve((size_t)resolvedRule.tileRanges.Size());
		for (const auto& range : resolvedRule.tileRanges)
		{
			if (range.first >= 0 && range.last >= 0)
			{
				rule.textureRanges.emplace_back((uint32_t)range.first, (uint32_t)range.last);
			}
		}
		for (const auto& textureName : resolvedRule.textureNames)
		{
			rule.textureNames.push_back(NormalizeLightOverlayTextureSelector(textureName.GetChars()));
		}
		if (rule.textureIds.empty() && rule.textureRanges.empty() && rule.textureNames.empty())
		{
			return false;
		}

		rule.hasMaterialResponse = resolvedRule.hasMaterialResponse;
		rule.materialResponse = resolvedRule.materialResponse;
		rule.hasMaterialResponseMin = resolvedRule.hasMaterialResponseMin;
		rule.materialResponseMin = resolvedRule.materialResponseMin;
		rule.hasMaterialResponseMax = resolvedRule.hasMaterialResponseMax;
		rule.materialResponseMax = resolvedRule.materialResponseMax;
		rule.hasVisibleGlowBlend = resolvedRule.hasVisibleGlowBlend;
		rule.visibleGlowBlend = resolvedRule.visibleGlowBlend;
		return true;
	}

	void BuildResolvedProducts(const ResolvedLightOverlaySet& resolved, NRILightRuleProducts& products)
	{
		products.emissiveOverrideRules.clear();
		products.emissiveOverrideRules.reserve((size_t)resolved.emissiveOverrideRules.Size());
		nri_light_rule_product_cache::AppendAcceptedProductsInSourceOrder(
			resolved.emissiveOverrideRules,
			products.emissiveOverrideRules,
			TryBuildEmissiveOverrideRule);

		products.surfaceLightFixtureRules.clear();
		products.surfaceLightFixtureRules.reserve((size_t)resolved.surfaceLightRules.Size());
		nri_light_rule_product_cache::AppendAcceptedProductsInSourceOrder(
			resolved.surfaceLightRules,
			products.surfaceLightFixtureRules,
			TryBuildSurfaceLightFixtureResponseRule);

		products.emissiveMaterialResponseRules.clear();
		products.emissiveMaterialResponseRules.reserve((size_t)resolved.emissiveMaterialResponseRules.Size());
		nri_light_rule_product_cache::AppendAcceptedProductsInSourceOrder(
			resolved.emissiveMaterialResponseRules,
			products.emissiveMaterialResponseRules,
			TryBuildEmissiveMaterialResponseRule);
	}
}

const NRILightRuleProducts& NRILightRuleProductCache::Resolve(
	const ResolvedLightOverlaySet& resolved,
	const nri_scene::PTMapWorld* mapWorld)
{
	mStats.resolvedProductsRebuilt = false;
	mStats.staticMapProductsRebuilt = false;
	if (!mIdentity.CanReuseResolvedProducts(resolved.resolvedGeneration))
	{
		BuildResolvedProducts(resolved, mProducts);
		mIdentity.CommitResolvedProducts(resolved.resolvedGeneration);
		mStats.resolvedProductBuildCount++;
		mStats.resolvedProductsRebuilt = true;
	}
	else
	{
		mStats.resolvedProductCacheHitCount++;
	}

	const bool mapAvailable = mapWorld != nullptr && mapWorld->valid;
	const uint64_t mapBuildSerial = mapAvailable ? mapWorld->buildSerial : 0;
	if (!mapAvailable)
	{
		mProducts.staticMapAnalyticOverlayRules.clear();
		mIdentity.InvalidateStaticMapProducts();
	}
	else if (!mIdentity.CanReuseStaticMapProducts(resolved.resolvedGeneration, mapBuildSerial))
	{
		BuildStaticMapAnalyticOverlayRules(resolved, *mapWorld, mProducts.staticMapAnalyticOverlayRules);
		mIdentity.CommitStaticMapProducts(resolved.resolvedGeneration, mapBuildSerial);
		mStats.staticMapProductBuildCount++;
		mStats.staticMapProductsRebuilt = true;
	}
	else
	{
		mStats.staticMapProductCacheHitCount++;
	}

	return mProducts;
}

void NRILightRuleProductCache::Reset()
{
	mProducts = {};
	mIdentity.Reset();
	mStats = {};
}
