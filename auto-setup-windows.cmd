@echo off
goto aftercopyright

**
** auto-setup-windows.cmd
** Automatic (easy) setup and build script for Windows
**
** Note that this script assumes you have both 'git' and 'cmake' installed properly and in your PATH!
** This script also assumes you have installed a build system that cmake can automatically detect.
** Such as Visual Studio Community. Requires appropriate SDK installed too!
** Without these items, this script will FAIL! So make sure you have your build environment properly
** set up in order for this script to succeed.
**
** The purpose of this script is to get someone easily going with a full working compile of Raze.
** This allows anyone to make simple changes or tweaks to the engine as they see fit and easily
** compile their own copy without having to follow complex instructions to get it working.
** Every build environment is different, and every computer system is different - this should work
** in most typical systems under Windows but it may fail under certain types of systems or conditions.
** Not guaranteed to work and your mileage will vary.
**
**---------------------------------------------------------------------------
** Copyright 2023-2024 Rachael Alexanderson and the GZDoom team
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**

:aftercopyright

setlocal enableextensions enabledelayedexpansion

rem Work around modern CMake disallowing LOCATION lookups inside old ports (yasm)
set "VCPKG_CMAKE_CONFIGURE_OPTIONS=-DCMAKE_POLICY_DEFAULT_CMP0026=OLD"
set "VCPKG_KEEP_ENV_VARS=VCPKG_CMAKE_CONFIGURE_OPTIONS"

rem -- Always operate within the build folder
if not exist "%~dp0\build" mkdir "%~dp0\build"
pushd "%~dp0\build"

set "VCPKG_ROOT=%CD%\vcpkg"
set "PATH=%VCPKG_ROOT%;%PATH%"
set "VCPKG_OVERLAY_PORTS=%~dp0vcpkg-overlays"

if exist vcpkg (
	git -C ./vcpkg pull
) else (
	git clone https://github.com/microsoft/vcpkg
)
git -C ./vcpkg checkout 74e6536215718009aae747d86d84b78376bf9e09

if not exist vcpkg\vcpkg.exe (
	call .\vcpkg\bootstrap-vcpkg.bat -disableMetrics
	if errorlevel 1 (
		echo Failed to bootstrap vcpkg.
		exit /b 1
	)
)

if exist zmusic (
	git -C ./zmusic pull
) else (
	git clone https://github.com/zdoom/zmusic
)

if not exist "%~dp0\build\zmusic\build" mkdir "%~dp0\build\zmusic\build"
if not exist "%~dp0\build\vcpkg_installed" mkdir "%~dp0\build\vcpkg_installed"

cmake -A x64 -S ./zmusic -B ./zmusic/build ^
	-DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake ^
	-DVCPKG_LIBSNDFILE=1 ^
	-DVCPKG_INSTALLED_DIR=../vcpkg_installed/
cmake --build ./zmusic/build --config Release -- -maxcpucount -verbosity:minimal

cmake -A x64 -S .. -B . ^
	-DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake ^
	-DZMUSIC_INCLUDE_DIR=./zmusic/include ^
	-DZMUSIC_LIBRARIES=./zmusic/build/source/Release/zmusiclite.lib ^
	-DVCPKG_INSTALLED_DIR=./vcpkg_installed/
cmake --build . --config RelWithDebInfo -- -maxcpucount -verbosity:minimal

echo === Installing vcpkg dependencies for x64-windows ===
.\vcpkg\vcpkg.exe install --triplet x64-windows
if errorlevel 1 (
	echo vcpkg install failed.
	exit /b 1
)

set "DEP_BIN_DIR=.\zmusic\build\source\Release\"
set "DEP_BIN_DEBUG_DIR=..\vcpkg_installed\x64-windows\debug\bin"
set "DEP_DLLS=FLAC.dll libmp3lame.dll mpg123.dll ogg.dll opus.dll out123.dll sndfile.dll syn123.dll vorbis.dll vorbisenc.dll vorbisfile.dll zlib1.dll zmusiclite.dll"
if not exist RelWithDebInfo mkdir RelWithDebInfo
for %%D in (%DEP_DLLS%) do (
	if exist "%DEP_BIN_DIR%\%%D" (
		copy /Y "%DEP_BIN_DIR%\%%D" RelWithDebInfo >nul
	) else if exist "%DEP_BIN_DEBUG_DIR%\%%D" (
		copy /Y "%DEP_BIN_DEBUG_DIR%\%%D" RelWithDebInfo >nul
	)
)

echo === Copying OpenAL DLL ===
pwd
ls
set "OPENAL_DLL=.\vcpkg\packages\openal-soft_x64-windows\bin\OpenAL32.dll"
if exist "%OPENAL_DLL%" (
	if not exist RelWithDebInfo mkdir RelWithDebInfo
	copy /Y "%OPENAL_DLL%" RelWithDebInfo >nul
)

rem -- If successful, show the build
if not errorlevel 1 if exist RelWithDebInfo\raze.exe explorer.exe RelWithDebInfo
