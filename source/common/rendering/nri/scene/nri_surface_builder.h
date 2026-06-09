#pragma once

#include "nri_scene_surface_types.h"

#include "flatvertices.h"

#include <cstdint>

namespace nri_scene
{
inline CapturedVertex BuildCapturedVertex(const FFlatVertex& source)
{
	CapturedVertex vertex = {};
	vertex.position[0] = source.x;
	vertex.position[1] = source.z;
	vertex.position[2] = source.y;
	vertex.prevPosition[0] = vertex.position[0];
	vertex.prevPosition[1] = vertex.position[1];
	vertex.prevPosition[2] = vertex.position[2];
	vertex.uv[0] = source.u;
	vertex.uv[1] = source.v;
	return vertex;
}

inline CapturedVertex BuildCapturedVertex(float x, float y, float z, float u, float v)
{
	CapturedVertex vertex = {};
	vertex.position[0] = x;
	vertex.position[1] = y;
	vertex.position[2] = z;
	vertex.prevPosition[0] = x;
	vertex.prevPosition[1] = y;
	vertex.prevPosition[2] = z;
	vertex.uv[0] = u;
	vertex.uv[1] = v;
	return vertex;
}

inline SurfaceRef BuildQuadSurface(const FFlatVertex* vertices, const MaterialRef& material, const SurfaceProvenance& provenance)
{
	SurfaceRef surface = {};
	surface.material = material;
	surface.provenance = provenance;
	surface.vertices.reserve(4);
	for (uint32_t i = 0; i < 4; ++i)
	{
		surface.vertices.push_back(BuildCapturedVertex(vertices[i]));
	}
	return surface;
}

inline SurfaceRef BuildSurfaceFromVertices(const FFlatVertex* vertices, uint32_t vertexCount, const MaterialRef& material, const SurfaceProvenance& provenance)
{
	SurfaceRef surface = {};
	surface.material = material;
	surface.provenance = provenance;
	surface.vertices.reserve(vertexCount);
	for (uint32_t i = 0; i < vertexCount; ++i)
	{
		surface.vertices.push_back(BuildCapturedVertex(vertices[i]));
	}
	return surface;
}
}
