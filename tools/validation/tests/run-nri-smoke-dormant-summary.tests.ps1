Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$outputDir = Join-Path $root 'build/smoke-dormant-summary-tests'
$testExe = Join-Path $outputDir 'nri_smoke_dormant_summary.tests.exe'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$source = Join-Path $PSScriptRoot 'nri_smoke_dormant_summary.tests.cpp'
$owner = Join-Path $root 'source/common/rendering/nri/renderer/nri_smoke_dormant_summary.cpp'
$include = Join-Path $root 'source/common/rendering/nri/renderer'
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'
$compile = 'call "' + $vsDevCmd + '" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /I"' +
    $include + '" /Fo"' + $outputDir + '/" "' + $source + '" "' + $owner + '" /Fe:"' + $testExe + '"'
$process = Start-Process -FilePath 'cmd.exe' -ArgumentList '/d', '/c', $compile -PassThru -Wait -NoNewWindow
if ($process.ExitCode -ne 0) { throw "smoke dormant-summary test compilation failed with exit code $($process.ExitCode)" }

& $testExe
if ($LASTEXITCODE -ne 0) { throw "smoke dormant-summary tests failed with exit code $LASTEXITCODE" }

$hlsl = Get-Content (Join-Path $root 'source/common/rendering/nri/shaders/Include/SmokeDormantSummary.hlsli') -Raw
if ($hlsl -notmatch 'return\s+exp2\(') { throw 'HLSL dormant decay must use exact exponential decay.' }
if ($hlsl -notmatch 'uint\s+SummaryGeneration') { throw 'HLSL dormant summary must carry generation identity.' }
Write-Host 'Smoke dormant-summary HLSL contract validation passed.'
