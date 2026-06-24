#pragma once

#include "nri_frame_resources.h"

#include <NRI.h>

#include <cstdint>
#include <vector>

class NRIRaySceneBuilder
{
public:
	NRIRaySceneBuilder(std::vector<nri::TopLevelInstance>& tlasInstances, std::vector<SceneInstanceData>& sceneRecords)
		: mTlasInstances(tlasInstances), mSceneRecords(sceneRecords)
	{
	}

	uint32_t NextSceneRecordIndex() const
	{
		return (uint32_t)mSceneRecords.size();
	}

	uint32_t AddLegacyInstance(nri::TopLevelInstance instance, const SceneInstanceData& sceneRecord)
	{
		const uint32_t sceneRecordIndex = NextSceneRecordIndex();
		instance.instanceId = sceneRecordIndex;
		mTlasInstances.push_back(instance);
		mSceneRecords.push_back(sceneRecord);
		return sceneRecordIndex;
	}

private:
	std::vector<nri::TopLevelInstance>& mTlasInstances;
	std::vector<SceneInstanceData>& mSceneRecords;
};
