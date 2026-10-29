param([string]$Config = 'config/vgds-catalog.json', [switch]$Tour, [switch]$LandOnly, [string]$SmokeTest, [double]$Duration = 0, [string]$Report, [string]$NavigationModels = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/runtime.ps1"
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $NavigationModels) { $NavigationModels = if (Test-Path "$projectRoot/config/navigation_models.local.json") { 'config/navigation_models.local.json' } else { 'config/navigation_models.json' } }
$viewerArgs = @('vgds', '--config', $Config, '--navigation-models', $NavigationModels)
if ($Tour) { $viewerArgs += '--tour' }
if ($LandOnly) { $viewerArgs += @('--marine', 'off') }
if ($SmokeTest) { $viewerArgs += @('--smoke-test', $SmokeTest) }
if ($Duration -gt 0) { $viewerArgs += @('--duration', $Duration) }
if ($Report) { $viewerArgs += @('--report', $Report) }
Push-Location $projectRoot
try {
    & "$projectRoot/build/Release/Veylaxis.exe" @viewerArgs
    if ($LASTEXITCODE) { throw "VGDS viewer failed (exit $LASTEXITCODE)" }
} finally { Pop-Location }
