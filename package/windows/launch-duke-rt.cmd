@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%.") do set "SCRIPT_DIR=%%~fI"

set "DIST_ROOT=%SCRIPT_DIR%"
set "OVERLAY_DIR=%DIST_ROOT%\release-overlay"
if not exist "%OVERLAY_DIR%" set "OVERLAY_DIR=%DIST_ROOT%\default-overlay"
set "RAZE_EXE=%DIST_ROOT%\raze.exe"
set "PREFLIGHT_PS1=%DIST_ROOT%\tools\dist\Prepare-CommercialNormals.ps1"

if not exist "%RAZE_EXE%" (
    set "DIST_ROOT=%SCRIPT_DIR%\..\.."
    for %%I in ("%DIST_ROOT%") do set "DIST_ROOT=%%~fI"
    set "OVERLAY_DIR=%DIST_ROOT%\release-overlay"
    if not exist "%OVERLAY_DIR%" set "OVERLAY_DIR=%DIST_ROOT%\default-overlay"
    set "RAZE_EXE=%DIST_ROOT%\build\terminal-release\raze.exe"
    if not exist "%RAZE_EXE%" set "RAZE_EXE=%DIST_ROOT%\build\terminal-ninja\raze.exe"
    set "PREFLIGHT_PS1=%DIST_ROOT%\tools\dist\Prepare-CommercialNormals.ps1"
)

if not exist "%RAZE_EXE%" (
    echo [duke-rt] raze.exe was not found beside the launcher or at the repo fallback path.
    exit /b 1
)

if not exist "%PREFLIGHT_PS1%" (
    echo [duke-rt] preflight script was not found: "%PREFLIGHT_PS1%"
    exit /b 1
)

set "PREFLIGHT_ARGS="
set "GAME_ARGS="

:parse_args
if "%~1"=="" goto args_done

if /I "%~1"=="-Ask" (
    set "PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -Ask"
    shift
    goto parse_args
)
if /I "%~1"=="-Yes" (
    set "PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -Yes"
    shift
    goto parse_args
)
if /I "%~1"=="-No" (
    set "PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -No"
    shift
    goto parse_args
)
if /I "%~1"=="-Force" (
    set "PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -Force"
    shift
    goto parse_args
)
if /I "%~1"=="-Quiet" (
    set "PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -Quiet"
    shift
    goto parse_args
)
if /I "%~1"=="-SourceRoot" (
    if "%~2"=="" (
        echo [duke-rt] -SourceRoot requires a path argument.
        exit /b 1
    )
    set "PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -SourceRoot ""%~2"""
    shift
    shift
    goto parse_args
)
if /I "%~1"=="-OverlayDir" (
    if "%~2"=="" (
        echo [duke-rt] -OverlayDir requires a path argument.
        exit /b 1
    )
    set "PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -OverlayDir ""%~2"""
    set "OVERLAY_DIR=%~f2"
    shift
    shift
    goto parse_args
)

set "GAME_ARGS=%GAME_ARGS% %1"
shift
goto parse_args

:args_done

if not exist "%OVERLAY_DIR%" (
    echo [duke-rt] overlay directory was not found: "%OVERLAY_DIR%"
    exit /b 1
)

powershell -ExecutionPolicy Bypass -File "%PREFLIGHT_PS1%" -LaunchRoot "%DIST_ROOT%" -OverlayDir "%OVERLAY_DIR%" %PREFLIGHT_ARGS%
if errorlevel 1 (
    echo [duke-rt] content preflight failed.
    exit /b 1
)

"%RAZE_EXE%" -file "%OVERLAY_DIR%" %GAME_ARGS%
exit /b %errorlevel%
