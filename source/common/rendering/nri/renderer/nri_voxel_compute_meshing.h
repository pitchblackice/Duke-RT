#pragma once

#include "tarray.h"

#include <cstdint>

class FVoxelModel;
class NRIRenderer;
struct NRIBufferResource;
struct FVoxelRawMeshStats;
struct FVoxelRawSlabRecord;
struct FVoxelRawFaceRecord;
struct FVoxelRawColorRunRecord;
struct FVoxelMeshData;
namespace nri_scene { struct GeometryData; }

enum class NRIVoxelComputeGeneratedGeometryStatus : uint8_t
{
	Unavailable,
	Queued,
	Ready,
	Failed,
};

enum class NRIVoxelComputeDirectPublishOutputKind : uint8_t
{
	None,
	SharedPersistentArena,
	PrivateBlasInputsAndSharedArena,
	PrivateBuffers,
};

enum class NRIVoxelComputeDirectPublishFailure : uint8_t
{
	None,
	Disabled,
	InvalidRequest,
	UnsupportedOutputKind,
	QueueFull,
	PrimitiveBudget,
	AllocationUnavailable,
	DispatchFailed,
	StatusFailed,
	StaleGeneration,
	Cancelled,
};

struct NRIVoxelComputeDirectPublishRange
{
	uint32_t offset = 0;
	uint32_t count = 0;
	uint32_t capacity = 0;
};

struct NRIVoxelComputeDirectPublishBounds
{
	float min[3] = {};
	float max[3] = {};
	bool valid = false;
};

struct NRIVoxelComputeDirectPublishOutputBuffers
{
	const NRIBufferResource* vertices = nullptr;
	const NRIBufferResource* indices = nullptr;
	const NRIBufferResource* primitives = nullptr;
};

struct NRIVoxelComputeDirectPublishRequest
{
	uint64_t meshResourceKey = 0;
	uint64_t generation = 0;
	FVoxelModel* model = nullptr;
	NRIVoxelComputeDirectPublishOutputKind outputKind = NRIVoxelComputeDirectPublishOutputKind::PrivateBlasInputsAndSharedArena;
	NRIVoxelComputeDirectPublishOutputBuffers outputBuffers;
	NRIVoxelComputeDirectPublishRange vertices;
	NRIVoxelComputeDirectPublishRange indices;
	NRIVoxelComputeDirectPublishRange primitives;
	uint32_t materialBase = 0;
	uint32_t materialCount = 0;
	bool validationReadbackAllowed = false;
};

struct NRIVoxelComputeDirectPublishedMesh
{
	uint64_t meshResourceKey = 0;
	uint64_t generation = 0;
	uint64_t sourceArchiveSerial = 0;
	uint32_t jobId = 0;
	uint32_t readyFrame = 0;
	NRIVoxelComputeGeneratedGeometryStatus status = NRIVoxelComputeGeneratedGeometryStatus::Unavailable;
	NRIVoxelComputeDirectPublishFailure failure = NRIVoxelComputeDirectPublishFailure::None;
	NRIVoxelComputeDirectPublishOutputKind outputKind = NRIVoxelComputeDirectPublishOutputKind::None;
	NRIVoxelComputeDirectPublishRange vertices;
	NRIVoxelComputeDirectPublishRange indices;
	NRIVoxelComputeDirectPublishRange primitives;
	uint32_t materialBase = 0;
	uint32_t materialCount = 0;
	NRIVoxelComputeDirectPublishBounds bounds;
};

struct NRIVoxelComputeRawSourcePreloadStats
{
	uint32_t requested = 0;
	uint32_t recorded = 0;
	uint32_t alreadyResident = 0;
	uint32_t skipped = 0;
	uint32_t failed = 0;
	uint32_t slabRecords = 0;
	uint32_t colorRunRecords = 0;
	uint64_t rawBytes = 0;
	uint64_t uploadBytes = 0;
	double buildMs = 0.0;
};

bool ShouldTraceNRIVoxelComputeMeshing();
bool ShouldRunNRIVoxelComputeMeshing();
bool ShouldEmitNRIVoxelComputeMeshing();
bool ShouldConsumeNRIVoxelComputeMeshing();
bool ShouldDirectPublishNRIVoxelComputeMeshing();
void QueueNRIVoxelComputeCountJob(
	FVoxelModel* model,
	const FVoxelRawMeshStats& stats,
	const TArray<FVoxelRawSlabRecord>* slabs,
	const TArray<FVoxelRawFaceRecord>* faces,
	const TArray<FVoxelRawColorRunRecord>* colorRuns,
	const FVoxelMeshData& cpuMesh);
NRIVoxelComputeGeneratedGeometryStatus RequestNRIVoxelComputeGeneratedGeometry(uint64_t requestKey, FVoxelModel* model);
bool TakeNRIVoxelComputeGeneratedGeometry(uint64_t requestKey, nri_scene::GeometryData& outGeometry, uint32_t* outJobId = nullptr);
NRIVoxelComputeGeneratedGeometryStatus RequestNRIVoxelComputeDirectPublication(const NRIVoxelComputeDirectPublishRequest& request);
bool TakeNRIVoxelComputeDirectPublication(uint64_t meshResourceKey, uint64_t generation, NRIVoxelComputeDirectPublishedMesh& outMesh);
void CancelNRIVoxelComputeDirectPublication(uint64_t meshResourceKey, uint64_t generation);
bool QueryNRIVoxelComputeRawSourceArchiveStats(FVoxelModel* model, FVoxelRawMeshStats& outStats);
bool QueryNRIVoxelComputeRawSourceStats(FVoxelModel* model, FVoxelRawMeshStats& outStats);
bool PreloadNRIVoxelComputeRawSource(FVoxelModel* model, NRIVoxelComputeRawSourcePreloadStats* outStats = nullptr);
void DispatchNRIVoxelComputeMeshingDiagnostics(NRIRenderer& renderer, uint64_t frameNumber);
void DestroyNRIVoxelComputeMeshingDiagnostics(NRIRenderer& renderer);
