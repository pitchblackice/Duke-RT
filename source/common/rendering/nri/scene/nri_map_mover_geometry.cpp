#include "nri_map_mover_geometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>

namespace nri_scene
{
namespace
{
	static constexpr uint64_t HashOffset = 1469598103934665603ull;
	static constexpr uint64_t HashPrime = 1099511628211ull;

	struct NormalizedTriangle
	{
		uint64_t cornerIds[3] = {};
	};

	struct NormalizedVertex
	{
		uint64_t stableCornerId = InvalidMapMoverStableId;
		WorldMapMoverPosition position;
		double uv[2] = {};
	};

	struct NormalizedSurface
	{
		MapMoverSurfaceProvenance provenance;
		uint64_t materialSlotKey = 0;
		uint64_t materialStateKey = 0;
		std::vector<NormalizedVertex> vertices;
		std::vector<NormalizedTriangle> triangles;
	};

	struct NormalizedGeometry
	{
		uint64_t sourceOrderSignature = 0;
		std::vector<NormalizedSurface> surfaces;
	};

	uint64_t HashBytes(uint64_t hash, const void* data, size_t size)
	{
		const auto* bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < size; ++i)
		{
			hash ^= bytes[i];
			hash *= HashPrime;
		}
		return hash;
	}

	template <typename T>
	uint64_t HashValue(uint64_t hash, const T& value)
	{
		return HashBytes(hash, &value, sizeof(value));
	}

	double CanonicalZero(double value)
	{
		return value == 0.0 ? 0.0 : value;
	}

	uint64_t HashDouble(uint64_t hash, double value)
	{
		value = CanonicalZero(value);
		uint64_t bits = 0;
		static_assert(sizeof(bits) == sizeof(value), "double hash size mismatch");
		std::memcpy(&bits, &value, sizeof(bits));
		return HashValue(hash, bits);
	}

	uint64_t HashProvenance(uint64_t hash, const MapMoverSurfaceProvenance& provenance)
	{
		hash = HashValue(hash, provenance.sourceType);
		hash = HashValue(hash, provenance.sectorIndex);
		hash = HashValue(hash, provenance.wallIndex);
		hash = HashValue(hash, provenance.sectionIndex);
		hash = HashValue(hash, provenance.surfaceKind);
		return HashValue(hash, provenance.stableSubSurfaceId);
	}

	bool IsFinite(double value)
	{
		return std::isfinite(value);
	}

	double Dot3(const double a[3], const double b[3])
	{
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	}

	double Determinant3x3(const double basis[3][3])
	{
		return
			basis[0][0] * (basis[1][1] * basis[2][2] - basis[1][2] * basis[2][1]) -
			basis[0][1] * (basis[1][0] * basis[2][2] - basis[1][2] * basis[2][0]) +
			basis[0][2] * (basis[1][0] * basis[2][1] - basis[1][1] * basis[2][0]);
	}

	bool ValidateTransform(
		const MapMoverLocalToWorldTransform& transform,
		const MapMoverGeometryTolerances& tolerances,
		MapMoverGeometryValidation& validation)
	{
		for (int row = 0; row < 3; ++row)
		{
			if (!IsFinite(transform.translation[row]))
			{
				validation.failure = MapMoverGeometryFailure::NonFiniteValue;
				return false;
			}
			for (int column = 0; column < 3; ++column)
			{
				if (!IsFinite(transform.basis[row][column]))
				{
					validation.failure = MapMoverGeometryFailure::NonFiniteValue;
					return false;
				}
			}
		}

		const double determinant = Determinant3x3(transform.basis);
		if (determinant < 0.0)
		{
			validation.failure = MapMoverGeometryFailure::MirroredBasis;
			return false;
		}
		if (std::fabs(determinant - 1.0) > tolerances.basisOrthonormal)
		{
			validation.failure = MapMoverGeometryFailure::InvalidBasis;
			return false;
		}
		for (int row = 0; row < 3; ++row)
		{
			if (std::fabs(Dot3(transform.basis[row], transform.basis[row]) - 1.0) > tolerances.basisOrthonormal)
			{
				validation.failure = MapMoverGeometryFailure::InvalidBasis;
				return false;
			}
			for (int other = row + 1; other < 3; ++other)
			{
				if (std::fabs(Dot3(transform.basis[row], transform.basis[other])) > tolerances.basisOrthonormal)
				{
					validation.failure = MapMoverGeometryFailure::InvalidBasis;
					return false;
				}
			}
		}
		return true;
	}

	bool LessTriangle(const NormalizedTriangle& a, const NormalizedTriangle& b)
	{
		return std::tie(a.cornerIds[0], a.cornerIds[1], a.cornerIds[2]) <
			std::tie(b.cornerIds[0], b.cornerIds[1], b.cornerIds[2]);
	}

	bool EqualTriangle(const NormalizedTriangle& a, const NormalizedTriangle& b)
	{
		return a.cornerIds[0] == b.cornerIds[0] &&
			a.cornerIds[1] == b.cornerIds[1] &&
			a.cornerIds[2] == b.cornerIds[2];
	}

	NormalizedTriangle RotateTriangleToCanonicalStart(uint64_t a, uint64_t b, uint64_t c)
	{
		const std::array<std::array<uint64_t, 3>, 3> rotations =
		{{
			{{ a, b, c }},
			{{ b, c, a }},
			{{ c, a, b }},
		}};
		const auto best = std::min_element(rotations.begin(), rotations.end());
		NormalizedTriangle result = {};
		std::copy(best->begin(), best->end(), result.cornerIds);
		return result;
	}

	NormalizedTriangle ReverseTriangleWinding(const NormalizedTriangle& triangle)
	{
		return RotateTriangleToCanonicalStart(
			triangle.cornerIds[0],
			triangle.cornerIds[2],
			triangle.cornerIds[1]);
	}

	double TriangleAreaSquared(
		const WorldMapMoverPosition& p0,
		const WorldMapMoverPosition& p1,
		const WorldMapMoverPosition& p2)
	{
		const double a[3] =
		{
			p1.value[0] - p0.value[0],
			p1.value[1] - p0.value[1],
			p1.value[2] - p0.value[2],
		};
		const double b[3] =
		{
			p2.value[0] - p0.value[0],
			p2.value[1] - p0.value[1],
			p2.value[2] - p0.value[2],
		};
		const double cross[3] =
		{
			a[1] * b[2] - a[2] * b[1],
			a[2] * b[0] - a[0] * b[2],
			a[0] * b[1] - a[1] * b[0],
		};
		return Dot3(cross, cross) * 0.25;
	}

	bool NormalizeGeometry(
		const WorldMapMoverGeometry& input,
		NormalizedGeometry& output,
		MapMoverGeometryValidation& validation,
		const MapMoverGeometryTolerances& tolerances)
	{
		output = {};
		validation = {};
		if (input.surfaces.empty())
		{
			validation.failure = MapMoverGeometryFailure::EmptyGeometry;
			return false;
		}

		uint64_t sourceOrderHash = HashOffset;
		output.surfaces.reserve(input.surfaces.size());
		for (uint32_t surfaceIndex = 0; surfaceIndex < input.surfaces.size(); ++surfaceIndex)
		{
			const WorldMapMoverSurface& source = input.surfaces[surfaceIndex];
			validation.surfaceIndex = surfaceIndex;
			sourceOrderHash = HashProvenance(sourceOrderHash, source.provenance);
			if (source.vertices.empty() || source.triangles.empty())
			{
				validation.failure = MapMoverGeometryFailure::EmptyGeometry;
				return false;
			}

			NormalizedSurface normalized = {};
			normalized.provenance = source.provenance;
			normalized.materialSlotKey = source.materialSlotKey;
			normalized.materialStateKey = source.materialStateKey;
			normalized.vertices.reserve(source.vertices.size());
			for (uint32_t vertexIndex = 0; vertexIndex < source.vertices.size(); ++vertexIndex)
			{
				const WorldMapMoverVertex& vertex = source.vertices[vertexIndex];
				validation.vertexIndex = vertexIndex;
				if (vertex.stableCornerId == InvalidMapMoverStableId)
				{
					validation.failure = MapMoverGeometryFailure::MissingCornerIdentity;
					return false;
				}
				for (double component : vertex.position.value)
				{
					if (!IsFinite(component))
					{
						validation.failure = MapMoverGeometryFailure::NonFiniteValue;
						return false;
					}
				}
				if (!IsFinite(vertex.uv[0]) || !IsFinite(vertex.uv[1]))
				{
					validation.failure = MapMoverGeometryFailure::NonFiniteValue;
					return false;
				}
				sourceOrderHash = HashValue(sourceOrderHash, vertex.stableCornerId);
				NormalizedVertex copy = {};
				copy.stableCornerId = vertex.stableCornerId;
				copy.position = vertex.position;
				copy.uv[0] = vertex.uv[0];
				copy.uv[1] = vertex.uv[1];
				normalized.vertices.push_back(copy);
			}

			std::sort(normalized.vertices.begin(), normalized.vertices.end(), [](const NormalizedVertex& a, const NormalizedVertex& b)
			{
				return a.stableCornerId < b.stableCornerId;
			});
			for (size_t i = 1; i < normalized.vertices.size(); ++i)
			{
				if (normalized.vertices[i - 1].stableCornerId == normalized.vertices[i].stableCornerId)
				{
					validation.vertexIndex = (uint32_t)i;
					validation.failure = MapMoverGeometryFailure::DuplicateCornerIdentity;
					return false;
				}
			}

			normalized.triangles.reserve(source.triangles.size());
			for (uint32_t triangleIndex = 0; triangleIndex < source.triangles.size(); ++triangleIndex)
			{
				validation.triangleIndex = triangleIndex;
				const WorldMapMoverTriangle& triangle = source.triangles[triangleIndex];
				uint64_t cornerIds[3] = {};
				for (int corner = 0; corner < 3; ++corner)
				{
					if (triangle.cornerIndices[corner] >= source.vertices.size())
					{
						validation.failure = MapMoverGeometryFailure::InvalidTriangle;
						return false;
					}
					cornerIds[corner] = source.vertices[triangle.cornerIndices[corner]].stableCornerId;
					sourceOrderHash = HashValue(sourceOrderHash, cornerIds[corner]);
				}
				if (cornerIds[0] == cornerIds[1] || cornerIds[1] == cornerIds[2] || cornerIds[2] == cornerIds[0])
				{
					validation.failure = MapMoverGeometryFailure::InvalidTriangle;
					return false;
				}
				if (TriangleAreaSquared(
						source.vertices[triangle.cornerIndices[0]].position,
						source.vertices[triangle.cornerIndices[1]].position,
						source.vertices[triangle.cornerIndices[2]].position) <= tolerances.degenerateTriangleArea)
				{
					validation.failure = MapMoverGeometryFailure::DegenerateTriangle;
					return false;
				}
				normalized.triangles.push_back(RotateTriangleToCanonicalStart(cornerIds[0], cornerIds[1], cornerIds[2]));
			}
			std::sort(normalized.triangles.begin(), normalized.triangles.end(), LessTriangle);
			for (size_t i = 1; i < normalized.triangles.size(); ++i)
			{
				if (EqualTriangle(normalized.triangles[i - 1], normalized.triangles[i]))
				{
					validation.triangleIndex = (uint32_t)i;
					validation.failure = MapMoverGeometryFailure::DuplicateTriangle;
					return false;
				}
			}
			output.surfaces.push_back(std::move(normalized));
		}

		std::sort(output.surfaces.begin(), output.surfaces.end(), [](const NormalizedSurface& a, const NormalizedSurface& b)
		{
			return a.provenance < b.provenance;
		});
		for (size_t i = 1; i < output.surfaces.size(); ++i)
		{
			if (output.surfaces[i - 1].provenance == output.surfaces[i].provenance)
			{
				validation.surfaceIndex = (uint32_t)i;
				validation.failure = MapMoverGeometryFailure::DuplicateSurface;
				return false;
			}
		}

		output.sourceOrderSignature = sourceOrderHash;
		validation.valid = true;
		validation.failure = MapMoverGeometryFailure::None;
		validation.surfaceIndex = UINT32_MAX;
		validation.vertexIndex = UINT32_MAX;
		validation.triangleIndex = UINT32_MAX;
		return true;
	}

	CanonicalLocalMapMoverPosition InverseTransformMapMoverPosition(
		const MapMoverLocalToWorldTransform& transform,
		const WorldMapMoverPosition& worldPosition)
	{
		const double translated[3] =
		{
			worldPosition.value[0] - transform.translation[0],
			worldPosition.value[1] - transform.translation[1],
			worldPosition.value[2] - transform.translation[2],
		};
		CanonicalLocalMapMoverPosition result = {};
		for (int column = 0; column < 3; ++column)
		{
			result.value[column] = CanonicalZero(
				transform.basis[0][column] * translated[0] +
				transform.basis[1][column] * translated[1] +
				transform.basis[2][column] * translated[2]);
		}
		return result;
	}

	uint64_t BuildTopologyKey(const CanonicalLocalMapMoverGeometry& geometry)
	{
		uint64_t hash = HashOffset;
		hash = HashValue(hash, (uint64_t)geometry.surfaces.size());
		for (const CanonicalLocalMapMoverSurface& surface : geometry.surfaces)
		{
			hash = HashProvenance(hash, surface.provenance);
			hash = HashValue(hash, (uint64_t)surface.vertices.size());
			for (const CanonicalLocalMapMoverVertex& vertex : surface.vertices)
			{
				hash = HashValue(hash, vertex.stableCornerId);
			}
			hash = HashValue(hash, (uint64_t)surface.triangles.size());
			for (const CanonicalLocalMapMoverTriangle& triangle : surface.triangles)
			{
				for (uint64_t id : triangle.stableCornerIds)
				{
					hash = HashValue(hash, id);
				}
			}
		}
		return hash;
	}

	uint64_t BuildMaterialLayoutKey(const CanonicalLocalMapMoverGeometry& geometry)
	{
		uint64_t hash = HashOffset;
		for (const CanonicalLocalMapMoverSurface& surface : geometry.surfaces)
		{
			hash = HashProvenance(hash, surface.provenance);
			hash = HashValue(hash, surface.materialSlotKey);
		}
		return hash;
	}

	uint64_t BuildResourceKey(const CanonicalLocalMapMoverGeometry& geometry)
	{
		uint64_t hash = HashOffset;
		hash = HashValue(hash, geometry.topologyKey);
		hash = HashValue(hash, geometry.materialLayoutKey);
		for (const CanonicalLocalMapMoverSurface& surface : geometry.surfaces)
		{
			for (const CanonicalLocalMapMoverVertex& vertex : surface.vertices)
			{
				for (double component : vertex.position.value)
				{
					hash = HashDouble(hash, component);
				}
				hash = HashDouble(hash, vertex.uv[0]);
				hash = HashDouble(hash, vertex.uv[1]);
			}
		}
		return hash;
	}

	uint64_t BuildMaterialStateKey(const CanonicalLocalMapMoverGeometry& geometry)
	{
		uint64_t hash = HashOffset;
		for (const CanonicalLocalMapMoverSurface& surface : geometry.surfaces)
		{
			hash = HashProvenance(hash, surface.provenance);
			hash = HashValue(hash, surface.materialSlotKey);
			hash = HashValue(hash, surface.materialStateKey);
		}
		return hash;
	}

	bool BasisIsIdentity(const MapMoverLocalToWorldTransform& transform, double tolerance)
	{
		for (int row = 0; row < 3; ++row)
		{
			for (int column = 0; column < 3; ++column)
			{
				const double expected = row == column ? 1.0 : 0.0;
				if (std::fabs(transform.basis[row][column] - expected) > tolerance)
				{
					return false;
				}
			}
		}
		return true;
	}

	bool SameCanonicalTriangle(
		const CanonicalLocalMapMoverTriangle& canonical,
		const NormalizedTriangle& normalized)
	{
		return canonical.stableCornerIds[0] == normalized.cornerIds[0] &&
			canonical.stableCornerIds[1] == normalized.cornerIds[1] &&
			canonical.stableCornerIds[2] == normalized.cornerIds[2];
	}

	bool HasMirroredWinding(
		const CanonicalLocalMapMoverSurface& canonical,
		const NormalizedSurface& current)
	{
		for (const NormalizedTriangle& currentTriangle : current.triangles)
		{
			const NormalizedTriangle reversed = ReverseTriangleWinding(currentTriangle);
			for (const CanonicalLocalMapMoverTriangle& canonicalTriangle : canonical.triangles)
			{
				if (SameCanonicalTriangle(canonicalTriangle, reversed))
				{
					return true;
				}
			}
		}
		return false;
	}
}

bool operator==(const MapMoverSurfaceProvenance& a, const MapMoverSurfaceProvenance& b)
{
	return a.sourceType == b.sourceType &&
		a.sectorIndex == b.sectorIndex &&
		a.wallIndex == b.wallIndex &&
		a.sectionIndex == b.sectionIndex &&
		a.surfaceKind == b.surfaceKind &&
		a.stableSubSurfaceId == b.stableSubSurfaceId;
}

bool operator<(const MapMoverSurfaceProvenance& a, const MapMoverSurfaceProvenance& b)
{
	return std::tie(a.sourceType, a.sectorIndex, a.wallIndex, a.sectionIndex, a.surfaceKind, a.stableSubSurfaceId) <
		std::tie(b.sourceType, b.sectorIndex, b.wallIndex, b.sectionIndex, b.surfaceKind, b.stableSubSurfaceId);
}

MapMoverLocalToWorldTransform MakeIdentityMapMoverLocalToWorldTransform()
{
	return {};
}

WorldMapMoverPosition TransformMapMoverPosition(
	const MapMoverLocalToWorldTransform& transform,
	const CanonicalLocalMapMoverPosition& localPosition)
{
	WorldMapMoverPosition result = {};
	for (int row = 0; row < 3; ++row)
	{
		result.value[row] = CanonicalZero(
			transform.basis[row][0] * localPosition.value[0] +
			transform.basis[row][1] * localPosition.value[1] +
			transform.basis[row][2] * localPosition.value[2] +
			transform.translation[row]);
	}
	return result;
}

bool BuildCanonicalLocalMapMoverGeometry(
	const WorldMapMoverGeometry& worldGeometry,
	const MapMoverLocalToWorldTransform& localToWorld,
	CanonicalLocalMapMoverGeometry& outGeometry,
	MapMoverGeometryValidation& outValidation,
	const MapMoverGeometryTolerances& tolerances)
{
	outGeometry = {};
	outValidation = {};
	if (!ValidateTransform(localToWorld, tolerances, outValidation))
	{
		return false;
	}

	NormalizedGeometry normalized = {};
	if (!NormalizeGeometry(worldGeometry, normalized, outValidation, tolerances))
	{
		return false;
	}

	outGeometry.surfaces.reserve(normalized.surfaces.size());
	for (const NormalizedSurface& source : normalized.surfaces)
	{
		CanonicalLocalMapMoverSurface destination = {};
		destination.provenance = source.provenance;
		destination.materialSlotKey = source.materialSlotKey;
		destination.materialStateKey = source.materialStateKey;
		destination.vertices.reserve(source.vertices.size());
		for (const NormalizedVertex& sourceVertex : source.vertices)
		{
			CanonicalLocalMapMoverVertex destinationVertex = {};
			destinationVertex.stableCornerId = sourceVertex.stableCornerId;
			destinationVertex.position = InverseTransformMapMoverPosition(localToWorld, sourceVertex.position);
			destinationVertex.uv[0] = CanonicalZero(sourceVertex.uv[0]);
			destinationVertex.uv[1] = CanonicalZero(sourceVertex.uv[1]);
			destination.vertices.push_back(destinationVertex);
		}
		destination.triangles.reserve(source.triangles.size());
		for (const NormalizedTriangle& sourceTriangle : source.triangles)
		{
			CanonicalLocalMapMoverTriangle destinationTriangle = {};
			for (int corner = 0; corner < 3; ++corner)
			{
				destinationTriangle.stableCornerIds[corner] = sourceTriangle.cornerIds[corner];
				const auto vertexIt = std::lower_bound(
					destination.vertices.begin(),
					destination.vertices.end(),
					sourceTriangle.cornerIds[corner],
					[](const CanonicalLocalMapMoverVertex& vertex, uint64_t id)
					{
						return vertex.stableCornerId < id;
					});
				destinationTriangle.cornerIndices[corner] = (uint32_t)std::distance(destination.vertices.begin(), vertexIt);
			}
			destination.triangles.push_back(destinationTriangle);
		}
		outGeometry.surfaces.push_back(std::move(destination));
	}

	outGeometry.sourceOrderSignature = normalized.sourceOrderSignature;
	outGeometry.topologyKey = BuildTopologyKey(outGeometry);
	outGeometry.materialLayoutKey = BuildMaterialLayoutKey(outGeometry);
	outGeometry.materialStateKey = BuildMaterialStateKey(outGeometry);
	outGeometry.resourceKey = BuildResourceKey(outGeometry);
	outGeometry.valid = true;
	outValidation.valid = true;
	outValidation.failure = MapMoverGeometryFailure::None;
	return true;
}

MapMoverGeometryComparison ClassifyMapMoverGeometryChange(
	const CanonicalLocalMapMoverGeometry& canonicalGeometry,
	const WorldMapMoverGeometry& currentWorldGeometry,
	const MapMoverLocalToWorldTransform& proposedLocalToWorld,
	const MapMoverGeometryTolerances& tolerances)
{
	MapMoverGeometryComparison result = {};
	if (!canonicalGeometry.valid || canonicalGeometry.surfaces.empty())
	{
		result.validation.failure = MapMoverGeometryFailure::EmptyGeometry;
		return result;
	}
	if (!ValidateTransform(proposedLocalToWorld, tolerances, result.validation))
	{
		return result;
	}

	NormalizedGeometry current = {};
	if (!NormalizeGeometry(currentWorldGeometry, current, result.validation, tolerances))
	{
		return result;
	}
	result.generatedOrderChanged = canonicalGeometry.sourceOrderSignature != current.sourceOrderSignature;

	size_t canonicalIndex = 0;
	size_t currentIndex = 0;
	double residualTotal = 0.0;
	while (canonicalIndex < canonicalGeometry.surfaces.size() || currentIndex < current.surfaces.size())
	{
		if (canonicalIndex >= canonicalGeometry.surfaces.size())
		{
			result.membershipChanged = true;
			currentIndex++;
			continue;
		}
		if (currentIndex >= current.surfaces.size())
		{
			result.membershipChanged = true;
			canonicalIndex++;
			continue;
		}

		const CanonicalLocalMapMoverSurface& canonicalSurface = canonicalGeometry.surfaces[canonicalIndex];
		const NormalizedSurface& currentSurface = current.surfaces[currentIndex];
		if (canonicalSurface.provenance < currentSurface.provenance)
		{
			result.membershipChanged = true;
			canonicalIndex++;
			continue;
		}
		if (currentSurface.provenance < canonicalSurface.provenance)
		{
			result.membershipChanged = true;
			currentIndex++;
			continue;
		}

		if (canonicalSurface.materialSlotKey != currentSurface.materialSlotKey)
		{
			result.materialSlotChanged = true;
		}
		if (canonicalSurface.materialStateKey != currentSurface.materialStateKey)
		{
			result.materialStateChanged = true;
		}
		bool surfaceTopologyMatches = canonicalSurface.vertices.size() == currentSurface.vertices.size() &&
			canonicalSurface.triangles.size() == currentSurface.triangles.size();
		if (surfaceTopologyMatches)
		{
			for (size_t vertexIndex = 0; vertexIndex < canonicalSurface.vertices.size(); ++vertexIndex)
			{
				if (canonicalSurface.vertices[vertexIndex].stableCornerId != currentSurface.vertices[vertexIndex].stableCornerId)
				{
					surfaceTopologyMatches = false;
					break;
				}
			}
		}
		if (surfaceTopologyMatches)
		{
			for (size_t triangleIndex = 0; triangleIndex < canonicalSurface.triangles.size(); ++triangleIndex)
			{
				if (!SameCanonicalTriangle(canonicalSurface.triangles[triangleIndex], currentSurface.triangles[triangleIndex]))
				{
					surfaceTopologyMatches = false;
					break;
				}
			}
		}
		if (!surfaceTopologyMatches)
		{
			if (HasMirroredWinding(canonicalSurface, currentSurface))
			{
				result.validation.failure = MapMoverGeometryFailure::MirroredWinding;
				return result;
			}
			result.topologyChanged = true;
			canonicalIndex++;
			currentIndex++;
			continue;
		}

		for (size_t vertexIndex = 0; vertexIndex < canonicalSurface.vertices.size(); ++vertexIndex)
		{
			const CanonicalLocalMapMoverVertex& canonicalVertex = canonicalSurface.vertices[vertexIndex];
			const NormalizedVertex& currentVertex = currentSurface.vertices[vertexIndex];
			const WorldMapMoverPosition expected = TransformMapMoverPosition(proposedLocalToWorld, canonicalVertex.position);
			double residualSquared = 0.0;
			for (int component = 0; component < 3; ++component)
			{
				const double delta = expected.value[component] - currentVertex.position.value[component];
				residualSquared += delta * delta;
			}
			const double residual = std::sqrt(residualSquared);
			residualTotal += residual;
			result.rigidFitMaxResidual = std::max(result.rigidFitMaxResidual, residual);
			result.rigidFitVertexCount++;
			if (std::fabs(canonicalVertex.uv[0] - currentVertex.uv[0]) > tolerances.vertexAttribute ||
				std::fabs(canonicalVertex.uv[1] - currentVertex.uv[1]) > tolerances.vertexAttribute)
			{
				result.vertexAttributeChanged = true;
			}
		}

		canonicalIndex++;
		currentIndex++;
	}

	if (result.rigidFitVertexCount > 0)
	{
		result.rigidFitMeanResidual = residualTotal / (double)result.rigidFitVertexCount;
		result.rigidFitWithinTolerance = result.rigidFitMaxResidual <= tolerances.rigidFit;
	}
	const bool deformed = !result.membershipChanged &&
		!result.topologyChanged &&
		(!result.rigidFitWithinTolerance || result.vertexAttributeChanged);

	const bool materialChanged = result.materialSlotChanged || result.materialStateChanged;
	if (result.membershipChanged)
	{
		result.classification = (result.topologyChanged || materialChanged) ?
			MapMoverGeometryClassification::Mixed :
			MapMoverGeometryClassification::MembershipChange;
	}
	else if (result.topologyChanged)
	{
		result.classification = materialChanged ?
			MapMoverGeometryClassification::Mixed :
			MapMoverGeometryClassification::TopologyChange;
	}
	else if (deformed)
	{
		result.classification = materialChanged ?
			MapMoverGeometryClassification::Mixed :
			MapMoverGeometryClassification::StableLayoutDeformer;
	}
	else if (result.materialSlotChanged)
	{
		result.classification = MapMoverGeometryClassification::MaterialSlotChange;
	}
	else if (result.materialStateChanged)
	{
		result.classification = MapMoverGeometryClassification::MaterialStateChange;
	}
	else
	{
		result.classification = BasisIsIdentity(proposedLocalToWorld, tolerances.transformBasisIdentity) ?
			MapMoverGeometryClassification::RigidTranslation :
			MapMoverGeometryClassification::RigidTransform;
	}

	result.validation.valid = true;
	result.validation.failure = MapMoverGeometryFailure::None;
	result.validation.surfaceIndex = UINT32_MAX;
	result.validation.vertexIndex = UINT32_MAX;
	result.validation.triangleIndex = UINT32_MAX;
	return result;
}

const char* GetMapMoverGeometryClassificationName(MapMoverGeometryClassification classification)
{
	switch (classification)
	{
	case MapMoverGeometryClassification::RigidTranslation: return "rigid-translation";
	case MapMoverGeometryClassification::RigidTransform: return "rigid-transform";
	case MapMoverGeometryClassification::StableLayoutDeformer: return "stable-layout-deformer";
	case MapMoverGeometryClassification::TopologyChange: return "topology-change";
	case MapMoverGeometryClassification::MembershipChange: return "membership-change";
	case MapMoverGeometryClassification::MaterialSlotChange: return "material-slot-change";
	case MapMoverGeometryClassification::MaterialStateChange: return "material-state-change";
	case MapMoverGeometryClassification::Mixed: return "mixed";
	default: return "unknown";
	}
}

const char* GetMapMoverGeometryFailureName(MapMoverGeometryFailure failure)
{
	switch (failure)
	{
	case MapMoverGeometryFailure::None: return "none";
	case MapMoverGeometryFailure::EmptyGeometry: return "empty-geometry";
	case MapMoverGeometryFailure::NonFiniteValue: return "non-finite-value";
	case MapMoverGeometryFailure::InvalidBasis: return "invalid-basis";
	case MapMoverGeometryFailure::MirroredBasis: return "mirrored-basis";
	case MapMoverGeometryFailure::DuplicateSurface: return "duplicate-surface";
	case MapMoverGeometryFailure::MissingCornerIdentity: return "missing-corner-identity";
	case MapMoverGeometryFailure::DuplicateCornerIdentity: return "duplicate-corner-identity";
	case MapMoverGeometryFailure::InvalidTriangle: return "invalid-triangle";
	case MapMoverGeometryFailure::DegenerateTriangle: return "degenerate-triangle";
	case MapMoverGeometryFailure::DuplicateTriangle: return "duplicate-triangle";
	case MapMoverGeometryFailure::MirroredWinding: return "mirrored-winding";
	default: return "unknown";
	}
}
}
