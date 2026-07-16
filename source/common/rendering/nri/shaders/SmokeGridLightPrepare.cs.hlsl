#include "Include/SmokeGridLightingResources.hlsli"

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	(void)dispatchThreadId;
	SmokeGridLightControl control = (SmokeGridLightControl)0;
	control.FrameStamp = gSmokeConstants.FrameIndex;
	control.SimulationEpoch = gSmokeConstants.SimulationEpoch;
	control.FieldPing = (gSmokeConstants.Flags & NRI_SMOKE_GRID_LIGHT_FIELD_PING) != 0u ? 1u : 0u;
	gSmokeGridLightControl[0] = control;
	uint smokeControlCount, ignoredStride;
	gSmokeControl.GetDimensions(smokeControlCount, ignoredStride);
	if (smokeControlCount != 0u)
	{
		gSmokeControl[0].EmissiveInnerRisSets = 0u;
		gSmokeControl[0].EmissiveInnerPointProposals = 0u;
		gSmokeControl[0].EmissiveInnerZeroProposals = 0u;
		gSmokeControl[0].EmissiveInnerRisRejects = 0u;
		gSmokeControl[0].EmissiveInnerSelections = 0u;
		gSmokeControl[0].EmissiveInnerVisibilityRays = 0u;
	}
}
