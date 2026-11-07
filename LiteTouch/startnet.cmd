@echo off
setlocal EnableExtensions
title Switch Lite - Windows touch test
color 1F
echo Initializing ARM64 Windows PE...
wpeinit
set "logs=%SystemDrive%\TouchLogs"
md "%logs%" 2>nul
pnputil /scan-devices >"%logs%\scan.txt" 2>&1
for %%D in (SWOA0101 SWOA0201 SWOA0502) do pnputil /enum-devices /deviceid "ACPI\%%D" /drivers >"%logs%\%%D.txt" 2>&1
reg query HKLM\SYSTEM\CurrentControlSet\Services\switchtouch\Parameters >"%logs%\probe.txt" 2>&1
echo Starting touch check...
"%SystemRoot%\System32\touch-check.exe"
set "rc=%errorlevel%"
if "%rc%"=="0" exit /b 0
title Switch Lite - touch driver diagnostics
:retry
cls
echo Touch app exit %rc% - driver diagnostics
for %%D in (SWOA0101 SWOA0201 SWOA0502) do (
 echo ACPI\%%D
 pnputil /enum-devices /deviceid "ACPI\%%D" >"%logs%\%%D.txt" 2>&1
 %SystemRoot%\System32\find.exe /i "Status:" "%logs%\%%D.txt"
 %SystemRoot%\System32\find.exe /i "Problem" "%logs%\%%D.txt"
)
for %%V in (ChipId ProbeStage ProbeStatus Events Reports Errors LastLandscapeX LastLandscapeY) do (
 reg query HKLM\SYSTEM\CurrentControlSet\Services\switchtouch\Parameters /v %%V 2>nul | %SystemRoot%\System32\find.exe "REG_DWORD"
)
echo Touch and lift. Photograph this screen for diagnosis.
ping -n 4 127.0.0.1 >nul 2>&1
goto retry
