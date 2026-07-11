#include "nri_smoke.h"

#include "nri_pass_dispatch.h"
#include "nri_renderer.h"
#include "printf.h"

bool NRISmokeSystem::PrepareFrame(NRIRenderer& renderer, bool mainViewEligible)
{
	mSettings = BuildNRISmokeSettingsFromCVars();
	mStatus.enabled = mSettings.enabled;
	mStatus.mainViewEligible = mainViewEligible;
	mStatus.preparedFrame = renderer.mFrameIndex;
	return true;
}

bool NRISmokeSystem::DispatchRoute(NRIRenderer& renderer, const NRISmokeRouteDesc& route)
{
	mStatus.enabled = mSettings.enabled;
	mStatus.routeSupported = route.supported;
	mStatus.dispatchedFrame = renderer.mFrameIndex;
	mStatus.inputSlot = (uint32_t)route.inputSlot;
	mStatus.outputSlot = (uint32_t)route.outputSlot;
	mStatus.depthSlot = (uint32_t)route.depthSlot;
	mStatus.routeWidth = route.width;
	mStatus.routeHeight = route.height;
	mStatus.routePlacement = (uint32_t)route.placement;
	mStatus.exposureDomain = (uint32_t)route.exposureDomain;

	if (!route.supported)
	{
		return true;
	}

	// Phase 0 deliberately preserves the pre-smoke route even when the master
	// CVar is enabled. Visual dispatch replaces this copy in Phase 2.
	renderer.CopyTexture(renderer.GetFrameTexture(route.inputSlot), renderer.GetFrameTexture(route.outputSlot));
	return true;
}

void NRISmokeSystem::Reset(const char* reason)
{
	mStatus.simulationEpoch++;
	if (mStatus.simulationEpoch == 0)
	{
		mStatus.simulationEpoch = 1;
	}
	mStatus.preparedFrame = UINT32_MAX;
	mStatus.dispatchedFrame = UINT32_MAX;
	mStatus.resetReason = reason != nullptr ? reason : "unspecified";
}

void NRISmokeSystem::Shutdown()
{
	Reset("renderer-shutdown");
}

void NRISmokeSystem::PrintStatus(const NRIRenderer& renderer) const
{
	const NRISmokeStatusSnapshot& status = mStatus;
	const char* placement = status.routePlacement == (uint32_t)NRISmokeRoutePlacement::DlrrPostUpscale ? "dlrr_post_upscale" : "standard_pre_upscale";
	const char* inputName = status.inputSlot < (uint32_t)NRIRenderer::FrameTextureSlot::Count ? renderer.GetFrameTextureSlotName((NRIRenderer::FrameTextureSlot)status.inputSlot) : "none";
	const char* outputName = status.outputSlot < (uint32_t)NRIRenderer::FrameTextureSlot::Count ? renderer.GetFrameTextureSlotName((NRIRenderer::FrameTextureSlot)status.outputSlot) : "none";
	const char* depthName = status.depthSlot < (uint32_t)NRIRenderer::FrameTextureSlot::Count ? renderer.GetFrameTextureSlotName((NRIRenderer::FrameTextureSlot)status.depthSlot) : "none";
	Printf("NRI PT smoke status: enabled=%s phase=route-seam epoch=%u prepared_frame=%u dispatched_frame=%u main_view=%s route_supported=%s placement=%s input=%s output=%s depth=%s extent=%ux%u exposure=%u reset=%s\n",
		status.enabled ? "yes" : "no",
		status.simulationEpoch,
		status.preparedFrame,
		status.dispatchedFrame,
		status.mainViewEligible ? "yes" : "no",
		status.routeSupported ? "yes" : "no",
		placement,
		inputName,
		outputName,
		depthName,
		status.routeWidth,
		status.routeHeight,
		status.exposureDomain,
		status.resetReason);
}
