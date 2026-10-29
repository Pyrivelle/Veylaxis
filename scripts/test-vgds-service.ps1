param([switch]$NoOsgb, [string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/runtime.ps1" -Configuration $Configuration
$projectRoot = Split-Path $PSScriptRoot -Parent
$directory = if ($NoOsgb) { 'vgds-no-osg' } else { 'vgds-service' }
$buildDirectory = Join-Path $projectRoot "build/$directory"
$osgb = if ($NoOsgb) { 'OFF' } else { 'ON' }
$cmake = 'C:/Program Files/CMake/bin/cmake.exe'
$ctest = 'C:/Program Files/CMake/bin/ctest.exe'
& $cmake -S $projectRoot -B $buildDirectory -G 'Visual Studio 18 2026' -A x64 "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake" '-DVCPKG_TARGET_TRIPLET=x64-windows' '-DVEYLAXIS_VGDS_SERVICE_ONLY=ON' "-DVEYLAXIS_VGDS_OSGB=$osgb"
if ($LASTEXITCODE) { throw 'VGDS service configure failed' }
& $cmake --build $buildDirectory --config $Configuration --parallel
if ($LASTEXITCODE) { throw 'VGDS service build failed' }
& $ctest --test-dir $buildDirectory -C $Configuration --output-on-failure
if ($LASTEXITCODE) { throw 'VGDS service tests failed' }
