#include "nri_smoke_analytic_carriers.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
void Require(bool condition, const char* message)
{
	if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

NRISmokeAnalyticCarrierRequest Request(uint32_t sourceId, uint32_t epoch,
	double authoredSeconds = 10.0)
{
	NRISmokeAnalyticCarrierRequest request = {};
	request.position[0] = 1.0f;
	request.position[1] = 2.0f;
	request.position[2] = 3.0f;
	request.initialRadius = 2.0f;
	request.velocity[0] = 4.0f;
	request.initialDensity = 8.0f;
	request.expansionVelocity = 2.0f;
	request.densityHalfLife = 1.0f;
	request.lifetimeSeconds = 3.0f;
	request.styleIndex = 5u;
	request.sourceId = sourceId;
	request.epoch = epoch;
	request.authoredGameplaySeconds = authoredSeconds;
	return request;
}
}

int main()
{
	NRISmokeAnalyticCarriers owner;
	owner.Reset(7u);
	auto unprepared = owner.Admit(Request(1u, 7u));
	Require(unprepared.dropReason == NRISmokeAnalyticCarrierDropReason::NotPrepared,
		"admission before frame preparation must fail explicitly");

	owner.BeginFrame(10.0, 2u);
	auto first = owner.Admit(Request(1u, 7u));
	auto second = owner.Admit(Request(2u, 7u));
	Require(first.Accepted() && second.Accepted() && owner.IsLive(first.handle) &&
		owner.GetSnapshot().activeQuantity == 2u,
		"requests within the fixed frame quantity must become live immediately");
	auto overflow = owner.Admit(Request(3u, 7u));
	Require(overflow.dropReason == NRISmokeAnalyticCarrierDropReason::Capacity &&
		owner.GetSnapshot().droppedCapacity == 1u,
		"capacity overflow must be dropped instead of queued");

	owner.BeginFrame(11.5, 2u);
	const auto& evolved = owner.GetGpuCarriers();
	Require(evolved.size() == 2u && std::abs(evolved[0].position[0] - 7.0f) < 1e-5f &&
		std::abs(evolved[0].radius - 5.0f) < 1e-5f &&
		std::abs(evolved[0].densityScale - 8.0f * std::exp2(-1.5f) *
			(2.0f * 2.0f * 2.0f) / (5.0f * 5.0f * 5.0f)) < 1e-5f &&
		(evolved[0].flags & 1u) != 0u,
		"GPU records must evolve with half-life decay and expansion-volume dilution");
	Require(((evolved[0].flags & 0xfeu) >> 1u) == first.handle.slot &&
		(evolved[0].flags >> 8u) == first.handle.generation,
		"GPU records must preserve stable physical slot and generation identity");

	owner.BeginFrame(13.0, 2u);
	Require(owner.GetSnapshot().activeQuantity == 0u &&
		owner.GetSnapshot().expired == 2u && !owner.IsLive(first.handle),
		"carriers must expire at their authored gameplay lifetime");
	auto reused = owner.Admit(Request(4u, 7u, 13.0));
	Require(reused.Accepted() && reused.handle.slot == first.handle.slot &&
		reused.handle.generation != first.handle.generation && !owner.IsLive(first.handle),
		"slot reuse must advance its generation without reviving a stale handle");

	auto staleEpoch = owner.Admit(Request(5u, 6u, 13.0));
	Require(staleEpoch.dropReason == NRISmokeAnalyticCarrierDropReason::StaleEpoch,
		"requests from an old smoke epoch must be rejected distinctly");
	auto expiredArrival = owner.Admit(Request(6u, 7u, 9.0));
	Require(expiredArrival.dropReason == NRISmokeAnalyticCarrierDropReason::ExpiredOnArrival,
		"already-expired requests must never appear late");
	auto late = Request(9u, 7u, 12.0);
	late.maximumLatencySeconds = 0.05f;
	Require(owner.Admit(late).dropReason == NRISmokeAnalyticCarrierDropReason::StaleOnArrival,
		"events outside their freshness window must never appear late");
	auto invalid = Request(7u, 7u, 13.0);
	invalid.densityHalfLife = 0.0f;
	Require(owner.Admit(invalid).dropReason == NRISmokeAnalyticCarrierDropReason::InvalidRequest,
		"invalid analytic shape or lifetime data must have its own rejection category");

	owner.BeginFrame(13.0, 0u);
	Require(owner.Admit(Request(8u, 7u, 13.0)).dropReason ==
		NRISmokeAnalyticCarrierDropReason::Disabled,
		"a zero profile quantity must report disabled instead of capacity overflow");
	Require(owner.IsLive(reused.handle),
		"profile reduction must not evict an already-visible analytic carrier");

	owner.Reset(8u);
	Require(owner.GetSnapshot().epoch == 8u && owner.GetSnapshot().activeQuantity == 0u &&
		owner.GetGpuCarriers().empty() && !owner.IsLive(reused.handle),
		"epoch reset must clear carriers and invalidate every prior handle");
	owner.BeginFrame(20.0, NRISmokeAnalyticCarriers::FixedCarrierCapacity + 100u);
	Require(owner.GetSnapshot().maximumActiveQuantity ==
		NRISmokeAnalyticCarriers::FixedCarrierCapacity,
		"profile quantity must clamp to physical analytic capacity");

	const auto snapshot = owner.GetSnapshot();
	Require(snapshot.requested == 0u && snapshot.admitted == 0u &&
		snapshot.droppedCapacity == 0u,
		"reset must begin a fresh epoch telemetry interval");

	owner.Reset(9u);
	owner.BeginFrame(30.0, 3u);
	auto batchA = Request(10u, 9u, 30.0);
	auto batchB = Request(10u, 9u, 30.0);
	Require(owner.AdmitBatch(&batchA, 1u) == 1u,
		"an analytic batch must publish when its complete quantity fits");
	NRISmokeAnalyticCarrierRequest pair[2] = { batchA, batchB };
	Require(owner.AdmitBatch(pair, 2u) == 2u && owner.GetSnapshot().activeQuantity == 3u,
		"a multi-carrier effect must be admitted atomically when it fits");
	owner.Reset(10u);
	owner.BeginFrame(40.0, 1u);
	pair[0] = Request(11u, 10u, 40.0);
	pair[1] = Request(11u, 10u, 40.0);
	Require(owner.AdmitBatch(pair, 2u) == 0u && owner.GetSnapshot().activeQuantity == 0u &&
		owner.GetSnapshot().droppedCapacity == 2u,
		"capacity pressure must drop a complete multi-carrier effect without partial mass");

	std::cout << "Smoke analytic carrier lifecycle tests passed.\n";
	return 0;
}
