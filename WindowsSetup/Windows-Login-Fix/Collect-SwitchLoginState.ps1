<#
.SYNOPSIS
  Read-only snapshot of the Switch Windows SD's logon/setup state, taken on a PC.

.DESCRIPTION
  Copies (never modifies) from the SD's Windows partition:
    - SYSTEM / SOFTWARE / SAM hives (+ .LOG1/.LOG2), loaded as COPIES only
    - C:\unattend.xml and Windows\Panther\unattend.xml (if present)
    - Windows\Panther\setupact.log / setuperr.log, Panther\UnattendGC\*.log,
      Windows\System32\Sysprep\Panther\*.log, Windows\Setup\State\State.ini
  and writes REPORT.txt with:
    - SYSTEM\Setup (OOBEInProgress, SystemSetupInProgress, SetupType, CmdLine, ...)
    - computer name, Winlogon auto-logon values (passwords are redacted)
    - LogonUI last user, local user names in the SAM copy (if readable)
    - unattend.xml with passwords redacted, and the last lines of the setup logs

  Run from an elevated PowerShell:
    powershell -ExecutionPolicy Bypass -File Collect-SwitchLoginState.ps1 -WindowsDrive F:
#>
param(
  [Parameter(Mandatory = $true)][string]$WindowsDrive,
  [string]$OutRoot = "D:\Codex\SwitchWOA-Bootstrap\outputs\Windows-Login-Fix"
)

$ErrorActionPreference = 'Stop'
$WindowsDrive = $WindowsDrive.TrimEnd('\', ':') + ':'
$Win = Join-Path $WindowsDrive 'Windows'
if (-not (Test-Path (Join-Path $Win 'System32\config\SYSTEM'))) {
  throw "No Windows\System32\config\SYSTEM on $WindowsDrive. Pass the Switch SD's Windows partition."
}
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw 'Run this from an elevated (Administrator) PowerShell.' }

$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$Out = Join-Path $OutRoot "LoginState-$Stamp"
$Raw = Join-Path $Out 'raw'
New-Item -ItemType Directory -Force -Path $Raw | Out-Null
$Report = Join-Path $Out 'REPORT.txt'
function Add-Report([string]$Text) { Add-Content -Path $Report -Value $Text -Encoding UTF8 }
function Redact([string]$Text) {
  $Text = $Text -replace '(?is)(<Value>).*?(</Value>)', '$1(redacted)$2'
  $Text = $Text -replace '(?im)^(\s*(DefaultPassword|AutoLogonPassword)\s+REG_\w+\s+).*$', '$1(redacted)'
  return $Text
}
function Copy-IfExists([string]$Src, [string]$Dst, [string[]]$Files) {
  New-Item -ItemType Directory -Force -Path $Dst | Out-Null
  foreach ($f in $Files) {
    if (Test-Path -LiteralPath (Join-Path $Src $f)) { robocopy $Src $Dst $f /B /R:0 /W:0 /NJH /NJS /NP | Out-Null }
  }
}

Add-Report "Switch Windows logon/setup state  collected=$Stamp  source=$WindowsDrive"
Add-Report ('=' * 78)

Copy-IfExists "$Win\System32\config" "$Raw\config" @('SYSTEM', 'SYSTEM.LOG1', 'SYSTEM.LOG2', 'SOFTWARE', 'SOFTWARE.LOG1', 'SOFTWARE.LOG2', 'SAM', 'SAM.LOG1', 'SAM.LOG2')
Copy-IfExists "$WindowsDrive\" "$Raw\root" @('unattend.xml', 'Autounattend.xml')
Copy-IfExists "$Win\Panther" "$Raw\Panther" @('unattend.xml', 'setupact.log', 'setuperr.log')
Copy-IfExists "$Win\Panther\UnattendGC" "$Raw\Panther-UnattendGC" @('setupact.log', 'setuperr.log', 'diagerr.xml', 'diagwrn.xml')
Copy-IfExists "$Win\System32\Sysprep\Panther" "$Raw\Sysprep-Panther" @('setupact.log', 'setuperr.log')
Copy-IfExists "$Win\Setup\State" "$Raw\Setup-State" @('State.ini')
Add-Report 'Copied files:'
Get-ChildItem -Recurse -File $Raw | ForEach-Object { Add-Report ("  {0,10}  {1:yyyy-MM-dd HH:mm:ss}  {2}" -f $_.Length, $_.LastWriteTime, $_.FullName.Substring($Raw.Length + 1)) }

function Dump-Key([string]$Key, [string]$Title, [switch]$NoRecurse) {
  Add-Report ''
  Add-Report "### $Title  ($Key)"
  $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  if ($NoRecurse) { $o = & reg.exe query $Key 2>$null } else { $o = & reg.exe query $Key /s 2>$null }
  $code = $LASTEXITCODE; $ErrorActionPreference = $prev
  if ($code -ne 0) { Add-Report '  (not present or access denied)' } else { $o | ForEach-Object { Add-Report ('  ' + (Redact $_)) } }
}

$loaded = @()
try {
  & reg.exe load HKLM\SWOA_SYSTEM "$Raw\config\SYSTEM" | Out-Null; $loaded += 'HKLM\SWOA_SYSTEM'
  & reg.exe load HKLM\SWOA_SOFTWARE "$Raw\config\SOFTWARE" | Out-Null; $loaded += 'HKLM\SWOA_SOFTWARE'
  $sel = (Get-ItemProperty -Path 'HKLM:\SWOA_SYSTEM\Select').Current
  $cs = 'HKLM\SWOA_SYSTEM\ControlSet{0:D3}' -f $sel
  Dump-Key 'HKLM\SWOA_SYSTEM\Setup' 'SYSTEM\Setup (setup/OOBE state)' -NoRecurse
  Dump-Key "$cs\Control\ComputerName\ComputerName" 'Computer name'
  Dump-Key "$cs\Services\Tcpip\Parameters" 'Tcpip hostname/domain' -NoRecurse
  Dump-Key 'HKLM\SWOA_SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon' 'Winlogon (passwords redacted)' -NoRecurse
  Dump-Key 'HKLM\SWOA_SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\LogonUI' 'LogonUI (last user)' -NoRecurse
  Dump-Key 'HKLM\SWOA_SOFTWARE\Microsoft\Windows\CurrentVersion\Setup\State' 'Setup\State (ImageState)' -NoRecurse
  Dump-Key 'HKLM\SWOA_SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList' 'ProfileList (accounts that have logged on)'
  if (Test-Path "$Raw\config\SAM") {
    & reg.exe load HKLM\SWOA_SAM "$Raw\config\SAM" | Out-Null; $loaded += 'HKLM\SWOA_SAM'
    Dump-Key 'HKLM\SWOA_SAM\SAM\Domains\Account\Users\Names' 'Local user names (SAM copy; may be access denied)'
  }
}
finally {
  [GC]::Collect(); [GC]::WaitForPendingFinalizers()
  foreach ($k in $loaded) { & reg.exe unload $k | Out-Null }
}

foreach ($u in @("$Raw\root\unattend.xml", "$Raw\root\Autounattend.xml", "$Raw\Panther\unattend.xml")) {
  if (Test-Path $u) {
    Add-Report ''
    Add-Report "### $($u.Substring($Raw.Length + 1)) (passwords redacted)"
    Add-Report (Redact (Get-Content -LiteralPath $u -Raw))
  }
}
if (Test-Path "$Raw\Setup-State\State.ini") {
  Add-Report ''
  Add-Report '### Windows\Setup\State\State.ini'
  Get-Content "$Raw\Setup-State\State.ini" | ForEach-Object { Add-Report "  $_" }
}
foreach ($log in @("$Raw\Panther\setuperr.log", "$Raw\Panther-UnattendGC\setuperr.log", "$Raw\Panther-UnattendGC\setupact.log", "$Raw\Panther\setupact.log")) {
  if (Test-Path $log) {
    Add-Report ''
    Add-Report "### last 80 lines of $($log.Substring($Raw.Length + 1))"
    Get-Content -LiteralPath $log -Tail 80 | ForEach-Object { Add-Report ('  ' + (Redact $_)) }
  }
}

Write-Host "Report: $Report"
Write-Host "Raw copies: $Raw"
