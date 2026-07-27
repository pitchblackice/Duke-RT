Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))

function Read-RequiredSource([string]$RelativePath, [string]$Purpose) {
    $path = Join-Path $root $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "missing Slice 5C $Purpose source: $RelativePath"
    }
    return Get-Content -LiteralPath $path -Raw
}

function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}

function Assert-NotMatch([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -match $Pattern) { throw $Message }
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

# Slice 5C builds on the immutable ranged-pulse ABI from Slice 5B. Epoch is a
# compatibility generation and may rebase; stable fallback identity is the
# authored pulse plus its exact half-open range.
$contracts = Read-RequiredSource 'source/common/rendering/nri/renderer/nri_smoke_contracts.h' 'pulse contract'
$pulseHeader = Read-RequiredSource 'source/common/rendering/nri/renderer/nri_smoke_pulses.h' 'pulse owner'
$pulseSource = Read-RequiredSource 'source/common/rendering/nri/renderer/nri_smoke_pulses.cpp' 'pulse owner'
foreach ($field in @('rangeBegin', 'rangeCount', 'pulseIdLow', 'pulseIdHigh')) {
    Assert-Match $contracts ("uint32_t\s+" + $field + "\s*=") "Slice 5B command ABI is missing $field."
}
Assert-Match $contracts 'sizeof\(NRISmokeInjectionCommandGpu\)\s*==\s*112' `
    'Prompt fallback requires the reviewed 112-byte ranged-pulse ABI.'
Assert-Match ($pulseHeader + $pulseSource) `
    'PulseId\([\s\S]*pulseIdHigh[\s\S]*pulseIdLow[\s\S]*RangeEnd\([\s\S]*rangeBegin[\s\S]*rangeCount' `
    'Prompt fallback requires stable pulse and half-open range identity.'

# CPU mirror -----------------------------------------------------------------
# Each unit retains deposition progress independently from visual authority.
# A fallback proxy can make a pending unit visible, but it is not deposited
# nominal mass and therefore cannot participate in the mass-conservation sum.
function New-PulseUnit {
    param(
        [uint64]$PulseId,
        [uint32]$Offset,
        [uint32]$AuthoredRendererFrame,
        [uint32]$AuthoredPresentation,
        [bool]$Interactive = $true
    )
    return [pscustomobject]@{
        pulseId = $PulseId
        rangeBegin = $Offset
        rangeCount = [uint32]1
        authoredRendererFrame = $AuthoredRendererFrame
        authoredPresentation = $AuthoredPresentation
        interactive = $Interactive
        pending = $true
        gridPlanned = $false
        deposited = $false
        resetDiscarded = $false
        authority = 'none'
        firstVisibleRendererFrame = [uint32]::MaxValue
        firstVisiblePresentation = [uint32]::MaxValue
    }
}

function Get-RangeIdentity([object]$Unit) {
    return ('{0:x16}:{1}:{2}' -f [uint64]$Unit.pulseId, [uint32]$Unit.rangeBegin, [uint32]$Unit.rangeCount)
}

function Select-FixedFallbackWork {
    param([object[]]$Units, [uint32]$Quantity)
    # This signature intentionally contains no timing, target, or headroom
    # input. Stable identity supplies deterministic service order.
    return @(
        $Units |
            Where-Object { $_.pending -and $_.authority -eq 'none' } |
            Sort-Object @{ Expression = { if ($_.interactive) { 0 } else { 1 } } },
                        @{ Expression = { [uint64]$_.pulseId } },
                        @{ Expression = { [uint32]$_.rangeBegin } } |
            Select-Object -First $Quantity
    )
}

function Publish-Fallback {
    param([object[]]$Units, [uint32]$RendererFrame, [uint32]$Presentation)
    foreach ($unit in $Units) {
        Assert-True ($unit.pending -and -not $unit.deposited -and $unit.authority -eq 'none') `
            'Fallback publication attempted to create dual or completed authority.'
        $unit.authority = 'fallback'
        $unit.firstVisibleRendererFrame = $RendererFrame
        $unit.firstVisiblePresentation = $Presentation
    }
}

function Plan-GridRange([object[]]$Units) {
    foreach ($unit in $Units) {
        Assert-True ($unit.pending -and -not $unit.deposited -and -not $unit.gridPlanned) `
            'Grid planning attempted to replay committed or already planned mass.'
        $unit.gridPlanned = $true
    }
}

function Rollback-GridRange([object[]]$Units) {
    foreach ($unit in $Units) {
        Assert-True $unit.gridPlanned 'Rollback requires the exact active grid range plan.'
        $unit.gridPlanned = $false
        # Existing fallback remains authoritative; rollback cannot make current
        # smoke disappear or consume deposition progress.
    }
}

function Commit-GridHandoff([object[]]$Units, [uint32]$RendererFrame, [uint32]$Presentation) {
    foreach ($unit in $Units) {
        Assert-True ($unit.gridPlanned -and $unit.pending -and -not $unit.deposited) `
            'Grid handoff requires one pending planned range exactly once.'
        $unit.gridPlanned = $false
        $unit.pending = $false
        $unit.deposited = $true
        # One state field is the all-or-fallback authority. There is no state
        # in which fallback and grid are simultaneously composited.
        $unit.authority = 'grid'
        if ($unit.firstVisibleRendererFrame -eq [uint32]::MaxValue) {
            $unit.firstVisibleRendererFrame = $RendererFrame
            $unit.firstVisiblePresentation = $Presentation
        }
    }
}

function Assert-MassClosure([object[]]$Units, [uint32]$AuthoredMass) {
    $pending = @($Units | Where-Object pending).Count
    $deposited = @($Units | Where-Object deposited).Count
    $discarded = @($Units | Where-Object resetDiscarded).Count
    Assert-True (($pending + $deposited + $discarded) -eq $AuthoredMass) `
        "Nominal mass did not close: authored=$AuthoredMass pending=$pending deposited=$deposited discarded=$discarded."
    foreach ($unit in $Units) {
        Assert-True (-not ($unit.pending -and $unit.deposited)) 'One nominal range was both pending and deposited.'
        Assert-True ($unit.authority -in @('none', 'fallback', 'grid')) 'One range has invalid or dual visual authority.'
        if ($unit.authority -eq 'grid') {
            Assert-True $unit.deposited 'Grid visual authority exists without committed deposition.'
        }
        if ($unit.authority -eq 'fallback') {
            Assert-True ($unit.pending -and -not $unit.deposited) 'Fallback authority survived committed grid handoff.'
        }
    }
}

$fixedQuantity = [uint32]3
$units = @(
    for ($offset = 0; $offset -lt 7; ++$offset) {
        New-PulseUnit -PulseId 0x100000002 -Offset $offset -AuthoredRendererFrame 40 -AuthoredPresentation 100
    }
)
$identityBefore = @($units | ForEach-Object { Get-RangeIdentity $_ })
$firstService = @(Select-FixedFallbackWork -Units $units -Quantity $fixedQuantity)
Assert-True ($firstService.Count -eq $fixedQuantity) 'Saturated prompt fallback did not execute its exact fixed quantity.'
Publish-Fallback -Units $firstService -RendererFrame 40 -Presentation 101
Assert-True (@($units | Where-Object authority -eq 'fallback').Count -eq $fixedQuantity) `
    'Fallback publication exceeded or under-filled the fixed quantity.'

$secondService = @(Select-FixedFallbackWork -Units $units -Quantity $fixedQuantity)
Assert-True ($secondService.Count -eq $fixedQuantity) 'Second fallback frame did not retain fixed service.'
Assert-True (@($firstService | Where-Object { $secondService -contains $_ }).Count -eq 0) `
    'Fixed fallback service replayed an already authoritative range.'
Assert-True ((@($units | ForEach-Object { Get-RangeIdentity $_ }) -join '|') -eq ($identityBefore -join '|')) `
    'Fallback scheduling changed stable pulse/range identity.'

# Rollback preserves both mass and already visible fallback authority.
$handoff = @($firstService | Select-Object -First 2)
Plan-GridRange -Units $handoff
Rollback-GridRange -Units $handoff
Assert-True (@($handoff | Where-Object authority -eq 'fallback').Count -eq 2) `
    'Grid rollback removed prompt fallback authority.'
Assert-MassClosure -Units $units -AuthoredMass 7

# Successful commit consumes each nominal unit exactly once and atomically
# replaces fallback authority. A stale second commit must be impossible.
Plan-GridRange -Units $handoff
Commit-GridHandoff -Units $handoff -RendererFrame 41 -Presentation 101
Assert-True (@($handoff | Where-Object authority -eq 'grid').Count -eq 2) `
    'Grid commit did not retire fallback authority atomically.'
$staleCommitRejected = $false
try { Commit-GridHandoff -Units $handoff -RendererFrame 42 -Presentation 102 }
catch { $staleCommitRejected = $true }
Assert-True $staleCommitRejected 'A committed fallback range could deposit twice.'
Assert-MassClosure -Units $units -AuthoredMass 7

# Supported interactive smoke publishes in its authored renderer frame and is
# observable by the next main-view presentation. The deliberately late sample
# proves the deadline oracle rejects presentation + 2.
foreach ($unit in $firstService) {
    Assert-True ($unit.firstVisibleRendererFrame -le $unit.authoredRendererFrame) `
        'Supported interactive fallback was not published in the authored renderer frame.'
    Assert-True ($unit.firstVisiblePresentation -le $unit.authoredPresentation + 1) `
        'Supported interactive fallback missed next-main-view presentation.'
}
$late = New-PulseUnit -PulseId 9 -Offset 0 -AuthoredRendererFrame 50 -AuthoredPresentation 200
Publish-Fallback -Units @($late) -RendererFrame 51 -Presentation 202
Assert-True (-not ($late.firstVisibleRendererFrame -le $late.authoredRendererFrame -and
    $late.firstVisiblePresentation -le $late.authoredPresentation + 1)) `
    'Next-frame visibility oracle accepted a renderer/presentation deadline miss.'

# Structural contract ---------------------------------------------------------
# Slice 5C should remain a focused owner rather than adding fallback policy to
# the already broad frame orchestrator.
$fallbackHeader = Read-RequiredSource 'source/common/rendering/nri/renderer/nri_smoke_prompt_fallback.h' 'focused owner'
$fallbackSource = Read-RequiredSource 'source/common/rendering/nri/renderer/nri_smoke_prompt_fallback.cpp' 'focused owner'
$runtime = Read-RequiredSource 'source/common/rendering/nri/renderer/nri_smoke.cpp' 'runtime integration'
$smokeHeader = Read-RequiredSource 'source/common/rendering/nri/renderer/nri_smoke.h' 'runtime integration'
$focused = $fallbackHeader + "`n" + $fallbackSource

Assert-Match $focused 'pulseIdLow[\s\S]*pulseIdHigh[\s\S]*rangeBegin[\s\S]*rangeCount' `
    'Prompt fallback records must preserve exact Slice 5B pulse/range identity.'
Assert-Match $focused '(?i)(?:authority|owner)[\s\S]*(?:fallback|coarse|kernel)[\s\S]*(?:grid|deposit)[\s\S]*(?:handoff|commit)' `
    'Prompt fallback must encode all-or-fallback authority and explicit grid handoff.'
Assert-Match $focused '(?i)(?:Plan|Prepare)[\s\S]*(?:Commit|Handoff)[\s\S]*Rollback' `
    'Prompt fallback must expose transactional plan/commit/rollback handoff.'
Assert-Match $focused '(?i)(?:fixed|maximum|capacity)[A-Za-z0-9_]*(?:Fallback|Kernel|Carrier)|(?:Fallback|Kernel|Carrier)[A-Za-z0-9_]*(?:Quantity|Capacity|Maximum)' `
    'Prompt fallback must own an explicit fixed per-frame work quantity.'
Assert-Match $focused '(?i)(?:authored|enqueued)[A-Za-z0-9_]*(?:Frame|Presentation)[\s\S]*(?:visible|published)[A-Za-z0-9_]*(?:Frame|Presentation)' `
    'Prompt fallback must publish authored-to-visible frame/presentation evidence.'
Assert-Match ($focused + $runtime + $smokeHeader) '(?i)(?:pending|authored)[A-Za-z0-9_]*Mass[\s\S]*(?:committed|deposited)[A-Za-z0-9_]*Mass[\s\S]*(?:fallback|handoff)' `
    'Prompt fallback telemetry must expose nominal mass closure and handoff.'

# Static work means static work: the fallback owner cannot inspect renderer GPU
# timing, projected headroom, a target frame duration, or adaptive deadlines.
Assert-NotMatch $focused '(?i)gpu\s*time|gputim|headroom|frame\s*time|frametime|target\s*(?:fps|frame|milliseconds|ms)|projected|predicted|adaptive\s*(?:budget|quantity|work)' `
    'Prompt fallback work quantity depends on timing/headroom feedback.'
Assert-Match ($runtime + $focused) '(?i)(?:scheduled|executed)[A-Za-z0-9_]*(?:Fallback|Kernel|Carrier)[\s\S]*(?:quantity|capacity|maximum)' `
    'Prompt fallback runtime must publish executed work against its fixed quantity.'

Write-Host 'Smoke prompt fallback structural and CPU-mirror tests passed.'
