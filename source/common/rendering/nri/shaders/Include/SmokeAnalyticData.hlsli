#ifndef NRI_SMOKE_ANALYTIC_DATA_HLSLI
#define NRI_SMOKE_ANALYTIC_DATA_HLSLI

#define NRI_SMOKE_ANALYTIC_MAX_CARRIERS 128u
#define NRI_SMOKE_ANALYTIC_TILE_SIZE 4u
// The per-tile list can retain every carrier in the fixed pool. Projection
// removes off-screen/non-overlapping work without making overflow destructive.
#define NRI_SMOKE_ANALYTIC_MAX_CARRIERS_PER_TILE NRI_SMOKE_ANALYTIC_MAX_CARRIERS
#define NRI_SMOKE_ANALYTIC_CARRIER_ACTIVE 0x1u
#define NRI_SMOKE_ANALYTIC_CARRIER_SLOT_SHIFT 1u
#define NRI_SMOKE_ANALYTIC_CARRIER_SLOT_MASK 0xfeu
#define NRI_SMOKE_ANALYTIC_CARRIER_GENERATION_SHIFT 8u

uint SmokeAnalyticCarrierSlot(uint flags)
{
	return (flags & NRI_SMOKE_ANALYTIC_CARRIER_SLOT_MASK) >>
		NRI_SMOKE_ANALYTIC_CARRIER_SLOT_SHIFT;
}

uint SmokeAnalyticCarrierGeneration(uint flags)
{
	return flags >> NRI_SMOKE_ANALYTIC_CARRIER_GENERATION_SHIFT;
}

// Keep this 64-byte layout synchronized with NRISmokeAnalyticCarrierGpu.
// CPU policy publishes already-aged position, radius, and density scale; this
// view pass never advances or simulates a carrier.
struct SmokeAnalyticCarrier
{
	float3 Position;
	float Radius;
	float3 HalfAxisU;
	uint Shape;
	float3 HalfAxisV;
	uint StyleIndex;
	float DensityScale;
	uint RangeCount;
	uint Epoch;
	uint Flags;
};

struct SmokeAnalyticTileHeader
{
	uint Count;
	uint Overflow;
};

// One persistent record per physical carrier slot. Data0-2 retain the common
// emissive reservoir payload; Data3 provides carrier-domain temporal identity.
struct SmokeAnalyticEmissiveStorageRecord
{
	uint4 Data0;
	uint4 Data1;
	uint4 Data2;
	uint4 Data3;
};

uint2 SmokeAnalyticTileCount(uint froxelWidth, uint froxelHeight)
{
	return uint2(
		(froxelWidth + NRI_SMOKE_ANALYTIC_TILE_SIZE - 1u) / NRI_SMOKE_ANALYTIC_TILE_SIZE,
		(froxelHeight + NRI_SMOKE_ANALYTIC_TILE_SIZE - 1u) / NRI_SMOKE_ANALYTIC_TILE_SIZE);
}

uint SmokeAnalyticTileIndex(uint2 tile, uint2 tileCount)
{
	return tile.y * tileCount.x + tile.x;
}

#endif
