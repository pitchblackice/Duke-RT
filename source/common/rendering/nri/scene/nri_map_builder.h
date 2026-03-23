#pragma once

#include "nri_map_world.h"

#include <cstdint>

namespace nri_scene
{
void NotifyLevelGeometryReady();
uint64_t GetPendingLevelGeometryBuildSerial();
uint64_t GetCurrentLevelGeometrySignature();
bool BuildMapWorld(PTMapWorld& outWorld);
}
