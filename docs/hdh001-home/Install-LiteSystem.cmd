@echo off
setlocal EnableExtensions
rem Add shared system drivers to an existing Stage 6 Home installation.
rem Place this script beside the signed driver directories.
rem Optional switchbt requires Firmware\BCM4356A3.hcd.
rem Optional switchwifi requires WifiPrepare.efi and the target board's A3 files in Firmware.
rem Optional tegraxusb also needs UsbPrepare.efi and the private B01 Falcon image.
set "drivers=switchfan switchpwrbtn switchbattery switchrtc switchaudio"
if exist "%~dp0switchsio\switchsio.inf" set "drivers=%drivers% switchsio"
if exist "%~dp0switchbt\switchbt.inf" set "drivers=%drivers% switchbt"
if exist "%~dp0switchwifi\switchwifi.inf" set "drivers=%drivers% switchwifi"
set "component="
set "usbOnly="
set "usb="
if /i "%~1"=="online" set "component=%~2"
if /i "%~1"=="offline" set "component=%~3"
if /i "%component%"=="wifi" (
 if not exist "%~dp0switchwifi\switchwifi.inf" exit /b 2
 set "drivers=switchwifi"
) else if /i "%component%"=="usb-input" (
 if not exist "%~dp0switchsio\switchsio.inf" exit /b 2
 set "usb=1"
 set "drivers=switchbattery tegraxusb switchsio"
) else if /i "%component%"=="usb" (
 set "usb=1"
 set "drivers=switchbattery tegraxusb"
) else if defined component (
 goto usage
) else if exist "%~dp0tegraxusb\tegraxusb.inf" (
 set "usb=1"
 set "drivers=%drivers% tegraxusb"
)
if defined usb if defined component set "usbOnly=1"
if defined usb (
 for %%F in (switchbattery\switchbattery.inf tegraxusb\tegraxusb.inf UsbPrepare.efi Firmware\tegra210b01_xusb_firmware) do if not exist "%~dp0%%F" (
  echo Missing USB host kit file: %%F
  exit /b 2
 )
)
if /i "%~1"=="online" goto online
if /i "%~1"=="offline" goto offline
:usage
echo Install-LiteSystem.cmd online [wifi ^| usb ^| usb-input]
echo Install-LiteSystem.cmd offline W:\ [wifi ^| usb ^| usb-input]
echo W: must be the drive assigned to the offline WINARM Windows installation.
exit /b 2

:online
for %%S in (ROOT TrustedPublisher) do (
 reg query "HKLM\SOFTWARE\Microsoft\SystemCertificates\%%S\Certificates\99C510FC6160C2294603EAD48DA8831210BBC56D" >nul 2>&1
 if errorlevel 1 goto trust
)
if not defined usbOnly (
 call :wifi "%SystemRoot%" "%SystemDrive%"
 if errorlevel 1 exit /b 1
)
if not defined component (
 call :bluetooth "%SystemRoot%"
 if errorlevel 1 exit /b 1
)
set "reboot=0"
for %%N in (%drivers%) do (
 pnputil /add-driver "%~dp0%%N\%%N.inf" /install
 if errorlevel 3011 exit /b 1
 if errorlevel 3010 (
  set "reboot=1"
 ) else if errorlevel 1 exit /b 1
)
if defined usb (
 call :publish-usb "%SystemDrive%"
 if errorlevel 1 exit /b 1
)
if defined component goto installed
pnputil /scan-devices
if errorlevel 1 exit /b 1
call :tools "%PUBLIC%\Desktop"
if errorlevel 1 exit /b 1
:installed
if "%reboot%"=="1" exit /b 3010
exit /b 0

:offline
if "%~2"=="" exit /b 2
set "vol=%~d2"
if not defined vol exit /b 2
if /i not "%~f2"=="%vol%\" exit /b 2
if /i "%vol%"=="%SystemDrive%" exit /b 2
if not exist "%vol%\Windows\System32\config\SOFTWARE" exit /b 2
set "hive=LiteSystemQuery%RANDOM%%RANDOM%"
reg load "HKLM\%hive%" "%vol%\Windows\System32\config\SOFTWARE" >nul || exit /b 1
set "trusted=1"
for %%S in (ROOT TrustedPublisher) do (
 reg query "HKLM\%hive%\Microsoft\SystemCertificates\%%S\Certificates\99C510FC6160C2294603EAD48DA8831210BBC56D" >nul 2>&1
 if errorlevel 1 set "trusted=0"
)
reg unload "HKLM\%hive%" >nul || exit /b 1
if "%trusted%"=="0" goto trust
if not defined usbOnly (
 call :wifi "%vol%\Windows" "%vol%"
 if errorlevel 1 exit /b 1
)
if not defined component (
 call :bluetooth "%vol%\Windows"
 if errorlevel 1 exit /b 1
)
set "reboot=0"
for %%N in (%drivers%) do (
 dism /Image:%vol%\ /Add-Driver /Driver:"%~dp0%%N\%%N.inf"
 if errorlevel 3011 exit /b 1
 if errorlevel 3010 (
  set "reboot=1"
 ) else if errorlevel 1 exit /b 1
)
if defined usb (
 call :publish-usb "%vol%"
 if errorlevel 1 exit /b 1
)
if /i "%component%"=="usb-input" (
 call :device-rebind "%vol%" SWOA0902
 if errorlevel 1 exit /b 1
)
if defined component goto installed
for %%H in (SWOA0402 SWOA0C01) do (
 call :device-rebind "%vol%" %%H
 if errorlevel 1 exit /b 1
)
call :tools "%vol%\Users\Public\Desktop"
if errorlevel 1 exit /b 1
if "%reboot%"=="1" exit /b 3010
exit /b 0

:trust
echo The target needs the Stage 6 test certificate in Root and TrustedPublisher.
exit /b 1

:bluetooth
if not exist "%~dp0switchbt\switchbt.inf" exit /b 0
if not exist "%~dp0Firmware\BCM4356A3.hcd" (
 echo Supply the Switch BCM4356A3 patch as Firmware\BCM4356A3.hcd before installing Bluetooth.
 exit /b 2
)
if not exist "%~1\System32\drivers\SwitchWOA" mkdir "%~1\System32\drivers\SwitchWOA"
copy /y "%~dp0Firmware\BCM4356A3.hcd" "%~1\System32\drivers\SwitchWOA\BCM4356A3.hcd" >nul || exit /b 1
exit /b 0

:wifi
if not exist "%~dp0switchwifi\switchwifi.inf" exit /b 0
setlocal
set "board=%~1\System32\drivers\SwitchWOA"
set "efi=%~2\SwitchWOA\Wifi"
set "pending=%~1\System32\drivers\SwitchWOA\switchwifi.pending"
if not exist "%~dp0WifiPrepare.efi" goto wifi-missing
if not exist "%board%" mkdir "%board%" || exit /b 1
if not exist "%efi%" mkdir "%efi%" || exit /b 1
if exist "%pending%\" goto wifi-publish
if exist "%board%\brcmfmac4356A3-pcie.bin" (
 if exist "%board%\brcmfmac4356A3-pcie.txt" goto wifi-efi
 goto wifi-partial
)
if exist "%board%\brcmfmac4356A3-pcie.txt" goto wifi-partial
for %%F in (brcmfmac4356A3-pcie.bin brcmfmac4356A3-pcie.txt) do (
 if not exist "%~dp0Firmware\%%F" goto wifi-missing
 for %%G in ("%~dp0Firmware\%%F") do if %%~zG LEQ 0 goto wifi-missing
)
rem Publish a complete pair on the same volume. A retry finishes this pair
rem even if the source kit has since changed. Preserve an existing board pair.
set "incoming=%board%\switchwifi-%RANDOM%-%RANDOM%.tmp"
mkdir "%incoming%" || exit /b 1
for %%F in (brcmfmac4356A3-pcie.bin brcmfmac4356A3-pcie.txt) do (
 copy /b /y "%~dp0Firmware\%%F" "%incoming%\%%F" >nul || goto wifi-copy-fail
)
ren "%incoming%" switchwifi.pending || goto wifi-copy-fail
:wifi-publish
for %%F in (brcmfmac4356A3-pcie.bin brcmfmac4356A3-pcie.txt) do if not exist "%board%\%%F" (
 if not exist "%pending%\%%F" goto wifi-partial
 move /y "%pending%\%%F" "%board%\%%F" >nul || exit /b 1
)
for %%F in (brcmfmac4356A3-pcie.bin brcmfmac4356A3-pcie.txt) do if exist "%pending%\%%F" del /q "%pending%\%%F" || exit /b 1
rmdir "%pending%" || exit /b 1
:wifi-efi
set "tmp=%efi%\wifi-%RANDOM%-%RANDOM%.tmp"
copy /b /y "%~dp0WifiPrepare.efi" "%tmp%" >nul || goto wifi-efi-fail
move /y "%tmp%" "%efi%\WifiPrepare.efi" >nul || goto wifi-efi-fail
exit /b 0
:wifi-missing
echo Wi-Fi needs WifiPrepare.efi and both private A3 board files for a new installation.
exit /b 2
:wifi-partial
echo An incomplete board pair is present. Complete the board files before installing Wi-Fi.
exit /b 2
:wifi-copy-fail
for %%F in (brcmfmac4356A3-pcie.bin brcmfmac4356A3-pcie.txt) do if exist "%incoming%\%%F" del /q "%incoming%\%%F"
rmdir "%incoming%" 2>nul
exit /b 1
:wifi-efi-fail
if exist "%tmp%" del /q "%tmp%"
exit /b 1

:publish-usb
rem The boot manager looks for UsbPrepare.efi on WINARM. Publish it last.
setlocal
set "dir=%~1\SwitchWOA\Usb"
if not exist "%dir%" mkdir "%dir%" || exit /b 1
set "tmp=%dir%\install-%RANDOM%-%RANDOM%.tmp"
mkdir "%tmp%" || exit /b 1
copy /b /y "%~dp0Firmware\tegra210b01_xusb_firmware" "%tmp%\tegra210b01_xusb_firmware" >nul || goto usb-fail
copy /b /y "%~dp0UsbPrepare.efi" "%tmp%\UsbPrepare.efi" >nul || goto usb-fail
for %%F in (tegra210b01_xusb_firmware UsbPrepare.efi) do (
 move /y "%tmp%\%%F" "%dir%\%%F" >nul || goto usb-fail
)
rmdir "%tmp%" || exit /b 1
exit /b 0
:usb-fail
for %%F in (tegra210b01_xusb_firmware UsbPrepare.efi) do if exist "%tmp%\%%F" del /q "%tmp%\%%F"
rmdir "%tmp%" 2>nul
exit /b 1

:tools
if not exist "%~dp0LitePerformanceCheck.exe" exit /b 0
if not exist "%~1" mkdir "%~1"
copy /y "%~dp0LitePerformanceCheck.exe" "%~1\LitePerformanceCheck.exe" >nul || exit /b 1
exit /b 0

:device-rebind
rem DISM stages packages; an existing devnode also needs reinstallation.
setlocal EnableDelayedExpansion
set "fanHive=LiteDeviceUpdate%RANDOM%%RANDOM%"
reg load "HKLM\!fanHive!" "%~1\Windows\System32\config\SYSTEM" >nul || exit /b 1
set "current="
for /f "tokens=1,2,3" %%A in ('reg query "HKLM\!fanHive!\Select" /v Current 2^>nul') do if /i "%%A"=="Current" set /a "current=%%C" >nul
if not defined current goto device-unload-fail
set "cs=00!current!"
set "node=HKLM\!fanHive!\ControlSet!cs:~-3!\Enum\ACPI\%~2\0"
reg query "!node!" >nul 2>&1
if errorlevel 1 goto device-unload-ok
set "flags=0"
for /f "tokens=1,2,3" %%A in ('reg query "!node!" /v ConfigFlags 2^>nul') do if /i "%%A"=="ConfigFlags" set /a "flags=%%C" >nul
set /a "flags=flags | 0x20" >nul
reg add "!node!" /v ConfigFlags /t REG_DWORD /d !flags! /f >nul || goto device-unload-fail
:device-unload-ok
reg unload "HKLM\!fanHive!" >nul || exit /b 1
exit /b 0
:device-unload-fail
reg unload "HKLM\!fanHive!" >nul
exit /b 1
