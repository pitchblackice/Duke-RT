#pragma once

#include "runtime_map_mover.h"

BEGIN_DUKE_NS

void ResetRuntimeMapMoverAuthority();
void UpdateRuntimeMapMoverAuthority();
void CaptureRuntimeMapMoverAuthority(TArray<RuntimeMapMoverSnapshot>& out, double presentationFraction);

END_DUKE_NS
