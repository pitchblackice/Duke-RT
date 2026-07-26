#include "nri_voxel_representation_policy.h"

#include <cstdint>
#include <iostream>

namespace
{
	NRIVoxelRepresentationFrameInput BuildFrame(uint32_t frameIndex, uint64_t mapBuildSerial = 7ull)
	{
		NRIVoxelRepresentationFrameInput frame = {};
		frame.mapBuildSerial = mapBuildSerial;
		frame.frameIndex = frameIndex;
		frame.renderWidth = 100u;
		frame.renderHeight = 100u;
		frame.cameraForward = { 0.0f, 0.0f, 1.0f };
		frame.cameraRight = { 1.0f, 0.0f, 0.0f };
		frame.cameraUp = { 0.0f, 1.0f, 0.0f };
		return frame;
	}

	NRIVoxelRepresentationFrameInput BuildProxyFrame(uint32_t frameIndex, uint32_t transitionLimit = 8u)
	{
		NRIVoxelRepresentationFrameInput frame = BuildFrame(frameIndex);
		frame.shadowProxyRouteEnabled = true;
		frame.shadowProxyTransitionsPerFrame = transitionLimit;
		return frame;
	}

	NRIVoxelRepresentationFacts BuildFacts()
	{
		NRIVoxelRepresentationFacts facts = {};
		facts.sourceIdentityKey = 0x1234ull;
		facts.meshResourceKey = 0x2345ull;
		facts.materialKeyHash = 0x3456ull;
		facts.actorIndex = 41;
		facts.resolvedVoxelIndex = 9;
		facts.primitiveCount = 321u;
		facts.retainedFrameAge = 8u;
		facts.workloadMask = (uint8_t)(
			NRI_TLAS_MASK_MAIN |
			NRI_TLAS_MASK_GI |
			NRI_TLAS_MASK_EMISSIVE);
		facts.capturedThisFrame = true;
		facts.routedThroughSharedBlas = true;
		facts.boundsValid = true;
		facts.boundsMin = { -1.0f, -1.0f, 9.0f };
		facts.boundsMax = { 1.0f, 1.0f, 11.0f };
		return facts;
	}

	NRIVoxelRepresentationFacts BuildProxyFacts()
	{
		NRIVoxelRepresentationFacts facts = BuildFacts();
		facts.workloadMask |= NRI_TLAS_MASK_SHADOW | NRI_TLAS_MASK_REFLECTION;
		facts.shadowProxyCertified = true;
		facts.shadowProxyReady = true;
		facts.shadowProxyPrimitiveCount = 40u;
		facts.shadowProxyCompatibilityKey = 0xabcdefull;
		return facts;
	}

	bool ExactDecisionPreservesWorkload()
	{
		NRIVoxelRepresentationPolicy policy;
		policy.BeginFrame(BuildFrame(10u));
		const NRIVoxelRepresentationFacts facts = BuildFacts();
		const NRIVoxelRepresentationDecision decision = policy.EvaluateExact(facts);
		return decision.representation == NRIVoxelRepresentationKind::Exact &&
			decision.reason == NRIVoxelRepresentationReason::ExactOnly &&
			decision.requestedWorkloadMask == facts.workloadMask &&
			decision.exactWorkloadMask == facts.workloadMask &&
			decision.proxyWorkloadMask == 0u &&
			!decision.proxyEligible &&
			!decision.proxyReady &&
			!decision.transitionReady;
	}

	bool RayFamiliesAreAttributedSeparately()
	{
		NRIVoxelRepresentationPolicy policy;
		policy.BeginFrame(BuildFrame(10u));
		const NRIVoxelRepresentationDecision decision = policy.EvaluateExact(BuildFacts());
		return decision.primaryWorkloadMask == NRI_TLAS_MASK_MAIN &&
			decision.shadowWorkloadMask == 0u &&
			decision.reflectionWorkloadMask == 0u &&
			decision.giWorkloadMask == NRI_TLAS_MASK_GI &&
			decision.emissiveWorkloadMask == NRI_TLAS_MASK_EMISSIVE &&
			decision.debugWorkloadMask == 0u;
	}

	bool ProjectedBoundsAreRecorded()
	{
		NRIVoxelRepresentationPolicy policy;
		policy.BeginFrame(BuildFrame(10u));
		const NRIVoxelRepresentationDecision decision = policy.EvaluateExact(BuildFacts());
		return decision.projectedBounds.valid &&
			decision.projectedBounds.intersectsViewport &&
			decision.projectedBounds.minX < 50.0f &&
			decision.projectedBounds.maxX > 50.0f &&
			decision.projectedBounds.minY < 50.0f &&
			decision.projectedBounds.maxY > 50.0f &&
			decision.projectedBounds.areaPixels > 0.0f &&
			decision.projectedBounds.nearestDepth == 9.0f;
	}

	bool HysteresisIsObservationalOnly()
	{
		NRIVoxelRepresentationPolicy policy;
		NRIVoxelRepresentationDecision decision = {};
		for (uint32_t frameIndex = 10u; frameIndex <= 12u; ++frameIndex)
		{
			policy.BeginFrame(BuildFrame(frameIndex));
			decision = policy.EvaluateExact(BuildFacts());
		}
		return decision.framesInExactState == 3u &&
			decision.consecutiveProjectedFrames == 3u &&
			decision.hysteresisObservationReady &&
			decision.transitionCount == 0u &&
			!decision.proxyEligible &&
			!decision.proxyReady &&
			!decision.transitionReady;
	}

	bool ActorMotionPreservesDecisionIdentity()
	{
		NRIVoxelRepresentationPolicy policy;
		NRIVoxelRepresentationFacts facts = BuildFacts();
		policy.BeginFrame(BuildFrame(10u));
		const NRIVoxelRepresentationDecision before = policy.EvaluateExact(facts);
		facts.transform[3] = 20.0f;
		policy.BeginFrame(BuildFrame(11u));
		const NRIVoxelRepresentationDecision after = policy.EvaluateExact(facts);
		return before.decisionIdentity == after.decisionIdentity &&
			after.framesInExactState == 2u;
	}

	bool SourceIdentityChangeResetsReadiness()
	{
		NRIVoxelRepresentationPolicy policy;
		NRIVoxelRepresentationFacts facts = BuildFacts();
		policy.BeginFrame(BuildFrame(10u));
		policy.EvaluateExact(facts);
		policy.BeginFrame(BuildFrame(11u));
		policy.EvaluateExact(facts);
		facts.sourceIdentityKey = 0x9876ull;
		policy.BeginFrame(BuildFrame(12u));
		const NRIVoxelRepresentationDecision restored = policy.EvaluateExact(facts);
		return restored.framesInExactState == 1u &&
			restored.consecutiveProjectedFrames == 1u &&
			!restored.hysteresisObservationReady;
	}

	bool MapGenerationChangeResetsReadiness()
	{
		NRIVoxelRepresentationPolicy policy;
		policy.BeginFrame(BuildFrame(10u, 7ull));
		policy.EvaluateExact(BuildFacts());
		policy.BeginFrame(BuildFrame(11u, 7ull));
		policy.EvaluateExact(BuildFacts());
		policy.BeginFrame(BuildFrame(12u, 8ull));
		const NRIVoxelRepresentationDecision changedMap = policy.EvaluateExact(BuildFacts());
		return changedMap.framesInExactState == 1u &&
			changedMap.consecutiveProjectedFrames == 1u;
	}

	bool StaleActorStateIsPruned()
	{
		NRIVoxelRepresentationPolicy policy;
		policy.BeginFrame(BuildFrame(10u));
		policy.EvaluateExact(BuildFacts());
		policy.BeginFrame(BuildFrame(12u));
		const NRIVoxelRepresentationDecision reappeared = policy.EvaluateExact(BuildFacts());
		return reappeared.framesInExactState == 1u &&
			reappeared.consecutiveProjectedFrames == 1u;
	}

	bool ProjectionFailuresStayExact()
	{
		NRIVoxelRepresentationPolicy policy;
		policy.BeginFrame(BuildFrame(10u));
		NRIVoxelRepresentationFacts facts = BuildFacts();
		facts.boundsValid = false;
		const NRIVoxelRepresentationDecision missingBounds = policy.EvaluateExact(facts);
		facts.boundsValid = true;
		facts.boundsMin = { -1.0f, -1.0f, -11.0f };
		facts.boundsMax = { 1.0f, 1.0f, -9.0f };
		facts.actorIndex++;
		const NRIVoxelRepresentationDecision behindCamera = policy.EvaluateExact(facts);
		return missingBounds.representation == NRIVoxelRepresentationKind::Exact &&
			missingBounds.reason == NRIVoxelRepresentationReason::MissingBounds &&
			behindCamera.representation == NRIVoxelRepresentationKind::Exact &&
			behindCamera.reason == NRIVoxelRepresentationReason::BehindCamera;
	}

	bool SnapshotReportsExactOnlyState()
	{
		NRIVoxelRepresentationPolicy policy;
		policy.BeginFrame(BuildFrame(10u));
		policy.EvaluateExact(BuildFacts());
		const NRIVoxelRepresentationSnapshot& snapshot = policy.GetSnapshot();
		return snapshot.mapBuildSerial == 7ull &&
			snapshot.frameIndex == 10u &&
			snapshot.decisionCount == 1u &&
			snapshot.exactDecisionCount == 1u &&
			snapshot.proxyDecisionCount == 0u &&
			snapshot.projectedDecisionCount == 1u &&
			snapshot.viewportIntersectionCount == 1u &&
			snapshot.proxyReadyCount == 0u &&
			snapshot.primaryOccurrenceCount == 1u &&
			snapshot.shadowOccurrenceCount == 0u &&
			snapshot.reflectionOccurrenceCount == 0u &&
			snapshot.giOccurrenceCount == 1u &&
			snapshot.emissiveOccurrenceCount == 1u &&
			snapshot.debugOccurrenceCount == 0u &&
			snapshot.exactPrimitiveCount == 321u &&
			snapshot.decisions.size() == 1u;
	}

	bool CertifiedProxyTransitionsAfterHysteresis()
	{
		NRIVoxelRepresentationPolicy policy;
		NRIVoxelRepresentationDecision decision = {};
		for (uint32_t frameIndex = 10u; frameIndex <= 12u; ++frameIndex)
		{
			policy.BeginFrame(BuildProxyFrame(frameIndex));
			decision = policy.EvaluateExact(BuildProxyFacts());
		}
		const uint8_t requested = BuildProxyFacts().workloadMask;
		return decision.representation == NRIVoxelRepresentationKind::ExactWithCertifiedShadowProxy &&
			decision.reason == NRIVoxelRepresentationReason::CertifiedShadowProxy &&
			decision.proxyWorkloadMask == NRI_TLAS_MASK_SHADOW &&
			(decision.exactWorkloadMask & NRI_TLAS_MASK_SHADOW) == 0u &&
			(decision.exactWorkloadMask | decision.proxyWorkloadMask) == requested &&
			(decision.exactWorkloadMask & decision.proxyWorkloadMask) == 0u &&
			decision.consecutiveProxyReadyFrames == 3u && decision.transitionCount == 1u;
	}

	bool ProxyInvalidationFallsBackImmediately()
	{
		NRIVoxelRepresentationPolicy policy;
		NRIVoxelRepresentationFacts facts = BuildProxyFacts();
		for (uint32_t frameIndex = 10u; frameIndex <= 12u; ++frameIndex)
		{
			policy.BeginFrame(BuildProxyFrame(frameIndex));
			policy.EvaluateExact(facts);
		}
		facts.shadowProxyCertified = false;
		policy.BeginFrame(BuildProxyFrame(13u));
		const NRIVoxelRepresentationDecision decision = policy.EvaluateExact(facts);
		return decision.representation == NRIVoxelRepresentationKind::Exact &&
			decision.exactWorkloadMask == facts.workloadMask &&
			decision.proxyWorkloadMask == 0u && decision.consecutiveProxyReadyFrames == 0u;
	}

	bool ProxyTransitionLimitDefersWithoutDroppingExact()
	{
		NRIVoxelRepresentationPolicy policy;
		NRIVoxelRepresentationFacts first = BuildProxyFacts();
		NRIVoxelRepresentationFacts second = BuildProxyFacts();
		second.actorIndex++;
		second.sourceIdentityKey++;
		for (uint32_t frameIndex = 10u; frameIndex <= 11u; ++frameIndex)
		{
			policy.BeginFrame(BuildProxyFrame(frameIndex, 1u));
			policy.EvaluateExact(first);
			policy.EvaluateExact(second);
		}
		policy.BeginFrame(BuildProxyFrame(12u, 1u));
		const NRIVoxelRepresentationDecision admitted = policy.EvaluateExact(first);
		const NRIVoxelRepresentationDecision deferred = policy.EvaluateExact(second);
		return admitted.representation == NRIVoxelRepresentationKind::ExactWithCertifiedShadowProxy &&
			deferred.representation == NRIVoxelRepresentationKind::Exact &&
			deferred.reason == NRIVoxelRepresentationReason::ProxyTransitionLimited &&
			deferred.exactWorkloadMask == second.workloadMask && deferred.proxyWorkloadMask == 0u;
	}

	bool CompatibilityMutationsResetProxyReadiness()
	{
		const auto mutationResets = [](int mutation) -> bool
		{
			NRIVoxelRepresentationPolicy policy;
			NRIVoxelRepresentationFacts facts = BuildProxyFacts();
			for (uint32_t frameIndex = 10u; frameIndex <= 12u; ++frameIndex)
			{
				policy.BeginFrame(BuildProxyFrame(frameIndex));
				policy.EvaluateExact(facts);
			}
			switch (mutation)
			{
			case 0: facts.meshResourceKey++; break;
			case 1: facts.materialKeyHash++; break;
			case 2: facts.workloadMask ^= NRI_TLAS_MASK_GI; break;
			case 3: facts.resolvedVoxelIndex++; break;
			default: facts.shadowProxyCompatibilityKey++; break;
			}
			policy.BeginFrame(BuildProxyFrame(13u));
			const NRIVoxelRepresentationDecision decision = policy.EvaluateExact(facts);
			return decision.representation == NRIVoxelRepresentationKind::Exact &&
				decision.consecutiveProxyReadyFrames == 1u && decision.proxyWorkloadMask == 0u;
		};
		for (int mutation = 0; mutation < 5; ++mutation)
		{
			if (!mutationResets(mutation)) return false;
		}
		return true;
	}

	bool Run(const char* name, bool (*test)())
	{
		if (test()) return true;
		std::cerr << "FAILED: " << name << '\n';
		return false;
	}
}

int main()
{
	bool passed = true;
	passed &= Run("exact workload preservation", ExactDecisionPreservesWorkload);
	passed &= Run("ray family attribution", RayFamiliesAreAttributedSeparately);
	passed &= Run("projected bounds", ProjectedBoundsAreRecorded);
	passed &= Run("observational hysteresis", HysteresisIsObservationalOnly);
	passed &= Run("actor motion identity", ActorMotionPreservesDecisionIdentity);
	passed &= Run("source identity reset", SourceIdentityChangeResetsReadiness);
	passed &= Run("map generation reset", MapGenerationChangeResetsReadiness);
	passed &= Run("stale actor pruning", StaleActorStateIsPruned);
	passed &= Run("projection failure exact fallback", ProjectionFailuresStayExact);
	passed &= Run("exact-only snapshot", SnapshotReportsExactOnlyState);
	passed &= Run("certified shadow proxy transition", CertifiedProxyTransitionsAfterHysteresis);
	passed &= Run("proxy invalidation exact fallback", ProxyInvalidationFallsBackImmediately);
	passed &= Run("proxy transition limit", ProxyTransitionLimitDefersWithoutDroppingExact);
	passed &= Run("proxy compatibility mutation reset", CompatibilityMutationsResetProxyReadiness);
	return passed ? 0 : 1;
}
