$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$emitters = Get-Content -Raw (Join-Path $root 'source\common\rendering\nri\renderer\nri_smoke_emitters.cpp')
$emitterHeader = Get-Content -Raw (Join-Path $root 'source\common\rendering\nri\renderer\nri_smoke_emitters.h')
$spawn = Get-Content -Raw (Join-Path $root 'source\common\rendering\nri\shaders\SmokeSpawn.cs.hlsl')
$sourceShaping = Get-Content -Raw (Join-Path $root 'source\common\rendering\nri\shaders\Include\SmokeSourceShaping.hlsli')
$device = Get-Content -Raw (Join-Path $root 'source\common\rendering\nri\system\nri_renderdevice.cpp')
$overlay = Get-Content -Raw (Join-Path $root 'source\core\lightoverlay.cpp')

function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}

Assert-Match $emitterHeader 'const TArray<PathTracingWeaponLightEvent>& weaponEvents' 'Smoke emitters must consume the immutable shared weapon-event batch.'
Assert-Match $emitters 'event\.eventId\.CompareNoCase\(rule\.id\)' 'Smoke event IDs must resolve case-insensitively.'
Assert-Match $emitters 'BuildSmokeOffsetRandomSeed\(event\)[\s\S]*rule\.offsetRandom\[0\][\s\S]*event\.basisRight \* resolvedOffset\[0\][\s\S]*event\.basisForward \* resolvedOffset\[1\][\s\S]*event\.basisUp \* resolvedOffset\[2\]' 'Smoke event offsets and jitter must use one event-local basis sample.'
Assert-Match $emitters '"smoke-event-offset"[\s\S]*HashCombine64\(hash, event\.serial\)[\s\S]*event\.emitterActorIndex \+ 1[\s\S]*event\.eventId\.GetChars' 'Smoke event jitter identity must use a domain tag, gameplay serial, emitter identity, and event ID.'
Assert-Match $emitters "\*cursor - 'A' \+ 'a'" 'Smoke event jitter identity must normalize ASCII event IDs case-insensitively.'
if ($emitters -match 'BuildSmokeOffsetRandomSeed[\s\S]*renderer\.mFrameIndex') { throw 'Smoke event jitter must not depend on renderer frame order.' }
Assert-Match $emitters 'outPosition\[1\]\s*=\s*\(float\)-worldPosition\.Z' 'Smoke event positions must use canonical path-tracing Y.'
Assert-Match $emitters 'outPosition\[2\]\s*=\s*\(float\)-worldPosition\.Y' 'Smoke event positions must use canonical path-tracing Z.'
Assert-Match $emitters 'worldVelocity = event\.basisForward\.Unit\(\)[\s\S]*WorldToPathTracingDirection\(worldVelocity, command\.velocity\)' 'Smoke event aim/fallback direction must come from the canonicalized event forward basis.'
Assert-Match $emitters 'command\.serial\s*=\s*nextSerial\+\+' 'Every actor, synthetic, and weapon smoke command must share one monotonic random-seed serial.'
Assert-Match $emitters 'source_serial=%llu command_serial=%u' 'Smoke event traces must preserve source-event provenance independently of the command serial.'
Assert-Match $emitters 'command\.velocityCone\s*=\s*rule\.velocityCone' 'Smoke event cone authoring must reach the GPU command.'
Assert-Match $emitters 'event=weapon-frame-summary source_events=%u commands=%u particles=%u' 'Smoke event tracing must expose fan-out totals.'
Assert-Match $emitters 'event=weapon-ignored source_event=%s source_serial=%llu reason=no-rule' 'Smoke tracing must expose independent missing-rule skips.'
if ($emitters -match 'commands\.size\(\)\s*>=\s*256u') { throw 'Emitter gathering must preserve over-cap commands so upload-drop telemetry remains accurate.' }
if (([regex]::Matches($overlay, 'rule\.count\s*=\s*\(uint32_t\)std::clamp\(sc\.Number, 1, 256\)')).Count -lt 2) { throw 'Actor and event smoke counts must match the GPU per-command cap.' }

Assert-Match $sourceShaping 'velocityLengthSquared\s*<=\s*1e-8[\s\S]*return sphericalDirection' 'Smoke spawn must retain isotropic fallback for commands without a direction.'
Assert-Match $sourceShaping 'clamp\(SmokeSourceFinite\(velocityCone, 0\.0\), 0\.0, 180\.0\)' 'Smoke spawn must bound the authored velocity cone.'
Assert-Match $sourceShaping 'cosTheta\s*=\s*lerp\(1\.0, coneCosine, SmokeRandom01\(randomState\)\)' 'Smoke cone samples must be uniform in solid angle.'
Assert-Match $sourceShaping 'return coneAxis \* cosTheta' 'Smoke cone samples must be oriented around the event direction.'
Assert-Match $spawn 'velocityDirection \* style\.VelocityRandom' 'Smoke velocity variation must use the cone direction.'
Assert-Match $sourceShaping 'const float z[\s\S]*const float phi[\s\S]*const float radius' 'Smoke spawn-radius samples must use an isotropic spherical direction.'
Assert-Match $device 'nri_ptsmoke_test <event_rule_id>[\s\S]*FindResolvedSmokeEventRule[\s\S]*EmitPathTracingWeaponLightEvent' 'Smoke event test command must enter the shared weapon-event queue.'

Write-Host 'Smoke event runtime static validation passed.'
