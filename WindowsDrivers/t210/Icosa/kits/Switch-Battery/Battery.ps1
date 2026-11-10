<#
.SYNOPSIS
  Battery driver on the Switch's Windows (Administrator). Needs UEFI R371 (DSDT BAT0)
  and the I2C driver from stage 3.

  -Mode Install : pnputil /add-driver switchbattery.inf /install, then report
  -Mode Status  : report BAT0 (ACPI\SWOA0301), Win32_Battery and the driver trace
  -Mode Remove  : remove the driver package

  Log: C:\SwitchWOA-Battery.txt
#>
param([Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'Remove')][string]$Mode)

$ErrorActionPreference = 'Continue'
# Windows PowerShell on Windows 10 ARM64 is x86: reach System32 via Sysnative.
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-Battery.txt'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) prompt.'; exit 1 }
Log ''
Log "=== Battery $Mode  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"

function Show-Status {
  $dev = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
           Where-Object { $_.PNPDeviceID -like 'ACPI\SWOA0301*' -or $_.PNPDeviceID -like 'ACPI\SWOA0201*' -or $_.PNPDeviceID -like '*COMPOSITEBATTERY*' })
  if (-not ($dev | Where-Object { $_.PNPDeviceID -like 'ACPI\SWOA0301*' })) { Log 'BAT0: NOT FOUND (ACPI\SWOA0301). Is the Switch booted with UEFI R371?' }
  foreach ($d in $dev) {
    Log ("device: {0}  name='{1}' status={2} problem={3} service={4}" -f $d.PNPDeviceID, $d.Name, $d.Status, $d.ConfigManagerErrorCode, $d.Service)
  }
  foreach ($b in @(Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue)) {
    Log ("Win32_Battery: name='{0}' charge={1}% status={2} (1=discharging 2=AC) chemistry={3} voltage={4}" -f $b.Name, $b.EstimatedChargeRemaining, $b.BatteryStatus, $b.Chemistry, $b.DesignVoltage)
  }
  $ps = & (Join-Path $Sys 'powercfg.exe') /batteryreport /output C:\SwitchWOA-battery-report.html 2>&1
  Log ('powercfg /batteryreport: ' + (($ps | Out-String) -replace '\s+', ' '))
  $p = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\switchbattery\Parameters' -ErrorAction SilentlyContinue
  if ($p) {
    $vals = $p.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object { '{0}=0x{1:X}' -f $_.Name, [uint32]$_.Value }
    Log ('driver trace: ' + ($vals -join ' '))
  } else { Log 'driver trace: (no Parameters key)' }
  $q = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\tegra210i2c\Parameters' -ErrorAction SilentlyContinue
  if ($q) {
    $vals = $q.PSObject.Properties | Where-Object { $_.Name -like 'I2C_C000_*' -or $_.Name -eq 'I2C_0000_DriverVersion' } |
            ForEach-Object { '{0}=0x{1:X}' -f ($_.Name -replace '^I2C_C000_', ''), [uint32]$_.Value }
    Log ('I2C1 trace: ' + ($vals -join ' '))
  }
  Log 'PowerState bits: 1 = AC online, 2 = discharging, 4 = charging. Capacity = %. Voltage = mV.'
}

switch ($Mode) {
  'Install' {
    $inf = Join-Path $Here 'switchbattery.inf'
    Log "pnputil /add-driver $inf /install"
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    Log "  exit=$LASTEXITCODE"
    Start-Sleep -Seconds 5
    Show-Status
  }
  'Status' { Show-Status }
  'Remove' {
    $oem = & $Pnputil /enum-drivers 2>&1 | Out-String
    foreach ($x in [regex]::Matches($oem, '(?ms)Published Name\s*:\s*(oem\d+\.inf)\s*\r?\n\s*Original Name\s*:\s*switchbattery\.inf')) {
      $name = $x.Groups[1].Value
      Log "pnputil /delete-driver $name /uninstall /force"
      (& $Pnputil /delete-driver $name /uninstall /force 2>&1) | ForEach-Object { Log "  $_" }
    }
    Show-Status
  }
}
Log "Log: $Log"
