param([switch]$Pristine, [string]$Sdk = 'C:/ncs/v3.4.2', [string]$Toolchain = 'C:/ncs/toolchains/5f5ef0cea0')
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$spec = Get-Content "$Toolchain/environment.json" | ConvertFrom-Json
foreach ($item in $spec.env_vars) {
 if ($item.type -eq 'relative_paths') {
  $value = ($item.values | ForEach-Object { Join-Path $Toolchain $_ }) -join ';'
  if ($item.existing_value_treatment -eq 'prepend_to') { $value += ';' + [Environment]::GetEnvironmentVariable($item.key) }
 } else { $value = $item.value }
 [Environment]::SetEnvironmentVariable($item.key, $value, 'Process')
}
$env:ZEPHYR_BASE = "$Sdk/zephyr"
$buildArgs = @('build', '--no-sysbuild', '-b', 'ecw54/nrf54l15/cpuapp', '-d', "$project/build", $project)
if ($Pristine) { $buildArgs += @('-p', 'always') }
Push-Location $project
try { & "$Toolchain/opt/bin/python.exe" -m west @buildArgs; if ($LASTEXITCODE -ne 0) { throw 'Build failed' } }
finally { Pop-Location }
