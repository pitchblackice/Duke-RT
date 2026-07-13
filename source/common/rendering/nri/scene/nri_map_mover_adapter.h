#pragma once

#include "nri_map_mover_geometry.h"
#include "nri_scene_surface_types.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace nri_scene
{
struct PTMapChunk;
struct PTMapWorld;

enum class PTMapMoverAdapterFailure : uint8_t
{
	None = 0,
	EmptyChunk,
	InvalidChunkRange,
	InvalidSurfaceReference,
	InvalidSurfaceIdentity,
	DuplicateSurfaceIdentity,
	UnsupportedSurfaceKind,
	MissingAuthoredWall,
	MissingAuthoredCorner,
	AmbiguousAuthoredCorner,
	InvalidVertexLayout,
	InvalidPrimitiveLayout,
	NonFiniteValue,
	MissingMaterialIdentity,
};

struct PTMapMoverAdapterValidation
{
	bool valid = false;
	PTMapMoverAdapterFailure failure = PTMapMoverAdapterFailure::None;
	uint32_t surfaceIndex = UINT32_MAX;
	uint32_t vertexIndex = UINT32_MAX;
	uint32_t primitiveIndex = UINT32_MAX;
	// Horizontal Euclidean distance to the nearest valid authored corner in
	// the expected wall/section. Infinity means no candidate was available.
	double nearestAuthoredCornerDistance = std::numeric_limits<double>::infinity();
};

// Current authored endpoints for a wall. point ids are map wall-point ids,
// while wallIndex identifies the wall surface whose start/end roles they own.
struct PTMapMoverAuthoredWall
{
	int32_t wallIndex = -1;
	int32_t startPointId = -1;
	int32_t endPointId = -1;
	double startPosition[2] = {};
	double endPosition[2] = {};
};

// A section boundary point used to identify plane triangle corners. Repeated
// records are permitted for shared/adjacent section lines; identity is derived
// from the sorted unique authored wall-point ids at the matched position.
struct PTMapMoverAuthoredPlanePoint
{
	int32_t sectionIndex = -1;
	int32_t wallPointId = -1;
	double position[2] = {};
};

struct PTMapMoverAuthoredTopology
{
	std::vector<PTMapMoverAuthoredWall> walls;
	std::vector<PTMapMoverAuthoredPlanePoint> planePoints;
};

using PTMapMoverTextureIdentityResolver = bool (*)(
	const FGameTexture* texture,
	uint64_t& outStableIdentity,
	void* userData);

struct PTMapMoverAdapterOptions
{
	// CapturedVertex positions are floats while authored map positions are
	// doubles. A coordinate matches when its error is no greater than this
	// absolute floor plus authoredPositionFloatUlpScale times one float ULP at
	// the larger coordinate magnitude. This admits representational rounding
	// without applying a broad map-wide epsilon.
	double authoredPositionTolerance = 1.0e-5;
	double authoredPositionFloatUlpScale = 2.0;
	double duplicateAttributeTolerance = 1.0e-6;
	PTMapMoverTextureIdentityResolver resolveTextureIdentity = nullptr;
	void* textureIdentityUserData = nullptr;
};

double ComputePTMapMoverAuthoredPositionTolerance(
	double capturedPosition,
	double authoredPosition,
	const PTMapMoverAdapterOptions& options = {});

// Lightweight view used by direct tests and by the PTMapWorld wrapper. It
// deliberately carries semantic keys rather than source-container indices.
struct PTMapMoverSurfaceView
{
	const SurfaceRef* surface = nullptr;
	uint32_t surfaceKind = UINT32_MAX;
	uint32_t keyPrimary = UINT32_MAX;
	uint32_t keySecondary = UINT32_MAX;
	uint32_t chunkIndex = UINT32_MAX;
};

struct PTMapMoverChunkView
{
	uint32_t chunkIndex = UINT32_MAX;
	int32_t sectorIndex = -1;
	std::vector<PTMapMoverSurfaceView> surfaces;
};

bool BuildWorldMapMoverGeometryFromPTMapChunkView(
	const PTMapMoverChunkView& chunkView,
	const PTMapMoverAuthoredTopology& authoredTopology,
	WorldMapMoverGeometry& outGeometry,
	PTMapMoverAdapterValidation& outValidation,
	const PTMapMoverAdapterOptions& options = {});

bool BuildWorldMapMoverGeometryFromPTMapChunk(
	const PTMapWorld& mapWorld,
	const PTMapChunk& chunk,
	const PTMapMoverAuthoredTopology& authoredTopology,
	WorldMapMoverGeometry& outGeometry,
	PTMapMoverAdapterValidation& outValidation,
	const PTMapMoverAdapterOptions& options = {});

const char* GetPTMapMoverAdapterFailureName(PTMapMoverAdapterFailure failure);
}
