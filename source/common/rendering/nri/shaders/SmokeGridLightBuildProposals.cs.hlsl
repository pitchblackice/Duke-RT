#include "Include/SmokeGridLightingResources.hlsli"
#include "Include/SmokeFroxel.hlsli"
#include "Include/SmokeLighting.hlsli"

bool SmokeGridLightCandidateSphere(EmissivePrimitiveData candidate, out float3 center, out float radius)
{
	center = 0.0;
	radius = 0.0;
	// CPU candidate publication already owns conservative world-space bounds.
	// Placed ranges use the transformed occurrence bound, avoiding a complete
	// primitive-range scan for every candidate/brick pair.
	center = candidate.boundsCenter;
	radius = candidate.boundsRadius;
	return all(isfinite(center)) && isfinite(radius) && radius >= 0.0;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint brickIndex = dispatchThreadId.x;
	uint proposalCapacity, ignoredStride;
	gSmokeGridLightProposals.GetDimensions(proposalCapacity, ignoredStride);
	if (brickIndex >= proposalCapacity)
		return;
	SmokeGridLightProposal output = (SmokeGridLightProposal)0;
	[unroll] for (uint slot = 0u; slot < NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY; ++slot)
		output.CandidateIndices[slot] = 0xffffffffu;
	uint brickCapacity;
	gSmokeRenderGridBricks.GetDimensions(brickCapacity, ignoredStride);
	if (brickIndex >= brickCapacity)
	{
		gSmokeGridLightProposals[brickIndex] = output;
		return;
	}
	const SmokeGridBrick brick = gSmokeRenderGridBricks[brickIndex];
	if (brick.State != NRI_SMOKE_GRID_RESIDENT ||
		(brick.Flags & (NRI_SMOKE_GRID_BRICK_CONTENT | NRI_SMOKE_GRID_BRICK_HALO)) == 0u)
	{
		gSmokeGridLightProposals[brickIndex] = output;
		return;
	}
	uint headerCount, headerStride, candidateCapacity, candidateStride;
	gSmokeEmissivePrimitiveHeaders.GetDimensions(headerCount, headerStride);
	gSmokeEmissivePrimitives.GetDimensions(candidateCapacity, candidateStride);
	const uint candidateCount = headerCount > 0u ? min(gSmokeEmissivePrimitiveHeaders[0].activeCount, candidateCapacity) : 0u;
	const float cellSize = max(asfloat(gSmokeRenderGridControl[0].CellSizeBits), 0.0001);
	const float3 brickCenter = ((float3)(brick.Coordinate * NRI_SMOKE_GRID_BRICK_AXIS) + 4.0) * cellSize;
	const float brickRadius = 6.92820323028 * cellSize;
	float scores[NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY];
	uint indices[NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY];
	[unroll] for (uint slot = 0u; slot < NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY; ++slot)
	{
		scores[slot] = -1.0;
		indices[slot] = 0xffffffffu;
	}
	uint eligibleCount = 0u;
	[loop]
	for (uint candidateIndex = 0u; candidateIndex < candidateCount; ++candidateIndex)
	{
		const EmissivePrimitiveData candidate = gSmokeEmissivePrimitives[candidateIndex];
		if (!isfinite(candidate.selectionPdf) || candidate.selectionPdf <= 0.0 ||
			!isfinite(candidate.emissionScale) ||
			SmokeResolveEmissiveRadianceScale(candidate, candidate.primitiveIndex) <= 0.0)
			continue;
		float3 candidateCenter;
		float candidateRadius;
		if (!SmokeGridLightCandidateSphere(candidate, candidateCenter, candidateRadius))
			continue;
		const float separation = max(length(candidateCenter - brickCenter) - brickRadius - candidateRadius, 0.5 * cellSize);
		const float score = candidate.selectionPdf / max(separation * separation, 1e-6);
		if (!isfinite(score) || score <= 0.0)
			continue;
		eligibleCount++;
		uint insertion = NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY;
		[unroll]
		for (uint slot = 0u; slot < NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY; ++slot)
		{
			if (score > scores[slot] || (score == scores[slot] && candidateIndex < indices[slot]))
			{
				insertion = slot;
				break;
			}
		}
		if (insertion < NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY)
		{
			for (uint slot = NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY - 1u; slot > insertion; --slot)
			{
				scores[slot] = scores[slot - 1u];
				indices[slot] = indices[slot - 1u];
			}
			scores[insertion] = score;
			indices[insertion] = candidateIndex;
		}
	}
	output.Count = min(eligibleCount, NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY);
	output.BrickGeneration = brick.Generation;
	output.SimulationEpoch = gSmokeConstants.SimulationEpoch;
	output.FrameStamp = gSmokeConstants.FrameIndex;
	[unroll] for (uint slot = 0u; slot < NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY; ++slot)
		output.CandidateIndices[slot] = indices[slot];
	gSmokeGridLightProposals[brickIndex] = output;
	InterlockedAdd(gSmokeGridLightControl[0].ProposalListsBuilt, 1u);
	InterlockedAdd(gSmokeGridLightControl[0].ProposalCandidatesTested, candidateCount);
	InterlockedAdd(gSmokeGridLightControl[0].ProposalCandidatesAccepted, output.Count);
	if (eligibleCount > NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY)
		InterlockedAdd(gSmokeGridLightControl[0].ProposalTruncations, eligibleCount - NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY);
	InterlockedMax(gSmokeGridLightControl[0].ProposalMaximumCount, output.Count);
}
