param([string]$VcpkgRoot, [string]$Configuration = 'Release', [double]$ZScale = 1, [switch]$ExportOnly, [switch]$SeabedOnly)
$ErrorActionPreference = 'Stop'
$explicitZScale = $PSBoundParameters.ContainsKey('ZScale')
. "$PSScriptRoot\runtime.ps1" -VcpkgRoot $VcpkgRoot -Configuration $Configuration
# Default exports preserve metre geometry; only interactive viewing exaggerates Z.
if ($ExportOnly -and -not $explicitZScale) { $ZScale = 1 }
$viewerArguments = @('view', "$projectRoot\output\region001\region001_elevation.tif", '--z-scale', $ZScale)
if ($SeabedOnly) { $viewerArguments += @('--marine','off') }
if ($ExportOnly) { $viewerArguments += @('--export', "$projectRoot\output\region001\region001_terrain.osgt") }
& "$projectRoot\build\$Configuration\Veylaxis.exe" @viewerArguments
if ($LASTEXITCODE) { throw "Viewer failed (exit $LASTEXITCODE)" }
