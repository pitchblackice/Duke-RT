Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$outputDir = Join-Path $root 'build/smoke-continuous-source-tests'
$testExe = Join-Path $outputDir 'nri_smoke_continuous_sources.tests.exe'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$include = Join-Path $root 'source/common/rendering/nri/renderer'
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'
$testSource = Join-Path $PSScriptRoot 'nri_smoke_continuous_sources.tests.cpp'
$compile = 'call "' + $vsDevCmd + '" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /I"' +
	$include + '" /Fo"' + $outputDir + '/" "' + $testSource + '" /Fe:"' + $testExe + '"'
$process = Start-Process -FilePath 'cmd.exe' -ArgumentList '/d', '/c', $compile -PassThru -Wait -NoNewWindow
if ($process.ExitCode -ne 0) { throw "smoke continuous-source test compilation failed with exit code $($process.ExitCode)" }
& $testExe
if ($LASTEXITCODE -ne 0) { throw "smoke continuous-source tests failed with exit code $LASTEXITCODE" }
