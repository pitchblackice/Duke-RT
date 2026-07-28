param(
    [Parameter(Mandatory = $true)][string]$InputPath,
    [Parameter(Mandatory = $true)][string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$prefix = 'PERF pt smoke analytic light NRI:'
$rows = foreach ($line in Get-Content -LiteralPath $InputPath) {
    $prefixIndex = $line.IndexOf($prefix, [StringComparison]::Ordinal)
    if ($prefixIndex -lt 0) { continue }
    $row = [ordered]@{}
    $payload = $line.Substring($prefixIndex + $prefix.Length)
    foreach ($match in [regex]::Matches($payload, '(?<key>[a-z0-9_]+)=(?<value>\S+)')) {
        $key = $match.Groups['key'].Value
        $value = $match.Groups['value'].Value
        [uint64]$number = 0
        $row[$key] = if ([uint64]::TryParse($value, [ref]$number)) { $number } else { $value }
    }
    [pscustomobject]$row
}

$parent = Split-Path -Parent $OutputPath
if ($parent -and -not (Test-Path -LiteralPath $parent)) {
    New-Item -ItemType Directory -Path $parent | Out-Null
}
@($rows) | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $OutputPath
