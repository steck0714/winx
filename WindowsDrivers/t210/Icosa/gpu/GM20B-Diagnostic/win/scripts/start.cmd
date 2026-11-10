@echo off
rem Start the driver (runs the bring-up up to MaxStage) and print its status.
rem If the machine hangs: power-cycle, boot Windows again and run collect-logs.cmd.
setlocal
cd /d "%~dp0"
net session >nul 2>&1
if errorlevel 1 (echo Please run this from an elevated ^(Administrator^) prompt.& exit /b 1)
echo Starting gm20b ... (journal: %SystemRoot%\gm20b-journal.bin)
sc start gm20b
echo.
if exist gm20bctl.exe gm20bctl.exe status
exit /b 0
