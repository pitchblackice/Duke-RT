#include "nri_render_geometry_helpers.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
	static bool TryComputeCapturedSurfaceNormal(const nri_scene::SurfaceRef& surface, float outNormal[3])
	{
		if (surface.vertices.size() < 3)
		{
			return false;
		}

		const nri_scene::CapturedVertex& a = surface.vertices[0];
		const nri_scene::CapturedVertex& b = surface.vertices[1];
		const nri_scene::CapturedVertex& c = surface.vertices[2];
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float nx = aby * acz - abz * acy;
		const float ny = abz * acx - abx * acz;
		const float nz = abx * acy - aby * acx;
		const float lengthSq = nx * nx + ny * ny + nz * nz;
		if (lengthSq <= 1.0e-8f)
		{
			return false;
		}

		const float invLength = 1.0f / std::sqrt(lengthSq);
		outNormal[0] = nx * invLength;
		outNormal[1] = ny * invLength;
		outNormal[2] = nz * invLength;
		return true;
	}
}

uint32_t CountOrphanLocalSpaces(const nri_scene::PTMapWorld& mapWorld)
{
	if (!mapWorld.valid || mapWorld.localSpaces.empty())
	{
		return 0;
	}

	std::vector<uint8_t> linked(mapWorld.localSpaces.size(), 0u);
	for (const auto& portal : mapWorld.portals)
	{
		if (portal.sourceLocalSpaceIndex < linked.size())
		{
			linked[portal.sourceLocalSpaceIndex] = 1u;
		}

		for (uint32_t i = 0; i < portal.targetCount; ++i)
		{
			const uint32_t targetIndex = portal.firstTarget + i;
			if (targetIndex >= mapWorld.portalTargets.size())
			{
				break;
			}

			const uint32_t localSpaceIndex = mapWorld.portalTargets[targetIndex].localSpaceIndex;
			if (localSpaceIndex < linked.size())
			{
				linked[localSpaceIndex] = 1u;
			}
		}
	}

	uint32_t orphanCount = 0;
	for (uint8_t value : linked)
	{
		if (value == 0u)
		{
			orphanCount++;
		}
	}

	return orphanCount;
}

MapCeilingNudgeStats NudgeMapCeilingSections(nri_scene::SceneView& sceneView, float depthNudge)
{
	MapCeilingNudgeStats stats = {};
	if (depthNudge <= 0.0f)
	{
		return stats;
	}

	for (nri_scene::SurfaceRef& surface : sceneView.opaqueFlats)
	{
		if (surface.provenance.sourceType != nri_scene::SurfaceSourceType::MapCeilingSection)
		{
			continue;
		}

		const uint32_t flags = surface.material.flags;
		if ((flags & nri_scene::MaterialFlag_Flat) == 0 ||
			(flags & (nri_scene::MaterialFlag_Sprite | nri_scene::MaterialFlag_Mirror | nri_scene::MaterialFlag_Sky | nri_scene::MaterialFlag_Portal)) != 0)
		{
			continue;
		}

		float normal[3] = {};
		if (!TryComputeCapturedSurfaceNormal(surface, normal))
		{
			stats.skippedNormalCount++;
			continue;
		}

		if (normal[1] > 0.0f)
		{
			normal[0] = -normal[0];
			normal[1] = -normal[1];
			normal[2] = -normal[2];
		}
		if (normal[1] >= 0.0f)
		{
			stats.skippedNormalCount++;
			continue;
		}

		for (nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			vertex.position[0] += normal[0] * depthNudge;
			vertex.position[1] += normal[1] * depthNudge;
			vertex.position[2] += normal[2] * depthNudge;
			vertex.prevPosition[0] += normal[0] * depthNudge;
			vertex.prevPosition[1] += normal[1] * depthNudge;
			vertex.prevPosition[2] += normal[2] * depthNudge;
			stats.vertexCount++;
		}
		stats.surfaceCount++;
	}

	return stats;
}

void TranslateGeometry(nri_scene::GeometryData& geometry, float dx, float dy, float dz, float prevDx, float prevDy, float prevDz)
{
	for (auto& vertex : geometry.vertices)
	{
		vertex.position[0] += dx;
		vertex.position[1] += dy;
		vertex.position[2] += dz;
		vertex.prevPosition[0] += prevDx;
		vertex.prevPosition[1] += prevDy;
		vertex.prevPosition[2] += prevDz;
	}
}

void AssignGeometryPortalIndices(const nri_scene::PTMapWorld& mapWorld, nri_scene::GeometryData& geometry)
{
	const size_t count = std::min(geometry.primitives.size(), geometry.primitiveProvenance.size());
	for (size_t i = 0; i < count; ++i)
	{
		geometry.primitives[i].portalIndex = UINT32_MAX;
		const uint32_t flags = geometry.primitives[i].flags;
		if ((flags & (nri_scene::MaterialFlag_Mirror | nri_scene::MaterialFlag_Portal)) == 0)
		{
			continue;
		}

		const int32_t portalIndex = nri_scene::FindMapWorldPortalIndex(mapWorld, geometry.primitiveProvenance[i]);
		if (portalIndex >= 0)
		{
			geometry.primitives[i].portalIndex = (uint32_t)portalIndex;
		}
	}
}

void AppendGeometry(const nri_scene::GeometryData& source, uint32_t materialIndexOffset, nri_scene::GeometryData& destination)
{
	const uint32_t vertexBase = (uint32_t)destination.vertices.size();
	destination.vertices.insert(destination.vertices.end(), source.vertices.begin(), source.vertices.end());

	destination.indices.reserve(destination.indices.size() + source.indices.size());
	for (uint32_t index : source.indices)
	{
		destination.indices.push_back(vertexBase + index);
	}

	destination.primitives.reserve(destination.primitives.size() + source.primitives.size());
	for (const auto& primitive : source.primitives)
	{
		nri_scene::PrimitiveData copy = primitive;
		copy.indices[0] += vertexBase;
		copy.indices[1] += vertexBase;
		copy.indices[2] += vertexBase;
		copy.materialIndex += materialIndexOffset;
		destination.primitives.push_back(copy);
	}

	destination.primitiveProvenance.insert(destination.primitiveProvenance.end(), source.primitiveProvenance.begin(), source.primitiveProvenance.end());
}
