#include "nri_actor_sprite_diagnostics.h"

#include "v_video.h"

namespace nri_actor_sprite_diag
{
	bool ShouldTraceVerbose(int traceMode, int traceFrames)
	{
		return traceMode == 1 && traceFrames > 0;
	}

	bool ShouldTraceCoherency(int traceMode, int traceFrames)
	{
		return traceMode > 0 && traceFrames > 0;
	}

	bool ShouldTraceMismatch(int traceMode, int traceFrames)
	{
		return traceMode >= 1 && traceFrames > 0;
	}

	const char* GetTraceStageName(PathTracingActorSpriteTraceStage stage)
	{
		switch (stage)
		{
		case PathTracingActorSpriteTraceStage::Draw: return "draw";
		case PathTracingActorSpriteTraceStage::CaptureScene: return "capture_scene";
		case PathTracingActorSpriteTraceStage::CaptureActorScene: return "capture_actor_scene";
		default: return "unknown";
		}
	}
}
