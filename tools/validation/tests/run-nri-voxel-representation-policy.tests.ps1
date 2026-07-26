$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$sourceDir = Join-Path $repoRoot 'source\common\rendering\nri\renderer'
$testSource = Join-Path $PSScriptRoot 'nri_voxel_representation_policy.tests.cpp'
$policySource = Join-Path $sourceDir 'nri_voxel_representation_policy.cpp'
$outputDir = Join-Path $repoRoot 'build\voxel-representation-policy-tests'
$testExe = Join-Path $outputDir 'nri_voxel_representation_policy.tests.exe'
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$compile = 'call "{0}" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /I"{1}" /Fo"{5}\\" "{2}" "{3}" /Fe:"{4}"' -f `
	$vsDevCmd, $sourceDir, $policySource, $testSource, $testExe, $outputDir
cmd /c $compile
if ($LASTEXITCODE -ne 0)
{
	throw "Voxel representation policy test compilation failed with exit code $LASTEXITCODE."
}

& $testExe
if ($LASTEXITCODE -ne 0)
{
	throw "Voxel representation policy tests failed with exit code $LASTEXITCODE."
}

$sceneFrameBuild = Get-Content -LiteralPath (Join-Path $sourceDir 'nri_scene_frame_build.cpp') -Raw
if ($sceneFrameBuild -notmatch 'if\s*\(!inputs\.preserveHistory\)[\s\S]{0,1800}mVoxelRepresentationPolicy\.BeginFrame' -or
	$sceneFrameBuild -notmatch 'if\s*\(!inputs\.preserveHistory\)[\s\S]{0,500}persistentVoxelTlasServices\.evaluateRepresentation')
{
	throw 'Voxel representation decisions must remain main-view-only so offscreen history-preserving builds cannot replace the camera or duplicate the snapshot.'
}

Write-Host 'Voxel representation policy tests passed.'
