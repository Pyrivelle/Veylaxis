param([string]$VcpkgRoot, [string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/load-env.ps1"
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $VcpkgRoot) {
    $cache = Join-Path $projectRoot 'build\CMakeCache.txt'
    if (Test-Path $cache) {
        $line = Get-Content $cache | Where-Object { $_ -match '^VCPKG_INSTALLED_DIR:PATH=' } | Select-Object -First 1
        if ($line) { $VcpkgRoot = Split-Path ($line -replace '^VCPKG_INSTALLED_DIR:PATH=', '') -Parent }
    }
}
if (-not $VcpkgRoot) { $VcpkgRoot = $env:VCPKG_ROOT }
if (-not $VcpkgRoot) { $VcpkgRoot = 'C:\CProj\vcpkg' }
$prefix = Join-Path $VcpkgRoot 'installed\x64-windows'
$binarySubdir = if ($Configuration -eq 'Debug') { 'debug\bin' } else { 'bin' }
$pluginSubdir = if ($Configuration -eq 'Debug') { 'debug\plugins' } else { 'plugins' }
$env:PATH = "$prefix\$binarySubdir;$env:PATH"
$env:GDAL_DATA = "$prefix\share\gdal"
$env:S57_CSV = $env:GDAL_DATA
$env:PROJ_DATA = "$prefix\share\proj"
$env:GDAL_DRIVER_PATH = 'disable'
$env:OSG_LIBRARY_PATH = "$prefix\$pluginSubdir\osgPlugins-3.6.5"
