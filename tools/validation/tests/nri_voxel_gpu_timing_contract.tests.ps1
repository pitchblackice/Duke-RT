Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$systemRoot = Join-Path $repoRoot 'source/common/rendering/nri/system'
$rendererRoot = Join-Path $repoRoot 'source/common/rendering/nri/renderer'
$script:passed = 0
$script:failed = 0

function Assert-VoxelGpuTimingContract {
    param([bool]$Condition, [string]$Message)

    if ($Condition) {
        $script:passed++
        Write-Host "PASS: $Message"
    }
    else {
        $script:failed++
        Write-Host "FAIL: $Message"
    }
}

function Get-Source {
    param([string]$Root, [string]$RelativePath)

    return Get-Content -LiteralPath (Join-Path $Root $RelativePath) -Raw
}

$timingHeader = Get-Source $systemRoot 'nri_gpu_timing.h'
$timingSource = Get-Source $systemRoot 'nri_gpu_timing.cpp'
$renderDevice = Get-Source $systemRoot 'nri_renderdevice.cpp'
$voxelCompute = Get-Source $rendererRoot 'nri_voxel_compute_meshing.cpp'
$voxelServices = Get-Source $rendererRoot 'nri_persistent_voxel_services.cpp'
$acceleration = Get-Source $rendererRoot 'nri_acceleration.cpp'

$scopeNames = @(
    'VoxelAdmission',
    'VoxelUpload',
    'VoxelArenaCopy',
    'VoxelClassify',
    'VoxelScan',
    'VoxelEmit',
    'VoxelFinalize',
    'VoxelBlas',
    'WorldTlas'
)

foreach ($scopeName in $scopeNames) {
    Assert-VoxelGpuTimingContract ($timingHeader -match "(?m)^\s*$scopeName,\s*$") "$scopeName has a dedicated GPU timing scope"
    Assert-VoxelGpuTimingContract ($timingSource -match "case NRIGpuTimingScope::${scopeName}:") "$scopeName is resolved into the deferred timing row"
}

$queryCapacityMatch = [regex]::Match($timingHeader, 'QueryCapacity\s*=\s*(\d+)')
$scopeCapacityMatch = [regex]::Match($timingHeader, 'ScopeCapacity\s*=\s*(\d+)')
$queryCapacity = if ($queryCapacityMatch.Success) { [int]$queryCapacityMatch.Groups[1].Value } else { 0 }
$scopeCapacity = if ($scopeCapacityMatch.Success) { [int]$scopeCapacityMatch.Groups[1].Value } else { 0 }
Assert-VoxelGpuTimingContract ($scopeCapacity -ge 31) 'timing owner reserves at least 31 scope pairs'
Assert-VoxelGpuTimingContract ($queryCapacity -ge (2 + (2 * $scopeCapacity))) 'query capacity covers the segment pair and every scope pair'

Assert-VoxelGpuTimingContract (
    $timingSource -match '(?s)void NRIGpuTiming::RetireSlot.*?core\.MapBuffer.*?PERF pt voxel gpu timing NRI:.*?PerfCompactCaptureResolveGpuSegment') `
    'voxel timing is read only during fence-retired slot processing'
Assert-VoxelGpuTimingContract (
    $timingSource -notmatch '\b(?:Wait|WaitIdle|DeviceWaitIdle|WaitForFenceValue)\s*\(') `
    'GPU timing ownership does not introduce a wait'
Assert-VoxelGpuTimingContract (
    $timingSource -match '(?s)BeginSegment\([^\)]*uint64_t rendererFrame\).*?slot\.rendererFrame\s*=\s*rendererFrame') `
    'segment recording captures renderer-frame identity'
Assert-VoxelGpuTimingContract (
    $renderDevice -match 'BeginSegment\(mCore, \*mCommandBuffer, mCurrentQueuedFrameIndex, mFrameIndex\)') `
    'the render device supplies renderer-frame identity at command-list begin'

$rowFields = @(
    'renderer_frame=%llu',
    'presentation_gen=%llu',
    'queued_slot=%u',
    'segment=%.6f',
    'segment_valid=%u',
    'admission=%.6f',
    'upload=%.6f',
    'arena_copy=%.6f',
    'classify=%.6f',
    'scan=%.6f',
    'emit=%.6f',
    'finalize=%.6f',
    'voxel_blas=%.6f',
    'world_tlas=%.6f',
    'scopes=%u',
    'valid=%u',
    'invalid=%u',
    'dropped=%u',
    'compact=1',
    'epoch=%llu',
    'record=%u'
)
foreach ($field in $rowFields) {
    Assert-VoxelGpuTimingContract ($timingSource.Contains($field)) "deferred voxel timing row contains $field"
}
Assert-VoxelGpuTimingContract (
    $timingSource -match 'if \(IsVoxelTimingScope\(scope\)\) slot\.droppedVoxelScopes\+\+') `
    'scope-capacity drops are explicit in voxel timing diagnostics'

Assert-VoxelGpuTimingContract (
    $voxelCompute -match '(?s)UploadPendingRawSourcePage.*?NRIGpuTimingScope::VoxelAdmission.*?NRIGpuTimingScope::VoxelUpload.*?CmdCopyBuffer') `
    'raw archive uploads contribute to admission and upload GPU timing'
Assert-VoxelGpuTimingContract (
    $voxelCompute -match '(?s)CompletionSlotRecordingGuard recordingGuard.*?NRIGpuTimingScope::VoxelAdmission.*?CmdDispatch') `
    'accepted compute work has an aggregate admission scope'
Assert-VoxelGpuTimingContract (
    $voxelCompute -match '(?s)NRIGpuTimingScope::VoxelUpload.*?slot\.jobBuffer.*?CmdCopyBuffer') `
    'compute job uploads have a dedicated upload scope'

$parallelStageMappings = @(
    @('VoxelComputeClassify', 'VoxelClassify'),
    @('VoxelComputeScan', 'VoxelScan'),
    @('VoxelComputeEmitParallel', 'VoxelEmit'),
    @('VoxelComputeFinalize', 'VoxelFinalize')
)
foreach ($mapping in $parallelStageMappings) {
    $pipeline = $mapping[0]
    $timingScope = $mapping[1]
    Assert-VoxelGpuTimingContract (
        $voxelCompute -match "(?s)PipelineSlot::$pipeline,\s*NRIGpuTimingScope::$timingScope,") `
        "$pipeline dispatch maps to $timingScope timing"
}
Assert-VoxelGpuTimingContract (
    $voxelCompute -match '(?s)emit \? NRIGpuTimingScope::VoxelEmit : NRIGpuTimingScope::VoxelScan.*?CmdDispatch') `
    'serial emit/count dispatches map to emit/scan timing'

Assert-VoxelGpuTimingContract (
    $voxelServices -match '(?s)PersistentVoxelArenaCopyRequired.*?alreadyCompatible.*?resource\.usedSize != 0.*?recordsCopy \? renderer\.mFrameBuffer : nullptr, NRIGpuTimingScope::VoxelAdmission.*?NRIGpuTimingScope::VoxelArenaCopy') `
    'arena-copy timing is conditional on preserving resident GPU data'
Assert-VoxelGpuTimingContract (
    $voxelServices -match '(?s)recordsUpload \? renderer\.mFrameBuffer : nullptr, NRIGpuTimingScope::VoxelAdmission.*?NRIGpuTimingScope::VoxelUpload.*?StageResidentBufferCopyRange') `
    'resident buffer copies contribute to admission and upload timing'
Assert-VoxelGpuTimingContract (
    ([regex]::Matches($voxelServices, 'NRIGpuTimingScope::VoxelBlas')).Count -ge 2) `
    'direct and shared voxel BLAS build paths have GPU timing scopes'

Assert-VoxelGpuTimingContract (
    $acceleration -match '(?s)NRIGpuTimingScope::WorldTlas.*?"Raze\.WorldTLAS\.Build".*?CmdBuildTopLevelAccelerationStructures') `
    'world TLAS build has a GPU timing scope'
Assert-VoxelGpuTimingContract (
    $acceleration -match '(?s)NRIGpuTimingScope::WorldTlas.*?"Raze\.WorldTLAS\.Update".*?CmdBuildTopLevelAccelerationStructures') `
    'world TLAS update has a GPU timing scope'

Write-Host "Voxel GPU timing contract tests complete: passed=$script:passed failed=$script:failed"
if ($script:failed -ne 0) { exit 1 }
