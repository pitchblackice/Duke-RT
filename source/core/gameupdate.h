#pragma once

#include <cstdint>

struct GameUpdateSnapshot
{
	uint64_t presentationGeneration = 0;
	uint64_t engineUpdateGeneration = 0;
	uint64_t simulationGeneration = 0;
	uint32_t ticksExecutedThisPresentation = 0;
};

// Returns the simulation state published by the most recent TryRunTics call.
// The generation advances only after GameTicker completes; menu/input-only
// updates do not masquerade as gameplay updates.
GameUpdateSnapshot GetGameUpdateSnapshot();
