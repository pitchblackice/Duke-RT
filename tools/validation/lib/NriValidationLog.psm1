Set-StrictMode -Version Latest

function ConvertFrom-NriKeyValueLine {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Line,

        [string]$Prefix = "NRI PT selftest:"
    )

    if (-not (Test-NriLinePrefix -Line $Line -Prefix $Prefix)) {
        return $null
    }

    $result = [ordered]@{}
    $payload = $Line.Substring($Prefix.Length).Trim()
    if ([string]::IsNullOrWhiteSpace($payload)) {
        return [pscustomobject]$result
    }

    foreach ($token in ($payload -split '\s+')) {
        $equals = $token.IndexOf('=')
        if ($equals -le 0) {
            continue
        }

        $key = $token.Substring(0, $equals)
        $value = $token.Substring($equals + 1)
        $result[$key] = $value
    }

    [pscustomobject]$result
}

function Test-NriLinePrefix {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Line,

        [Parameter(Mandatory = $true)]
        [string]$Prefix
    )

    return $Line.StartsWith($Prefix)
}

function Test-NriTruthyValue {
    param([object]$Value)

    if ($null -eq $Value) {
        return $false
    }

    $text = [string]$Value
    return $text -eq "1" -or
        $text.Equals("yes", [System.StringComparison]::OrdinalIgnoreCase) -or
        $text.Equals("true", [System.StringComparison]::OrdinalIgnoreCase)
}

function Get-NriObjectPropertyValue {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Object,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    if ($Object.PSObject.Properties.Name.Contains($Name)) {
        return $Object.$Name
    }

    return $null
}

function ConvertTo-NriInt64 {
    param([object]$Value)

    if ($null -eq $Value) {
        return $null
    }

    $parsed = 0L
    if ([int64]::TryParse([string]$Value, [ref]$parsed)) {
        return $parsed
    }

    return $null
}

function Get-NriValidationLogSummary {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [string[]]$RequiredPrefixes = @("NRI PT selftest:"),

        [string[]]$ForbiddenPatterns = @(
            "Device removed",
            "validation error",
            "failed to create",
            "assertion failed"
        )
    )

    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    $requiredHits = [ordered]@{}
    foreach ($prefix in $RequiredPrefixes) {
        $requiredHits[$prefix] = 0
    }

    $forbiddenHits = New-Object System.Collections.Generic.List[object]
    $selfTestFrames = New-Object System.Collections.Generic.List[object]
    $acceptedFrames = New-Object System.Collections.Generic.List[object]
    $loadingSummaries = New-Object System.Collections.Generic.List[object]

    $lineNumber = 0
    $stream = [System.IO.File]::Open($resolved.Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        $reader = [System.IO.StreamReader]::new($stream)
        try {
            while ($true) {
                $line = $reader.ReadLine()
                if ($null -eq $line) {
                    break
                }

                $lineNumber++

                foreach ($prefix in $RequiredPrefixes) {
                    if (Test-NriLinePrefix -Line $line -Prefix $prefix) {
                        $requiredHits[$prefix] = [int]$requiredHits[$prefix] + 1
                    }
                }

                foreach ($pattern in $ForbiddenPatterns) {
                    if ($line.IndexOf($pattern, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                        $forbiddenHits.Add([pscustomobject]@{
                            line = $lineNumber
                            pattern = $pattern
                            text = $line
                        })
                    }
                }

                if (Test-NriLinePrefix -Line $line -Prefix "NRI PT selftest:") {
                    $frame = ConvertFrom-NriKeyValueLine -Line $line
                    if ($null -ne $frame) {
                        $frame | Add-Member -NotePropertyName "line" -NotePropertyValue $lineNumber
                        $selfTestFrames.Add($frame)
                        if ((Test-NriTruthyValue (Get-NriObjectPropertyValue -Object $frame -Name "world_active")) -and
                            (Test-NriTruthyValue (Get-NriObjectPropertyValue -Object $frame -Name "gameplay_frame"))) {
                            $acceptedFrames.Add($frame)
                        }
                    }
                }

                if (Test-NriLinePrefix -Line $line -Prefix "NRI PT loading summary:") {
                    $loading = ConvertFrom-NriKeyValueLine -Line $line -Prefix "NRI PT loading summary:"
                    if ($null -ne $loading) {
                        $loading | Add-Member -NotePropertyName "line" -NotePropertyValue $lineNumber
                        $loadingSummaries.Add($loading)
                    }
                }
            }
        }
        finally {
            $reader.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }

    [pscustomobject]@{
        path = $resolved.Path
        requiredPrefixes = $requiredHits
        forbiddenHits = $forbiddenHits.ToArray()
        loadingSummaryCount = $loadingSummaries.Count
        loadingSummaries = $loadingSummaries.ToArray()
        firstLoadingSummary = @($loadingSummaries.ToArray()) | Select-Object -First 1
        latestLoadingSummary = @($loadingSummaries.ToArray()) | Select-Object -Last 1
        selfTestFrameCount = $selfTestFrames.Count
        acceptedSelfTestFrameCount = $acceptedFrames.Count
        selfTestFrames = $selfTestFrames.ToArray()
        acceptedSelfTestFrames = $acceptedFrames.ToArray()
    }
}

function Test-NriLoadingAssertions {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Summary,

        [object]$Assertions
    )

    $errors = New-Object System.Collections.Generic.List[string]
    if ($null -eq $Assertions) {
        return [pscustomobject]@{
            ok = $true
            errors = $errors.ToArray()
        }
    }

    $latest = $Summary.latestLoadingSummary
    if (($Assertions.PSObject.Properties.Name.Contains("requirePreloadReady") -and [bool]$Assertions.requirePreloadReady) -or
        ($Assertions.PSObject.Properties.Name.Contains("requireSummary") -and [bool]$Assertions.requireSummary)) {
        if ($Summary.loadingSummaryCount -le 0 -or $null -eq $latest) {
            $errors.Add("missing NRI PT loading summary")
        }
    }

    $firstAccepted = @($Summary.acceptedSelfTestFrames) | Select-Object -First 1
    if ($null -ne $latest -and $null -ne $firstAccepted) {
        $preloadBeforeFirstAccepted = $false
        foreach ($loading in @($Summary.loadingSummaries)) {
            if ([int]$loading.line -lt [int]$firstAccepted.line) {
                $preloadBeforeFirstAccepted = $true
                break
            }
        }
        if (-not $preloadBeforeFirstAccepted) {
            $errors.Add("loading summary was not emitted before the first accepted selftest frame")
        }
    }

    if ($null -ne $latest) {
        if ($Assertions.PSObject.Properties.Name.Contains("forbidFrameTargetWait") -and [bool]$Assertions.forbidFrameTargetWait) {
            if (Test-NriTruthyValue (Get-NriObjectPropertyValue -Object $latest -Name "frame_target_used")) {
                $errors.Add("loading summary used an onscreen frame target")
            }
        }

        if ($Assertions.PSObject.Properties.Name.Contains("requireStandaloneContext") -and [bool]$Assertions.requireStandaloneContext) {
            if (-not (Test-NriTruthyValue (Get-NriObjectPropertyValue -Object $latest -Name "standalone_context_used"))) {
                $errors.Add("loading summary did not use a standalone preload context")
            }
        }

        if ($Assertions.PSObject.Properties.Name.Contains("maxRequiredVoxelPendingAtReady")) {
            $value = ConvertTo-NriInt64 (Get-NriObjectPropertyValue -Object $latest -Name "required_voxel_pending")
            if ($null -eq $value) {
                $errors.Add("loading summary is missing numeric required_voxel_pending")
            }
            elseif ($value -gt [int64]$Assertions.maxRequiredVoxelPendingAtReady) {
                $errors.Add("required_voxel_pending $value > allowed $($Assertions.maxRequiredVoxelPendingAtReady)")
            }
        }

        if ($Assertions.PSObject.Properties.Name.Contains("maxStartupCorrectionPendingAtReady")) {
            $value = ConvertTo-NriInt64 (Get-NriObjectPropertyValue -Object $latest -Name "startup_correction_pending")
            if ($null -eq $value) {
                $errors.Add("loading summary is missing numeric startup_correction_pending")
            }
            elseif ($value -gt [int64]$Assertions.maxStartupCorrectionPendingAtReady) {
                $errors.Add("startup_correction_pending $value > allowed $($Assertions.maxStartupCorrectionPendingAtReady)")
            }
        }
    }

    if ($null -ne $firstAccepted) {
        if ($Assertions.PSObject.Properties.Name.Contains("maxFirstFrameRuntimeVoxelAdmissions")) {
            $value = ConvertTo-NriInt64 (Get-NriObjectPropertyValue -Object $firstAccepted -Name "runtime_voxel_onboarding_admitted")
            if ($null -eq $value) {
                $errors.Add("first accepted selftest frame is missing numeric runtime_voxel_onboarding_admitted")
            }
            elseif ($value -gt [int64]$Assertions.maxFirstFrameRuntimeVoxelAdmissions) {
                $errors.Add("first-frame runtime voxel admissions $value > allowed $($Assertions.maxFirstFrameRuntimeVoxelAdmissions)")
            }
        }

        if ($Assertions.PSObject.Properties.Name.Contains("maxFirstFrameStaticSceneBuilds")) {
            $uploads = ConvertTo-NriInt64 (Get-NriObjectPropertyValue -Object $firstAccepted -Name "static_scene_upload_this_frame")
            $asBuilds = ConvertTo-NriInt64 (Get-NriObjectPropertyValue -Object $firstAccepted -Name "static_scene_as_build_this_frame")
            if ($null -eq $uploads -or $null -eq $asBuilds) {
                $errors.Add("first accepted selftest frame is missing static scene loading fields")
            }
            else {
                $total = $uploads + $asBuilds
                if ($total -gt [int64]$Assertions.maxFirstFrameStaticSceneBuilds) {
                    $errors.Add("first-frame static scene loading work $total > allowed $($Assertions.maxFirstFrameStaticSceneBuilds)")
                }
            }
        }

        if ($Assertions.PSObject.Properties.Name.Contains("maxFirstFrameTexturePrewarmDefers")) {
            $value = ConvertTo-NriInt64 (Get-NriObjectPropertyValue -Object $firstAccepted -Name "runtime_voxel_texture_prewarm_deferred")
            if ($null -eq $value) {
                $errors.Add("first accepted selftest frame is missing numeric runtime_voxel_texture_prewarm_deferred")
            }
            elseif ($value -gt [int64]$Assertions.maxFirstFrameTexturePrewarmDefers) {
                $errors.Add("first-frame texture prewarm defers $value > allowed $($Assertions.maxFirstFrameTexturePrewarmDefers)")
            }
        }
    }

    [pscustomobject]@{
        ok = $errors.Count -eq 0
        errors = $errors.ToArray()
    }
}

function Test-NriValidationSummary {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Summary,

        [int]$MinSelfTestFrames = 1
    )

    $errors = New-Object System.Collections.Generic.List[string]

    foreach ($entry in $Summary.requiredPrefixes.GetEnumerator()) {
        if ([int]$entry.Value -le 0) {
            $errors.Add("missing required prefix '$($entry.Key)'")
        }
    }

    if ($Summary.forbiddenHits.Count -gt 0) {
        foreach ($hit in $Summary.forbiddenHits) {
            $errors.Add("forbidden pattern '$($hit.pattern)' at line $($hit.line)")
        }
    }

    if ($Summary.acceptedSelfTestFrameCount -lt $MinSelfTestFrames) {
        $errors.Add("accepted selftest frames $($Summary.acceptedSelfTestFrameCount) < required $MinSelfTestFrames")
    }

    $requiredFrameFields = @(
        "frame",
        "map",
        "api",
        "world_active",
        "gameplay_frame",
        "route",
        "passes",
        "prims",
        "mats",
        "scene_instances",
        "final_valid"
    )

    foreach ($frame in $Summary.acceptedSelfTestFrames) {
        foreach ($field in $requiredFrameFields) {
            if (-not $frame.PSObject.Properties.Name.Contains($field)) {
                $errors.Add("accepted selftest frame is missing '$field'")
            }
        }

        if ($frame.PSObject.Properties.Name.Contains("route") -and [string]$frame.route -eq "unknown") {
            $errors.Add("accepted selftest frame has unknown route")
        }
        if ($frame.PSObject.Properties.Name.Contains("passes") -and [string]$frame.passes -eq "unknown") {
            $errors.Add("accepted selftest frame has unknown pass list")
        }
        if ($frame.PSObject.Properties.Name.Contains("prims") -and [int64]$frame.prims -le 0) {
            $errors.Add("accepted selftest frame has no primitives")
        }
        if ($frame.PSObject.Properties.Name.Contains("mats") -and [int64]$frame.mats -le 0) {
            $errors.Add("accepted selftest frame has no materials")
        }
        if ($frame.PSObject.Properties.Name.Contains("scene_instances") -and [int64]$frame.scene_instances -le 0) {
            $errors.Add("accepted selftest frame has no scene instances")
        }
        if ($frame.PSObject.Properties.Name.Contains("final_valid") -and -not (Test-NriTruthyValue $frame.final_valid)) {
            $errors.Add("accepted selftest frame has invalid final output texture")
        }
    }

    [pscustomobject]@{
        ok = $errors.Count -eq 0
        errors = $errors.ToArray()
    }
}

Export-ModuleMember -Function ConvertFrom-NriKeyValueLine, Test-NriTruthyValue, Get-NriValidationLogSummary, Test-NriValidationSummary, Test-NriLoadingAssertions
