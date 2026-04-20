param(
    [string]$LaunchRoot = "",
    [string]$OverlayDir = "",
    [string]$SourceRoot = "",
    [string]$StatePath = "",
    [switch]$Yes,
    [switch]$No,
    [switch]$Ask,
    [switch]$Force,
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"

function Write-Info {
    param([string]$Message)
    Write-Host "[duke-rt] $Message"
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
        throw "$Label path '$candidate' is outside launch root '$root'."
    }
}

function Load-State {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return @{
            providers = @{
                world_tour_normals = @{
                    prompt = "ask"
                }
            }
        }
    }

    $raw = Get-Content -LiteralPath $Path -Raw
    if ([string]::IsNullOrWhiteSpace($raw)) {
        return @{
            providers = @{
                world_tour_normals = @{
                    prompt = "ask"
                }
            }
        }
    }

    $loadedObject = $raw | ConvertFrom-Json
    $loaded = ConvertTo-Hashtable -InputObject $loadedObject
    if (-not $loaded.ContainsKey("providers")) {
        $loaded["providers"] = @{}
    }
    if (-not $loaded["providers"].ContainsKey("world_tour_normals")) {
        $loaded["providers"]["world_tour_normals"] = @{ prompt = "ask" }
    }
    if (-not $loaded["providers"]["world_tour_normals"].ContainsKey("prompt")) {
        $loaded["providers"]["world_tour_normals"]["prompt"] = "ask"
    }
    return $loaded
}

function ConvertTo-Hashtable {
    param([Parameter(Mandatory = $true)]$InputObject)

    if ($null -eq $InputObject) {
        return $null
    }

    if ($InputObject -is [System.Collections.IDictionary]) {
        $table = @{}
        foreach ($key in $InputObject.Keys) {
            $table[$key] = ConvertTo-Hashtable -InputObject $InputObject[$key]
        }
        return $table
    }

    if (($InputObject -is [System.Collections.IEnumerable]) -and -not ($InputObject -is [string])) {
        $items = @()
        foreach ($item in $InputObject) {
            $items += ,(ConvertTo-Hashtable -InputObject $item)
        }
        return $items
    }

    if ($InputObject -is [pscustomobject]) {
        $table = @{}
        foreach ($property in $InputObject.PSObject.Properties) {
            $table[$property.Name] = ConvertTo-Hashtable -InputObject $property.Value
        }
        return $table
    }

    return $InputObject
}

function Save-State {
    param(
        [string]$Path,
        [hashtable]$State
    )

    $stateDir = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $stateDir)) {
        New-Item -ItemType Directory -Path $stateDir | Out-Null
    }
    $json = $State | ConvertTo-Json -Depth 8
    Set-Content -LiteralPath $Path -Value ($json + [Environment]::NewLine) -Encoding UTF8
}

function Resolve-SourceRoot {
    param([string]$ExplicitSourceRoot)

    if ($ExplicitSourceRoot) {
        return [System.IO.Path]::GetFullPath($ExplicitSourceRoot)
    }

    $candidates = @(
        "C:\Program Files (x86)\Steam\steamapps\common\Duke Nukem 3D Twentieth Anniversary World Tour\textures",
        "C:\Program Files\Steam\steamapps\common\Duke Nukem 3D Twentieth Anniversary World Tour\textures"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    return $null
}

function Get-NormalSourceFiles {
    param([string]$Root)

    if (-not $Root -or -not (Test-Path -LiteralPath $Root)) {
        return @()
    }

    return @(Get-ChildItem -LiteralPath $Root -Recurse -File -Filter *_n.bmp |
        Where-Object {
            $_.Directory.Name -match '^TILES\d+$' -and
            $_.BaseName -match '^(?<tile>\d+)_n$'
        } |
        Sort-Object FullName)
}

function Convert-BmpToPng {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourcePath,
        [Parameter(Mandatory = $true)]
        [string]$DestinationPath
    )

    Add-Type -AssemblyName System.Drawing

    $bitmap = $null
    try {
        $bitmap = New-Object System.Drawing.Bitmap($SourcePath)
        $bitmap.Save($DestinationPath, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        if ($bitmap) {
            $bitmap.Dispose()
        }
    }
}

function Prompt-YesNo {
    param([string]$Question)

    while ($true) {
        $reply = Read-Host "$Question [Y/N]"
        if ($null -eq $reply) {
            continue
        }

        switch ($reply.Trim().ToUpperInvariant()) {
            "Y" { return $true }
            "YES" { return $true }
            "N" { return $false }
            "NO" { return $false }
        }
    }
}

if (-not $LaunchRoot) {
    $LaunchRoot = Get-Location
}
$LaunchRoot = [System.IO.Path]::GetFullPath($LaunchRoot)

if (-not $OverlayDir) {
    $OverlayDir = Get-FullPathSafe -Base $LaunchRoot -Child "default-overlay"
} elseif (-not [System.IO.Path]::IsPathRooted($OverlayDir)) {
    $OverlayDir = Get-FullPathSafe -Base $LaunchRoot -Child $OverlayDir
} else {
    $OverlayDir = [System.IO.Path]::GetFullPath($OverlayDir)
}

if (-not $StatePath) {
    $StatePath = Get-FullPathSafe -Base $LaunchRoot -Child "generated-content\state\content-preferences.json"
} elseif (-not [System.IO.Path]::IsPathRooted($StatePath)) {
    $StatePath = Get-FullPathSafe -Base $LaunchRoot -Child $StatePath
} else {
    $StatePath = [System.IO.Path]::GetFullPath($StatePath)
}

Ensure-WithinRoot -RootPath $LaunchRoot -CandidatePath $OverlayDir -Label "Overlay"
Ensure-WithinRoot -RootPath $LaunchRoot -CandidatePath $StatePath -Label "State"

$NormalTargetDir = Get-FullPathSafe -Base $OverlayDir -Child "materials\normalmaps\auto"
Ensure-WithinRoot -RootPath $LaunchRoot -CandidatePath $NormalTargetDir -Label "Normals target"

$resolvedSourceRoot = Resolve-SourceRoot -ExplicitSourceRoot $SourceRoot
$state = Load-State -Path $StatePath
$providerState = $state["providers"]["world_tour_normals"]

if ($No) {
    $providerState["prompt"] = "skip"
    Save-State -Path $StatePath -State $state
    if (-not $Quiet) {
        Write-Info "Skipping World Tour normals by user request."
    }
    exit 0
}

if (-not $resolvedSourceRoot) {
    if (-not $Quiet) {
        Write-Info "World Tour textures source not found; skipping optional normals import."
    }
    exit 0
}

$sourceFiles = Get-NormalSourceFiles -Root $resolvedSourceRoot
if ($sourceFiles.Count -eq 0) {
    if (-not $Quiet) {
        Write-Info "No '*_n.bmp' normals were found under '$resolvedSourceRoot'; skipping import."
    }
    exit 0
}

$shouldPrompt = $Ask -or (-not $Yes -and -not $Force -and $providerState["prompt"] -ne "always-yes")
if (-not $Ask -and $providerState["prompt"] -eq "skip") {
    if (-not $Quiet) {
        Write-Info "World Tour normals import is disabled in local content preferences; skipping."
    }
    exit 0
}

$consent = $true
if (-not $Yes -and -not $Force -and $shouldPrompt) {
    Write-Host ""
    Write-Info "An owned Duke Nukem 3D: Twentieth Anniversary World Tour install was found."
    Write-Info "Optional source: $resolvedSourceRoot"
    Write-Info "This step copies local '*_n.bmp' normal maps, converts them to PNG, and writes them into:"
    Write-Info "  $NormalTargetDir"
    Write-Info "The copied files stay on this machine and are not redistributed automatically."
    Write-Host ""
    $consent = Prompt-YesNo -Question "Import World Tour normals into the mounted overlay now?"

    if ($consent) {
        $providerState["prompt"] = "always-yes"
    } else {
        $providerState["prompt"] = "skip"
    }
    Save-State -Path $StatePath -State $state
}

if (-not $consent) {
    if (-not $Quiet) {
        Write-Info "World Tour normals import declined."
    }
    exit 0
}

if (-not (Test-Path -LiteralPath $OverlayDir)) {
    New-Item -ItemType Directory -Path $OverlayDir | Out-Null
}
if (-not (Test-Path -LiteralPath $NormalTargetDir)) {
    New-Item -ItemType Directory -Path $NormalTargetDir -Force | Out-Null
}

$converted = 0
$skipped = 0
$failures = New-Object System.Collections.Generic.List[string]

foreach ($file in $sourceFiles) {
    $match = [regex]::Match($file.BaseName, '^(?<tile>\d+)_n$')
    if (-not $match.Success) {
        $skipped++
        continue
    }

    $tileNumber = [int]$match.Groups["tile"].Value
    $destinationName = "#{0:D5}.png" -f $tileNumber
    $destinationPath = Get-FullPathSafe -Base $NormalTargetDir -Child $destinationName
    Ensure-WithinRoot -RootPath $LaunchRoot -CandidatePath $destinationPath -Label "Converted normal"

    try {
        if ((-not $Force) -and (Test-Path -LiteralPath $destinationPath)) {
            $sourceTime = $file.LastWriteTimeUtc
            $destTime = (Get-Item -LiteralPath $destinationPath).LastWriteTimeUtc
            if ($destTime -ge $sourceTime) {
                $skipped++
                continue
            }
        }

        Convert-BmpToPng -SourcePath $file.FullName -DestinationPath $destinationPath
        $converted++
    }
    catch {
        $failures.Add("$($file.FullName): $($_.Exception.Message)")
    }
}

$providerState["last_source_root"] = $resolvedSourceRoot
$providerState["last_scan_utc"] = [DateTime]::UtcNow.ToString("o")
$providerState["last_found_count"] = $sourceFiles.Count
$providerState["last_converted_count"] = $converted
$providerState["last_skipped_count"] = $skipped
Save-State -Path $StatePath -State $state

if (-not $Quiet) {
    Write-Info "World Tour normals import complete: converted=$converted skipped=$skipped found=$($sourceFiles.Count)"
}

if ($failures.Count -gt 0) {
    foreach ($failure in $failures) {
        Write-Warning $failure
    }
    exit 1
}

exit 0
