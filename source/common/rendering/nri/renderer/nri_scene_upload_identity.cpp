#include "nri_scene_upload_identity.h"

#include <algorithm>

namespace
{
	constexpr uint64_t IdentitySeed = 1469598103934665603ull;

	uint64_t NonzeroIncrement(uint64_t value)
	{
		value++;
		return value != 0 ? value : 1;
	}

	uint64_t BuildChannelStamp(uint64_t contentGeneration, uint64_t layoutGeneration, uint64_t channel)
	{
		uint64_t hash = IdentitySeed;
		hash = NRISceneUploadCombineIdentity(hash, contentGeneration);
		hash = NRISceneUploadCombineIdentity(hash, layoutGeneration);
		hash = NRISceneUploadCombineIdentity(hash, channel);
		return hash != 0 ? hash : 1;
	}

	size_t RecordIndex(NRISceneBufferUploadDomain domain, NRISceneUploadBufferKind kind)
	{
		return (size_t)domain * (size_t)NRISceneUploadBufferKind::Count + (size_t)kind;
	}

	uint64_t BuildClaimedIdentity(
		uint64_t stamp,
		uint64_t extraIdentity,
		uint64_t byteOffset,
		uint64_t byteSize)
	{
		uint64_t claimedIdentity = IdentitySeed;
		claimedIdentity = NRISceneUploadCombineIdentity(claimedIdentity, stamp);
		claimedIdentity = NRISceneUploadCombineIdentity(claimedIdentity, extraIdentity);
		claimedIdentity = NRISceneUploadCombineIdentity(claimedIdentity, byteOffset);
		claimedIdentity = NRISceneUploadCombineIdentity(claimedIdentity, byteSize);
		return claimedIdentity;
	}
}

uint64_t NRISceneUploadHashBytes(const void* data, uint64_t size)
{
	const uint8_t* bytes = static_cast<const uint8_t*>(data);
	uint64_t hash = IdentitySeed;
	for (uint64_t i = 0; i < size; ++i)
	{
		hash ^= bytes != nullptr ? bytes[i] : 0u;
		hash *= 1099511628211ull;
	}
	hash = NRISceneUploadCombineIdentity(hash, size);
	return hash != 0 ? hash : 1;
}

uint64_t NRISceneUploadCombineIdentity(uint64_t hash, uint64_t value)
{
	return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
}

uint64_t NRISceneUploadGetSpanStamp(const NRISceneBufferUploadDomainSpan& span, NRISceneUploadBufferKind kind)
{
	switch (kind)
	{
	case NRISceneUploadBufferKind::Vertex: return span.stamp.vertexPayloadStamp;
	case NRISceneUploadBufferKind::Index: return span.stamp.indexPayloadStamp;
	case NRISceneUploadBufferKind::Primitive: return span.stamp.primitivePayloadStamp;
	case NRISceneUploadBufferKind::Provenance: return span.stamp.primitiveProvenanceStamp;
	case NRISceneUploadBufferKind::Material: return span.stamp.materialPayloadStamp;
	case NRISceneUploadBufferKind::Count: break;
	}
	return 0;
}

void NRISceneUploadGetSpanElementRange(
	const NRISceneBufferUploadDomainSpan& span,
	NRISceneUploadBufferKind kind,
	uint32_t& outOffset,
	uint32_t& outCount)
{
	switch (kind)
	{
	case NRISceneUploadBufferKind::Vertex:
		outOffset = span.vertexOffset;
		outCount = span.vertexCount;
		return;
	case NRISceneUploadBufferKind::Index:
		outOffset = span.indexOffset;
		outCount = span.indexCount;
		return;
	case NRISceneUploadBufferKind::Primitive:
	case NRISceneUploadBufferKind::Provenance:
		outOffset = span.primitiveOffset;
		outCount = span.primitiveCount;
		return;
	case NRISceneUploadBufferKind::Material:
		outOffset = span.materialOffset;
		outCount = span.materialCount;
		return;
	case NRISceneUploadBufferKind::Count:
		break;
	}
	outOffset = 0;
	outCount = 0;
}

NRISceneBufferUploadProducerStamp NRISceneUploadProducerGenerations::Publish(
	NRISceneBufferUploadDomain domain,
	uint64_t contentKey,
	uint64_t layoutKey,
	bool conservativelyContentChanged,
	bool conservativelyLayoutChanged)
{
	NRISceneBufferUploadProducerStamp stamp = {};
	const size_t domainIndex = (size_t)domain;
	if (domainIndex >= mDomains.size())
	{
		return stamp;
	}

	DomainState& state = mDomains[domainIndex];
	const bool contentChanged = conservativelyContentChanged || !state.valid || contentKey == 0 || state.contentKey != contentKey;
	const bool layoutChanged = conservativelyLayoutChanged || !state.valid || layoutKey == 0 || state.layoutKey != layoutKey;
	if (contentChanged)
	{
		state.contentGeneration = NonzeroIncrement(state.contentGeneration);
	}
	if (layoutChanged)
	{
		state.layoutGeneration = NonzeroIncrement(state.layoutGeneration);
	}
	state.contentKey = contentKey;
	state.layoutKey = layoutKey;
	state.valid = true;

	stamp.vertexPayloadStamp = BuildChannelStamp(state.contentGeneration, state.layoutGeneration, 1u);
	stamp.indexPayloadStamp = BuildChannelStamp(state.layoutGeneration, state.layoutGeneration, 2u);
	stamp.primitivePayloadStamp = BuildChannelStamp(state.contentGeneration, state.layoutGeneration, 3u);
	stamp.primitiveProvenanceStamp = BuildChannelStamp(state.layoutGeneration, state.layoutGeneration, 4u);
	stamp.materialPayloadStamp = BuildChannelStamp(state.contentGeneration, state.layoutGeneration, 5u);
	return stamp;
}

void NRISceneUploadProducerGenerations::Reset()
{
	mDomains = {};
}

NRISceneUploadIdentityBuildResult BuildNRISceneUploadPayloadIdentity(
	const std::vector<NRISceneBufferUploadDomainSpan>* spans,
	NRISceneUploadBufferKind kind,
	const NRISceneUploadPayloadView& payload,
	const NRISceneUploadIdentityValidator* validator)
{
	NRISceneUploadIdentityBuildResult result = {};
	if (payload.stride == 0 || payload.byteSize % payload.stride != 0)
	{
		result.stats.coverageRejects++;
		return result;
	}

	uint64_t hash = IdentitySeed;
	hash = NRISceneUploadCombineIdentity(hash, (uint64_t)kind);
	hash = NRISceneUploadCombineIdentity(hash, payload.byteSize);
	hash = NRISceneUploadCombineIdentity(hash, payload.stride);
	hash = NRISceneUploadCombineIdentity(hash, payload.extraIdentity);
	if (spans == nullptr)
	{
		result.stats.fallbackBytes = payload.byteSize;
		result.stats.fallbackSpans = payload.byteSize != 0 ? 1u : 0u;
		return result;
	}

	uint64_t expectedOffset = 0;
	uint32_t ordinal = 0;
	for (const NRISceneBufferUploadDomainSpan& span : *spans)
	{
		uint32_t elementOffset = 0;
		uint32_t elementCount = 0;
		NRISceneUploadGetSpanElementRange(span, kind, elementOffset, elementCount);
		if (elementCount == 0)
		{
			continue;
		}
		const uint64_t byteOffset = (uint64_t)elementOffset * payload.stride;
		const uint64_t byteSize = (uint64_t)elementCount * payload.stride;
		if (byteOffset != expectedOffset || byteOffset > payload.byteSize || byteSize > payload.byteSize - byteOffset)
		{
			result.stats.coverageRejects++;
			result.stats.fallbackBytes = payload.byteSize;
			result.stats.fallbackSpans = 1;
			return result;
		}
		expectedOffset += byteSize;

		const size_t domainIndex = (size_t)span.domain;
		if (domainIndex < result.stats.domainChecks.size())
		{
			result.stats.domainChecks[domainIndex]++;
		}
		const uint64_t stamp = NRISceneUploadGetSpanStamp(span, kind);
		const bool quarantined =
			stamp != 0 && validator != nullptr &&
			validator->IsQuarantined(span.domain, kind, stamp, payload.extraIdentity, byteOffset, byteSize);
		if (quarantined)
		{
			result.stats.quarantinedSpans++;
		}
		const bool useProducerStamp = stamp != 0 && !quarantined;
		uint64_t spanIdentity = useProducerStamp ? stamp : 0;
		if (!useProducerStamp)
		{
			const uint8_t* bytes = static_cast<const uint8_t*>(payload.data);
			spanIdentity = NRISceneUploadHashBytes(bytes != nullptr ? bytes + byteOffset : nullptr, byteSize);
			result.stats.fallbackBytes += byteSize;
			result.stats.fallbackSpans++;
			if (domainIndex < result.stats.domainFallbacks.size())
			{
				result.stats.domainFallbacks[domainIndex]++;
			}
		}
		else
		{
			result.stats.stampedBytes += byteSize;
			result.stats.stampedSpans++;
		}

		hash = NRISceneUploadCombineIdentity(hash, (uint64_t)span.domain);
		hash = NRISceneUploadCombineIdentity(hash, ordinal++);
		hash = NRISceneUploadCombineIdentity(hash, byteOffset);
		hash = NRISceneUploadCombineIdentity(hash, byteSize);
		hash = NRISceneUploadCombineIdentity(hash, useProducerStamp ? 1u : 2u);
		hash = NRISceneUploadCombineIdentity(hash, spanIdentity);
	}

	if (expectedOffset != payload.byteSize)
	{
		result.stats.coverageRejects++;
		result.stats.fallbackBytes = payload.byteSize;
		result.stats.fallbackSpans = 1;
		return result;
	}

	result.identity = hash != 0 ? hash : 1;
	result.completeCoverage = true;
	result.usedOnlyProducerStamps = result.stats.fallbackBytes == 0;
	return result;
}

bool NRISceneUploadIdentityValidator::Validate(
	const std::vector<NRISceneBufferUploadDomainSpan>& spans,
	NRISceneUploadBufferKind kind,
	const NRISceneUploadPayloadView& payload,
	NRISceneUploadIdentityValidationStats& outStats)
{
	outStats = {};
	bool valid = true;
	for (const NRISceneBufferUploadDomainSpan& span : spans)
	{
		uint32_t elementOffset = 0;
		uint32_t elementCount = 0;
		NRISceneUploadGetSpanElementRange(span, kind, elementOffset, elementCount);
		if (elementCount == 0)
		{
			continue;
		}
		const uint64_t stamp = NRISceneUploadGetSpanStamp(span, kind);
		if (stamp == 0)
		{
			outStats.skippedUnstamped++;
			continue;
		}
		const uint64_t byteOffset = (uint64_t)elementOffset * payload.stride;
		const uint64_t byteSize = (uint64_t)elementCount * payload.stride;
		if (payload.stride == 0 || byteOffset > payload.byteSize || byteSize > payload.byteSize - byteOffset)
		{
			outStats.mismatches++;
			valid = false;
			continue;
		}

		const uint64_t claimedIdentity =
			BuildClaimedIdentity(stamp, payload.extraIdentity, byteOffset, byteSize);
		const uint8_t* bytes = static_cast<const uint8_t*>(payload.data);
		const uint64_t exactHash = NRISceneUploadHashBytes(bytes != nullptr ? bytes + byteOffset : nullptr, byteSize);
		Record& record = mRecords[RecordIndex(span.domain, kind)];
		outStats.checks++;
		if (record.valid && record.claimedIdentity == claimedIdentity && record.exactContentHash != exactHash)
		{
			outStats.mismatches++;
			valid = false;
			record.quarantined = true;
		}
		else if (!record.valid || record.claimedIdentity != claimedIdentity)
		{
			record.quarantined = false;
		}
		record.claimedIdentity = claimedIdentity;
		record.exactContentHash = exactHash;
		record.valid = true;
	}
	return valid;
}

bool NRISceneUploadIdentityValidator::IsQuarantined(
	NRISceneBufferUploadDomain domain,
	NRISceneUploadBufferKind kind,
	uint64_t stamp,
	uint64_t extraIdentity,
	uint64_t byteOffset,
	uint64_t byteSize) const
{
	const size_t domainIndex = (size_t)domain;
	const size_t kindIndex = (size_t)kind;
	if (domainIndex >= (size_t)NRISceneBufferUploadDomain::Count ||
		kindIndex >= (size_t)NRISceneUploadBufferKind::Count || stamp == 0)
	{
		return false;
	}
	const Record& record = mRecords[RecordIndex(domain, kind)];
	return record.valid && record.quarantined &&
		record.claimedIdentity == BuildClaimedIdentity(stamp, extraIdentity, byteOffset, byteSize);
}

void NRISceneUploadIdentityValidator::Reset()
{
	mRecords = {};
}
