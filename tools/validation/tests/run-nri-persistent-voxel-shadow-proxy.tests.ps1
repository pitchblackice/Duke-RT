param([string]$OutputDirectory)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$rendererDir = Join-Path $repoRoot 'source\common\rendering\nri\renderer'
$proxySource = Join-Path $rendererDir 'nri_persistent_voxel_shadow_proxy.cpp'
$testSource = Join-Path $PSScriptRoot 'nri_persistent_voxel_shadow_proxy.tests.cpp'
$outputDir = if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    Join-Path $repoRoot 'build\persistent-voxel-shadow-proxy-tests'
} else {
    [IO.Path]::GetFullPath($OutputDirectory)
}
$testExe = Join-Path $outputDir 'nri_persistent_voxel_shadow_proxy.tests.exe'
$nriInclude = Join-Path $repoRoot 'libraries\NRIFramework\External\NRI\Include'
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$includeDirs = @(
    $nriInclude,
    (Join-Path $repoRoot 'source'),
    (Join-Path $repoRoot 'source\common'),
    (Join-Path $repoRoot 'source\common\utility'),
    $rendererDir
)
$includeArgs = ($includeDirs | ForEach-Object { '/I"{0}"' -f $_ }) -join ' '
$compile = 'call "{0}" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /permissive- {1} "{2}" "{3}" /Fo"{4}/" /Fe:"{5}"' -f `
    $vsDevCmd, $includeArgs, $proxySource, $testSource, $outputDir, $testExe
cmd /c $compile
if ($LASTEXITCODE -ne 0) {
    throw "persistent voxel shadow-proxy test compilation failed with exit code $LASTEXITCODE"
}

& $testExe
if ($LASTEXITCODE -ne 0) {
    throw "persistent voxel shadow-proxy tests failed with exit code $LASTEXITCODE"
}

Write-Host 'Persistent voxel shadow-proxy tests passed.'
