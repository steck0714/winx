@echo off
setlocal EnableExtensions
rem Add shared system drivers to an existing Stage 6 Home installation.
rem Place this script beside switchfan, switchpwrbtn, switchbattery, switchrtc, winxdisplay and switchaudio.
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
set "reboot=0"
for %%N in (switchfan switchpwrbtn switchbattery switchrtc winxdisplay switchaudio) do (
 pnputil /add-driver "%~dp0%%N\%%N.inf" /install
 if errorlevel 3011 exit /b 1
 if errorlevel 3010 (
  set "reboot=1"
 ) else if errorlevel 1 exit /b 1
)
if exist "%~dp0LiteLandscape.exe" (
 call :landscape "%ProgramData%\SwitchLite" SOFTWARE
 if errorlevel 1 exit /b 1
)
pnputil /scan-devices
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
if "%trusted%"=="1" if exist "%~dp0LiteLandscape.exe" (
 call :landscape "%vol%\ProgramData\SwitchLite" "%hive%"
 if errorlevel 1 set "landscape_failed=1"
)
reg unload "HKLM\%hive%" >nul || exit /b 1
if defined landscape_failed exit /b 1
if "%trusted%"=="0" goto trust
set "reboot=0"
for %%N in (switchfan switchpwrbtn switchbattery switchrtc winxdisplay switchaudio) do (
 dism /Image:%vol%\ /Add-Driver /Driver:"%~dp0%%N\%%N.inf"
 if errorlevel 3011 exit /b 1
 if errorlevel 3010 (
  set "reboot=1"
 ) else if errorlevel 1 exit /b 1
)
if "%reboot%"=="1" exit /b 3010
exit /b 0

:trust
echo The target needs the Stage 6 test certificate in Root and TrustedPublisher.
exit /b 1

:landscape
if not exist "%~1" mkdir "%~1"
copy /y "%~dp0LiteLandscape.exe" "%~1\LiteLandscape.exe" >nul || exit /b 1
set "regfile=%TEMP%\LiteLandscape-%RANDOM%%RANDOM%.reg"
>"%regfile%" echo Windows Registry Editor Version 5.00
>>"%regfile%" echo.
>>"%regfile%" echo [HKEY_LOCAL_MACHINE\%~2\Microsoft\Windows\CurrentVersion\Run]
>>"%regfile%" echo "LiteLandscape"=hex(2):22,00,25,00,50,00,72,00,6f,00,67,00,72,00,61,00,6d,00,44,00,61,00,74,00,61,00,25,00,5c,00,53,00,77,00,69,00,74,00,63,00,68,00,4c,00,69,00,74,00,65,00,5c,00,4c,00,69,00,74,00,65,00,4c,00,61,00,6e,00,64,00,73,00,63,00,61,00,70,00,65,00,2e,00,65,00,78,00,65,00,22,00,00,00
reg import "%regfile%" >nul
set "landscape_rc=%errorlevel%"
del "%regfile%"
exit /b %landscape_rc%
