#pragma once

#include "runtime_map_mover.h"

class FSerializer;

BEGIN_DUKE_NS

class DDukeActor;

void ResetRuntimeMapMoverAuthority();
void UpdateRuntimeMapMoverAuthority();
void CaptureRuntimeMapMoverAuthority(TArray<RuntimeMapMoverSnapshot>& out);
void RetireRuntimeMapMoverAuthority(DDukeActor* actor);
void SerializeRuntimeMapMoverAuthority(FSerializer& arc);
void RestoreRuntimeMapMoverActorIdentityAllocator();

END_DUKE_NS
