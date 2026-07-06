#pragma once

#include "tarray.h"

#include <cstdint>

class FVoxelModel;
class NRIRenderer;
struct FVoxelRawMeshStats;
struct FVoxelRawSlabRecord;
struct FVoxelRawFaceRecord;
struct FVoxelMeshData;

bool ShouldTraceNRIVoxelComputeMeshing();
bool ShouldRunNRIVoxelComputeMeshing();
bool ShouldEmitNRIVoxelComputeMeshing();
void QueueNRIVoxelComputeCountJob(
	FVoxelModel* model,
	const FVoxelRawMeshStats& stats,
	const TArray<FVoxelRawSlabRecord>* slabs,
	const TArray<FVoxelRawFaceRecord>* faces,
	const FVoxelMeshData& cpuMesh);
void DispatchNRIVoxelComputeMeshingDiagnostics(NRIRenderer& renderer, uint64_t frameNumber);
void DestroyNRIVoxelComputeMeshingDiagnostics(NRIRenderer& renderer);
