#pragma once

#include "v_video.h"

#include <cstdint>

class NRIRenderDevice;

class NRIWeaponEventBatch
{
public:
	void Capture(NRIRenderDevice& frameBuffer, uint32_t frameIndex);
	uint32_t Reset();

	const TArray<PathTracingWeaponLightEvent>& Events() const { return mEvents; }
	uint32_t FrameIndex() const { return mFrameIndex; }

private:
	TArray<PathTracingWeaponLightEvent> mEvents;
	uint32_t mFrameIndex = UINT32_MAX;
};
