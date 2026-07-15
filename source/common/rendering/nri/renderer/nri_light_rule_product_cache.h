#pragma once

#include "nri_light_rule_product_cache_policy.h"
#include "nri_scene_lights.h"

#include <cstdint>
#include <vector>

struct ResolvedLightOverlaySet;

struct NRILightRuleProducts
{
	std::vector<SceneLightSystem::AnalyticLightRegistry::MapOverlayRule> staticMapAnalyticOverlayRules;
	std::vector<SceneLightSystem::EmissiveOverrideRule> emissiveOverrideRules;
	std::vector<SceneLightSystem::EmissiveOverrideRule> surfaceLightFixtureRules;
	std::vector<SceneLightSystem::EmissiveMaterialResponseRule> emissiveMaterialResponseRules;
};

struct NRILightRuleProductCacheStats
{
	uint64_t resolvedProductBuildCount = 0;
	uint64_t resolvedProductCacheHitCount = 0;
	uint64_t staticMapProductBuildCount = 0;
	uint64_t staticMapProductCacheHitCount = 0;
	bool resolvedProductsRebuilt = false;
	bool staticMapProductsRebuilt = false;
};

class NRILightRuleProductCache
{
public:
	const NRILightRuleProducts& Resolve(
		const ResolvedLightOverlaySet& resolved,
		const nri_scene::PTMapWorld* mapWorld);
	void Reset();

	const NRILightRuleProductCacheStats& GetStats() const { return mStats; }

private:
	NRILightRuleProducts mProducts;
	NRILightRuleProductCacheIdentity mIdentity;
	NRILightRuleProductCacheStats mStats;
};
