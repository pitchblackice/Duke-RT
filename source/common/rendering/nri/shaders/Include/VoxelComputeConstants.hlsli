#ifndef RAZE_NRI_VOXEL_COMPUTE_CONSTANTS_HLSLI
#define RAZE_NRI_VOXEL_COMPUTE_CONSTANTS_HLSLI

#include "NRI.hlsl"

#define NRI_VOXEL_COMPUTE_SET_INPUTS 0
#define NRI_VOXEL_COMPUTE_SET_OUTPUTS 1
#define NRI_VOXEL_COMPUTE_SET_ROOT 2
#define NRI_VOXEL_COMPUTE_ROOT_REGISTER 0

struct NRIVoxelComputeConstants
{
	uint JobCount;
	uint SlabRecordCount;
	uint Reserved0;
	uint Reserved1;
};

struct NRIVoxelComputeJob
{
	uint SlabOffset;
	uint SlabCount;
	uint ExpectedFaces;
	uint ExpectedIndices;
	uint ExpectedVerticesNoDedupe;
	uint ExpectedVoxels;
	uint JobId;
	uint Reserved0;
};

struct NRIVoxelComputeSlabRecord
{
	uint CullMask;
	uint ZLength;
	uint ColorRunCount;
	uint Reserved0;
};

struct NRIVoxelComputeResult
{
	uint FaceCount;
	uint IndexCount;
	uint VertexCountNoDedupe;
	uint VoxelCount;
	uint SlabCount;
	uint MismatchMask;
	uint JobId;
	uint Status;
};

NRI_ROOT_CONSTANTS(NRIVoxelComputeConstants, gVoxelComputeConstants, NRI_VOXEL_COMPUTE_ROOT_REGISTER, NRI_VOXEL_COMPUTE_SET_ROOT);

#endif
