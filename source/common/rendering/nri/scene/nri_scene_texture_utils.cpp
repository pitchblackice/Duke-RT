#include "nri_scene_texture_utils.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <windows.h>

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

	MEMORY_BASIC_INFORMATION pointerInfo = {};
	if (VirtualQuery(texture, &pointerInfo, sizeof(pointerInfo)) != sizeof(pointerInfo) ||
		pointerInfo.State != MEM_COMMIT ||
		(pointerInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
	{
		return false;
	}

	void* vtable = nullptr;
	__try
	{
		vtable = *(void**)texture;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		vtable = nullptr;
	}

	if (vtable == nullptr)
	{
		return false;
	}

	MEMORY_BASIC_INFORMATION vtableInfo = {};
	return VirtualQuery(vtable, &vtableInfo, sizeof(vtableInfo)) == sizeof(vtableInfo) &&
		vtableInfo.State == MEM_COMMIT &&
		(vtableInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}
}
