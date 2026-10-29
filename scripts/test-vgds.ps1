param([string]$JsonPackage = 'C:\CProj\vcpkg\installed\x64-windows\share\nlohmann_json', [string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$cmake = 'C:\Program Files\CMake\bin\cmake.exe'
$ctest = 'C:\Program Files\CMake\bin\ctest.exe'
$buildDir = Join-Path $projectRoot 'build\vgds-core'
& $cmake -S $projectRoot -B $buildDir -G 'Visual Studio 18 2026' -A x64 '-DVEYLAXIS_VGDS_ONLY=ON' "-Dnlohmann_json_DIR=$JsonPackage"
if ($LASTEXITCODE) { throw 'VGDS-only configure failed' }
& $cmake --build $buildDir --config $Configuration --parallel
if ($LASTEXITCODE) { throw 'VGDS-only build failed' }
& $ctest --test-dir $buildDir -C $Configuration --output-on-failure
if ($LASTEXITCODE) { throw 'VGDS-only tests failed' }
