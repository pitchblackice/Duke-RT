param(
    [string]$LaunchRoot = "",
    [string]$OverlayDir = "",
    [string]$GameRoot = "",
    [string]$SourceRoot = "",
    [string]$StatePath = "",
    [string]$LaunchVarsPath = "",
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

function Write-LaunchVars {
    param(
        [string]$Path,
        [string]$InstallRoot,
        [string]$GrpPath,
        [string]$NormalSourceRoot
    )

    $varsDir = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $varsDir)) {
        New-Item -ItemType Directory -Path $varsDir -Force | Out-Null
    }

    $lines = @(
        "@echo off",
        "set `"DUKE_RT_INSTALL_ROOT=$InstallRoot`"",
        "set `"DUKE_RT_GRP=$GrpPath`"",
        "set `"DUKE_RT_NORMAL_SOURCE=$NormalSourceRoot`""
    )
    Set-Content -LiteralPath $Path -Value ($lines -join [Environment]::NewLine) -Encoding ASCII
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

function New-DefaultState {
    return @{
        providers = @{
            world_tour_normals = @{
                prompt = "ask"
            }
            duke_world_tour = @{}
        }
    }
}

function Ensure-StateShape {
    param([hashtable]$State)

    if (-not $State.ContainsKey("providers")) {
        $State["providers"] = @{}
    }
    if (-not $State["providers"].ContainsKey("world_tour_normals")) {
        $State["providers"]["world_tour_normals"] = @{ prompt = "ask" }
    }
    if (-not $State["providers"]["world_tour_normals"].ContainsKey("prompt")) {
        $State["providers"]["world_tour_normals"]["prompt"] = "ask"
    }
    if (-not $State["providers"].ContainsKey("duke_world_tour")) {
        $State["providers"]["duke_world_tour"] = @{}
    }
    return $State
}

function Load-State {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return New-DefaultState
    }

    $raw = Get-Content -LiteralPath $Path -Raw
    if ([string]::IsNullOrWhiteSpace($raw)) {
        return New-DefaultState
    }

    $loadedObject = $raw | ConvertFrom-Json
    $loaded = ConvertTo-Hashtable -InputObject $loadedObject
    return Ensure-StateShape -State $loaded
}

function Save-State {
    param(
        [string]$Path,
        [hashtable]$State
    )

    $stateDir = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $stateDir)) {
        New-Item -ItemType Directory -Path $stateDir -Force | Out-Null
    }
    $json = $State | ConvertTo-Json -Depth 8
    Set-Content -LiteralPath $Path -Value ($json + [Environment]::NewLine) -Encoding UTF8
}

function Normalize-CandidatePath {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ""
    }

    $expanded = [Environment]::ExpandEnvironmentVariables($Path.Trim().Trim('"'))
    return [System.IO.Path]::GetFullPath($expanded)
}

function Test-DukeInstallRoot {
    param([string]$CandidateRoot)

    if ([string]::IsNullOrWhiteSpace($CandidateRoot)) {
        return $null
    }

    $full = Normalize-CandidatePath -Path $CandidateRoot
    $isFile = $false
    if (Test-Path -LiteralPath $full -PathType Leaf) {
        $name = [System.IO.Path]::GetFileName($full)
        if ($name.Equals("DUKE3D.GRP", [System.StringComparison]::OrdinalIgnoreCase)) {
            $isFile = $true
        }
    }

    $root = $full
    $grp = if ($isFile) { $full } else { Join-Path $full "DUKE3D.GRP" }
    if ($isFile) {
        $root = Split-Path -Parent $full
    }

    if (Test-Path -LiteralPath $grp -PathType Leaf) {
        return @{
            install_root = [System.IO.Path]::GetFullPath($root)
            grp_path = [System.IO.Path]::GetFullPath($grp)
        }
    }

    return $null
}

function Add-Candidate {
    param(
        [System.Collections.Generic.List[string]]$Candidates,
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }

    try {
        $full = Normalize-CandidatePath -Path $Path
        if (-not $Candidates.Contains($full)) {
            $Candidates.Add($full)
        }
    }
    catch {
    }
}

function Get-RegistryValue {
    param(
        [string]$Path,
        [string]$Name
    )

    try {
        $item = Get-ItemProperty -LiteralPath $Path -ErrorAction Stop
        return $item.$Name
    }
    catch {
        return $null
    }
}

function Get-SteamRoots {
    $roots = New-Object System.Collections.Generic.List[string]
    $steamRegistryPaths = @(
        "HKCU:\Software\Valve\Steam",
        "HKLM:\SOFTWARE\Valve\Steam",
        "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam"
    )

    foreach ($path in $steamRegistryPaths) {
        $installPath = Get-RegistryValue -Path $path -Name "SteamPath"
        Add-Candidate -Candidates $roots -Path $installPath
        $installPath = Get-RegistryValue -Path $path -Name "InstallPath"
        Add-Candidate -Candidates $roots -Path $installPath
    }

    Add-Candidate -Candidates $roots -Path "C:\Program Files (x86)\Steam"
    Add-Candidate -Candidates $roots -Path "C:\Program Files\Steam"
    return $roots
}

function Convert-SteamVdfPath {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ""
    }

    return $Path.Replace("\\", "\")
}

function Get-SteamLibraryRoots {
    $libraries = New-Object System.Collections.Generic.List[string]

    foreach ($steamRoot in Get-SteamRoots) {
        if (-not (Test-Path -LiteralPath $steamRoot -PathType Container)) {
            continue
        }

        Add-Candidate -Candidates $libraries -Path $steamRoot
        $libraryFile = Join-Path $steamRoot "steamapps\libraryfolders.vdf"
        if (-not (Test-Path -LiteralPath $libraryFile -PathType Leaf)) {
            continue
        }

        $raw = Get-Content -LiteralPath $libraryFile -Raw
        foreach ($match in [regex]::Matches($raw, '"path"\s+"([^"]+)"')) {
            Add-Candidate -Candidates $libraries -Path (Convert-SteamVdfPath -Path $match.Groups[1].Value)
        }
        foreach ($match in [regex]::Matches($raw, '"\d+"\s+"([^"]+)"')) {
            Add-Candidate -Candidates $libraries -Path (Convert-SteamVdfPath -Path $match.Groups[1].Value)
        }
    }

    return $libraries
}

function Get-AutoInstallCandidates {
    param([string]$LaunchRoot)

    $candidates = New-Object System.Collections.Generic.List[string]
    $worldTourDir = "Duke Nukem 3D Twentieth Anniversary World Tour"

    $registryInstallPaths = @(
        "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 434050",
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 434050"
    )
    foreach ($path in $registryInstallPaths) {
        Add-Candidate -Candidates $candidates -Path (Get-RegistryValue -Path $path -Name "InstallLocation")
    }

    foreach ($library in Get-SteamLibraryRoots) {
        Add-Candidate -Candidates $candidates -Path (Join-Path $library "steamapps\common\$worldTourDir")
    }

    $packageCandidates = @(
        (Join-Path $LaunchRoot "games\$worldTourDir"),
        (Join-Path $LaunchRoot "games\duke3d"),
        (Join-Path $LaunchRoot "duke3d"),
        (Join-Path $LaunchRoot $worldTourDir),
        $LaunchRoot
    )
    foreach ($candidate in $packageCandidates) {
        Add-Candidate -Candidates $candidates -Path $candidate
    }

    Add-Candidate -Candidates $candidates -Path "C:\Program Files (x86)\Steam\steamapps\common\$worldTourDir"
    Add-Candidate -Candidates $candidates -Path "C:\Program Files\Steam\steamapps\common\$worldTourDir"

    return $candidates
}

function Resolve-AutoInstallCandidate {
    param([string]$LaunchRoot)

    foreach ($candidate in Get-AutoInstallCandidates -LaunchRoot $LaunchRoot) {
        $resolved = Test-DukeInstallRoot -CandidateRoot $candidate
        if ($resolved) {
            return $resolved
        }
    }

    return $null
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

function Prompt-ForInstallRoot {
    while ($true) {
        $reply = Read-Host "Enter the folder containing DUKE3D.GRP, or leave blank to cancel"
        if ($null -eq $reply -or [string]::IsNullOrWhiteSpace($reply)) {
            return $null
        }

        try {
            $resolved = Test-DukeInstallRoot -CandidateRoot $reply
            if ($resolved) {
                return $resolved
            }
        }
        catch {
        }

        Write-Info "That folder does not contain DUKE3D.GRP."
    }
}

function Save-DukeInstallState {
    param(
        [hashtable]$State,
        [hashtable]$ResolvedInstall
    )

    $installState = $State["providers"]["duke_world_tour"]
    $installState["install_root"] = $ResolvedInstall["install_root"]
    $installState["grp_path"] = $ResolvedInstall["grp_path"]
    $installState["last_verified_utc"] = [DateTime]::UtcNow.ToString("o")
}

function Resolve-DukeInstall {
    param(
        [hashtable]$State,
        [string]$ExplicitGameRoot,
        [string]$LaunchRoot
    )

    $installState = $State["providers"]["duke_world_tour"]

    if ($ExplicitGameRoot) {
        $explicit = Test-DukeInstallRoot -CandidateRoot $ExplicitGameRoot
        if (-not $explicit) {
            throw "The path passed to -GameRoot does not contain DUKE3D.GRP: $ExplicitGameRoot"
        }
        Save-DukeInstallState -State $State -ResolvedInstall $explicit
        return $explicit
    }

    if ($installState.ContainsKey("grp_path")) {
        $saved = Test-DukeInstallRoot -CandidateRoot $installState["grp_path"]
        if ($saved) {
            return $saved
        }

        $installState.Remove("install_root")
        $installState.Remove("grp_path")
    }

    $candidate = Resolve-AutoInstallCandidate -LaunchRoot $LaunchRoot
    if ($candidate) {
        Write-Host ""
        Write-Info "Found Duke Nukem 3D: Twentieth Anniversary World Tour at:"
        Write-Info "  $($candidate["install_root"])"
        Write-Host ""
        if (Prompt-YesNo -Question "Use this install") {
            Save-DukeInstallState -State $State -ResolvedInstall $candidate
            return $candidate
        }
    }

    Write-Host ""
    Write-Info "A valid Duke Nukem 3D install is required to launch Duke-RT."
    $manual = Prompt-ForInstallRoot
    if ($manual) {
        Save-DukeInstallState -State $State -ResolvedInstall $manual
        return $manual
    }

    throw "No valid DUKE3D.GRP path was provided."
}

function Resolve-SourceRoot {
    param(
        [string]$ExplicitSourceRoot,
        [string]$InstallRoot
    )

    if ($ExplicitSourceRoot) {
        $sourceRootPath = Normalize-CandidatePath -Path $ExplicitSourceRoot
        if (Test-Path -LiteralPath $sourceRootPath -PathType Container) {
            return $sourceRootPath
        }
        return $null
    }

    if ($InstallRoot) {
        $candidate = Join-Path $InstallRoot "textures"
        if (Test-Path -LiteralPath $candidate -PathType Container) {
            return [System.IO.Path]::GetFullPath($candidate)
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

function Invoke-NormalImport {
    param(
        [string]$LaunchRoot,
        [string]$OverlayDir,
        [string]$SourceRoot,
        [hashtable]$State
    )

    $providerState = $State["providers"]["world_tour_normals"]
    $normalTargetDir = Get-FullPathSafe -Base $OverlayDir -Child "materials\normalmaps\auto"
    Ensure-WithinRoot -RootPath $LaunchRoot -CandidatePath $normalTargetDir -Label "Normals target"

    if ($No) {
        $providerState["prompt"] = "skip"
        if (-not $Quiet) {
            Write-Info "Skipping World Tour normals by user request."
        }
        return
    }

    if (-not $SourceRoot) {
        if (-not $Quiet) {
            Write-Info "World Tour textures source not found; skipping optional normals import."
        }
        return
    }

    $sourceFiles = Get-NormalSourceFiles -Root $SourceRoot
    if ($sourceFiles.Count -eq 0) {
        if (-not $Quiet) {
            Write-Info "No '*_n.bmp' normals were found under '$SourceRoot'; skipping import."
        }
        return
    }

    $shouldPrompt = $Ask -or (-not $Yes -and -not $Force -and $providerState["prompt"] -ne "always-yes")
    if (-not $Ask -and $providerState["prompt"] -eq "skip") {
        if (-not $Quiet) {
            Write-Info "World Tour normals import is disabled in local content preferences; skipping."
        }
        return
    }

    $consent = $true
    if (-not $Yes -and -not $Force -and $shouldPrompt) {
        Write-Host ""
        Write-Info "Optional World Tour normal maps were found."
        Write-Info "Optional source: $SourceRoot"
        Write-Info "This step copies local '*_n.bmp' normal maps, converts them to PNG, and writes them into:"
        Write-Info "  $normalTargetDir"
        Write-Info "The copied files stay on this machine and are not redistributed automatically."
        Write-Host ""
        $consent = Prompt-YesNo -Question "Import World Tour normals into the mounted overlay now?"

        if ($consent) {
            $providerState["prompt"] = "always-yes"
        } else {
            $providerState["prompt"] = "skip"
        }
    }

    if (-not $consent) {
        if (-not $Quiet) {
            Write-Info "World Tour normals import declined."
        }
        return
    }

    if (-not (Test-Path -LiteralPath $OverlayDir)) {
        New-Item -ItemType Directory -Path $OverlayDir | Out-Null
    }
    if (-not (Test-Path -LiteralPath $normalTargetDir)) {
        New-Item -ItemType Directory -Path $normalTargetDir -Force | Out-Null
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
        $destinationPath = Get-FullPathSafe -Base $normalTargetDir -Child $destinationName
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

    $providerState["last_source_root"] = $SourceRoot
    $providerState["last_scan_utc"] = [DateTime]::UtcNow.ToString("o")
    $providerState["last_found_count"] = $sourceFiles.Count
    $providerState["last_converted_count"] = $converted
    $providerState["last_skipped_count"] = $skipped

    if (-not $Quiet) {
        Write-Info "World Tour normals import complete: converted=$converted skipped=$skipped found=$($sourceFiles.Count)"
    }

    if ($failures.Count -gt 0) {
        foreach ($failure in $failures) {
            Write-Warning $failure
        }
        Write-Warning "World Tour normals import had failures; launch will continue."
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

if (-not $LaunchVarsPath) {
    $LaunchVarsPath = Get-FullPathSafe -Base $LaunchRoot -Child "generated-content\state\launch-duke-rt-vars.cmd"
} elseif (-not [System.IO.Path]::IsPathRooted($LaunchVarsPath)) {
    $LaunchVarsPath = Get-FullPathSafe -Base $LaunchRoot -Child $LaunchVarsPath
} else {
    $LaunchVarsPath = [System.IO.Path]::GetFullPath($LaunchVarsPath)
}

Ensure-WithinRoot -RootPath $LaunchRoot -CandidatePath $OverlayDir -Label "Overlay"
Ensure-WithinRoot -RootPath $LaunchRoot -CandidatePath $StatePath -Label "State"
Ensure-WithinRoot -RootPath $LaunchRoot -CandidatePath $LaunchVarsPath -Label "Launch vars"

$state = Load-State -Path $StatePath
$resolvedInstall = Resolve-DukeInstall -State $state -ExplicitGameRoot $GameRoot -LaunchRoot $LaunchRoot
$resolvedSourceRoot = Resolve-SourceRoot -ExplicitSourceRoot $SourceRoot -InstallRoot $resolvedInstall["install_root"]

try {
    Invoke-NormalImport -LaunchRoot $LaunchRoot -OverlayDir $OverlayDir -SourceRoot $resolvedSourceRoot -State $state
}
catch {
    Write-Warning "World Tour normals import failed: $($_.Exception.Message)"
    Write-Warning "Launch will continue without imported commercial normals."
}

Save-DukeInstallState -State $state -ResolvedInstall $resolvedInstall
Save-State -Path $StatePath -State $state
Write-LaunchVars -Path $LaunchVarsPath -InstallRoot $resolvedInstall["install_root"] -GrpPath $resolvedInstall["grp_path"] -NormalSourceRoot $resolvedSourceRoot

if (-not $Quiet) {
    Write-Info "Using Duke3D.grp at: $($resolvedInstall["grp_path"])"
}

exit 0
