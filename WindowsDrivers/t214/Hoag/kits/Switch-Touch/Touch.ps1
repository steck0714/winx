<#
.SYNOPSIS
  Touch screen driver on the Switch's Windows (Administrator). Needs UEFI R373
  (touch power-on, DSDT I2C3 + TCH0), the GPIO driver (stage 1) and the I2C
  driver v3 (stage 3).

  -Mode Install   : pnputil /add-driver switchtouch.inf /install, then report
  -Mode Status    : report TCH0 (ACPI\SWOA0501), vhf, the HID touch screen and the driver trace
  -Mode Watch     : print the driver counters and last touch every second (Ctrl+C to stop)
  -Mode Calibrate : touch the TOP-LEFT corner of the screen; sets CfgInvertX/Y and restarts the device
  -Mode Remove    : remove the driver package

  Log: C:\SwitchWOA-Touch.txt
#>
param([Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'Watch', 'Calibrate', 'Remove')][string]$Mode)

$ErrorActionPreference = 'Continue'
# Windows PowerShell on Windows 10 ARM64 is x86: reach System32 via Sysnative.
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-Touch.txt'
$Key = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchtouch\Parameters'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) prompt.'; exit 1 }

function Get-TouchDevice { @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'ACPI\SWOA0501*' }) }

function Show-Status {
  $dev = Get-TouchDevice
  if ($dev.Count -eq 0) { Log 'TCH0: NOT FOUND (ACPI\SWOA0501). Is the Switch booted with UEFI R373?' }
  foreach ($d in $dev) {
    Log ("device: {0}  name='{1}' status={2} problem={3} service={4}" -f $d.PNPDeviceID, $d.Name, $d.Status, $d.ConfigManagerErrorCode, $d.Service)
  }
  $vhf = Get-CimInstance Win32_SystemDriver -Filter "Name='vhf'" -ErrorAction SilentlyContinue
  Log ("vhf service: {0}  vhf.sys present: {1}" -f $(if ($vhf) { "$($vhf.State) start=$($vhf.StartMode)" } else { 'NOT REGISTERED' }),
       (Test-Path (Join-Path $Sys 'drivers\vhf.sys')))
  $hid = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'HID\VID_057E&PID_5501*' })
  if ($hid.Count -eq 0) { Log 'HID child: none (HID\VID_057E&PID_5501)' }
  foreach ($h in $hid) { Log ("HID child: {0}  name='{1}' status={2} problem={3}" -f $h.PNPDeviceID, $h.Name, $h.Status, $h.ConfigManagerErrorCode) }
  $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
  if ($p) {
    $vals = $p.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object { '{0}=0x{1:X}' -f $_.Name, [uint32]$_.Value }
    Log ('driver trace: ' + ($vals -join ' '))
  } else { Log 'driver trace: (no Parameters key)' }
}

switch ($Mode) {
  'Install' {
    Log ''
    Log "=== Touch Install  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    $inf = Join-Path $Here 'switchtouch.inf'
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    Log "  exit=$LASTEXITCODE"
    Start-Sleep -Seconds 5
    Show-Status
  }
  'Status' { Log ''; Log "=== Touch Status  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"; Show-Status }
  'Watch' {
    Write-Host 'time      irqs  polls  events  reports  errors  last(landscape x,y)   (Ctrl+C to stop)'
    while ($true) {
      $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
      if ($p) {
        Write-Host ("{0}  {1,4}  {2,5}  {3,6}  {4,7}  {5,6}  {6},{7}  held={8}" -f (Get-Date -Format 'HH:mm:ss'), $p.Irqs, $p.Polls, $p.Events,
                    $p.Reports, $p.Errors, $p.LastLandscapeX, $p.LastLandscapeY, $p.Held)
      }
      Start-Sleep -Seconds 1
    }
  }
  'Calibrate' {
    Log ''
    Log "=== Touch Calibrate  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    $before = Get-ItemProperty $Key -ErrorAction SilentlyContinue
    Set-ItemProperty $Key -Name LastLandscapeX -Value 9999 -Type DWord
    Set-ItemProperty $Key -Name LastLandscapeY -Value 9999 -Type DWord
    Write-Host 'Touch and hold the TOP-LEFT corner of the screen (as you see it now) within 20 seconds...'
    $p = $null
    for ($i = 0; $i -lt 40; $i++) {
      Start-Sleep -Milliseconds 500
      $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
      if ($p -and $p.LastLandscapeX -ne 9999) { break }
    }
    if (-not $p -or $p.LastLandscapeX -eq 9999) { Log 'No touch seen. Is the driver running? (-Mode Status)'; break }
    $lx = [int]$p.LastLandscapeX; $ly = [int]$p.LastLandscapeY
    $ix = [int]($ly -gt 360); $iy = [int]($lx -gt 640)
    Log ("touch at landscape {0},{1} -> CfgInvertX={2} CfgInvertY={3} (was {4}/{5})" -f $lx, $ly, $ix, $iy, $before.CfgInvertX, $before.CfgInvertY)
    Set-ItemProperty $Key -Name CfgInvertX -Value $ix -Type DWord
    Set-ItemProperty $Key -Name CfgInvertY -Value $iy -Type DWord
    foreach ($d in Get-TouchDevice) {
      Log "pnputil /restart-device $($d.PNPDeviceID)"
      (& $Pnputil /restart-device $d.PNPDeviceID 2>&1) | ForEach-Object { Log "  $_" }
    }
    Start-Sleep -Seconds 3
    Show-Status
  }
  'Remove' {
    $oem = & $Pnputil /enum-drivers 2>&1 | Out-String
    foreach ($x in [regex]::Matches($oem, '(?ms)Published Name\s*:\s*(oem\d+\.inf)\s*\r?\n\s*Original Name\s*:\s*switchtouch\.inf')) {
      $name = $x.Groups[1].Value
      Log "pnputil /delete-driver $name /uninstall /force"
      (& $Pnputil /delete-driver $name /uninstall /force 2>&1) | ForEach-Object { Log "  $_" }
    }
    Show-Status
  }
}
