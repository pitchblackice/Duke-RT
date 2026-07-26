#ifndef NRI_INDIRECT_RADIANCE_CACHE_TRACE_HLSLI
#define NRI_INDIRECT_RADIANCE_CACHE_TRACE_HLSLI

static const uint NRI_INDIRECT_RADIANCE_CACHE_ENTRY_COUNT = 262144u;
static const uint NRI_INDIRECT_RADIANCE_CACHE_ENTRY_MASK = NRI_INDIRECT_RADIANCE_CACHE_ENTRY_COUNT - 1u;

uint IndirectRadianceCacheHash(uint value)
{
	value ^= value >> 16u;
	value *= 0x7feb352du;
	value ^= value >> 15u;
	value *= 0x846ca68bu;
	value ^= value >> 16u;
	return value;
}

bool IsIndirectRadianceCacheIdentityTransform(SceneInstanceData instanceData)
{
	const float epsilon = 1e-6;
	return
		all(abs(instanceData.currentTransformRow0 - float4(1.0, 0.0, 0.0, 0.0)) <= epsilon) &&
		all(abs(instanceData.currentTransformRow1 - float4(0.0, 1.0, 0.0, 0.0)) <= epsilon) &&
		all(abs(instanceData.currentTransformRow2 - float4(0.0, 0.0, 1.0, 0.0)) <= epsilon);
}

bool IsIndirectRadianceCacheEligible(HitData hit, MaterialData material)
{
	const uint unsupportedFlags =
		MATERIAL_FLAG_SPRITE |
		MATERIAL_FLAG_MIRROR |
		MATERIAL_FLAG_SKY |
		MATERIAL_FLAG_PORTAL |
		MATERIAL_FLAG_ONE_WAY |
		MATERIAL_FLAG_ALPHA_CLIP |
		MATERIAL_FLAG_FACING_BILLBOARD |
		MATERIAL_FLAG_PLAIN_MIRROR;
	if (hit.dataSource != SCENE_DATA_SOURCE_STATIC ||
		hit.pathFlags != 0u ||
		(material.flags & unsupportedFlags) != 0u ||
		(material.flags & MATERIAL_FLAG_FULLBRIGHT) != 0u ||
		material.emissiveMode != 0u ||
		hit.instanceId == 0xffffffffu)
	{
		return false;
	}

	return IsIndirectRadianceCacheIdentityTransform(GetSceneInstanceData(hit.instanceId));
}

IndirectRadianceCacheRecord BuildIndirectRadianceCacheRecord(
	HitData hit,
	MaterialData material,
	float3 incidentRadiance)
{
	const SceneInstanceData instanceData = GetSceneInstanceData(hit.instanceId);
	const int3 quantizedPosition = (int3)round(hit.position * 8.0);
	const int3 quantizedNormal = (int3)round(normalize(hit.normal) * 511.0);
	const uint packedNormal =
		((uint)quantizedNormal.x & 0x3ffu) |
		(((uint)quantizedNormal.y & 0x3ffu) << 10u) |
		(((uint)quantizedNormal.z & 0x3ffu) << 20u) |
		((hit.pathFlags & 0x3u) << 30u);

	uint stableLo = IndirectRadianceCacheHash(hit.primitiveIndex ^ instanceData.metadata0);
	stableLo = IndirectRadianceCacheHash(stableLo ^ (uint)quantizedPosition.x);
	stableLo = IndirectRadianceCacheHash(stableLo ^ (uint)quantizedPosition.z);
	uint stableHi = IndirectRadianceCacheHash(hit.materialIndex ^ instanceData.metadata1 ^ 0x9e3779b9u);
	stableHi = IndirectRadianceCacheHash(stableHi ^ (uint)quantizedPosition.y);
	stableHi = IndirectRadianceCacheHash(stableHi ^ packedNormal);

	IndirectRadianceCacheRecord record = (IndirectRadianceCacheRecord)0;
	record.StableKey = uint2(stableLo, stableHi);
	record.Compatibility = uint2(instanceData.metadata0, instanceData.metadata1);
	record.QuantizedWorldPosition = uint2((uint)quantizedPosition.x, (uint)quantizedPosition.y);
	record.PackedNormalAndRoute = packedNormal;
	record.MaterialFlags = material.flags ^ (material.materialClass << 24u) ^ (uint)quantizedPosition.z;
	record.IncidentRadiance = incidentRadiance;
	record.Metadata = gTraceConstants.FrameIndex + 1u;
	return record;
}

bool IndirectRadianceCacheRecordMatches(
	IndirectRadianceCacheRecord candidate,
	IndirectRadianceCacheRecord reference)
{
	return
		all(candidate.StableKey == reference.StableKey) &&
		all(candidate.Compatibility == reference.Compatibility) &&
		all(candidate.QuantizedWorldPosition == reference.QuantizedWorldPosition) &&
		candidate.PackedNormalAndRoute == reference.PackedNormalAndRoute &&
		candidate.MaterialFlags == reference.MaterialFlags;
}

bool TryReadIndirectRadianceCache(
	HitData hit,
	MaterialData material,
	out float3 incidentRadiance)
{
	incidentRadiance = 0.0;
	InterlockedAdd(gIndirectRadianceCacheTelemetry[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_LOOKUPS], 1u);
	if (!IsIndirectRadianceCacheEligible(hit, material))
	{
		InterlockedAdd(gIndirectRadianceCacheTelemetry[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_UNSUPPORTED_ROUTE], 1u);
		return false;
	}

	const IndirectRadianceCacheRecord reference = BuildIndirectRadianceCacheRecord(hit, material, 0.0);
	const uint tableIndex = reference.StableKey.x & NRI_INDIRECT_RADIANCE_CACHE_ENTRY_MASK;
	const IndirectRadianceCacheRecord candidate = gIndirectRadianceCachePrevious[tableIndex];
	const uint expectedMetadata = gTraceConstants.FrameIndex;
	if (candidate.Metadata != 0u && candidate.Metadata != expectedMetadata)
	{
		InterlockedAdd(gIndirectRadianceCacheTelemetry[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_STALE_GENERATION], 1u);
	}
	else if (candidate.Metadata != 0u && candidate.Metadata == expectedMetadata &&
		!IndirectRadianceCacheRecordMatches(candidate, reference))
	{
		InterlockedAdd(gIndirectRadianceCacheTelemetry[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_COLLISIONS], 1u);
	}

	if ((gTraceConstants.Flags & NRI_FLAG_INDIRECT_RADIANCE_CACHE_ACCEPT) == 0u)
	{
		InterlockedAdd(gIndirectRadianceCacheTelemetry[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_FORCED_MISSES], 1u);
		return false;
	}
	if (candidate.Metadata != expectedMetadata || !IndirectRadianceCacheRecordMatches(candidate, reference))
	{
		return false;
	}

	incidentRadiance = candidate.IncidentRadiance;
	InterlockedAdd(gIndirectRadianceCacheTelemetry[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_ACCEPTED_HITS], 1u);
	return true;
}

void WriteIndirectRadianceCache(HitData hit, MaterialData material, float3 incidentRadiance)
{
	if (!IsIndirectRadianceCacheEligible(hit, material))
	{
		return;
	}

	const IndirectRadianceCacheRecord record = BuildIndirectRadianceCacheRecord(hit, material, incidentRadiance);
	const uint tableIndex = record.StableKey.x & NRI_INDIRECT_RADIANCE_CACHE_ENTRY_MASK;
	const uint previousMetadata = gIndirectRadianceCacheCurrent[tableIndex].Metadata;
	gIndirectRadianceCacheCurrent[tableIndex] = record;
	InterlockedAdd(gIndirectRadianceCacheTelemetry[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_UPDATES], 1u);
	if (previousMetadata != record.Metadata)
	{
		InterlockedAdd(gIndirectRadianceCacheTelemetry[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_OCCUPANCY], 1u);
	}
}

#endif
