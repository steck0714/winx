@echo off
setlocal EnableExtensions EnableDelayedExpansion
title Switch Lite - Tegra SD read test
color 1F
set "log=%SystemDrive%\sdtest"
set "sid=ACPI\NVDA0101"
if not exist "%log%" mkdir "%log%"
echo Initializing Windows PE...
wpeinit
set "initrc=%errorlevel%"
echo Checking storage with the preinstalled driver...
set "relative=switchroot\woa-lite-winpe\MEDIA.ID"
set "matched="
set "bad="
set "ramvol="
for /f "tokens=1" %%V in ('mountvol %SystemDrive%\ /L') do set "ramvol=%%V"
for /l %%T in (1,1,6) do (
    call :scan
    if defined matched goto devices
    if %%T LSS 6 ping -n 4 127.0.0.1 >nul 2>&1
)

:devices
pnputil /enum-devices /deviceid "%sid%" >"%log%\host.txt" 2>&1
pnputil /enum-devices /deviceid "SD\CLASS_STORAGE" >"%log%\card.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\NVDA0101" /s /v Service >"%log%\service.txt" 2>nul
reg query "HKLM\SYSTEM\CurrentControlSet\Services\nvtsdhc\Parameters\Diag" >"%log%\diag.txt" 2>nul
>"%log%\list.txt" echo list disk
diskpart /s "%log%\list.txt" >"%log%\disks.txt" 2>&1
set "diskrc=%errorlevel%"
cls
echo LITE WINDOWS / TEGRA SD / PIO READ-ONLY
echo WPEINIT %initrc%   %PROCESSOR_ARCHITECTURE%   %SystemDrive%
set "instance="
for /f "usebackq tokens=1,* delims=:" %%A in ("%log%\host.txt") do (
    if /i "%%A"=="Instance ID" set "instance=%%B"
    if /i "%%A"=="Status" echo Host status:%%B
    if /i "%%A"=="Problem Code" echo Problem code:%%B
    if /i "%%A"=="Problem Status" echo Problem status:%%B
)
if defined instance (echo Host:!instance!) else echo Host %sid% not enumerated
set "service="
for /f "usebackq tokens=1,2,*" %%A in ("%log%\service.txt") do if /i "%%A"=="Service" set "service=%%C"
if defined service (echo Driver service: !service!) else echo Driver service: none
set "card=none"
for /f "usebackq tokens=1,* delims=:" %%A in ("%log%\card.txt") do (
    if /i "%%A"=="Status" set "card=%%B"
    if /i "%%A"=="Problem Code" echo SD child problem:%%B
)
echo SD storage child: !card!
for /f "usebackq tokens=1,2,3" %%A in ("%log%\diag.txt") do (
    for %%N in (ClockKhzActual BusWidth Commands CommandErrors LastCommand LastErrorCommand BlocksRead) do if /i "%%A"=="%%N" echo %%A  %%C
)
set /a disks=0 >nul
for /f "usebackq tokens=1,2,3,*" %%A in ("%log%\disks.txt") do (
    if /i "%%A"=="Disk" if not "%%B"=="###" (
        echo Disk %%B  %%C  %%D
        set /a disks+=1 >nul
    )
)
echo Disks listed: !disks!   DiskPart result: %diskrc%
if defined matched (
    echo SD MARKER READ OK
) else if defined bad (
    echo SD MARKER FOUND - CONTENT OR READ ERROR
) else (
    echo SD MARKER NOT FOUND
)
echo TEST END - photograph this screen
exit /b

:scan
mountvol >"%log%\volumes.txt" 2>&1
for /f "usebackq tokens=1" %%V in ("%log%\volumes.txt") do (
    set "v=%%V"
    if "!v:~0,11!"=="\\?\Volume{" if /i not "!v!"=="!ramvol!" call :check "%%V"
)
if defined matched exit /b
for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z A B) do (
    if /i not "%%D:"=="%SystemDrive%" if not defined matched call :check "%%D:\"
)
exit /b

:check
if defined matched exit /b
set "candidate=%~1%relative%"
if not exist "!candidate!" exit /b
for %%F in ("!candidate!") do set "bytes=%%~zF"
if not "!bytes!"=="27" (
    set "bad=1"
    exit /b
)
set "value="
for /f "usebackq delims=" %%L in ("!candidate!") do set "value=%%L"
if "!value!"=="SWITCH-LITE-WINPE-22621-R1" set "matched=%~1"
if not defined matched set "bad=1"
exit /b
