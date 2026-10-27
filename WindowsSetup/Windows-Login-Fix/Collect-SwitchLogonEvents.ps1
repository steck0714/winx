<#
.SYNOPSIS
  Read-only: copy the Switch Windows SD's event logs and report why logon fails.

.DESCRIPTION
  Copies (never modifies) Security/System/Application .evtx and the SYSTEM hive
  from the SD, then writes REPORT.txt with:
    - Security 4625 (logon failure: Status/SubStatus), 4624, 4776
    - System/Application errors and warnings (service start failures etc.)
    - SYSTEM\Setup values and the start type / dependencies of logon-related services

  Run from an elevated PowerShell:
    powershell -ExecutionPolicy Bypass -File Collect-SwitchLogonEvents.ps1 -WindowsDrive F:
#>
param(
  [Parameter(Mandatory = $true)][string]$WindowsDrive,
  [string]$OutRoot = "D:\Codex\SwitchWOA-Bootstrap\outputs\Windows-Login-Fix"
)

$ErrorActionPreference = 'Stop'
$WindowsDrive = $WindowsDrive.TrimEnd('\', ':') + ':'
$Win = Join-Path $WindowsDrive 'Windows'
if (-not (Test-Path (Join-Path $Win 'System32\config\SYSTEM'))) { throw "No Windows\System32\config\SYSTEM on $WindowsDrive." }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw 'Run this from an elevated (Administrator) PowerShell.' }

$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$Out = Join-Path $OutRoot "LogonEvents-$Stamp"
$Raw = Join-Path $Out 'raw'
New-Item -ItemType Directory -Force -Path $Raw | Out-Null
$Report = Join-Path $Out 'REPORT.txt'
function Add-Report([string]$Text) { Add-Content -Path $Report -Value $Text -Encoding UTF8 }

robocopy "$Win\System32\winevt\Logs" $Raw Security.evtx System.evtx Application.evtx /B /R:0 /W:0 /NJH /NJS /NP | Out-Null
robocopy "$Win\System32\config" $Raw SYSTEM SYSTEM.LOG1 SYSTEM.LOG2 /B /R:0 /W:0 /NJH /NJS /NP | Out-Null
Add-Report "Switch Windows logon events  collected=$Stamp  source=$WindowsDrive"
Add-Report ('=' * 78)
Get-ChildItem -File $Raw | ForEach-Object { Add-Report ("  {0,10}  {1:yyyy-MM-dd HH:mm:ss}  {2}" -f $_.Length, $_.LastWriteTime, $_.Name) }

function Short([string]$s, [int]$n = 400) { $s = ($s -replace '\s+', ' ').Trim(); if ($s.Length -gt $n) { $s.Substring(0, $n) + '...' } else { $s } }
function Get-Events([string]$Log, [string]$XPath, [int]$Max) {
  $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $e = Get-WinEvent -Path (Join-Path $Raw "$Log.evtx") -FilterXPath $XPath -MaxEvents $Max -ErrorAction SilentlyContinue
  $ErrorActionPreference = $prev
  return $e
}

Add-Report ''
Add-Report '### Security: 4625 (failure) / 4624 (success) / 4776 (NTLM validation), newest first'
foreach ($ev in (Get-Events 'Security' '*[System[(EventID=4625 or EventID=4624 or EventID=4776)]]' 40)) {
  $x = [xml]$ev.ToXml(); $h = @{}
  foreach ($d in $x.Event.EventData.Data) { $h[$d.Name] = $d.'#text' }
  if ($ev.Id -eq 4776) {
    Add-Report ("  {0:yyyy-MM-dd HH:mm:ss} 4776 account={1} workstation={2} status={3}" -f $ev.TimeCreated, $h.TargetUserName, $h.Workstation, $h.Status)
  } elseif ($h.LogonType -in @('2', '7', '10', '11')) {
    Add-Report ("  {0:yyyy-MM-dd HH:mm:ss} {1} user={2}\{3} type={4} status={5} sub={6} reason={7} pkg={8}/{9} proc={10}" -f $ev.TimeCreated, $ev.Id, $h.TargetDomainName, $h.TargetUserName, $h.LogonType, $h.Status, $h.SubStatus, $h.FailureReason, $h.LogonProcessName, $h.AuthenticationPackageName, $h.ProcessName)
  }
}

foreach ($log in 'System', 'Application') {
  Add-Report ''
  Add-Report "### ${log}: errors and warnings (newest 80)"
  foreach ($ev in (Get-Events $log '*[System[(Level=1 or Level=2 or Level=3)]]' 80)) {
    $msg = try { $ev.FormatDescription() } catch { $null }
    if (-not $msg) { $msg = ($ev.Properties | ForEach-Object { $_.Value }) -join ' | ' }
    Add-Report ("  {0:yyyy-MM-dd HH:mm:ss} {1} L{2} {3}: {4}" -f $ev.TimeCreated, $ev.Id, $ev.Level, $ev.ProviderName, (Short $msg))
  }
}

Add-Report ''
Add-Report '### System: Winlogon / LSA / Netlogon / User Profile events (newest 40)'
foreach ($ev in (Get-Events 'System' "*[System[Provider[@Name='Microsoft-Windows-Winlogon' or @Name='LsaSrv' or @Name='NETLOGON' or @Name='Microsoft-Windows-Security-Kerberos' or @Name='Microsoft-Windows-User Profiles Service']]]" 40)) {
  $msg = try { $ev.FormatDescription() } catch { $null }
  Add-Report ("  {0:yyyy-MM-dd HH:mm:ss} {1} L{2} {3}: {4}" -f $ev.TimeCreated, $ev.Id, $ev.Level, $ev.ProviderName, (Short $msg))
}

try {
  & reg.exe load HKLM\SWOA_EVT_SYSTEM "$Raw\SYSTEM" | Out-Null
  $p = Get-ItemProperty 'HKLM:\SWOA_EVT_SYSTEM\Setup'
  Add-Report ''
  Add-Report ("### SYSTEM\Setup: SetupType={0} CmdLine='{1}' SystemSetupInProgress={2} OOBEInProgress={3}" -f $p.SetupType, $p.CmdLine, $p.SystemSetupInProgress, $p.OOBEInProgress)
  $sel = (Get-ItemProperty 'HKLM:\SWOA_EVT_SYSTEM\Select').Current
  $svc = 'HKLM:\SWOA_EVT_SYSTEM\ControlSet{0:D3}\Services' -f $sel
  Add-Report ''
  Add-Report '### Logon-related services (Start: 0 boot,1 system,2 auto,3 manual,4 disabled; missing = not installed)'
  foreach ($n in 'SamSs', 'KeyIso', 'LanmanWorkstation', 'LanmanServer', 'Netlogon', 'Netman', 'NlaSvc', 'nsi', 'Dhcp', 'Dnscache', 'Tcpip', 'NetBT', 'Bowser', 'MRxSmb', 'MRxSmb20', 'mrxsmb10', 'rdbss', 'Afd', 'ProfSvc', 'gpsvc', 'Schedule', 'CryptSvc', 'VaultSvc', 'seclogon', 'UserManager', 'LSM') {
    $k = Join-Path $svc $n
    if (Test-Path $k) {
      $s = Get-ItemProperty $k
      Add-Report ("  {0,-18} Start={1} Type={2} DependOnService={3} ImagePath={4}" -f $n, $s.Start, $s.Type, ($s.DependOnService -join ','), $s.ImagePath)
    } else { Add-Report ("  {0,-18} (missing)" -f $n) }
  }
  $lsa = 'HKLM:\SWOA_EVT_SYSTEM\ControlSet{0:D3}\Control\Lsa' -f $sel
  Add-Report ''
  Add-Report '### Control\Lsa (values)'
  (Get-ItemProperty $lsa).PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object { Add-Report ("  {0} = {1}" -f $_.Name, ($_.Value -join ',')) }
}
finally {
  [GC]::Collect(); [GC]::WaitForPendingFinalizers()
  & reg.exe unload HKLM\SWOA_EVT_SYSTEM | Out-Null
}

Write-Host "Report: $Report"
