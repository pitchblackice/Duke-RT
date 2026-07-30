$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$runner = Get-Content -LiteralPath (Join-Path $root 'tools/validation/run-smoke-grid-lifecycle-repro.ps1') -Raw
$analyzer = Get-Content -LiteralPath (Join-Path $root 'tools/validation/analyze-smoke-grid-lifecycle-repro.ps1') -Raw
$overlay = Get-Content -LiteralPath (Join-Path $root 'tools/validation/fixtures/smoke-lifecycle-no-continuous/LIGHTOVR') -Raw

function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}
function Assert-NotMatch([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -match $Pattern) { throw $Message }
}

Assert-Match $runner '\[ValidateRange\(1, 20\)\]\[int\]\$Runs\s*=\s*3' 'Lifecycle validation must default to three runs.'
Assert-Match $runner '\[string\]\$Map\s*=\s*"e1l5"' 'Lifecycle validation must use a source-free map by default.'
Assert-Match $runner 'PostMapWarmupWaitTics\s*=\s*600' 'Lifecycle validation must wait through map startup before its cold snapshot.'
Assert-Match $runner 'InjectionSettleWaitTics\s*=\s*120' 'Injection settling must be configurable.'
Assert-Match $runner 'LifecycleDrainWaitTics\s*=\s*600' 'Optical draining must be configurable.'
Assert-Match $runner 'ReclaimGraceFrames\s*=\s*120' 'The reclamation grace used by a run must be explicit and configurable.'
Assert-Match $runner 'nri_ptsmokegridreclaimgrace'',\s*\[string\]\$ReclaimGraceFrames' 'The runner must pass its recorded reclamation grace to the renderer.'
$injections = [regex]::Matches($runner, 'nri_ptsmoke_test').Count
if ($injections -ne 2) { throw "Lifecycle validation must inject exactly twice without reset; found $injections" }
Assert-NotMatch $runner 'nri_ptsmokereset|map\s+\$Map[\s\S]*map\s+\$Map' 'The two cycles must not reset or reload the map.'

Assert-Match $overlay 'smokestyle\s+"validation_lifecycle"' 'The lifecycle overlay must pin its synthetic style.'
Assert-Match $overlay 'lifetime\s+5\.0[\s\S]*densityhalflife\s+2\.2' 'The lifecycle fixture must retain its optical-tail witness.'
Assert-NotMatch $overlay 'smoke(?:actor|event)rule|smokeemitter' 'The lifecycle fixture must not contain authored smoke sources.'

foreach ($field in @('active_ping', 'resident', 'free', 'bricks', 'allocated', 'reclaimed', 'occupied', 'empty', 'field_hash')) {
    Assert-Match $analyzer ([regex]::Escape($field)) "Lifecycle closure must inspect $field."
}
foreach ($field in @('hash_empty', 'hash_tombstone', 'hash_resident', 'hash_new', 'hash_claimed', 'hash_invalid_state', 'hash_invalid_mapping')) {
    Assert-Match $analyzer ([regex]::Escape($field)) "Analyzer must recognize exact hash-health gauge $field."
}
Assert-Match $analyzer '\$empty\s*\+\s*\$tombstones\s*-ne\s*\$hashCapacity' 'Drained hash accounting must close to hash capacity.'
Assert-Match $analyzer 'second injection did not allocate fresh topology without reset' 'Analyzer must prove reinjection after reclamation.'
Assert-Match $analyzer 'Device removed[\s\S]*Unknown command' 'Analyzer must scan explicit runtime failure strings.'

Write-Host 'Smoke grid source-free lifecycle harness contract passed.'
