#include "nri_static_scene_geometry_upload.h"

#include "nri_static_scene_geometry.h"

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

bool UploadStaticMapChunkAtlas(
	const nri_scene::PTMapWorld& mapWorld,
	const NRIStaticSceneGeometryUploadServices& services,
	NRIBufferResource& vertexBuffer,
	SceneBufferDebugStats& vertexStats,
	NRIBufferResource& indexBuffer,
	SceneBufferDebugStats& indexStats,
	NRIBufferResource& primitiveBuffer,
	SceneBufferDebugStats& primitiveStats,
	NRIBufferResource& materialBuffer,
	SceneBufferDebugStats& materialStats,
	StaticMapChunkAtlas& atlas,
	const StaticMapSceneCache& staticScene,
	const std::vector<nri_scene::MaterialData>& gpuMaterials)
{
	if (!nri_static_scene_geometry::BuildStaticMapChunkAtlasLayout(staticScene, atlas))
	{
		return false;
	}

	std::vector<nri_scene::SceneVertex> atlasVertices(atlas.vertexCapacity);
	std::vector<uint32_t> atlasIndices(atlas.indexCapacity);
	std::vector<nri_scene::PrimitiveData> atlasPrimitives(atlas.primitiveCapacity);
	std::vector<nri_scene::MaterialData> atlasMaterials(atlas.materialCapacity);

	for (uint32_t chunkListIndex = 0; chunkListIndex < staticScene.chunks.size(); ++chunkListIndex)
	{
		const auto& sourceChunk = staticScene.chunks[chunkListIndex];
		const auto& atlasChunk = atlas.chunks[chunkListIndex];
		nri_static_scene_geometry::CopyChunkGeometryToAtlas(
			mapWorld,
			staticScene.geometry,
			sourceChunk,
			atlasChunk,
			atlasVertices,
			atlasIndices,
			atlasPrimitives);
		nri_static_scene_geometry::CopyChunkMaterialsToAtlas(
			gpuMaterials,
			sourceChunk,
			atlasChunk,
			atlasMaterials);
	}

	return
		UploadResidentStaticAtlasVertexBuffer(services, vertexBuffer, vertexStats, atlasVertices) &&
		UploadResidentStaticAtlasIndexBuffer(services, indexBuffer, indexStats, atlasIndices) &&
		UploadResidentStaticAtlasPrimitiveBuffer(services, primitiveBuffer, primitiveStats, atlasPrimitives) &&
		UploadResidentStaticAtlasMaterialBuffer(services, materialBuffer, materialStats, atlasMaterials);
}

bool UploadStaticMapChunkMaterialAtlas(
	const NRIStaticSceneGeometryUploadServices& services,
	NRIBufferResource& materialBuffer,
	SceneBufferDebugStats& materialStats,
	const StaticMapChunkAtlas& atlas,
	const StaticMapSceneCache& staticScene,
	const std::vector<nri_scene::MaterialData>& gpuMaterials)
{
	if (!atlas.valid || atlas.chunks.size() != staticScene.chunks.size())
	{
		return false;
	}

	std::vector<nri_scene::MaterialData> atlasMaterials(atlas.materialCount);
	for (uint32_t chunkListIndex = 0; chunkListIndex < staticScene.chunks.size(); ++chunkListIndex)
	{
		nri_static_scene_geometry::CopyChunkMaterialsToAtlas(
			gpuMaterials,
			staticScene.chunks[chunkListIndex],
			atlas.chunks[chunkListIndex],
			atlasMaterials);
	}

	return UploadResidentStaticAtlasMaterialBuffer(
		services,
		materialBuffer,
		materialStats,
		atlasMaterials);
}
}
