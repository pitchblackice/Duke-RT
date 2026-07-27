#include "Include/SmokeViewWorkResources.hlsli"

// Correctness-first deterministic exclusive scan. A later checkpoint may
// parallelize this without changing compact ordering or overflow semantics.
[numthreads(1, 1, 1)]
void main()
{
	uint offset = 0u;
	const uint columnCount = SmokeViewColumnCount();
	[loop]
	for (uint column = 0u; column < columnCount; ++column)
	{
		const uint count = gViewColumnOffsets[column];
		gViewColumnOffsets[column] = offset;
		offset += count;
	}
	gViewWorkControl[0].PrefixColumns = columnCount;
	if (offset > gViewConstants.FroxelCapacity)
	{
		gViewWorkControl[0].Overflow = 1u;
		gViewWorkControl[0].CompactCount = 0u;
		gViewIndirectArgs[0] = uint3(0u, 1u, 1u);
		gViewIndirectArgs[1] = uint3(0u, 1u, 1u);
		return;
	}
	gViewWorkControl[0].CompactCount = offset;
	gViewIndirectArgs[0] = uint3((offset + 63u) / 64u, 1u, 1u);
	gViewIndirectArgs[1] = uint3(0u, 1u, 1u);
}
