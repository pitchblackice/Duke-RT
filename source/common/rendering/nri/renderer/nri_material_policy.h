#pragma once

#include "nri_scene_lights.h"
#include "../scene/nri_material_bridge.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

struct ResolvedLightOverlaySet;

namespace nri_material_policy
{
	enum ActorMaterialOverrideBits : uint32_t
	{
		ActorMaterialOverride_None = 0,
		ActorMaterialOverride_NoShadowReceive = 1u << 0,
		ActorMaterialOverride_NoShadowCast = 1u << 1,
		ActorMaterialOverride_Fullbright = 1u << 2,
	};

	struct ActorMaterialOverrideCache
	{
		bool valid = false;
		uint32_t frameIndex = UINT32_MAX;
		uint32_t resolvedGeneration = 0;
		bool hasFullbrightOverrides = false;
		std::unordered_map<int32_t, uint32_t> overrides;
	};

	bool HasActorMaterialOverrideRules(const ResolvedLightOverlaySet& resolved);
	bool HasActorFullbrightOverrides(const ResolvedLightOverlaySet& resolved);
	void BuildActorMaterialOverrideMap(
		const ResolvedLightOverlaySet& resolved,
		std::unordered_map<int32_t, uint32_t>& outOverrides);
	const std::unordered_map<int32_t, uint32_t>& GetActorMaterialOverrideMapForFrame(
		const ResolvedLightOverlaySet& resolved,
		uint32_t frameIndex,
		ActorMaterialOverrideCache& cache,
		bool& outBuilt);

	void ApplyActorFullbrightOverridesToBuiltMaterials(
		const std::unordered_map<int32_t, uint32_t>& actorOverrides,
		float fullbrightBoost,
		nri_scene::MaterialBridgeData& materials);

	void ApplyEmissiveMaterialOverrides(
		const SceneLightSystem& sceneLights,
		float glowmapVisibleBlendScale,
		const nri_scene::MaterialBridgeData& materials,
		std::vector<nri_scene::MaterialData>& inOutGpuMaterials);

	void ApplyActorShadowMaterialOverrides(
		const std::unordered_map<int32_t, uint32_t>& actorOverrides,
		float fullbrightBoost,
		const nri_scene::MaterialBridgeData& materials,
		std::vector<nri_scene::MaterialData>& inOutGpuMaterials);

	uint64_t ComputeChunkActorOverrideHash(
		const std::unordered_map<int32_t, uint32_t>& actorOverrides,
		const nri_scene::MaterialBridgeData& materials);

	uint64_t ComputeChunkEmissiveOverrideHash(
		const SceneLightSystem& sceneLights,
		const nri_scene::MaterialBridgeData& materials);
}
