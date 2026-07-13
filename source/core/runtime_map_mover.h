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

enum RuntimeMapMoverMemberFlags : uint32_t
{
	RuntimeMapMoverMember_None = 0,
	RuntimeMapMoverMember_OwnsWalls = 1u << 0,
	RuntimeMapMoverMember_OwnsFloor = 1u << 1,
	RuntimeMapMoverMember_OwnsCeiling = 1u << 2,
	RuntimeMapMoverMember_SharedVertexPropagation = 1u << 3,
	RuntimeMapMoverMember_ControlOnly = 1u << 4,
};

struct RuntimeMapMoverPose
{
	DVector3 translation = {};
	DAngle rotation = {};
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
	TArray<RuntimeMapMoverMember> members;
};
