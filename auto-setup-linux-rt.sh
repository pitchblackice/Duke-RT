#!/usr/bin/env bash
#
# auto-setup-linux-rt.sh
#
# Builds Duke-RT on Linux with the NRI path-tracing renderer enabled.
#
# The stock auto-setup-linux.sh cannot work for this fork: it pins ZMusic 1.1.12
# (too old - the engine needs zmusic_mod_preferredplayer, and the Windows
# instructions deliberately do not pin a tag), and it builds the non-RT
# configuration, which does not link because shared game code references nri_*
# symbols unconditionally.
#
# This script needs no root. If the SDL2/OpenAL development packages are not
# installed it downloads the .debs and extracts them into a local sysroot.
#
# Tools fetched locally when the system copies are unsuitable:
#   * CMake >= 3.30   (NRI requires it; Ubuntu 24.04 ships 3.28)
#   * DXC             (compiles the NRI shaders; not packaged for Ubuntu)
#
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
BUILD="$ROOT/build"
TOOLS="$BUILD/tools"
SYSROOT="$BUILD/sysroot"
LIBDIR="$SYSROOT/usr/lib/x86_64-linux-gnu"
JOBS="$(nproc)"

info() { printf '\n=== %s ===\n' "$*"; }

mkdir -p "$BUILD" "$TOOLS"

# ---------------------------------------------------------------- toolchain --

info "Checking toolchain"
for t in git gcc g++ make pkg-config python3; do
	command -v "$t" >/dev/null || { echo "missing required tool: $t"; exit 1; }
done

CMAKE="$(command -v cmake || true)"
need_cmake=1
if [[ -n "$CMAKE" ]]; then
	ver="$($CMAKE --version | head -1 | grep -oE '[0-9]+\.[0-9]+')"
	if [[ "$(printf '%s\n3.30\n' "$ver" | sort -V | head -1)" == "3.30" ]]; then
		need_cmake=0
	fi
fi
if [[ $need_cmake -eq 1 ]]; then
	CM_VER=3.31.9
	if [[ ! -x "$TOOLS/cmake-$CM_VER-linux-x86_64/bin/cmake" ]]; then
		info "Fetching CMake $CM_VER (NRI needs >= 3.30)"
		curl -fsSL -o "$TOOLS/cmake.tar.gz" \
			"https://github.com/Kitware/CMake/releases/download/v$CM_VER/cmake-$CM_VER-linux-x86_64.tar.gz"
		tar xzf "$TOOLS/cmake.tar.gz" -C "$TOOLS"
	fi
	CMAKE="$TOOLS/cmake-$CM_VER-linux-x86_64/bin/cmake"
fi
echo "cmake: $CMAKE ($($CMAKE --version | head -1))"

DXC="$(command -v dxc || true)"
if [[ -z "$DXC" ]]; then
	if [[ ! -x "$TOOLS/bin/dxc" ]]; then
		info "Fetching DXC (not packaged for Ubuntu)"
		url="$(curl -fsSL https://api.github.com/repos/microsoft/DirectXShaderCompiler/releases/latest \
			| python3 -c 'import json,sys;print(next(a["browser_download_url"] for a in json.load(sys.stdin)["assets"] if "linux" in a["name"] and a["name"].endswith(".tar.gz")))')"
		curl -fsSL -o "$TOOLS/dxc.tar.gz" "$url"
		tar xzf "$TOOLS/dxc.tar.gz" -C "$TOOLS"
		chmod +x "$TOOLS/bin/dxc"
	fi
	DXC="$TOOLS/bin/dxc"
fi
echo "dxc: $DXC"

# ------------------------------------------------------------------ sysroot --

CMAKE_EXTRA=()
if pkg-config --exists sdl2 2>/dev/null; then
	info "Using system SDL2"
else
	info "SDL2 dev package missing - building a local sysroot (no root needed)"
	mkdir -p "$SYSROOT" "$BUILD/debs"
	( cd "$BUILD/debs"
	  for p in libsdl2-dev libsdl2-2.0-0 libopenal-dev libopenal1 \
	           libwebp-dev libwebp7 libsndfile1-dev libsndfile1 libmpg123-dev; do
		apt-get download "$p" >/dev/null 2>&1 || echo "  (skipped $p)"
	  done
	  for f in *.deb; do [[ -e "$f" ]] && dpkg-deb -x "$f" "$SYSROOT"; done )

	INC="-isystem $SYSROOT/usr/include -isystem $SYSROOT/usr/include/SDL2"
	INC="$INC -isystem $SYSROOT/usr/include/x86_64-linux-gnu"
	INC="$INC -isystem $SYSROOT/usr/include/x86_64-linux-gnu/SDL2"
	CMAKE_EXTRA=(
		-DSDL2_INCLUDE_DIR="$SYSROOT/usr/include/SDL2"
		-DSDL2_LIBRARY="$LIBDIR/libSDL2.so"
		-DCMAKE_C_FLAGS="$INC"
		-DCMAKE_CXX_FLAGS="$INC"
		-DCMAKE_EXE_LINKER_FLAGS="-L$LIBDIR -Wl,-rpath,$LIBDIR"
	)
	export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
	export PKG_CONFIG_LIBDIR="$LIBDIR/pkgconfig"
fi

# ------------------------------------------------------------------- ZMusic --

info "Building ZMusic (master - 1.1.12 is too old for this fork)"
if [[ ! -d "$BUILD/zmusic/.git" ]]; then
	git clone -q https://github.com/zdoom/zmusic "$BUILD/zmusic"
fi
git -C "$BUILD/zmusic" fetch -q --all
git -C "$BUILD/zmusic" checkout -q master
git -C "$BUILD/zmusic" pull -q
"$CMAKE" -S "$BUILD/zmusic" -B "$BUILD/zmusic/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
make -C "$BUILD/zmusic/build" -j"$JOBS" zmusiclite >/dev/null
echo "zmusiclite: $BUILD/zmusic/build/source/libzmusiclite.so"

# ---------------------------------------------------------- FidelityFX SDK --
#
# Frame generation needs libamd_fidelityfx_vk.so. AMD ship no Linux build and
# the SDK does not compile with GCC as-is, so we clone the pinned release and
# apply linux-port/ffx-sdk-1.1.4-linux.patch, which covers:
#   * porting the FidelityFX_SC shader compiler off MSVC/DXC to a GLSL-only build
#   * three upstream SDK bugs (a wrong CMake target, an snprintf overrun in
#     MD5HashString, and a shader-permutation list returning its own variable name)
#   * Win32-only constructs in the Vulkan backend and the ffx-api dispatch layer
#
# This is optional: without it the build falls back to the frame generation stub.

FFX_SDK="$BUILD/ffx-sdk"
FFX_COMMIT=c6efa6b            # AMD FidelityFX SDK 1.1.4
FFX_PATCH="$ROOT/linux-port/ffx-sdk-1.1.4-linux.patch"

if [[ -f "$FFX_PATCH" ]]; then
	info "Preparing the AMD FidelityFX SDK"

	# glslangValidator drives the GLSL -> SPIR-V step. Reuse whatever is around
	# before downloading anything.
	GLSLANG="$(command -v glslangValidator || true)"
	if [[ -z "$GLSLANG" ]]; then
		found="$(find "$BUILD/tools" -name glslangValidator -type f 2>/dev/null | head -1)"
		[[ -n "$found" ]] && GLSLANG="$found"
	fi
	if [[ -z "$GLSLANG" ]]; then
		if [[ ! -x "$TOOLS/glslang/bin/glslangValidator" ]]; then
			echo "fetching glslang"
			mkdir -p "$TOOLS/glslang"
			curl -fsSL -o "$TOOLS/glslang.zip" \
				"https://github.com/KhronosGroup/glslang/releases/download/14.3.0/glslang-master-linux-Release.zip" \
				&& unzip -qo "$TOOLS/glslang.zip" -d "$TOOLS/glslang" || true
		fi
		[[ -x "$TOOLS/glslang/bin/glslangValidator" ]] && GLSLANG="$TOOLS/glslang/bin/glslangValidator"
	fi

	if [[ -z "$GLSLANG" ]]; then
		echo "WARNING: no glslangValidator; skipping FidelityFX (frame generation will use the stub)"
	else
		if [[ ! -d "$FFX_SDK/.git" ]]; then
			git clone -q https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git "$FFX_SDK"
		fi
		git -C "$FFX_SDK" fetch -q --all
		git -C "$FFX_SDK" checkout -q "$FFX_COMMIT"

		# Idempotent: only patch when the tree is still pristine.
		if git -C "$FFX_SDK" apply --check --reverse "$FFX_PATCH" 2>/dev/null; then
			echo "FidelityFX patch already applied"
		else
			git -C "$FFX_SDK" checkout -q -- .
			git -C "$FFX_SDK" apply "$FFX_PATCH"
			echo "applied $FFX_PATCH"
		fi
		cp -f "$ROOT/linux-port/ffx_linux_compat.h" "$ROOT/linux-port/ffx_win32_thread_compat.h" "$FFX_SDK/"

		info "Building the FidelityFX shader compiler"
		SC_DIR="$FFX_SDK/sdk/tools/ffx_shader_compiler"
		"$CMAKE" -S "$SC_DIR" -B "$SC_DIR/build-linux" -DCMAKE_BUILD_TYPE=Release >/dev/null
		make -C "$SC_DIR/build-linux" -j"$JOBS" >/dev/null
		# The compiler resolves glslang next to itself when -glslangexe is absent.
		cp -f "$GLSLANG" "$SC_DIR/bin/glslangValidator"

		info "Building libamd_fidelityfx_vk.so (this compiles every shader permutation)"
		"$CMAKE" -S "$FFX_SDK/ffx-api" -B "$FFX_SDK/ffx-api/build-vk" \
			-DCMAKE_BUILD_TYPE=Release -DFFX_API_BACKEND=VK_X64 >/dev/null
		make -C "$FFX_SDK/ffx-api/build-vk" -j"$JOBS" >/dev/null
		echo "ffx runtime: $FFX_SDK/ffx-api/build-vk/libamd_fidelityfx_vk.so"
	fi
fi

# ------------------------------------------------------------------ Duke-RT --

info "Configuring Duke-RT (HAVE_NRI=ON)"
"$CMAKE" -S "$ROOT" -B "$BUILD" \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DHAVE_NRI=ON \
	-DRAZE_DXC_EXECUTABLE="$DXC" \
	-DZMUSIC_INCLUDE_DIR="$BUILD/zmusic/include" \
	-DZMUSIC_LIBRARIES="$BUILD/zmusic/build/source/libzmusiclite.so" \
	"${CMAKE_EXTRA[@]}"

info "Building raze"
make -C "$BUILD" -j"$JOBS" raze

# NRI is dlopen'd at runtime rather than linked, so it is not pulled in by the
# raze target and has to be built and staged beside the binary explicitly.
info "Building and staging libNRI.so"
make -C "$BUILD" -j"$JOBS" NRI
cp -f "$BUILD/libraries/NRIFramework/External/NRI/libNRI.so" "$BUILD/libNRI.so"

info "Done"
cat <<EOF

Binary:   $BUILD/raze
Run with your Duke 3D data, for example:

  $BUILD/raze -gamegrp "/path/to/DUKE3D.GRP" -file /path/to/release-overlay

The Vulkan NRI backend is selected automatically on Linux.

DLSS super resolution and ray reconstruction both work. The NGX runtime is
staged next to the binary by the build; without it CreateUpscaler fails and the
upscale pass emits a black frame. Selecting an upscaler in the menu also flips
the denoiser to match (DLRR does its own denoising, everything else needs NRD).

HDR falls back to SDR: NRI's DisplayDescHelper is a stub off Windows and X11 has
no HDR path.

Frame generation loads its Vulkan runtime and initialises, but does not take over
presentation yet: the FFX Vulkan swapchain replaces the VkSwapchainKHR outright
and NRI neither exposes its own nor adopts an external one, so a separate
presentation path is still needed. It reports 'vk-present-bridge-not-implemented'
and falls back to the native present.
EOF
