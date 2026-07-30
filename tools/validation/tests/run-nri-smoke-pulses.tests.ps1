Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$outputDir = Join-Path $root 'build/smoke-pulse-tests'
$testExe = Join-Path $outputDir 'nri_smoke_pulses.tests.exe'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$source = Join-Path $PSScriptRoot 'nri_smoke_pulses.tests.cpp'
$owner = Join-Path $root 'source/common/rendering/nri/renderer/nri_smoke_pulses.cpp'
$include = Join-Path $root 'source/common/rendering/nri/renderer'
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'
$compile = 'call "' + $vsDevCmd + '" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /I"' +
    $include + '" /Fo"' + $outputDir + '/" "' + $source + '" "' + $owner + '" /Fe:"' + $testExe + '"'
$process = Start-Process -FilePath 'cmd.exe' -ArgumentList '/d', '/c', $compile -PassThru -Wait -NoNewWindow
if ($process.ExitCode -ne 0) { throw "smoke pulse test compilation failed with exit code $($process.ExitCode)" }

& $testExe
if ($LASTEXITCODE -ne 0) { throw "smoke pulse tests failed with exit code $LASTEXITCODE" }
