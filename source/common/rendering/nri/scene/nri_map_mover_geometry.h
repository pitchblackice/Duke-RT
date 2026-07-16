#pragma once

#include <cstdint>
#include <vector>

namespace nri_scene
{
static constexpr uint64_t InvalidMapMoverStableId = UINT64_MAX;

enum class MapMoverGeometryClassification : uint8_t
{
	Unknown = 0,
	RigidTranslation,
	RigidTransform,
	StableLayoutDeformer,
	TopologyChange,
	MembershipChange,
	MaterialSlotChange,
	MaterialStateChange,
	Mixed,
};

enum class MapMoverGeometryFailure : uint8_t
{
	None = 0,
	EmptyGeometry,
	NonFiniteValue,
	InvalidBasis,
	MirroredBasis,
	DuplicateSurface,
	MissingCornerIdentity,
	DuplicateCornerIdentity,
	InvalidTriangle,
	DegenerateTriangle,
	DuplicateTriangle,
	MirroredWinding,
};

// Authored identity supplied by the game/map adapter. No emitted vertex or
// primitive order participates in this identity.
struct MapMoverSurfaceProvenance
{
	uint32_t sourceType = 0;
	int32_t sectorIndex = -1;
	int32_t wallIndex = -1;
	int32_t sectionIndex = -1;
	uint32_t surfaceKind = 0;
	uint32_t stableSubSurfaceId = 0;
};

bool operator==(const MapMoverSurfaceProvenance& a, const MapMoverSurfaceProvenance& b);
bool operator<(const MapMoverSurfaceProvenance& a, const MapMoverSurfaceProvenance& b);

struct WorldMapMoverPosition
{
	double value[3] = {};
};

struct CanonicalLocalMapMoverPosition
{
	double value[3] = {};
};

struct MapMoverLocalToWorldTransform
{
	double basis[3][3] =
	{
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 },
	};
	double translation[3] = {};
};

struct WorldMapMoverVertex
{
	uint64_t stableCornerId = InvalidMapMoverStableId;
	WorldMapMoverPosition position;
	double uv[2] = {};
};

struct WorldMapMoverTriangle
{
	uint32_t cornerIndices[3] = {};
};

struct WorldMapMoverSurface
{
	MapMoverSurfaceProvenance provenance;
	// Identifies the logical material slot attached to this authored surface.
	// Live texture/palette/shade state must not participate in this key.
	uint64_t materialSlotKey = 0;
	uint64_t materialStateKey = 0;
	std::vector<WorldMapMoverVertex> vertices;
	std::vector<WorldMapMoverTriangle> triangles;
};

struct WorldMapMoverGeometry
{
	std::vector<WorldMapMoverSurface> surfaces;
};

struct CanonicalLocalMapMoverVertex
{
	uint64_t stableCornerId = InvalidMapMoverStableId;
	CanonicalLocalMapMoverPosition position;
	double uv[2] = {};
};

struct CanonicalLocalMapMoverTriangle
{
	uint32_t cornerIndices[3] = {};
	uint64_t stableCornerIds[3] = {};
};

struct CanonicalLocalMapMoverSurface
{
	MapMoverSurfaceProvenance provenance;
	uint64_t materialSlotKey = 0;
	uint64_t materialStateKey = 0;
	std::vector<CanonicalLocalMapMoverVertex> vertices;
	std::vector<CanonicalLocalMapMoverTriangle> triangles;
};

struct CanonicalLocalMapMoverGeometry
{
	bool valid = false;
	uint64_t topologyKey = 0;
	uint64_t materialLayoutKey = 0;
	uint64_t materialStateKey = 0;
	uint64_t resourceKey = 0;
	uint64_t sourceOrderSignature = 0;
	std::vector<CanonicalLocalMapMoverSurface> surfaces;
};

struct MapMoverGeometryTolerances
{
	double basisOrthonormal = 1.0e-6;
	double transformBasisIdentity = 1.0e-6;
	double rigidFit = 1.0e-4;
	double vertexAttribute = 1.0e-6;
	double degenerateTriangleArea = 1.0e-12;
};

struct MapMoverGeometryValidation
{
	bool valid = false;
	MapMoverGeometryFailure failure = MapMoverGeometryFailure::None;
	uint32_t surfaceIndex = UINT32_MAX;
	uint32_t vertexIndex = UINT32_MAX;
	uint32_t triangleIndex = UINT32_MAX;
};

struct MapMoverGeometryComparison
{
	MapMoverGeometryClassification classification = MapMoverGeometryClassification::Unknown;
	MapMoverGeometryValidation validation;
	bool generatedOrderChanged = false;
	bool membershipChanged = false;
	bool topologyChanged = false;
	bool materialSlotChanged = false;
	bool materialStateChanged = false;
	bool vertexAttributeChanged = false;
	bool rigidFitWithinTolerance = false;
	uint32_t rigidFitVertexCount = 0;
	double rigidFitMeanResidual = 0.0;
	double rigidFitMaxResidual = 0.0;
};

MapMoverLocalToWorldTransform MakeIdentityMapMoverLocalToWorldTransform();
WorldMapMoverPosition TransformMapMoverPosition(
	const MapMoverLocalToWorldTransform& transform,
	const CanonicalLocalMapMoverPosition& localPosition);

bool BuildCanonicalLocalMapMoverGeometry(
	const WorldMapMoverGeometry& worldGeometry,
	const MapMoverLocalToWorldTransform& localToWorld,
	CanonicalLocalMapMoverGeometry& outGeometry,
	MapMoverGeometryValidation& outValidation,
	const MapMoverGeometryTolerances& tolerances = {});

MapMoverGeometryComparison ClassifyMapMoverGeometryChange(
	const CanonicalLocalMapMoverGeometry& canonicalGeometry,
	const WorldMapMoverGeometry& currentWorldGeometry,
	const MapMoverLocalToWorldTransform& proposedLocalToWorld,
	const MapMoverGeometryTolerances& tolerances = {});

const char* GetMapMoverGeometryClassificationName(MapMoverGeometryClassification classification);
const char* GetMapMoverGeometryFailureName(MapMoverGeometryFailure failure);
}
