$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '../../..')
$outputDir = Join-Path $root 'build/smoke-dormant-grid-tests'
$testExe = Join-Path $outputDir 'nri_smoke_dormant_grid.tests.exe'
$testSource = Join-Path $PSScriptRoot 'nri_smoke_dormant_grid.tests.cpp'
$includeDir = Join-Path $root 'source/common/rendering/nri/renderer'

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'
$command = 'call "{0}" -arch=x64 -host_arch=x64 && cl /nologo /std:c++17 /EHsc /W4 /WX /I"{1}" "{2}" /Fe:"{3}"' -f $vsDevCmd, $includeDir, $testSource, $testExe
cmd /c $command
if ($LASTEXITCODE -ne 0) { throw "smoke dormant-grid tests failed to compile with exit code $LASTEXITCODE" }

& $testExe
if ($LASTEXITCODE -ne 0) { throw "smoke dormant-grid tests failed with exit code $LASTEXITCODE" }
Write-Host 'nri_smoke_dormant_grid.tests: PASS'
