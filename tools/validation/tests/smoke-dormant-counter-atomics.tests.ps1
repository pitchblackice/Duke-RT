$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '../../..')
$archiveShader = Get-Content (Join-Path $root 'source/common/rendering/nri/shaders/SmokeDormantGridArchive.cs.hlsl') -Raw
$rehydrateShader = Get-Content (Join-Path $root 'source/common/rendering/nri/shaders/SmokeDormantGridRehydrate.cs.hlsl') -Raw

foreach ($contract in @(
    @{ Name = 'archive attempts'; Text = $archiveShader; Pattern = 'InterlockedAdd\(gDormantControl\[0\]\.ArchiveAttempts, 1u' },
    @{ Name = 'archive published'; Text = $archiveShader; Pattern = 'InterlockedAdd\(gDormantControl\[0\]\.ArchivePublished, 1u' },
    @{ Name = 'archive max probe'; Text = $archiveShader; Pattern = 'InterlockedMax\(gDormantControl\[0\]\.MaximumArchiveProbe' },
    @{ Name = 'promote attempts'; Text = $rehydrateShader; Pattern = 'InterlockedAdd\(gDormantControl\[0\]\.RehydrateAttempts, 1u' },
    @{ Name = 'promote published'; Text = $rehydrateShader; Pattern = 'InterlockedAdd\(gDormantControl\[0\]\.RehydratePublished, 1u' },
    @{ Name = 'promote max probe'; Text = $rehydrateShader; Pattern = 'InterlockedMax\(gDormantControl\[0\]\.MaximumFineProbe' }
)) {
    if ($contract.Text -notmatch $contract.Pattern) {
        throw "Dormant smoke counter contract missing atomic $($contract.Name) update"
    }
}

Write-Host 'smoke-dormant-counter-atomics.tests: PASS'
