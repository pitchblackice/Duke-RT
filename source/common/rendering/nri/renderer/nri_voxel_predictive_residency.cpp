#include "nri_voxel_predictive_residency.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace
{
	uint64_t HashValue(uint64_t hash, uint64_t value)
	{
		hash ^= value;
		hash *= 1099511628211ull;
		return hash;
	}

}

NRIVoxelPredictionTier ClassifyNRIVoxelPredictiveCandidate(
	const NRIVoxelPredictiveCandidate& candidate,
	const NRIVoxelPredictiveResidencySettings& settings)
{
	if (candidate.required)
		return NRIVoxelPredictionTier::Required;
	if (candidate.dynamicMaterial)
		return NRIVoxelPredictionTier::ExcludedDynamic;
	if (candidate.localPlayer)
		return NRIVoxelPredictionTier::LocalPlayerReflection;
	if (candidate.currentActor)
	{
		const float nearbyDistanceSquared = settings.nearbyDistance * settings.nearbyDistance;
		return candidate.actorDistanceSquared <= nearbyDistanceSquared ?
			NRIVoxelPredictionTier::NearbyCurrentActor : NRIVoxelPredictionTier::CurrentActor;
	}
	if (candidate.mapAuthored)
		return NRIVoxelPredictionTier::MapAuthored;
	if (candidate.initialAnimation)
		return NRIVoxelPredictionTier::InitialAnimation;
	if (candidate.commonAuthored)
		return NRIVoxelPredictionTier::CommonAuthored;
	return NRIVoxelPredictionTier::RareOptional;
}

NRIVoxelPredictiveSelection SelectNRIVoxelPredictiveResidency(
	const std::vector<NRIVoxelPredictiveCandidate>& candidates,
	const NRIVoxelPredictiveResidencySettings& settings)
{
	NRIVoxelPredictiveSelection result = {};
	result.candidates = (uint32_t)candidates.size();
	result.tiers.resize(candidates.size(), (uint8_t)NRIVoxelPredictionTier::RareOptional);
	result.geometrySelected.resize(candidates.size(), 0u);

	struct RankedCandidate
	{
		size_t index = 0;
		NRIVoxelPredictionTier tier = NRIVoxelPredictionTier::RareOptional;
	};
	std::vector<RankedCandidate> ranked;
	ranked.reserve(candidates.size());
	for (size_t i = 0; i < candidates.size(); ++i)
	{
		const NRIVoxelPredictionTier tier = ClassifyNRIVoxelPredictiveCandidate(candidates[i], settings);
		result.tiers[i] = (uint8_t)tier;
		result.excludedDynamic += tier == NRIVoxelPredictionTier::ExcludedDynamic ? 1u : 0u;
		ranked.push_back({ i, tier });
	}

	std::stable_sort(ranked.begin(), ranked.end(), [&](const RankedCandidate& a, const RankedCandidate& b)
	{
		const NRIVoxelPredictiveCandidate& ca = candidates[a.index];
		const NRIVoxelPredictiveCandidate& cb = candidates[b.index];
		if (a.tier != b.tier)
			return (uint8_t)a.tier < (uint8_t)b.tier;
		if (ca.admissionRank != cb.admissionRank)
			return ca.admissionRank < cb.admissionRank;
		if (ca.actorDistanceSquared != cb.actorDistanceSquared)
			return ca.actorDistanceSquared < cb.actorDistanceSquared;
		if (ca.meshKey != cb.meshKey)
			return ca.meshKey < cb.meshKey;
		if (ca.materialKey != cb.materialKey)
			return ca.materialKey < cb.materialKey;
		return ca.stableKey < cb.stableKey;
	});

	std::unordered_set<uint64_t> selectedMeshes;
	std::unordered_set<uint64_t> predictedMeshes;
	std::unordered_map<uint64_t, uint32_t> predictedMaterialCountByMesh;
	uint64_t predictedGeometryBytes = 0;
	for (const RankedCandidate& rankedCandidate : ranked)
	{
		const NRIVoxelPredictiveCandidate& candidate = candidates[rankedCandidate.index];
		const bool required = rankedCandidate.tier == NRIVoxelPredictionTier::Required;
		const bool eligiblePrediction =
			settings.enabled && rankedCandidate.tier <= NRIVoxelPredictionTier::CommonAuthored;
		if (!required && !settings.strict && !eligiblePrediction)
			continue;
		if (!candidate.sourceReady || !candidate.materialReady || candidate.meshKey == 0 || candidate.materialKey == 0)
			continue;

		const bool newMesh = selectedMeshes.find(candidate.meshKey) == selectedMeshes.end();
		const uint32_t predictedMaterialCount = predictedMaterialCountByMesh[candidate.meshKey];
		if (!required && !settings.strict)
		{
			if (settings.maxBindings != 0 && result.selectedPredicted >= settings.maxBindings)
			{
				result.capSkippedBindings++;
				continue;
			}
			if (newMesh && settings.maxUniqueMeshes != 0 && predictedMeshes.size() >= settings.maxUniqueMeshes)
			{
				result.capSkippedMeshes++;
				continue;
			}
			if (settings.maxMaterialsPerMesh != 0 && predictedMaterialCount >= settings.maxMaterialsPerMesh)
			{
				result.capSkippedMaterials++;
				continue;
			}
			if (newMesh && settings.maxUniqueGeometryBytes != 0 &&
				predictedGeometryBytes + candidate.estimatedGeometryBytes > settings.maxUniqueGeometryBytes)
			{
				result.capSkippedBytes++;
				continue;
			}
		}

		result.geometrySelected[rankedCandidate.index] = 1u;
		result.selectedBindings++;
		result.selectedRequired += required ? 1u : 0u;
		result.selectedPredicted += required ? 0u : 1u;
		if (!required)
		{
			predictedMaterialCountByMesh[candidate.meshKey] = predictedMaterialCount + 1u;
			predictedMeshes.insert(candidate.meshKey);
			if (newMesh)
				predictedGeometryBytes += candidate.estimatedGeometryBytes;
		}
		if (selectedMeshes.insert(candidate.meshKey).second)
		{
			result.selectedUniqueMeshes++;
			result.selectedGeometryBytes += candidate.estimatedGeometryBytes;
		}
		result.digest = HashValue(result.digest, candidate.meshKey);
		result.digest = HashValue(result.digest, candidate.materialKey);
		result.digest = HashValue(result.digest, (uint64_t)(uint8_t)rankedCandidate.tier);
	}

	return result;
}

const char* NRIVoxelPredictionTierName(NRIVoxelPredictionTier tier)
{
	switch (tier)
	{
	case NRIVoxelPredictionTier::Required: return "required";
	case NRIVoxelPredictionTier::LocalPlayerReflection: return "local-player";
	case NRIVoxelPredictionTier::NearbyCurrentActor: return "nearby-current";
	case NRIVoxelPredictionTier::CurrentActor: return "current-actor";
	case NRIVoxelPredictionTier::MapAuthored: return "map-authored";
	case NRIVoxelPredictionTier::InitialAnimation: return "initial-animation";
	case NRIVoxelPredictionTier::CommonAuthored: return "common-authored";
	case NRIVoxelPredictionTier::RareOptional: return "rare-optional";
	case NRIVoxelPredictionTier::ExcludedDynamic: return "excluded-dynamic";
	default: return "unknown";
	}
}
