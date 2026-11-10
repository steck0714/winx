@echo off
rem Stop and remove the gm20b service.  The journal file %SystemRoot%\gm20b-journal.bin is kept.
setlocal
net session >nul 2>&1
if errorlevel 1 (echo Please run this from an elevated ^(Administrator^) prompt.& exit /b 1)
sc stop gm20b >nul 2>&1
timeout /t 2 /nobreak >nul
sc delete gm20b
del /f /q "%SystemRoot%\System32\drivers\gm20b.sys" >nul 2>&1
echo Removed.  The journal file %SystemRoot%\gm20b-journal.bin was kept.
exit /b 0
