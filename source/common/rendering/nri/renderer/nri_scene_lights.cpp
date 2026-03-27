#include "nri_scene_lights.h"

#include <algorithm>
#include <cmath>

namespace
{
	void ComputeSurfaceBounds(const nri_scene::SurfaceRef& surface, float outCenter[3], float& outRadius)
	{
		outCenter[0] = 0.0f;
		outCenter[1] = 0.0f;
		outCenter[2] = 0.0f;
		outRadius = 0.0f;

		if (surface.vertices.empty())
		{
			return;
		}

		for (const nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			outCenter[0] += vertex.position[0];
			outCenter[1] += vertex.position[1];
			outCenter[2] += vertex.position[2];
		}

		const float invCount = 1.0f / (float)surface.vertices.size();
		outCenter[0] *= invCount;
		outCenter[1] *= invCount;
		outCenter[2] *= invCount;

		for (const nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			const float dx = vertex.position[0] - outCenter[0];
			const float dy = vertex.position[1] - outCenter[1];
			const float dz = vertex.position[2] - outCenter[2];
			outRadius = std::max(outRadius, std::sqrt(dx * dx + dy * dy + dz * dz));
		}
	}
}

void SceneLightSystem::Reset()
{
	mSurfaceRecords.clear();
	mFrameSerial = 0;
}

void SceneLightSystem::BeginFrame(uint64_t frameSerial)
{
	mFrameSerial = frameSerial;
	mSurfaceRecords.clear();
}

void SceneLightSystem::AppendSceneView(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials, SceneLightRecordSource source)
{
	uint32_t materialIndex = 0;
	AppendSurfaceList(sceneView.opaqueWalls, materials, source, materialIndex);
	AppendSurfaceList(sceneView.opaqueFlats, materials, source, materialIndex);
	AppendSurfaceList(sceneView.opaqueSprites, materials, source, materialIndex);
}

void SceneLightSystem::AppendSurfaceList(const std::vector<nri_scene::SurfaceRef>& surfaces, const nri_scene::MaterialBridgeData& materials, SceneLightRecordSource source, uint32_t& inOutMaterialIndex)
{
	for (const nri_scene::SurfaceRef& surface : surfaces)
	{
		SurfaceRecord record = {};
		record.source = source;
		record.materialIndex = inOutMaterialIndex;
		record.provenance = surface.provenance;
		ComputeSurfaceBounds(surface, record.center, record.boundsRadius);

		if (inOutMaterialIndex < materials.lightMetadata.size())
		{
			record.material = materials.lightMetadata[inOutMaterialIndex];
		}
		else if (inOutMaterialIndex < materials.materials.size())
		{
			record.material.paletteIndex = materials.materials[inOutMaterialIndex].paletteIndex;
			record.material.materialFlags = materials.materials[inOutMaterialIndex].flags;
			record.material.alpha = materials.materials[inOutMaterialIndex].alpha;
			record.material.lightLevel = materials.materials[inOutMaterialIndex].lightLevel;
		}

		mSurfaceRecords.push_back(record);
		++inOutMaterialIndex;
	}
}
