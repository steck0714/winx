<#
.SYNOPSIS
  Run on the PC with the Switch SD inserted (elevated PowerShell).
  Turns test signing on/off in the SD's boot BCD (EFI system partition), which
  the Switch's Windows cannot open itself ("volume has been externally altered").

    -Mode Show    : print the BCD only
    -Mode Enable  : back up the BCD, then testsigning on for the default loader
    -Mode Disable : back up the BCD, then testsigning off

  -WindowsDrive is the SD's Windows partition (e.g. F:). The EFI system
  partition on the same disk gets a temporary drive letter, removed at the end.
#>
param(
  [Parameter(Mandatory = $true)][string]$WindowsDrive,
  [Parameter(Mandatory = $true)][ValidateSet('Show', 'Enable', 'Disable')][string]$Mode,
  [string]$BackupRoot = 'D:\Codex\SwitchWOA-Bootstrap\outputs\Windows-Drivers\Switch-Stage0'
)

$ErrorActionPreference = 'Stop'
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw 'Run this from an elevated (Administrator) PowerShell.' }
$Letter = $WindowsDrive.TrimEnd('\', ':')
if (-not (Test-Path "${Letter}:\Windows\System32\config\SYSTEM")) { throw "${Letter}: is not the Switch Windows partition." }

$winPart = Get-Partition -DriveLetter $Letter
$disk = Get-Disk -Number $winPart.DiskNumber
Write-Host ("Disk {0}: {1} {2:N1} GB bus={3}" -f $disk.Number, $disk.FriendlyName, ($disk.Size / 1GB), $disk.BusType)
if ($disk.Number -eq (Get-Partition -DriveLetter ($env:SystemDrive.TrimEnd(':'))).DiskNumber) { throw 'Refusing: that is this PC''s system disk.' }
$esp = Get-Partition -DiskNumber $disk.Number | Where-Object { $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' } | Select-Object -First 1
if (-not $esp) { throw 'No EFI system partition on that disk.' }

$added = $false
$espLetter = $esp.DriveLetter
if (-not $espLetter -or $espLetter -eq [char]0) {
  $esp | Add-PartitionAccessPath -AssignDriveLetter
  $esp = Get-Partition -DiskNumber $disk.Number -PartitionNumber $esp.PartitionNumber
  $espLetter = $esp.DriveLetter
  $added = $true
}
try {
  $Store = "${espLetter}:\EFI\Microsoft\Boot\BCD"
  if (-not (Test-Path $Store)) { throw "No BCD at $Store" }
  Write-Host "BCD store: $Store"
  & bcdedit /store $Store /enum '{default}'
  if ($Mode -ne 'Show') {
    # bcdedit cannot combine /store with /export, so copy the store file (and
    # its transaction logs) as the backup; the PC does not have it open.
    $backup = Join-Path $BackupRoot ("BCD-backup-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force $backup | Out-Null
    Get-ChildItem -Force (Split-Path $Store) | Where-Object { $_.Name -like 'BCD*' } |
      ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $backup -Force }
    if (-not (Test-Path (Join-Path $backup 'BCD'))) { throw 'BCD backup copy failed; nothing changed.' }
    Write-Host "Backup: $backup ($((Get-ChildItem $backup | ForEach-Object Name) -join ', '))"
    $value = if ($Mode -eq 'Enable') { 'on' } else { 'off' }
    & bcdedit /store $Store /set '{default}' testsigning $value
    if ($LASTEXITCODE -ne 0) { throw "bcdedit /set testsigning $value failed." }
    Write-Host "After:"
    & bcdedit /store $Store /enum '{default}' | Select-String -Pattern 'identifier|description|osdevice|testsigning'
  }
}
finally {
  if ($added) { $esp | Remove-PartitionAccessPath -AccessPath "${espLetter}:\" }
}
