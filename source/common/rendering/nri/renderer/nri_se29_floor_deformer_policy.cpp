#include "nri_se29_floor_deformer_policy.h"

#include <algorithm>
#include <limits>

namespace
{
	constexpr uint32_t AllowedVertexFields =
		NRISE29FloorDeformerVertexMutable_Position |
		NRISE29FloorDeformerVertexMutable_PreviousPosition |
		NRISE29FloorDeformerVertexMutable_TextureCoordinates;
	constexpr uint32_t AllowedPrimitiveFields =
		NRISE29FloorDeformerPrimitiveMutable_TextureCoordinates |
		NRISE29FloorDeformerPrimitiveMutable_GeometricNormal;

	bool IsValidIdentity(const NRISE29FloorDeformerIdentity& identity)
	{
		return identity.stableKey != 0 && identity.chunkIndex != UINT32_MAX;
	}

	bool IsValidLayout(const NRISE29FloorDeformerLayoutFingerprint& layout)
	{
		return IsValidIdentity(layout.identity) &&
			layout.memberCount != 0 &&
			layout.vertexCount != 0 &&
			layout.indexCount != 0 &&
			layout.primitiveCount != 0 &&
			layout.primitiveCount <= UINT32_MAX / 3u &&
			layout.indexCount == layout.primitiveCount * 3u &&
			layout.membershipFingerprint != 0 &&
			layout.topologyFingerprint != 0 &&
			layout.vertexOrderFingerprint != 0 &&
			layout.indexOrderFingerprint != 0 &&
			layout.primitiveOrderFingerprint != 0 &&
			layout.primitiveProvenanceFingerprint != 0 &&
			layout.materialSlotLayoutFingerprint != 0;
	}

	bool IsValidStamp(const NRISE29FloorDeformerDependencyStamp& stamp)
	{
		return stamp.mapEpoch != 0 &&
			stamp.authorityGeneration != 0 &&
			stamp.dependencyGeneration != 0;
	}

	uint32_t CompareStamps(
		const NRISE29FloorDeformerDependencyStamp& captured,
		const NRISE29FloorDeformerDependencyStamp& current)
	{
		uint32_t rejectMask = NRISE29FloorDeformerReject_None;
		if (!IsValidStamp(captured) || !IsValidStamp(current))
			rejectMask |= NRISE29FloorDeformerReject_InvalidStamp;
		if (captured.mapEpoch != current.mapEpoch)
			rejectMask |= NRISE29FloorDeformerReject_StaleEpoch;
		if (captured.dependencyGeneration != current.dependencyGeneration)
			rejectMask |= NRISE29FloorDeformerReject_StaleDependency;
		if (captured.authorityGeneration != current.authorityGeneration ||
			captured.topologyGeneration != current.topologyGeneration ||
			captured.membershipGeneration != current.membershipGeneration ||
			captured.geometryGeneration != current.geometryGeneration ||
			captured.materialSlotGeneration != current.materialSlotGeneration)
		{
			rejectMask |= NRISE29FloorDeformerReject_StaleGeneration;
		}
		return rejectMask;
	}

	bool TryAddProduct(uint64_t count, uint64_t stride, uint64_t& total)
	{
		if (stride != 0 && count > std::numeric_limits<uint64_t>::max() / stride)
			return false;
		const uint64_t bytes = count * stride;
		if (bytes > std::numeric_limits<uint64_t>::max() - total)
			return false;
		total += bytes;
		return true;
	}

	NRISE29FloorDeformerDecision EvaluateWork(
		const NRISE29FloorDeformerPendingWork& work)
	{
		NRISE29FloorDeformerDecision decision;
		decision.identity = work.residentLayout.identity;
		const NRISE29FloorDeformerLayoutComparison layout =
			CompareNRISE29FloorDeformerLayouts(work.residentLayout, work.currentLayout);
		decision.rejectMask |= layout.rejectMask;

		if (work.effectorLotag != NRI_SE29_FLOOR_DEFORMER_LOTAG || !work.floorPlaneOnly)
			decision.rejectMask |= NRISE29FloorDeformerReject_UnsupportedLane;
		if ((work.vertexMutableFieldMask & ~AllowedVertexFields) != 0 ||
			(work.primitiveMutableFieldMask & ~AllowedPrimitiveFields) != 0)
		{
			decision.rejectMask |= NRISE29FloorDeformerReject_UnsupportedMutableField;
		}
		if (work.usesSmoothNormals ||
			(work.primitiveMutableFieldMask & NRISE29FloorDeformerPrimitiveMutable_SmoothNormals) != 0)
		{
			decision.rejectMask |= NRISE29FloorDeformerReject_SmoothNormals;
		}
		decision.rejectMask |= CompareStamps(work.capturedStamp, work.currentStamp);

		decision.vertexPlan = CoalesceNRISE29FloorDeformerDirtySpans(
			work.vertexDirtySpans, work.currentLayout.vertexCount);
		decision.primitivePlan = CoalesceNRISE29FloorDeformerDirtySpans(
			work.primitiveDirtySpans, work.currentLayout.primitiveCount);
		const bool vertexSpanContract =
			(work.vertexMutableFieldMask == NRISE29FloorDeformerVertexMutable_None) ==
			(decision.vertexPlan.dirtyElementCount == 0);
		const bool primitiveSpanContract =
			(work.primitiveMutableFieldMask == NRISE29FloorDeformerPrimitiveMutable_None) ==
			(decision.primitivePlan.dirtyElementCount == 0);
		if (!decision.vertexPlan.valid || !decision.primitivePlan.valid ||
			!vertexSpanContract || !primitiveSpanContract ||
			(decision.vertexPlan.dirtyElementCount == 0 &&
				decision.primitivePlan.dirtyElementCount == 0) ||
			(decision.vertexPlan.dirtyElementCount != 0 && work.vertexStrideBytes == 0) ||
			(decision.primitivePlan.dirtyElementCount != 0 && work.primitiveStrideBytes == 0))
		{
			decision.rejectMask |= NRISE29FloorDeformerReject_InvalidDirtySpans;
		}

		decision.primitiveCost = work.currentLayout.primitiveCount;
		if (!TryAddProduct(
			decision.vertexPlan.dirtyElementCount,
			work.vertexStrideBytes,
			decision.uploadBytes) ||
			!TryAddProduct(
				decision.primitivePlan.dirtyElementCount,
				work.primitiveStrideBytes,
				decision.uploadBytes))
		{
			decision.rejectMask |= NRISE29FloorDeformerReject_UploadBudget;
		}
		return decision;
	}

	bool HasUrgentPriority(const NRISE29FloorDeformerPendingWork& work)
	{
		return work.rayVisible || work.required;
	}

	bool WorkPriorityLess(
		const NRISE29FloorDeformerPendingWork* a,
		const NRISE29FloorDeformerPendingWork* b)
	{
		const bool aUrgent = HasUrgentPriority(*a);
		const bool bUrgent = HasUrgentPriority(*b);
		if (aUrgent != bUrgent) return aUrgent;
		if (a->pendingSinceFrame != b->pendingSinceFrame)
			return a->pendingSinceFrame < b->pendingSinceFrame;
		if (a->residentLayout.identity.stableKey != b->residentLayout.identity.stableKey)
			return a->residentLayout.identity.stableKey < b->residentLayout.identity.stableKey;
		return a->residentLayout.identity.chunkIndex < b->residentLayout.identity.chunkIndex;
	}
}

bool operator==(
	const NRISE29FloorDeformerIdentity& a,
	const NRISE29FloorDeformerIdentity& b)
{
	return a.stableKey == b.stableKey && a.chunkIndex == b.chunkIndex;
}

NRISE29FloorDeformerLayoutComparison CompareNRISE29FloorDeformerLayouts(
	const NRISE29FloorDeformerLayoutFingerprint& resident,
	const NRISE29FloorDeformerLayoutFingerprint& current)
{
	NRISE29FloorDeformerLayoutComparison result;
	if (!IsValidIdentity(resident.identity) || !IsValidIdentity(current.identity))
		result.rejectMask |= NRISE29FloorDeformerReject_InvalidIdentity;
	if (!IsValidLayout(resident) || !IsValidLayout(current))
		result.rejectMask |= NRISE29FloorDeformerReject_InvalidLayout;
	if (!(resident.identity == current.identity))
		result.rejectMask |= NRISE29FloorDeformerReject_IdentityChanged;
	if (resident.memberCount != current.memberCount ||
		resident.membershipFingerprint != current.membershipFingerprint)
	{
		result.rejectMask |= NRISE29FloorDeformerReject_MembershipChanged;
	}
	if (resident.vertexCount != current.vertexCount ||
		resident.indexCount != current.indexCount ||
		resident.primitiveCount != current.primitiveCount)
	{
		result.rejectMask |= NRISE29FloorDeformerReject_CountChanged;
	}
	if (resident.topologyFingerprint != current.topologyFingerprint)
		result.rejectMask |= NRISE29FloorDeformerReject_TopologyChanged;
	if (resident.vertexOrderFingerprint != current.vertexOrderFingerprint ||
		resident.indexOrderFingerprint != current.indexOrderFingerprint ||
		resident.primitiveOrderFingerprint != current.primitiveOrderFingerprint)
	{
		result.rejectMask |= NRISE29FloorDeformerReject_OrderChanged;
	}
	if (resident.primitiveProvenanceFingerprint != current.primitiveProvenanceFingerprint)
		result.rejectMask |= NRISE29FloorDeformerReject_ProvenanceChanged;
	if (resident.materialSlotLayoutFingerprint != current.materialSlotLayoutFingerprint)
		result.rejectMask |= NRISE29FloorDeformerReject_MaterialSlotsChanged;
	result.compatible = result.rejectMask == NRISE29FloorDeformerReject_None;
	return result;
}

NRISE29FloorDeformerDirtySpanPlan CoalesceNRISE29FloorDeformerDirtySpans(
	const std::vector<NRISE29FloorDeformerDirtySpan>& spans,
	uint32_t elementCapacity)
{
	NRISE29FloorDeformerDirtySpanPlan result;
	result.spans.reserve(spans.size());
	for (const NRISE29FloorDeformerDirtySpan& span : spans)
	{
		if (span.elementCount == 0) continue;
		if (span.firstElement >= elementCapacity ||
			span.elementCount > elementCapacity - span.firstElement)
		{
			return result;
		}
		result.spans.push_back(span);
	}

	std::sort(result.spans.begin(), result.spans.end(),
		[](const auto& a, const auto& b)
		{
			if (a.firstElement != b.firstElement) return a.firstElement < b.firstElement;
			return a.elementCount < b.elementCount;
		});

	std::vector<NRISE29FloorDeformerDirtySpan> coalesced;
	coalesced.reserve(result.spans.size());
	for (const NRISE29FloorDeformerDirtySpan& span : result.spans)
	{
		if (coalesced.empty())
		{
			coalesced.push_back(span);
			continue;
		}
		NRISE29FloorDeformerDirtySpan& previous = coalesced.back();
		const uint32_t previousEnd = previous.firstElement + previous.elementCount;
		const uint32_t currentEnd = span.firstElement + span.elementCount;
		if (span.firstElement <= previousEnd)
		{
			previous.elementCount = std::max(previousEnd, currentEnd) - previous.firstElement;
		}
		else
		{
			coalesced.push_back(span);
		}
	}

	result.spans = std::move(coalesced);
	for (const NRISE29FloorDeformerDirtySpan& span : result.spans)
		result.dirtyElementCount += span.elementCount;
	result.valid = true;
	return result;
}

NRISE29FloorDeformerPendingUpdate NRISE29FloorDeformerPendingSet::QueueLatest(
	const NRISE29FloorDeformerPendingWork& work)
{
	const NRISE29FloorDeformerIdentity identity = work.residentLayout.identity;
	if (!IsValidIdentity(identity) || !IsValidStamp(work.capturedStamp))
		return NRISE29FloorDeformerPendingUpdate::Invalid;

	auto found = std::find_if(m_items.begin(), m_items.end(),
		[&identity](const auto& item) { return item.residentLayout.identity == identity; });
	if (found == m_items.end())
	{
		m_items.push_back(work);
		return NRISE29FloorDeformerPendingUpdate::Added;
	}

	if (work.capturedStamp.mapEpoch < found->capturedStamp.mapEpoch ||
		(work.capturedStamp.mapEpoch == found->capturedStamp.mapEpoch &&
			work.capturedStamp.authorityGeneration < found->capturedStamp.authorityGeneration))
	{
		return NRISE29FloorDeformerPendingUpdate::StaleIgnored;
	}
	if (work.capturedStamp.mapEpoch == found->capturedStamp.mapEpoch &&
		work.capturedStamp.authorityGeneration == found->capturedStamp.authorityGeneration)
	{
		return NRISE29FloorDeformerPendingUpdate::DuplicateIgnored;
	}

	NRISE29FloorDeformerPendingWork replacement = work;
	if (work.capturedStamp.mapEpoch == found->capturedStamp.mapEpoch)
		replacement.pendingSinceFrame = std::min(found->pendingSinceFrame, work.pendingSinceFrame);
	*found = std::move(replacement);
	return NRISE29FloorDeformerPendingUpdate::ReplacedLatest;
}

bool NRISE29FloorDeformerPendingSet::Remove(
	const NRISE29FloorDeformerIdentity& identity)
{
	const auto found = std::find_if(m_items.begin(), m_items.end(),
		[&identity](const auto& item) { return item.residentLayout.identity == identity; });
	if (found == m_items.end()) return false;
	m_items.erase(found);
	return true;
}

void NRISE29FloorDeformerPendingSet::Clear()
{
	m_items.clear();
}

NRISE29FloorDeformerBatchPlan SelectNRISE29FloorDeformerWork(
	const std::vector<NRISE29FloorDeformerPendingWork>& pending,
	const NRISE29FloorDeformerBudget& budget)
{
	NRISE29FloorDeformerBatchPlan result;
	std::vector<const NRISE29FloorDeformerPendingWork*> ordered;
	ordered.reserve(pending.size());
	for (const NRISE29FloorDeformerPendingWork& work : pending) ordered.push_back(&work);
	std::sort(ordered.begin(), ordered.end(), WorkPriorityLess);
	result.decisions.reserve(ordered.size());

	for (const NRISE29FloorDeformerPendingWork* work : ordered)
	{
		NRISE29FloorDeformerDecision decision = EvaluateWork(*work);
		if (decision.rejectMask == NRISE29FloorDeformerReject_None)
		{
			if (result.admittedChunks >= budget.maxChunks)
				decision.rejectMask |= NRISE29FloorDeformerReject_ChunkBudget;
			if (decision.primitiveCost > budget.maxPrimitives -
				std::min(result.admittedPrimitives, budget.maxPrimitives))
			{
				decision.rejectMask |= NRISE29FloorDeformerReject_PrimitiveBudget;
			}
			if (decision.uploadBytes > budget.maxUploadBytes -
				std::min(result.admittedUploadBytes, budget.maxUploadBytes))
			{
				decision.rejectMask |= NRISE29FloorDeformerReject_UploadBudget;
			}
		}

		if (decision.rejectMask == NRISE29FloorDeformerReject_None)
		{
			decision.action = NRISE29FloorDeformerAction::Refit;
			result.admittedChunks++;
			result.admittedPrimitives += decision.primitiveCost;
			result.admittedUploadBytes += decision.uploadBytes;
		}
		else
		{
			result.exactFallbacks++;
		}
		result.decisions.push_back(std::move(decision));
	}
	return result;
}
