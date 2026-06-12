#include "nri_static_scene_geometry_upload.h"

namespace
{
template<typename T>
T NRIStaticSceneGeometryUploadFlags(T a, T b)
{
	return (T)((uint32_t)a | (uint32_t)b);
}

nri::AccessStage StaticGeometryAccelerationStructureBuildInputAccess()
{
	return { nri::AccessBits::SHADER_RESOURCE, nri::StageBits::ALL_SHADERS };
}
}

namespace nri_static_scene_geometry_upload
{
bool UploadResidentStaticAtlasVertexBuffer(
	const NRIStaticSceneGeometryUploadServices& services,
	NRIBufferResource& vertexBuffer,
	SceneBufferDebugStats& vertexStats,
	const std::vector<nri_scene::SceneVertex>& atlasVertices)
{
	return services.EnsureResidentStructuredBuffer(
		vertexBuffer,
		vertexStats,
		atlasVertices.data(),
		atlasVertices.size() * sizeof(nri_scene::SceneVertex),
		sizeof(nri_scene::SceneVertex),
		NRIStaticSceneGeometryUploadFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT),
		StaticGeometryAccelerationStructureBuildInputAccess(),
		"resident_chunk_write",
		ResidentUploadKind_Vertex);
}
}
