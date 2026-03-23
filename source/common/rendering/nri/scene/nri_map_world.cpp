#include "nri_map_world.h"

namespace nri_scene
{
void PTMapWorld::Reset()
{
	level = nullptr;
	buildSerial = 0;
	valid = false;
	chunks.clear();
	surfaces.clear();
	stats = {};
}
}
