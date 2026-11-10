<#
.SYNOPSIS
  Read-only USB (XUSB) register snapshot on the Switch's Windows (Administrator).
  Loads swoadiag.sys once (sc create/start), prints the registers it read,
  then stops and deletes the service. Nothing is written to the hardware.

  -Label <text> : a name for this snapshot (e.g. working, unplugged, replugged)

  Log: C:\SwitchWOA-UsbDiag.txt
#>
param([string]$Label = 'snapshot')

$ErrorActionPreference = 'Continue'
# Windows PowerShell on Windows 10 ARM64 is x86: reach System32 via Sysnative.
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'drivers'))) { $Sys = Join-Path $env:windir 'System32' }
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-UsbDiag.txt'
$Key = 'HKLM:\SYSTEM\CurrentControlSet\Services\SwoaDiag\Parameters'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) prompt.'; exit 1 }

function Portsc([uint32]$v) {
  $pls = ($v -shr 5) -band 0xF
  $plsName = @{ 0 = 'U0'; 2 = 'U2'; 3 = 'U3'; 4 = 'Disabled'; 5 = 'RxDetect'; 6 = 'Inactive'; 7 = 'Polling'; 8 = 'Recovery'; 9 = 'HotReset'; 15 = 'Resume' }[[int]$pls]
  $speed = @{ 0 = '-'; 1 = 'FS'; 2 = 'LS'; 3 = 'HS'; 4 = 'SS' }[[int](($v -shr 10) -band 0xF)]
  $flags = @()
  foreach ($b in @(@(0, 'CCS'), @(1, 'PED'), @(3, 'OCA'), @(4, 'PR'), @(9, 'PP'), @(17, 'CSC'), @(18, 'PEC'), @(19, 'WRC'), @(20, 'OCC'), @(21, 'PRC'), @(22, 'PLC'), @(23, 'CEC'))) {
    if ($v -band (1 -shl $b[0])) { $flags += $b[1] }
  }
  '{0:X8} PLS={1}({2}) speed={3} {4}' -f $v, $pls, $plsName, $speed, ($flags -join ' ')
}

Log ''
Log "=== UsbDiag [$Label]  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
Copy-Item (Join-Path $Here 'swoadiag.sys') (Join-Path $Sys 'drivers\swoadiag.sys') -Force
& sc.exe stop SwoaDiag 2>&1 | Out-Null
& sc.exe delete SwoaDiag 2>&1 | Out-Null
Remove-Item -Path $Key -Recurse -ErrorAction SilentlyContinue
$create = (& sc.exe create SwoaDiag type= kernel start= demand binPath= 'System32\drivers\swoadiag.sys' 2>&1) -join ' '
$start = (& sc.exe start SwoaDiag 2>&1) -join ' ' -replace '\s+', ' '
Start-Sleep -Milliseconds 500
$p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
& sc.exe stop SwoaDiag 2>&1 | Out-Null
& sc.exe delete SwoaDiag 2>&1 | Out-Null
if (-not $p) { Log "FAIL: no snapshot. sc create: $create | sc start: $start"; exit 1 }

Log ('xHCI: USBCMD={0:X8} USBSTS={1:X8} IMAN0={2:X8} MaxPorts={3}' -f [uint32]$p.XhciUSBCMD, [uint32]$p.XhciUSBSTS, [uint32]$p.XhciIMAN0, $p.XhciMaxPorts)
foreach ($prop in ($p.PSObject.Properties | Where-Object { $_.Name -like 'XhciPORTSC*' } | Sort-Object Name)) {
  $n = [int]$prop.Name.Substring(10)
  Log ('  port {0,2}: {1}' -f $n, (Portsc ([uint32]$prop.Value)))
}
Log ('mailbox: CMD={0:X8} DATA_IN={1:X8} DATA_OUT={2:X8} OWNER={3:X8} SMI_INTR={4:X8}  (DATA_OUT cmd={5})' -f `
  [uint32]$p.MboxCmd, [uint32]$p.MboxDataIn, [uint32]$p.MboxDataOut, [uint32]$p.MboxOwner, [uint32]$p.SmiIntr, (([uint32]$p.MboxDataOut -shr 24) -band 0xFF))
Log ('FPCI command={0:X8} cfg+0x200={1:X8}  IPFS config={2:X8} intr-mask={3:X8} clkgate={4:X8}' -f `
  [uint32]$p.FpciCommand, [uint32]$p.AruControl, [uint32]$p.IpfsConfig, [uint32]$p.IpfsIntrMask, [uint32]$p.IpfsClkGateHyst)
Log ('PADCTL: mux={0:X8} cap={1:X8} elpg0={2:X8} elpg1={3:X8} bc0={4:X8} bc1={5:X8} otg0={6:X8} otg1={7:X8} bias={8:X8} vbus_id={9:X8}' -f `
  [uint32]$p.PadMux, [uint32]$p.PortCap, [uint32]$p.ElpgProgram0, [uint32]$p.ElpgProgram1, [uint32]$p.BattChrgOtgPad0Ctl0,
  [uint32]$p.BattChrgOtgPad0Ctl1, [uint32]$p.OtgPad0Ctl0, [uint32]$p.OtgPad0Ctl1, [uint32]$p.BiasPadCtl0, [uint32]$p.Usb2VbusId)

$b = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\switchbattery\Parameters' -ErrorAction SilentlyContinue
if ($b) {
  Log ('USB-C (switchbattery): state={0} changes={1} S1={2:X4} S2={3:X4} BQ REG01={4:X2} refused={5}' -f `
    $b.UsbcState, $b.UsbcChanges, [uint32]$b.UsbcStatus1, [uint32]$b.UsbcStatus2, [uint32]$b.UsbcBqReg01, $b.UsbcVbusRefused)
}
$usb = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'USB\*' })
foreach ($d in $usb) { Log ('  pnp: {0}  "{1}" status={2} problem={3}' -f $d.PNPDeviceID, $d.Name, $d.Status, $d.ConfigManagerErrorCode) }
