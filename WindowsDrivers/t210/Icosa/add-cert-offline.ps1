# add-cert-offline.ps1  (ADMIN PowerShell on the PC)
# Puts a certificate into the Trusted Root and Trusted Publishers stores of an OFFLINE Windows,
# the same way SwitchPatcher does. The offline SOFTWARE hive must already be loaded, e.g.
#   reg load HKLM\SWOA2 E:\Windows\System32\config\SOFTWARE
# usage:
#   powershell -ExecutionPolicy Bypass -File .\add-cert-offline.ps1 -Cer C:\path\SwitchWOA-Test.cer -Hive SWOA2
param(
    [Parameter(Mandatory = $true)][string]$Cer,
    [string]$Hive = 'SWOA2'
)
$ErrorActionPreference = 'Stop'
$root = "HKLM:\$Hive"
if (-not (Test-Path $root)) { throw "Hive $root is not loaded. Run: reg load HKLM\$Hive E:\Windows\System32\config\SOFTWARE" }
if (-not (Test-Path "$root\Microsoft")) { throw "$root does not look like a SOFTWARE hive (no Microsoft key)." }

$cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 ((Resolve-Path $Cer).Path)
$thumb = $cert.Thumbprint
Write-Host "Certificate: $($cert.Subject)  thumbprint $thumb"

# Let Windows write its own Blob format into a temporary per-user store, then read it back.
$tmp = 'SwitchPatcherTemp'
$store = New-Object System.Security.Cryptography.X509Certificates.X509Store ($tmp, 'CurrentUser')
$store.Open('ReadWrite')
$store.Add($cert)
$store.Close()
try {
    $src = "HKCU:\Software\Microsoft\SystemCertificates\$tmp\Certificates\$thumb"
    $blob = (Get-ItemProperty -Path $src -Name Blob).Blob
    if (-not $blob) { throw 'certificate blob not found' }
    foreach ($name in 'ROOT', 'TrustedPublisher') {
        $key = "$root\Microsoft\SystemCertificates\$name\Certificates\$thumb"
        New-Item -Path $key -Force | Out-Null
        New-ItemProperty -Path $key -Name Blob -PropertyType Binary -Value $blob -Force | Out-Null
        Write-Host "added to $name"
    }
}
finally {
    Remove-Item -Path "HKCU:\Software\Microsoft\SystemCertificates\$tmp" -Recurse -Force -ErrorAction SilentlyContinue
}
[GC]::Collect()
[GC]::WaitForPendingFinalizers()
Write-Host 'Done. Now run: reg unload HKLM\' + $Hive
