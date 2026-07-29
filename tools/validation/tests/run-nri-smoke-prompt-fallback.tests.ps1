Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$outputDir = Join-Path $root 'build/smoke-prompt-tests'
$testExe = Join-Path $outputDir 'nri_smoke_prompt_fallback.tests.exe'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$include = Join-Path $root 'source/common/rendering/nri/renderer'
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'
$sources = @(
    (Join-Path $PSScriptRoot 'nri_smoke_prompt_fallback.tests.cpp'),
    (Join-Path $include 'nri_smoke_prompt_fallback.cpp'),
    (Join-Path $include 'nri_smoke_pulses.cpp')
)
$quotedSources = ($sources | ForEach-Object { '"' + $_ + '"' }) -join ' '
$compile = 'call "' + $vsDevCmd + '" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /I"' +
    $include + '" /Fo"' + $outputDir + '/" ' + $quotedSources + ' /Fe:"' + $testExe + '"'
$process = Start-Process -FilePath 'cmd.exe' -ArgumentList '/d', '/c', $compile -PassThru -Wait -NoNewWindow
if ($process.ExitCode -ne 0) { throw "prompt fallback test compilation failed with exit code $($process.ExitCode)" }
& $testExe
if ($LASTEXITCODE -ne 0) { throw "prompt fallback tests failed with exit code $LASTEXITCODE" }
