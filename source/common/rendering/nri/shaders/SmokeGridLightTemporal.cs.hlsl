#include "Include/SmokeGridLightingResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint activeCapacity, ignoredStride;
	gSmokeGridLightActive.GetDimensions(activeCapacity, ignoredStride);
	if (dispatchThreadId.x >= min(gSmokeGridLightControl[0].ActiveCount, activeCapacity))
		return;
	const uint cellIndex = gSmokeGridLightActive[dispatchThreadId.x];
	const uint brickIndex = cellIndex / NRI_SMOKE_GRID_CELLS_PER_BRICK;
	const SmokeGridBrick brick = gSmokeRenderGridBricks[brickIndex];
	const bool targetHistory = (gSmokeConstants.Flags & NRI_SMOKE_GRID_LIGHT_FIELD_PING) != 0u;
	SmokeGridLightRecord current;
	SmokeGridLightRecord history;
	if (targetHistory)
	{
		current = gSmokeGridLightHistory[cellIndex];
		history = gSmokeGridLightCurrent[cellIndex];
	}
	else
	{
		current = gSmokeGridLightCurrent[cellIndex];
		history = gSmokeGridLightHistory[cellIndex];
	}
	if (!SmokeGridLightRecordValid(current, brick.Generation, gSmokeConstants.SimulationEpoch))
		return;
	// Seed already copied compatible history for unscheduled partition work.
	// Its positive age is explicit incomplete-radiance state, not a new sample.
	if (SmokeGridLightAge(current) > 0u)
		return;
	uint outputCount = SmokeGridLightSampleCount(current);
	if (SmokeGridLightRecordValid(history, brick.Generation, gSmokeConstants.SimulationEpoch))
	{
		const uint historyCount = SmokeGridLightSampleCount(history);
		const float alpha = historyCount < NRI_SMOKE_GRID_LIGHT_MAX_HISTORY ?
			(float)outputCount / (float)max(historyCount + outputCount, 1u) : rcp((float)NRI_SMOKE_GRID_LIGHT_MAX_HISTORY);
		[unroll]
		for (uint lobe = 0u; lobe < 6u; ++lobe)
		{
			const float3 mean = lerp(SmokeGridLightMean(history, lobe), SmokeGridLightMean(current, lobe), alpha);
			const float3 second = max(lerp(SmokeGridLightSecondMoment(history, lobe), SmokeGridLightSecondMoment(current, lobe), alpha), mean * mean);
			SmokeGridLightStoreLobe(current, lobe, mean, second);
		}
		outputCount = min(historyCount + outputCount, NRI_SMOKE_GRID_LIGHT_MAX_HISTORY);
		InterlockedAdd(gSmokeGridLightControl[0].TemporalAccepted, 1u);
	}
	else
		InterlockedAdd(gSmokeGridLightControl[0].TemporalRejected, 1u);
	SmokeGridLightSetMetadata(current, brick.Generation, gSmokeConstants.SimulationEpoch, outputCount,
		gSmokeConstants.FrameIndex & 0xffu, (float)outputCount / 64.0, SmokeGridLightEvidence(current),
		gSmokeConstants.FrameIndex, 0u);
	if (targetHistory) gSmokeGridLightHistory[cellIndex] = current; else gSmokeGridLightCurrent[cellIndex] = current;

	if (!SmokeSelfShadowEnabled(gSmokeConstants.DebugMode))
		return;
	SmokeGridLightRecord shadowCurrent;
	SmokeGridLightRecord shadowHistory;
	if (targetHistory)
	{
		shadowCurrent = gSmokeGridLightSelfShadowHistory[cellIndex];
		shadowHistory = gSmokeGridLightSelfShadowCurrent[cellIndex];
	}
	else
	{
		shadowCurrent = gSmokeGridLightSelfShadowCurrent[cellIndex];
		shadowHistory = gSmokeGridLightSelfShadowHistory[cellIndex];
	}
	if (!SmokeGridLightRecordValid(shadowCurrent, brick.Generation, gSmokeConstants.SimulationEpoch))
		return;
	uint shadowAge = 0u;
	const uint shadowBlock = SmokeGridLightSelfShadowBlock(shadowCurrent);
	float resolvedTransmittance = SmokeGridLightMeanTransmittance(shadowCurrent);
	const bool consecutive = SmokeGridLightLastUpdate(shadowHistory) == ((gSmokeConstants.FrameIndex - 1u) & 0xffffu);
	const bool sameBlock = SmokeGridLightSelfShadowBlock(shadowHistory) == SmokeGridLightSelfShadowBlock(shadowCurrent);
	if (SmokeGridLightRecordValid(shadowHistory, brick.Generation, gSmokeConstants.SimulationEpoch) &&
		consecutive && sameBlock && SmokeGridLightAge(shadowHistory) < 7u)
	{
		shadowAge = SmokeGridLightAge(shadowHistory) + 1u;
		const float alpha = rcp((float)(shadowAge + 1u));
		[unroll]
		for (uint lobe = 0u; lobe < 6u; ++lobe)
		{
			const float3 mean = lerp(SmokeGridLightMean(shadowHistory, lobe), SmokeGridLightMean(shadowCurrent, lobe), alpha);
			const float3 second = max(lerp(SmokeGridLightSecondMoment(shadowHistory, lobe),
				SmokeGridLightSecondMoment(shadowCurrent, lobe), alpha), mean * mean);
			SmokeGridLightStoreLobe(shadowCurrent, lobe, mean, second);
		}
		resolvedTransmittance = lerp(SmokeGridLightMeanTransmittance(shadowHistory),
			SmokeGridLightMeanTransmittance(shadowCurrent), alpha);
		InterlockedAdd(gSmokeGridLightControl[0].SelfShadowHistoryAccepted, 1u);
		InterlockedMax(gSmokeGridLightControl[0].SelfShadowMaximumAge, shadowAge);
	}
	else
		InterlockedAdd(gSmokeGridLightControl[0].SelfShadowHistoryRestarted, 1u);
	SmokeGridLightSetMetadata(shadowCurrent, brick.Generation, gSmokeConstants.SimulationEpoch, shadowAge + 1u,
		gSmokeConstants.FrameIndex & 0xffu, (float)(shadowAge + 1u) / 8.0, SmokeGridLightEvidence(shadowCurrent),
		gSmokeConstants.FrameIndex, shadowAge);
	SmokeGridLightSetSelfShadowEvidence(shadowCurrent, shadowBlock, resolvedTransmittance);
	if (targetHistory) gSmokeGridLightSelfShadowHistory[cellIndex] = shadowCurrent;
	else gSmokeGridLightSelfShadowCurrent[cellIndex] = shadowCurrent;
}
