#pragma once

#include <cstdint>

enum class PathTracingActorSpriteTraceStage : uint32_t;

namespace nri_actor_sprite_diag
{
	bool ShouldTraceVerbose(int traceMode, int traceFrames);
	bool ShouldTraceCoherency(int traceMode, int traceFrames);
	bool ShouldTraceMismatch(int traceMode, int traceFrames);
	const char* GetTraceStageName(PathTracingActorSpriteTraceStage stage);
}
