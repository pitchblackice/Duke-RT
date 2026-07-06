#include "Include/VoxelComputeConstants.hlsli"

StructuredBuffer<NRIVoxelComputeJob> VoxelComputeJobs : register(t0, space0);
StructuredBuffer<NRIVoxelComputeSlabRecord> VoxelComputeSlabs : register(t1, space0);
StructuredBuffer<NRIVoxelComputeFaceRecord> VoxelComputeFaces : register(t2, space0);
RWStructuredBuffer<NRIVoxelComputeResult> VoxelComputeResults : register(u0, space1);
RWStructuredBuffer<NRIVoxelComputeSceneVertex> VoxelComputeVertices : register(u1, space1);
RWStructuredBuffer<uint> VoxelComputeIndices : register(u2, space1);
RWStructuredBuffer<NRIVoxelComputePrimitiveData> VoxelComputePrimitives : register(u3, space1);

uint HashCombine(uint hash, uint value)
{
	hash ^= value;
	hash *= 16777619u;
	return hash;
}

float2 VoxelUv(uint color)
{
	return float2(((color & 15u) + 0.5f) / 16.0f, ((color >> 4u) + 0.5f) / 16.0f);
}

float3 TransformVoxelPoint(NRIVoxelComputeJob job, int x, int y, int z)
{
	return float3((float)x - job.PivotX, -(float)z + job.PivotZ, -(float)y + job.PivotY);
}

NRIVoxelComputeSceneVertex MakeVertex(float3 position, float2 uv)
{
	NRIVoxelComputeSceneVertex vertex;
	vertex.Position = position;
	vertex.PrevPosition = position;
	vertex.Uv = uv;
	return vertex;
}

NRIVoxelComputePrimitiveData MakePrimitive(uint3 indices, uint materialIndex, float2 uv0, float2 uv1, float2 uv2, float3 p0, float3 p1, float3 p2)
{
	NRIVoxelComputePrimitiveData primitive;
	primitive.Indices = indices;
	primitive.MaterialIndex = materialIndex;
	primitive.Uv0 = uv0;
	primitive.Uv1 = uv1;
	primitive.Uv2 = uv2;
	const float3 normal = cross(p1 - p0, p2 - p0);
	const float normalLengthSq = dot(normal, normal);
	primitive.Normal = normalLengthSq > 1.0e-12f ? normal * rsqrt(normalLengthSq) : float3(0.0f, 1.0f, 0.0f);
	primitive.Flags = 0u;
	primitive.PortalIndex = 0xffffffffu;
	primitive.Reserved0 = 0xffffffffu;
	return primitive;
}

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint jobIndex = dispatchThreadId.x;
	if (jobIndex >= gVoxelComputeConstants.JobCount)
	{
		return;
	}

	const NRIVoxelComputeJob job = VoxelComputeJobs[jobIndex];
	uint vertexHash = 2166136261u;
	uint indexHash = 2166136261u;
	uint primitiveHash = 2166136261u;
	uint emittedFaces = 0u;

	for (uint localFace = 0u; localFace < job.ExpectedFaces; ++localFace)
	{
		const uint faceIndex = job.FaceOffset + localFace;
		if (faceIndex >= gVoxelComputeConstants.FaceRecordCount)
		{
			break;
		}

		const NRIVoxelComputeFaceRecord face = VoxelComputeFaces[faceIndex];
		const float2 uv = VoxelUv(face.Color);
		const float3 p0 = TransformVoxelPoint(job, face.X[0], face.Y[0], face.Z[0]);
		const float3 p1 = TransformVoxelPoint(job, face.X[1], face.Y[1], face.Z[1]);
		const float3 p2 = TransformVoxelPoint(job, face.X[3], face.Y[3], face.Z[3]);
		const float3 p3 = TransformVoxelPoint(job, face.X[2], face.Y[2], face.Z[2]);

		const uint vertexBase = job.VertexOffset + localFace * 4u;
		const uint indexBase = job.IndexOffset + localFace * 6u;
		const uint primitiveBase = job.PrimitiveOffset + localFace * 2u;

		VoxelComputeVertices[vertexBase + 0u] = MakeVertex(p0, uv);
		VoxelComputeVertices[vertexBase + 1u] = MakeVertex(p1, uv);
		VoxelComputeVertices[vertexBase + 2u] = MakeVertex(p2, uv);
		VoxelComputeVertices[vertexBase + 3u] = MakeVertex(p3, uv);

		const uint indices[6] = {
			vertexBase + 0u,
			vertexBase + 1u,
			vertexBase + 3u,
			vertexBase + 1u,
			vertexBase + 2u,
			vertexBase + 3u
		};
		[unroll]
		for (uint i = 0u; i < 6u; ++i)
		{
			VoxelComputeIndices[indexBase + i] = indices[i];
			indexHash = HashCombine(indexHash, indices[i]);
		}

		VoxelComputePrimitives[primitiveBase + 0u] = MakePrimitive(uint3(indices[0], indices[1], indices[2]), face.MaterialIndex, uv, uv, uv, p0, p1, p3);
		VoxelComputePrimitives[primitiveBase + 1u] = MakePrimitive(uint3(indices[3], indices[4], indices[5]), face.MaterialIndex, uv, uv, uv, p1, p2, p3);

		[unroll]
		for (uint v = 0u; v < 4u; ++v)
		{
			const NRIVoxelComputeSceneVertex vertex = VoxelComputeVertices[vertexBase + v];
			vertexHash = HashCombine(vertexHash, asuint(vertex.Position.x));
			vertexHash = HashCombine(vertexHash, asuint(vertex.Position.y));
			vertexHash = HashCombine(vertexHash, asuint(vertex.Position.z));
			vertexHash = HashCombine(vertexHash, asuint(vertex.Uv.x));
			vertexHash = HashCombine(vertexHash, asuint(vertex.Uv.y));
		}
		primitiveHash = HashCombine(primitiveHash, face.MaterialIndex);
		primitiveHash = HashCombine(primitiveHash, indices[0]);
		primitiveHash = HashCombine(primitiveHash, indices[1]);
		primitiveHash = HashCombine(primitiveHash, indices[2]);
		primitiveHash = HashCombine(primitiveHash, indices[3]);
		primitiveHash = HashCombine(primitiveHash, indices[4]);
		primitiveHash = HashCombine(primitiveHash, indices[5]);
		++emittedFaces;
	}

	NRIVoxelComputeResult result;
	result.FaceCount = emittedFaces;
	result.IndexCount = emittedFaces * 6u;
	result.VertexCountNoDedupe = emittedFaces * 4u;
	result.VoxelCount = job.ExpectedVoxels;
	result.SlabCount = job.SlabCount;
	result.PrimitiveCount = emittedFaces * 2u;
	result.MismatchMask = 0u;
	result.MismatchMask |= result.FaceCount == job.ExpectedFaces ? 0u : 1u;
	result.MismatchMask |= result.IndexCount == job.ExpectedIndices ? 0u : 2u;
	result.MismatchMask |= result.VertexCountNoDedupe == job.ExpectedVerticesNoDedupe ? 0u : 4u;
	result.MismatchMask |= result.PrimitiveCount == job.ExpectedFaces * 2u ? 0u : 16u;
	result.JobId = job.JobId;
	result.Status = result.MismatchMask == 0u ? NRI_VOXEL_COMPUTE_STATUS_EMIT_OK : NRI_VOXEL_COMPUTE_STATUS_EMIT_MISMATCH;
	result.VertexHash = vertexHash;
	result.IndexHash = indexHash;
	result.PrimitiveHash = primitiveHash;
	VoxelComputeResults[jobIndex] = result;
}
