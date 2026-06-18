#include "nri_renderer.h"
#include "nri_cvars.h"
#include "../scene/nri_map_builder.h"
#include "mapinfo.h"
#include "printf.h"

#include <cstring>
#include <string>
#include <vector>

namespace
{
	struct StartupMapWorldChunkDiffSample
	{
		uint32_t chunkIndex = 0;
		int32_t currentSectorIndex = -1;
		int32_t rebuiltSectorIndex = -1;
		uint32_t currentSurfaceCount = 0;
		uint32_t rebuiltSurfaceCount = 0;
		uint32_t currentTriangleCount = 0;
		uint32_t rebuiltTriangleCount = 0;
		uint32_t surfaceDiffCount = 0;
	};

	struct StartupMapWorldDiffDetails
	{
		bool validMismatch = false;
		bool chunkCountMismatch = false;
		bool surfaceCountMismatch = false;
		bool portalCountMismatch = false;
		bool portalTargetCountMismatch = false;
		bool statsMismatch = false;
		std::vector<StartupMapWorldChunkDiffSample> chunkSamples;
		std::vector<uint32_t> lateVisibleValidationChunks;
	};

	static bool StartupMapWorldSurfaceDiffers(
		const nri_scene::PTMapSurface& currentSurface,
		const nri_scene::PTMapSurface& rebuiltSurface)
	{
		const auto& a = currentSurface.surface;
		const auto& b = rebuiltSurface.surface;
		return currentSurface.kind != rebuiltSurface.kind ||
			a.vertices.size() != b.vertices.size() ||
			a.material.flags != b.material.flags ||
			a.material.texture != b.material.texture ||
			a.material.palette != b.material.palette ||
			a.provenance.sourceType != b.provenance.sourceType ||
			a.provenance.sectorIndex != b.provenance.sectorIndex ||
			a.provenance.wallIndex != b.provenance.wallIndex ||
			a.provenance.sectionIndex != b.provenance.sectionIndex;
	}

	static std::string BuildStartupMapWorldDiffReasonSummary(const StartupMapWorldDiffDetails& details)
	{
		std::string summary;
		auto appendReason = [&summary](const char* reason)
		{
			if (reason == nullptr || reason[0] == '\0')
			{
				return;
			}
			if (!summary.empty())
			{
				summary += ",";
			}
			summary += reason;
		};

		appendReason(details.validMismatch ? "valid" : nullptr);
		appendReason(details.chunkCountMismatch ? "chunk-count" : nullptr);
		appendReason(details.surfaceCountMismatch ? "surface-count" : nullptr);
		appendReason(details.portalCountMismatch ? "portal-count" : nullptr);
		appendReason(details.portalTargetCountMismatch ? "portal-target-count" : nullptr);
		appendReason(details.statsMismatch ? "stats" : nullptr);
		return summary.empty() ? "chunk-or-surface" : summary;
	}

	static bool StartupMapWorldStructureDiffers(
		const nri_scene::PTMapWorld& currentWorld,
		const nri_scene::PTMapWorld& rebuiltWorld,
		uint32_t& outChunkDiffCount,
		uint32_t& outSurfaceDiffCount,
		StartupMapWorldDiffDetails* outDetails = nullptr)
	{
		outChunkDiffCount = 0;
		outSurfaceDiffCount = 0;

		StartupMapWorldDiffDetails localDetails = {};
		StartupMapWorldDiffDetails& details = outDetails != nullptr ? *outDetails : localDetails;
		details = {};

		details.validMismatch = currentWorld.valid != rebuiltWorld.valid;
		details.chunkCountMismatch = currentWorld.chunks.size() != rebuiltWorld.chunks.size();
		details.surfaceCountMismatch = currentWorld.surfaces.size() != rebuiltWorld.surfaces.size();
		details.portalCountMismatch = currentWorld.portals.size() != rebuiltWorld.portals.size();
		details.portalTargetCountMismatch = currentWorld.portalTargets.size() != rebuiltWorld.portalTargets.size();
		details.statsMismatch = std::memcmp(&currentWorld.stats, &rebuiltWorld.stats, sizeof(currentWorld.stats)) != 0;

		const size_t chunkCount = std::min(currentWorld.chunks.size(), rebuiltWorld.chunks.size());
		details.chunkSamples.reserve(std::min<size_t>(chunkCount, 12u));
		details.lateVisibleValidationChunks.reserve(std::min<size_t>(chunkCount, 64u));
		for (size_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
		{
			const auto& currentChunk = currentWorld.chunks[chunkIndex];
			const auto& rebuiltChunk = rebuiltWorld.chunks[chunkIndex];
			const bool chunkMetadataDiff =
				currentChunk.kind != rebuiltChunk.kind ||
				currentChunk.chunkIndex != rebuiltChunk.chunkIndex ||
				currentChunk.sectorIndex != rebuiltChunk.sectorIndex ||
				currentChunk.localSpaceIndex != rebuiltChunk.localSpaceIndex ||
				currentChunk.firstSurface != rebuiltChunk.firstSurface ||
				currentChunk.surfaceCount != rebuiltChunk.surfaceCount ||
				currentChunk.triangleCount != rebuiltChunk.triangleCount;

			const uint32_t currentAvailableSurfaceCount =
				currentChunk.firstSurface < currentWorld.surfaces.size() ?
				(uint32_t)std::min<size_t>(currentChunk.surfaceCount, currentWorld.surfaces.size() - currentChunk.firstSurface) :
				0u;
			const uint32_t rebuiltAvailableSurfaceCount =
				rebuiltChunk.firstSurface < rebuiltWorld.surfaces.size() ?
				(uint32_t)std::min<size_t>(rebuiltChunk.surfaceCount, rebuiltWorld.surfaces.size() - rebuiltChunk.firstSurface) :
				0u;
			const uint32_t comparableSurfaceCount = std::min(currentAvailableSurfaceCount, rebuiltAvailableSurfaceCount);

			uint32_t chunkSurfaceDiffCount = 0;
			for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < comparableSurfaceCount; ++localSurfaceIndex)
			{
				const auto& currentSurface = currentWorld.surfaces[currentChunk.firstSurface + localSurfaceIndex];
				const auto& rebuiltSurface = rebuiltWorld.surfaces[rebuiltChunk.firstSurface + localSurfaceIndex];
				if (StartupMapWorldSurfaceDiffers(currentSurface, rebuiltSurface))
				{
					chunkSurfaceDiffCount++;
				}
			}

			chunkSurfaceDiffCount +=
				currentAvailableSurfaceCount > rebuiltAvailableSurfaceCount ?
				(currentAvailableSurfaceCount - rebuiltAvailableSurfaceCount) :
				(rebuiltAvailableSurfaceCount - currentAvailableSurfaceCount);

			outSurfaceDiffCount += chunkSurfaceDiffCount;
			if (chunkMetadataDiff || chunkSurfaceDiffCount > 0u)
			{
				outChunkDiffCount++;
				const bool lateVisibleValidationCandidate =
					chunkSurfaceDiffCount > 0u &&
					currentAvailableSurfaceCount == rebuiltAvailableSurfaceCount &&
					currentChunk.triangleCount == rebuiltChunk.triangleCount &&
					currentChunk.chunkIndex == rebuiltChunk.chunkIndex;
				if (lateVisibleValidationCandidate)
				{
					details.lateVisibleValidationChunks.push_back(currentChunk.chunkIndex);
				}
				if (details.chunkSamples.size() < 12u)
				{
					StartupMapWorldChunkDiffSample sample = {};
					sample.chunkIndex = (uint32_t)chunkIndex;
					sample.currentSectorIndex = currentChunk.sectorIndex;
					sample.rebuiltSectorIndex = rebuiltChunk.sectorIndex;
					sample.currentSurfaceCount = currentAvailableSurfaceCount;
					sample.rebuiltSurfaceCount = rebuiltAvailableSurfaceCount;
					sample.currentTriangleCount = currentChunk.triangleCount;
					sample.rebuiltTriangleCount = rebuiltChunk.triangleCount;
					sample.surfaceDiffCount = chunkSurfaceDiffCount;
					details.chunkSamples.push_back(sample);
				}
			}
		}

		return details.validMismatch ||
			details.chunkCountMismatch ||
			details.surfaceCountMismatch ||
			details.portalCountMismatch ||
			details.portalTargetCountMismatch ||
			outChunkDiffCount > 0 ||
			outSurfaceDiffCount > 0;
	}
}

void NRIRenderer::RebuildStartupMutationBaseline()
{
	mPendingStartupMutationRebaseline = false;
	mAllowStartupMutationRebaseline = false;
}

bool NRIRenderer::ApplyStartupMapWorldCorrectionIfNeeded(const char* trigger)
{
	if (!mAllowStartupMapWorldCorrection)
	{
		return true;
	}

	if (mFrameIndex > mStartupMapWorldCorrectionDeadlineFrame)
	{
		mAllowStartupMapWorldCorrection = false;
		return true;
	}

	if (!mMapWorld.valid ||
		!mStaticMapScene.valid ||
		!mRuntimeMutation.CanApplyStartupCorrection((uint32_t)mMapWorld.chunks.size()))
	{
		return true;
	}

	nri_scene::PTMapWorld correctedWorld = {};
	nri_scene::PTMapBuildOptions mapBuildOptions = {};
	if (!nri_scene::BuildMapWorld(correctedWorld, mapBuildOptions))
	{
		Printf(TEXTCOLOR_RED "NRI PT startup world correction: authoritative rebuild failed for %s trigger=%s frame=%u.\n",
			currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)",
			trigger != nullptr ? trigger : "unknown",
			mFrameIndex);
		mAllowStartupMapWorldCorrection = false;
		mStartupMapWorldCorrectionDeadlineFrame = 0;
		return true;
	}

	uint32_t chunkDiffCount = 0;
	uint32_t surfaceDiffCount = 0;
	StartupMapWorldDiffDetails diffDetails = {};
	if (!StartupMapWorldStructureDiffers(mMapWorld, correctedWorld, chunkDiffCount, surfaceDiffCount, &diffDetails))
	{
		return true;
	}

	DestroyStaticMapSceneCache("startup-world-correction");
	mStaticMapScene = {};
	mStaticAccelerationBuildSerial = 0;
	mSkyEnvironment.PreservedStaticMapSky() = {};
	mMapWorld = std::move(correctedWorld);
	mObservedMapWorldBuildSerial = nri_scene::GetPendingLevelGeometryBuildSerial();
	mAllowStartupMapWorldCorrection = false;
	mStartupMapWorldCorrectionDeadlineFrame = 0;
	if (mPendingStartupVisibleChunkValidation.size() < mMapWorld.chunks.size())
	{
		mPendingStartupVisibleChunkValidation.resize(mMapWorld.chunks.size(), 0u);
	}
	mRuntimeMutation.PrepareStartupBaseline(mMapWorld.buildSerial, (uint32_t)mMapWorld.chunks.size());
	for (uint32_t chunkIndex : diffDetails.lateVisibleValidationChunks)
	{
		if (chunkIndex < mPendingStartupVisibleChunkValidation.size())
		{
			mPendingStartupVisibleChunkValidation[chunkIndex] = 1u;
		}
	}
	RequestHistoryReset("startup-world-correction");

	Printf("NRI PT startup world correction: trigger=%s level=%s frame=%u build_serial=%llu chunk_diffs=%u surface_diffs=%u chunks=%u surfaces=%u tris=%u\n",
		trigger != nullptr ? trigger : "unknown",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		mFrameIndex,
		(unsigned long long)mMapWorld.buildSerial,
		chunkDiffCount,
		surfaceDiffCount,
		mMapWorld.stats.chunkCount,
		mMapWorld.stats.surfaceCount,
		mMapWorld.stats.triangleCount);

	if (nri_ptscenestats)
	{
		const auto& stats = mMapWorld.stats;
		Printf("NRI PT startup world correction: trigger=%s level=%s frame=%u build_serial=%llu chunk_diffs=%u surface_diffs=%u chunks=%u surfaces=%u walls=%u flats=%u portals=%u skies=%u tris=%u\n",
			trigger != nullptr ? trigger : "unknown",
			mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
			mFrameIndex,
			(unsigned long long)mMapWorld.buildSerial,
			chunkDiffCount,
			surfaceDiffCount,
			stats.chunkCount,
			stats.surfaceCount,
			stats.wallSurfaceCount,
			stats.flatSurfaceCount,
			stats.portalSurfaceCount,
			stats.skySurfaceCount,
			stats.triangleCount);
		const std::string reasonSummary = BuildStartupMapWorldDiffReasonSummary(diffDetails);
		Printf("NRI PT startup world correction detail: trigger=%s frame=%u reasons=%s sampled_chunks=%u/%u\n",
			trigger != nullptr ? trigger : "unknown",
			mFrameIndex,
			reasonSummary.c_str(),
			(uint32_t)diffDetails.chunkSamples.size(),
			chunkDiffCount);
		if (!diffDetails.lateVisibleValidationChunks.empty())
		{
			Printf("NRI PT startup world correction late-visible: trigger=%s frame=%u chunks=%u\n",
				trigger != nullptr ? trigger : "unknown",
				mFrameIndex,
				(uint32_t)diffDetails.lateVisibleValidationChunks.size());
		}
		for (size_t sampleIndex = 0; sampleIndex < diffDetails.chunkSamples.size(); ++sampleIndex)
		{
			const auto& sample = diffDetails.chunkSamples[sampleIndex];
			Printf("NRI PT startup world correction chunk: trigger=%s frame=%u sample=%u/%u chunk=%u sector=%d->%d surfaces=%u->%u tris=%u->%u surface_diffs=%u\n",
				trigger != nullptr ? trigger : "unknown",
				mFrameIndex,
				(uint32_t)(sampleIndex + 1u),
				(uint32_t)diffDetails.chunkSamples.size(),
				sample.chunkIndex,
				sample.currentSectorIndex,
				sample.rebuiltSectorIndex,
				sample.currentSurfaceCount,
				sample.rebuiltSurfaceCount,
				sample.currentTriangleCount,
				sample.rebuiltTriangleCount,
				sample.surfaceDiffCount);
		}
	}
	return true;
}
