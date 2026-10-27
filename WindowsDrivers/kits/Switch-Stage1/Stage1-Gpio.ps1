<#
.SYNOPSIS
  Stage 1 on the Switch's Windows (Administrator). Needs UEFI R367 (DSDT GPI0).

  -Mode Install : pnputil /add-driver tegra210gpio.inf /install, then report
  -Mode Status  : report the GPI0 device (ACPI\SWOA0101) and the driver
  -Mode Remove  : remove the driver package from the driver store

  Log: C:\SwitchWOA-Stage1.txt
#>
param([Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'Remove')][string]$Mode)

$ErrorActionPreference = 'Continue'
# Windows PowerShell on Windows 10 ARM64 is x86: reach System32 via Sysnative.
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-Stage1.txt'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) prompt.'; exit 1 }
Log ''
Log "=== Stage1 $Mode  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"

function Show-Status {
  $dev = Get-CimInstance Win32_PnPEntity -Filter "PNPDeviceID LIKE 'ACPI\\SWOA0101%'" -ErrorAction SilentlyContinue
  if (-not $dev) {
    Log 'GPI0 device: NOT FOUND (ACPI\SWOA0101). Is the Switch booted with UEFI R367?'
  } else {
    foreach ($d in $dev) {
      Log ("GPI0 device: {0}  name='{1}' status={2} problem={3} service={4}" -f $d.PNPDeviceID, $d.Name, $d.Status, $d.ConfigManagerErrorCode, $d.Service)
    }
  }
  # Stage 2 (R368): the button array and the HID devices created under it.
  $btn = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
         Where-Object { $_.PNPDeviceID -like 'ACPI\PNP0C40*' -or $_.PNPDeviceID -like 'ACPI\ACPI0011*' -or
                        $_.PNPDeviceID -like 'HID\*PNP0C40*' -or $_.PNPDeviceID -like 'HID\*ACPI0011*' -or ($_.PNPDeviceID -like 'HID\*' -and $_.Name -match 'consumer|button|system controller' -and $_.PNPDeviceID -notlike 'HID\VID_*') }
  if (-not $btn) { Log 'button array: NOT FOUND (ACPI\PNP0C40). Is the Switch booted with UEFI R368?' }
  foreach ($b in $btn) {
    Log ("button device: {0}  name='{1}' status={2} problem={3} service={4}" -f $b.PNPDeviceID, $b.Name, $b.Status, $b.ConfigManagerErrorCode, $b.Service)
  }
  Log ('pnputil ACPI0011: ' + ((& $Pnputil /enum-devices /instanceid 'ACPI\ACPI0011\0' /drivers 2>&1) -join ' ; ' -replace '\s+', ' '))
  Log ('sc query tegra210gpio: ' + ((& sc.exe query tegra210gpio 2>&1) -join ' ' -replace '\s+', ' '))
  # Driver v2 records each start-up step here (DWORD, NTSTATUS in hex).
  $p = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\tegra210gpio\Parameters' -ErrorAction SilentlyContinue
  if ($p) {
    $vals = $p.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object { '{0}=0x{1:X}' -f $_.Name, [uint32]$_.Value }
    Log ('driver trace: ' + ($vals -join ' '))
  } else { Log 'driver trace: (no Parameters key)' }
  $oem = & $Pnputil /enum-drivers 2>&1 | Out-String
  $m = [regex]::Matches($oem, '(?ms)Published Name\s*:\s*(oem\d+\.inf)\s*\r?\n\s*Original Name\s*:\s*tegra210gpio\.inf')
  Log ('driver store: ' + $(if ($m.Count) { ($m | ForEach-Object { $_.Groups[1].Value }) -join ', ' } else { '(not installed)' }))
  Log ('pnputil /enum-devices /instanceid: ' + ((& $Pnputil /enum-devices /instanceid 'ACPI\SWOA0101\0' 2>&1) -join ' ; ' -replace '\s+', ' '))
}

switch ($Mode) {
  'Install' {
    $inf = Join-Path $Here 'tegra210gpio.inf'
    Log "pnputil /add-driver $inf /install"
    $out = & $Pnputil /add-driver $inf /install 2>&1
    $out | ForEach-Object { Log "  $_" }
    Log "  exit=$LASTEXITCODE"
    Start-Sleep -Seconds 3
    Show-Status
    Log 'Expected: status=OK problem=0 service=tegra210gpio. Problem 28 = no driver, 10/31 = driver failed to start.'
  }
  'Status' { Show-Status }
  'Remove' {
    $oem = & $Pnputil /enum-drivers 2>&1 | Out-String
    foreach ($x in [regex]::Matches($oem, '(?ms)Published Name\s*:\s*(oem\d+\.inf)\s*\r?\n\s*Original Name\s*:\s*tegra210gpio\.inf')) {
      $name = $x.Groups[1].Value
      Log "pnputil /delete-driver $name /uninstall /force"
      (& $Pnputil /delete-driver $name /uninstall /force 2>&1) | ForEach-Object { Log "  $_" }
    }
    Show-Status
  }
}
Log "Log: $Log"
