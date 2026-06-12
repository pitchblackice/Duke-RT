#pragma once

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"

#include <cstdint>
#include <vector>

struct NRISceneFrameGeometrySelectionInputs
{
	uint64_t staticBuildSerial = 0;
	const nri_scene::GeometryData* staticGeometry = nullptr;
	const nri_scene::MaterialBridgeData* staticMaterialBridge = nullptr;
	const std::vector<nri_scene::MaterialData>* staticGpuMaterials = nullptr;

	const nri_scene::GeometryData* overlayGeometry = nullptr;
	uint32_t overlayMaterialOffset = 0;

	const nri_scene::MaterialBridgeData* combinedMaterialBridge = nullptr;
	const std::vector<nri_scene::MaterialData>* combinedGpuMaterials = nullptr;

	double* totalMs = nullptr;
	double* staticCopyMs = nullptr;
	double* overlayAppendMs = nullptr;
	double* selectMs = nullptr;
};

struct NRISceneFrameGeometrySelection
{
	const nri_scene::GeometryData* geometry = nullptr;
	const nri_scene::MaterialBridgeData* materialBridge = nullptr;
	const std::vector<nri_scene::MaterialData>* gpuMaterials = nullptr;

	uint32_t staticProbePrimitiveCount = 0;
	uint32_t combinedPrimitiveCount = 0;
	uint32_t combinedMaterialCount = 0;
	bool usedCombinedGeometry = false;
	bool usedStaticOnlyGeometry = false;
};

struct NRISceneFrameGeometryStaticChunkSlice
{
	bool active = false;
	uint32_t vertexOffset = 0;
	uint32_t vertexCount = 0;
	uint32_t indexOffset = 0;
	uint32_t indexCount = 0;
	uint32_t primitiveOffset = 0;
	uint32_t primitiveCount = 0;
};

struct NRISceneFrameGeometryStaticPrefixRefresh
{
	uint64_t staticBuildSerial = 0;
	const nri_scene::GeometryData* staticGeometry = nullptr;
	uint32_t staticMaterialCount = 0;
	const std::vector<NRISceneFrameGeometryStaticChunkSlice>* changedChunks = nullptr;
};

class NRISceneFrameGeometry
{
public:
	NRISceneFrameGeometrySelection SelectActiveGeometry(const NRISceneFrameGeometrySelectionInputs& inputs);
	void RefreshStaticPrefixForResidentUpdate(const NRISceneFrameGeometryStaticPrefixRefresh& refresh);
	void Reset();

private:
	struct CombinedGeometryCache
	{
		bool staticPrefixValid = false;
		uint64_t staticBuildSerial = 0;
		uint32_t staticVertexCount = 0;
		uint32_t staticIndexCount = 0;
		uint32_t staticPrimitiveCount = 0;
		uint32_t staticPrimitiveProvenanceCount = 0;
		uint32_t staticMaterialCount = 0;
		nri_scene::GeometryData geometry;
	};

	CombinedGeometryCache mCombinedGeometryCache;
};
