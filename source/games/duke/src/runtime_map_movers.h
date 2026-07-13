#pragma once

#include "runtime_map_mover.h"

BEGIN_DUKE_NS

void ResetRuntimeMapMoverAuthority();
void UpdateRuntimeMapMoverAuthority();
void CaptureRuntimeMapMoverAuthority(TArray<RuntimeMapMoverSnapshot>& out);

END_DUKE_NS
