<#
.SYNOPSIS
  Real-time clock driver on the Switch's Windows (Administrator). Needs UEFI
  R377 (DSDT RTC0) and the I2C driver v3 (stage 3).

  -Mode Install : pnputil /add-driver switchrtc.inf /install, then report
  -Mode Status  : report RTC0 (ACPI\SWOA0801), the RTC reading, the learned offset and the driver trace
  -Mode Sync    : w32tm /resync (needs the network); the driver then learns the offset
  -Mode Remove  : remove the driver package

  Log: C:\SwitchWOA-Rtc.txt
#>
param([Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'Sync', 'Remove')][string]$Mode)

$ErrorActionPreference = 'Continue'
# Windows PowerShell on Windows 10 ARM64 is x86: reach System32 via Sysnative.
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$W32tm = Join-Path $Sys 'w32tm.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-Rtc.txt'
$Key = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchrtc\Parameters'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) prompt.'; exit 1 }

function FromBcd([int]$v, [bool]$binary) { if ($binary) { $v } else { ($v -shr 4) * 10 + ($v -band 0xF) } }

function Show-Status {
  $dev = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'ACPI\SWOA0801*' })
  if ($dev.Count -eq 0) { Log 'RTC0: NOT FOUND (ACPI\SWOA0801). Is the Switch booted with UEFI R377?' }
  foreach ($d in $dev) {
    Log ("device: {0}  name='{1}' status={2} problem={3} service={4}" -f $d.PNPDeviceID, $d.Name, $d.Status, $d.ConfigManagerErrorCode, $d.Service)
  }
  $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
  if (-not $p) { Log 'driver trace: (no Parameters key)'; return }
  $vals = $p.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object { '{0}=0x{1:X}' -f $_.Name, $_.Value }
  Log ('driver trace: ' + ($vals -join ' '))
  if ($null -ne $p.RtcRaw0) {
    $bin = ([int]$p.RtcControl -band 1) -eq 0      # CONTROL bit 0 set = BCD
    $r0 = [uint32]$p.RtcRaw0; $r1 = [uint32]$p.RtcRaw1
    $sec = FromBcd ($r0 -band 0x7F) $bin; $min = FromBcd (($r0 -shr 8) -band 0x7F) $bin
    $hour = FromBcd (($r0 -shr 16) -band 0x3F) $bin
    $month = FromBcd ($r1 -band 0x1F) $bin; $year = 2000 + (FromBcd (($r1 -shr 8) -band 0x7F) $bin)
    $day = FromBcd (($r1 -shr 16) -band 0x3F) $bin
    Log ("RTC (last read by the driver, raw): {0:D4}-{1:D2}-{2:D2} {3:D2}:{4:D2}:{5:D2}" -f $year, $month, $day, $hour, $min, $sec)
  }
  if ($null -ne $p.OffsetSeconds -and $p.OffsetFormat -eq 2) {
    $o = [int64]$p.OffsetSeconds
    Log ("learned offset: {0} s (~{1:N1} days)" -f $o, ($o / 86400))
  } else { Log 'learned offset: none yet (run -Mode Sync once the network is up)' }
  Log ("now (UTC): {0:yyyy-MM-dd HH:mm:ss}  local: {1:yyyy-MM-dd HH:mm:ss}" -f (Get-Date).ToUniversalTime(), (Get-Date))
}

switch ($Mode) {
  'Install' {
    Log ''
    Log "=== Rtc Install  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    $inf = Join-Path $Here 'switchrtc.inf'
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    Log "  exit=$LASTEXITCODE"
    Start-Sleep -Seconds 3
    Show-Status
  }
  'Status' { Log ''; Log "=== Rtc Status  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"; Show-Status }
  'Sync' {
    Log ''
    Log "=== Rtc Sync  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    (& $W32tm /resync /force 2>&1) | ForEach-Object { Log "  $_" }
    Start-Sleep -Seconds 2
    Show-Status
  }
  'Remove' {
    $oem = & $Pnputil /enum-drivers 2>&1 | Out-String
    foreach ($x in [regex]::Matches($oem, '(?ms)Published Name\s*:\s*(oem\d+\.inf)\s*\r?\n\s*Original Name\s*:\s*switchrtc\.inf')) {
      $name = $x.Groups[1].Value
      Log "pnputil /delete-driver $name /uninstall /force"
      (& $Pnputil /delete-driver $name /uninstall /force 2>&1) | ForEach-Object { Log "  $_" }
    }
    Show-Status
  }
}
