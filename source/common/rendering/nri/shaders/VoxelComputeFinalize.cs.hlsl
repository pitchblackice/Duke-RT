#include "Include/VoxelComputeConstants.hlsli"

StructuredBuffer<NRIVoxelComputeJob> VoxelComputeJobs : register(t0, space0);
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
	NRIVoxelComputeResult result = VoxelComputeResults[jobIndex];
	if (result.JobId != job.JobId || result.Status != NRI_VOXEL_COMPUTE_STATUS_SCAN_OK)
	{
		result.MismatchMask |= NRI_VOXEL_COMPUTE_MISMATCH_ALGORITHM;
	}
	result.Status = result.MismatchMask == 0u ? NRI_VOXEL_COMPUTE_STATUS_EMIT_OK : NRI_VOXEL_COMPUTE_STATUS_EMIT_MISMATCH;
	result.VertexHash = 0u;
	result.IndexHash = 0u;
	result.PrimitiveHash = 0u;
	VoxelComputeResults[jobIndex] = result;
}
