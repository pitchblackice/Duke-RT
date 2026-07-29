#include "nri_smoke_analytic_trail_bridge.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
void Require(bool condition, const char* message)
{
	if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

NRISmokeAnalyticTrailObservation Observation(uint64_t key, uint64_t ordinal,
	double authoredSeconds = 10.0)
{
	NRISmokeAnalyticTrailObservation value = {};
	value.stableSourceKey = key;
	value.updateOrdinal = ordinal;
	value.epoch = 7u;
	value.sourceId = static_cast<uint32_t>(key);
	value.styleIndex = 4u;
	value.authoredGameplaySeconds = authoredSeconds;
	value.velocity[0] = 2.0f;
	value.radius = 2.0f;
	value.densityScale = 3.0f;
	value.expansionVelocity = 1.0f;
	value.densityHalfLife = 2.0f;
	value.presentationLifetimeSeconds = 0.25f;
	value.maximumLatencySeconds = 0.05f;
	value.maximumSegmentLength = 5.0f;
	return value;
}

NRISmokeAnalyticTrailPoint Point(float x, uint32_t ranges = 2u)
{
	NRISmokeAnalyticTrailPoint point = {};
	point.position[0] = x;
	point.rangeCount = ranges;
	return point;
}
}

int main()
{
	NRISmokeAnalyticTrailBridge owner;
	owner.Reset(7u);
	owner.BeginFrame(10.0, 7u);
	const NRISmokeAnalyticTrailPoint points[] = {
		Point(0.0f), Point(3.0f), Point(6.0f), Point(9.0f)
	};
	Require(owner.Observe(Observation(100u, 1u), points, 4u) ==
		NRISmokeAnalyticTrailObserveResult::Accepted,
		"a valid transitory projectile update must be accepted");
	Require(owner.GetPresentations().empty() && owner.PublishFallback(100u, 1u, 7u),
		"a bridge must remain hidden until exact-grid fallback is reported");
	const auto first = owner.GetPresentations()[0];
	Require(owner.GetPresentations().size() == 1u && first.coalescedPointCount == 2u &&
		std::abs(first.carrier.position[0] - 7.5f) < 1e-5f &&
		std::abs(first.carrier.halfAxisU[0] - 1.5f) < 1e-5f &&
		first.carrier.shape == 1u && first.carrier.rangeCount == 4u,
		"one update must coalesce only its newest bounded points into one rounded segment");
	const float sphereVolume = (4.0f * 3.14159265359f / 3.0f) * 8.0f;
	const float capsuleVolume = 2.0f * 3.14159265359f * 1.5f * 4.0f + sphereVolume;
	Require(std::abs(first.carrier.initialDensity - 3.0f * sphereVolume /
		capsuleVolume) < 1e-5f,
		"coalescing must divide density by rounded-segment support volume");

	const NRISmokeAnalyticTrailPoint newer[] = { Point(10.0f), Point(12.0f) };
	Require(owner.Observe(Observation(100u, 2u, 10.01), newer, 2u) ==
		NRISmokeAnalyticTrailObserveResult::Accepted,
		"a newer update for the same projectile must replace its presentation");
	const auto replaced = owner.GetPresentations()[0];
	Require(owner.GetPresentations().size() == 1u &&
		replaced.segmentRevision != first.segmentRevision &&
		replaced.lightingGroupKey == first.lightingGroupKey &&
		replaced.lightingGroupGeneration == first.lightingGroupGeneration &&
		owner.GetSnapshot().replacements == 1u,
		"replacement must retain one stable source lighting group without accumulating carriers");
	Require(owner.Observe(Observation(100u, 1u, 10.02), newer, 2u) ==
		NRISmokeAnalyticTrailObserveResult::StaleUpdate &&
		owner.GetPresentations()[0].segmentRevision == replaced.segmentRevision,
		"out-of-order updates must not roll back newest-only authority");

	auto persistent = Observation(200u, 1u);
	persistent.persistentSource = true;
	Require(owner.Observe(persistent, points, 4u) ==
		NRISmokeAnalyticTrailObserveResult::PersistentSource,
		"persistent smoke must fail closed outside the transitory trail bridge");

	auto token = owner.BeginExactGridHandoff(100u, replaced.segmentRevision);
	Require(token.Valid() && owner.GetPresentations().size() == 1u,
		"beginning a handoff must retain analytic authority until exact grid acknowledgement");
	NRISmokeAnalyticTrailGridReceipt wrong = {};
	wrong.stableSourceKey = 100u;
	wrong.segmentRevision = replaced.segmentRevision;
	wrong.epoch = 7u;
	wrong.depositedRangeCount = replaced.carrier.rangeCount - 1u;
	wrong.gridAuthorityGeneration = 3u;
	Require(!owner.CommitExactGridHandoff(token, wrong) &&
		owner.GetPresentations().size() == 1u,
		"partial grid deposition must not retire analytic authority");
	NRISmokeAnalyticTrailGridReceipt exact = wrong;
	exact.depositedRangeCount = replaced.carrier.rangeCount;
	Require(owner.CommitExactGridHandoff(token, exact) && owner.GetPresentations().empty(),
		"an exact source/revision/range receipt must atomically retire analytic authority");

	Require(owner.Observe(Observation(300u, 1u, 10.03), points, 4u) ==
		NRISmokeAnalyticTrailObserveResult::Accepted,
		"a second source must be independently admitted");
	Require(owner.PublishFallback(300u, 1u, 7u),
		"the second source must publish only after fallback");
	const auto beforeReplacement = owner.GetPresentations()[0];
	auto staleToken = owner.BeginExactGridHandoff(300u, beforeReplacement.segmentRevision);
	Require(owner.Observe(Observation(300u, 2u, 10.04), newer, 2u) ==
		NRISmokeAnalyticTrailObserveResult::Accepted &&
		!owner.CommitExactGridHandoff(staleToken, exact),
		"a newer spatial update must invalidate an older in-flight handoff token");
	Require(owner.RetireSource(300u, 7u) && owner.GetPresentations().empty(),
		"projectile retirement must remove its bounded analytic presentation immediately");

	auto cappedLifetime = Observation(400u, 1u, 20.0);
	cappedLifetime.presentationLifetimeSeconds = 20.0f;
	owner.BeginFrame(20.0, 7u);
	Require(owner.Observe(cappedLifetime, points, 4u) ==
		NRISmokeAnalyticTrailObserveResult::Accepted &&
		owner.PublishFallback(400u, 1u, 7u) &&
		std::abs(owner.GetPresentations()[0].carrier.lifetimeSeconds -
			NRISmokeAnalyticTrailBridge::MaximumPresentationLifetimeSeconds) < 1e-6f,
		"authored trail lifetime must clamp to the bridge's newest-only bound");
	owner.BeginFrame(20.5, 7u);
	Require(owner.GetPresentations().empty() && owner.GetSnapshot().expired == 1u,
		"a source that stops updating must expire at the bounded gameplay lifetime");

	std::vector<NRISmokeAnalyticTrailPoint> manyPoints;
	for (uint32_t index = 0u; index < 20u; ++index) manyPoints.push_back(Point((float)index));
	auto many = Observation(500u, 1u, 30.0);
	many.maximumSegmentLength = 100.0f;
	owner.BeginFrame(30.0, 7u);
	Require(owner.Observe(many, manyPoints.data(), manyPoints.size()) ==
		NRISmokeAnalyticTrailObserveResult::Accepted &&
		owner.PublishFallback(500u, 1u, 7u) &&
		owner.GetPresentations()[0].coalescedPointCount ==
			NRISmokeAnalyticTrailBridge::MaximumPointsPerUpdate &&
		owner.GetPresentations()[0].carrier.rangeCount == 32u,
		"one update must retain only the fixed newest point quantity");

	owner.BeginFrame(30.0, 8u);
	Require(owner.GetPresentations().empty() && owner.GetSnapshot().epoch == 8u &&
		owner.GetSnapshot().activeSources == 0u,
		"epoch changes must invalidate every projectile and lighting generation");

	std::cout << "Smoke analytic trail bridge tests passed.\n";
	return 0;
}
