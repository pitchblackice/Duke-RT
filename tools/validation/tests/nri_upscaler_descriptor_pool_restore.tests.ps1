Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$dispatchPath = Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_pass_dispatch.cpp'
$dispatch = Get-Content -LiteralPath $dispatchPath -Raw

function Assert-InOrder {
	param([string]$Text, [string[]]$Tokens, [string]$Message)

	$offset = 0
	foreach ($token in $Tokens) {
		$index = $Text.IndexOf($token, $offset, [StringComparison]::Ordinal)
		if ($index -lt 0) {
			throw "$Message (missing or out of order: $token)"
		}
		$offset = $index + $token.Length
	}
}

$start = $dispatch.IndexOf('bool NRIPassDispatcher::DispatchUpscaleChain', [StringComparison]::Ordinal)
$end = $dispatch.IndexOf('bool NRIPassDispatcher::DispatchFinal', $start, [StringComparison]::Ordinal)
if ($start -lt 0 -or $end -lt 0) {
	throw 'could not isolate DispatchUpscaleChain'
}
$upscaleChain = $dispatch.Substring($start, $end - $start)

Assert-InOrder -Text $upscaleChain -Message 'main upscaler dispatch must restore the application descriptor pool before handling failure' -Tokens @(
	'const bool mainUpscalerDispatched = context.mUpscalerService.DispatchMainUpscaler(mainKind, upscalerDesc);',
	'context.mCommands.RestoreDescriptorPool();',
	'if (!mainUpscalerDispatched)'
)

Assert-InOrder -Text $upscaleChain -Message 'post-sharpen dispatch must restore the application descriptor pool before handling failure' -Tokens @(
	'const bool postSharpenDispatched = context.mUpscalerService.DispatchPostSharpen(postSharpenKind, postDesc);',
	'context.mCommands.RestoreDescriptorPool();',
	'if (!postSharpenDispatched)'
)

if ([regex]::Matches($upscaleChain, 'context\.mCommands\.RestoreDescriptorPool\(\);').Count -ne 2) {
	throw 'DispatchUpscaleChain must restore the application descriptor pool exactly once after each external upscaler dispatch'
}

Write-Host 'NRI upscaler descriptor-pool restore tests passed.'
