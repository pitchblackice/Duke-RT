$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$sourcePath = Join-Path $repoRoot 'source\common\rendering\nri\renderer\nri_voxel_compute_meshing.cpp'
$source = Get-Content -LiteralPath $sourcePath -Raw

function Require-Contract {
	param([bool]$Condition, [string]$Message)
	if (-not $Condition) {
		throw $Message
	}
}

function Get-FunctionBody {
	param([string]$Start, [string]$End)
	$startIndex = $source.IndexOf($Start, [StringComparison]::Ordinal)
	$endIndex = $source.IndexOf($End, $startIndex + $Start.Length, [StringComparison]::Ordinal)
	Require-Contract ($startIndex -ge 0 -and $endIndex -gt $startIndex) "Could not isolate $Start"
	return $source.Substring($startIndex, $endIndex - $startIndex)
}

$consumptionGate = Get-FunctionBody `
	'bool IsConsumptionEnabled()' `
	'bool IsDirectGpuPublicationEnabled()'
$consumePolicy = Get-FunctionBody `
	'bool ShouldConsumeNRIVoxelComputeMeshing()' `
	'bool ShouldDirectPublishNRIVoxelComputeMeshing()'
$directPolicy = Get-FunctionBody `
	'bool ShouldDirectPublishNRIVoxelComputeMeshing()' `
	'NRIVoxelComputeGeneratedGeometryStatus RequestNRIVoxelComputeGeneratedGeometry('
$queueBody = Get-FunctionBody `
	'void QueueNRIVoxelComputeCountJob(' `
	'void DispatchNRIVoxelComputeMeshingDiagnostics('

Require-Contract ($consumptionGate.Contains('(int)nri_ptvoxelcomputemode >= 6')) `
	'Production consumption must remain mode six or higher.'
Require-Contract ($consumePolicy.Contains('IsConsumptionEnabled()') -and
	$consumePolicy.Contains('!(bool)nri_ptvoxelcomputeforcecpu')) `
	'Production consumption must retain the mode-six and force-CPU gates.'
foreach ($gate in @(
	'ShouldConsumeNRIVoxelComputeMeshing()',
	'IsDirectGpuPublicationEnabled()',
	'IsDirectPublicationEnabled()',
	'IsRawSourceArchiveEnabled()',
	'!IsFullGeneratedReadbackEnabled()')) {
	Require-Contract $directPolicy.Contains($gate) "Direct-publication policy lost gate: $gate"
}

$archiveIndex = $queueBody.IndexOf('RecordRawSourceArchive(', [StringComparison]::Ordinal)
$suppressIndex = $queueBody.IndexOf(
	'archivedSource != nullptr && ShouldDirectPublishNRIVoxelComputeMeshing()',
	[StringComparison]::Ordinal)
$archiveCopyIndex = $queueBody.IndexOf('CopyRawArchiveRecords(', [StringComparison]::Ordinal)
$queueIndex = $queueBody.IndexOf('queuedJobs.push_back(', [StringComparison]::Ordinal)
Require-Contract ($archiveIndex -ge 0 -and $archiveIndex -lt $suppressIndex) `
	'The production-direct suppression gate must run only after successful archive recording is attempted.'
Require-Contract ($suppressIndex -lt $archiveCopyIndex -and $archiveCopyIndex -lt $queueIndex) `
	'The suppression gate must precede diagnostic record copies and queue publication.'
Require-Contract ($queueBody.Contains('diagnosticSidecarsSuppressed == 1')) `
	'Diagnostic suppression tracing must remain bounded to one row per meshing-owner lifetime.'

Write-Host 'NRI voxel compute diagnostic-sidecar tests passed.'
