#pragma once

#include "nri_frame_resources.h"
#include "nri_map_mover_rigid_route_policy.h"

#include <array>
#include <cstdint>
#include <map>
#include <vector>

class NRIMapMoverSystem;
class NRIMapMoverShadowState;
class NRIRuntimeMutationSystem;
struct ResidentMapChunkRegistry;
struct StaticMapChunkAtlas;
struct StaticMapSceneCache;

namespace nri_scene
{
	struct PTMapWorld;
}

struct NRIMapMoverRigidRouteFrameInput
{
	const NRIMapMoverSystem* movers = nullptr;
	const NRIMapMoverShadowState* shadowState = nullptr;
	const nri_scene::PTMapWorld* mapWorld = nullptr;
	const StaticMapSceneCache* staticScene = nullptr;
	const StaticMapChunkAtlas* atlas = nullptr;
	const ResidentMapChunkRegistry* registry = nullptr;
	const NRIRuntimeMutationSystem* runtimeMutation = nullptr;
	uint64_t frameIndex = 0;
	int mode = 0;
	int traceMode = 0;
};

struct NRIMapMoverRigidRouteFrameStats
{
	uint64_t frameIndex = 0;
	uint64_t buildSerial = 0;
	uint64_t mapEpoch = 0;
	uint32_t shadowRecordCount = 0;
	uint32_t candidateCount = 0;
	uint32_t admittedCount = 0;
	uint32_t capturedResourceCount = 0;
	uint32_t residentDriftCount = 0;
	uint32_t bypassChunkCount = 0;
	uint32_t patchedInstanceCount = 0;
	uint32_t patchMissCount = 0;
	std::array<uint32_t, 18> rejectBitCounts = {};
};

class NRIMapMoverRigidRoute
{
public:
	void Update(const NRIMapMoverRigidRouteFrameInput& input);
	bool ShouldBypassExactChunk(uint32_t chunkIndex) const;
	bool PatchStaticInstances(
		std::vector<nri::TopLevelInstance>& tlasInstances,
		std::vector<SceneInstanceData>& sceneInstances);
	void Reset();

	const NRIMapMoverRigidRouteFrameStats& GetFrameStats() const { return m_frameStats; }
	uint32_t GetRetainedResourceCount() const { return (uint32_t)m_resources.size(); }

private:
	struct ResidentFingerprint
	{
		uint32_t staticSceneChunkListIndex = UINT32_MAX;
		uint32_t vertexOffset = 0;
		uint32_t vertexCount = 0;
		uint32_t indexOffset = 0;
		uint32_t indexCount = 0;
		uint32_t primitiveOffset = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialOffset = 0;
		uint32_t materialCount = 0;
		uint64_t baselineSignature = 0;
		uint64_t exactGeometrySignature = 0;
		uint64_t geometryPayloadHash = 0;
		uint64_t materialPayloadHash = 0;
		const nri::AccelerationStructure* chunkBlas = nullptr;

		bool operator==(const ResidentFingerprint& other) const;
	};

	struct RetainedResource
	{
		NRIMapMoverRigidRouteResource policyResource;
		ResidentFingerprint fingerprint;
	};

	struct ActiveRoute
	{
		NRIMapMoverShadowRecordKey key;
		NRIMapMoverRigidRouteTransforms transforms;
	};

	using ResourceMap = std::map<NRIMapMoverShadowRecordKey, RetainedResource>;
	using ActiveRouteMap = std::map<uint32_t, ActiveRoute>;

	ResourceMap m_resources;
	ActiveRouteMap m_activeRoutes;
	NRIMapMoverRigidRouteFrameStats m_frameStats;
	uint64_t m_buildSerial = 0;
	uint64_t m_mapEpoch = 0;
	int m_traceMode = 0;
};
