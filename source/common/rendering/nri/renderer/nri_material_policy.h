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

	struct ActorMaterialOverrideState
	{
		uint32_t bits = ActorMaterialOverride_None;
		uint32_t emissiveStableFrames = 0;

		bool Empty() const
		{
			return bits == ActorMaterialOverride_None && emissiveStableFrames == 0;
		}
	};

	using ActorMaterialOverrideMap = std::unordered_map<int32_t, ActorMaterialOverrideState>;

	struct ActorMaterialOverrideCache
	{
		bool valid = false;
		uint32_t frameIndex = UINT32_MAX;
		uint32_t resolvedGeneration = 0;
		bool hasFullbrightOverrides = false;
		ActorMaterialOverrideMap overrides;
	};

	bool HasActorMaterialOverrideRules(const ResolvedLightOverlaySet& resolved);
	bool HasActorFullbrightOverrides(const ResolvedLightOverlaySet& resolved);
	void BuildActorMaterialOverrideMap(
		const ResolvedLightOverlaySet& resolved,
		ActorMaterialOverrideMap& outOverrides);
	const ActorMaterialOverrideMap& GetActorMaterialOverrideMapForFrame(
		const ResolvedLightOverlaySet& resolved,
		uint32_t frameIndex,
		ActorMaterialOverrideCache& cache,
		bool& outBuilt);

	void ApplyActorMaterialOverridesToBuiltMaterials(
		const ActorMaterialOverrideMap& actorOverrides,
		float fullbrightBoost,
		nri_scene::MaterialBridgeData& materials);

	void ApplyEmissiveMaterialOverrides(
		const SceneLightSystem& sceneLights,
		const ResolvedLightOverlaySet& resolved,
		float glowmapVisibleBlendScale,
		const nri_scene::MaterialBridgeData& materials,
		std::vector<nri_scene::MaterialData>& inOutGpuMaterials);

	void ApplyActorShadowMaterialOverrides(
		const ActorMaterialOverrideMap& actorOverrides,
		float fullbrightBoost,
		const nri_scene::MaterialBridgeData& materials,
		std::vector<nri_scene::MaterialData>& inOutGpuMaterials);

	bool MaterialDataEqual(
		const nri_scene::MaterialData& a,
		const nri_scene::MaterialData& b);

	bool MaterialDataVectorEqual(
		const std::vector<nri_scene::MaterialData>& a,
		const std::vector<nri_scene::MaterialData>& b);

	uint64_t ComputeChunkActorOverrideHash(
		const ActorMaterialOverrideMap& actorOverrides,
		const nri_scene::MaterialBridgeData& materials);

	uint64_t ComputeChunkEmissiveOverrideHash(
		const SceneLightSystem& sceneLights,
		const nri_scene::MaterialBridgeData& materials);
}
