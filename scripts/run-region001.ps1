param([string]$VcpkgRoot, [string]$Configuration = 'Release', [double]$Resolution = 5)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
. "$PSScriptRoot\runtime.ps1" -VcpkgRoot $VcpkgRoot -Configuration $Configuration
& "$projectRoot\build\$Configuration\Veylaxis.exe" reconstruct --input "$projectRoot\S57_DATA\ENC_ROOT\US2EC04M\US2EC04M.000" --output "$projectRoot\output\region001" --resolution $Resolution
if ($LASTEXITCODE) { throw "Reconstruction failed or has incomplete ROI coverage (exit $LASTEXITCODE)" }
