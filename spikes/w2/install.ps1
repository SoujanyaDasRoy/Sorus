#Requires -RunAsAdministrator
# Spike W2 (throwaway): installs the unsigned -6 dB test APO on one render endpoint and measures the level change.
# What it changes (all undone by uninstall.ps1):
#   1. Copies sorus_w2_apo.dll to C:\Program Files\Sorus\spike-w2\
#   2. Registers the COM class + APO under HKLM\SOFTWARE\Classes (CLSID and AudioEngine\AudioProcessingObjects)
#   3. Appends the APO to the endpoint's composite mode-effect list (FxProperties {d04e05a6-...},14),
#      saving the original list in HKLM\SOFTWARE\Sorus\SpikeW2 first
#   4. Restarts the Windows Audio service (sound cuts out for a few seconds)
param([string]$Endpoint = '{a0ecdea8-ea58-4be5-890b-cb242cb309ff}')  # Realtek Speaker on this PC
$ErrorActionPreference = 'Stop'

$clsid = '{469205DD-A208-4C1A-A434-D3514B371236}'
$build = "$env:USERPROFILE\build\sorus\w2"
$dir = "$env:ProgramFiles\Sorus\spike-w2"
$fx = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$Endpoint\FxProperties"
$prop = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14'
$backup = 'HKLM:\SOFTWARE\Sorus\SpikeW2'
$meterId = "{0.0.0.00000000}.$Endpoint"

function Measure-Level {
  # First run warms up the endpoint's effects; average the next three.
  & "$build\meter.exe" $meterId | Out-Null
  $v = 1..3 | ForEach-Object {
    $r = & "$build\meter.exe" $meterId
    if ($LASTEXITCODE -ne 0) { throw "meter failed: no audio on this endpoint. Run uninstall.ps1 to restore sound." }
    [double]$r
  }
  [math]::Round(($v | Measure-Object -Average).Average, 2)
}

function Get-FxList { (Get-ItemProperty $fx -Name $prop -ErrorAction SilentlyContinue).$prop }
# Administrators only have SetValue on FxProperties; Set-ItemProperty asks for more and is denied.
function Set-FxList([string[]]$list) {
  $rel = $fx -replace '^HKLM:\\', ''
  $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($rel,
    [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
    [System.Security.AccessControl.RegistryRights]'SetValue, QueryValues')
  try { $key.SetValue($prop, $list, [Microsoft.Win32.RegistryValueKind]::MultiString) } finally { $key.Close() }
}

if ((Get-FxList) -contains $clsid) { throw 'Already active: run uninstall.ps1 first.' }
Write-Host 'Pause any other audio. Measuring baseline...'
$before = Measure-Level
Write-Host "Baseline: $before dBFS"

New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item "$build\sorus_w2_apo.dll" $dir -Force

$com = "HKLM:\SOFTWARE\Classes\CLSID\$clsid"
New-Item -Force $com -Value 'Sorus W2 spike APO' | Out-Null
New-Item -Force "$com\InprocServer32" -Value "$dir\sorus_w2_apo.dll" | Out-Null
New-ItemProperty "$com\InprocServer32" -Name ThreadingModel -Value Both -Force | Out-Null

$apo = "HKLM:\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\$clsid"
New-Item -Force $apo | Out-Null
$values = [ordered]@{
  FriendlyName = 'Sorus W2 spike APO'; Copyright = 'Sorus contributors, GPL-3.0'
  MajorVersion = 1; MinorVersion = 0; Flags = 0xE
  MinInputConnections = 1; MaxInputConnections = 1; MinOutputConnections = 1; MaxOutputConnections = 1
  MaxInstances = [int]-1; NumAPOInterfaces = 1
  APOInterface0 = '{FD7F2B29-24D0-4b5c-B177-592C39F9CA10}'  # IAudioProcessingObject
}
foreach ($k in $values.Keys) {
  $type = if ($values[$k] -is [string]) { 'String' } else { 'DWord' }
  New-ItemProperty $apo -Name $k -Value $values[$k] -PropertyType $type -Force | Out-Null
}

$orig = Get-FxList
if (-not (Test-Path $backup)) {  # keep the first backup if an earlier run stopped part-way
  New-Item -Force $backup | Out-Null
  New-ItemProperty $backup -Name Endpoint -Value $Endpoint -Force | Out-Null
  if ($null -ne $orig) { New-ItemProperty $backup -Name Original -Value ([string[]]$orig) -PropertyType MultiString -Force | Out-Null }
}
Set-FxList (@($orig | Where-Object { $_ }) + $clsid)
Write-Host "Mode-effect list written: $((Get-FxList) -join ', ')"

Write-Host 'Restarting Windows Audio...'
Restart-Service audiosrv -Force
Start-Sleep 3
Write-Host "Mode-effect list after restart: $((Get-FxList) -join ', ')"

$after = Measure-Level
Write-Host "With APO at its default 0 dB: $after dBFS (should match the baseline; w2test.exe changes the gain)"
Write-Host 'audiodg modules matching sorus_w2_apo:'
tasklist /m sorus_w2_apo.dll
