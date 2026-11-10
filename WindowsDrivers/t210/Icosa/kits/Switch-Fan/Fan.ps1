<#
.SYNOPSIS
  Fan control driver on the Switch's Windows (Administrator). Needs UEFI R372
  (DSDT FAN0) and the I2C driver from stage 3.

  -Mode Install : pnputil /add-driver switchfan.inf /install, then report
  -Mode Status  : report FAN0 (ACPI\SWOA0401) and the driver trace (temperature, duty)
  -Mode Watch   : print temperature and duty every 2 s (Ctrl+C to stop)
  -Mode Remove  : remove the driver package

  Log: C:\SwitchWOA-Fan.txt
#>
param([Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'Watch', 'Remove')][string]$Mode)

$ErrorActionPreference = 'Continue'
# Windows PowerShell on Windows 10 ARM64 is x86: reach System32 via Sysnative.
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-Fan.txt'
$Key = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchfan\Parameters'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) prompt.'; exit 1 }

function Show-Status {
  $dev = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'ACPI\SWOA0401*' })
  if ($dev.Count -eq 0) { Log 'FAN0: NOT FOUND (ACPI\SWOA0401). Is the Switch booted with UEFI R372?' }
  foreach ($d in $dev) {
    Log ("device: {0}  name='{1}' status={2} problem={3} service={4}" -f $d.PNPDeviceID, $d.Name, $d.Status, $d.ConfigManagerErrorCode, $d.Service)
  }
  $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
  if ($p) {
    $vals = $p.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object { '{0}=0x{1:X}' -f $_.Name, [uint32]$_.Value }
    Log ('driver trace: ' + ($vals -join ' '))
    if ($null -ne $p.Duty) { Log ("now: SoC {0} C, board {1} C, fan duty {2}/236 ({3}%)" -f $p.TempSoC, $p.TempBoard, $p.Duty, [int]($p.Duty * 100 / 236)) }
  } else { Log 'driver trace: (no Parameters key)' }
}

switch ($Mode) {
  'Install' {
    Log ''
    Log "=== Fan Install  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    $inf = Join-Path $Here 'switchfan.inf'
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    Log "  exit=$LASTEXITCODE"
    Start-Sleep -Seconds 5
    Show-Status
  }
  'Status' { Log ''; Log "=== Fan Status  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"; Show-Status }
  'Watch' {
    Write-Host 'time      SoC  board  duty  runs   (Ctrl+C to stop)'
    while ($true) {
      $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
      if ($p) { Write-Host ("{0}  {1,3}  {2,5}  {3,4}  {4}" -f (Get-Date -Format 'HH:mm:ss'), $p.TempSoC, $p.TempBoard, $p.Duty, $p.Runs) }
      Start-Sleep -Seconds 2
    }
  }
  'Remove' {
    $oem = & $Pnputil /enum-drivers 2>&1 | Out-String
    foreach ($x in [regex]::Matches($oem, '(?ms)Published Name\s*:\s*(oem\d+\.inf)\s*\r?\n\s*Original Name\s*:\s*switchfan\.inf')) {
      $name = $x.Groups[1].Value
      Log "pnputil /delete-driver $name /uninstall /force"
      (& $Pnputil /delete-driver $name /uninstall /force 2>&1) | ForEach-Object { Log "  $_" }
    }
    Show-Status
  }
}
