<#
.SYNOPSIS
  Stage 0 on the Switch's Windows (run as Administrator, Windows PowerShell 5.1).

  -Mode Prepare : trust the SwitchWOA test certificate and turn on test signing
                  (bcdedit). Reboot afterwards.
  -Mode Hello   : load the SwoaHello test driver once (sc create/start), check
                  that it wrote its registry value, stop and remove it again.
  -Mode Status  : show test-signing / certificate / SwoaHello state only.

  Log: C:\SwitchWOA-Stage0.txt
#>
param([Parameter(Mandatory = $true)][ValidateSet('Prepare', 'Hello', 'Status')][string]$Mode)

$ErrorActionPreference = 'Continue'
# Windows PowerShell 5.1 on Windows 10 ARM64 is always x86 (there is no ARM64
# .NET Framework), so System32 is redirected to SysWOW64, which has no
# bcdedit.exe/pnputil.exe and its own drivers\ folder. Reach the real
# System32 through the Sysnative alias.
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'bcdedit.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Bcdedit = Join-Path $Sys 'bcdedit.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-Stage0.txt'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) PowerShell.'; exit 1 }
Log ''
Log "=== Stage0 $Mode  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  process=$env:PROCESSOR_ARCHITECTURE 64bit=$([Environment]::Is64BitProcess) ps=$((Get-Process -Id $PID).Path)"

$Cer = Join-Path $Here 'SwitchWOA-Test.cer'
$Thumb = (New-Object Security.Cryptography.X509Certificates.X509Certificate2 $Cer).Thumbprint

function Show-Status {
  $bcd = & $Bcdedit /enum '{current}' 2>&1
  Log ('bcdedit {current}: ' + (($bcd | Select-String -Pattern 'testsigning|nointegritychecks|path|description') -join ' ; '))
  foreach ($store in 'Root', 'TrustedPublisher') {
    $has = Test-Path "Cert:\LocalMachine\$store\$Thumb"
    Log "certificate in LocalMachine\$store : $has"
  }
  $sb = try { Confirm-SecureBootUEFI } catch { "n/a ($($_.Exception.Message))" }
  Log "SecureBoot: $sb  (native tools from $Sys)"
  Log ('sc query SwoaHello: ' + ((& sc.exe query SwoaHello 2>&1) -join ' ' -replace '\s+', ' '))
}

switch ($Mode) {
  'Prepare' {
    Log "certutil -addstore Root/TrustedPublisher $Cer"
    & certutil.exe -f -addstore Root $Cer | Out-Null; Log "  Root exit=$LASTEXITCODE"
    & certutil.exe -f -addstore TrustedPublisher $Cer | Out-Null; Log "  TrustedPublisher exit=$LASTEXITCODE"
    $out = & $Bcdedit /set '{current}' testsigning on 2>&1
    Log "bcdedit /set {current} testsigning on : $out (exit=$LASTEXITCODE)"
    Show-Status
    Log 'Next: restart Windows (Start > Power > Restart, or re-inject the payload), then run -Mode Hello.'
  }
  'Hello' {
    Show-Status
    $dst = Join-Path $Sys 'drivers\SwoaHello.sys'
    Copy-Item (Join-Path $Here 'SwoaHello.sys') $dst -Force
    $sig = Get-AuthenticodeSignature $dst
    Log "signature: $($sig.Status) $($sig.SignerCertificate.Subject)"
    & sc.exe delete SwoaHello 2>&1 | Out-Null
    Log ('sc create: ' + ((& sc.exe create SwoaHello type= kernel start= demand binPath= 'System32\drivers\SwoaHello.sys' 2>&1) -join ' '))
    Log ('sc start : ' + ((& sc.exe start SwoaHello 2>&1) -join ' ' -replace '\s+', ' '))
    Start-Sleep -Seconds 1
    $p = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\SwoaHello\Parameters' -ErrorAction SilentlyContinue
    if ($p -and $p.Loaded -eq 1) { Log "RESULT: PASS  driver loaded, Loaded=$($p.Loaded) KmdfVersion=$($p.KmdfVersion)" }
    else { Log 'RESULT: FAIL  driver did not write Parameters\Loaded (see sc start above)' }
    Log ('sc stop  : ' + ((& sc.exe stop SwoaHello 2>&1) -join ' ' -replace '\s+', ' '))
    Log ('sc delete: ' + ((& sc.exe delete SwoaHello 2>&1) -join ' '))
    Remove-Item $dst -Force -ErrorAction SilentlyContinue
  }
  'Status' { Show-Status }
}
Log "Log: $Log"
