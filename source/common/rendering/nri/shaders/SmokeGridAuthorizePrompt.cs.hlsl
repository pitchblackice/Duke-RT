#include "Include/SmokeGridResources.hlsli"

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x != 0u) return;
	[unroll]
	for (uint slot = 0u; slot < NRI_SMOKE_PROMPT_FALLBACK_QUANTITY; ++slot)
	{
		SmokePromptOutcome outcome = gSmokePromptOutcomes[slot];
		if (outcome.Outcome != NRI_SMOKE_PROMPT_OUTCOME_GRID_NEW) continue;
		if (outcome.RequestedBricks == 0u || outcome.RequestedBricks != outcome.AdmittedBricks)
			outcome.Outcome = NRI_SMOKE_PROMPT_OUTCOME_FALLBACK;
		outcome.RequestedBricks = 0u;
		outcome.AdmittedBricks = 0u;
		gSmokePromptOutcomes[slot] = outcome;
	}
}
