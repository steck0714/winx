@echo off
rem offline-install.cmd D:   install gm20b into an OFFLINE Windows installation on drive D: (e.g. from WinPE).
rem Registers a DEMAND-start service (Start=3).  Nothing is loaded until you run "sc start gm20b" inside that Windows.
setlocal
cd /d "%~dp0"
if "%~1"=="" (echo usage: offline-install.cmd D:& exit /b 2)
set WIN=%~d1\Windows
if not exist "%WIN%\System32\config\SYSTEM" (echo %WIN% does not look like a Windows installation.& exit /b 1)
if not exist gm20b.sys (echo gm20b.sys not found next to this script.& exit /b 1)
copy /y gm20b.sys "%WIN%\System32\drivers\gm20b.sys" >nul
if errorlevel 1 (echo copy failed.& exit /b 1)
reg load HKLM\OFFSYS "%WIN%\System32\config\SYSTEM" >nul
if errorlevel 1 (echo could not load the offline SYSTEM hive.& exit /b 1)
set CS=1
for /f "tokens=3" %%a in ('reg query HKLM\OFFSYS\Select /v Current 2^>nul ^| find "Current"') do set /a CS=%%a
set CSN=00%CS%
set CSN=%CSN:~-3%
set K=HKLM\OFFSYS\ControlSet%CSN%\Services\gm20b
reg add "%K%" /v Type /t REG_DWORD /d 1 /f >nul
reg add "%K%" /v Start /t REG_DWORD /d 3 /f >nul
reg add "%K%" /v ErrorControl /t REG_DWORD /d 0 /f >nul
reg add "%K%" /v ImagePath /t REG_EXPAND_SZ /d "\SystemRoot\System32\drivers\gm20b.sys" /f >nul
reg add "%K%" /v DisplayName /t REG_SZ /d "winx GM20B GPU bring-up" /f >nul
reg add "%K%\Parameters" /v MaxStage /t REG_DWORD /d 0 /f >nul
reg unload HKLM\OFFSYS >nul
echo Installed into ControlSet%CSN% of %WIN%  (Start=3 demand, MaxStage=0).
exit /b 0
