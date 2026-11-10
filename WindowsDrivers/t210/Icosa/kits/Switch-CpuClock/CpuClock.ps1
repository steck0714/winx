<#
.SYNOPSIS
  CPU clock control on the Switch's Windows (Administrator). Needs UEFI R375
  (DSDT CPUC) and the I2C driver v3 (stage 3).

  -Mode Install  : pnputil /add-driver switchcpu.inf /install, then report
  -Mode Status   : device state, current clock/voltage/temperature, profile table, driver trace
  -Mode Set      : -Profile <n>  switch to profile n (see Status), then report
  -Mode Bench    : 3 s single-thread loop benchmark at the current clock
  -Mode Test     : Bench at every non-experimental profile, then back to the default
  -Mode Watch    : clock, voltage and temperature every second (Ctrl+C to stop)
  -Mode Remove   : remove the driver package (the driver puts the default clock back)

  Log: C:\SwitchWOA-CpuClock.txt
#>
param(
  [Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'Set', 'Bench', 'Test', 'Watch', 'Remove')][string]$Mode,
  [int]$Profile = -1
)

$ErrorActionPreference = 'Continue'
# Windows PowerShell on Windows 10 ARM64 is x86: reach System32 via Sysnative.
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-CpuClock.txt'
$Key = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchcpu\Parameters'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) prompt.'; exit 1 }

Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class SwCpu {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] inBuf, int inLen, byte[] outBuf, int outLen, out int returned, IntPtr ov);

    public const uint GET_STATUS = 0x00226000, GET_PROFILES = 0x00226004, SET_PROFILE = 0x0022A008;

    static byte[] Call(uint code, byte[] input, int outLen) {
        using (SafeFileHandle h = CreateFile(@"\\.\SwitchCpu", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
            if (h.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "open \\\\.\\SwitchCpu");
            byte[] output = new byte[outLen];
            int returned;
            if (!DeviceIoControl(h, code, input, input == null ? 0 : input.Length, output, outLen, out returned, IntPtr.Zero))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), string.Format("IOCTL 0x{0:X8}", code));
            return output;
        }
    }
    // SWCPU_STATUS: 12 ULONG/LONG fields.
    public static int[] Status() {
        byte[] b = Call(GET_STATUS, null, 48);
        int[] v = new int[12];
        for (int i = 0; i < 12; i++) v[i] = BitConverter.ToInt32(b, i * 4);
        return v;
    }
    // SWCPU_PROFILES: Count, then 8 x (Khz, Mv, Flags).
    public static int[] Profiles() {
        byte[] b = Call(GET_PROFILES, null, 4 + 8 * 12);
        int[] v = new int[1 + 8 * 3];
        for (int i = 0; i < v.Length; i++) v[i] = BitConverter.ToInt32(b, i * 4);
        return v;
    }
    public static void Set(int profile) { Call(SET_PROFILE, BitConverter.GetBytes(profile), 0); }

    public static long Bench(int ms) {
        Stopwatch sw = Stopwatch.StartNew();
        long n = 0; uint x = 12345;
        while (sw.ElapsedMilliseconds < ms) {
            for (int i = 0; i < 10000; i++) { x = x * 1103515245 + 12345; }
            n += 10000;
        }
        if (x == 1) n++;
        return n * 1000 / ms;
    }
}
'@

function Show-Clock {
  try {
    $s = [SwCpu]::Status()
    $t = if ($s[8] -ne 0) { "SoC $($s[6]) C, board $($s[7]) C" } else { 'temperature unreadable' }
    Log ("now: {0:N1} MHz, {1} mV, profile {2} (requested {3}){4}, {5}, switches={6} lastError=0x{7:X8}" -f ($s[1] / 1000), $s[2], $s[3], $s[4],
         $(if ($s[5] -ne 0) { ' THROTTLED' } else { '' }), $t, $s[10], $s[11])
    return $true
  } catch { Log "status: $($_.Exception.Message)"; return $false }
}

function Show-Profiles {
  try {
    $p = [SwCpu]::Profiles()
    for ($i = 0; $i -lt $p[0]; $i++) {
      $f = $p[3 + $i * 3]
      $tag = @(); if ($f -band 2) { $tag += 'default' }; if ($f -band 1) { $tag += 'EXPERIMENTAL' }
      Log ("  profile {0}: {1,7:N1} MHz  {2} mV  {3}" -f $i, ($p[1 + $i * 3] / 1000), $p[2 + $i * 3], ($tag -join ' '))
    }
  } catch { Log "profiles: $($_.Exception.Message)" }
}

function Show-Status {
  $dev = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'ACPI\SWOA0701*' })
  if ($dev.Count -eq 0) { Log 'CPUC: NOT FOUND (ACPI\SWOA0701). Is the Switch booted with UEFI R375?' }
  foreach ($d in $dev) {
    Log ("device: {0}  name='{1}' status={2} problem={3} service={4}" -f $d.PNPDeviceID, $d.Name, $d.Status, $d.ConfigManagerErrorCode, $d.Service)
  }
  if (Show-Clock) { Show-Profiles }
  $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
  if ($p) {
    $vals = $p.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object { '{0}=0x{1:X}' -f $_.Name, [uint32]$_.Value }
    Log ('driver trace: ' + ($vals -join ' '))
  } else { Log 'driver trace: (no Parameters key)' }
}

function Run-Bench {
  $r = [SwCpu]::Bench(3000)
  Log ("bench: {0:N0} loop/s" -f $r)
  return $r
}

switch ($Mode) {
  'Install' {
    Log ''
    Log "=== CpuClock Install  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    $inf = Join-Path $Here 'switchcpu.inf'
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    Log "  exit=$LASTEXITCODE"
    Start-Sleep -Seconds 5
    Show-Status
  }
  'Status' { Log ''; Log "=== CpuClock Status  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"; Show-Status }
  'Set' {
    if ($Profile -lt 0) { Write-Host 'Give -Profile <n> (see -Mode Status).'; break }
    Log ''
    Log "=== CpuClock Set $Profile  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    try { [SwCpu]::Set($Profile); Log 'set: OK' } catch { Log "set: $($_.Exception.Message)" }
    Show-Clock | Out-Null
  }
  'Bench' { Log ''; Show-Clock | Out-Null; Run-Bench | Out-Null }
  'Test' {
    Log ''
    Log "=== CpuClock Test  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    $p = [SwCpu]::Profiles()
    $default = 1
    for ($i = 0; $i -lt $p[0]; $i++) {
      if ($p[3 + $i * 3] -band 2) { $default = $i }
      if ($p[3 + $i * 3] -band 1) { continue }      # experimental profiles only by hand
      Log "--- profile $i"
      try { [SwCpu]::Set($i) } catch { Log "set: $($_.Exception.Message)"; continue }
      Start-Sleep -Milliseconds 500
      Show-Clock | Out-Null
      $r = Run-Bench
      Log ("    loop/s per MHz: {0:N0}" -f ($r / ($p[1 + $i * 3] / 1000)))
    }
    Log "--- back to profile $default"
    try { [SwCpu]::Set($default) } catch { Log "set: $($_.Exception.Message)" }
    Show-Clock | Out-Null
  }
  'Watch' {
    while ($true) {
      try {
        $s = [SwCpu]::Status()
        Write-Host ("{0}  {1,7:N1} MHz  {2,4} mV  profile {3}  SoC {4} C{5}" -f (Get-Date -Format 'HH:mm:ss'), ($s[1] / 1000), $s[2], $s[3], $s[6],
                    $(if ($s[5] -ne 0) { '  THROTTLED' } else { '' }))
      } catch { Write-Host $_.Exception.Message }
      Start-Sleep -Seconds 1
    }
  }
  'Remove' {
    $oem = & $Pnputil /enum-drivers 2>&1 | Out-String
    foreach ($x in [regex]::Matches($oem, '(?ms)Published Name\s*:\s*(oem\d+\.inf)\s*\r?\n\s*Original Name\s*:\s*switchcpu\.inf')) {
      $name = $x.Groups[1].Value
      Log "pnputil /delete-driver $name /uninstall /force"
      (& $Pnputil /delete-driver $name /uninstall /force 2>&1) | ForEach-Object { Log "  $_" }
    }
    Show-Status
  }
}
