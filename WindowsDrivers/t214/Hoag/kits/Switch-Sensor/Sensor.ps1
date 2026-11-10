<#
.SYNOPSIS
  Light sensor and accelerometer driver on the Switch's Windows (Administrator). Needs UEFI R379
  (DSDT I2C2 and SENS) and the I2C driver from stage 3.

  -Mode Install : pnputil /add-driver switchsensor.inf /install, then report
  -Mode Status  : report SENS (ACPI\SWOA0901) and the driver trace (light, acceleration)
  -Mode Watch   : print light and acceleration (the driver saves them every 10 s; Ctrl+C to stop)
  -Mode Remove  : remove the driver package

  Log: C:\SwitchWOA-Sensor.txt
#>
param([Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'Watch', 'Remove')][string]$Mode)

$ErrorActionPreference = 'Continue'
# Windows PowerShell on Windows 10 ARM64 is x86: reach System32 via Sysnative.
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-Sensor.txt'
$Key = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchsensor\Parameters'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) prompt.'; exit 1 }

function Show-Status {
  $dev = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'ACPI\SWOA0901*' })
  if ($dev.Count -eq 0) { Log 'SENS: NOT FOUND (ACPI\SWOA0901). Is the Switch booted with UEFI R379?' }
  foreach ($d in $dev) {
    Log ("device: {0}  name='{1}' status={2} problem={3} service={4}" -f $d.PNPDeviceID, $d.Name, $d.Status, $d.ConfigManagerErrorCode, $d.Service)
  }
  $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
  if ($p) {
    $vals = $p.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object { '{0}=0x{1:X}' -f $_.Name, $_.Value }
    Log ('driver trace: ' + ($vals -join ' '))
    Log ("light sensor ok={0} part=0x{1:X}   IMU ok={2} WHO_AM_I=0x{3:X}" -f $p.AlsOk, $p.AlsPartId, $p.ImuOk, $p.ImuWhoAmI)
    if ($null -ne $p.Samples) { Log ("last: {0:0.0} lux (gain x{1})  accel X {2} Y {3} Z {4} mg" -f ($p.LuxMilli / 1000.0), $p.AlsGain, [int32]$p.AccelX, [int32]$p.AccelY, [int32]$p.AccelZ) }
  } else { Log 'driver trace: (no Parameters key)' }
}

switch ($Mode) {
  'Install' {
    Log ''
    Log "=== Sensor Install  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    $inf = Join-Path $Here 'switchsensor.inf'
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    Log "  exit=$LASTEXITCODE"
    Start-Sleep -Seconds 5
    Show-Status
  }
  'Status' { Log ''; Log "=== Sensor Status  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"; Show-Status }
  'Watch' {
    Write-Host 'time         lux  gain      X      Y      Z   samples   (Ctrl+C to stop)'
    while ($true) {
      $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
      if ($p) { Write-Host ("{0}  {1,7:0.0}  {2,4}  {3,5}  {4,5}  {5,5}   {6}" -f (Get-Date -Format 'HH:mm:ss'), ($p.LuxMilli / 1000.0), $p.AlsGain, [int32]$p.AccelX, [int32]$p.AccelY, [int32]$p.AccelZ, $p.Samples) }
      Start-Sleep -Seconds 2
    }
  }
  'Remove' {
    $oem = & $Pnputil /enum-drivers 2>&1 | Out-String
    foreach ($x in [regex]::Matches($oem, '(?ms)Published Name\s*:\s*(oem\d+\.inf)\s*\r?\n\s*Original Name\s*:\s*switchsensor\.inf')) {
      $name = $x.Groups[1].Value
      Log "pnputil /delete-driver $name /uninstall /force"
      (& $Pnputil /delete-driver $name /uninstall /force 2>&1) | ForEach-Object { Log "  $_" }
    }
    Show-Status
  }
}
