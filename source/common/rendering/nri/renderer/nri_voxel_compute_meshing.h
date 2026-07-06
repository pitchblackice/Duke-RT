#pragma once

#include "tarray.h"

#include <cstdint>

class FVoxelModel;
class NRIRenderer;
struct FVoxelRawMeshStats;
struct FVoxelRawSlabRecord;
struct FVoxelMeshData;

bool ShouldTraceNRIVoxelComputeMeshing();
bool ShouldRunNRIVoxelComputeMeshing();
void QueueNRIVoxelComputeCountJob(
	FVoxelModel* model,
	const FVoxelRawMeshStats& stats,
	const TArray<FVoxelRawSlabRecord>* slabs,
	const FVoxelMeshData& cpuMesh);
void DispatchNRIVoxelComputeMeshingDiagnostics(NRIRenderer& renderer, uint64_t frameNumber);
void DestroyNRIVoxelComputeMeshingDiagnostics(NRIRenderer& renderer);
