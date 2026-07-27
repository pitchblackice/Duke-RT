$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$sourceDir = Join-Path $repoRoot 'source\common\rendering\nri\renderer'
$testSource = Join-Path $PSScriptRoot 'nri_smoke_admission.tests.cpp'
$outputDir = Join-Path $repoRoot 'build\smoke-admission-tests'
$testExe = Join-Path $outputDir 'nri_smoke_admission.tests.exe'
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$compile = 'call "{0}" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /I"{1}" /Fo"{4}/" "{2}" /Fe:"{3}"' -f `
	$vsDevCmd, $sourceDir, $testSource, $testExe, $outputDir
cmd /c $compile
if ($LASTEXITCODE -ne 0) { throw "Smoke admission test compilation failed with exit code $LASTEXITCODE." }

& $testExe
if ($LASTEXITCODE -ne 0) { throw "Smoke admission tests failed with exit code $LASTEXITCODE." }

$smokeOwner = Get-Content -Raw (Join-Path $sourceDir 'nri_smoke.cpp')
if ($smokeOwner -notmatch 'gridAuthority\s*&&\s*!particleAuthority[\s\S]{0,220}SelectGridCommands\(availableCommands')
{
	throw 'Grid selection must consume the persistent pulse queue before the fixed upload cap.'
}
if ($smokeOwner -notmatch 'boundedDeferred\s*=\s*mStatus\.admission\.rejected;[\s\S]{0,80}mStatus\.admission\.rejected\s*=\s*0u')
{
	throw 'Unselected pulse ranges must remain bounded-deferred rather than becoming terminal rejection.'
}

Write-Host 'Smoke admission structural and pure tests passed.'
