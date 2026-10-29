param([string]$VcpkgRoot, [string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\runtime.ps1" -VcpkgRoot $VcpkgRoot -Configuration $Configuration
& "$projectRoot\build\$Configuration\Veylaxis.exe" view "$projectRoot\output\region001\region001_elevation.tif" --smoke-test "$projectRoot\output\region001\region001_marine.png"
if ($LASTEXITCODE) { throw "Marine rendering test failed (exit $LASTEXITCODE)" }
