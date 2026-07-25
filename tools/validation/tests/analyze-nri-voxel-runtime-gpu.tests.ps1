Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '../../..')
$analyzer = Join-Path $repoRoot 'tools/validation/analyze-nri-voxel-runtime-gpu.ps1'
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('raze-voxel-runtime-gpu-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tempRoot | Out-Null

$validLog = @'
PERF pt voxel admission summary NRI: phase=runtime queued=2 ready=0 deferred=0 failed=0 uploaded=0 bytes_pending=200 bytes_uploaded=0 runtime_pending=2 direct_requests=1 direct_ready=0 direct_pending=1 direct_failures=0 cpu_geometry_avoided=0 cpu_geometry_fallback=0 required_admitted=0 optional_admitted=1 ms_used=0.200 blas_used=0 stop=scheduler-compute-lane
PERF pt voxel admission summary NRI: phase=runtime queued=1 ready=1 deferred=0 failed=0 uploaded=1 bytes_pending=100 bytes_uploaded=100 runtime_pending=1 direct_requests=0 direct_ready=1 direct_pending=0 direct_failures=0 cpu_geometry_avoided=1 cpu_geometry_fallback=0 required_admitted=0 optional_admitted=0 ms_used=0.100 blas_used=1 stop=none
PERF pt voxel runtime tail NRI: action=request frame=101 mesh_resource=0x1
PERF pt voxel runtime tail NRI: action=tlas-visible frame=102 mesh_resource=0x1 request_to_ready=1 ready_to_blas=1 blas_to_publish=0 publish_to_tlas=1 request_to_tlas=3
PERF pt gpu timing NRI: frame=10 nri_frame=1 segment=10.000 scene=9.000 trace=8.000 trace_dispatch=7.000 denoise=1.000 compose=1.000 upscale=0.000 final=0.000 segments=1 invalid=0 dropped=0 resolved=1 expected=1 compact=1 epoch=7 sample=0
PERF pt gpu timing NRI: frame=11 nri_frame=2 segment=17.000 scene=16.000 trace=12.000 trace_dispatch=11.000 denoise=2.000 compose=1.000 upscale=0.000 final=0.000 segments=1 invalid=0 dropped=0 resolved=1 expected=1 compact=1 epoch=7 sample=1
PERF pt gpu timing NRI: frame=12 nri_frame=3 segment=20.000 scene=19.000 trace=13.000 trace_dispatch=12.000 denoise=3.000 compose=1.000 upscale=0.000 final=0.000 segments=1 invalid=0 dropped=0 resolved=1 expected=1 compact=1 epoch=7 sample=2
PERF pt gpu timing NRI: frame=13 nri_frame=4 segment=12.000 scene=11.000 trace=9.000 trace_dispatch=8.000 denoise=1.000 compose=1.000 upscale=0.000 final=0.000 segments=1 invalid=0 dropped=0 resolved=1 expected=1 compact=1 epoch=7 sample=3
PERF pt voxel gpu timing NRI: renderer_frame=100 presentation_gen=10 queued_slot=0 segment=10.000000 segment_valid=1 admission=0.000000 upload=0.000000 arena_copy=0.000000 classify=0.000000 scan=0.000000 emit=0.000000 finalize=0.000000 voxel_blas=0.000000 world_tlas=0.300000 scopes=1 valid=1 invalid=0 dropped=0 compact=1 epoch=7 record=0
PERF pt voxel gpu timing NRI: renderer_frame=101 presentation_gen=11 queued_slot=1 segment=17.000000 segment_valid=1 admission=1.000000 upload=0.200000 arena_copy=0.000000 classify=0.400000 scan=0.100000 emit=0.600000 finalize=0.050000 voxel_blas=0.000000 world_tlas=0.300000 scopes=7 valid=7 invalid=0 dropped=0 compact=1 epoch=7 record=1
PERF pt voxel gpu timing NRI: renderer_frame=102 presentation_gen=12 queued_slot=2 segment=20.000000 segment_valid=1 admission=3.000000 upload=0.000000 arena_copy=0.000000 classify=2.000000 scan=0.500000 emit=2.500000 finalize=0.200000 voxel_blas=1.200000 world_tlas=0.300000 scopes=7 valid=7 invalid=0 dropped=0 compact=1 epoch=7 record=2
PERF pt voxel gpu timing NRI: renderer_frame=103 presentation_gen=13 queued_slot=0 segment=12.000000 segment_valid=1 admission=0.000000 upload=0.000000 arena_copy=0.000000 classify=0.000000 scan=0.000000 emit=0.000000 finalize=0.000000 voxel_blas=0.000000 world_tlas=0.300000 scopes=1 valid=1 invalid=0 dropped=0 compact=1 epoch=7 record=3
PERF compact capture complete: epoch=7 status=complete requested=4 eligible=4 observed=4 pending_gpu=0 dropped=0 first_use_records=0 first_use_pending=0 first_use_dropped=0 first_use_duplicates=0 first_use_unresolved=0 first_use_drain_frames=0 reason=none
'@

try {
    $validPath = Join-Path $tempRoot 'valid.log'
    $validSummary = Join-Path $tempRoot 'valid.json'
    [System.IO.File]::WriteAllText($validPath, $validLog)
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $analyzer -LogPath $validPath -TargetMs 16.667 -SummaryOutput $validSummary | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Valid voxel runtime GPU fixture failed.' }

    $summary = Get-Content -LiteralPath $validSummary -Raw | ConvertFrom-Json
    if (-not $summary.ok) { throw 'Valid summary did not report ok.' }
    if ($summary.completeGpuMs.distribution.p50 -ne 12.0 -or $summary.completeGpuMs.distribution.p95 -ne 20.0) {
        throw 'Complete-GPU percentiles are incorrect.'
    }
    if ($summary.completeGpuMs.targetMissCount -ne 2 -or $summary.completeGpuMs.targetMissPercent -ne 50.0) {
        throw 'Complete-GPU target misses are incorrect.'
    }
    if ($summary.stageTimingsMs.admission.nonzeroEventCount -ne 2 -or
        $summary.stageTimingsMs.admission.allFrames.p50 -ne 0.0 -or
        $summary.stageTimingsMs.admission.nonzeroEvents.p50 -ne 1.0 -or
        $summary.stageTimingsMs.admission.nonzeroEvents.p95 -ne 3.0) {
        throw 'Admission all-frame/nonzero distributions are incorrect.'
    }
    if ($summary.stageTimingsMs.upload.nonzeroEventCount -ne 1 -or
        $summary.stageTimingsMs.world_tlas.nonzeroEventCount -ne 4) {
        throw 'Stage nonzero-event counts are incorrect.'
    }
    if ($summary.timestampValidity.voxelScopes -ne 16 -or $summary.timestampValidity.voxelInvalid -ne 0 -or
        $summary.timestampValidity.voxelDropped -ne 0) {
        throw 'Voxel timestamp validity totals are incorrect.'
    }
    if ($summary.admissionTelemetry.rows -ne 2 -or $summary.admissionTelemetry.workRows -ne 2 -or
        $summary.admissionTelemetry.frameJoinAvailable) {
        throw 'Admission aggregate telemetry is incorrect.'
    }
    if ($summary.runtimeTailCorrelation.uniqueEventFrames -ne 2 -or
        $summary.runtimeTailCorrelation.matchedVoxelGpuFrames -ne 2 -or
        $summary.runtimeTailCorrelation.matchedFramesWithVoxelWork -ne 2 -or
        $summary.runtimeTailCorrelation.completeGpuTargetMissesOnMatchedFrames -ne 2) {
        throw 'Runtime-tail renderer-frame correlation is incorrect.'
    }
    if ($summary.runtimeTailCorrelation.latencyFrames.request_to_tlas.p50 -ne 3.0) {
        throw 'Runtime-tail latency telemetry is incorrect.'
    }

    $invalidPath = Join-Path $tempRoot 'invalid.log'
    $invalidSummary = Join-Path $tempRoot 'invalid.json'
    $invalidLog = $validLog.Replace(
        'scopes=7 valid=7 invalid=0 dropped=0 compact=1 epoch=7 record=2',
        'scopes=8 valid=7 invalid=1 dropped=0 compact=1 epoch=7 record=2')
    [System.IO.File]::WriteAllText($invalidPath, $invalidLog)
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $analyzer -LogPath $invalidPath -SummaryOutput $invalidSummary | Out-Null
    if ($LASTEXITCODE -eq 0) { throw 'Invalid timestamp fixture passed.' }
    $invalid = Get-Content -LiteralPath $invalidSummary -Raw | ConvertFrom-Json
    if ($invalid.ok -or $invalid.timestampValidity.voxelInvalid -ne 1) {
        throw 'Invalid timestamp fixture was not reported precisely.'
    }
}
finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host 'NRI voxel runtime GPU analyzer tests passed.'
