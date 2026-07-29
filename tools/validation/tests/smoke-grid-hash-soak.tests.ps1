$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$runner = Get-Content -LiteralPath (Join-Path $root 'tools/validation/run-smoke-grid-hash-soak.ps1') -Raw
$analyzer = Get-Content -LiteralPath (Join-Path $root 'tools/validation/analyze-smoke-grid-hash-soak.ps1') -Raw
$fixture = Get-Content -LiteralPath (Join-Path $root 'tools/validation/fixtures/smoke-lifecycle-no-continuous/LIGHTOVR') -Raw

function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}
function Assert-NotMatch([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -match $Pattern) { throw $Message }
}

Assert-Match $runner '\$checkpoints\s*=\s*@\(30, 60, 120\)' 'Soak checkpoints must remain exactly 30/60/120 seconds.'
Assert-Match $runner '\$gameTicsPerSecond\s*=\s*30' 'Console wait seconds must use the engine GameTicRate contract.'
Assert-Match $runner "nri_ptsmokereadback',\s*'true'" 'Exact grid readback must be enabled explicitly.'
Assert-Match $runner "nri_ptsmokegridreclaimgrace',\s*\[string\]\`$ReclaimGraceFrames" 'The soak must record and set its reclamation grace.'
Assert-Match $runner 'while \(\$elapsedSeconds -lt \$checkpoint\)[\s\S]*nri_ptsmoke_test[\s\S]*CycleSeconds \* \$gameTicsPerSecond' 'Each interval must use deterministic repeated injection/drain cycles.'
Assert-Match $runner 'nri_ptsmokestatus[\s\S]*foreach \(\$checkpoint in \$checkpoints\)' 'The soak must record a source-free baseline before checkpoint cycles.'
Assert-NotMatch $fixture 'smoke(?:actor|event)rule|smokeemitter' 'The soak fixture must remain source-free.'

foreach ($field in @(
    'hash_empty', 'hash_claimed', 'hash_resident', 'hash_new', 'hash_tombstone',
    'hash_invalid_state', 'hash_invalid_mapping', 'control_probe_total',
    'lookup_probe_total', 'insertion_probe_total', 'control_probe_bins', 'max_probe'
)) {
    Assert-Match $analyzer ([regex]::Escape($field)) "Analyzer must consume exact $field evidence."
}
foreach ($field in @(
    'lookup_probe_limit_failures', 'insertion_probe_limit_failures',
    'insertion_capacity_failures', 'insertion_active_failures',
    'reclaim_invalid_mapping_failures', 'hash_rebuild_failures'
)) {
    Assert-Match $analyzer ([regex]::Escape($field)) "Analyzer must reject $field."
}
Assert-Match $analyzer '\$sum\s*-ne\s*\$hashCapacity' 'All physical hash states must close exactly to capacity.'
Assert-Match $analyzer "'stable-exact'[\s\S]*'monotonic-growth'[\s\S]*'non-monotonic'" 'Analyzer must classify stable and monotonic-growth evidence explicitly.'
Assert-Match $analyzer "rebuildPolicy\s*=\s*'none-inferred'" 'The analyzer must not invent an automatic rebuild threshold.'
Assert-NotMatch $analyzer 'TombstoneThreshold|ProbeThreshold|hashTombstone\s*-g[te]|averageProbeLength\s*-g[te]' 'The soak must report evidence without choosing a rebuild trigger.'

$temporary = Join-Path ([System.IO.Path]::GetTempPath()) ('raze-smoke-hash-soak-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    $log = Join-Path $temporary 'synthetic.log'
    $summary = Join-Path $temporary 'summary.json'
    $rows = @(
        @{ tomb = 0; probes = 0; bins = 0; allocated = 0 },
        @{ tomb = 10; probes = 30; bins = 30; allocated = 3 },
        @{ tomb = 10; probes = 60; bins = 60; allocated = 6 },
        @{ tomb = 10; probes = 120; bins = 120; allocated = 12 }
    ) | ForEach-Object {
        "NRI PT smoke grid status: resources=ready gpu_stats=valid bricks=512 hash=1024 resident=0 free=512 allocated=$($_.allocated) reclaimed=$($_.allocated) allocation_failures=0 probe_failures=0 nan=0 hash_empty=$(1024 - $_.tomb) hash_claimed=0 hash_resident=0 hash_new=0 hash_tombstone=$($_.tomb) hash_invalid_state=0 hash_invalid_mapping=0 control_probe_total=$($_.probes) control_probe_bins=$($_.bins)/0/0/0/0 lookup_probe_total=$($_.probes) insertion_probe_total=0 max_probe=1 lookup_probe_limit_failures=0 insertion_probe_limit_failures=0 insertion_capacity_failures=0 insertion_active_failures=0 reclaim_invalid_mapping_failures=0 hash_rebuild_attempts=0 hash_rebuild_successes=0 hash_rebuild_failures=0"
    }
    $rows | Set-Content -LiteralPath $log -Encoding UTF8
    $analyzerPath = Join-Path $root 'tools/validation/analyze-smoke-grid-hash-soak.ps1'
    & $analyzerPath -LogPath $log -SummaryOutput $summary -CycleSeconds 10 | Out-Null
    $result = Get-Content -LiteralPath $summary -Raw | ConvertFrom-Json
    if (-not $result.integrityPassed) { throw 'Synthetic hash-soak analysis did not pass integrity.' }
    if ($result.runs[0].classification.tombstones -ne 'stable-exact' -or
        $result.runs[0].classification.averageProbeLength -ne 'stable-exact') {
        throw 'Synthetic stable plateau was not classified exactly.'
    }
    if ($result.runs[0].samples[2].controlProbeDelta -ne 60) {
        throw 'The 60-to-120-second probe delta was not preserved exactly.'
    }
}
finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force
}

Write-Host 'Smoke grid 30/60/120-second hash-health soak contracts passed.'
