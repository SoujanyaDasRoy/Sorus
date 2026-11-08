#Requires -RunAsAdministrator
# Spike W1: undoes install.ps1 (restores the endpoint's original mode-effect list, removes the APO and its files)
# and measures the level again.
$ErrorActionPreference = 'Stop'

$clsid = '{B3433C7B-8D79-4545-8FD7-D6309ED9A037}'
$build = "$env:USERPROFILE\build\sorus\w1"
$dir = "$env:ProgramFiles\Sorus\spike-w1"
$prop = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14'
$backup = 'HKLM:\SOFTWARE\Sorus\SpikeW1'

if (-not (Test-Path $backup)) { throw 'Nothing to undo: no backup at HKLM\SOFTWARE\Sorus\SpikeW1.' }
$b = Get-ItemProperty $backup
$fx = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$($b.Endpoint)\FxProperties"
# Administrators only have SetValue on FxProperties, so open the key with exactly that.
$key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(($fx -replace '^HKLM:\\', ''),
  [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
  [System.Security.AccessControl.RegistryRights]'SetValue, QueryValues')
try {
  if ($null -ne $b.Original) { $key.SetValue($prop, [string[]]$b.Original, [Microsoft.Win32.RegistryValueKind]::MultiString) }
  else { $key.DeleteValue($prop, $false) }
} finally { $key.Close() }
Write-Host "Restored mode-effect list: $((Get-ItemProperty $fx -Name $prop -ErrorAction SilentlyContinue).$prop -join ', ')"

Write-Host 'Restarting Windows Audio...'
Restart-Service audiosrv -Force
Start-Sleep 3

Remove-Item -Recurse -Force "HKLM:\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\$clsid", "HKLM:\SOFTWARE\Classes\CLSID\$clsid"
Remove-Item -Recurse -Force $dir
Remove-Item -Recurse -Force $backup
if (-not (Get-ChildItem 'HKLM:\SOFTWARE\Sorus' -ErrorAction SilentlyContinue)) { Remove-Item 'HKLM:\SOFTWARE\Sorus' -ErrorAction SilentlyContinue }
if (-not (Get-ChildItem "$env:ProgramFiles\Sorus" -ErrorAction SilentlyContinue)) { Remove-Item "$env:ProgramFiles\Sorus" -ErrorAction SilentlyContinue }

$meterId = "{0.0.0.00000000}.$($b.Endpoint)"
& "$build\meter.exe" $meterId | Out-Null
$v = 1..3 | ForEach-Object { [double](& "$build\meter.exe" $meterId) }
Write-Host ("Level after uninstall: {0:N2} dBFS (should match the baseline)" -f ($v | Measure-Object -Average).Average)
