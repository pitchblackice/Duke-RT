$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
function Read-Source([string]$Path) { Get-Content -Raw (Join-Path $root $Path) }
function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
	if ($Text -notmatch $Pattern) { throw $Message }
}

$cvars = Read-Source 'source/common/rendering/nri/renderer/nri_cvars.cpp'
$settings = Read-Source 'source/common/rendering/nri/renderer/nri_renderer_settings.cpp'
$scheduler = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_work_scheduler.cpp'
$schedulerHeader = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_work_scheduler.h'
$runtime = Read-Source 'source/common/rendering/nri/renderer/nri_smoke.cpp'
$menu = Read-Source 'wadsrc/static/menudef.txt'
$seed = Read-Source 'source/common/rendering/nri/shaders/SmokeGridLightSeed.cs.hlsl'

Assert-Match $cvars 'CVAR\(Int, nri_ptsmokeworkprofile, 0, CVAR_ARCHIVE \| CVAR_GLOBALCONFIG\)' 'Work profile must be archived and default to Reference.'
Assert-Match $settings 'settings\.workProfile\s*=\s*\(uint32_t\)std::max\(\(int\)nri_ptsmokeworkprofile,\s*0\)' 'Captured settings must carry the requested profile.'
Assert-Match $menu 'Option\s+"Smoke Work Profile",\s*"nri_ptsmokeworkprofile",\s*"NRISmokeWorkProfile"' 'Video options must expose the smoke work profile.'
Assert-Match $menu 'OptionValue NRISmokeWorkProfile[\s\S]*0, "Reference"[\s\S]*1, "High"[\s\S]*2, "Medium"[\s\S]*3, "Low"' 'Menu values must match the runtime profile ABI.'
Assert-Match $schedulerHeader 'no GPU[\s\S]*timing[\s\S]*renderer-headroom[\s\S]*target-frame-time[\s\S]*predicted-cost' 'Scheduler contract must reject adaptive timing inputs explicitly.'
Assert-Match $scheduler 'NRISmokeWorkProfile::High[\s\S]*emissionCommands = 128u[\s\S]*NRISmokeWorkProfile::Medium[\s\S]*emissionCommands = 32u[\s\S]*NRISmokeWorkProfile::Low[\s\S]*emissionCommands = 16u' 'Static profile command tables are missing.'
Assert-Match $schedulerHeader 'admissionBrickRequests = Unrestricted[\s\S]*depositionCellVisits = Unrestricted[\s\S]*projectionWorkUnits = Unrestricted[\s\S]*materializedFroxels = Unrestricted[\s\S]*worldLinkRays = Unrestricted[\s\S]*directReceiverSamples = Unrestricted[\s\S]*dormantPromotions = Unrestricted' 'Unsupported GPU families must not advertise fake caps.'
Assert-Match $runtime 'maximumCommands\s*=\s*std::min\(kMaxCommands, workTable\.emissionCommands\)' 'Emission transport must apply the fixed profile quantity.'
Assert-Match $runtime 'mPromptFallback\.Prepare\([\s\S]*workTable\.firstUseSources' 'Prompt first-use must apply the fixed profile quantity.'
Assert-Match $runtime 'std::min\(dueSubsteps, workTable\.simulationSubsteps\)[\s\S]*RecordSimulation\(dueSubsteps, substeps, debtSubsteps\)' 'Simulation must cap work while retaining explicit debt.'
Assert-Match $seed 'NRI_SMOKE_GRID_LIGHT_WORK_LIMITED[\s\S]*SmokeGridLightClaimNewInvalid[\s\S]*SmokeGridLightClaimMaintenance' 'Static profiles must activate both existing radiance ticket lanes.'
Assert-Match $runtime 'PERF pt smoke schedule NRI:[\s\S]*unsupported_unrestricted=%u' 'Compact telemetry must publish the fixed table and unsupported sentinel.'

Write-Host 'Smoke work profile structural tests passed.'
