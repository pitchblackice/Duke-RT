#pragma once

#include "nri_map_world.h"

#include <cstdint>

namespace nri_scene
{
void NotifyLevelGeometryReady();
uint64_t GetPendingLevelGeometryBuildSerial();
bool BuildMapWorld(PTMapWorld& outWorld);
bool BuildLiveMapChunkSceneView(const PTMapChunk& chunk, SceneView& outView, PTMapWorldStats* outStats = nullptr);
uint64_t ComputeMapChunkGeometrySignature(const PTMapChunk& chunk);
}
