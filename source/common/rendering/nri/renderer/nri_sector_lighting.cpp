#include "nri_scene_lights.h"
#include "nri_cvars.h"

#include "nri_renderer.h"
#include "nri_runtime_mutation_trace.h"
#include "printf.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
	const char* YesNo(bool value)
	{
		return value ? "yes" : "no";
	}
}
float NRIGetSectorLightMultiplier()
{
	return std::max(0.0f, (float)nri_ptsectorlightmultiplier);
}

void NRIRenderer::TraceEmissiveSectorResponseChange()
{
	mSceneLights.TraceEmissiveSectorResponseChange(mFrameIndex, mCurrentCameraPos, nri_runtime_mutation::ShouldTracePtPerf());
}

void NRIRenderer::UpdateBoundSectorLightingState()
{
	const NRISectorLightingBoundState state = mSceneLights.BuildSectorLightingBoundState(NRIGetSectorLightMultiplier());
	mBoundSectorLightSectorCount = state.sectorCount;
	mBoundSectorLightActiveCount = state.activeCount;
	mBoundSectorLightPulsingCount = state.pulsingCount;
	mBoundSectorLightDominantSector = state.dominantSector;
	mBoundSectorLightDominantContribution = state.dominantContribution;
}

void SceneLightSystem::PrintSectorLightDump(
	const float currentCameraPos[3],
	float sectorLightMultiplier,
	float radius,
	uint32_t limit) const
{
	const auto& registry = mSectorLighting;
	if (registry.activeSectorIndices.empty() && registry.rawActiveSectorIndices.empty())
	{
		Printf("NRI PT sector lights: no active sector-light records are available. raw_active=%u raw_nonneutral=%u eligible=%u\n",
			registry.rawActiveSectorCount,
			registry.rawNonNeutralSectorCount,
			registry.eligibleSectorCount);
		return;
	}

	struct SectorCandidate
	{
		uint32_t sectorIndex = UINT32_MAX;
		float distanceSq = std::numeric_limits<float>::max();
		float center[3] = {};
	};

	std::vector<float> centerSums((size_t)registry.sectorCount * 3u, 0.0f);
	std::vector<uint32_t> centerCounts(registry.sectorCount, 0u);
	for (const auto& record : mSurfaceRecords)
	{
		if (record.provenance.sectorIndex < 0)
		{
			continue;
		}

		const uint32_t sectorIndex = (uint32_t)record.provenance.sectorIndex;
		if (sectorIndex >= registry.sectorCount)
		{
			continue;
		}

		centerSums[(size_t)sectorIndex * 3u + 0u] += record.center[0];
		centerSums[(size_t)sectorIndex * 3u + 1u] += record.center[1];
		centerSums[(size_t)sectorIndex * 3u + 2u] += record.center[2];
		centerCounts[sectorIndex]++;
	}

	std::vector<SectorCandidate> candidates;
	candidates.reserve(std::max(registry.activeSectorIndices.size(), registry.rawActiveSectorIndices.size()));
	std::vector<uint8_t> candidateSectors(registry.sectorCount, 0u);
	for (uint32_t sectorIndex : registry.activeSectorIndices)
	{
		if (sectorIndex < candidateSectors.size())
		{
			candidateSectors[sectorIndex] = 1u;
		}
	}
	for (uint32_t sectorIndex : registry.rawActiveSectorIndices)
	{
		if (sectorIndex < candidateSectors.size())
		{
			candidateSectors[sectorIndex] = 1u;
		}
	}
	const float radiusSq = radius > 0.0f ? radius * radius : std::numeric_limits<float>::max();
	for (uint32_t sectorIndex = 0; sectorIndex < (uint32_t)candidateSectors.size(); ++sectorIndex)
	{
		if (candidateSectors[sectorIndex] == 0u)
		{
			continue;
		}
		if (sectorIndex >= registry.sectorCount || sectorIndex >= centerCounts.size() || centerCounts[sectorIndex] == 0u)
		{
			continue;
		}

		SectorCandidate candidate = {};
		candidate.sectorIndex = sectorIndex;
		const float invCount = 1.0f / (float)centerCounts[sectorIndex];
		candidate.center[0] = centerSums[(size_t)sectorIndex * 3u + 0u] * invCount;
		candidate.center[1] = centerSums[(size_t)sectorIndex * 3u + 1u] * invCount;
		candidate.center[2] = centerSums[(size_t)sectorIndex * 3u + 2u] * invCount;
		const float dx = candidate.center[0] - currentCameraPos[0];
		const float dy = candidate.center[1] - currentCameraPos[1];
		const float dz = candidate.center[2] - currentCameraPos[2];
		candidate.distanceSq = dx * dx + dy * dy + dz * dz;
		if (candidate.distanceSq <= radiusSq)
		{
			candidates.push_back(candidate);
		}
	}

	std::sort(candidates.begin(), candidates.end(), [](const SectorCandidate& a, const SectorCandidate& b)
	{
		if (a.distanceSq != b.distanceSq)
		{
			return a.distanceSq < b.distanceSq;
		}
		return a.sectorIndex < b.sectorIndex;
	});

	Printf("NRI PT sector lights: active=%u raw_active=%u raw_nonneutral=%u response=boost:%u dim:%u neutral:%u eligible=%u fog=%u pulsing=%u radius=%.1f limit=%u multiplier=%.3f scales=(%.3f, %.3f, %.3f) clamp=%.3f sector_response=%.3f/[%.3f,%.3f] intensity=[%.3f,%.3f] reach=[%.3f,%.3f] filter=pal=%d shade=[%d,%d] lotag=%d pulse=%d/%.3f\n",
		registry.activeSectorCount,
		registry.rawActiveSectorCount,
		registry.rawNonNeutralSectorCount,
		registry.responseBoostSectorCount,
		registry.responseDimSectorCount,
		registry.responseNeutralSectorCount,
		registry.eligibleSectorCount,
		registry.fogSectorCount,
		registry.pulsingSectorCount,
		radius,
		limit,
		sectorLightMultiplier,
		(float)nri_ptsectorambientscale,
		(float)nri_ptsectorhemiscale,
		(float)nri_ptsectorfogscale,
		(float)nri_ptsectorclamp,
		(float)nri_ptsectoremissionsignalstrength,
		(float)nri_ptsectoremissionresponsemin,
		(float)nri_ptsectoremissionresponsemax,
		(float)nri_ptsectoremissionlightmin,
		(float)nri_ptsectoremissionlightmax,
		(float)nri_ptsectoremissionreachmin,
		(float)nri_ptsectoremissionreachmax,
		(int)nri_ptsectorfilterpal,
		(int)nri_ptsectorfilterminshade,
		(int)nri_ptsectorfiltermaxshade,
		(int)nri_ptsectorfilterlotag,
		(int)nri_ptsectorpulseframes,
		(float)nri_ptsectorpulseamount);

	const uint32_t printCount = std::min<uint32_t>((uint32_t)candidates.size(), limit);
	for (uint32_t i = 0; i < printCount; ++i)
	{
		const SectorCandidate& candidate = candidates[i];
		const auto& entry = registry.sectors[candidate.sectorIndex];
		Printf("NRI PT sector light %u: sector=%u dist=%.2f center=(%.2f, %.2f, %.2f) applied=(%.3f, %.3f, %.3f)*%.3f hemi=%.3f fog=%.3f raw_light=%.3f raw_floor=%.3f raw_ceil=%.3f raw_ambient=%.3f raw_hemi=%.3f raw_brightness=%.3f response=%.3f raw_fog=%.3f pulse=%.3f palette=%d shade=%d raw_shade=%d lotag=%d hitag=%d flags=0x%x\n",
			i,
			candidate.sectorIndex,
			std::sqrt(candidate.distanceSq),
			candidate.center[0],
			candidate.center[1],
			candidate.center[2],
			entry.ambientColor[0],
			entry.ambientColor[1],
			entry.ambientColor[2],
			entry.ambientIntensity * sectorLightMultiplier,
			entry.hemisphereAmount * sectorLightMultiplier,
			entry.fogAmount * sectorLightMultiplier,
			entry.rawLightLevel,
			entry.rawFloorLight,
			entry.rawCeilingLight,
			entry.rawAmbientIntensity,
			entry.rawHemisphereAmount,
			entry.rawResponseBrightness,
			entry.emitterResponseScale,
			entry.rawFogAmount,
			entry.pulseScale,
			entry.paletteIndex,
			entry.averageShade,
			entry.rawAverageShade,
			entry.lotag,
			entry.hitag,
			entry.sourceFlags);
	}

	if (printCount == 0)
	{
		Printf("NRI PT sector lights: no active or raw sector lights matched the requested radius.\n");
	}
}
