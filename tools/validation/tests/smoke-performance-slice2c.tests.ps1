$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
function Read-Source([string]$Path) { Get-Content -Raw (Join-Path $root $Path) }
function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}
function Assert-NotMatch([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -match $Pattern) { throw $Message }
}

$sourceDir = Join-Path $root 'source\common\rendering\nri\renderer'
$policy = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_grid_reserve_policy.h'
$data = Read-Source 'source/common/rendering/nri/shaders/Include/SmokeGridData.hlsli'
$resources = Read-Source 'source/common/rendering/nri/shaders/Include/SmokeGridResources.hlsli'
$allocate = Read-Source 'source/common/rendering/nri/shaders/SmokeGridAllocateCommands.cs.hlsl'
$rebuild = Read-Source 'source/common/rendering/nri/shaders/SmokeGridRebuild.cs.hlsl'
$runtime = Read-Source 'source/common/rendering/nri/renderer/nri_smoke.cpp'

Assert-Match $policy 'FIRST_USE_CORE_DIVISOR\s*=\s*16u' 'The CPU policy must document a one-sixteenth soft first-use core.'
Assert-Match $data 'NRI_SMOKE_GRID_FIRST_USE_CORE_DIVISOR\s+16u' 'The HLSL policy must mirror the one-sixteenth soft core.'
Assert-Match $allocate 'freeBeforeAllocation\s*<=\s*SmokeGridFirstUseCoreCapacity\(\)[\s\S]*BRICK_BORROWED_FIRST_USE[\s\S]*BorrowedAllocations\+\+' 'Ambient allocations may consume the soft core but must be marked borrowed.'
Assert-Match $resources 'opticallyDormant\s*=\s*candidate\.State\s*==\s*NRI_SMOKE_GRID_RESIDENT[\s\S]*BRICK_HALO[\s\S]*BRICK_CONTENT\)\s*==\s*0u[\s\S]*candidate\.IdleFrames\s*>\s*0u' 'Replacement must require prior-frame authoritative optical dormancy.'
Assert-Match $resources 'candidate\.HashSlot\s*!=\s*destination[\s\S]*NRI_SMOKE_GRID_TOMBSTONE[\s\S]*replacement\.State\s*=\s*NRI_SMOKE_GRID_NEW' 'Replacement must preserve the hash chain and publish through NEW state.'
Assert-Match $resources 'SmokeGridPromoteBorrowedBrickSerial[\s\S]*BorrowedReturns\+\+[\s\S]*BorrowedPromotions\+\+' 'Shared borrowed bricks must be promoted rather than duplicated.'
Assert-Match $allocate 'SmokeGridFindBrickSerial[\s\S]*SmokeGridPromoteBorrowedBrickSerial[\s\S]*ExistingHits\+\+' 'Shared-brick promotion must happen on the existing-hit path.'
Assert-Match $rebuild 'retainedPolicyFlags[\s\S]*BRICK_CONTENT[\s\S]*retainedPolicyFlags[\s\S]*BRICK_HALO' 'Simulation must preserve borrowed ownership until promotion or reclamation.'
Assert-Match $runtime 'PERF pt smoke first use NRI:[\s\S]*borrowed_resident=%u[\s\S]*blocked_visible_total=%u[\s\S]*capacity_failures_delta=%u' 'Compact telemetry must expose borrowed, returned, blocked, and failure outcomes.'
Assert-NotMatch ($resources + $allocate) 'Camera|Frustum|DistanceToCamera|ViewDistance' 'Reserve recovery must not use camera or distance eviction.'

$replaceStart = $resources.IndexOf('bool SmokeGridTryReplaceBorrowedDormantSerial')
$replaceEnd = $resources.IndexOf('bool SmokeGridAllocateAtSlotSerial', $replaceStart)
if ($replaceStart -lt 0 -or $replaceEnd -le $replaceStart) { throw 'Could not isolate borrowed replacement implementation.' }
$replace = $resources.Substring($replaceStart, $replaceEnd - $replaceStart)
Assert-NotMatch $replace 'SmokeGridAppendActive|SmokeGridPushFree|SmokeGridPopFreeSerial' 'Synchronous replacement must reuse the existing active-list slot without duplicating an index.'

$testSource = Join-Path $PSScriptRoot 'nri_smoke_grid_reserve_policy.tests.cpp'
$outputDir = Join-Path $root 'build\smoke-grid-reserve-tests'
$testExe = Join-Path $outputDir 'nri_smoke_grid_reserve_policy.tests.exe'
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$compile = 'call "{0}" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /I"{1}" /Fo"{4}/" "{2}" /Fe:"{3}"' -f `
    $vsDevCmd, $sourceDir, $testSource, $testExe, $outputDir
cmd /c $compile
if ($LASTEXITCODE -ne 0) { throw "Smoke reserve policy test compilation failed with exit code $LASTEXITCODE." }
& $testExe
if ($LASTEXITCODE -ne 0) { throw "Smoke reserve policy tests failed with exit code $LASTEXITCODE." }

Write-Host 'Smoke Slice 2C structural and native policy tests passed.'
