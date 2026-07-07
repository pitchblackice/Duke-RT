#include "nri_renderer.h"
#include "nri_acceleration.h"
#include "nri_cvars.h"
#include "nri_frame_resources.h"
#include "nri_upload_hash.h"

#include <algorithm>
#include <cstdint>

namespace
{
	static constexpr size_t MaxDynamicOverlayBlasAssets = 64;

	static bool IsNonEmptyPrimitiveSpan(const NRIRenderer::SceneBufferUploadDomainSpan& span)
	{
		return span.primitiveCount != 0 || span.indexCount != 0 || span.vertexCount != 0;
	}

	static uint64_t BuildDynamicOverlayBlasKey(
		const NRIRenderer::SceneBufferUploadDomainSpan& span,
		const std::vector<nri_scene::SceneVertex>& vertices,
		const std::vector<uint32_t>& indices)
	{
		uint64_t key = 1469598103934665603ull;
		key = NRIHashCombine64(key, (uint64_t)span.domain);
		key = NRIHashCombine64(key, (uint64_t)span.vertexCount);
		key = NRIHashCombine64(key, (uint64_t)span.indexCount);
		key = NRIHashCombine64(key, (uint64_t)span.primitiveCount);
		key = NRIHashCombine64(key, (uint64_t)span.materialCount);
		key = NRIHashCombine64(key, span.stamp.vertexPayloadStamp);
		key = NRIHashCombine64(key, span.stamp.indexPayloadStamp);
		key = NRIHashCombine64(key, span.stamp.primitivePayloadStamp);
		key = NRIHashCombine64(key, span.stamp.materialPayloadStamp);
		key = NRIHashCombine64(key, NRIHashUploadPayloadBytes(vertices.data(), (uint64_t)vertices.size() * sizeof(nri_scene::SceneVertex)));
		key = NRIHashCombine64(key, NRIHashUploadPayloadBytes(indices.data(), (uint64_t)indices.size() * sizeof(uint32_t)));
		return key != 0 ? key : 1;
	}
}

void NRIRenderer::ResetDynamicOverlayBlasCache()
{
	for (DynamicOverlayBlasAsset& asset : mDynamicOverlayBlasAssets)
	{
		DestroyAccelerationStructureResource(asset.accelerationStructure);
		DestroyBufferResource(asset.vertexBuffer);
		DestroyBufferResource(asset.indexBuffer);
	}
	mDynamicOverlayBlasAssets.clear();
	mDynamicOverlayBlasVertexScratch.clear();
	mDynamicOverlayBlasIndexScratch.clear();
}

bool NRIRenderer::BuildDynamicOverlayBlasRoute(
	const nri_scene::GeometryData& geometry,
	const std::vector<SceneBufferUploadDomainSpan>& uploadSpans,
	DynamicOverlayBlasRoute& outRoute)
{
	outRoute = {};

	const bool buildEnabled = (bool)nri_ptdynamicoverlayblasbuild;
	const bool routeEnabled = (bool)nri_ptdynamicoverlayblasroute;
	const uint32_t buildBudget = (uint32_t)std::max(0, (int)nri_ptdynamicoverlayblasbuilds);
	if (!buildEnabled && !routeEnabled)
	{
		return true;
	}

	const SceneBufferUploadDomainSpan* dynamicSpan = nullptr;
	uint32_t nonEmptySpanCount = 0;
	for (const SceneBufferUploadDomainSpan& span : uploadSpans)
	{
		if (!IsNonEmptyPrimitiveSpan(span))
		{
			continue;
		}

		nonEmptySpanCount++;
		if (span.domain != SceneBufferUploadDomain::Dynamic)
		{
			return true;
		}
		if (dynamicSpan != nullptr)
		{
			return true;
		}
		dynamicSpan = &span;
	}

	if (dynamicSpan == nullptr || nonEmptySpanCount != 1)
	{
		return true;
	}

	const SceneBufferUploadDomainSpan& span = *dynamicSpan;
	if (span.vertexCount == 0 || span.indexCount == 0 || span.primitiveCount == 0)
	{
		return true;
	}
	if (span.vertexOffset > geometry.vertices.size() ||
		span.indexOffset > geometry.indices.size() ||
		span.vertexOffset + span.vertexCount > geometry.vertices.size() ||
		span.indexOffset + span.indexCount > geometry.indices.size())
	{
		return true;
	}

	mDynamicOverlayBlasVertexScratch.assign(
		geometry.vertices.begin() + span.vertexOffset,
		geometry.vertices.begin() + span.vertexOffset + span.vertexCount);
	mDynamicOverlayBlasIndexScratch.clear();
	mDynamicOverlayBlasIndexScratch.reserve(span.indexCount);
	const uint32_t vertexEnd = span.vertexOffset + span.vertexCount;
	for (uint32_t i = 0; i < span.indexCount; ++i)
	{
		const uint32_t index = geometry.indices[span.indexOffset + i];
		if (index < span.vertexOffset || index >= vertexEnd)
		{
			return true;
		}
		mDynamicOverlayBlasIndexScratch.push_back(index - span.vertexOffset);
	}

	const uint64_t key = BuildDynamicOverlayBlasKey(span, mDynamicOverlayBlasVertexScratch, mDynamicOverlayBlasIndexScratch);
	auto found = std::find_if(mDynamicOverlayBlasAssets.begin(), mDynamicOverlayBlasAssets.end(),
		[key](const DynamicOverlayBlasAsset& asset)
		{
			return asset.key == key;
		});

	DynamicOverlayBlasAsset* asset = found != mDynamicOverlayBlasAssets.end() ? &*found : nullptr;
	if (asset != nullptr &&
		asset->accelerationStructure.accelerationStructure != nullptr &&
		asset->vertexBuffer.buffer != nullptr &&
		asset->indexBuffer.buffer != nullptr)
	{
		mLastPerfShellTraceStats.dynamicOverlayBlasCacheHits++;
		asset->lastUsedFrame = mFrameIndex;
	}
	else
	{
		mLastPerfShellTraceStats.dynamicOverlayBlasCacheMisses++;
		if (!buildEnabled || buildBudget == 0)
		{
			return true;
		}

		mLastPerfShellTraceStats.dynamicOverlayBlasBuildAttempts++;
		if (asset == nullptr)
		{
			if (mDynamicOverlayBlasAssets.size() >= MaxDynamicOverlayBlasAssets)
			{
				auto evictIt = std::min_element(mDynamicOverlayBlasAssets.begin(), mDynamicOverlayBlasAssets.end(),
					[](const DynamicOverlayBlasAsset& a, const DynamicOverlayBlasAsset& b)
					{
						return a.lastUsedFrame < b.lastUsedFrame;
					});
				if (evictIt != mDynamicOverlayBlasAssets.end())
				{
					DestroyAccelerationStructureResource(evictIt->accelerationStructure);
					DestroyBufferResource(evictIt->vertexBuffer);
					DestroyBufferResource(evictIt->indexBuffer);
					mDynamicOverlayBlasAssets.erase(evictIt);
				}
			}

			mDynamicOverlayBlasAssets.emplace_back();
			asset = &mDynamicOverlayBlasAssets.back();
			asset->key = key;
		}

		asset->vertexCount = span.vertexCount;
		asset->indexCount = span.indexCount;
		asset->primitiveCount = span.primitiveCount;
		asset->lastUsedFrame = mFrameIndex;

		const uint64_t vertexBytes = (uint64_t)mDynamicOverlayBlasVertexScratch.size() * sizeof(nri_scene::SceneVertex);
		const uint64_t indexBytes = (uint64_t)mDynamicOverlayBlasIndexScratch.size() * sizeof(uint32_t);
		const bool uploaded =
			EnsureResidentStructuredBuffer(
				asset->vertexBuffer,
				asset->vertexStats,
				mDynamicOverlayBlasVertexScratch.data(),
				vertexBytes,
				sizeof(nri_scene::SceneVertex),
				NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT),
				NRIResourceAccelerationStructureBuildInputAccess(),
				"dynamic-overlay-blas-vertex",
				ResidentUploadKind_Vertex) &&
			EnsureResidentStructuredBuffer(
				asset->indexBuffer,
				asset->indexStats,
				mDynamicOverlayBlasIndexScratch.data(),
				indexBytes,
				sizeof(uint32_t),
				NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT),
				NRIResourceAccelerationStructureBuildInputAccess(),
				"dynamic-overlay-blas-index",
				ResidentUploadKind_Index);
		if (!uploaded)
		{
			return false;
		}

		if (!BuildBottomLevelAccelerationStructure(
			asset->vertexBuffer,
			asset->indexBuffer,
			0u,
			span.vertexCount,
			0u,
			span.indexCount,
			span.primitiveCount,
			asset->accelerationStructure,
			false))
		{
			return false;
		}
		mLastPerfShellTraceStats.dynamicOverlayBlasBuildSuccesses++;
	}

	if (routeEnabled &&
		asset != nullptr &&
		asset->accelerationStructure.accelerationStructure != nullptr)
	{
		outRoute.accelerationStructure = &asset->accelerationStructure;
		outRoute.span = span;
		outRoute.routeAllOverlay = true;
		mLastPerfShellTraceStats.dynamicOverlayBlasRoutedInstances = 1;
		mLastPerfShellTraceStats.dynamicOverlayBlasFallbackDomains = 0;
		mLastPerfShellTraceStats.dynamicOverlayBlasFallbackPrimitives = 0;
		mLastPerfShellTraceStats.dynamicOverlayBlasMonolithicRefs = 0;
	}

	return true;
}
