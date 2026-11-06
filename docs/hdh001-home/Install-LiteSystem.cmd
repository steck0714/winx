@echo off
setlocal EnableExtensions
rem Add shared system drivers to an existing Stage 6 Home installation.
rem Place this script beside the five shared system/audio packages.
rem Optional switchbt also requires Firmware\BCM4356A3.hcd.
set "drivers=switchfan switchpwrbtn switchbattery switchrtc switchaudio"
if exist "%~dp0switchbt\switchbt.inf" set "drivers=%drivers% switchbt"
if /i "%~1"=="online" goto online
if /i "%~1"=="offline" goto offline
echo Install-LiteSystem.cmd online
echo Install-LiteSystem.cmd offline W:\
echo W: must be the drive assigned to the offline WINARM Windows installation.
exit /b 2

:online
for %%S in (ROOT TrustedPublisher) do (
 reg query "HKLM\SOFTWARE\Microsoft\SystemCertificates\%%S\Certificates\99C510FC6160C2294603EAD48DA8831210BBC56D" >nul 2>&1
 if errorlevel 1 goto trust
)
call :bluetooth "%SystemRoot%"
if errorlevel 1 exit /b 1
set "reboot=0"
for %%N in (%drivers%) do (
 pnputil /add-driver "%~dp0%%N\%%N.inf" /install
 if errorlevel 3011 exit /b 1
 if errorlevel 3010 (
  set "reboot=1"
 ) else if errorlevel 1 exit /b 1
)
pnputil /scan-devices
if errorlevel 1 exit /b 1
call :tools "%PUBLIC%\Desktop"
if errorlevel 1 exit /b 1
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
call :bluetooth "%vol%\Windows"
if errorlevel 1 exit /b 1
set "reboot=0"
for %%N in (%drivers%) do (
 dism /Image:%vol%\ /Add-Driver /Driver:"%~dp0%%N\%%N.inf"
 if errorlevel 3011 exit /b 1
 if errorlevel 3010 (
  set "reboot=1"
 ) else if errorlevel 1 exit /b 1
)
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
