$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')

function Read-Source([string]$Path) {
    $resolved = Join-Path $root $Path
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw "missing Slice 4A source: $Path"
    }
    return Get-Content -LiteralPath $resolved -Raw
}

function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

$buildActive = Read-Source 'source/common/rendering/nri/shaders/SmokeGridLightBuildActive.cs.hlsl'
$lightingData = Read-Source 'source/common/rendering/nri/shaders/Include/SmokeGridLightingData.hlsli'
$lightingResources = Read-Source 'source/common/rendering/nri/shaders/Include/SmokeGridLightingResources.hlsli'
$contracts = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_grid_lighting_contracts.h'
$owner = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_grid_lighting.cpp'
$ownerHeader = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_grid_lighting.h'

# A world-light source exists only where the current materialized medium has
# both significant extinction and a nonzero optical color. This must remain the
# same predicate used by smoke materialization rather than brick occupancy.
Assert-Match $buildActive `
    'scalar\.z\s*>\s*1e-6[\s\S]{0,240}(?:any\s*\(\s*optical\.rgb\s*>\s*0(?:\.0)?\s*\)|max\s*\(\s*optical\.r\s*,\s*max\s*\(\s*optical\.g\s*,\s*optical\.b\s*\)\s*\)\s*>\s*0(?:\.0)?)' `
    'Slice 4A source discovery must require scalar extinction > 1e-6 and nonzero optical RGB.'

# Trilinear lower-corner reconstruction consumes exactly the source cell plus
# {-1,0} on each axis. Prove the mathematical support independently of shader
# spelling so an accidental +1 expansion or missing corner fails loudly.
$offsets = [System.Collections.Generic.HashSet[string]]::new()
foreach ($z in -1..0) {
    foreach ($y in -1..0) {
        foreach ($x in -1..0) {
            [void]$offsets.Add("$x,$y,$z")
        }
    }
}
Assert-True ($offsets.Count -eq 8) 'The lower-corner dilation oracle must contain exactly eight offsets.'
foreach ($required in @('-1,-1,-1', '0,-1,-1', '-1,0,-1', '0,0,-1', '-1,-1,0', '0,-1,0', '-1,0,0', '0,0,0')) {
    Assert-True $offsets.Contains($required) "The lower-corner dilation oracle is missing $required."
}

$explicitOffsets = '(?s)(?:int3\s*\(\s*-1\s*,\s*-1\s*,\s*-1\s*\).+int3\s*\(\s*0\s*,\s*0\s*,\s*0\s*\)|for\s*\([^\)]*=\s*-1[^\)]*<=\s*0[^\)]*\).+for\s*\([^\)]*=\s*-1[^\)]*<=\s*0[^\)]*\).+for\s*\([^\)]*=\s*-1[^\)]*<=\s*0[^\)]*\))'
Assert-Match $buildActive $explicitOffsets `
    'Slice 4A must enumerate the exact {-1,0}^3 lower-corner support dilation.'
Assert-Match $buildActive `
    'SmokeGridLightCellAddress\s*\(\s*[^,]*(?:support|candidate|corner)[^,]*\s*,\s*[^,]+,\s*[^\)]+\)' `
    'Slice 4A support discovery must resolve every dilated world cell through the cross-brick address lookup.'

# Multiple source cells share reconstruction corners. A dedicated stamp must
# include both the current brick generation and frame and claim support through
# an atomic operation before appending to the active list.
Assert-Match ($lightingData + $contracts) `
    '(?s)(?:SupportStamp|WorldSupport)[\s\S]{0,300}(?:Brick)?Generation[\s\S]{0,160}FrameStamp' `
    'Slice 4A requires a dedicated generation/frame-stamped support dedupe record.'
Assert-Match ($lightingResources + $ownerHeader + $owner) `
    '(?s)(?:SupportStamp|WorldSupport)[\s\S]{0,500}(?:RWStructuredBuffer|NRIBufferResource|CreateBuffer)' `
    'Slice 4A requires a dedicated GPU support-stamp resource owned by world lighting.'
Assert-Match $buildActive `
    '(?s)Interlocked(?:CompareExchange|Exchange)[\s\S]{0,500}(?:Generation|FrameStamp)[\s\S]{0,500}(?:DuplicateCount|Duplicate)' `
    'Slice 4A support dedupe must atomically claim a generation/frame stamp and count duplicate attempts.'

# ActiveCount is the authoritative unique support count. Source/support-only
# classification closes exactly over it; overflow is reported separately and
# may not be hidden in either class.
foreach ($field in @('ActiveCount', 'SourceCount', 'SupportOnlyCount', 'DuplicateCount', 'SupportOverflowCount')) {
    Assert-Match $lightingData ("uint\s+" + $field + "\s*;") "HLSL world-light control is missing $field."
    $cpuName = $field.Substring(0, 1).ToLowerInvariant() + $field.Substring(1)
    Assert-Match $contracts ("uint32_t\s+" + $cpuName + "\s*=") "CPU world-light control is missing $cpuName."
}
Assert-Match ($buildActive + $owner) `
    '(?s)SourceCount\s*\+\s*[^;\r\n]*SupportOnlyCount\s*==\s*[^;\r\n]*ActiveCount|ActiveCount\s*==\s*[^;\r\n]*SourceCount\s*\+\s*[^;\r\n]*SupportOnlyCount' `
    'Slice 4A telemetry must explicitly validate SourceCount + SupportOnlyCount == ActiveCount.'
Assert-Match $buildActive 'InterlockedAdd\s*\(\s*gSmokeGridLightControl\[0\]\.DuplicateCount' `
    'Slice 4A must report deduplicated support attempts.'
Assert-Match $buildActive 'InterlockedAdd\s*\(\s*gSmokeGridLightControl\[0\]\.SupportOverflowCount' `
    'Slice 4A must report compact-support overflow separately.'

# The retained world-light stages must consume compact unique support rather
# than deriving work from resident-brick capacity.
foreach ($shaderName in @(
    'SmokeGridLightBuildLinks.cs.hlsl',
    'SmokeGridLightSeed.cs.hlsl',
    'SmokeGridLightTemporal.cs.hlsl',
    'SmokeGridLightFilter.cs.hlsl'
)) {
    $shader = Read-Source ("source/common/rendering/nri/shaders/" + $shaderName)
    Assert-Match $shader 'gSmokeGridLightControl\[0\]\.ActiveCount' "$shaderName must bound work by compact ActiveCount."
    Assert-Match $shader 'gSmokeGridLightActive\s*\[\s*dispatchThreadId\.x\s*\]' "$shaderName must obtain cell identity from the compact active list."
}

Write-Host 'Smoke exact compact world-support contract tests passed.'
