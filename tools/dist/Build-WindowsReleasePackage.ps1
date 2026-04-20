param(
    [string]$VsDevCmd = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
    [string]$ZMusicSourceDir = "",
    [string]$ZMusicBuildDir = "",
    [string]$RazeBuildDir = "",
    [string]$PackageDir = "",
    [string]$ZipPath = "",
    [switch]$SkipBuild,
    [switch]$SkipZip
)

$ErrorActionPreference = "Stop"

function Write-Info {
    param([string]$Message)
    Write-Host "[release-package] $Message"
}

function Get-FullPathSafe {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Base,
        [Parameter(Mandatory = $true)]
        [string]$Child
    )

    return [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($Base, $Child))
}

function Ensure-WithinRoot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RootPath,
        [Parameter(Mandatory = $true)]
        [string]$CandidatePath,
        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    $root = [System.IO.Path]::GetFullPath($RootPath)
    $candidate = [System.IO.Path]::GetFullPath($CandidatePath)
    $rootWithSlash = $root.TrimEnd('\') + '\'
    if ($candidate -ne $root -and -not $candidate.StartsWith($rootWithSlash, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label path '$candidate' is outside root '$root'."
    }
}

function Invoke-CmdChecked {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Commands,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    Write-Info $Description
    $tempFile = Join-Path ([System.IO.Path]::GetTempPath()) ("raze-release-" + [System.Guid]::NewGuid().ToString("N") + ".cmd")
    $batchLines = @(
        "@echo off",
        "setlocal"
    )
    foreach ($command in $Commands) {
        $batchLines += $command
        $batchLines += "if errorlevel 1 exit /b %errorlevel%"
    }

    Set-Content -LiteralPath $tempFile -Value ($batchLines -join [Environment]::NewLine) -Encoding ASCII
    try {
        & $tempFile
        if ($LASTEXITCODE -ne 0) {
            throw "$Description failed with exit code $LASTEXITCODE."
        }
    }
    finally {
        if (Test-Path -LiteralPath $tempFile) {
            Remove-Item -LiteralPath $tempFile -Force
        }
    }
}

function Copy-RequiredFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourcePath,
        [Parameter(Mandatory = $true)]
        [string]$DestinationPath
    )

    if (-not (Test-Path -LiteralPath $SourcePath)) {
        throw "Required file not found: $SourcePath"
    }

    $destinationDir = Split-Path -Parent $DestinationPath
    if (-not (Test-Path -LiteralPath $destinationDir)) {
        New-Item -ItemType Directory -Path $destinationDir -Force | Out-Null
    }

    Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -Force
}

function Copy-OptionalDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourcePath,
        [Parameter(Mandatory = $true)]
        [string]$DestinationPath
    )

    if (Test-Path -LiteralPath $SourcePath) {
        Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -Recurse -Force
    }
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $scriptRoot "..\.."))
$packageRootDir = Join-Path $repoRoot "out\release"

if (-not $ZMusicSourceDir) {
    $ZMusicSourceDir = Join-Path $repoRoot "build\zmusic"
}
if (-not $ZMusicBuildDir) {
    $ZMusicBuildDir = Join-Path $repoRoot "build\zmusic\build-ninja-ovl2"
}
if (-not $RazeBuildDir) {
    $RazeBuildDir = Join-Path $repoRoot "build\terminal-release"
}
if (-not $PackageDir) {
    $PackageDir = Join-Path $packageRootDir "raze-windows-release"
}
if (-not $ZipPath) {
    $ZipPath = Join-Path $packageRootDir "raze-windows-release.zip"
}

$ZMusicSourceDir = [System.IO.Path]::GetFullPath($ZMusicSourceDir)
$ZMusicBuildDir = [System.IO.Path]::GetFullPath($ZMusicBuildDir)
$RazeBuildDir = [System.IO.Path]::GetFullPath($RazeBuildDir)
$PackageDir = [System.IO.Path]::GetFullPath($PackageDir)
$ZipPath = [System.IO.Path]::GetFullPath($ZipPath)

Ensure-WithinRoot -RootPath $repoRoot -CandidatePath $ZMusicSourceDir -Label "ZMusic source"
Ensure-WithinRoot -RootPath $repoRoot -CandidatePath $ZMusicBuildDir -Label "ZMusic build"
Ensure-WithinRoot -RootPath $repoRoot -CandidatePath $RazeBuildDir -Label "Raze build"
Ensure-WithinRoot -RootPath $repoRoot -CandidatePath $PackageDir -Label "Package"
Ensure-WithinRoot -RootPath $repoRoot -CandidatePath $ZipPath -Label "Zip"

if (-not $SkipBuild) {
    throw "Build-WindowsReleasePackage.ps1 stages an existing Release build. Use tools\\dist\\Build-WindowsReleasePackage.cmd for the full build-and-package flow."
}

$zmusicLib = Join-Path $ZMusicBuildDir "source\zmusiclite.lib"
$zmusicIncludeDir = Join-Path $repoRoot "build\zmusic\include"
$packageLauncher = Join-Path $repoRoot "package\windows\launch-duke-rt.cmd"
$prepareNormals = Join-Path $repoRoot "tools\dist\Prepare-CommercialNormals.ps1"
$releaseOverlay = Join-Path $repoRoot "release-overlay"

$requiredBuildFiles = @(
    (Join-Path $RazeBuildDir "raze.exe"),
    (Join-Path $RazeBuildDir "raze.pk3"),
    (Join-Path $RazeBuildDir "OpenAL32.dll"),
    (Join-Path $RazeBuildDir "zmusiclite.dll")
)

foreach ($requiredFile in $requiredBuildFiles) {
    if (-not (Test-Path -LiteralPath $requiredFile)) {
        throw "Required Release build output was not found: $requiredFile"
    }
}

if (-not (Test-Path -LiteralPath $releaseOverlay)) {
    throw "release-overlay was not found: $releaseOverlay"
}

if (-not (Test-Path -LiteralPath $packageRootDir)) {
    New-Item -ItemType Directory -Path $packageRootDir -Force | Out-Null
}

if (Test-Path -LiteralPath $PackageDir) {
    Write-Info "Removing existing package dir $PackageDir"
    Remove-Item -LiteralPath $PackageDir -Recurse -Force
}

New-Item -ItemType Directory -Path $PackageDir -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $PackageDir "tools\dist") -Force | Out-Null

Write-Info "Copying release binaries and staged runtime files"
Copy-RequiredFile -SourcePath (Join-Path $RazeBuildDir "raze.exe") -DestinationPath (Join-Path $PackageDir "raze.exe")
Copy-RequiredFile -SourcePath (Join-Path $RazeBuildDir "raze.pk3") -DestinationPath (Join-Path $PackageDir "raze.pk3")

$runtimeDlls = Get-ChildItem -LiteralPath $RazeBuildDir -File -Filter *.dll | Sort-Object Name
foreach ($runtimeDll in $runtimeDlls) {
    Copy-RequiredFile -SourcePath $runtimeDll.FullName -DestinationPath (Join-Path $PackageDir $runtimeDll.Name)
}

Copy-OptionalDirectory -SourcePath (Join-Path $RazeBuildDir "AgilitySDK") -DestinationPath (Join-Path $PackageDir "AgilitySDK")
Copy-OptionalDirectory -SourcePath (Join-Path $RazeBuildDir "shaders") -DestinationPath (Join-Path $PackageDir "shaders")
Copy-OptionalDirectory -SourcePath (Join-Path $RazeBuildDir "nri_shaders") -DestinationPath (Join-Path $PackageDir "nri_shaders")
Copy-OptionalDirectory -SourcePath (Join-Path $RazeBuildDir "soundfonts") -DestinationPath (Join-Path $PackageDir "soundfonts")

Write-Info "Copying release launcher assets and overlay"
Copy-RequiredFile -SourcePath $packageLauncher -DestinationPath (Join-Path $PackageDir "launch-duke-rt.cmd")
Copy-RequiredFile -SourcePath $prepareNormals -DestinationPath (Join-Path $PackageDir "tools\dist\Prepare-CommercialNormals.ps1")
Copy-Item -LiteralPath $releaseOverlay -Destination (Join-Path $PackageDir "release-overlay") -Recurse -Force

Get-ChildItem -LiteralPath $PackageDir -Recurse -File -Filter *.pdb | Remove-Item -Force

if (Test-Path -LiteralPath $ZipPath) {
    Remove-Item -LiteralPath $ZipPath -Force
}

if (-not $SkipZip) {
    Write-Info "Creating zip archive $ZipPath"
    Compress-Archive -Path $PackageDir -DestinationPath $ZipPath -CompressionLevel Optimal
}

$packagedRequiredFiles = @(
    (Join-Path $PackageDir "raze.exe"),
    (Join-Path $PackageDir "raze.pk3"),
    (Join-Path $PackageDir "launch-duke-rt.cmd"),
    (Join-Path $PackageDir "tools\dist\Prepare-CommercialNormals.ps1"),
    (Join-Path $PackageDir "release-overlay"),
    (Join-Path $PackageDir "OpenAL32.dll"),
    (Join-Path $PackageDir "zmusiclite.dll")
)

foreach ($packagedFile in $packagedRequiredFiles) {
    if (-not (Test-Path -LiteralPath $packagedFile)) {
        throw "Packaged output is missing required entry: $packagedFile"
    }
}

$remainingPdbs = Get-ChildItem -LiteralPath $PackageDir -Recurse -File -Filter *.pdb
if ($remainingPdbs) {
    throw "Packaged output still contains PDB files."
}

Write-Info "Package ready:"
Write-Info "  folder: $PackageDir"
if (-not $SkipZip) {
    Write-Info "  zip:    $ZipPath"
}
