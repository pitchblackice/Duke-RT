#pragma once

#include "nri_resources.h"

#include <cstdint>

struct NRIRendererFrameContext
{
	uint32_t frameIndex = 0;
	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	uint32_t targetWidth = 0;
	uint32_t targetHeight = 0;
	int drawMode = 0;
	int debugMode = 0;
	bool portal = false;
	bool preserveHistory = false;
};
