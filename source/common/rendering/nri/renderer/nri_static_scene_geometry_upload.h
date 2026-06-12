#pragma once

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"
#include "nri_frame_resources.h"
#include "nri_static_scene.h"

#include <cstdint>
#include <vector>

struct NRIStaticSceneGeometryUploadServices
{
	using EnsureResidentStructuredBufferFn = bool (*)(
		void* user,
		NRIBufferResource& resource,
		SceneBufferDebugStats& stats,
		const void* data,
		uint64_t size,
		uint32_t stride,
		nri::BufferUsageBits usage,
		nri::AccessStage after,
		const char* waitReason,
		int uploadKind);
	using RefreshResidentStaticSceneDataSetFn = bool (*)(void* user);
	using NoteResidentStaticAtlasGrowFn = void (*)(void* user);

	void* user = nullptr;
	EnsureResidentStructuredBufferFn ensureResidentStructuredBuffer = nullptr;
	RefreshResidentStaticSceneDataSetFn refreshResidentStaticSceneDataSet = nullptr;
	NoteResidentStaticAtlasGrowFn noteResidentStaticAtlasGrow = nullptr;

	bool EnsureResidentStructuredBuffer(
		NRIBufferResource& resource,
		SceneBufferDebugStats& stats,
		const void* data,
		uint64_t size,
		uint32_t stride,
		nri::BufferUsageBits usage,
		nri::AccessStage after,
		const char* waitReason,
		int uploadKind) const
	{
		return ensureResidentStructuredBuffer != nullptr &&
			ensureResidentStructuredBuffer(user, resource, stats, data, size, stride, usage, after, waitReason, uploadKind);
	}

	bool RefreshResidentStaticSceneDataSet() const
	{
		return refreshResidentStaticSceneDataSet != nullptr &&
			refreshResidentStaticSceneDataSet(user);
	}

	void NoteResidentStaticAtlasGrow() const
	{
		if (noteResidentStaticAtlasGrow != nullptr)
		{
			noteResidentStaticAtlasGrow(user);
		}
	}
};

namespace nri_static_scene_geometry_upload
{
bool UploadResidentStaticAtlasVertexBuffer(
	const NRIStaticSceneGeometryUploadServices& services,
	NRIBufferResource& vertexBuffer,
	SceneBufferDebugStats& vertexStats,
	const std::vector<nri_scene::SceneVertex>& atlasVertices);
bool UploadResidentStaticAtlasIndexBuffer(
	const NRIStaticSceneGeometryUploadServices& services,
	NRIBufferResource& indexBuffer,
	SceneBufferDebugStats& indexStats,
	const std::vector<uint32_t>& atlasIndices);
bool UploadResidentStaticAtlasPrimitiveBuffer(
	const NRIStaticSceneGeometryUploadServices& services,
	NRIBufferResource& primitiveBuffer,
	SceneBufferDebugStats& primitiveStats,
	const std::vector<nri_scene::PrimitiveData>& atlasPrimitives);
bool UploadResidentStaticAtlasMaterialBuffer(
	const NRIStaticSceneGeometryUploadServices& services,
	NRIBufferResource& materialBuffer,
	SceneBufferDebugStats& materialStats,
	const std::vector<nri_scene::MaterialData>& atlasMaterials);
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
	const std::vector<nri_scene::MaterialData>& gpuMaterials);
}
