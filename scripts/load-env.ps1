param([string]$Path = (Join-Path (Split-Path $PSScriptRoot -Parent) '.env'))
if ($env:VEYLAXIS_ENV_FILE) { $Path = $env:VEYLAXIS_ENV_FILE }
if (Test-Path -LiteralPath $Path) {
    foreach ($line in Get-Content -LiteralPath $Path -Encoding utf8) {
        if ($line -match '^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=(.*)$') {
            $key = $Matches[1]; $value = $Matches[2].Trim()
            if ($value.Length -ge 2 -and (($value.StartsWith('"') -and $value.EndsWith('"')) -or ($value.StartsWith("'") -and $value.EndsWith("'")))) { $value = $value.Substring(1, $value.Length - 2) }
            if ($null -eq [Environment]::GetEnvironmentVariable($key, 'Process')) { [Environment]::SetEnvironmentVariable($key, $value, 'Process') }
        }
    }
}
