param([string]$QgisRoot = 'C:\Program Files\QGIS 3.34.15', [switch]$AnalyseOnly)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
foreach ($case in @(@('baseline',0), @('halo250',250), @('halo250_normals',250))) {
    $analysis = if ($AnalyseOnly) { ' --analyse-only' } else { '' }
    $command = 'call "{0}\bin\o4w_env.bat" >nul && python "{1}\scripts\tile-experiment.py" --phase {2} --halo {3} --models{4}' -f $QgisRoot,$projectRoot,$case[0],$case[1],$analysis
    & cmd.exe /d /c $command
    if ($LASTEXITCODE) { throw "Tile experiment failed: $($case[0])" }
}
