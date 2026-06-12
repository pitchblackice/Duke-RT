#pragma once

#include "nri_scene_lights.h"
#include "../scene/nri_material_bridge.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace nri_material_policy
{
	enum ActorMaterialOverrideBits : uint32_t
	{
		ActorMaterialOverride_None = 0,
		ActorMaterialOverride_NoShadowReceive = 1u << 0,
		ActorMaterialOverride_NoShadowCast = 1u << 1,
		ActorMaterialOverride_Fullbright = 1u << 2,
	};

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
