param([string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$ctest = 'C:\Program Files\CMake\bin\ctest.exe'
if (-not (Test-Path $ctest)) { $ctest = (Get-Command ctest).Source }
& $ctest --test-dir "$projectRoot\build" -C $Configuration --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed' }
