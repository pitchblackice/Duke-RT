$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
function Read-Source([string]$Path) { Get-Content -Raw (Join-Path $root $Path) }
function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
	if ($Text -notmatch $Pattern) { throw $Message }
}

$cvars = Read-Source 'source/common/rendering/nri/renderer/nri_cvars.cpp'
$settings = Read-Source 'source/common/rendering/nri/renderer/nri_renderer_settings.cpp'
$settingsHeader = Read-Source 'source/common/rendering/nri/renderer/nri_renderer_settings.h'
$scheduler = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_work_scheduler.cpp'
$schedulerHeader = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_work_scheduler.h'
$runtime = Read-Source 'source/common/rendering/nri/renderer/nri_smoke.cpp'
$menu = Read-Source 'wadsrc/static/menudef.txt'
$seed = Read-Source 'source/common/rendering/nri/shaders/SmokeGridLightSeed.cs.hlsl'

Assert-Match $cvars 'CVAR\(Int, nri_ptsmokeworkprofile, 0, CVAR_ARCHIVE \| CVAR_GLOBALCONFIG\)' 'Work profile must be archived and default to Reference.'
Assert-Match $cvars 'CVAR\(Bool, nri_ptsmokedormantgrid, true, CVAR_ARCHIVE \| CVAR_GLOBALCONFIG\)' 'Bounded dormant smoke residency must be enabled by default.'
Assert-Match $settingsHeader 'bool dormantGrid = true;' 'Captured smoke settings must agree with the dormant residency CVar default.'
Assert-Match $settings 'settings\.workProfile\s*=\s*\(uint32_t\)std::max\(\(int\)nri_ptsmokeworkprofile,\s*0\)' 'Captured settings must carry the requested profile.'
Assert-Match $menu 'Option\s+"Smoke Work Profile",\s*"nri_ptsmokeworkprofile",\s*"NRISmokeWorkProfile"' 'Video options must expose the smoke work profile.'
Assert-Match $menu 'OptionValue NRISmokeWorkProfile[\s\S]*0, "Reference"[\s\S]*1, "High"[\s\S]*2, "Medium"[\s\S]*3, "Low"' 'Menu values must match the runtime profile ABI.'
Assert-Match $schedulerHeader 'no GPU[\s\S]*timing[\s\S]*renderer-headroom[\s\S]*target-frame-time[\s\S]*predicted-cost' 'Scheduler contract must reject adaptive timing inputs explicitly.'
Assert-Match $scheduler 'NRISmokeWorkProfile::High[\s\S]*emissionCommands = 128u[\s\S]*NRISmokeWorkProfile::Medium[\s\S]*emissionCommands = 32u[\s\S]*NRISmokeWorkProfile::Low[\s\S]*emissionCommands = 16u' 'Static profile command tables are missing.'
Assert-Match $scheduler 'NRISmokeWorkProfile::High[\s\S]*froxelPixelSize = 16u[\s\S]*froxelDepth = 48u' 'High static froxel table is missing.'
Assert-Match $scheduler 'NRISmokeWorkProfile::Medium[\s\S]*froxelPixelSize = 24u[\s\S]*froxelDepth = 32u' 'Medium static froxel table is missing.'
Assert-Match $scheduler 'NRISmokeWorkProfile::Low[\s\S]*froxelPixelSize = 32u[\s\S]*froxelDepth = 24u' 'Low static froxel table is missing.'
Assert-Match $scheduler 'NRISmokeWorkProfile::Low[\s\S]*emissiveLights = 0u[\s\S]*emissiveBackend = 0u[\s\S]*lightSamples = 1u[\s\S]*maximumLightCandidates = 4u' 'Low static lighting table is missing.'
Assert-Match $schedulerHeader 'admissionBrickRequests = Unrestricted[\s\S]*depositionCellVisits = Unrestricted[\s\S]*projectionWorkUnits = Unrestricted[\s\S]*materializedFroxels = Unrestricted[\s\S]*worldLinkRays = Unrestricted[\s\S]*directReceiverSamples = Unrestricted' 'Unsupported GPU families must not advertise fake caps.'
Assert-Match $scheduler 'NRISmokeWorkProfile::High[\s\S]*dormantArchives = 8u[\s\S]*dormantPromotions = 8u[\s\S]*dormantEvolution = 32u[\s\S]*NRISmokeWorkProfile::Medium[\s\S]*dormantArchives = 4u[\s\S]*dormantPromotions = 4u[\s\S]*dormantEvolution = 16u[\s\S]*NRISmokeWorkProfile::Low[\s\S]*dormantArchives = 2u[\s\S]*dormantPromotions = 2u[\s\S]*dormantEvolution = 8u' 'Dormant residency must use immutable profile quantities.'
Assert-Match $runtime 'maximumCommands\s*=\s*std::min\(kMaxCommands, workTable\.emissionCommands\)' 'Emission transport must apply the fixed profile quantity.'
Assert-Match $runtime 'mPromptFallback\.Prepare\([\s\S]*workTable\.firstUseSources' 'Prompt first-use must apply the fixed profile quantity.'
Assert-Match $runtime 'std::min\(dueSubsteps, workTable\.simulationSubsteps\)[\s\S]*RecordSimulation\(dueSubsteps, substeps, debtSubsteps\)' 'Simulation must cap work while retaining explicit debt.'
Assert-Match $runtime 'workSchedule\.table\.froxelPixelSize[\s\S]*mSettings\.froxelPixelSize[\s\S]*workSchedule\.table\.froxelDepth[\s\S]*mSettings\.froxelDepth' 'Profiles must resolve static froxel dimensions before resource preparation.'
Assert-Match $runtime 'workSchedule\.table\.emissiveLights[\s\S]*mSettings\.emissiveLights[\s\S]*workSchedule\.table\.lightSamples[\s\S]*mSettings\.lightSamples' 'Profiles must resolve static lighting quantities before resource preparation.'
Assert-Match $seed 'NRI_SMOKE_GRID_LIGHT_WORK_LIMITED[\s\S]*SmokeGridLightClaimNewInvalid[\s\S]*SmokeGridLightClaimMaintenance' 'Static profiles must activate both existing radiance ticket lanes.'
Assert-Match $runtime 'PERF pt smoke schedule NRI:[\s\S]*unsupported_unrestricted=%u' 'Compact telemetry must publish the fixed table and unsupported sentinel.'

Write-Host 'Smoke work profile structural tests passed.'
