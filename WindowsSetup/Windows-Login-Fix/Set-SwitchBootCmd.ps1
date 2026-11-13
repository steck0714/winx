<#
.SYNOPSIS
  Make the Switch Windows SD open a SYSTEM command prompt before logon (and undo it).

.DESCRIPTION
  This WRITES to the SD's SYSTEM hive. Before any change, it copies the
  SYSTEM hive (and .LOG1/.LOG2) to a backup folder on this PC.

  -Mode Enable  : HKLM\SYSTEM\Setup  SetupType=2, CmdLine="cmd.exe"
                  (Windows runs CmdLine as SYSTEM during boot, before LogonUI)
  -Mode Disable : SetupType=0, CmdLine="" (normal boot)
  -Mode Show    : print the current values only (no write)

  Run from an elevated PowerShell with the SD's Windows partition as -WindowsDrive:
    powershell -ExecutionPolicy Bypass -File Set-SwitchBootCmd.ps1 -WindowsDrive F: -Mode Enable
#>
param(
  [Parameter(Mandatory = $true)][string]$WindowsDrive,
  [Parameter(Mandatory = $true)][ValidateSet('Enable', 'Disable', 'Show')][string]$Mode,
  [string]$BackupRoot = "D:\Codex\SwitchWOA-Bootstrap\outputs\Windows-Login-Fix"
)

$ErrorActionPreference = 'Stop'
$WindowsDrive = $WindowsDrive.TrimEnd('\', ':') + ':'
$Config = Join-Path $WindowsDrive 'Windows\System32\config'
$Hive = Join-Path $Config 'SYSTEM'
if (-not (Test-Path $Hive)) { throw "No Windows\System32\config\SYSTEM on $WindowsDrive." }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw 'Run this from an elevated (Administrator) PowerShell.' }

function Show-Setup {
  $p = Get-ItemProperty -Path 'HKLM:\SWOA_FIX\Setup'
  Write-Host ("  SetupType={0} CmdLine='{1}' SystemSetupInProgress={2} OOBEInProgress={3}" -f $p.SetupType, $p.CmdLine, $p.SystemSetupInProgress, $p.OOBEInProgress)
}

if ($Mode -ne 'Show') {
  $Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
  $Backup = Join-Path $BackupRoot "SYSTEM-backup-$Stamp"
  New-Item -ItemType Directory -Force -Path $Backup | Out-Null
  robocopy $Config $Backup SYSTEM SYSTEM.LOG1 SYSTEM.LOG2 /B /R:0 /W:0 /NJH /NJS /NP | Out-Null
  if (-not (Test-Path (Join-Path $Backup 'SYSTEM'))) { throw "Backup failed; nothing was changed." }
  Write-Host "Backup of the SD's SYSTEM hive: $Backup"
}

& reg.exe load HKLM\SWOA_FIX $Hive | Out-Null
if ($LASTEXITCODE -ne 0) { throw "reg load failed for $Hive" }
try {
  Write-Host 'Before:'; Show-Setup
  if ($Mode -eq 'Enable') {
    Set-ItemProperty -Path 'HKLM:\SWOA_FIX\Setup' -Name SetupType -Value 2 -Type DWord
    Set-ItemProperty -Path 'HKLM:\SWOA_FIX\Setup' -Name CmdLine -Value 'cmd.exe' -Type String
  } elseif ($Mode -eq 'Disable') {
    Set-ItemProperty -Path 'HKLM:\SWOA_FIX\Setup' -Name SetupType -Value 0 -Type DWord
    Set-ItemProperty -Path 'HKLM:\SWOA_FIX\Setup' -Name CmdLine -Value '' -Type String
  }
  Write-Host 'After:'; Show-Setup
}
finally {
  [GC]::Collect(); [GC]::WaitForPendingFinalizers()
  & reg.exe unload HKLM\SWOA_FIX | Out-Null
}
