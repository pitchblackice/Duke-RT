param([string]$OutputDirectory)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$runnerPath = Join-Path $repo 'tools\validation\run-nri-trace-profile-quality.ps1'
$comparatorPath = Join-Path $repo 'tools\validation\compare-nri-srgb-png.ps1'
$runner = Get-Content -LiteralPath $runnerPath -Raw

function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}
function Assert-NotMatch([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -match $Pattern) { throw $Message }
}

Assert-Match $runner "label = 'default-a'; light = 4; mirror = 8; portal = 6; emissive = 4; budget = 2" 'Default A profile is not explicit.'
Assert-Match $runner "label = 'candidate'; light = 2; mirror = 2; portal = 3; emissive = 1; budget = 2" 'Candidate profile is not explicit.'
Assert-Match $runner "label = 'default-b'; light = 4; mirror = 8; portal = 6; emissive = 4; budget = 2" 'Default B profile is not explicit.'
Assert-Match $runner "processOrder = @\('default-a', 'candidate', 'default-b'\)" 'The quality pair is not bracketed by controls.'
Assert-Match $runner 'linearHdrEvidence = \$false' 'The runner must reject a linear-HDR evidence claim.'
Assert-NotMatch $runner 'nri_ptspatial(?:route|scene|dynamic|secondary)' 'The current-path quality runner must not depend on SPATIAL controls.'

$testRoot = if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    Join-Path $env:TEMP ('nri-trace-profile-quality-' + [Guid]::NewGuid().ToString('N'))
} else {
    [System.IO.Path]::GetFullPath($OutputDirectory)
}
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
Add-Type -AssemblyName System.Drawing
$referencePath = Join-Path $testRoot 'reference.png'
$candidatePath = Join-Path $testRoot 'candidate.png'
foreach ($spec in @(
    [pscustomobject]@{ path = $referencePath; color = [Drawing.Color]::FromArgb(32, 64, 96) },
    [pscustomobject]@{ path = $candidatePath; color = [Drawing.Color]::FromArgb(48, 64, 96) }
)) {
    $bitmap = [Drawing.Bitmap]::new(4, 4)
    try {
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try { $graphics.Clear($spec.color) } finally { $graphics.Dispose() }
        $bitmap.Save($spec.path, [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $bitmap.Dispose()
    }
}
$summaryPath = Join-Path $testRoot 'summary.json'
$differencePath = Join-Path $testRoot 'difference.png'
& $comparatorPath -ReferencePath $referencePath -CandidatePath $candidatePath `
    -SummaryOutput $summaryPath -DifferencePath $differencePath | Out-Null
$summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
if (-not $summary.ok -or $summary.linearHdrEvidence) { throw 'Comparator evidence classification is incorrect.' }
if ([int]$summary.changedPixelCount -ne 16) { throw 'Comparator did not report all changed pixels.' }
if ([double]$summary.meanAbsoluteLuminanceError -le 0.0) { throw 'Comparator did not report a luminance difference.' }
if (-not (Test-Path -LiteralPath $differencePath)) { throw 'Comparator did not publish a difference image.' }

Write-Host 'NRI trace-profile quality contracts passed.'
