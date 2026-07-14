#pragma once

#include <cstdint>

#include "tarray.h"
#include "vectors.h"

// Game-owned map mutation authority. Renderer storage and draw-list identity
// are deliberately absent from this contract.
enum class RuntimeMapMoverCapability : uint8_t
{
	Unknown,
	RigidTranslation,
	RigidTransform,
	StableTopologyDeformer,
	MaterialOrLightOnly,
};

// Explicit game-owned lifecycle. Terminal authorities outlive the controller
// actor that issued their final map state and remain authoritative until the
// game advances the map epoch.
enum class RuntimeMapMoverLifecycle : uint8_t
{
	Active,
	Terminal,
};

enum RuntimeMapMoverMemberFlags : uint32_t
{
	RuntimeMapMoverMember_None = 0,
	RuntimeMapMoverMember_OwnsWalls = 1u << 0,
	RuntimeMapMoverMember_OwnsFloor = 1u << 1,
	RuntimeMapMoverMember_OwnsCeiling = 1u << 2,
	RuntimeMapMoverMember_SharedVertexPropagation = 1u << 3,
	RuntimeMapMoverMember_ControlOnly = 1u << 4,
	// Direct sector membership does not yet enumerate dragpoint() propagation
	// and adjacent two-sided wall-band plane dependencies. Optimized routing is
	// quarantined until a shadow owner proves and clears this boundary.
	RuntimeMapMoverMember_AdjacencyUnproven = 1u << 5,
};

struct RuntimeMapMoverPose
{
	DVector3 translation = {};
	DAngle rotation = {};
};

enum class RuntimeMapMoverDeformerKind : uint8_t
{
	None,
	SectorFloorPlane,
};

struct RuntimeMapMoverDeformerState
{
	double floorZ = 0.0;
	int16_t floorHeinum = 0;
};

struct RuntimeMapMoverDeformerPayload
{
	RuntimeMapMoverDeformerKind kind = RuntimeMapMoverDeformerKind::None;
	int32_t sectorIndex = -1;
	RuntimeMapMoverDeformerState simulationPrevious = {};
	RuntimeMapMoverDeformerState simulationCurrent = {};
	RuntimeMapMoverDeformerState presentationPrevious = {};
	RuntimeMapMoverDeformerState presentationCurrent = {};
};

struct RuntimeMapMoverMember
{
	int32_t sectorIndex = -1;
	int32_t canonicalWallOffset = -1;
	int32_t wallCount = 0;
	uint32_t flags = RuntimeMapMoverMember_None;
};

struct RuntimeMapMoverSnapshot
{
	uint64_t stableGroupId = 0;
	uint64_t mapEpoch = 0;
	RuntimeMapMoverCapability capability = RuntimeMapMoverCapability::Unknown;
	RuntimeMapMoverLifecycle lifecycle = RuntimeMapMoverLifecycle::Active;
	int32_t ownerActorIndex = -1; // Diagnostics only, never persistent identity.
	int32_t ownerSectorIndex = -1;
	int32_t effectorLotag = 0;
	int32_t effectorHitag = 0;

	uint64_t topologyGeneration = 0;
	uint64_t geometryGeneration = 0;
	uint64_t materialGeneration = 0;
	uint64_t transformGeneration = 0;
	uint64_t visibilityGeneration = 0;
	uint64_t lightGeneration = 0;
	uint64_t topologySignature = 0;
	uint64_t geometrySignature = 0;
	uint64_t materialSignature = 0;
	uint64_t visibilitySignature = 0;
	uint64_t lightSignature = 0;

	RuntimeMapMoverPose simulationPreviousPose = {};
	RuntimeMapMoverPose simulationCurrentPose = {};
	RuntimeMapMoverPose presentationPreviousPose = {};
	RuntimeMapMoverPose presentationCurrentPose = {};
	RuntimeMapMoverDeformerPayload deformer = {};
	TArray<RuntimeMapMoverMember> members;
};

// Cheap authority stamp queried every render frame. The full snapshot array is
// copied only when revision changes at the game tick boundary. mapEpoch remains
// valid even for maps with no movers, so an empty array is unambiguous.
struct RuntimeMapMoverAuthorityState
{
	bool available = false;
	uint64_t mapEpoch = 0;
	uint64_t revision = 0;
};
