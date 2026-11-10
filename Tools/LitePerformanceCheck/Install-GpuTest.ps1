# Install the signed vulkan-test probe and the matching fan PMIC bridge.
# Run from the kit directory on the existing ARM64 Windows installation.
$ErrorActionPreference = 'Stop'
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process -FilePath "$PSHOME\powershell.exe" -Verb RunAs -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $PSCommandPath))
    exit
}
try {
    $kit = $PSScriptRoot
    foreach ($file in @('switchfan\switchfan.inf', 'switchfan\switchfan.sys', 'switchfan\switchfan.cat',
                         'tegragpuprobe\tegragpuprobe.inf', 'tegragpuprobe\tegragpuprobe.sys', 'tegragpuprobe\tegragpuprobe.cat',
                         'LitePerformanceCheck.exe', 'GpuTest-CodeSigning.cer')) {
        if (-not (Test-Path -LiteralPath (Join-Path $kit $file))) { throw "Missing kit file: $file" }
    }
    if (-not (Test-Path 'Cert:\LocalMachine\Root\540075DA9D410808E322CBA1F0E3A70E0A76761C')) {
        throw 'The existing Stage 6 development root is not trusted. Install the matching root before this kit.'
    }
    # The trusted CA issues a separate CA:FALSE code-signing leaf. Using the
    # CA itself as the Authenticode signer fails TRUST_E_BASIC_CONSTRAINTS.
    $certificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2 (Join-Path $kit 'GpuTest-CodeSigning.cer')
    if ($certificate.Thumbprint -ne '7A86AAC1EE2F23E8DFAA604670F15430F3A321A8') { throw 'Unexpected GPU signing certificate.' }
    $publisher = New-Object Security.Cryptography.X509Certificates.X509Store ('TrustedPublisher', 'LocalMachine')
    try {
        $publisher.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $publisher.Add($certificate)
    } finally { $publisher.Close() }
    foreach ($name in @('switchfan', 'tegragpuprobe')) {
        foreach ($ext in @('sys', 'cat')) {
            $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $kit "$name\$name.$ext")
            if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) {
                throw "Invalid signature on $name.$ext ($($signature.Status))."
            }
        }
    }
    # PnPUtil can stage a package successfully yet return 259 because an
    # existing device's driver has equal or higher rank. This kit requires
    # its exact SwitchFan PMIC bridge, so bind it explicitly in that case.
    # UpdateDriverForPlugAndPlayDevices fails if the hardware is absent;
    # do not treat 259 alone as proof that the requested driver is active.
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class LiteGpuDriverInstall {
    [DllImport("newdev.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool UpdateDriverForPlugAndPlayDevicesW(
        IntPtr parent, string hardwareId, string infPath, uint flags,
        [MarshalAs(UnmanagedType.Bool)] out bool reboot);
}
'@
    $reboot = $false
    foreach ($driver in @(
        @{ Name = 'switchfan'; HardwareId = 'ACPI\SWOA0402' },
        @{ Name = 'tegragpuprobe'; HardwareId = 'ACPI\SWOA0F01' }
    )) {
        $name = $driver.Name
        $inf = Join-Path $kit "$name\$name.inf"
        & "$env:SystemRoot\System32\pnputil.exe" /add-driver $inf /install
        $result = $LASTEXITCODE
        if ($result -eq 3010) { $reboot = $true }
        elseif ($result -eq 259) {
            Write-Host "Selecting the packaged $name driver for $($driver.HardwareId)."
            $needsRestart = $false
            # INSTALLFLAG_FORCE = 1; only these two explicit hardware IDs.
            if (-not [LiteGpuDriverInstall]::UpdateDriverForPlugAndPlayDevicesW(
                [IntPtr]::Zero, $driver.HardwareId, $inf, 1, [ref]$needsRestart)) {
                $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
                throw "Driver binding failed for $name (Win32 $errorCode). Check that the matching GPU-test firmware is installed."
            }
            # Even if PnP can restart the device live, start the next GPU test
            # with a fresh boot of both matching drivers.
            $reboot = $true
        }
        elseif ($result -ne 0) { throw "PnPUtil failed for $name ($result)." }
    }
    $desktop = [Environment]::GetFolderPath('CommonDesktopDirectory')
    Copy-Item -LiteralPath (Join-Path $kit 'LitePerformanceCheck.exe') -Destination (Join-Path $desktop 'LitePerformanceCheck.exe') -Force
    Write-Host 'GPU test kit installed. Open the existing LitePerformanceCheck desktop tool.'
    Write-Host 'Use 729.6 MHz first. The 1228.8 MHz profile is optional and unverified on hardware.'
    if ($reboot) { Write-Host 'Restart Windows before testing so both new drivers are active.' }
    Read-Host 'Press Enter to close'
} catch {
    Write-Host $_ -ForegroundColor Red
    Read-Host 'Installation stopped. Press Enter to close'
    exit 1
}
