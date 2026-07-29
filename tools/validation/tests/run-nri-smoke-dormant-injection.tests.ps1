$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '../../..')
$outputDir = Join-Path $root 'build/smoke-dormant-injection-tests'
$testExe = Join-Path $outputDir 'nri_smoke_dormant_injection.tests.exe'
$testSource = Join-Path $PSScriptRoot 'nri_smoke_dormant_injection.tests.cpp'
$implementation = Join-Path $root 'source/common/rendering/nri/renderer/nri_smoke_dormant_injection.cpp'
$includeDir = Join-Path $root 'source/common/rendering/nri/renderer'

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'
$command = 'call "{0}" -arch=x64 -host_arch=x64 && cl /nologo /std:c++17 /EHsc /W4 /WX /I"{1}" "{2}" "{3}" /Fe:"{4}"' -f $vsDevCmd, $includeDir, $testSource, $implementation, $testExe
cmd /c $command
if ($LASTEXITCODE -ne 0) { throw "smoke dormant-injection tests failed to compile with exit code $LASTEXITCODE" }

& $testExe
if ($LASTEXITCODE -ne 0) { throw "smoke dormant-injection tests failed with exit code $LASTEXITCODE" }
Write-Host 'nri_smoke_dormant_injection.tests: PASS'
