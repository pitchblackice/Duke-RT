$ErrorActionPreference = "Stop"

function Assert-Match {
    param([string]$Text, [string]$Pattern, [string]$Message)
    if ($Text -notmatch $Pattern) { throw $Message }
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "../../..")
$frameBuild = Get-Content -Raw -LiteralPath (Join-Path $repoRoot "source/common/rendering/nri/renderer/nri_scene_frame_build.cpp")
$dispatch = Get-Content -Raw -LiteralPath (Join-Path $repoRoot "source/common/rendering/nri/renderer/nri_pass_dispatch.cpp")
$captureOwner = Get-Content -Raw -LiteralPath (Join-Path $repoRoot "source/common/rendering/nri/system/nri_perf_capture.cpp")
$capture = Get-Content -Raw -LiteralPath (Join-Path $repoRoot "source/common/engine/perf_capture.cpp")

Assert-Match $frameBuild 'traceVoxelOccurrenceControl\s*=\s*persistentVoxelSettings\.omitTlasOccurrences\s*\?\s*1u\s*:\s*0u' `
    'The frame snapshot must retain the diagnostic occurrence-control identity.'
Assert-Match $dispatch 'workloadValues[\s\S]*?tracePerf\.traceVoxelOccurrenceControl' `
    'The workload key must distinguish the voxel-occurrence control.'
Assert-Match $captureOwner 'traceVoxelOccurrences\s*=\s*shell\.sceneInstancePersistentVoxelCount[\s\S]*?traceVoxelInstancePrimitives\s*=\s*shell\.persistentVoxelInstancePrimitiveCount' `
    'Compact capture must use authoritative current-frame occurrence and primitive counts.'
Assert-Match $capture 'PERF pt trace workload NRI:[^\n]*schema=3[^\n]*voxel_occurrences=%u[^\n]*voxel_instance_prims=%llu[^\n]*voxel_occurrence_control=%u' `
    'Trace workload schema 3 must print the voxel occurrence identity and population.'

Write-Host "NRI trace-workload voxel identity contract tests passed."
