@echo off
wpeinit
set LOG=X:\winpe_usb_report.txt
echo ===== WinPE ARM64 USB test report =====> %LOG%
echo %date% %time%>> %LOG%
echo.>> %LOG%
echo --- EHCI controller (ACPI NVDA0214 / PNP0D20) --->> %LOG%
pnputil /enum-devices /instanceid "ACPI\NVDA0214\0">> %LOG% 2>&1
echo.>> %LOG%
echo --- URS dual-role controller (variant B: ACPI PNP0C90) --->> %LOG%
pnputil /enum-devices /instanceid "ACPI\PNP0C90\0">> %LOG% 2>&1
echo.>> %LOG%
echo --- does the in-box URS Chipidea INF list PNP0C90? --->> %LOG%
dir X:\Windows\INF\urs*.inf>> %LOG% 2>&1
findstr /i /m /c:"PNP0C90" X:\Windows\INF\urs*.inf>> %LOG% 2>&1
echo.>> %LOG%
echo --- devices with a problem --->> %LOG%
pnputil /enum-devices /problem>> %LOG% 2>&1
echo.>> %LOG%
echo --- USB class, connected --->> %LOG%
pnputil /enum-devices /connected /class USB>> %LOG% 2>&1
echo.>> %LOG%
echo --- Keyboard / HID, connected --->> %LOG%
pnputil /enum-devices /connected /class Keyboard>> %LOG% 2>&1
pnputil /enum-devices /connected /class HIDClass>> %LOG% 2>&1
echo.>> %LOG%
echo --- SD host (ACPI NVDA0212 / PNP0D40) --->> %LOG%
pnputil /enum-devices /instanceid "ACPI\NVDA0212\0">> %LOG% 2>&1
echo.>> %LOG%
echo --- driver install log (usbehci / PNP0D20 / NVDA0214) --->> %LOG%
findstr /i /c:"NVDA0214" /c:"PNP0D20" /c:"usbehci" X:\Windows\INF\setupapi.dev.log>> %LOG% 2>&1
rem Show only the key lines on screen (the full report stays in X:\winpe_usb_report.txt)
cls
echo ===== WinPE ARM64 USB test: key lines =====
findstr /i /c:"Instance ID" /c:"Status" /c:"Problem" /c:"Driver Name" /c:"Class Name" %LOG%
rem Copy the report to any volume that contains the marker file \WINPE_LOG_HERE.txt (for example the SD card)
for %%d in (C D E F G H I J) do if exist %%d:\WINPE_LOG_HERE.txt copy /y %LOG% %%d:\winpe_usb_report.txt >nul
echo.
echo Report: %LOG%   (run: type %LOG% ^| more   if a keyboard works)
cmd /k
