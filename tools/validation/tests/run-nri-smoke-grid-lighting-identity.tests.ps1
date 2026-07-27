$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$sourceDir = Join-Path $repoRoot 'source\common\rendering\nri\renderer'
$testSource = Join-Path $PSScriptRoot 'nri_smoke_grid_lighting_identity.tests.cpp'
$identitySource = Join-Path $sourceDir 'nri_smoke_grid_lighting_identity.cpp'
$outputDir = Join-Path $repoRoot 'build\smoke-grid-lighting-identity-tests'
$testExe = Join-Path $outputDir 'nri_smoke_grid_lighting_identity.tests.exe'
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$compile = 'call "{0}" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /I"{1}" /Fo"{5}/" "{2}" "{3}" /Fe:"{4}"' -f `
	$vsDevCmd, $sourceDir, $identitySource, $testSource, $testExe, $outputDir
cmd /c $compile
if ($LASTEXITCODE -ne 0)
{
	throw "Smoke grid lighting identity test compilation failed with exit code $LASTEXITCODE."
}

& $testExe
if ($LASTEXITCODE -ne 0)
{
	throw "Smoke grid lighting identity tests failed with exit code $LASTEXITCODE."
}

Write-Host 'Smoke grid lighting identity tests passed.'
