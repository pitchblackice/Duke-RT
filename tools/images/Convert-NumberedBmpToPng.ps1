<#
.SYNOPSIS
Converts recursively discovered `*_n.bmp` images into zero-padded PNG files.

.DESCRIPTION
Searches the input folder and all subfolders for bitmap files whose base filename
matches `NUMBER_n`. Each match is converted to PNG and written to the destination
folder as `#NNNNN.png`.

.EXAMPLE
.\Convert-NumberedBmpToPng.ps1 C:\input C:\output
#>

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$SearchFolder,

    [Parameter(Mandatory = $true, Position = 1)]
    [string]$DestinationFolder
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-AbsolutePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    return [System.IO.Path]::GetFullPath($Path)
}

function Get-OutputName {
    param(
        [Parameter(Mandatory = $true)]
        [string]$NumericStem
    )

    if ($NumericStem -notmatch '^\d+$') {
        return $null
    }

    $numericValue = [int64]$NumericStem
    if ($numericValue -gt 99999) {
        throw "Source id '$NumericStem' is too large to fit into '#NNNNN.png'."
    }

    return ('#{0:D5}.png' -f $numericValue)
}

if (-not (Test-Path -LiteralPath $SearchFolder -PathType Container)) {
    throw "Search folder does not exist or is not a directory: $SearchFolder"
}

$resolvedSearchFolder = (Resolve-Path -LiteralPath $SearchFolder).Path
$resolvedDestinationFolder = Get-AbsolutePath -Path $DestinationFolder

$candidates = @()
foreach ($file in Get-ChildItem -LiteralPath $resolvedSearchFolder -Recurse -File -Filter *.bmp) {
    if ($file.BaseName -match '^(?<Id>\d+)_n$') {
        $outputName = Get-OutputName -NumericStem $matches['Id']
        $candidates += [pscustomobject]@{
            SourcePath = $file.FullName
            OutputName = $outputName
        }
    }
}

if ($candidates.Count -eq 0) {
    Write-Warning "No matching files found under '$resolvedSearchFolder'. Expected filenames like '123_n.bmp'."
    exit 0
}

$duplicateTargets = $candidates |
    Group-Object -Property OutputName |
    Where-Object { $_.Count -gt 1 }

if ($duplicateTargets) {
    $details = foreach ($group in $duplicateTargets) {
        $sources = $group.Group.SourcePath -join ', '
        "'$($group.Name)' <= $sources"
    }

    throw "Multiple source files would map to the same destination name: $($details -join '; ')"
}

New-Item -ItemType Directory -Path $resolvedDestinationFolder -Force | Out-Null

Add-Type -AssemblyName System.Drawing

$convertedCount = 0

foreach ($candidate in $candidates) {
    $targetPath = Join-Path -Path $resolvedDestinationFolder -ChildPath $candidate.OutputName

    if (Test-Path -LiteralPath $targetPath) {
        throw "Destination file already exists: $targetPath"
    }

    $image = $null
    try {
        $image = [System.Drawing.Image]::FromFile($candidate.SourcePath)
        $image.Save($targetPath, [System.Drawing.Imaging.ImageFormat]::Png)
        $convertedCount++
        Write-Host "Converted '$($candidate.SourcePath)' -> '$targetPath'"
    }
    finally {
        if ($null -ne $image) {
            $image.Dispose()
        }
    }
}

Write-Host "Converted $convertedCount file(s) to '$resolvedDestinationFolder'."
