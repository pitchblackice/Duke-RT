#pragma once

#include <cstddef>
#include <cstdint>

namespace nri_scene
{
	struct PTMapSurface;
	struct SurfaceRef;
}

namespace nri_map_chunk_diag
{
	struct SurfaceKey
	{
		uint32_t kind = UINT32_MAX;
		uint32_t sourceType = 0u;
		int32_t sectorIndex = -1;
		int32_t wallIndex = -1;
		int32_t sectionIndex = -1;
		int32_t nextSectorIndex = -1;
		int32_t actorIndex = -1;
		uint32_t cstat = 0;
		uint32_t materialFlags = 0;
		uint32_t primaryKey = UINT32_MAX;
		uint32_t secondaryKey = UINT32_MAX;

		bool operator==(const SurfaceKey& other) const;
	};

	struct SurfaceKeyHash
	{
		size_t operator()(const SurfaceKey& key) const;
	};

	struct SurfaceMetrics
	{
		float centroid[3] = {};
		float normal[3] = {};
		float area = 0.0f;
		float aabbMin[3] = {};
		float aabbMax[3] = {};
		uint32_t vertexCount = 0;
		uint32_t triangleCount = 0;
		uint32_t textureId = 0;
		int palette = 0;
		int shade = 0;
		float alpha = 1.0f;
		uint32_t materialFlags = 0;
	};

	struct MatchRecord
	{
		uint32_t staticSurfaceIndex = UINT32_MAX;
		uint32_t liveSurfaceIndex = UINT32_MAX;
		SurfaceKey key = {};
		SurfaceMetrics staticMetrics = {};
		SurfaceMetrics liveMetrics = {};
		float delta[3] = {};
		float deltaDistance = 0.0f;
		float areaRatio = 1.0f;
		float normalDot = 1.0f;
		float materialScore = 0.0f;
		float deviationFromMean = 0.0f;
		float score = 0.0f;
	};

	uint32_t CountSurfaceTriangles(const nri_scene::SurfaceRef& surface);
	uint32_t GetSurfaceTextureId(const nri_scene::PTMapSurface& surface);
	SurfaceKey BuildSurfaceKey(const nri_scene::PTMapSurface& surface);
	SurfaceMetrics ComputeSurfaceMetrics(const nri_scene::PTMapSurface& surface);
	float Distance3(const float a[3], const float b[3]);
	float Dot3(const float a[3], const float b[3]);
}
