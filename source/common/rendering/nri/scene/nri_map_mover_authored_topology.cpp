#include "nri_map_mover_authored_topology.h"

#include <algorithm>
#include <cmath>

namespace
{
	using namespace nri_scene;

	constexpr uint32_t MapSurfaceFloor = 0;
	constexpr uint32_t MapSurfaceCeiling = 1;
	constexpr uint32_t MapSurfaceFirstWall = 2;
	constexpr uint32_t MapSurfaceLastWall = 6;

	void SetFailure(
		PTMapMoverAuthoredTopologyValidation& validation,
		PTMapMoverAuthoredTopologyFailure failure,
		int32_t sourceIndex = -1,
		uint32_t surfaceIndex = UINT32_MAX)
	{
		validation = {};
		validation.failure = failure;
		validation.sourceIndex = sourceIndex;
		validation.surfaceIndex = surfaceIndex;
	}

	template<class T, class Identity>
	bool HasDuplicateIdentity(const std::vector<T>& values, Identity identity, int32_t& outIdentity)
	{
		std::vector<int32_t> identities;
		identities.reserve(values.size());
		for (const T& value : values)
		{
			const int32_t current = identity(value);
			if (current < 0)
			{
				outIdentity = current;
				return true;
			}
			identities.push_back(current);
		}
		std::sort(identities.begin(), identities.end());
		const auto duplicate = std::adjacent_find(identities.begin(), identities.end());
		if (duplicate == identities.end())
		{
			return false;
		}
		outIdentity = *duplicate;
		return true;
	}

	template<class T, class Identity>
	const T* FindByIdentity(const std::vector<T>& values, int32_t target, Identity identity)
	{
		const auto found = std::find_if(values.begin(), values.end(),
			[target, identity](const T& value) { return identity(value) == target; });
		return found == values.end() ? nullptr : &*found;
	}

	bool IsFinitePosition(const PTMapMoverSourceWall& wall)
	{
		return std::isfinite(wall.position[0]) && std::isfinite(wall.position[1]);
	}
}

namespace nri_scene
{
bool BuildPTMapMoverAuthoredTopology(
	const PTMapMoverChunkView& chunkView,
	const std::vector<PTMapMoverSourceWall>& walls,
	const std::vector<PTMapMoverSourceSection>& sections,
	const std::vector<PTMapMoverSourceSectionLine>& sectionLines,
	PTMapMoverAuthoredTopology& outTopology,
	PTMapMoverAuthoredTopologyValidation& outValidation)
{
	outTopology = {};
	outValidation = {};

	int32_t duplicateOrInvalid = -1;
	if (HasDuplicateIdentity(walls, [](const auto& value) { return value.wallIndex; }, duplicateOrInvalid))
	{
		SetFailure(outValidation, duplicateOrInvalid < 0
			? PTMapMoverAuthoredTopologyFailure::InvalidSourceIndex
			: PTMapMoverAuthoredTopologyFailure::DuplicateWallIndex, duplicateOrInvalid);
		return false;
	}
	if (HasDuplicateIdentity(sections, [](const auto& value) { return value.sectionIndex; }, duplicateOrInvalid))
	{
		SetFailure(outValidation, duplicateOrInvalid < 0
			? PTMapMoverAuthoredTopologyFailure::InvalidSourceIndex
			: PTMapMoverAuthoredTopologyFailure::DuplicateSectionIndex, duplicateOrInvalid);
		return false;
	}
	if (HasDuplicateIdentity(sectionLines, [](const auto& value) { return value.lineIndex; }, duplicateOrInvalid))
	{
		SetFailure(outValidation, duplicateOrInvalid < 0
			? PTMapMoverAuthoredTopologyFailure::InvalidSourceIndex
			: PTMapMoverAuthoredTopologyFailure::DuplicateSectionLineIndex, duplicateOrInvalid);
		return false;
	}

	std::vector<int32_t> referencedWalls;
	std::vector<int32_t> referencedSections;
	for (uint32_t surfaceIndex = 0; surfaceIndex < chunkView.surfaces.size(); ++surfaceIndex)
	{
		const PTMapMoverSurfaceView& surface = chunkView.surfaces[surfaceIndex];
		if (surface.surface == nullptr || surface.chunkIndex != chunkView.chunkIndex)
		{
			SetFailure(outValidation, PTMapMoverAuthoredTopologyFailure::InvalidSurfaceReference, -1, surfaceIndex);
			return false;
		}
		if (surface.surfaceKind == MapSurfaceFloor || surface.surfaceKind == MapSurfaceCeiling)
		{
			if (surface.keyPrimary > INT32_MAX)
			{
				SetFailure(outValidation, PTMapMoverAuthoredTopologyFailure::InvalidSourceIndex, -1, surfaceIndex);
				return false;
			}
			referencedSections.push_back((int32_t)surface.keyPrimary);
		}
		else if (surface.surfaceKind >= MapSurfaceFirstWall && surface.surfaceKind <= MapSurfaceLastWall)
		{
			if (surface.keyPrimary > INT32_MAX)
			{
				SetFailure(outValidation, PTMapMoverAuthoredTopologyFailure::InvalidSourceIndex, -1, surfaceIndex);
				return false;
			}
			referencedWalls.push_back((int32_t)surface.keyPrimary);
		}
		else
		{
			SetFailure(outValidation, PTMapMoverAuthoredTopologyFailure::UnsupportedSurfaceKind, -1, surfaceIndex);
			return false;
		}
	}

	std::sort(referencedWalls.begin(), referencedWalls.end());
	referencedWalls.erase(std::unique(referencedWalls.begin(), referencedWalls.end()), referencedWalls.end());
	std::sort(referencedSections.begin(), referencedSections.end());
	referencedSections.erase(std::unique(referencedSections.begin(), referencedSections.end()), referencedSections.end());

	PTMapMoverAuthoredTopology topology;
	for (int32_t wallIndex : referencedWalls)
	{
		const auto source = FindByIdentity(walls, wallIndex, [](const auto& value) { return value.wallIndex; });
		if (source == nullptr)
		{
			SetFailure(outValidation, PTMapMoverAuthoredTopologyFailure::MissingWall, wallIndex);
			return false;
		}
		if (source->point2Index < 0)
		{
			SetFailure(outValidation, PTMapMoverAuthoredTopologyFailure::InvalidSourceIndex, source->point2Index);
			return false;
		}
		const auto endpoint = FindByIdentity(walls, source->point2Index, [](const auto& value) { return value.wallIndex; });
		if (endpoint == nullptr)
		{
			SetFailure(outValidation, PTMapMoverAuthoredTopologyFailure::MissingWall, source->point2Index);
			return false;
		}
		if (!IsFinitePosition(*source) || !IsFinitePosition(*endpoint))
		{
			SetFailure(outValidation, PTMapMoverAuthoredTopologyFailure::NonFinitePosition,
				!IsFinitePosition(*source) ? source->wallIndex : endpoint->wallIndex);
			return false;
		}

		PTMapMoverAuthoredWall authored = {};
		authored.wallIndex = source->wallIndex;
		authored.startPointId = source->wallIndex;
		authored.endPointId = source->point2Index;
		authored.startPosition[0] = source->position[0];
		authored.startPosition[1] = -source->position[1];
		authored.endPosition[0] = endpoint->position[0];
		authored.endPosition[1] = -endpoint->position[1];
		topology.walls.push_back(authored);
	}

	for (int32_t sectionIndex : referencedSections)
	{
		const auto section = FindByIdentity(sections, sectionIndex, [](const auto& value) { return value.sectionIndex; });
		if (section == nullptr)
		{
			SetFailure(outValidation, PTMapMoverAuthoredTopologyFailure::MissingSection, sectionIndex);
			return false;
		}
		std::vector<int32_t> uniqueLineIndices = section->lineIndices;
		std::sort(uniqueLineIndices.begin(), uniqueLineIndices.end());
		if (std::adjacent_find(uniqueLineIndices.begin(), uniqueLineIndices.end()) != uniqueLineIndices.end())
		{
			const auto duplicate = std::adjacent_find(uniqueLineIndices.begin(), uniqueLineIndices.end());
			SetFailure(outValidation, PTMapMoverAuthoredTopologyFailure::DuplicateSectionLineReference, *duplicate);
			return false;
		}

		for (int32_t lineIndex : section->lineIndices)
		{
			if (lineIndex < 0)
			{
				SetFailure(outValidation, PTMapMoverAuthoredTopologyFailure::InvalidSourceIndex, lineIndex);
				return false;
			}
			const auto line = FindByIdentity(sectionLines, lineIndex, [](const auto& value) { return value.lineIndex; });
			if (line == nullptr)
			{
				SetFailure(outValidation, PTMapMoverAuthoredTopologyFailure::MissingSectionLine, lineIndex);
				return false;
			}
			if (line->sectionIndex != sectionIndex)
			{
				SetFailure(outValidation, PTMapMoverAuthoredTopologyFailure::SectionLineOwnerMismatch, lineIndex);
				return false;
			}
			if (line->startPointId < 0 || line->endPointId < 0)
			{
				SetFailure(outValidation, PTMapMoverAuthoredTopologyFailure::InvalidSourceIndex,
					line->startPointId < 0 ? line->startPointId : line->endPointId);
				return false;
			}

			const int32_t pointIds[2] = { line->startPointId, line->endPointId };
			for (int32_t pointId : pointIds)
			{
				const auto point = FindByIdentity(walls, pointId, [](const auto& value) { return value.wallIndex; });
				if (point == nullptr)
				{
					SetFailure(outValidation, PTMapMoverAuthoredTopologyFailure::MissingWall, pointId);
					return false;
				}
				if (!IsFinitePosition(*point))
				{
					SetFailure(outValidation, PTMapMoverAuthoredTopologyFailure::NonFinitePosition, pointId);
					return false;
				}
				PTMapMoverAuthoredPlanePoint authored = {};
				authored.sectionIndex = sectionIndex;
				authored.wallPointId = pointId;
				authored.position[0] = point->position[0];
				authored.position[1] = -point->position[1];
				topology.planePoints.push_back(authored);
			}
		}
	}

	outTopology = std::move(topology);
	outValidation.valid = true;
	outValidation.failure = PTMapMoverAuthoredTopologyFailure::None;
	return true;
}

bool BuildPTMapMoverSceneTransformFromDukePose(
	double dukeX,
	double dukeY,
	double dukeZ,
	double dukeAngleRadians,
	MapMoverLocalToWorldTransform& outTransform,
	float outRowMajor3x4[12])
{
	outTransform = MakeIdentityMapMoverLocalToWorldTransform();
	if (outRowMajor3x4 == nullptr || !std::isfinite(dukeX) || !std::isfinite(dukeY) ||
		!std::isfinite(dukeZ) || !std::isfinite(dukeAngleRadians))
	{
		return false;
	}

	const double cosine = std::cos(dukeAngleRadians);
	const double sine = std::sin(dukeAngleRadians);
	outTransform.basis[0][0] = cosine;
	outTransform.basis[0][2] = sine;
	outTransform.basis[2][0] = -sine;
	outTransform.basis[2][2] = cosine;
	outTransform.translation[0] = dukeX;
	outTransform.translation[1] = -dukeZ;
	outTransform.translation[2] = -dukeY;

	const double rows[12] =
	{
		cosine, 0.0, sine, dukeX,
		0.0, 1.0, 0.0, -dukeZ,
		-sine, 0.0, cosine, -dukeY,
	};
	for (uint32_t i = 0; i < 12; ++i)
	{
		outRowMajor3x4[i] = (float)rows[i];
	}
	return true;
}

const char* GetPTMapMoverAuthoredTopologyFailureName(PTMapMoverAuthoredTopologyFailure failure)
{
	switch (failure)
	{
	case PTMapMoverAuthoredTopologyFailure::None: return "none";
	case PTMapMoverAuthoredTopologyFailure::InvalidSurfaceReference: return "invalid-surface-reference";
	case PTMapMoverAuthoredTopologyFailure::UnsupportedSurfaceKind: return "unsupported-surface-kind";
	case PTMapMoverAuthoredTopologyFailure::InvalidSourceIndex: return "invalid-source-index";
	case PTMapMoverAuthoredTopologyFailure::DuplicateWallIndex: return "duplicate-wall-index";
	case PTMapMoverAuthoredTopologyFailure::DuplicateSectionIndex: return "duplicate-section-index";
	case PTMapMoverAuthoredTopologyFailure::DuplicateSectionLineIndex: return "duplicate-section-line-index";
	case PTMapMoverAuthoredTopologyFailure::DuplicateSectionLineReference: return "duplicate-section-line-reference";
	case PTMapMoverAuthoredTopologyFailure::MissingWall: return "missing-wall";
	case PTMapMoverAuthoredTopologyFailure::MissingSection: return "missing-section";
	case PTMapMoverAuthoredTopologyFailure::MissingSectionLine: return "missing-section-line";
	case PTMapMoverAuthoredTopologyFailure::SectionLineOwnerMismatch: return "section-line-owner-mismatch";
	case PTMapMoverAuthoredTopologyFailure::NonFinitePosition: return "non-finite-position";
	}
	return "unknown";
}
}
