#include "nri_weapon_event_batch.h"

#include "../system/nri_renderdevice.h"

void NRIWeaponEventBatch::Capture(NRIRenderDevice& frameBuffer, uint32_t frameIndex)
{
	if (mFrameIndex == frameIndex)
	{
		return;
	}

	frameBuffer.ConsumePathTracingWeaponLightEvents(mEvents);
	mFrameIndex = frameIndex;
}

uint32_t NRIWeaponEventBatch::Reset()
{
	const uint32_t discardedEventCount = (uint32_t)mEvents.Size();
	mEvents.Clear();
	mFrameIndex = UINT32_MAX;
	return discardedEventCount;
}
