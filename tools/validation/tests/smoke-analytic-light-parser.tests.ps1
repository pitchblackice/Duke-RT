$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$fixture = Join-Path $env:TEMP 'raze-smoke-analytic-light-parser.log'
$output = Join-Path $env:TEMP 'raze-smoke-analytic-light-parser.json'
try {
    @'
noise
PERF pt smoke analytic light NRI: source_frame=42 epoch=7 readback=valid profile=1 revision=6 implementation=2 events_requested=1 events_admitted=1 events_rejected=0 anchors_built=4 samples_executed=16 build_visibility_rays=16 apply_visibility_rays=0 apply_rays_zero=yes compact=1
'@ | Set-Content -LiteralPath $fixture
    & (Join-Path $root 'tools\validation\parse-smoke-analytic-light.ps1') `
        -InputPath $fixture -OutputPath $output
    $rows = @(Get-Content -Raw -LiteralPath $output | ConvertFrom-Json)
    if ($rows.Count -ne 1 -or $rows[0].source_frame -ne 42 -or
        $rows[0].samples_executed -ne 16 -or $rows[0].apply_visibility_rays -ne 0 -or
        $rows[0].apply_rays_zero -ne 'yes') {
        throw 'Analytic light parser did not preserve frame identity, work quantities, and ray invariant.'
    }
    Write-Host 'Smoke analytic light parser tests passed.'
}
finally {
    Remove-Item -LiteralPath $fixture, $output -ErrorAction SilentlyContinue
}
