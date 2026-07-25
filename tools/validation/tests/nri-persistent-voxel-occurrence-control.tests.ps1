$ErrorActionPreference = "Stop"

function Assert-Match {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ($Text -notmatch $Pattern) {
        throw $Message
    }
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "../../..")
$cvars = Get-Content -Raw -LiteralPath (Join-Path $repoRoot "source/common/rendering/nri/renderer/nri_cvars.cpp")
$settingsHeader = Get-Content -Raw -LiteralPath (Join-Path $repoRoot "source/common/rendering/nri/renderer/nri_renderer_settings.h")
$settingsOwner = Get-Content -Raw -LiteralPath (Join-Path $repoRoot "source/common/rendering/nri/renderer/nri_renderer_settings.cpp")
$voxelOwner = Get-Content -Raw -LiteralPath (Join-Path $repoRoot "source/common/rendering/nri/renderer/nri_persistent_voxels.cpp")

Assert-Match $cvars 'CVAR\(Bool,\s*nri_ptvoxelomitoccurrences,\s*false,\s*0\)' `
    'The all-occurrence diagnostic must remain session-only and disabled by default.'
Assert-Match $settingsHeader 'bool\s+omitTlasOccurrences\s*=\s*false' `
    'Persistent voxel settings must carry the diagnostic explicitly.'
Assert-Match $settingsOwner 'settings\.omitTlasOccurrences\s*=\s*\(bool\)nri_ptvoxelomitoccurrences' `
    'The diagnostic CVar must be resolved through the focused settings owner.'
Assert-Match $voxelOwner 'omittedByDiagnostic\s*=\s*settings\.omitTlasOccurrences[\s\S]*?if\s*\(omittedByDiagnostic\s*\|\|\s*excludedByIndex\s*\|\|\s*excludedByPrimitiveCount\)' `
    'The control must omit TLAS occurrences at publication without disabling resident voxel ownership.'
Assert-Match $voxelOwner 'omittedByDiagnostic\s*\?\s*"diagnostic-omit-all"' `
    'Diagnostic omission must have an explicit trace reason.'

Write-Host "NRI persistent voxel occurrence-control contract tests passed."
