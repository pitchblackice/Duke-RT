#pragma once

#include "nri_map_mover_adapter.h"

#include <cstdint>
#include <vector>

namespace nri_scene
{
// Renderer-agnostic views of the Duke wall/section tables. The engine-facing
// caller copies only identity and position data; this module never reads the
// mutable global wall/section arrays.
struct PTMapMoverSourceWall
{
	int32_t wallIndex = -1;
	int32_t point2Index = -1;
	double position[2] = {};
};

struct PTMapMoverSourceSection
{
	int32_t sectionIndex = -1;
	std::vector<int32_t> lineIndices;
};

struct PTMapMoverSourceSectionLine
{
	int32_t lineIndex = -1;
	int32_t sectionIndex = -1;
	int32_t startPointId = -1;
	int32_t endPointId = -1;
};

enum class PTMapMoverAuthoredTopologyFailure : uint8_t
{
	None = 0,
	InvalidSurfaceReference,
	UnsupportedSurfaceKind,
	InvalidSourceIndex,
	DuplicateWallIndex,
	DuplicateSectionIndex,
	DuplicateSectionLineIndex,
	DuplicateSectionLineReference,
	MissingWall,
	MissingSection,
	MissingSectionLine,
	SectionLineOwnerMismatch,
	NonFinitePosition,
};

struct PTMapMoverAuthoredTopologyValidation
{
	bool valid = false;
	PTMapMoverAuthoredTopologyFailure failure = PTMapMoverAuthoredTopologyFailure::None;
	int32_t sourceIndex = -1;
	uint32_t surfaceIndex = UINT32_MAX;
};

// Filters the explicit authored tables to the walls and plane sections named
// by chunkView. Output is suitable for BuildWorldMapMoverGeometryFromPTMapChunkView.
bool BuildPTMapMoverAuthoredTopology(
	const PTMapMoverChunkView& chunkView,
	const std::vector<PTMapMoverSourceWall>& walls,
	const std::vector<PTMapMoverSourceSection>& sections,
	const std::vector<PTMapMoverSourceSectionLine>& sectionLines,
	PTMapMoverAuthoredTopology& outTopology,
	PTMapMoverAuthoredTopologyValidation& outValidation);

// Converts Duke engine coordinates (X, Y, Z and counter-clockwise XY angle)
// to the NRI scene convention (X, -Z, -Y). rowMajor3x4 is laid out as three
// rows of [basis.xyz, translation], matching NRI scene instance transforms.
bool BuildPTMapMoverSceneTransformFromDukePose(
	double dukeX,
	double dukeY,
	double dukeZ,
	double dukeAngleRadians,
	MapMoverLocalToWorldTransform& outTransform,
	float outRowMajor3x4[12]);

const char* GetPTMapMoverAuthoredTopologyFailureName(PTMapMoverAuthoredTopologyFailure failure);
}
