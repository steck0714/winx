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
                         'LitePerformanceCheck.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $kit $file))) { throw "Missing kit file: $file" }
    }
    foreach ($store in @('Root', 'TrustedPublisher')) {
        if (-not (Test-Path "Cert:\LocalMachine\$store\540075DA9D410808E322CBA1F0E3A70E0A76761C")) {
            throw 'The existing Stage 6 development certificate is not trusted. Install the matching certificate before this kit.'
        }
    }
    $reboot = $false
    foreach ($name in @('switchfan', 'tegragpuprobe')) {
        & "$env:SystemRoot\System32\pnputil.exe" /add-driver (Join-Path $kit "$name\$name.inf") /install
        if ($LASTEXITCODE -eq 3010) { $reboot = $true }
        elseif ($LASTEXITCODE -ne 0) { throw "PnPUtil failed for $name ($LASTEXITCODE)." }
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
