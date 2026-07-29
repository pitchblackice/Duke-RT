// Compatibility shims for building the FidelityFX SDK with GCC/Clang on Linux.
// The SDK targets MSVC and relies on its CRT extensions; these are the pieces it
// uses that are either MSVC-only or simply not included by the SDK's own headers.
#pragma once

#include <cstring>
#include <cwchar>
#include <cstddef>
#include <cstdio>
#include <cmath>

// Win32 threading/timing primitives used by the VK frame-interpolation swapchain.
#include "ffx_win32_thread_compat.h"

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef FFX_UNUSED
#define FFX_UNUSED(x) ((void)(x))
#endif

// MSVC's bounds-checked wide string copy. The SDK always passes the destination
// capacity in elements, matching wcsncpy semantics plus explicit termination.
#ifndef _MSC_VER
static inline int wcscpy_s(wchar_t* dst, size_t sizeInWords, const wchar_t* src)
{
	if (dst == nullptr || src == nullptr || sizeInWords == 0)
	{
		return 22; // EINVAL
	}

	wcsncpy(dst, src, sizeInWords - 1);
	dst[sizeInWords - 1] = L'\0';
	return 0;
}

// MSVC's bounds-checked sprintf. The SDK always passes an explicit capacity and
// uses the return value as the number of characters written.
template <typename... Args>
static inline int sprintf_s(char* buffer, size_t sizeInBytes, const char* format, Args... args)
{
	if (buffer == nullptr || sizeInBytes == 0)
	{
		return -1;
	}

	const int written = snprintf(buffer, sizeInBytes, format, args...);
	return (written < 0 || (size_t)written >= sizeInBytes) ? -1 : written;
}

// MSVC also provides a template overload that deduces the destination capacity
// from a fixed-size array, which is the form the SDK actually calls.
template <size_t N>
static inline int wcscpy_s(wchar_t (&dst)[N], const wchar_t* src)
{
	return wcscpy_s(dst, N, src);
}
#endif
