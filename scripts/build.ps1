param(
    [string]$SdkRoot = 'C:\ncs\v3.4.2',
    [string]$ToolchainRoot = 'C:\ncs\toolchains\5f5ef0cea0',
    [switch]$Pristine
)
$ErrorActionPreference = 'Stop'
$appRoot = Split-Path -Parent $PSScriptRoot
$envFile = Join-Path $ToolchainRoot 'environment.json'
$config = Get-Content -LiteralPath $envFile -Raw | ConvertFrom-Json
foreach ($entry in $config.env_vars) {
    if ($entry.type -eq 'string') { $value = $entry.value }
    else { $value = ($entry.values | ForEach-Object { Join-Path $ToolchainRoot $_ }) -join ';' }
    if ($entry.existing_value_treatment -eq 'prepend_to') {
        $value += ';' + [Environment]::GetEnvironmentVariable($entry.key,'Process')
    }
    [Environment]::SetEnvironmentVariable($entry.key,$value,'Process')
}
$env:ZEPHYR_BASE = Join-Path $SdkRoot 'zephyr'
$pythonExe = Join-Path $ToolchainRoot 'opt\bin\python.exe'
$buildArgs = @('-m','west','build','-b','ecw54/nrf54l15/cpuapp','-d',(Join-Path $appRoot 'build'),$appRoot)
if ($Pristine) { $buildArgs += @('-p','always') }
$buildArgs += @('--',"-DBOARD_ROOT=$appRoot")
Push-Location $appRoot
try {
    & $pythonExe @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
} finally { Pop-Location }
