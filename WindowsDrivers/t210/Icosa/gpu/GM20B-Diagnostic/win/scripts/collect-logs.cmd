@echo off
rem Collect everything needed to understand a run into .\gm20b-logs\<timestamp>\   (works after a hang + reboot).
setlocal
cd /d "%~dp0"
set STAMP=%DATE:/=-%_%TIME::=-%
set STAMP=%STAMP: =0%
set STAMP=%STAMP:,=.%
set DST=gm20b-logs\%STAMP%
mkdir "%DST%" 2>nul
copy /y "%SystemRoot%\gm20b-journal.bin" "%DST%\journal.bin" >nul 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Services\gm20b" /s > "%DST%\registry.txt" 2>&1
if exist gm20bctl.exe (
  gm20bctl.exe decode "%DST%\journal.bin" > "%DST%\journal.txt" 2>&1
  gm20bctl.exe status > "%DST%\status.txt" 2>&1
)
echo Collected into %CD%\%DST%
echo Please keep journal.bin and registry.txt ^(it holds the LastEntry breadcrumb^). journal.txt is the decoded text.
exit /b 0
