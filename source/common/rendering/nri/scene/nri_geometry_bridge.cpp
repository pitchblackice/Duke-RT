#include "nri_geometry_bridge.h"

#include <algorithm>

namespace
{
	using namespace nri_scene;

	SceneVertex MakeVertex(const FFlatVertex& source)
	{
		SceneVertex vertex = {};
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

	SceneVertex MakeVertex(const CapturedVertex& source)
	{
		SceneVertex vertex = {};
		vertex.position[0] = source.position[0];
		vertex.position[1] = source.position[1];
		vertex.position[2] = source.position[2];
		vertex.prevPosition[0] = source.prevPosition[0];
		vertex.prevPosition[1] = source.prevPosition[1];
		vertex.prevPosition[2] = source.prevPosition[2];
		vertex.uv[0] = source.uv[0];
		vertex.uv[1] = source.uv[1];
		return vertex;
	}

	void ComputeNormal(const SceneVertex& a, const SceneVertex& b, const SceneVertex& c, float outNormal[3])
	{
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];

		const float nx = aby * acz - abz * acy;
		const float ny = abz * acx - abx * acz;
		const float nz = abx * acy - aby * acx;
		const float length = std::max(0.0001f, sqrtf(nx * nx + ny * ny + nz * nz));

		outNormal[0] = nx / length;
		outNormal[1] = ny / length;
		outNormal[2] = nz / length;
	}

	void AppendTriangle(const SceneVertex& v0, const SceneVertex& v1, const SceneVertex& v2, uint32_t materialIndex, uint32_t flags, GeometryData& outGeometry)
	{
		const uint32_t vertexBase = (uint32_t)outGeometry.vertices.size();
		outGeometry.vertices.push_back(v0);
		outGeometry.vertices.push_back(v1);
		outGeometry.vertices.push_back(v2);

		outGeometry.indices.push_back(vertexBase + 0);
		outGeometry.indices.push_back(vertexBase + 1);
		outGeometry.indices.push_back(vertexBase + 2);

		PrimitiveData primitive = {};
		primitive.indices[0] = vertexBase + 0;
		primitive.indices[1] = vertexBase + 1;
		primitive.indices[2] = vertexBase + 2;
		primitive.materialIndex = materialIndex;
		primitive.uv0[0] = v0.uv[0];
		primitive.uv0[1] = v0.uv[1];
		primitive.uv1[0] = v1.uv[0];
		primitive.uv1[1] = v1.uv[1];
		primitive.uv2[0] = v2.uv[0];
		primitive.uv2[1] = v2.uv[1];
		primitive.flags = flags;
		ComputeNormal(v0, v1, v2, primitive.normal);
		outGeometry.primitives.push_back(primitive);
	}
}

namespace nri_scene
{
void BuildGeometry(const SceneView& sceneView, GeometryData& outGeometry)
{
	outGeometry = {};

	uint32_t materialIndex = 0;
	for (const SurfaceRef& wall : sceneView.opaqueWalls)
	{
		if (wall.vertices.size() < 3)
		{
			materialIndex++;
			continue;
		}

		SceneVertex root = MakeVertex(wall.vertices[0]);
		for (uint32_t i = 1; i + 1 < wall.vertices.size(); ++i)
		{
			AppendTriangle(root, MakeVertex(wall.vertices[i]), MakeVertex(wall.vertices[i + 1]), materialIndex, wall.material.flags, outGeometry);
		}

		materialIndex++;
	}

	for (const SurfaceRef& flat : sceneView.opaqueFlats)
	{
		if (flat.vertices.size() < 3)
		{
			materialIndex++;
			continue;
		}

		for (uint32_t i = 0; i + 2 < flat.vertices.size(); i += 3)
		{
			AppendTriangle(MakeVertex(flat.vertices[i]), MakeVertex(flat.vertices[i + 1]), MakeVertex(flat.vertices[i + 2]), materialIndex, flat.material.flags, outGeometry);
		}

		materialIndex++;
	}

	for (const SurfaceRef& sprite : sceneView.opaqueSprites)
	{
		if (sprite.vertices.size() < 3)
		{
			materialIndex++;
			continue;
		}

		SceneVertex root = MakeVertex(sprite.vertices[0]);
		for (uint32_t i = 1; i + 1 < sprite.vertices.size(); ++i)
		{
			AppendTriangle(root, MakeVertex(sprite.vertices[i]), MakeVertex(sprite.vertices[i + 1]), materialIndex, sprite.material.flags, outGeometry);
		}

		materialIndex++;
	}
}
}
