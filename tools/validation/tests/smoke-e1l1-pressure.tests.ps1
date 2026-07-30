$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')

function Read-Repo([string]$Path) { Get-Content -Raw (Join-Path $root $Path) }
function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }

$runner = Read-Repo 'tools/validation/run-smoke-e1l1-pressure.ps1'
$analyzer = Read-Repo 'tools/validation/analyze-smoke-e1l1-pressure.ps1'
$emitter = Read-Repo 'source/common/rendering/nri/renderer/nri_smoke_emitters.cpp'
$header = Read-Repo 'source/common/rendering/nri/renderer/nri_smoke_emitters.h'

Require ($runner -match 'map e1l1; wait 1; closemenu') 'fixture must use the known delayed-map and closemenu workflow'
Require ($runner -match 'nri_ptsmokegridbricks", "512"') 'fixture must test the production 512-brick capacity'
Require ($runner -match 'FarWindow|windows') 'fixture must visit the window pressure region'
Require ($runner -match 'Get-VisitCommand -Name "fire-first"' -and $runner -match 'Get-VisitCommand -Name "fire-return"') 'fixture must encode forward and reverse source ordering'
Require ($runner -match 'warptocoords 1040 3432 -6') 'fixture must target the E1L1 open-courtyard DukeFire actor'
Require ($runner -match '\+Move_Forward; wait 2; -Move_Forward') 'fixture must relink after the position-only warp command'

foreach ($field in @('appearance_ready', 'appearance_seen', 'activation_latched', 'cadence_active')) {
    Require ($emitter.Contains($field)) "actor authority telemetry is missing $field"
}
Require ($header -match 'bool appearanceObserved = false') 'surface appearance evidence must remain latched for attribution'
Require ($emitter -match 'event=actor-authority[\s\S]*duke_fire_sustained') 'DukeFire needs uncapped actor-specific authority transitions'
Require ($analyzer -match 'targetActor[\s\S]*actor = \[uint64\]405') 'analyzer must isolate the open-courtyard DukeFire from other E1L1 fires'
foreach ($classification in @('activation_actor_absent', 'activation_surface_absent', 'activation_latch_absent', 'cadence_command_absent', 'command_loss_before_grid', 'grid_probe_failure', 'grid_capacity_failure_with_free_bricks', 'occupied_exhaustion', 'medium_absent_after_admission')) {
    Require ($analyzer.Contains($classification)) "analyzer is missing classification $classification"
}
Require ($analyzer -match 'firstAdmissionFrame' -and $analyzer -match 'firstMediumFrame') 'analyzer must report first admission and first deposited-medium frames'
Require ($analyzer -match 'FarWindow.*NearWindow.*RoofLong.*RoofSquare') 'analyzer must require all four pressure sources'

Write-Host 'E1L1 smoke pressure fixture contracts passed.'
