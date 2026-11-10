@echo off
rem Stop the driver: every stage is torn down (GPU back into reset, the clocks we enabled off) and the journal is closed.
setlocal
net session >nul 2>&1
if errorlevel 1 (echo Please run this from an elevated ^(Administrator^) prompt.& exit /b 1)
sc stop gm20b
exit /b 0
