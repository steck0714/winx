<#
.SYNOPSIS
  Bluetooth driver, stage 1 (link check), on the Switch's Windows (Administrator). Needs UEFI R390
  (DSDT BTH0; UART-D clocked and the BCM4356A3 powered).

  -Mode Install : pnputil /add-driver switchbt.inf /install, then report
  -Mode Status  : report BTH0 (ACPI\SWOA0B01), the driver trace (chip answers) and whether
                  Windows has the BthMini UART transport (MS_BTHX_BTHMINI in bth.inf)
  -Mode Remove  : remove the driver package
  -Mode Retry   : after a boot that hung, the driver stays idle (SafeMode=1); this lets it run again
                  at the next boot (Install does the same)
  -Mode Firmware -Path <file> : copy the Broadcom patch file BCM4356A3.hcd that you have (for example
                  from a Switchroot Linux or Android install) to System32\drivers\SwitchWOA, where
                  switchbt 1.0.0.8+ loads it at start. The file is Broadcom's and is not shipped here.

  Log: C:\SwitchWOA-Bluetooth.txt
#>
param([Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'Remove', 'Retry', 'Firmware')][string]$Mode,
      [string]$Path)

$ErrorActionPreference = 'Continue'
# Windows PowerShell on Windows 10 ARM64 is x86: reach System32 via Sysnative.
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-Bluetooth.txt'
$Key = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchbt\Parameters'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) prompt.'; exit 1 }

function Show-Status {
  $dev = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'ACPI\SWOA0B01*' })
  if ($dev.Count -eq 0) { Log 'BTH0: NOT FOUND (ACPI\SWOA0B01). Is the Switch booted with UEFI R390?' }
  foreach ($d in $dev) {
    Log ("device: {0}  name='{1}' status={2} problem={3} service={4}" -f $d.PNPDeviceID, $d.Name, $d.Status, $d.ConfigManagerErrorCode, $d.Service)
  }
  $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
  if ($p) {
    $vals = $p.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object {
      if ($_.Value -is [string]) { '{0}={1}' -f $_.Name, $_.Value } else { '{0}=0x{1:X}' -f $_.Name, $_.Value } }
    Log ('driver trace: ' + ($vals -join ' '))
    # 1.0.0.5: how far the last boot with the driver got (1 added, 2 queues, 3 hardware, 4 probe, 5 running, 6 child added).
    if ($null -ne $p.Stage) {
      Log ("boot record: stage {0}  boot pending {1}  safe mode {2}" -f $p.Stage, $p.BootPending, $p.SafeMode)
      if ($p.SafeMode -eq 1) { Log 'SAFE MODE: the boot before did not finish, so the driver stays idle. -Mode Retry lets it run again.' }
    }
    if ($p.Result -eq 'ok') {
      Log ("chip: '{0}'  address {1}  maker 0x{2:X4} (000F = Broadcom)  LMP subversion 0x{3:X4}" -f $p.LocalName, $p.BdAddr, $p.Manufacturer, $p.LmpSubversion)
      Log 'RESULT: the Bluetooth chip answers.'
    } else {
      Log "RESULT: the chip did not answer ($($p.Result))."
    }
  } else { Log 'driver trace: (no Parameters key)' }
  $bth = Join-Path $env:windir 'INF\bth.inf'
  $mini = (Test-Path $bth) -and (Select-String -Path $bth -Pattern 'MS_BTHX_BTHMINI' -Quiet)
  $sysFile = Test-Path (Join-Path $Sys 'drivers\BthMini.sys')
  Log ("Windows BthMini: bth.inf has MS_BTHX_BTHMINI={0}  BthMini.sys={1}" -f $mini, $sysFile)
  # Stage 2: the radio child (SWOA\BTH_UART_H4, BthMini) and what Windows built on it.
  $radio = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'SWOA\BTH_UART_H4*' })
  if ($radio.Count -eq 0) { Log 'radio: NOT FOUND (SWOA\BTH_UART_H4). Is switchbt 1.0.0.2 installed?' }
  foreach ($d in $radio) {
    Log ("radio: {0}  name='{1}' status={2} problem={3} service={4}" -f $d.PNPDeviceID, $d.Name, $d.Status, $d.ConfigManagerErrorCode, $d.Service)
  }
  $bt = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPClass -eq 'Bluetooth' })
  foreach ($d in $bt) { Log ("bluetooth: '{0}' status={1} problem={2}" -f $d.Name, $d.Status, $d.ConfigManagerErrorCode) }
  $svc = Get-Service bthserv -ErrorAction SilentlyContinue
  if ($svc) { Log ("Bluetooth Support Service: {0}" -f $svc.Status) }
  if ($radio.Count -gt 0 -and $radio[0].ConfigManagerErrorCode -eq 0) { Log 'RESULT 2: the radio runs under BthMini. Look for Bluetooth in Settings.' }
  # Traffic between the Windows Bluetooth stack and the chip (switchbt 1.0.0.3+).
  if ($p -and $null -ne $p.IsrCalls) {
    Log ("traffic: interrupts {0} (IIR seen 0x{1:X})  sent {2} bytes ({3} commands, {4} ACL)  received {5} bytes ({6} events, {7} ACL)" -f `
      $p.IsrCalls, $p.IirSeen, $p.TxBytes, $p.CmdsOut, $p.AclOut, $p.RxBytes, $p.EventsIn, $p.AclIn)
    Log ("requests: write {0}  read event {1}  read ACL {2}  other {3} (last 0x{4:X})  tx busy {5}  dropped {6}  overflow {7}" -f `
      $p.WriteRequests, $p.ReadEventRequests, $p.ReadAclRequests, $p.OtherIoctls, $p.LastOtherIoctl, $p.TxBusy, $p.Dropped, $p.RxOverflows)
    if ($null -ne $p.LineOverruns) {
      Log ("receive errors (1.0.0.10+): UART overruns {0}  parity/framing/break {1}  bytes skipped to find a packet {2}" -f `
        $p.LineOverruns, $p.LineErrors, $p.ParserSkipped)
    }
    if ($null -ne $p.ChildD0) {
      Log ("child (1.0.0.6+, added 20 s after start): D0 {0}  requests {1}  forward failures {2} (last status 0x{3:X8})  last IOCTL 0x{4:X}" -f `
        $p.ChildD0, $p.ChildRequests, $p.ChildForwardFailures, $p.LastForwardStatus, $p.LastChildIoctl)
    }
    if ($null -ne $p.UartClockMHz) {
      $hs = @{ 0 = '115200 (HighSpeed=0, or no CAR access)'; 1 = 'refused by the chip, 115200'; 2 = '3 Mbaud, flow control'; 3 = '3 Mbaud, no flow control'; 4 = 'LOST at 3 Mbaud'; 5 = '3 Mbaud (the chip was still at 3 Mbaud from the last session)' }
      $fw = if ($p.FirmwareResult -eq 1) { "patched ($($p.FirmwareRecords) records, $($p.FirmwareBytes) bytes)" } elseif ($p.FirmwareResult -eq 0) { 'none (ROM code)' } elseif ($p.FirmwareResult -eq 2) { 'kept from the last session' } else { 'FAILED 0x{0:X8}' -f $p.FirmwareResult }
      Log ("stage 3 (1.0.0.8+): UART clock {0} MHz  speed {1}  firmware {2}" -f $p.UartClockMHz, $hs[[int]$p.HighSpeedResult], $fw)
    }
    if ($null -ne $p.CapsRequests) {
      Log ("setup (1.0.0.7+): get version {0}  set version {1} (value {2})  capabilities {3}  last failure 0x{4:X8}" -f `
        $p.GetVersionRequests, $p.SetVersionRequests, $p.SetVersionValue, $p.CapsRequests, $p.LastSetupStatus)
    }
    Log ("last commands: {0}" -f $p.LastCommands)
    Log ("last events (code:opcode): {0}" -f $p.LastEvents)
  }
  $ev = @(Get-WinEvent -LogName System -MaxEvents 400 -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -match 'BTH|Bluetooth' } | Select-Object -First 8)
  foreach ($e in $ev) {
    $msg = ($e.Message -replace '\s+', ' ')
    if ($msg.Length -gt 160) { $msg = $msg.Substring(0, 160) }
    Log ("event {0:HH:mm:ss} {1} id={2}: {3}" -f $e.TimeCreated, $e.ProviderName, $e.Id, $msg)
  }
}

function Clear-BootPending {
  if (Test-Path $Key) { Set-ItemProperty -Path $Key -Name BootPending -Value 0 -Type DWord }
}

switch ($Mode) {
  'Firmware' {
    Log ''
    Log "=== Bluetooth Firmware  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { Log "File not found: '$Path'. Use -Path <your BCM4356A3.hcd>."; exit 1 }
    $b = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Path).Path)
    # An .hcd is a list of HCI commands: opcode (2, little endian), length, parameters. Broadcom's are
    # vendor commands (opcode high byte FC) and the list ends exactly at the end of the file.
    $off = 0; $n = 0; $ok = $b.Length -gt 3
    while ($ok -and $off + 3 -le $b.Length) {
      if ($b[$off + 1] -ne 0xFC) { $ok = $false; break }
      $off += 3 + $b[$off + 2]; $n++
    }
    if (-not $ok -or $off -ne $b.Length) { Log "Not a Broadcom .hcd patch file ($($b.Length) bytes). Nothing copied."; exit 1 }
    $dir = Join-Path $Sys 'drivers\SwitchWOA'
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item -LiteralPath $Path -Destination (Join-Path $dir 'BCM4356A3.hcd') -Force
    Log ("Copied {0} ({1} bytes, {2} records) to {3}\BCM4356A3.hcd. It is used from the next start of the driver (reboot)." -f $Path, $b.Length, $n, $dir)
  }
  'Retry' {
    Log ''
    Log "=== Bluetooth Retry  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    Clear-BootPending
    Log 'BootPending cleared: switchbt 1.0.0.13+ starts within a few seconds (older versions at the next restart).'
  }
  'Install' {
    Log ''
    Log "=== Bluetooth Install  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    Clear-BootPending
    $inf = Join-Path $Here 'switchbt.inf'
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    Log "  exit=$LASTEXITCODE"
    Start-Sleep -Seconds 5
    Show-Status
  }
  'Status' { Log ''; Log "=== Bluetooth Status  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"; Show-Status }
  'Remove' {
    $oem = & $Pnputil /enum-drivers 2>&1 | Out-String
    foreach ($x in [regex]::Matches($oem, '(?ms)Published Name\s*:\s*(oem\d+\.inf)\s*\r?\n\s*Original Name\s*:\s*switchbt\.inf')) {
      $name = $x.Groups[1].Value
      Log "pnputil /delete-driver $name /uninstall /force"
      (& $Pnputil /delete-driver $name /uninstall /force 2>&1) | ForEach-Object { Log "  $_" }
    }
    Show-Status
  }
}
