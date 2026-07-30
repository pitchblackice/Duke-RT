#pragma once

#include "nri_resources.h"

#include <array>
#include <cstdint>
#include <vector>

class FVoxelModel;
struct NRIVoxelComputeRawSourceArchiveSnapshot;
namespace nri_scene
{
	struct MaterialBridgeData;
	struct PrimitiveData;
}

enum class NRIVoxelShadowProxyRejectReason : uint8_t
{
	None = 0,
	Disabled,
	MissingSource,
	ArchiveUnavailable,
	ArchiveMismatch,
	NonLocalGeometry,
	InvalidRawRange,
	TemporaryMemoryLimit,
	EmptyGeometry,
	NoPrimitiveSavings,
	BoundsMismatch,
	PrimitiveSemantics,
	MaterialClosure,
	MaterialFlags,
	MaterialAlpha,
	NoShadowMaterial,
	EmissiveMaterial,
	ActorOverlay,
	InvalidTransform,
	ResourceUnavailable,
	TransitionLimited,
};

enum class NRIVoxelShadowProxyResourceState : uint8_t
{
	Missing = 0,
	CpuReady,
	Resident,
	Failed,
};

struct NRIVoxelShadowProxyBuildLimits
{
	uint64_t maxTemporaryMaskCells = 8ull * 1024ull * 1024ull;
};

struct NRIVoxelShadowProxyVertex
{
	float position[3] = {};
	float prevPosition[3] = {};
	float uv[2] = {};
};
static_assert(sizeof(NRIVoxelShadowProxyVertex) == 32, "Shadow proxy vertices must match the BLAS position layout.");

struct NRIVoxelShadowProxyCpuGeometry
{
	std::vector<NRIVoxelShadowProxyVertex> vertices;
	std::vector<uint32_t> indices;
	uint64_t temporaryMaskCells = 0;
	uint32_t exactPrimitiveCount = 0;
	uint32_t proxyPrimitiveCount = 0;
	float boundsMin[3] = {};
	float boundsMax[3] = {};
	bool boundsValid = false;
};

struct NRIVoxelShadowProxyMaterialFacts
{
	bool flagsSupported = true;
	bool alphaOpaque = true;
	bool lightingNeutral = true;
	bool emissiveFree = true;
	bool actorOverlayFree = true;
};

struct NRIVoxelShadowProxyPrimitiveFacts
{
	bool flagsSupported = true;
	bool portalFree = true;
	bool materialInRange = true;
};

struct NRIVoxelShadowProxyResource
{
	FVoxelModel* sourceModel = nullptr;
	uint64_t sourceArchiveSerial = 0;
	uint64_t sourceContentHash = 0;
	uint64_t geometrySignature = 0;
	uint32_t exactPrimitiveCount = 0;
	uint32_t proxyPrimitiveCount = 0;
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
	uint32_t readyFrame = 0;
	uint32_t firstRequestFrame = UINT32_MAX;
	uint32_t failedFrame = UINT32_MAX;
	uint64_t residentBytes = 0;
	NRIVoxelShadowProxyResourceState state = NRIVoxelShadowProxyResourceState::Missing;
	NRIVoxelShadowProxyRejectReason rejectReason = NRIVoxelShadowProxyRejectReason::None;
	NRIBufferResource vertexBuffer;
	NRIBufferResource indexBuffer;
	NRIAccelerationStructureResource accelerationStructure;
};

struct NRIVoxelShadowProxyBuildStats
{
	uint32_t candidates = 0;
	uint32_t archiveHits = 0;
	uint32_t archiveMisses = 0;
	uint32_t cpuBuilds = 0;
	uint32_t uploads = 0;
	uint32_t blasBuilds = 0;
	uint32_t failures = 0;
	uint32_t materialRejects = 0;
	uint32_t geometryRejects = 0;
	uint32_t resourceRejects = 0;
	uint64_t exactPrimitives = 0;
	uint64_t proxyPrimitives = 0;
	uint64_t uploadBytes = 0;
	uint64_t temporaryMaskCells = 0;
	double cpuBuildMs = 0.0;
};

bool BuildNRIVoxelShadowProxyGeometry(
	const NRIVoxelComputeRawSourceArchiveSnapshot& source,
	const NRIVoxelShadowProxyBuildLimits& limits,
	NRIVoxelShadowProxyCpuGeometry& outGeometry,
	NRIVoxelShadowProxyRejectReason& outReason);

bool CertifyNRIVoxelShadowProxyPrimitiveSemantics(
	const std::vector<nri_scene::PrimitiveData>& primitives,
	uint32_t materialCount);
bool CertifyNRIVoxelShadowProxyPrimitiveFacts(const NRIVoxelShadowProxyPrimitiveFacts& facts);

bool CertifyNRIVoxelShadowProxyMaterialClosure(
	const nri_scene::MaterialBridgeData& materials,
	bool hasActorOverlayLights,
	NRIVoxelShadowProxyRejectReason& outReason);
bool CertifyNRIVoxelShadowProxyMaterialFacts(
	const NRIVoxelShadowProxyMaterialFacts& facts,
	NRIVoxelShadowProxyRejectReason& outReason);
bool CertifyNRIVoxelShadowProxyMaterialClosureFacts(
	const std::vector<NRIVoxelShadowProxyMaterialFacts>& materials,
	bool hasActorOverlayLights,
	NRIVoxelShadowProxyRejectReason& outReason);

bool IsNRIVoxelShadowProxyTransformValid(const std::array<float, 12>& transform);
bool IsNRIVoxelShadowProxyBoundsEquivalent(
	const float exactMin[3],
	const float exactMax[3],
	const float proxyMin[3],
	const float proxyMax[3]);
const char* GetNRIVoxelShadowProxyRejectReasonName(NRIVoxelShadowProxyRejectReason reason);
