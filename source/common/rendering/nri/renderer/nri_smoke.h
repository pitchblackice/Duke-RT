#pragma once

#include "nri_renderer_settings.h"

#include <cstdint>

class NRIRenderer;
struct NRISmokeRouteDesc;

struct NRISmokeStatusSnapshot
{
	bool enabled = false;
	bool mainViewEligible = false;
	bool routeSupported = false;
	uint32_t simulationEpoch = 1;
	uint32_t preparedFrame = UINT32_MAX;
	uint32_t dispatchedFrame = UINT32_MAX;
	uint32_t inputSlot = UINT32_MAX;
	uint32_t outputSlot = UINT32_MAX;
	uint32_t depthSlot = UINT32_MAX;
	uint32_t routeWidth = 0;
	uint32_t routeHeight = 0;
	uint32_t routePlacement = 0;
	uint32_t exposureDomain = 0;
	const char* resetReason = "initial";
};

class NRISmokeSystem
{
public:
	bool PrepareFrame(NRIRenderer& renderer, bool mainViewEligible);
	bool DispatchRoute(NRIRenderer& renderer, const NRISmokeRouteDesc& route);
	void Reset(const char* reason);
	void Shutdown();
	void PrintStatus(const NRIRenderer& renderer) const;
	const NRISmokeStatusSnapshot& GetStatusSnapshot() const { return mStatus; }

private:
	NRISmokeSettings mSettings = {};
	NRISmokeStatusSnapshot mStatus = {};
};
