#include "nri_material_policy.h"

#include "coreactor.h"
#include "lightoverlay.h"
#include "texinfo.h"

#include <algorithm>
#include <cstring>

namespace
{
	uint64_t HashCombine64(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
	}

	uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	float GetFullbrightRoughnessHint(uint32_t materialFlags)
	{
		if ((materialFlags & nri_scene::MaterialFlag_Sprite) != 0)
		{
			return 0.45f;
		}
		if ((materialFlags & nri_scene::MaterialFlag_Flat) != 0)
		{
			return 0.60f;
		}
		return 0.55f;
	}

	void ApplyFullbrightMaterialOverride(nri_scene::MaterialData& material, float fullbrightBoost)
	{
		material.flags |= nri_scene::MaterialFlag_Fullbright;
		material.lightLevel = 1.0f;
		material.roughnessHint = GetFullbrightRoughnessHint(material.flags);
		material.lightingFlags |= nri_scene::MaterialLightingFlag_MaterialFullbright;
		material.emissiveMode = nri_scene::MaterialEmissiveMode_UseBaseTexture;
		material.emissiveTextureIndex = material.textureIndex;
		material.emissiveIntensity = 1.0f;
		material.emissiveMaskScale = 1.0f;
		material.emissiveReserved = fullbrightBoost;
		material.emissiveColor[0] = 1.0f;
		material.emissiveColor[1] = 1.0f;
		material.emissiveColor[2] = 1.0f;
	}
}

bool nri_material_policy::HasActorMaterialOverrideRules(const ResolvedLightOverlaySet& resolved)
{
	return resolved.actorRules.Size() > 0 || resolved.actorOverrideRules.Size() > 0;
}

bool nri_material_policy::HasActorFullbrightOverrides(const ResolvedLightOverlaySet& resolved)
{
	for (const auto& rule : resolved.actorRules)
	{
		if (rule.hasFullbright)
		{
			return true;
		}
	}
	return false;
}

void nri_material_policy::BuildActorMaterialOverrideMap(
	const ResolvedLightOverlaySet& resolved,
	std::unordered_map<int32_t, uint32_t>& outOverrides)
{
	if (!HasActorMaterialOverrideRules(resolved))
	{
		return;
	}

	TSpriteIterator<DCoreActor> it;
	while (auto actor = it.Next())
	{
		if (actor == nullptr || !actor->exists() || (actor->ObjectFlags & OF_EuthanizeMe) != 0)
		{
			continue;
		}

		PClass* actorClass = actor->GetClass();
		if (actorClass == nullptr)
		{
			continue;
		}

		uint32_t overrideBits = ActorMaterialOverride_None;
		bool touched = false;
		const uint32_t actorTextureId = (unsigned)actor->spr.picnum < MAXTILES ? (uint32_t)tileGetTextureID(actor->spr.picnum).GetIndex() : 0u;
		for (const auto& resolvedRule : resolved.actorRules)
		{
			if (!resolvedRule.actorClassResolved ||
				resolvedRule.actorClass == nullptr ||
				(actorClass != resolvedRule.actorClass && !actorClass->IsDescendantOf(resolvedRule.actorClass)))
			{
				continue;
			}

			if (resolvedRule.hasTileFilter && actorTextureId != (uint32_t)resolvedRule.tileFilter)
			{
				continue;
			}

			if (resolvedRule.hasShadowReceive)
			{
				touched = true;
				if (resolvedRule.shadowReceive)
				{
					overrideBits &= ~ActorMaterialOverride_NoShadowReceive;
				}
				else
				{
					overrideBits |= ActorMaterialOverride_NoShadowReceive;
				}
			}

			if (resolvedRule.hasShadowCast)
			{
				touched = true;
				if (resolvedRule.shadowCast)
				{
					overrideBits &= ~ActorMaterialOverride_NoShadowCast;
				}
				else
				{
					overrideBits |= ActorMaterialOverride_NoShadowCast;
				}
			}

			if (resolvedRule.hasFullbright)
			{
				touched = true;
				if (resolvedRule.fullbright)
				{
					overrideBits |= ActorMaterialOverride_Fullbright;
				}
				else
				{
					overrideBits &= ~ActorMaterialOverride_Fullbright;
				}
			}
		}

		for (const auto& resolvedRule : resolved.actorOverrideRules)
		{
			if (!resolvedRule.actorClassResolved ||
				resolvedRule.actorClass == nullptr ||
				(actorClass != resolvedRule.actorClass && !actorClass->IsDescendantOf(resolvedRule.actorClass)))
			{
				continue;
			}

			if (resolvedRule.hasShadowReceive)
			{
				touched = true;
				if (resolvedRule.shadowReceive)
				{
					overrideBits &= ~ActorMaterialOverride_NoShadowReceive;
				}
				else
				{
					overrideBits |= ActorMaterialOverride_NoShadowReceive;
				}
			}

			if (resolvedRule.hasShadowCast)
			{
				touched = true;
				if (resolvedRule.shadowCast)
				{
					overrideBits &= ~ActorMaterialOverride_NoShadowCast;
				}
				else
				{
					overrideBits |= ActorMaterialOverride_NoShadowCast;
				}
			}
		}

		if (touched && overrideBits != ActorMaterialOverride_None)
		{
			outOverrides[(int32_t)actor->GetIndex()] = overrideBits;
		}
	}
}

void nri_material_policy::ApplyActorFullbrightOverridesToBuiltMaterials(
	const std::unordered_map<int32_t, uint32_t>& actorOverrides,
	float fullbrightBoost,
	nri_scene::MaterialBridgeData& materials)
{
	const uint32_t count = std::min<uint32_t>((uint32_t)materials.materials.size(), (uint32_t)materials.lightMetadata.size());
	for (uint32_t materialIndex = 0; materialIndex < count; ++materialIndex)
	{
		nri_scene::MaterialLightingMetadata& metadata = materials.lightMetadata[materialIndex];
		if (metadata.actorIndex < 0)
		{
			continue;
		}

		auto it = actorOverrides.find(metadata.actorIndex);
		if (it == actorOverrides.end() || (it->second & ActorMaterialOverride_Fullbright) == 0)
		{
			continue;
		}

		nri_scene::MaterialData& material = materials.materials[materialIndex];
		ApplyFullbrightMaterialOverride(material, fullbrightBoost);

		metadata.materialFlags |= nri_scene::MaterialFlag_Fullbright;
		metadata.lightingFlags |= nri_scene::MaterialLightingFlag_MaterialFullbright;
		metadata.lightLevel = 1.0f;
		metadata.emissiveMode = nri_scene::MaterialEmissiveMode_UseBaseTexture;
		metadata.emissiveTextureIndex = material.textureIndex;
		metadata.emissiveIntensity = 1.0f;
		metadata.emissiveMaskScale = 1.0f;
		metadata.visibleFullbrightBoost = fullbrightBoost;
		metadata.emissiveColor[0] = 1.0f;
		metadata.emissiveColor[1] = 1.0f;
		metadata.emissiveColor[2] = 1.0f;
	}
}

void nri_material_policy::ApplyEmissiveMaterialOverrides(
	const SceneLightSystem& sceneLights,
	float glowmapVisibleBlendScale,
	const nri_scene::MaterialBridgeData& materials,
	std::vector<nri_scene::MaterialData>& inOutGpuMaterials)
{
	const uint32_t count = std::min<uint32_t>((uint32_t)inOutGpuMaterials.size(), (uint32_t)materials.lightMetadata.size());
	for (uint32_t materialIndex = 0; materialIndex < count; ++materialIndex)
	{
		nri_scene::MaterialData& material = inOutGpuMaterials[materialIndex];
		sceneLights.ApplyEmissiveMaterialSettings(materials.lightMetadata[materialIndex], material);
		if (material.emissiveMode == nri_scene::MaterialEmissiveMode_UseGlowmapTexture)
		{
			material.emissiveReserved = glowmapVisibleBlendScale;
		}
	}
}

void nri_material_policy::ApplyActorShadowMaterialOverrides(
	const std::unordered_map<int32_t, uint32_t>& actorOverrides,
	float fullbrightBoost,
	const nri_scene::MaterialBridgeData& materials,
	std::vector<nri_scene::MaterialData>& inOutGpuMaterials)
{
	if (actorOverrides.empty())
	{
		return;
	}

	const uint32_t count = std::min<uint32_t>((uint32_t)inOutGpuMaterials.size(), (uint32_t)materials.lightMetadata.size());
	for (uint32_t materialIndex = 0; materialIndex < count; ++materialIndex)
	{
		const nri_scene::MaterialLightingMetadata& metadata = materials.lightMetadata[materialIndex];
		if (metadata.actorIndex < 0)
		{
			continue;
		}

		auto it = actorOverrides.find(metadata.actorIndex);
		if (it == actorOverrides.end())
		{
			continue;
		}

		if ((it->second & ActorMaterialOverride_NoShadowReceive) != 0)
		{
			inOutGpuMaterials[materialIndex].lightingFlags |= nri_scene::MaterialLightingFlag_NoShadowReceive;
		}
		if ((it->second & ActorMaterialOverride_NoShadowCast) != 0)
		{
			inOutGpuMaterials[materialIndex].lightingFlags |= nri_scene::MaterialLightingFlag_NoShadowCast;
		}
		if ((it->second & ActorMaterialOverride_Fullbright) != 0)
		{
			ApplyFullbrightMaterialOverride(inOutGpuMaterials[materialIndex], fullbrightBoost);
		}
	}
}

uint64_t nri_material_policy::ComputeChunkActorOverrideHash(
	const std::unordered_map<int32_t, uint32_t>& actorOverrides,
	const nri_scene::MaterialBridgeData& materials)
{
	if (actorOverrides.empty() || materials.lightMetadata.empty())
	{
		return 0;
	}

	uint64_t hash = 1469598103934665603ull;
	bool touched = false;
	for (const auto& metadata : materials.lightMetadata)
	{
		if (metadata.actorIndex < 0)
		{
			continue;
		}

		auto it = actorOverrides.find(metadata.actorIndex);
		if (it == actorOverrides.end() || it->second == ActorMaterialOverride_None)
		{
			continue;
		}

		touched = true;
		hash = HashCombine64(hash, (uint64_t)(uint32_t)metadata.actorIndex);
		hash = HashCombine64(hash, (uint64_t)it->second);
	}

	return touched ? hash : 0;
}

uint64_t nri_material_policy::ComputeChunkEmissiveOverrideHash(
	const SceneLightSystem& sceneLights,
	const nri_scene::MaterialBridgeData& materials)
{
	const uint32_t count = std::min<uint32_t>((uint32_t)materials.materials.size(), (uint32_t)materials.lightMetadata.size());
	if (count == 0)
	{
		return 0;
	}

	uint64_t hash = 1469598103934665603ull;
	bool touched = false;
	for (uint32_t materialIndex = 0; materialIndex < count; ++materialIndex)
	{
		nri_scene::MaterialData effectiveMaterial = materials.materials[materialIndex];
		const bool emissiveApplied = sceneLights.ApplyEmissiveMaterialSettings(materials.lightMetadata[materialIndex], effectiveMaterial);
		if (!emissiveApplied)
		{
			continue;
		}

		touched = true;
		hash = HashCombine64(hash, (uint64_t)materialIndex);
		hash = HashCombine64(hash, (uint64_t)effectiveMaterial.materialClass);
		hash = HashCombine64(hash, (uint64_t)effectiveMaterial.emissiveMode);
		hash = HashCombine64(hash, (uint64_t)effectiveMaterial.emissiveTextureIndex);
		hash = HashCombine64(hash, (uint64_t)FloatBits(effectiveMaterial.emissiveColor[0]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(effectiveMaterial.emissiveColor[1]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(effectiveMaterial.emissiveColor[2]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(effectiveMaterial.emissiveIntensity));
		hash = HashCombine64(hash, (uint64_t)FloatBits(effectiveMaterial.emissiveMaskScale));
		hash = HashCombine64(hash, (uint64_t)FloatBits(effectiveMaterial.emissiveReserved));
	}

	return touched ? hash : 0;
}
