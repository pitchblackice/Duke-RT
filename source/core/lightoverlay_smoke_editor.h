#pragma once

#include <cstdint>

#include "d_eventbase.h"
#include "lightoverlay.h"

struct MapSmokeEmitterEditorRuntimePreview
{
	bool active = false;
	bool suppressPersistedRule = false;
	uint32_t revision = 0;
	ParsedLightOverlayMapSmokeEmitterRule rule;
};

bool IsMapSmokeEmitterEditorEnabled();
void TickMapSmokeEmitterEditor();
bool MapSmokeEmitterEditorResponder(event_t* ev);
bool GetMapSmokeEmitterEditorRuntimePreview(MapSmokeEmitterEditorRuntimePreview& outPreview);
