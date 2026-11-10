@echo off
rem winx gm20b -- install the driver as a DEMAND-start kernel service (it is never started at boot).
rem Run from an elevated prompt, in the folder that contains the (test-signed) gm20b.sys.
setlocal
cd /d "%~dp0"
net session >nul 2>&1
if errorlevel 1 (echo Please run this from an elevated ^(Administrator^) prompt.& exit /b 1)
if not exist gm20b.sys (echo gm20b.sys not found next to install.cmd.& exit /b 1)
sc query gm20b >nul 2>&1
if not errorlevel 1 (echo The service gm20b already exists. Run uninstall.cmd first.& exit /b 1)
copy /y gm20b.sys "%SystemRoot%\System32\drivers\gm20b.sys" >nul
if errorlevel 1 (echo copy failed.& exit /b 1)
sc create gm20b type= kernel start= demand error= ignore binPath= "%SystemRoot%\System32\drivers\gm20b.sys" DisplayName= "winx GM20B GPU bring-up"
if errorlevel 1 exit /b 1
reg add "HKLM\SYSTEM\CurrentControlSet\Services\gm20b\Parameters" /v MaxStage /t REG_DWORD /d 0 /f >nul
echo.
echo Installed.  MaxStage=0: the driver only maps its register windows and checks its HAL; it touches no hardware.
echo Next:  set-stage.cmd 1   then   start.cmd      (see WINDOWS.md, "Saturday procedure")
exit /b 0
