#include "Include/SmokeDormantGridResources.hlsli"

// Demotion runs after the final fine simulation ping. Compact before any next
// frame allocation can reuse a released index and append it a second time.
// The pool is fixed-capacity, so this deterministic serial scan is bounded.
[numthreads(1, 1, 1)]
void main()
{
	const uint activePing = min(gDormantConstants.ActivePing, 1u);
	const uint sourceCount = min(activePing == 0u ?
		gDormantFineControl[0].ActiveCountA : gDormantFineControl[0].ActiveCountB,
		gDormantConstants.FineBrickCapacity);
	uint destination = 0u;
	for (uint source = 0u; source < sourceCount; ++source)
	{
		const uint brickIndex = activePing == 0u ?
			gDormantFineActiveA[source] : gDormantFineActiveB[source];
		if (brickIndex >= gDormantConstants.FineBrickCapacity ||
			gDormantFineBricks[brickIndex].State != NRI_SMOKE_GRID_RESIDENT)
			continue;
		if (activePing == 0u) gDormantFineActiveA[destination] = brickIndex;
		else gDormantFineActiveB[destination] = brickIndex;
		destination++;
	}
	if (activePing == 0u) gDormantFineControl[0].ActiveCountA = destination;
	else gDormantFineControl[0].ActiveCountB = destination;
	gDormantControl[0].FineActiveCompactions++;
	gDormantControl[0].FineActiveEntriesRemoved += sourceCount - destination;
}
