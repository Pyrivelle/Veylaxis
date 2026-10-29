param([string]$VcpkgRoot, [string]$Configuration = 'Release', [switch]$Marine)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\runtime.ps1" -VcpkgRoot $VcpkgRoot -Configuration $Configuration
$mode = if ($Marine) { 'on' } else { 'off' }
& "$projectRoot\build\$Configuration\Veylaxis.exe" view "$projectRoot\output\tile-test\halo250_normals\tiles.osgb" --marine $mode
if ($LASTEXITCODE) { throw "Tile viewer failed (exit $LASTEXITCODE)" }
