#include "nri_scene_upload_dirty_plan.h"

#include <algorithm>

namespace
{
	struct SpanRange
	{
		NRISceneBufferUploadDomain domain = NRISceneBufferUploadDomain::StaticOverlay;
		uint64_t byteOffset = 0;
		uint64_t byteSize = 0;
		uint64_t stamp = 0;
	};

	bool BuildRanges(
		const std::vector<NRISceneBufferUploadDomainSpan>& spans,
		NRISceneUploadBufferKind kind,
		uint64_t payloadSize,
		uint32_t stride,
		std::vector<SpanRange>& outRanges)
	{
		outRanges.clear();
		if (stride == 0 || payloadSize % stride != 0)
		{
			return false;
		}
		uint64_t expectedOffset = 0;
		for (const NRISceneBufferUploadDomainSpan& span : spans)
		{
			uint32_t elementOffset = 0;
			uint32_t elementCount = 0;
			NRISceneUploadGetSpanElementRange(span, kind, elementOffset, elementCount);
			if (elementCount == 0)
			{
				continue;
			}
			SpanRange range = {};
			range.domain = span.domain;
			range.byteOffset = (uint64_t)elementOffset * stride;
			range.byteSize = (uint64_t)elementCount * stride;
			range.stamp = NRISceneUploadGetSpanStamp(span, kind);
			if (range.stamp == 0 || range.byteOffset != expectedOffset ||
				range.byteOffset > payloadSize || range.byteSize > payloadSize - range.byteOffset)
			{
				return false;
			}
			expectedOffset += range.byteSize;
			outRanges.push_back(range);
		}
		return expectedOffset == payloadSize;
	}
}

NRISceneUploadDirtyPlan BuildNRISceneUploadDirtyPlan(
	const std::vector<NRISceneBufferUploadDomainSpan>& currentSpans,
	const std::vector<NRISceneBufferUploadDomainSpan>& previousSpans,
	NRISceneUploadBufferKind kind,
	uint64_t payloadSize,
	uint32_t stride,
	uint64_t currentExtraIdentity,
	uint64_t previousExtraIdentity,
	uint64_t maxGapBytes)
{
	NRISceneUploadDirtyPlan result = {};
	std::vector<SpanRange> current;
	std::vector<SpanRange> previous;
	if (!BuildRanges(currentSpans, kind, payloadSize, stride, current) ||
		!BuildRanges(previousSpans, kind, payloadSize, stride, previous))
	{
		result.forceFull = payloadSize != 0;
		return result;
	}

	result.typed = true;
	const bool sameLayout = current.size() == previous.size() &&
		std::equal(current.begin(), current.end(), previous.begin(),
			[](const SpanRange& a, const SpanRange& b)
			{
				return a.domain == b.domain && a.byteOffset == b.byteOffset && a.byteSize == b.byteSize;
			});
	if (!sameLayout)
	{
		result.forceFull = payloadSize != 0;
		return result;
	}

	std::vector<SceneUploadDirtyRange> raw;
	if (currentExtraIdentity != previousExtraIdentity)
	{
		if (payloadSize != 0)
		{
			raw.push_back({ 0, payloadSize });
			result.changedBytes = payloadSize;
		}
	}
	else
	{
		for (size_t i = 0; i < current.size(); ++i)
		{
			if (current[i].stamp != previous[i].stamp)
			{
				raw.push_back({ current[i].byteOffset, current[i].byteSize });
				result.changedBytes += current[i].byteSize;
			}
		}
	}
	result.rawRanges = (uint32_t)raw.size();
	for (const SceneUploadDirtyRange& range : raw)
	{
		if (result.ranges.empty())
		{
			result.ranges.push_back(range);
			continue;
		}
		SceneUploadDirtyRange& last = result.ranges.back();
		const uint64_t lastEnd = last.byteOffset + last.size;
		const uint64_t gap = range.byteOffset - lastEnd;
		if (gap <= maxGapBytes)
		{
			last.size = range.byteOffset + range.size - last.byteOffset;
			result.gapBytes += gap;
		}
		else
		{
			result.rejectedCoalesces++;
			result.ranges.push_back(range);
		}
	}
	for (const SceneUploadDirtyRange& range : result.ranges)
	{
		result.uploadBytes += range.size;
	}
	return result;
}
