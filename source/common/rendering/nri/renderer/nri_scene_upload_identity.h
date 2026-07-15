#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

enum class NRISceneBufferUploadDomain : uint32_t
{
	StaticOverlay = 0,
	RuntimeSpaceLink,
	RuntimeMutation,
	Dynamic,
	LocalPlayerReflection,
	RuntimeDebugSphere,
	SurfaceLightOverlay,
	PersistentVoxelMaterial,
	Count,
};

enum class NRISceneUploadBufferKind : uint32_t
{
	Vertex = 0,
	Index,
	Primitive,
	Provenance,
	Material,
	Count,
};

struct NRISceneBufferUploadProducerStamp
{
	uint64_t vertexPayloadStamp = 0;
	uint64_t indexPayloadStamp = 0;
	uint64_t primitivePayloadStamp = 0;
	uint64_t primitiveProvenanceStamp = 0;
	uint64_t materialPayloadStamp = 0;
};

struct NRISceneBufferUploadDomainSpan
{
	NRISceneBufferUploadDomain domain = NRISceneBufferUploadDomain::StaticOverlay;
	uint32_t vertexOffset = 0;
	uint32_t vertexCount = 0;
	uint32_t indexOffset = 0;
	uint32_t indexCount = 0;
	uint32_t primitiveOffset = 0;
	uint32_t primitiveCount = 0;
	uint32_t materialOffset = 0;
	uint32_t materialCount = 0;
	NRISceneBufferUploadProducerStamp stamp = {};
};

struct NRISceneUploadPayloadView
{
	const void* data = nullptr;
	uint64_t byteSize = 0;
	uint32_t stride = 0;
	uint64_t extraIdentity = 0;
};

struct SceneUploadDirtyRange
{
	uint64_t byteOffset = 0;
	uint64_t size = 0;
};

struct NRISceneUploadIdentityBuildStats
{
	uint64_t stampedBytes = 0;
	uint64_t fallbackBytes = 0;
	uint32_t stampedSpans = 0;
	uint32_t fallbackSpans = 0;
	uint32_t quarantinedSpans = 0;
	uint32_t coverageRejects = 0;
	std::array<uint32_t, (size_t)NRISceneBufferUploadDomain::Count> domainChecks = {};
	std::array<uint32_t, (size_t)NRISceneBufferUploadDomain::Count> domainFallbacks = {};
};

struct NRISceneUploadIdentityBuildResult
{
	uint64_t identity = 0;
	bool completeCoverage = false;
	bool usedOnlyProducerStamps = false;
	NRISceneUploadIdentityBuildStats stats = {};
};

class NRISceneUploadProducerGenerations
{
public:
	NRISceneBufferUploadProducerStamp Publish(
		NRISceneBufferUploadDomain domain,
		uint64_t contentKey,
		uint64_t layoutKey,
		bool conservativelyContentChanged,
		bool conservativelyLayoutChanged);
	void Reset();

private:
	struct DomainState
	{
		uint64_t contentKey = 0;
		uint64_t layoutKey = 0;
		uint64_t contentGeneration = 0;
		uint64_t layoutGeneration = 0;
		bool valid = false;
	};

	std::array<DomainState, (size_t)NRISceneBufferUploadDomain::Count> mDomains = {};
};

struct NRISceneUploadIdentityValidationStats
{
	uint32_t checks = 0;
	uint32_t mismatches = 0;
	uint32_t skippedUnstamped = 0;
};

class NRISceneUploadIdentityValidator
{
public:
	bool Validate(
		const std::vector<NRISceneBufferUploadDomainSpan>& spans,
		NRISceneUploadBufferKind kind,
		const NRISceneUploadPayloadView& payload,
		NRISceneUploadIdentityValidationStats& outStats);
	bool IsQuarantined(
		NRISceneBufferUploadDomain domain,
		NRISceneUploadBufferKind kind,
		uint64_t stamp,
		uint64_t extraIdentity,
		uint64_t byteOffset,
		uint64_t byteSize) const;
	void Reset();

private:
	struct Record
	{
		uint64_t claimedIdentity = 0;
		uint64_t exactContentHash = 0;
		bool valid = false;
		bool quarantined = false;
	};

	std::array<Record,
		(size_t)NRISceneBufferUploadDomain::Count * (size_t)NRISceneUploadBufferKind::Count> mRecords = {};
};

uint64_t NRISceneUploadHashBytes(const void* data, uint64_t size);
uint64_t NRISceneUploadCombineIdentity(uint64_t hash, uint64_t value);
uint64_t NRISceneUploadGetSpanStamp(const NRISceneBufferUploadDomainSpan& span, NRISceneUploadBufferKind kind);
void NRISceneUploadGetSpanElementRange(
	const NRISceneBufferUploadDomainSpan& span,
	NRISceneUploadBufferKind kind,
	uint32_t& outOffset,
	uint32_t& outCount);
NRISceneUploadIdentityBuildResult BuildNRISceneUploadPayloadIdentity(
	const std::vector<NRISceneBufferUploadDomainSpan>* spans,
	NRISceneUploadBufferKind kind,
	const NRISceneUploadPayloadView& payload,
	const NRISceneUploadIdentityValidator* validator = nullptr);
