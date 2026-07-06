#pragma once

#include "tarray.h"

#include <cstdint>

class FVoxelModel;
class NRIRenderer;
struct FVoxelRawMeshStats;
struct FVoxelRawSlabRecord;
struct FVoxelRawFaceRecord;
struct FVoxelMeshData;
namespace nri_scene { struct GeometryData; }

enum class NRIVoxelComputeGeneratedGeometryStatus : uint8_t
{
	Unavailable,
	Queued,
	Ready,
	Failed,
};

bool ShouldTraceNRIVoxelComputeMeshing();
bool ShouldRunNRIVoxelComputeMeshing();
bool ShouldEmitNRIVoxelComputeMeshing();
bool ShouldConsumeNRIVoxelComputeMeshing();
void QueueNRIVoxelComputeCountJob(
	FVoxelModel* model,
	const FVoxelRawMeshStats& stats,
	const TArray<FVoxelRawSlabRecord>* slabs,
	const TArray<FVoxelRawFaceRecord>* faces,
	const FVoxelMeshData& cpuMesh);
NRIVoxelComputeGeneratedGeometryStatus RequestNRIVoxelComputeGeneratedGeometry(uint64_t requestKey, FVoxelModel* model);
bool TakeNRIVoxelComputeGeneratedGeometry(uint64_t requestKey, nri_scene::GeometryData& outGeometry, uint32_t* outJobId = nullptr);
void DispatchNRIVoxelComputeMeshingDiagnostics(NRIRenderer& renderer, uint64_t frameNumber);
void DestroyNRIVoxelComputeMeshingDiagnostics(NRIRenderer& renderer);
