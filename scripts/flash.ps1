param([string]$SerialNumber = '', [string]$JLink = 'E:/JLink/JLink_V992/JLink.exe')
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$hex = Join-Path $project 'build/zephyr/zephyr.hex'
$bin = Join-Path $project 'build/zephyr/zephyr.bin'
if (!(Test-Path -LiteralPath $hex)) { throw 'Build first: scripts/build.ps1' }
if (!(Test-Path -LiteralPath $JLink)) { throw 'Set -JLink to the installed JLink.exe path.' }
$commands = Join-Path $project 'build/flash.jlink'
# Device selection and J-Link auto-connect validate the core before loading.
# Do not use erase-all, recover, or APPROTECT unlocking automatically.
@('r', "loadfile `"$hex`"", "verifybin `"$bin`", 0x0", 'r', 'g', 'exit') | Set-Content -LiteralPath $commands -Encoding ascii
$flashArgs = @('-NoGui','1','-ExitOnError','1','-Device','nRF54L15_M33','-If','SWD','-Speed','1000','-AutoConnect','1','-CommandFile',$commands)
if ($SerialNumber) { $flashArgs += @('-SelectEmuBySN',$SerialNumber) }
$log = Join-Path $project 'build/flash.log'
& $JLink @flashArgs 2>&1 | Tee-Object -FilePath $log
$rc = $LASTEXITCODE
$result = Get-Content -LiteralPath $log -Raw
if ($rc -ne 0 -or $result -match 'Could not connect|Error occurred|Cannot connect|Verification failed' -or $result -notmatch 'Verify successful') {
 throw 'Flash/verify not confirmed. See build/flash.log and check SWD wiring.'
}
