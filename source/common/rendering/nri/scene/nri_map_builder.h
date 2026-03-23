#pragma once

#include "nri_map_world.h"

#include <cstdint>

namespace nri_scene
{
void NotifyLevelGeometryReady();
uint64_t GetPendingLevelGeometryBuildSerial();
bool BuildMapWorld(PTMapWorld& outWorld);
}
