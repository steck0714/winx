<#
  Run on the Switch, in the SYSTEM command prompt (Set-SwitchBootCmd.ps1 -Mode Enable).

    powershell -ExecutionPolicy Bypass -File C:\swoa-fix-primarydomain.ps1          (show only)
    powershell -ExecutionPolicy Bypass -File C:\swoa-fix-primarydomain.ps1 -Fix     (set workgroup)

  Shows the LSA account / primary / DNS domain information and the raw
  HKLM\SECURITY\Policy values (readable as SYSTEM). With -Fix it sets the
  workgroup "WORKGROUP" in three ways, re-reading after each, and stops at
  the first one that reads back:
    1. LsaSetInformationPolicy(PolicyDnsDomainInformation)  (Name=WORKGROUP, no DNS name/GUID/SID)
    2. LsaSetInformationPolicy(PolicyPrimaryDomainInformation)
    3. Win32_ComputerSystem.JoinDomainOrWorkgroup("WORKGROUP")  (what "join workgroup" does)
  then flushes HKLM\SECURITY to disk. Log: C:\primarydomain.txt
#>
param([switch]$Fix, [string]$Workgroup = 'WORKGROUP')

$Out = 'C:\primarydomain.txt'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Out -Value $s -Encoding UTF8 }
Add-Content -Path $Out -Value "`r`nswoa-fix-primarydomain v2  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  Fix=$Fix" -Encoding UTF8

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SwoaLsa2 {
  [StructLayout(LayoutKind.Sequential)]
  public struct LSA_OBJECT_ATTRIBUTES { public int Length; public IntPtr RootDirectory; public IntPtr ObjectName; public uint Attributes; public IntPtr SecurityDescriptor; public IntPtr SecurityQualityOfService; }
  [StructLayout(LayoutKind.Sequential)]
  public struct LSA_UNICODE_STRING { public ushort Length; public ushort MaximumLength; public IntPtr Buffer; }
  [StructLayout(LayoutKind.Sequential)]
  public struct DOMAIN_INFO { public LSA_UNICODE_STRING Name; public IntPtr Sid; }
  [StructLayout(LayoutKind.Sequential)]
  public struct DNS_DOMAIN_INFO { public LSA_UNICODE_STRING Name; public LSA_UNICODE_STRING DnsDomainName; public LSA_UNICODE_STRING DnsForestName; public Guid DomainGuid; public IntPtr Sid; }
  [DllImport("advapi32.dll")] public static extern uint LsaOpenPolicy(IntPtr system, ref LSA_OBJECT_ATTRIBUTES attrs, uint access, out IntPtr handle);
  [DllImport("advapi32.dll")] public static extern uint LsaQueryInformationPolicy(IntPtr handle, int infoClass, out IntPtr buffer);
  [DllImport("advapi32.dll")] public static extern uint LsaSetInformationPolicy(IntPtr handle, int infoClass, IntPtr buffer);
  [DllImport("advapi32.dll")] public static extern uint LsaFreeMemory(IntPtr buffer);
  [DllImport("advapi32.dll")] public static extern uint LsaClose(IntPtr handle);
  [DllImport("advapi32.dll")] public static extern int LsaNtStatusToWinError(uint status);
  [DllImport("advapi32.dll", CharSet = CharSet.Unicode)] public static extern int RegOpenKeyExW(UIntPtr hKey, string subKey, int options, int sam, out IntPtr result);
  [DllImport("advapi32.dll")] public static extern int RegFlushKey(IntPtr hKey);
  [DllImport("advapi32.dll")] public static extern int RegCloseKey(IntPtr hKey);
}
'@
$M = [Runtime.InteropServices.Marshal]

function St([uint32]$s) { '0x{0:X8} (Win32 {1})' -f $s, [SwoaLsa2]::LsaNtStatusToWinError($s) }
function UStr($u) { if ($u.Buffer -ne [IntPtr]::Zero -and $u.Length -gt 0) { "'" + $M::PtrToStringUni($u.Buffer, $u.Length / 2) + "'" } else { '(empty)' } }
function SidStr([IntPtr]$p) { if ($p -ne [IntPtr]::Zero) { (New-Object Security.Principal.SecurityIdentifier $p).Value } else { '(none)' } }

function Open-Policy {
  $attrs = New-Object SwoaLsa2+LSA_OBJECT_ATTRIBUTES
  $attrs.Length = $M::SizeOf($attrs)
  $h = [IntPtr]::Zero
  $s = [SwoaLsa2]::LsaOpenPolicy([IntPtr]::Zero, [ref]$attrs, 0x000F0FFF, [ref]$h)
  if ($s -ne 0) { Log ("LsaOpenPolicy failed {0}" -f (St $s)); exit 1 }
  return $h
}

function Show-All([string]$title) {
  Log "${title}:"
  $h = Open-Policy
  try {
    foreach ($c in 5, 3) {
      $buf = [IntPtr]::Zero
      $s = [SwoaLsa2]::LsaQueryInformationPolicy($h, $c, [ref]$buf)
      if ($s -ne 0) { Log ("  class {0}: query failed {1}" -f $c, (St $s)); continue }
      $i = $M::PtrToStructure($buf, [type][SwoaLsa2+DOMAIN_INFO])
      Log ("  {0}: Name={1} Sid={2}" -f $(if ($c -eq 5) { 'AccountDomain(5)' } else { 'PrimaryDomain(3)' }), (UStr $i.Name), (SidStr $i.Sid))
      [void][SwoaLsa2]::LsaFreeMemory($buf)
    }
    $buf = [IntPtr]::Zero
    $s = [SwoaLsa2]::LsaQueryInformationPolicy($h, 12, [ref]$buf)
    if ($s -ne 0) { Log ("  class 12: query failed {0}" -f (St $s)) } else {
      $i = $M::PtrToStructure($buf, [type][SwoaLsa2+DNS_DOMAIN_INFO])
      Log ("  DnsDomain(12): Name={0} DnsDomainName={1} DnsForestName={2} Guid={3} Sid={4}" -f (UStr $i.Name), (UStr $i.DnsDomainName), (UStr $i.DnsForestName), $i.DomainGuid, (SidStr $i.Sid))
      [void][SwoaLsa2]::LsaFreeMemory($buf)
    }
  }
  finally { [void][SwoaLsa2]::LsaClose($h) }
  foreach ($v in 'PolPrDmN', 'PolPrDmS', 'PolDnDDN', 'PolDnTrN', 'PolDnDmG') {
    $o = & reg.exe query "HKLM\SECURITY\Policy\$v" /ve 2>$null | Where-Object { $_ -match 'REG_' }
    Log ("  raw {0,-9}: {1}" -f $v, $(if ($o) { ($o -replace '^\s+', '') } else { '(not readable)' }))
  }
}

function Test-Primary {
  $h = Open-Policy
  try {
    $buf = [IntPtr]::Zero
    if ([SwoaLsa2]::LsaQueryInformationPolicy($h, 3, [ref]$buf) -ne 0) { return $false }
    $i = $M::PtrToStructure($buf, [type][SwoaLsa2+DOMAIN_INFO])
    $ok = ($i.Name.Buffer -ne [IntPtr]::Zero -and $i.Name.Length -gt 0)
    [void][SwoaLsa2]::LsaFreeMemory($buf)
    return $ok
  }
  finally { [void][SwoaLsa2]::LsaClose($h) }
}

function New-UStr([string]$s) {
  $u = New-Object SwoaLsa2+LSA_UNICODE_STRING
  if ($s) { $u.Length = [uint16]($s.Length * 2); $u.MaximumLength = [uint16](($s.Length + 1) * 2); $u.Buffer = $M::StringToHGlobalUni($s) }
  return $u
}

function Set-Info([int]$class, $struct) {
  $p = $M::AllocHGlobal($M::SizeOf($struct))
  try {
    $M::StructureToPtr($struct, $p, $false)
    $h = Open-Policy
    try { return [SwoaLsa2]::LsaSetInformationPolicy($h, $class, $p) } finally { [void][SwoaLsa2]::LsaClose($h) }
  }
  finally { $M::FreeHGlobal($p) }
}

function Flush-Security {
  $k = [IntPtr]::Zero
  $r = [SwoaLsa2]::RegOpenKeyExW([UIntPtr]([uint32]2147483650), 'SECURITY', 0, 0x20019, [ref]$k)
  if ($r -ne 0) { Log "  RegOpenKeyEx(HKLM\SECURITY) failed $r"; return }
  Log ("  RegFlushKey(HKLM\SECURITY) -> {0}" -f [SwoaLsa2]::RegFlushKey($k))
  [void][SwoaLsa2]::RegCloseKey($k)
}

Show-All 'Before'
if ($Fix) {
  $done = $false

  $dns = New-Object SwoaLsa2+DNS_DOMAIN_INFO
  $dns.Name = New-UStr $Workgroup
  $dns.DnsDomainName = New-UStr ''
  $dns.DnsForestName = New-UStr ''
  $dns.DomainGuid = [Guid]::Empty
  $dns.Sid = [IntPtr]::Zero
  Log ("1. Set PolicyDnsDomainInformation Name='{0}': {1}" -f $Workgroup, (St (Set-Info 12 $dns)))
  $M::FreeHGlobal($dns.Name.Buffer)
  $done = Test-Primary
  Log "   PrimaryDomain readable now: $done"

  if (-not $done) {
    $pri = New-Object SwoaLsa2+DOMAIN_INFO
    $pri.Name = New-UStr $Workgroup
    $pri.Sid = [IntPtr]::Zero
    Log ("2. Set PolicyPrimaryDomainInformation Name='{0}': {1}" -f $Workgroup, (St (Set-Info 3 $pri)))
    $M::FreeHGlobal($pri.Name.Buffer)
    $done = Test-Primary
    Log "   PrimaryDomain readable now: $done"
  }

  if (-not $done) {
    try {
      $cs = Get-WmiObject Win32_ComputerSystem
      $r = $cs.JoinDomainOrWorkgroup($Workgroup, $null, $null, $null, 0)
      Log ("3. JoinDomainOrWorkgroup('{0}') ReturnValue={1}" -f $Workgroup, $r.ReturnValue)
    }
    catch { Log ("3. JoinDomainOrWorkgroup failed: {0}" -f $_.Exception.Message) }
    $done = Test-Primary
    Log "   PrimaryDomain readable now: $done"
  }

  Flush-Security
  Show-All 'After'
}
Log "Saved: $Out"
