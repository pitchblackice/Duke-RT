#include "Include/VoxelComputeConstants.hlsli"

StructuredBuffer<NRIVoxelComputeJob> VoxelComputeJobs : register(t0, space0);
StructuredBuffer<NRIVoxelComputeSlabRecord> VoxelComputeSlabs : register(t1, space0);
RWStructuredBuffer<NRIVoxelComputeResult> VoxelComputeResults : register(u0, space1);

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint jobIndex = dispatchThreadId.x;
	if (jobIndex >= gVoxelComputeConstants.JobCount)
	{
		return;
	}

	const NRIVoxelComputeJob job = VoxelComputeJobs[jobIndex];
	uint faceCount = 0;
	uint voxelCount = 0;
	for (uint i = 0; i < job.SlabCount; ++i)
	{
		const uint slabIndex = job.SlabOffset + i;
		if (slabIndex >= gVoxelComputeConstants.SlabRecordCount)
		{
			break;
		}

		const NRIVoxelComputeSlabRecord slab = VoxelComputeSlabs[slabIndex];
		const uint cull = slab.CullMask;
		const uint topFaces = (cull & 16u) != 0u ? 1u : 0u;
		const uint bottomFaces = (cull & 32u) != 0u ? 1u : 0u;
		const uint sideDirections =
			((cull & 1u) != 0u ? 1u : 0u) +
			((cull & 2u) != 0u ? 1u : 0u) +
			((cull & 4u) != 0u ? 1u : 0u) +
			((cull & 8u) != 0u ? 1u : 0u);
		faceCount += topFaces + bottomFaces + sideDirections * slab.ColorRunCount;
		voxelCount += slab.ZLength;
	}

	NRIVoxelComputeResult result;
	result.FaceCount = faceCount;
	result.IndexCount = faceCount * 6u;
	result.VertexCountNoDedupe = faceCount * 4u;
	result.VoxelCount = voxelCount;
	result.SlabCount = job.SlabCount;
	result.PrimitiveCount = faceCount * 2u;
	result.MismatchMask = 0u;
	result.MismatchMask |= result.FaceCount == job.ExpectedFaces ? 0u : 1u;
	result.MismatchMask |= result.IndexCount == job.ExpectedIndices ? 0u : 2u;
	result.MismatchMask |= result.VertexCountNoDedupe == job.ExpectedVerticesNoDedupe ? 0u : 4u;
	result.MismatchMask |= result.VoxelCount == job.ExpectedVoxels ? 0u : 8u;
	result.JobId = job.JobId;
	result.Status = result.MismatchMask == 0u ? NRI_VOXEL_COMPUTE_STATUS_COUNT_OK : NRI_VOXEL_COMPUTE_STATUS_COUNT_MISMATCH;
	result.VertexHash = 0u;
	result.IndexHash = 0u;
	result.PrimitiveHash = 0u;
	VoxelComputeResults[jobIndex] = result;
}
