@echo off
setlocal EnableExtensions
if /i "%~1"=="online" goto online
if /i "%~1"=="offline" goto offline
echo Install-LiteTouch.cmd online
echo Install-LiteTouch.cmd offline W:\
echo W: must be the drive assigned to the offline WINARM Windows installation.
exit /b 2

:online
for %%S in (ROOT TrustedPublisher) do (
 reg query "HKLM\SOFTWARE\Microsoft\SystemCertificates\%%S\Certificates\99C510FC6160C2294603EAD48DA8831210BBC56D" >nul 2>&1
 if errorlevel 1 goto trust
)
set "reboot=0"
for %%N in (tegra210gpio tegra210i2c switchtouch) do (
 pnputil /add-driver "%~dp0%%N\%%N.inf" /install
 if errorlevel 3011 exit /b 1
 if errorlevel 3010 (
  set "reboot=1"
 ) else if errorlevel 1 exit /b 1
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
set "hive=LiteTouchQuery%RANDOM%%RANDOM%"
reg load "HKLM\%hive%" "%vol%\Windows\System32\config\SOFTWARE" >nul || exit /b 1
set "trusted=1"
for %%S in (ROOT TrustedPublisher) do (
 reg query "HKLM\%hive%\Microsoft\SystemCertificates\%%S\Certificates\99C510FC6160C2294603EAD48DA8831210BBC56D" >nul 2>&1
 if errorlevel 1 set "trusted=0"
)
reg unload "HKLM\%hive%" >nul || exit /b 1
if "%trusted%"=="0" goto trust
for %%N in (tegra210gpio tegra210i2c switchtouch) do (
 dism /Image:%vol%\ /Add-Driver /Driver:"%~dp0%%N\%%N.inf"
 if errorlevel 1 exit /b 1
)
exit /b 0

:trust
echo The target needs the Stage 6 test certificate in Root and TrustedPublisher.
exit /b 1
