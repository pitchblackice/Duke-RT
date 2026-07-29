#include "Include/SmokeViewWorkResources.hlsli"

// One deterministic group processes fixed chunks in ascending order. The
// exclusive scan is parallel inside a chunk; lane zero alone advances the
// carry between chunks, preserving exact column-major compact ordering.
#define NRI_SMOKE_VIEW_PREFIX_THREADS 256u
groupshared uint gPrefixValues[NRI_SMOKE_VIEW_PREFIX_THREADS];
groupshared uint gPrefixCarry;
groupshared uint gPrefixChunkTotal;

[numthreads(NRI_SMOKE_VIEW_PREFIX_THREADS, 1, 1)]
void main(uint lane : SV_GroupThreadID)
{
	const uint columnCount = SmokeViewColumnCount();
	if (lane == 0u)
		gPrefixCarry = 0u;
	GroupMemoryBarrierWithGroupSync();

	[loop]
	for (uint chunkBase = 0u; chunkBase < columnCount;
		chunkBase += NRI_SMOKE_VIEW_PREFIX_THREADS)
	{
		const uint column = chunkBase + lane;
		gPrefixValues[lane] = column < columnCount ? gViewColumnOffsets[column] : 0u;
		GroupMemoryBarrierWithGroupSync();

		[unroll]
		for (uint stride = 1u; stride < NRI_SMOKE_VIEW_PREFIX_THREADS; stride <<= 1u)
		{
			const uint index = ((lane + 1u) * stride * 2u) - 1u;
			if (index < NRI_SMOKE_VIEW_PREFIX_THREADS)
				gPrefixValues[index] += gPrefixValues[index - stride];
			GroupMemoryBarrierWithGroupSync();
		}

		if (lane == 0u)
		{
			gPrefixChunkTotal = gPrefixValues[NRI_SMOKE_VIEW_PREFIX_THREADS - 1u];
			gPrefixValues[NRI_SMOKE_VIEW_PREFIX_THREADS - 1u] = 0u;
		}
		GroupMemoryBarrierWithGroupSync();

		[unroll]
		for (uint stride = NRI_SMOKE_VIEW_PREFIX_THREADS >> 1u; stride > 0u; stride >>= 1u)
		{
			const uint index = ((lane + 1u) * stride * 2u) - 1u;
			if (index < NRI_SMOKE_VIEW_PREFIX_THREADS)
			{
				const uint left = gPrefixValues[index - stride];
				gPrefixValues[index - stride] = gPrefixValues[index];
				gPrefixValues[index] += left;
			}
			GroupMemoryBarrierWithGroupSync();
		}

		if (column < columnCount)
			gViewColumnOffsets[column] = gPrefixCarry + gPrefixValues[lane];
		GroupMemoryBarrierWithGroupSync();
		if (lane == 0u)
			gPrefixCarry += gPrefixChunkTotal;
		GroupMemoryBarrierWithGroupSync();
	}

	if (lane != 0u)
		return;
	const uint compactCount = gPrefixCarry;
	gViewWorkControl[0].PrefixColumns = columnCount;
	if (compactCount > gViewConstants.FroxelCapacity)
	{
		gViewWorkControl[0].Overflow = 1u;
		gViewWorkControl[0].CompactCount = 0u;
		gViewIndirectArgs[0] = uint3(0u, 1u, 1u);
		gViewIndirectArgs[1] = uint3(0u, 1u, 1u);
		return;
	}
	gViewWorkControl[0].CompactCount = compactCount;
	gViewIndirectArgs[0] = uint3((compactCount + 63u) / 64u, 1u, 1u);
	gViewIndirectArgs[1] = uint3(0u, 1u, 1u);
}
