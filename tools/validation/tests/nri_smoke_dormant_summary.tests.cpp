#include "nri_smoke_dormant_summary.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
void Require(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAILED: " << message << '\n';
		std::exit(1);
	}
}

bool Near(float left, float right)
{
	return std::fabs(left - right) <= 0.00001f;
}

NRISmokeDormantSummaryGpu Summary(uint32_t epoch, uint32_t brickGeneration, uint32_t tick)
{
	NRISmokeDormantSummaryGpu result = {};
	result.boundsMax[0] = result.boundsMax[1] = result.boundsMax[2] = 8.0f;
	result.centroid[0] = result.centroid[1] = result.centroid[2] = 4.0f;
	result.velocity[0] = 2.0f;
	result.opticalMass = 8.0f;
	result.thermalMass = 4.0f;
	result.densityMass = 8.0f;
	result.velocityWeight = 8.0f;
	result.sourceId = 17u;
	result.sourceClass = 1u;
	result.lastSimulationTick = tick;
	result.epoch = epoch;
	result.brickGeneration = brickGeneration;
	return result;
}
}

int main()
{
	Require(sizeof(NRISmokeDormantSummaryGpu) == 96u, "CPU summary contract must stay 96 bytes");
	Require(Near(NRISmokeDormantSummaryOwner::ExactDecay(60u, 1.0f / 60.0f, 1.0f), 0.5f),
		"one exact half-life must decay to one half");
	Require(Near(NRISmokeDormantSummaryOwner::ExactDecay(0u, 0.0f, 0.0f), 1.0f),
		"zero elapsed ticks must preserve the scalar exactly");

	NRISmokeDormantSummaryOwner owner(1u);
	owner.Reset(7u);
	Require(!owner.GetStatus().productionEnabled,
		"experimental owner must remain explicitly disconnected from production demotion");
	const auto first = owner.Claim(7u);
	Require(first.IsValid(), "free summary slot must be claimable");
	Require(!owner.Claim(7u).IsValid(), "summary-full must retain fine state by failing the claim");
	Require(owner.GetStatus().claimed == 1u && owner.GetStatus().claimFailures == 1u,
		"claim pressure must be explicit telemetry");

	auto invalid = Summary(7u, 3u, 10u);
	invalid.opticalMass = -1.0f;
	Require(!owner.Publish(first, invalid), "invalid optical moments must not become authoritative");
	Require(owner.GetStatus().claimed == 1u, "failed publication must retain the claim for cancellation");
	Require(owner.Publish(first, Summary(7u, 3u, 10u)), "valid summary publication must succeed");
	Require(owner.GetStatus().archived == 1u && owner.GetStatus().free == 0u,
		"published summary must own its fixed-capacity slot");

	auto none = owner.BeginRehydrate(7u, 70u, 0u, 1.0f / 60.0f, 1.0f, 0.5f);
	Require(none.empty(), "zero fixed quota must produce no rehydrate work");
	auto work = owner.BeginRehydrate(7u, 70u, 1u, 1.0f / 60.0f, 1.0f, 0.5f);
	Require(work.size() == 1u, "fixed quota must select one archived row");
	Require(Near(work[0].summary.opticalMass, 4.0f), "density-like mass must use exact exponential decay");
	Require(Near(work[0].summary.thermalMass, 1.0f), "thermal mass must use its independent half-life");
	Require(owner.CommitRehydrate(work[0].token, false), "failed fine publication must be acknowledged");
	Require(owner.GetStatus().archived == 1u && owner.GetStatus().free == 0u,
		"failed fine publication must retain the coarse authority");

	work = owner.BeginRehydrate(7u, 70u, 1u, 1.0f / 60.0f, 1.0f, 0.5f);
	Require(owner.CommitRehydrate(work[0].token, true), "successful fine publication must commit");
	Require(owner.GetStatus().free == 1u && owner.GetStatus().rehydrateCommits == 1u,
		"only successful fine publication may free a summary");

	const auto stale = owner.Claim(7u);
	owner.Reset(8u);
	Require(!owner.Publish(stale, Summary(7u, 4u, 1u)), "prior-epoch token must be rejected");
	Require(!owner.Claim(7u).IsValid(), "claim must use the current epoch");
	const auto current = owner.Claim(8u);
	Require(current.IsValid() && owner.CancelClaim(current), "current claim must remain cancellable");

	NRISmokeDormantSummaryOwner progressive(3u);
	progressive.Reset(9u);
	for (uint32_t index = 0u; index < 3u; ++index)
	{
		const auto token = progressive.Claim(9u);
		Require(token.IsValid() && progressive.Publish(token, Summary(9u, index + 1u, 0u)),
			"progressive fixture summary must publish");
	}
	auto firstBatch = progressive.BeginRehydrate(9u, 1u, 2u, 1.0f, 10.0f, 10.0f);
	Require(firstBatch.size() == 2u && firstBatch[0].token.slot == 0u && firstBatch[1].token.slot == 1u,
		"fixed rehydrate quota must select adjacent rows without cursor skipping");
	Require(progressive.CancelRehydrate(firstBatch[0].token) && progressive.CancelRehydrate(firstBatch[1].token),
		"cancelled work must return both summaries to the archive");
	auto secondBatch = progressive.BeginRehydrate(9u, 1u, 1u, 1.0f, 10.0f, 10.0f);
	Require(secondBatch.size() == 1u && secondBatch[0].token.slot == 2u,
		"progressive cursor must give the next archived row service");
	Require(progressive.CancelRehydrate(secondBatch[0].token), "final fixture row must remain recoverable");

	std::cout << "Smoke dormant-summary transaction tests passed.\n";
	return 0;
}
