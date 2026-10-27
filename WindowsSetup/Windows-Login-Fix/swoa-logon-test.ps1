<#
  Run on the Switch, in the SYSTEM command prompt that Set-SwitchBootCmd.ps1 -Mode Enable opens:
    powershell -ExecutionPolicy Bypass -File C:\swoa-logon-test.ps1
  Asks for the Switch account's password once (not shown, not saved), then calls
  LogonUser with several logon types / providers / domain forms and writes the
  result codes to C:\login-test.txt. Changes nothing.
#>
param([string]$User = 'Switch')

$Out = 'C:\login-test.txt'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Out -Value $s -Encoding UTF8 }
Set-Content -Path $Out -Value "swoa-logon-test  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  COMPUTERNAME=$env:COMPUTERNAME" -Encoding UTF8

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SwoaLogon {
  [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  public static extern bool LogonUserW(string user, string domain, IntPtr password, int type, int provider, out IntPtr token);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
'@

Log ''
Log '### services'
foreach ($s in 'SamSs', 'KeyIso', 'LanmanWorkstation', 'LanmanServer', 'Netlogon', 'seclogon', 'ProfSvc', 'UserManager', 'gpsvc', 'NlaSvc', 'nsi', 'Dhcp', 'Dnscache') {
  $o = Get-Service -Name $s -ErrorAction SilentlyContinue
  if ($o) { Log ("  {0,-18} {1}" -f $s, $o.Status) } else { Log ("  {0,-18} (not found)" -f $s) }
}
Log ''
Log '### computer'
$cs = Get-WmiObject Win32_ComputerSystem
Log ("  Name={0} Domain={1} Workgroup={2} PartOfDomain={3} DomainRole={4}" -f $cs.Name, $cs.Domain, $cs.Workgroup, $cs.PartOfDomain, $cs.DomainRole)
Log ("  Date={0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))

$sec = Read-Host -AsSecureString "Password for $User"
$pw = [Runtime.InteropServices.Marshal]::SecureStringToGlobalAllocUnicode($sec)
try {
  Log ''
  Log "### LogonUser results for user '$User' (0 = OK, otherwise Win32 error)"
  $types = @{ 2 = 'INTERACTIVE'; 3 = 'NETWORK'; 4 = 'BATCH' }
  $provs = @{ 0 = 'DEFAULT(Negotiate)'; 2 = 'WINNT40(NTLM)'; 3 = 'WINNT50(Negotiate)' }
  foreach ($dom in @($env:COMPUTERNAME, '.', $null)) {
    foreach ($t in 2, 3, 4) {
      foreach ($p in 0, 2, 3) {
        $tok = [IntPtr]::Zero
        $ok = [SwoaLogon]::LogonUserW($User, $dom, $pw, $t, $p, [ref]$tok)
        $err = if ($ok) { 0 } else { [Runtime.InteropServices.Marshal]::GetLastWin32Error() }
        if ($ok) { [void][SwoaLogon]::CloseHandle($tok) }
        $msg = if ($err) { (New-Object ComponentModel.Win32Exception $err).Message } else { 'OK' }
        Log ("  domain={0,-16} type={1,-11} provider={2,-19} -> {3} {4}" -f $(if ($null -eq $dom) { '(null)' } else { $dom }), $types[$t], $provs[$p], $err, $msg)
      }
    }
  }
  $tok = [IntPtr]::Zero
  $ok = [SwoaLogon]::LogonUserW("$User@$env:COMPUTERNAME", $null, $pw, 2, 0, [ref]$tok)
  $err = if ($ok) { 0 } else { [Runtime.InteropServices.Marshal]::GetLastWin32Error() }
  if ($ok) { [void][SwoaLogon]::CloseHandle($tok) }
  Log ("  UPN {0}@{1} INTERACTIVE DEFAULT -> {2}" -f $User, $env:COMPUTERNAME, $err)
}
finally {
  [Runtime.InteropServices.Marshal]::ZeroFreeGlobalAllocUnicode($pw)
}
Log ''
Log "Saved: $Out"
