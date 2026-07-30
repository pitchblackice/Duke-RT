param(
    [Parameter(Mandatory = $true)]
    [string]$LogDirectory,
    [string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Read-KeyValuePairs {
    param([Parameter(Mandatory = $true)][string]$Line)
    $pairs = @{}
    foreach ($match in [regex]::Matches($Line, '([A-Za-z_][A-Za-z0-9_]*)=("[^"]*"|\([^\)]*\)|[^\s]+)')) {
        $pairs[$match.Groups[1].Value] = $match.Groups[2].Value.Trim('"')
    }
    return $pairs
}

function Get-UInt64 {
    param([hashtable]$Pairs, [string]$Name)
    if (-not $Pairs.ContainsKey($Name)) { return [uint64]0 }
    return [uint64]$Pairs[$Name]
}

function Add-Maximum {
    param([hashtable]$Table, [string]$Name, [uint64]$Value)
    if (-not $Table.ContainsKey($Name) -or [uint64]$Table[$Name] -lt $Value) { $Table[$Name] = $Value }
}

$requiredMapRules = @("FarWindow", "NearWindow", "RoofLong", "RoofSquare")
$forbiddenPatterns = @(
    "Device removed", "device lost", "DXGI_ERROR_DEVICE", "QueueSubmit failed",
    "NRI render failed", "validation error", "assertion failed", "Fatal error",
    "warptocoords: must be in a level"
)
$directory = Resolve-Path -LiteralPath $LogDirectory -ErrorAction Stop
$logs = @(Get-ChildItem -LiteralPath $directory.Path -Filter "*.log" -File | Sort-Object Name)
if ($logs.Count -eq 0) { throw "no .log files found in $($directory.Path)" }

$manifestFailures = 0
$manifestPath = Join-Path $directory.Path "manifest.json"
if (Test-Path -LiteralPath $manifestPath) {
    $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    foreach ($run in @($manifest.runs)) {
        if ($run.timedOut -or $null -eq $run.exitCode -or [int]$run.exitCode -ne 0) { $manifestFailures++ }
    }
}

$summaries = @()
$errors = [System.Collections.Generic.List[string]]::new()
foreach ($log in $logs) {
    $direction = if ($log.BaseName.StartsWith("reverse")) { "reverse" } else { "forward" }
    $markers = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $mapRules = @{}
    $actor = @{}
    $actor["observed"] = [uint64]0
    $actor["appearance_ready"] = [uint64]0
    $actor["appearance_seen"] = [uint64]0
    $actor["activation_latched"] = [uint64]0
    $actor["cadence_active"] = [uint64]0
    $actor["emitted"] = [uint64]0
    $targetActor = @{ actor = [uint64]405; observed = [uint64]0; appearance_ready = [uint64]0; appearance_seen = [uint64]0; activation_latched = [uint64]0; cadence_active = [uint64]0; emitted = [uint64]0 }
    $sourceIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $sourceRows = @{}
    $closures = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $minimumFree = [uint64]::MaxValue
    $maximumOccupied = [uint64]0
    $brickCapacity = [uint64]0
    $forbidden = 0

    foreach ($line in [System.IO.File]::ReadLines($log.FullName)) {
        foreach ($pattern in $forbiddenPatterns) {
            if ($line.IndexOf($pattern, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) { $forbidden++; break }
        }
        if ($line.Contains("NRI_SMOKE_PRESSURE checkpoint=")) {
            $pairs = Read-KeyValuePairs -Line $line
            if ($pairs.ContainsKey("checkpoint")) { [void]$markers.Add($pairs["checkpoint"]) }
        }
        elseif ($line.StartsWith("NRI PT smoke emitter: event=map-frame-summary")) {
            $pairs = Read-KeyValuePairs -Line $line
            if ($pairs.ContainsKey("rule")) {
                $rule = $pairs["rule"]
                if (-not $mapRules.ContainsKey($rule)) { $mapRules[$rule] = @{ active = 0; emitted = 0; footprint = 0 } }
                Add-Maximum -Table $mapRules[$rule] -Name "active" -Value (Get-UInt64 $pairs "active")
                Add-Maximum -Table $mapRules[$rule] -Name "emitted" -Value (Get-UInt64 $pairs "emitted")
                Add-Maximum -Table $mapRules[$rule] -Name "footprint" -Value (Get-UInt64 $pairs "footprint_bricks")
            }
        }
        elseif ($line.StartsWith("NRI PT smoke emitter: event=frame-summary")) {
            $pairs = Read-KeyValuePairs -Line $line
            if ($pairs.ContainsKey("rule") -and $pairs["rule"] -eq "duke_fire_sustained") {
                foreach ($name in @("observed", "appearance_ready", "appearance_seen", "activation_latched", "cadence_active")) {
                    Add-Maximum -Table $actor -Name $name -Value (Get-UInt64 $pairs $name)
                }
                $actor["emitted"] = [uint64]$actor["emitted"] + (Get-UInt64 $pairs "emitted")
            }
        }
        elseif ($line.StartsWith("NRI PT smoke emitter: event=actor-authority")) {
            $pairs = Read-KeyValuePairs -Line $line
            if ($pairs.ContainsKey("rule") -and $pairs["rule"] -eq "duke_fire_sustained" -and
                (Get-UInt64 $pairs "actor") -eq 405) {
                $targetActor["observed"] = 1
                foreach ($name in @("appearance_ready", "appearance_seen", "activation_latched", "cadence_active")) {
                    Add-Maximum -Table $targetActor -Name $name -Value (Get-UInt64 $pairs $name)
                }
                $targetActor["emitted"] = [uint64]$targetActor["emitted"] + (Get-UInt64 $pairs "emitted_now")
            }
        }
        elseif (($line.StartsWith("NRI PT smoke emitter: event=interval") -or $line.StartsWith("NRI PT smoke emitter: event=spawn"))) {
            $pairs = Read-KeyValuePairs -Line $line
            if ($pairs.ContainsKey("rule") -and $pairs["rule"] -eq "duke_fire_sustained" -and
                (Get-UInt64 $pairs "actor") -eq 405 -and $pairs.ContainsKey("source_id")) {
                [void]$sourceIds.Add($pairs["source_id"])
            }
        }
        elseif ($line.StartsWith("NRI PT smoke grid source:") -or $line.StartsWith("PERF pt smoke grid source NRI:")) {
            $pairs = Read-KeyValuePairs -Line $line
            if (-not $pairs.ContainsKey("source_id")) { continue }
            $id = $pairs["source_id"]
            if (-not $sourceRows.ContainsKey($id)) {
                $sourceRows[$id] = @{
                    rows = [uint64]0; requested = [uint64]0; existing = [uint64]0; admitted = [uint64]0
                    capacity = [uint64]0; probe = [uint64]0; invalid = [uint64]0; deposited = [uint64]0
                    firstAdmissionFrame = $null; firstMediumFrame = $null
                }
            }
            $row = $sourceRows[$id]
            $row["rows"]++
            foreach ($mapping in @(
                @("requested", "requested_bricks", "requested"), @("existing", "existing_hits", "existing"),
                @("admitted", "admitted_new", "admitted"), @("capacity", "rejected_capacity", "rejected_capacity"),
                @("probe", "rejected_probe", "rejected_probe"), @("invalid", "rejected_invalid", "rejected_invalid"),
                @("deposited", "deposited_mass_q", "deposited_mass_q")
            )) {
                $value = if ($pairs.ContainsKey($mapping[1])) { Get-UInt64 $pairs $mapping[1] } else { Get-UInt64 $pairs $mapping[2] }
                $row[$mapping[0]] = [uint64]$row[$mapping[0]] + $value
            }
            $frame = if ($pairs.ContainsKey("frame")) { [uint64]$pairs["frame"] } else { $null }
            if ($null -eq $row["firstAdmissionFrame"] -and ((Get-UInt64 $pairs "admitted_new") -gt 0 -or (Get-UInt64 $pairs "admitted") -gt 0 -or (Get-UInt64 $pairs "existing_hits") -gt 0 -or (Get-UInt64 $pairs "existing") -gt 0)) {
                $row["firstAdmissionFrame"] = $frame
            }
            if ($null -eq $row["firstMediumFrame"] -and (Get-UInt64 $pairs "deposited_mass_q") -gt 0) { $row["firstMediumFrame"] = $frame }
        }
        elseif ($line.StartsWith("NRI PT smoke grid status:") -or $line.StartsWith("PERF pt smoke work NRI:")) {
            $pairs = Read-KeyValuePairs -Line $line
            $validGrid = ($line.StartsWith("NRI PT smoke grid status:") -and $pairs.ContainsKey("gpu_stats") -and $pairs["gpu_stats"] -eq "valid") -or
                ($line.StartsWith("PERF pt smoke work NRI:") -and $pairs.ContainsKey("grid_valid") -and $pairs["grid_valid"] -eq "1")
            if (-not $validGrid) { continue }
            $free = if ($pairs.ContainsKey("free")) { Get-UInt64 $pairs "free" } else { Get-UInt64 $pairs "grid_free" }
            $occupied = if ($pairs.ContainsKey("occupied")) { Get-UInt64 $pairs "occupied" } else { Get-UInt64 $pairs "grid_occupied" }
            if ($free -lt $minimumFree) { $minimumFree = $free }
            if ($occupied -gt $maximumOccupied) { $maximumOccupied = $occupied }
            if ($pairs.ContainsKey("bricks")) { $brickCapacity = Get-UInt64 $pairs "bricks" }
        }
        elseif ($line.StartsWith("NRI PT smoke admission:") -or $line.StartsWith("PERF pt smoke admission NRI:")) {
            $pairs = Read-KeyValuePairs -Line $line
            if ($pairs.ContainsKey("closure")) { [void]$closures.Add($pairs["closure"]) }
        }
    }

    $missingRules = @($requiredMapRules | Where-Object { -not $mapRules.ContainsKey($_) -or [uint64]$mapRules[$_]["active"] -eq 0 })
    $requiredMarkers = if ($direction -eq "reverse") { @("settled", "fire-first", "roof", "windows", "fire-return", "complete") } else { @("settled", "windows", "roof", "fire", "complete") }
    $missingMarkers = @($requiredMarkers | Where-Object { -not $markers.Contains($_) })

    $dukeRows = @()
    $totals = @{ rows = [uint64]0; requested = [uint64]0; existing = [uint64]0; admitted = [uint64]0; capacity = [uint64]0; probe = [uint64]0; invalid = [uint64]0; deposited = [uint64]0 }
    foreach ($id in $sourceIds) {
        if (-not $sourceRows.ContainsKey($id)) { continue }
        $row = $sourceRows[$id]
        $dukeRows += [pscustomobject]@{ sourceId = $id; stats = $row }
        foreach ($name in $totals.Keys.Clone()) { $totals[$name] = [uint64]$totals[$name] + [uint64]$row[$name] }
    }

    $classification = "established"
    if ($missingMarkers.Count -gt 0 -or $missingRules.Count -gt 0) { $classification = "fixture_incomplete" }
    elseif ([uint64]$targetActor["observed"] -eq 0) { $classification = "activation_actor_absent" }
    elseif ([uint64]$targetActor["appearance_seen"] -eq 0) { $classification = "activation_surface_absent" }
    elseif ([uint64]$targetActor["activation_latched"] -eq 0) { $classification = "activation_latch_absent" }
    elseif ([uint64]$targetActor["emitted"] -eq 0 -or $sourceIds.Count -eq 0) { $classification = "cadence_command_absent" }
    elseif ([uint64]$totals["rows"] -eq 0) { $classification = "command_loss_before_grid" }
    elseif ([uint64]$totals["probe"] -gt 0) { $classification = "grid_probe_failure" }
    elseif ([uint64]$totals["invalid"] -gt 0) { $classification = "grid_invalid_failure" }
    elseif ([uint64]$totals["capacity"] -gt 0 -and $minimumFree -eq 0 -and $maximumOccupied -gt 0) { $classification = "occupied_exhaustion" }
    elseif ([uint64]$totals["capacity"] -gt 0) { $classification = "grid_capacity_failure_with_free_bricks" }
    elseif ([uint64]$totals["admitted"] + [uint64]$totals["existing"] -eq 0) { $classification = "grid_admission_absent" }
    elseif ([uint64]$totals["deposited"] -eq 0) { $classification = "medium_absent_after_admission" }

    if ($classification -ne "established" -and $classification -ne "occupied_exhaustion") {
        $errors.Add("$($log.Name): classification=$classification")
    }
    if ($forbidden -gt 0) { $errors.Add("$($log.Name): forbidden runtime failures=$forbidden") }
    if ($brickCapacity -ne 0 -and $brickCapacity -ne 512) { $errors.Add("$($log.Name): expected bricks=512, observed=$brickCapacity") }

    $summaries += [pscustomobject]@{
        path = $log.FullName
        direction = $direction
        classification = $classification
        markers = @($markers)
        missingMarkers = $missingMarkers
        missingMapRules = $missingRules
        actor = $actor
        targetActor = $targetActor
        sourceIds = @($sourceIds)
        sourceTotals = $totals
        sources = $dukeRows
        minimumFree = if ($minimumFree -eq [uint64]::MaxValue) { $null } else { $minimumFree }
        maximumOccupied = $maximumOccupied
        brickCapacity = $brickCapacity
        admissionClosures = @($closures)
        forbiddenFailures = $forbidden
    }
}

if ($manifestFailures -gt 0) { $errors.Add("manifest process failures=$manifestFailures") }
$result = [pscustomobject]@{
    verdict = if ($errors.Count -eq 0) { "pass" } else { "fail" }
    interpretation = "established proves the DukeFire command reached grid admission and deposited medium; occupied_exhaustion is the Slice 9.1 decision-gate result, not a harness failure"
    runs = $summaries
    errors = @($errors)
}
if ($SummaryOutput) {
    $parent = Split-Path -Parent $SummaryOutput
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    $result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8
}
$result | ConvertTo-Json -Depth 12
if ($errors.Count -gt 0) { exit 1 }
