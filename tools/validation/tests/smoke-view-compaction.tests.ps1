$ErrorActionPreference = 'Stop'

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Compact-Masks([UInt64[]]$Masks, [int]$Width, [int]$Height, [int]$Depth, [int]$Capacity) {
    $counts = @($Masks | ForEach-Object {
        $value = $_; $count = 0
        while ($value -ne 0) { $count += [int]($value -band 1); $value = $value -shr 1 }
        $count
    })
    $offsets = New-Object int[] $counts.Count
    $total = 0
    for ($column = 0; $column -lt $counts.Count; ++$column) {
        $offsets[$column] = $total; $total += $counts[$column]
    }
    if ($total -gt $Capacity) { return @{ Overflow = 1; ArgsX = 0; Indices = @() } }
    $indices = New-Object int[] $total
    for ($column = 0; $column -lt $Masks.Count; ++$column) {
        $write = $offsets[$column]; $x = $column % $Width; $y = [math]::Floor($column / $Width)
        for ($z = 0; $z -lt $Depth; ++$z) {
            if (($Masks[$column] -band ([UInt64]1 -shl $z)) -ne 0) {
                $indices[$write++] = ($z * $Height + $y) * $Width + $x
            }
        }
    }
    return @{ Overflow = 0; ArgsX = [math]::Ceiling($total / 64.0); Indices = @($indices) }
}

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$renderer = Get-Content (Join-Path $root 'source/common/rendering/nri/renderer/nri_smoke.cpp') -Raw
$owner = Get-Content (Join-Path $root 'source/common/rendering/nri/renderer/nri_smoke_view_work.cpp') -Raw
$prefix = Get-Content (Join-Path $root 'source/common/rendering/nri/shaders/SmokeViewWorkPrefixColumns.cs.hlsl') -Raw
$scatter = Get-Content (Join-Path $root 'source/common/rendering/nri/shaders/SmokeViewWorkScatterFroxels.cs.hlsl') -Raw
$compact = Get-Content (Join-Path $root 'source/common/rendering/nri/shaders/SmokeEvaluateGridCompact.cs.hlsl') -Raw

$a = Compact-Masks @([UInt64]0x5, [UInt64]0x2, [UInt64]0x9, [UInt64]0) 2 2 4 16
$b = Compact-Masks @([UInt64]0x5, [UInt64]0x2, [UInt64]0x9, [UInt64]0) 2 2 4 16
Assert-True (($a.Indices -join ',') -eq '0,8,5,2,14') 'Compaction must preserve column-major then ascending-depth order.'
Assert-True (($a.Indices -join ',') -eq ($b.Indices -join ',')) 'Compaction must be deterministic.'
Assert-True ($a.ArgsX -eq 1) 'Indirect groups must exactly cover compact count.'
$overflow = Compact-Masks @([UInt64]0xf, [UInt64]0xf) 2 1 4 7
Assert-True ($overflow.Overflow -eq 1 -and $overflow.ArgsX -eq 0 -and $overflow.Indices.Count -eq 0) 'Overflow must fail closed with zero indirect work.'
Assert-True ($owner -match 'CountColumns[\s\S]*PrefixColumns[\s\S]*ScatterFroxels[\s\S]*Finalize') 'GPU pass order must be Count, Prefix, Scatter, Finalize.'
Assert-True ($prefix -match '\[numthreads\(1,\s*1,\s*1\)\]' -and $prefix -match 'offset > gViewConstants.FroxelCapacity') 'Prefix must be correctness-first and capacity checked.'
Assert-True ($scatter -match '\(z \* gViewConstants.FroxelHeight \+ y\) \*[\s\S]*gViewConstants.FroxelWidth \+ x') 'Scatter must emit canonical dense froxel indices.'
Assert-True ($compact -match '#include "SmokeEvaluateGrid.cs.hlsl"' -and $compact -match 'SmokeEvaluateGridFroxel') 'Dense and compact routes must share exact evaluation math.'
Assert-True ($renderer -match 'viewRoute == 2u' -and $renderer -match 'CmdDispatchIndirect') 'Route 2 must dispatch compact materialization indirectly.'

Write-Host 'smoke-view-compaction.tests.ps1: PASS'
