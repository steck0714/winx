<#
.SYNOPSIS
  Power button driver on the Switch's Windows (Administrator). Needs UEFI R374
  (DSDT PWRB) and the I2C driver v3 (stage 3).

  -Mode Install   : pnputil /add-driver switchpwrbtn.inf /install, then report
  -Mode Status    : report PWRB (ACPI\SWOA0601), the HID child, the power button action and the driver trace
  -Mode Watch     : print presses and reports every second (Ctrl+C to stop)
  -Mode SetAction : -Action Display | Shutdown | Sleep | Nothing  (AC and battery, current power plan)
  -Mode Remove    : remove the driver package

  Log: C:\SwitchWOA-PowerButton.txt
#>
param(
  [Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'Watch', 'SetAction', 'Remove')][string]$Mode,
  [ValidateSet('Display', 'Shutdown', 'Sleep', 'Nothing')][string]$Action = 'Display'
)

$ErrorActionPreference = 'Continue'
# Windows PowerShell on Windows 10 ARM64 is x86: reach System32 via Sysnative.
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Powercfg = Join-Path $Sys 'powercfg.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-PowerButton.txt'
$Key = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchpwrbtn\Parameters'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) prompt.'; exit 1 }

function Show-Action {
  $q = (& $Powercfg /query SCHEME_CURRENT SUB_BUTTONS PBUTTONACTION 2>&1) | Out-String
  $ac = [regex]::Match($q, 'Current AC Power Setting Index:\s*0x([0-9a-fA-F]+)').Groups[1].Value
  $dc = [regex]::Match($q, 'Current DC Power Setting Index:\s*0x([0-9a-fA-F]+)').Groups[1].Value
  $names = @{ 0 = 'Nothing'; 1 = 'Sleep'; 2 = 'Hibernate'; 3 = 'Shutdown'; 4 = 'Display off' }
  if ($ac -ne '') { Log ("power button action: AC={0} battery={1}" -f $names[[Convert]::ToInt32($ac, 16)], $names[[Convert]::ToInt32($dc, 16)]) }
  else { Log 'power button action: (powercfg query failed)'; Log $q }
}

function Show-Status {
  $dev = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'ACPI\SWOA0601*' })
  if ($dev.Count -eq 0) { Log 'PWRB: NOT FOUND (ACPI\SWOA0601). Is the Switch booted with UEFI R374?' }
  foreach ($d in $dev) {
    Log ("device: {0}  name='{1}' status={2} problem={3} service={4}" -f $d.PNPDeviceID, $d.Name, $d.Status, $d.ConfigManagerErrorCode, $d.Service)
  }
  $hid = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'HID\*' -and $_.PNPDeviceID -like '*057E*5601*' })
  foreach ($h in $hid) { Log ("HID child: {0}  name='{1}' status={2} problem={3}" -f $h.PNPDeviceID, $h.Name, $h.Status, $h.ConfigManagerErrorCode) }
  Show-Action
  $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
  if ($p) {
    $vals = $p.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object { '{0}=0x{1:X}' -f $_.Name, [uint32]$_.Value }
    Log ('driver trace: ' + ($vals -join ' '))
  } else { Log 'driver trace: (no Parameters key)' }
}

switch ($Mode) {
  'Install' {
    Log ''
    Log "=== PowerButton Install  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    $inf = Join-Path $Here 'switchpwrbtn.inf'
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    Log "  exit=$LASTEXITCODE"
    Start-Sleep -Seconds 5
    Show-Status
  }
  'Status' { Log ''; Log "=== PowerButton Status  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"; Show-Status }
  'Watch' {
    Write-Host 'time      presses  reports  failures   (Ctrl+C to stop)'
    while ($true) {
      $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
      if ($p) { Write-Host ("{0}  {1,7}  {2,7}  {3,8}" -f (Get-Date -Format 'HH:mm:ss'), $p.Presses, $p.Reports, $p.Failures) }
      Start-Sleep -Seconds 1
    }
  }
  'SetAction' {
    $index = @{ Nothing = 0; Sleep = 1; Shutdown = 3; Display = 4 }[$Action]
    Log ''
    Log "=== PowerButton SetAction $Action ($index)  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    & $Powercfg /setacvalueindex SCHEME_CURRENT SUB_BUTTONS PBUTTONACTION $index
    & $Powercfg /setdcvalueindex SCHEME_CURRENT SUB_BUTTONS PBUTTONACTION $index
    & $Powercfg /setactive SCHEME_CURRENT
    Show-Action
  }
  'Remove' {
    $oem = & $Pnputil /enum-drivers 2>&1 | Out-String
    foreach ($x in [regex]::Matches($oem, '(?ms)Published Name\s*:\s*(oem\d+\.inf)\s*\r?\n\s*Original Name\s*:\s*switchpwrbtn\.inf')) {
      $name = $x.Groups[1].Value
      Log "pnputil /delete-driver $name /uninstall /force"
      (& $Pnputil /delete-driver $name /uninstall /force 2>&1) | ForEach-Object { Log "  $_" }
    }
    Show-Status
  }
}
