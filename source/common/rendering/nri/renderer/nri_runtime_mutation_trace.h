#pragma once

#include "nri_renderer.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace nri_runtime_mutation
{
bool ShouldEmitTemporalTraceLogs();
bool ShouldTracePtPerf();
bool ShouldCollectRuntimeMutationPerfTiming();
const char* YesNo(bool value);

class ScopedPtPerfTimer
{
public:
	explicit ScopedPtPerfTimer(double& targetMs);
	~ScopedPtPerfTimer();

private:
	double* mTarget = nullptr;
	std::chrono::steady_clock::time_point mStart = {};
};

enum RuntimeStructuralRebuildTriggerBits : uint32_t
{
	RuntimeStructuralRebuildTrigger_ReplacementDelta = 1u << 0,
	RuntimeStructuralRebuildTrigger_ViewChanged = 1u << 1,
	RuntimeStructuralRebuildTrigger_StaticAnimatedFlip = 1u << 2,
	RuntimeStructuralRebuildTrigger_ExcludeStaticFlip = 1u << 3,
	RuntimeStructuralRebuildTrigger_ForceTopology = 1u << 4,
	RuntimeStructuralRebuildTrigger_Invalid = 1u << 5,
};

enum RuntimeGeometryDirtyFamilyBits : uint32_t
{
	RuntimeGeometryDirtyFamily_SectorGeometryOnly = 1u << 0,
	RuntimeGeometryDirtyFamily_WallGeometryOnly = 1u << 1,
	RuntimeGeometryDirtyFamily_SectorWallGeometry = 1u << 2,
	RuntimeGeometryDirtyFamily_DirtyOnly = 1u << 3,
	RuntimeGeometryDirtyFamily_GeometryDirtyMixed = 1u << 4,
};

uint32_t ScoreRuntimeSectorDirtyTruthEntry(const NRIRenderer::RuntimeSectorDirtyTruthTraceEntry& entry);
uint32_t ScoreRuntimeAnimatedChurnTraceEntry(const NRIRenderer::RuntimeAnimatedChurnTraceEntry& entry);
uint32_t ScoreRuntimeMaterialOnlyMismatchTraceEntry(const NRIRenderer::RuntimeMaterialOnlyMismatchTraceEntry& entry);
uint32_t ScoreRuntimeStructuralRebuildTraceEntry(const NRIRenderer::RuntimeStructuralRebuildTraceEntry& entry);
uint32_t ScoreRuntimeRecurringChunkTraceEntry(const NRIRenderer::RuntimeRecurringChunkTraceEntry& entry);
uint32_t ScoreRuntimeGeometryDirtyTraceEntry(const NRIRenderer::RuntimeGeometryDirtyTraceEntry& entry);

template <typename Entry, size_t N, typename ScoreFn>
void InsertRankedTraceEntry(std::array<Entry, N>& entries, Entry entry, ScoreFn scoreFn)
{
	entry.score = scoreFn(entry);
	size_t insertIndex = N;
	for (size_t i = 0; i < N; ++i)
	{
		if (!entries[i].valid || entry.score > entries[i].score)
		{
			insertIndex = i;
			break;
		}
	}

	if (insertIndex >= N)
	{
		return;
	}

	for (size_t i = N - 1; i > insertIndex; --i)
	{
		entries[i] = entries[i - 1];
	}
	entries[insertIndex] = entry;
	entries[insertIndex].valid = true;
}
}
