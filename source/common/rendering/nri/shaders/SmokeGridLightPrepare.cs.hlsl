#include "Include/SmokeGridLightingResources.hlsli"

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	(void)dispatchThreadId;
	SmokeGridLightControl control = (SmokeGridLightControl)0;
	control.FrameStamp = gSmokeConstants.FrameIndex;
	control.SimulationEpoch = gSmokeConstants.SimulationEpoch;
	control.FieldPing = (gSmokeConstants.Flags & NRI_SMOKE_GRID_LIGHT_FIELD_PING) != 0u ? 1u : 0u;
	control.RadiancePartitionCount = max(gSmokeConstants.ParticleCapacity, 1u);
	control.RadianceNewInvalidQuantity = max(gSmokeConstants.StyleCount, 1u);
	control.RadianceMaintenanceQuantity = max(gSmokeConstants.FroxelWidth, 1u);
	control.RadianceMaximumAge = max(gSmokeConstants.FroxelHeight, 1u);
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
		gSmokeControl[0].EmissiveInnerSourceVisibilityRays = 0u;
		gSmokeControl[0].EmissiveInnerVisibilityVisible = 0u;
		gSmokeControl[0].EmissiveInnerBlockerReceiverImmediate = 0u;
		gSmokeControl[0].EmissiveInnerBlockerReceiverCell = 0u;
		gSmokeControl[0].EmissiveInnerBlockerEmitterCell = 0u;
		gSmokeControl[0].EmissiveInnerBlockerInterior = 0u;
		gSmokeControl[0].EmissiveInnerSourceSelections = 0u;
		gSmokeControl[0].EmissiveInnerSourceOverflow = 0u;
		gSmokeControl[0].EmissiveTargetVisibilityRays = 0u;
		gSmokeControl[0].EmissiveTargetVisibilityVisible = 0u;
		gSmokeControl[0].EmissiveTargetBlockerExact = 0u;
		gSmokeControl[0].EmissiveTargetBlockerRange = 0u;
		gSmokeControl[0].EmissiveTargetBlockerOther = 0u;
		gSmokeControl[0].EmissiveTargetWitnessClaim = 0u;
		gSmokeControl[0].EmissiveTargetWitnessCandidate = 0xffffffffu;
		gSmokeControl[0].EmissiveTargetWitnessRelation = 0u;
		gSmokeControl[0].EmissiveTargetWitnessSamplePrimitive = 0xffffffffu;
		gSmokeControl[0].EmissiveTargetWitnessSampleMaterial = 0xffffffffu;
		gSmokeControl[0].EmissiveTargetWitnessBlockerDataSource = 0xffffffffu;
		gSmokeControl[0].EmissiveTargetWitnessBlockerInstance = 0xffffffffu;
		gSmokeControl[0].EmissiveTargetWitnessBlockerPrimitive = 0xffffffffu;
		gSmokeControl[0].EmissiveTargetWitnessBlockerMaterial = 0xffffffffu;
		gSmokeControl[0].EmissiveTargetWitnessDistanceBits = 0u;
	}
}
