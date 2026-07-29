#include "nri_scene_texture_utils.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#define RAZE_PTRPROBE_TRY __try
#define RAZE_PTRPROBE_EXCEPT __except (EXCEPTION_EXECUTE_HANDLER)
#else
// GCC/Clang have no SEH for hardware faults and no portable VirtualQuery; the
// cheap pointer sanity checks remain, the guarded access runs unprotected.
#define RAZE_PTRPROBE_TRY if (true)
#define RAZE_PTRPROBE_EXCEPT else
#endif

namespace nri_scene
{
bool IsUsableGameTexturePointer(FGameTexture* texture)
{
	const uintptr_t value = (uintptr_t)texture;
	if (value <= 0x10000 ||
		value == (uintptr_t)-1 ||
		(value & (sizeof(void*) - 1)) != 0)
	{
		return false;
	}

#ifdef _WIN32
	MEMORY_BASIC_INFORMATION pointerInfo = {};
	if (VirtualQuery(texture, &pointerInfo, sizeof(pointerInfo)) != sizeof(pointerInfo) ||
		pointerInfo.State != MEM_COMMIT ||
		(pointerInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
	{
		return false;
	}
#endif

	void* vtable = nullptr;
	RAZE_PTRPROBE_TRY
	{
		vtable = *(void**)texture;
	}
	RAZE_PTRPROBE_EXCEPT
	{
		vtable = nullptr;
	}

	if (vtable == nullptr)
	{
		return false;
	}

#ifdef _WIN32
	MEMORY_BASIC_INFORMATION vtableInfo = {};
	return VirtualQuery(vtable, &vtableInfo, sizeof(vtableInfo)) == sizeof(vtableInfo) &&
		vtableInfo.State == MEM_COMMIT &&
		(vtableInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
#else
	// A non-null vtable pointer is as much as can be verified portably.
	return true;
#endif
}
}
