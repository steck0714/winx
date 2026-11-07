@echo off
setlocal EnableExtensions
rem Run once as administrator after installing the matching S4 firmware and
rem nvtsdhc package. Do not force a hibernation or restart during setup.
set "state=HKLM\SOFTWARE\SwitchLite\Hibernate"
reg add "%state%" /v SetupVersion /t REG_DWORD /d 1 /f >nul 2>&1
reg add "%state%" /v SetupComplete /t REG_DWORD /d 0 /f >nul 2>&1
powercfg /hibernate on >nul 2>&1
if errorlevel 1 goto failed
powercfg /hibernate /type full >nul 2>&1
if errorlevel 1 goto failed
rem Keep normal shutdown a full shutdown on this multi-boot card.
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Power" /v HiberbootEnabled /t REG_DWORD /d 0 /f >nul 2>&1
if errorlevel 1 goto failed
rem Power-setting index 2 is Hibernate; index 3 would be Shutdown.
powercfg /setacvalueindex SCHEME_CURRENT SUB_BUTTONS PBUTTONACTION 2 >nul 2>&1
if errorlevel 1 goto failed
powercfg /setdcvalueindex SCHEME_CURRENT SUB_BUTTONS PBUTTONACTION 2 >nul 2>&1
if errorlevel 1 goto failed
powercfg /setactive SCHEME_CURRENT >nul 2>&1
if errorlevel 1 goto failed
reg add "%state%" /v LastError /t REG_DWORD /d 0 /f >nul 2>&1
reg add "%state%" /v SetupComplete /t REG_DWORD /d 1 /f >nul 2>&1
rem The source remains in Git; the one-time on-device copy is no longer needed.
del "%~f0"
exit /b 0
:failed
set "failure=%errorlevel%"
reg add "%state%" /v LastError /t REG_DWORD /d %failure% /f >nul 2>&1
exit /b %failure%
