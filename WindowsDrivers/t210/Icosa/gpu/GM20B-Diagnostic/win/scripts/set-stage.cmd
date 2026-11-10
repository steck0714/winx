@echo off
rem set-stage.cmd N [flags-hex]   set the highest bring-up stage (0..8) for the NEXT start of the driver.
rem   0 = map the register windows and check the HAL only (NO hardware access)
rem   1 = power: CAR/PMC reads, then clock/reset/clamp writes
rem   2 = read BOOT0 (the first GPU register access) and mask the GPU interrupts
rem   3..8 are not implemented yet (the driver reports NOTIMPL and tears down cleanly)
rem Optional second argument = Flags in hex (no 0x):  1 = journal every MMIO access,  10 = DIAG_ONLY (look, do not write),
rem   10000 = force the BAR0 access even if CAR/PMC say the GPU is not powered (only for debugging the guard itself).
rem AllowRawMmio (the raw read/write IOCTLs) is deliberately NOT settable here: use reg add by hand if you need it.
setlocal
if "%~1"=="" (echo usage: set-stage.cmd N [flags-hex]& exit /b 2)
reg add "HKLM\SYSTEM\CurrentControlSet\Services\gm20b\Parameters" /v MaxStage /t REG_DWORD /d %~1 /f >nul || exit /b 1
if not "%~2"=="" reg add "HKLM\SYSTEM\CurrentControlSet\Services\gm20b\Parameters" /v Flags /t REG_DWORD /d 0x%~2 /f >nul
echo MaxStage=%~1  Flags=%~2
echo Effective at the next start:  stop.cmd  then  start.cmd
exit /b 0
