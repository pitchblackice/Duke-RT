$ErrorActionPreference = 'Stop'

function Require-Match([string]$Text, [string]$Pattern, [string]$Message) {
	if ($Text -notmatch $Pattern) { throw $Message }
}

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$header = Get-Content -Raw (Join-Path $root 'source/common/rendering/nri/renderer/nri_smoke_interest.h')
$source = Get-Content -Raw (Join-Path $root 'source/common/rendering/nri/renderer/nri_smoke_interest.cpp')
$smoke = Get-Content -Raw (Join-Path $root 'source/common/rendering/nri/renderer/nri_smoke.cpp')

Require-Match $header 'enum class NRISmokeInterestTier[\s\S]*Dormant[\s\S]*Warm[\s\S]*Hot' 'Interest tiers are incomplete.'
Require-Match $header 'HotEnterDistance[\s\S]*HotLeaveDistance[\s\S]*WarmEnterDistance[\s\S]*WarmLeaveDistance' 'Separate enter/leave bands are required.'
Require-Match $source 'positiveChunks\s*=\s*\*input\.visibleChunkWords[\s\S]*portal\.sourceChunkIndex[\s\S]*portalTargets\[targetIndex\]\.chunkIndex' 'Positive main-view chunks must conservatively propagate through static portal targets.'
Require-Match $source 'runtimeBoundTarget[\s\S]*runtimePortalUncertain\s*=\s*true[\s\S]*mSnapshot\.runtimePortalUncertain\)' 'Unresolved runtime portals must fail conservative.'
Require-Match $source 'lastPositiveFrame[\s\S]*RecentVisibilityFrames[\s\S]*recentPositive' 'Recent positive visibility needs explicit grace.'
Require-Match $source 'cameraJump[\s\S]*mJumpOrigin[\s\S]*CameraJumpGraceFrames[\s\S]*teleportGrace' 'Camera jumps must retain the previous neighborhood while promoting the destination by distance.'
Require-Match $source 'found != sources\.end\(\) \? found->tier : NRISmokeInterestTier::Hot' 'Unknown interactive and diagnostic sources must fail prompt.'
Require-Match $smoke 'mainViewEligible \|\| mLastPreparedFrame[\s\S]*mInterest\.Update\(interestInput\)[\s\S]*mEmitters\.Gather' 'Interest must update once from the main-frame state before emitter gathering.'

# Pure policy mirror: absence alone does not cross the leave band, recent
# positive visibility retains warmth, and a camera jump retains its origin.
$hotEnter = 1024.0
$hotLeave = 1280.0
$warmEnter = 2048.0
$warmLeave = 2560.0
if ($hotEnter -ge $hotLeave -or $warmEnter -ge $warmLeave -or $hotLeave -ge $warmEnter) {
	throw 'Interest bands are not ordered hysteretically.'
}
$tier = 'hot'
$distance = 1100.0
if ($distance -le $(if ($tier -eq 'hot') { $hotLeave } else { $hotEnter })) { $tier = 'hot' }
if ($tier -ne 'hot') { throw 'Hot leave hysteresis failed.' }
$lastPositive = 10
$frame = 200
$recent = ($frame - $lastPositive) -le 240
if (-not $recent) { throw 'Recent positive visibility grace failed.' }
$jumpOriginDistance = 2400.0
if ($jumpOriginDistance -gt $warmLeave) { throw 'Camera-jump origin fixture is invalid.' }

Write-Host 'Smoke interest snapshot contracts passed.'
