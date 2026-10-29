param([string]$VcpkgRoot, [string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
. "$PSScriptRoot\runtime.ps1" -VcpkgRoot $VcpkgRoot -Configuration $Configuration
if ($env:DMU_PLUGIN_PATH -and (Test-Path -LiteralPath $env:DMU_PLUGIN_PATH)) {
    New-Item -ItemType Directory -Force "$projectRoot/plugins/osgPlugins-3.6.5" | Out-Null
    Copy-Item -LiteralPath $env:DMU_PLUGIN_PATH -Destination "$projectRoot/plugins/osgPlugins-3.6.5/osgdb_dmu.dll" -Force
}
$toolchain = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
if (-not (Test-Path $toolchain)) { throw "vcpkg toolchain missing: $toolchain" }
$cmake = 'C:\Program Files\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake).Source }
$localCmakeArgs = @()
foreach ($name in @('VEYLAXIS_SDK_ROOT','VEYLAXIS_QT_ROOT')) { $value = [Environment]::GetEnvironmentVariable($name); if ($value) { $localCmakeArgs += "-D$name=$value" } }
& $cmake @localCmakeArgs -S $projectRoot -B "$projectRoot\build" -G 'Visual Studio 18 2026' -A x64 "-DCMAKE_TOOLCHAIN_FILE=$toolchain" '-DVCPKG_TARGET_TRIPLET=x64-windows'
if ($LASTEXITCODE) { throw 'CMake configuration failed' }
& $cmake --build "$projectRoot\build" --config $Configuration --parallel
if ($LASTEXITCODE) { throw 'Build failed' }
