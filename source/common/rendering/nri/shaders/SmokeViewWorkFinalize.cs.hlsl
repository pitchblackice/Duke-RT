#include "Include/SmokeViewWorkResources.hlsli"

[numthreads(1, 1, 1)]
void main()
{
	// The first argument is reserved for exact compact materialization; the
	// second is reserved for the dense-reference comparator. Until integration
	// supplies compaction, publishing zero prevents accidental indirect use.
	gViewIndirectArgs[0] = uint3(0u, 1u, 1u);
	gViewIndirectArgs[1] = uint3(0u, 1u, 1u);
}
