[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Assert-Contract {
	param(
		[Parameter(Mandatory = $true)][bool]$Condition,
		[Parameter(Mandatory = $true)][string]$Message
	)

	if (-not $Condition) {
		throw $Message
	}
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "../../..")
$cvars = Get-Content -LiteralPath (Join-Path $repoRoot "source/common/rendering/nri/renderer/nri_cvars.cpp") -Raw
$config = Get-Content -LiteralPath (Join-Path $repoRoot "source/core/gameconfigfile.cpp") -Raw
$gameControl = Get-Content -LiteralPath (Join-Path $repoRoot "source/core/gamecontrol.cpp") -Raw

$profiles = [ordered]@{
	nri_ptlightbounces = [pscustomobject]@{ old = 4; new = 2 }
	nri_ptmirrorbounces = [pscustomobject]@{ old = 8; new = 2 }
	nri_ptportaldepth = [pscustomobject]@{ old = 6; new = 3 }
	nri_ptemissivesamples = [pscustomobject]@{ old = 4; new = 1 }
}

foreach ($entry in $profiles.GetEnumerator()) {
	$definition = 'CVAR\(Int,\s*' + [regex]::Escape($entry.Key) + ',\s*' + $entry.Value.new + ',\s*CVAR_ARCHIVE\s*\|\s*CVAR_GLOBALCONFIG\)'
	Assert-Contract -Condition ($cvars -match $definition) -Message "$($entry.Key) must default to $($entry.Value.new)"
}

Assert-Contract -Condition ($config -match '#define\s+LASTRUNVERSION\s+"8"') -Message "the trace-profile migration must advance the config version to 8"
$migrationStart = $config.IndexOf('if (last < 8)')
$globalSetupEnd = $config.IndexOf('void FGameConfigFile::DoGameSetup', $migrationStart)
Assert-Contract -Condition ($migrationStart -ge 0 -and $globalSetupEnd -gt $migrationStart) -Message "the version 8 trace-profile migration is missing"
$migrationBody = $config.Substring($migrationStart, $globalSetupEnd - $migrationStart)

Assert-Contract -Condition ($migrationBody -match 'GetGenericRep\(CVAR_Int\)\.Int\s*==\s*oldValue') -Message "the version 8 migration must preserve non-legacy custom values"
foreach ($entry in $profiles.GetEnumerator()) {
	$assignment = 'migrateIntCVar\("' + [regex]::Escape($entry.Key) + '",\s*' + $entry.Value.old + ',\s*' + $entry.Value.new + '\)'
	Assert-Contract -Condition ($migrationBody -match $assignment) -Message "the version 8 migration must change $($entry.Key) from $($entry.Value.old) to $($entry.Value.new)"
}

$loadConfig = $gameControl.IndexOf('G_LoadConfig();')
$videoInit = $gameControl.IndexOf('V_Init2();', $loadConfig)
$readConfig = $gameControl.IndexOf('G_ReadConfig(currentGame.GetChars());', $videoInit)
Assert-Contract -Condition ($loadConfig -ge 0 -and $videoInit -gt $loadConfig -and $readConfig -gt $videoInit) -Message "the archived-setting migration must run before normal command-line settings"
Assert-Contract -Condition ($config.IndexOf('exec = C_ParseCmdLineParams(exec);') -ge 0) -Message "normal command-line +set processing is missing"

Write-Host "NRI trace profile default and migration contract tests passed"
