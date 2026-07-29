# Duke-RT Linux port

Everything here exists so the Linux build can be reproduced from a clean
checkout. Run `../auto-setup-linux-rt.sh`; it needs no root.

## Contents

| File | Purpose |
| --- | --- |
| `ffx-sdk-1.1.4-linux.patch` | Makes the AMD FidelityFX SDK 1.1.4 build with GCC on Linux (25 files) |
| `ffx_linux_compat.h` | MSVC/CRT shims the SDK relies on (`_countof`, `wcscpy_s`, `sprintf_s`, …) |
| `ffx_win32_thread_compat.h` | Win32 threading/timing on pthreads, for the Vulkan frame-interpolation swapchain |

The FidelityFX pieces are optional. Without them the build falls back to the
frame generation stub and everything else still works.

## What the patch covers

The SDK is Windows/MSVC-only upstream. The patch splits into three groups.

**Shader compiler (`FidelityFX_SC`).** Ported off MSVC and DXC to a GLSL-only
build: guarded the D3D12/DXGI includes, replaced two Win32 path helpers with
`std::filesystem`, added a POSIX `main` that transcodes `argv` for the existing
`wmain`, replaced `WideCharToMultiByte`/`MultiByteToWideChar` with direct UTF-8
transcoding, added a `_wfopen_s` shim, and dropped the DXC/Agility/`dxguid`
dependencies from CMake. The SDK ships GLSL sources for every shader, so the
Vulkan path never needs HLSL.

**Three upstream SDK bugs**, all latent on Windows:

1. `libs/glslangValidator/CMakeLists.txt` called
   `target_include_directories(dxc …)` — a copy-paste error that only worked
   because the `dxc` target happened to exist.
2. `MD5HashString` passed a fixed size of 32 to `snprintf` while advancing the
   write pointer, overrunning its 33-byte buffer. Harmless under MSVC, fatal
   under glibc's `_FORTIFY_SOURCE`.
3. `CMakeCompileShaders.txt` returned its output-variable *name* as element 0 of
   the results list, because `list(APPEND PERMUTATION_OUTPUTS …)` shadows the
   parameter holding that name. Make then treats the literal name as a target.

**Vulkan backend and ffx-api.** `CMAKE_GENERATOR_PLATFORM` is MSVC-only and
breaks `project()` under Unix Makefiles; the platform-name detection had no
non-MSVC branch. `FFX_API_ENTRY` used `__declspec(dllexport)`. The MSVC `ui64`
literal suffix. `#include <FidelityFx/…>` with the wrong case, which only works
on case-insensitive filesystems. The AMD driver-side provider override is a
DX12/COM interface compiled unconditionally. Static libraries needed
`POSITION_INDEPENDENT_CODE` to link into a shared object. `VK_EXT_full_screen_exclusive`
is Windows-only by definition. `std::wstring_convert` is deprecated and gone in
newer libstdc++.

Notably, AMD's *Vulkan* frame-interpolation swapchain is itself written against
Win32 primitives (`CRITICAL_SECTION`, events, `CreateThread`,
`QueryPerformanceCounter`). `ffx_win32_thread_compat.h` maps those onto pthreads
and `CLOCK_MONOTONIC`. Two caveats: `SetThreadPriority` is a no-op because
raising priority needs privileges we cannot assume, and `QueryPerformanceCounter`
reports a 1 GHz counter.

## Regenerating the patch

```sh
cd build/ffx-sdk
git diff > ../../linux-port/ffx-sdk-1.1.4-linux.patch
```

It is pinned to commit `c6efa6b` (AMD FidelityFX SDK 1.1.4). Verify with:

```sh
git -C build/ffx-sdk apply --check linux-port/ffx-sdk-1.1.4-linux.patch
```

## Port notes beyond FidelityFX

These live in the tracked engine sources, not here, but are worth knowing.

**DLSS/NGX must sit next to the executable.** NGX resolves its runtime relative
to the binary. NRI's CMake stages the Linux `.so` files into its *own* target
directory, which is not where the game runs from, so `CreateUpscaler` returns
`FAILURE`, the whole upscale pass early-returns, and you get a black frame with
no other diagnostic. `source/CMakeLists.txt` now stages them beside the binary.
`nri_upscaler.cpp` also warns on creation failure instead of failing silently.

**The denoiser is paired to the upscaler.** DLRR does its own denoising and
needs NRD off; everything else needs NRD on. `nri_upscaler` is a `CUSTOM_CVAR`
that keeps the two in step, because they live in different menu sections.

**HDR is unavailable.** NRI's `DisplayDescHelper` is a stub off Windows and X11
has no HDR path, so the settings presets are pinned to SDR rather than selecting
a mode the renderer cannot present.

**Frame generation is incomplete.** The FFX Vulkan runtime loads and the
FrameGeneration context initialises, but presentation is not taken over. Unlike
DX12 — where FFX hands back a proxy swapchain the game presents through — the
Vulkan path *replaces* the `VkSwapchainKHR` and requires acquire/present to run
through FFX-supplied replacement functions. NRI owns its swapchain and exposes
neither the handle nor a way to adopt an external one (`NRIWrapperVK` adopts
buffers, textures, fences and pipelines, but not swapchains). Finishing this
means a parallel presentation path: own `VkSurfaceKHR` from the SDL window, hand
the create-info to `ffxCreateContextDescFrameGenerationSwapChainVK`, wrap the
resulting images back into NRI textures with `CreateTextureVK`, and present
through FFX. The Windows path already bypasses NRI's swapchain in the same way.
Until then it reports `vk-present-bridge-not-implemented` and falls back to the
native present.
