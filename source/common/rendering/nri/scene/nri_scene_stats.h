#pragma once

#include "nri_scene_bridge.h"

namespace nri_scene
{
void AccumulateSceneDebugStats(SceneDebugStats& target, const SceneDebugStats& source);
SceneDebugStats MergeSceneDebugStats(const SceneDebugStats& a, const SceneDebugStats& b);
bool SceneDebugStatsDiffer(const SceneDebugStats& a, const SceneDebugStats& b);
}
