<#
.SYNOPSIS
  Read-only driver/device inventory of Windows running on the Switch.

.DESCRIPTION
  Run on the Switch (Windows PowerShell 5.1, as Administrator):
    powershell -ExecutionPolicy Bypass -File Collect-SwitchDriverInventory.ps1

  Writes C:\SwitchDriverInventory-<stamp>\ and a .zip of it:
    REPORT.txt            summary: problem devices first, then every device
    devices.csv           all PnP devices (status, problem code, class, IDs, driver)
    pnputil-*.txt         pnputil /enum-devices (all, problem), /enum-drivers
    acpi-*.txt            ACPI device list and the DSDT/FACP/etc. table names
    system.txt            OS, CPU, memory, display, power, battery, thermal zones
  Changes nothing on the system.
#>
param([string]$OutRoot = 'C:\')

$ErrorActionPreference = 'Continue'
# Windows PowerShell on Windows 10 ARM64 is x86, so System32 (and pnputil.exe)
# is only reachable through the Sysnative alias.
$Pnputil = Join-Path $env:windir 'Sysnative\pnputil.exe'
if (-not (Test-Path $Pnputil)) { $Pnputil = Join-Path $env:windir 'System32\pnputil.exe' }
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$Out = Join-Path $OutRoot "SwitchDriverInventory-$Stamp"
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Report = Join-Path $Out 'REPORT.txt'
function Add-Report([string]$Text) { Add-Content -Path $Report -Value $Text -Encoding UTF8 }
function Save-Cmd([string]$Name, [scriptblock]$Block) {
  try { & $Block 2>&1 | Out-File -FilePath (Join-Path $Out $Name) -Encoding UTF8 -Width 400 }
  catch { "FAILED: $($_.Exception.Message)" | Out-File -FilePath (Join-Path $Out $Name) -Encoding UTF8 }
}

Add-Report "Switch Windows driver inventory  $Stamp"
Add-Report ('=' * 78)

# --- every PnP device, with problem code and driver --------------------------
$drivers = @{}
foreach ($d in (Get-CimInstance Win32_PnPSignedDriver -ErrorAction SilentlyContinue)) {
  if ($d.DeviceID) { $drivers[$d.DeviceID] = $d }
}
$rows = foreach ($e in (Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue)) {
  $drv = $drivers[$e.PNPDeviceID]
  [pscustomobject]@{
    Problem     = [int]$e.ConfigManagerErrorCode
    Status      = $e.Status
    Class       = $e.PNPClass
    Name        = $e.Name
    InstanceId  = $e.PNPDeviceID
    HardwareIds = ($e.HardwareID -join ' | ')
    CompatIds   = ($e.CompatibleID -join ' | ')
    Service     = $e.Service
    Inf         = $(if ($drv) { $drv.InfName } else { '' })
    DriverVer   = $(if ($drv) { $drv.DriverVersion } else { '' })
    Provider    = $(if ($drv) { $drv.DriverProviderName } else { '' })
  }
}
$rows | Sort-Object Problem -Descending | Export-Csv -Path (Join-Path $Out 'devices.csv') -NoTypeInformation -Encoding UTF8

$problemText = @{
  1 = 'not configured'; 3 = 'driver corrupt/out of memory'; 10 = 'cannot start'; 12 = 'not enough resources';
  18 = 'reinstall drivers'; 19 = 'registry problem'; 22 = 'disabled'; 24 = 'not present/not working';
  28 = 'NO DRIVER INSTALLED'; 31 = 'not working properly'; 32 = 'start type disabled'; 37 = 'driver entry failed';
  39 = 'driver missing/corrupt'; 43 = 'stopped after reporting problem'; 45 = 'not connected'; 52 = 'unsigned driver'
}
$bad = @($rows | Where-Object { $_.Problem -ne 0 })
Add-Report ''
Add-Report "### Devices with a problem: $($bad.Count) of $(@($rows).Count)"
foreach ($r in ($bad | Sort-Object InstanceId)) {
  Add-Report ("  [{0,2} {1}] {2}" -f $r.Problem, $problemText[$r.Problem], $r.Name)
  Add-Report ("        id : {0}" -f $r.InstanceId)
  Add-Report ("        hw : {0}" -f $r.HardwareIds)
  if ($r.CompatIds) { Add-Report ("        cid: {0}" -f $r.CompatIds) }
}

Add-Report ''
Add-Report '### All devices by class (Problem Class Name | InstanceId | Service | Inf)'
foreach ($r in ($rows | Sort-Object Class, Name)) {
  Add-Report ("  {0,2} {1,-16} {2} | {3} | {4} | {5}" -f $r.Problem, $r.Class, $r.Name, $r.InstanceId, $r.Service, $r.Inf)
}

# --- ACPI view: which DSDT devices Windows sees --------------------------------
$acpi = $rows | Where-Object { $_.InstanceId -like 'ACPI\*' } | Sort-Object InstanceId
$acpi | Format-Table -AutoSize Problem, Name, InstanceId, Service | Out-String -Width 300 |
  Out-File -FilePath (Join-Path $Out 'acpi-devices.txt') -Encoding UTF8
Save-Cmd 'acpi-tables.txt' { Get-ChildItem 'HKLM:\HARDWARE\ACPI' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name } }

# --- pnputil / driver store --------------------------------------------------
Save-Cmd 'pnputil-devices-problem.txt' { & $Pnputil /enum-devices /problem }
Save-Cmd 'pnputil-devices-all.txt' { & $Pnputil /enum-devices /drivers }
Save-Cmd 'pnputil-drivers.txt' { & $Pnputil /enum-drivers }

# --- system: CPU, memory, display, power, thermal ----------------------------
Save-Cmd 'system.txt' {
  '### OS'; Get-CimInstance Win32_OperatingSystem | Format-List Caption, Version, BuildNumber, OSArchitecture, LastBootUpTime, LocalDateTime
  '### Computer'; Get-CimInstance Win32_ComputerSystem | Format-List Manufacturer, Model, SystemType, TotalPhysicalMemory, NumberOfLogicalProcessors
  '### BIOS'; Get-CimInstance Win32_BIOS | Format-List Manufacturer, SMBIOSBIOSVersion, ReleaseDate
  '### CPU'; Get-CimInstance Win32_Processor | Format-List Name, NumberOfCores, CurrentClockSpeed, MaxClockSpeed, Architecture
  '### Display'; Get-CimInstance Win32_VideoController | Format-List Name, PNPDeviceID, CurrentHorizontalResolution, CurrentVerticalResolution, CurrentRefreshRate, DriverVersion, Status
  '### Battery'; Get-CimInstance Win32_Battery | Format-List *
  '### Thermal zones'; Get-CimInstance -Namespace root/wmi MSAcpi_ThermalZoneTemperature -ErrorAction SilentlyContinue | Format-List InstanceName, CurrentTemperature
  '### powercfg /a'; powercfg /a
  '### Processor performance states'; powercfg /query SCHEME_CURRENT SUB_PROCESSOR
}

Add-Report ''
Add-Report "Saved: $Out"
$Zip = "$Out.zip"
try { Compress-Archive -Path "$Out\*" -DestinationPath $Zip -Force; Add-Report "Zip: $Zip" } catch { Add-Report "Zip failed: $($_.Exception.Message)" }
Write-Host "Problem devices: $($bad.Count) of $(@($rows).Count)"
Write-Host "Report: $Report"
Write-Host "Zip:    $Zip"
