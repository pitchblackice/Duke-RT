#include "nri_map_chunk_diagnostics.h"

#include "../scene/nri_map_world.h"

#include <algorithm>
#include <cmath>

namespace nri_map_chunk_diag
{
	bool SurfaceKey::operator==(const SurfaceKey& other) const
	{
		return kind == other.kind &&
			sourceType == other.sourceType &&
			sectorIndex == other.sectorIndex &&
			wallIndex == other.wallIndex &&
			sectionIndex == other.sectionIndex &&
			nextSectorIndex == other.nextSectorIndex &&
			actorIndex == other.actorIndex &&
			cstat == other.cstat &&
			materialFlags == other.materialFlags &&
			primaryKey == other.primaryKey &&
			secondaryKey == other.secondaryKey;
	}

	size_t SurfaceKeyHash::operator()(const SurfaceKey& key) const
	{
		size_t h = 1469598103934665603ull;
		const auto mix = [&h](uint64_t value)
		{
			h ^= (size_t)value;
			h *= 1099511628211ull;
		};
		mix(key.kind);
		mix(key.sourceType);
		mix((uint32_t)key.sectorIndex);
		mix((uint32_t)key.wallIndex);
		mix((uint32_t)key.sectionIndex);
		mix((uint32_t)key.nextSectorIndex);
		mix((uint32_t)key.actorIndex);
		mix(key.cstat);
		mix(key.materialFlags);
		mix(key.primaryKey);
		mix(key.secondaryKey);
		return h;
	}

	uint32_t CountSurfaceTriangles(const nri_scene::SurfaceRef& surface)
	{
		if (!surface.indices.empty())
		{
			return (uint32_t)(surface.indices.size() / 3u);
		}
		return surface.vertices.size() >= 3 ? (uint32_t)surface.vertices.size() - 2u : 0u;
	}

	SurfaceKey BuildSurfaceKey(const nri_scene::PTMapSurface& surface)
	{
		SurfaceKey key = {};
		key.kind = (uint32_t)surface.kind;
		key.sourceType = (uint32_t)surface.surface.provenance.sourceType;
		key.sectorIndex = surface.surface.provenance.sectorIndex;
		key.wallIndex = surface.surface.provenance.wallIndex;
		key.sectionIndex = surface.surface.provenance.sectionIndex;
		key.nextSectorIndex = surface.surface.provenance.nextSectorIndex;
		key.actorIndex = surface.surface.provenance.actorIndex;
		key.cstat = surface.surface.provenance.cstat;
		key.materialFlags = surface.surface.provenance.materialFlags;
		key.primaryKey = surface.key.primary;
		key.secondaryKey = surface.key.secondary;
		return key;
	}

	uint32_t GetSurfaceTextureId(const nri_scene::PTMapSurface& surface)
	{
		return
			surface.surface.material.texture != nullptr ?
			(uint32_t)surface.surface.material.texture->GetID().GetIndex() :
			0u;
	}

	float Distance3(const float a[3], const float b[3])
	{
		const float dx = a[0] - b[0];
		const float dy = a[1] - b[1];
		const float dz = a[2] - b[2];
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	float Dot3(const float a[3], const float b[3])
	{
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	}

	static float ComputeTriangleArea(const nri_scene::CapturedVertex& a, const nri_scene::CapturedVertex& b, const nri_scene::CapturedVertex& c)
	{
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float crossX = aby * acz - abz * acy;
		const float crossY = abz * acx - abx * acz;
		const float crossZ = abx * acy - aby * acx;
		return 0.5f * std::sqrt(crossX * crossX + crossY * crossY + crossZ * crossZ);
	}

	static void ComputeTriangleNormal(const nri_scene::CapturedVertex& a, const nri_scene::CapturedVertex& b, const nri_scene::CapturedVertex& c, float outNormal[3])
	{
		outNormal[0] = 0.0f;
		outNormal[1] = 0.0f;
		outNormal[2] = 0.0f;
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float crossX = aby * acz - abz * acy;
		const float crossY = abz * acx - abx * acz;
		const float crossZ = abx * acy - aby * acx;
		const float length = std::sqrt(crossX * crossX + crossY * crossY + crossZ * crossZ);
		if (length <= 0.0001f)
		{
			return;
		}

		outNormal[0] = crossX / length;
		outNormal[1] = crossY / length;
		outNormal[2] = crossZ / length;
	}

	SurfaceMetrics ComputeSurfaceMetrics(const nri_scene::PTMapSurface& surface)
	{
		SurfaceMetrics metrics = {};
		const auto& vertices = surface.surface.vertices;
		metrics.vertexCount = (uint32_t)vertices.size();
		metrics.triangleCount = CountSurfaceTriangles(surface.surface);
		metrics.textureId = GetSurfaceTextureId(surface);
		metrics.palette = surface.surface.material.palette;
		metrics.shade = surface.surface.material.shade;
		metrics.alpha = surface.surface.material.alpha;
		metrics.materialFlags = surface.surface.material.flags;
		if (vertices.empty())
		{
			return metrics;
		}

		for (int axis = 0; axis < 3; ++axis)
		{
			metrics.aabbMin[axis] = vertices[0].position[axis];
			metrics.aabbMax[axis] = vertices[0].position[axis];
		}

		for (const auto& vertex : vertices)
		{
			for (int axis = 0; axis < 3; ++axis)
			{
				metrics.centroid[axis] += vertex.position[axis];
				metrics.aabbMin[axis] = std::min(metrics.aabbMin[axis], vertex.position[axis]);
				metrics.aabbMax[axis] = std::max(metrics.aabbMax[axis], vertex.position[axis]);
			}
		}

		const float invCount = 1.0f / (float)vertices.size();
		for (int axis = 0; axis < 3; ++axis)
		{
			metrics.centroid[axis] *= invCount;
		}

		if (vertices.size() >= 3)
		{
			if ((surface.surface.material.flags & nri_scene::MaterialFlag_Flat) != 0 &&
				(vertices.size() % 3u) == 0u)
			{
				for (size_t i = 0; i + 2 < vertices.size(); i += 3)
				{
					metrics.area += ComputeTriangleArea(vertices[i], vertices[i + 1], vertices[i + 2]);
					if (metrics.normal[0] == 0.0f && metrics.normal[1] == 0.0f && metrics.normal[2] == 0.0f)
					{
						ComputeTriangleNormal(vertices[i], vertices[i + 1], vertices[i + 2], metrics.normal);
					}
				}
			}
			else
			{
				const auto& root = vertices[0];
				for (size_t i = 1; i + 1 < vertices.size(); ++i)
				{
					metrics.area += ComputeTriangleArea(root, vertices[i], vertices[i + 1]);
					if (metrics.normal[0] == 0.0f && metrics.normal[1] == 0.0f && metrics.normal[2] == 0.0f)
					{
						ComputeTriangleNormal(root, vertices[i], vertices[i + 1], metrics.normal);
					}
				}
			}
		}

		return metrics;
	}
}
