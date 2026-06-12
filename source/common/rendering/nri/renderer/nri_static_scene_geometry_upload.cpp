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

nri::AccessStage StaticGeometryComputeShaderResourceAccess()
{
	return { nri::AccessBits::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER };
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

bool UploadResidentStaticAtlasIndexBuffer(
	const NRIStaticSceneGeometryUploadServices& services,
	NRIBufferResource& indexBuffer,
	SceneBufferDebugStats& indexStats,
	const std::vector<uint32_t>& atlasIndices)
{
	return services.EnsureResidentStructuredBuffer(
		indexBuffer,
		indexStats,
		atlasIndices.data(),
		atlasIndices.size() * sizeof(uint32_t),
		sizeof(uint32_t),
		NRIStaticSceneGeometryUploadFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT),
		StaticGeometryAccelerationStructureBuildInputAccess(),
		"resident_chunk_write",
		ResidentUploadKind_Index);
}

bool UploadResidentStaticAtlasPrimitiveBuffer(
	const NRIStaticSceneGeometryUploadServices& services,
	NRIBufferResource& primitiveBuffer,
	SceneBufferDebugStats& primitiveStats,
	const std::vector<nri_scene::PrimitiveData>& atlasPrimitives)
{
	return services.EnsureResidentStructuredBuffer(
		primitiveBuffer,
		primitiveStats,
		atlasPrimitives.data(),
		atlasPrimitives.size() * sizeof(nri_scene::PrimitiveData),
		sizeof(nri_scene::PrimitiveData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		StaticGeometryComputeShaderResourceAccess(),
		"resident_chunk_write",
		ResidentUploadKind_Primitive);
}

bool UploadResidentStaticAtlasMaterialBuffer(
	const NRIStaticSceneGeometryUploadServices& services,
	NRIBufferResource& materialBuffer,
	SceneBufferDebugStats& materialStats,
	const std::vector<nri_scene::MaterialData>& atlasMaterials)
{
	return services.EnsureResidentStructuredBuffer(
		materialBuffer,
		materialStats,
		atlasMaterials.data(),
		atlasMaterials.size() * sizeof(nri_scene::MaterialData),
		sizeof(nri_scene::MaterialData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		StaticGeometryComputeShaderResourceAccess(),
		"resident_chunk_write",
		ResidentUploadKind_Material);
}
}
