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
#define NRI_SMOKE_ANALYTIC_LIGHT_GROUP_OWNER 0x100u
#define NRI_SMOKE_ANALYTIC_LIGHT_BUILD_PENDING 0x200u
#define NRI_SMOKE_ANALYTIC_LIGHT_RECORD_VALID 0x80000000u
#define NRI_SMOKE_ANALYTIC_LIGHT_ANCHORS_PER_BANK 2u

uint SmokeAnalyticCarrierSlot(uint flags)
{
	return (flags & NRI_SMOKE_ANALYTIC_CARRIER_SLOT_MASK) >>
		NRI_SMOKE_ANALYTIC_CARRIER_SLOT_SHIFT;
}

uint SmokeAnalyticCarrierGeneration(uint flags)
{
	return flags >> NRI_SMOKE_ANALYTIC_CARRIER_GENERATION_SHIFT;
}

// Keep this 80-byte layout synchronized with NRISmokeAnalyticCarrierGpu.
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
	uint LightGroupSlot;
	uint LightGroupGeneration;
	uint LightAnchorCount;
	uint LightSampleCountAndFlags;
};

struct SmokeAnalyticTileHeader
{
	uint Count;
	uint Overflow;
};

// One persistent 64-byte record per event anchor. Words 0-8 hold six packed
// half-float RGB directional lobes, words 9-11 hold the anchor position, and
// words 12-15 provide exact event identity and build metadata.
struct SmokeAnalyticEmissiveStorageRecord
{
	uint4 Data0;
	uint4 Data1;
	uint4 Data2;
	uint4 Data3;
};

uint SmokeAnalyticLightWord(SmokeAnalyticEmissiveStorageRecord record, uint index)
{
	if (index < 4u) return record.Data0[index];
	if (index < 8u) return record.Data1[index - 4u];
	if (index < 12u) return record.Data2[index - 8u];
	return record.Data3[index - 12u];
}

void SmokeAnalyticLightStoreWord(inout SmokeAnalyticEmissiveStorageRecord record,
	uint index, uint value)
{
	if (index < 4u) record.Data0[index] = value;
	else if (index < 8u) record.Data1[index - 4u] = value;
	else if (index < 12u) record.Data2[index - 8u] = value;
	else record.Data3[index - 12u] = value;
}

void SmokeAnalyticLightStoreHalf(inout SmokeAnalyticEmissiveStorageRecord record,
	uint halfIndex, float value)
{
	const uint wordIndex = halfIndex >> 1u;
	const uint oldWord = SmokeAnalyticLightWord(record, wordIndex);
	const uint packed = f32tof16(clamp(value, 0.0, 65504.0));
	SmokeAnalyticLightStoreWord(record, wordIndex, (halfIndex & 1u) == 0u
		? (oldWord & 0xffff0000u) | packed : (oldWord & 0x0000ffffu) | (packed << 16u));
}

float SmokeAnalyticLightLoadHalf(SmokeAnalyticEmissiveStorageRecord record,
	uint halfIndex)
{
	const uint word = SmokeAnalyticLightWord(record, halfIndex >> 1u);
	return f16tof32((halfIndex & 1u) == 0u ? word & 0xffffu : word >> 16u);
}

void SmokeAnalyticLightStoreLobe(inout SmokeAnalyticEmissiveStorageRecord record,
	uint lobe, float3 value)
{
	const uint base = min(lobe, 5u) * 3u;
	SmokeAnalyticLightStoreHalf(record, base, value.x);
	SmokeAnalyticLightStoreHalf(record, base + 1u, value.y);
	SmokeAnalyticLightStoreHalf(record, base + 2u, value.z);
}

float3 SmokeAnalyticLightLobe(SmokeAnalyticEmissiveStorageRecord record, uint lobe)
{
	const uint base = min(lobe, 5u) * 3u;
	return float3(SmokeAnalyticLightLoadHalf(record, base),
		SmokeAnalyticLightLoadHalf(record, base + 1u),
		SmokeAnalyticLightLoadHalf(record, base + 2u));
}

float3 SmokeAnalyticLightAnchorPosition(SmokeAnalyticEmissiveStorageRecord record)
{
	return asfloat(uint3(record.Data2.y, record.Data2.z, record.Data2.w));
}

bool SmokeAnalyticLightIdentityMatches(SmokeAnalyticEmissiveStorageRecord record,
	uint groupSlot, uint groupGeneration, uint epoch, uint anchorIndex)
{
	return record.Data3.x == groupSlot && record.Data3.y == groupGeneration &&
		record.Data3.z == epoch && (record.Data3.w & NRI_SMOKE_ANALYTIC_LIGHT_RECORD_VALID) != 0u &&
		((record.Data3.w >> 8u) & 3u) == anchorIndex;
}

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
